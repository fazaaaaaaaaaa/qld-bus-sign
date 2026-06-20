#!/usr/bin/env python3
"""
build_schedule.py — One-off / weekly job.

Downloads the SEQ static GTFS zip, parses the stop-relevant data, and writes
cloud/stop_schedule.json — a compact precomputed schedule that update_departures.py
uses every 5 minutes WITHOUT needing the 28 MB GTFS download.

Run once at setup, then it auto-refreshes weekly via GitHub Actions.

Usage:
    python build_schedule.py [--config path/to/config.yaml]
"""

import argparse
import csv
import io
import json
import logging
import os
import sys
import zipfile
from pathlib import Path

import requests
import yaml

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
)
log = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Config helpers
# ---------------------------------------------------------------------------

def _load_config(config_path: str) -> dict:
    with open(config_path) as f:
        return yaml.safe_load(f)


def _find_config() -> str:
    """Walk up looking for config.yaml relative to this script."""
    here = Path(__file__).parent
    for candidate in [here / "config.yaml", here / "config.example.yaml"]:
        if candidate.exists():
            return str(candidate)
    raise FileNotFoundError("No config.yaml found — copy config.yaml and edit it.")


# ---------------------------------------------------------------------------
# GTFS zip download
# ---------------------------------------------------------------------------

def _download_zip(url: str, dest: Path, timeout: int = 60) -> None:
    log.info("Downloading static GTFS from %s …", url)
    resp = requests.get(url, timeout=timeout, stream=True)
    resp.raise_for_status()
    dest.parent.mkdir(parents=True, exist_ok=True)
    with open(dest, "wb") as f:
        for chunk in resp.iter_content(chunk_size=65536):
            f.write(chunk)
    log.info("Saved %.1f MB → %s", dest.stat().st_size / 1_048_576, dest)


# ---------------------------------------------------------------------------
# CSV helpers
# ---------------------------------------------------------------------------

def _open_csv(zf: zipfile.ZipFile, name: str) -> csv.DictReader:
    """Open a CSV member from a ZipFile, handling optional subdirectory prefix."""
    try:
        data = zf.read(name).decode("utf-8-sig")
    except KeyError:
        # Some zips have everything under a single subdirectory.
        data = zf.read(name.split("/")[-1]).decode("utf-8-sig")
    return csv.DictReader(io.StringIO(data))


# ---------------------------------------------------------------------------
# Core parsing
# ---------------------------------------------------------------------------

def _parse_gtfs(zf: zipfile.ZipFile, stop_ids: set) -> dict:
    """
    Parse the GTFS zip (already open) and return a compact schedule dict.

    Structure returned:
    {
      "routes":   { route_id: route_short_name },
      "calendar": {
          service_id: {
              "monday": 0|1, "tuesday": 0|1, ..., "sunday": 0|1,
              "start_date": "YYYYMMDD",
              "end_date":   "YYYYMMDD"
          }
      },
      "calendar_dates": [
          {"service_id": ..., "date": "YYYYMMDD", "exception_type": 1|2}
      ],
      "trips": {
          trip_id: {
              "route_id": ...,
              "service_id": ...,
              "headsign": ...,
          }
      },
      "stop_times": [
          {
              "trip_id": ...,
              "stop_id": ...,
              "departure_time": "HH:MM:SS",   # may be >=24:00 for after-midnight
              "stop_sequence": int
          }
      ]
    }
    """
    log.info("Parsing routes …")
    routes = {}
    for r in _open_csv(zf, "routes.txt"):
        routes[r["route_id"]] = r.get("route_short_name", r["route_id"])

    log.info("Parsing calendar …")
    calendar = {}
    try:
        _DAY_COLS = ["monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"]
        for r in _open_csv(zf, "calendar.txt"):
            calendar[r["service_id"]] = {
                col: int(r.get(col, 0) or 0) for col in _DAY_COLS
            }
            calendar[r["service_id"]]["start_date"] = r["start_date"]
            calendar[r["service_id"]]["end_date"] = r["end_date"]
    except Exception as exc:
        log.warning("calendar.txt parse issue (may be absent): %s", exc)

    log.info("Parsing calendar_dates …")
    calendar_dates = []
    try:
        for r in _open_csv(zf, "calendar_dates.txt"):
            calendar_dates.append({
                "service_id": r["service_id"],
                "date": r["date"],
                "exception_type": int(r["exception_type"]),
            })
    except Exception as exc:
        log.warning("calendar_dates.txt parse issue: %s", exc)

    log.info("Scanning stop_times for stop_ids %s …", stop_ids)
    relevant_trip_ids = set()
    stop_times = []
    for r in _open_csv(zf, "stop_times.txt"):
        if r["stop_id"] in stop_ids:
            dep = r.get("departure_time") or r.get("arrival_time", "")
            stop_times.append({
                "trip_id": r["trip_id"],
                "stop_id": r["stop_id"],
                "departure_time": dep.strip(),
                "stop_sequence": int(r.get("stop_sequence", 0) or 0),
            })
            relevant_trip_ids.add(r["trip_id"])
    log.info("Found %d stop_time rows across %d trips", len(stop_times), len(relevant_trip_ids))

    log.info("Parsing trips (filtering to relevant) …")
    trips = {}
    for r in _open_csv(zf, "trips.txt"):
        if r["trip_id"] in relevant_trip_ids:
            trips[r["trip_id"]] = {
                "route_id": r["route_id"],
                "service_id": r["service_id"],
                "headsign": r.get("trip_headsign", ""),
            }
    log.info("Kept %d trips", len(trips))

    return {
        "routes": routes,
        "calendar": calendar,
        "calendar_dates": calendar_dates,
        "trips": trips,
        "stop_times": stop_times,
    }


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description="Build stop_schedule.json from static GTFS.")
    parser.add_argument("--config", default=None, help="Path to config.yaml")
    args = parser.parse_args()

    config_path = args.config or _find_config()
    log.info("Using config: %s", config_path)
    config = _load_config(config_path)

    stop_ids = set(str(s) for s in config.get("stop_ids", []))
    if not stop_ids:
        log.error("No stop_ids configured — edit config.yaml")
        return 1

    zip_url = config.get("gtfs_static_zip_url", "https://gtfsrt.api.translink.com.au/GTFS/SEQ_GTFS.zip")
    timeout = int(config.get("feed_timeout_seconds", 15))

    # Download to a temp path next to this script.
    here = Path(__file__).parent
    zip_path = here / "_cache_gtfs.zip"
    try:
        _download_zip(zip_url, zip_path, timeout=max(timeout * 4, 60))
    except requests.HTTPError as exc:
        status = exc.response.status_code if exc.response is not None else "?"
        log.error("HTTP %s downloading static GTFS zip from:\n  %s", status, zip_url)
        if status == 404:
            log.error(
                "The static GTFS URL has moved.  Find the current URL at:\n"
                "  https://www.data.qld.gov.au/dataset/general-transit-feed-specification-gtfs-seq\n"
                "Then update 'gtfs_static_zip_url' in config.yaml and re-run this job."
            )
        return 1
    except Exception as exc:
        log.error("Failed to download static GTFS zip: %s", exc)
        return 1

    log.info("Opening zip …")
    with zipfile.ZipFile(zip_path) as zf:
        schedule = _parse_gtfs(zf, stop_ids)

    # Embed metadata.
    import time
    schedule["_meta"] = {
        "stop_ids": sorted(stop_ids),
        "stop_label": config.get("stop_label", ""),
        "built_at_epoch": int(time.time()),
        "gtfs_zip_url": zip_url,
    }

    out_path = here / "stop_schedule.json"
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(schedule, f, separators=(",", ":"))

    sz = out_path.stat().st_size
    log.info("Written %s (%.1f KB)", out_path, sz / 1024)

    # Clean up the large zip to keep the repo lean (it's not committed).
    try:
        zip_path.unlink()
        log.info("Removed temporary zip cache.")
    except Exception:
        pass

    return 0


if __name__ == "__main__":
    sys.exit(main())
