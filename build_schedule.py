#!/usr/bin/env python3
"""
build_schedule.py — One-off / weekly job.

Downloads the SEQ static GTFS zip, parses the stop-relevant data for ALL
configured stops, and writes cloud/stop_schedule.json — a compact precomputed
schedule (Data Contract v3) that update_departures.py uses every 5 minutes
WITHOUT needing the 28 MB GTFS download.

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
import time
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
# Stop resolver: match_ids → set of GTFS stop_ids
# ---------------------------------------------------------------------------

def _resolve_stop_ids(zf: zipfile.ZipFile, match_ids: list) -> set:
    """
    For each id in match_ids, scan stops.txt for rows where
    stop_id == id OR stop_code == id.  Return the union of all matched stop_ids.
    This lets callers supply either the GTFS stop_id or the public stop code
    and the resolver handles either form transparently.
    """
    match_set = set(str(m) for m in match_ids)
    resolved = set()
    for row in _open_csv(zf, "stops.txt"):
        sid = row.get("stop_id", "").strip()
        scode = row.get("stop_code", "").strip()
        if sid in match_set or scode in match_set:
            resolved.add(sid)
    return resolved


# ---------------------------------------------------------------------------
# Core parsing — v3 multi-stop
# ---------------------------------------------------------------------------

def _parse_gtfs_v3(zf: zipfile.ZipFile, stops_config: list) -> dict:
    """
    Parse the GTFS zip for ALL configured stops and return a v3 schedule dict.

    Each stop entry in stops_config must have: key, label, match_ids, routes.

    Structure returned (Data Contract v3 §B):
    {
      "version": 3,
      "built_at": <epoch int>,
      "timezone": "Australia/Brisbane",
      "stops": {
        "<key>": {
          "label": "...",
          "routes": ["320"],
          "trips": [
            {
              "trip_id": "...",
              "route": "320",
              "dest": "City",
              "service_id": "...",
              "dep": "07:42:00"    # local, may exceed 24:00:00 for after-midnight
            }
          ]
        },
        ...
      },
      "calendar":       { service_id: {weekday flags, start_date, end_date} },
      "calendar_dates": { service_id: [{date, exception_type}] }
    }
    """

    # ---- Resolve stop IDs for every configured stop -------------------------
    log.info("Resolving match_ids to GTFS stop_ids via stops.txt …")
    resolved_by_key: dict[str, set] = {}
    for stop_cfg in stops_config:
        key = stop_cfg["key"]
        match_ids = stop_cfg.get("match_ids", [])
        resolved = _resolve_stop_ids(zf, match_ids)
        resolved_by_key[key] = resolved
        if resolved:
            log.info("  Stop %r → GTFS stop_ids: %s", key, sorted(resolved))
        else:
            log.warning(
                "Stop %r: none of match_ids %s resolved to any stops.txt row — "
                "check your match_ids in config.yaml",
                key, match_ids,
            )

    # Flat set of all GTFS stop_ids we care about (for stop_times scan).
    all_stop_ids: set = set()
    for s in resolved_by_key.values():
        all_stop_ids |= s

    # ---- Parse routes -------------------------------------------------------
    log.info("Parsing routes …")
    routes: dict[str, str] = {}  # route_id → route_short_name
    for r in _open_csv(zf, "routes.txt"):
        routes[r["route_id"]] = r.get("route_short_name", r["route_id"])

    # ---- Parse calendar -----------------------------------------------------
    log.info("Parsing calendar …")
    calendar: dict = {}
    _DAY_COLS = ["monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"]
    try:
        for r in _open_csv(zf, "calendar.txt"):
            calendar[r["service_id"]] = {
                col: int(r.get(col, 0) or 0) for col in _DAY_COLS
            }
            calendar[r["service_id"]]["start_date"] = r["start_date"]
            calendar[r["service_id"]]["end_date"] = r["end_date"]
    except Exception as exc:
        log.warning("calendar.txt parse issue (may be absent): %s", exc)

    # ---- Parse calendar_dates -----------------------------------------------
    log.info("Parsing calendar_dates …")
    # v3 shape: dict keyed by service_id → list of {date, exception_type}
    calendar_dates: dict[str, list] = {}
    try:
        for r in _open_csv(zf, "calendar_dates.txt"):
            entry = {
                "date": r["date"],
                "exception_type": int(r["exception_type"]),
            }
            calendar_dates.setdefault(r["service_id"], []).append(entry)
    except Exception as exc:
        log.warning("calendar_dates.txt parse issue: %s", exc)

    # ---- Scan stop_times for relevant stops ---------------------------------
    log.info("Scanning stop_times for %d GTFS stop_ids …", len(all_stop_ids))
    relevant_trip_ids: set = set()
    # stop_times_by_trip_stop: (trip_id, stop_id) → (dep_time, stop_sequence)
    # We store one row per (trip, stop) — for a given stop there is typically
    # exactly one row per trip, but we keep the earliest stop_sequence in case
    # of duplicates.
    raw_stop_times: list[dict] = []
    for r in _open_csv(zf, "stop_times.txt"):
        if r["stop_id"] in all_stop_ids:
            dep = r.get("departure_time") or r.get("arrival_time", "")
            raw_stop_times.append({
                "trip_id": r["trip_id"],
                "stop_id": r["stop_id"],
                "departure_time": dep.strip(),
                "stop_sequence": int(r.get("stop_sequence", 0) or 0),
            })
            relevant_trip_ids.add(r["trip_id"])
    log.info(
        "Found %d stop_time rows across %d trips for all configured stops",
        len(raw_stop_times), len(relevant_trip_ids),
    )

    # ---- Parse trips (only those relevant to our stops) ---------------------
    log.info("Parsing trips (filtering to relevant) …")
    trips: dict = {}  # trip_id → {route_id, service_id, headsign}
    for r in _open_csv(zf, "trips.txt"):
        if r["trip_id"] in relevant_trip_ids:
            trips[r["trip_id"]] = {
                "route_id": r["route_id"],
                "service_id": r["service_id"],
                "headsign": r.get("trip_headsign", ""),
            }
    log.info("Kept %d trips", len(trips))

    # ---- Build per-stop trip lists -----------------------------------------
    # Index stop_times by trip_id for quick lookup.
    st_by_trip: dict[str, list] = {}
    for st in raw_stop_times:
        st_by_trip.setdefault(st["trip_id"], []).append(st)

    stops_out: dict = {}
    for stop_cfg in stops_config:
        key = stop_cfg["key"]
        label = stop_cfg.get("label", key)
        route_filter = set(stop_cfg.get("routes") or [])
        gtfs_stop_ids = resolved_by_key.get(key, set())

        trip_list: list[dict] = []
        for trip_id, trip in trips.items():
            route_short = routes.get(trip["route_id"], trip["route_id"])
            if route_filter and route_short not in route_filter:
                continue

            for st in st_by_trip.get(trip_id, []):
                if st["stop_id"] not in gtfs_stop_ids:
                    continue
                dep = st.get("departure_time", "")
                if not dep:
                    continue
                trip_list.append({
                    "trip_id": trip_id,
                    "route": route_short,
                    "dest": trip.get("headsign", ""),
                    "service_id": trip["service_id"],
                    "dep": dep,
                })

        if not trip_list:
            log.warning(
                "WARNING: Stop %r resolved to GTFS stop_ids %s but yielded ZERO trips "
                "for routes %s — check your match_ids and routes in config.yaml",
                key, sorted(gtfs_stop_ids), sorted(route_filter),
            )
        else:
            log.info("Stop %r: %d trips", key, len(trip_list))

        stops_out[key] = {
            "label": label,
            "routes": list(stop_cfg.get("routes") or []),
            # gtfs_stop_ids is the set of GTFS stop_id values (from stops.txt)
            # that correspond to this logical stop.  update_departures.py uses
            # this list to look up the correct StopTimeUpdate entry in RT feeds.
            "gtfs_stop_ids": sorted(gtfs_stop_ids),
            "trips": trip_list,
        }

    return {
        "version": 3,
        "built_at": int(time.time()),
        "timezone": "Australia/Brisbane",
        "stops": stops_out,
        "calendar": calendar,
        "calendar_dates": calendar_dates,
    }


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description="Build stop_schedule.json (v3) from static GTFS.")
    parser.add_argument("--config", default=None, help="Path to config.yaml")
    args = parser.parse_args()

    config_path = args.config or _find_config()
    log.info("Using config: %s", config_path)
    config = _load_config(config_path)

    stops_config = config.get("stops", [])
    if not stops_config:
        log.error("No 'stops' configured in config.yaml — add at least one stop entry.")
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
        schedule = _parse_gtfs_v3(zf, stops_config)

    out_path = here / "stop_schedule.json"
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(schedule, f, separators=(",", ":"))

    sz = out_path.stat().st_size
    log.info("Written %s (%.1f KB) — version %s, %d stops",
             out_path, sz / 1024, schedule.get("version"), len(schedule.get("stops", {})))

    # Clean up the large zip to keep the repo lean (it's not committed).
    try:
        zip_path.unlink()
        log.info("Removed temporary zip cache.")
    except Exception:
        pass

    return 0


if __name__ == "__main__":
    sys.exit(main())
