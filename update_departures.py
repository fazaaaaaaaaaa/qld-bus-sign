#!/usr/bin/env python3
"""
update_departures.py — Frequent job (runs every 5 min via GitHub Actions).

Loads stop_schedule.json (precomputed by build_schedule.py), determines which
trips run today, fetches GTFS-RT TripUpdates, merges them, and writes
cloud/public/departures.json conforming to the Data Contract v2.

The job NEVER hard-fails on a transient feed error — if RT is unavailable,
scheduled-only data (live=false) is emitted and the process exits 0 so the
sign keeps showing something useful.

Data Contract v2 output shape:
{
  "stop_label": "...",
  "generated_at": <epoch int>,
  "utc_offset_seconds": <int>,
  "departures": [
    {
      "route": "412",
      "dest": "City Botanic Gardens",
      "time": <epoch int>,
      "live": <bool>,
      "cancelled": <bool>
    },
    ...
  ]
}
"""

import argparse
import json
import logging
import os
import sys
import unicodedata
from datetime import date, datetime, timedelta, timezone
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

def _find_config(hint: str | None = None) -> str:
    if hint and os.path.exists(hint):
        return hint
    here = Path(__file__).parent
    for candidate in [here / "config.yaml", here / "config.example.yaml"]:
        if candidate.exists():
            return str(candidate)
    raise FileNotFoundError("No config.yaml found.")


def _load_config(path: str) -> dict:
    with open(path) as f:
        return yaml.safe_load(f)


# ---------------------------------------------------------------------------
# Timezone helpers
# ---------------------------------------------------------------------------

def _get_tz(tz_name: str):
    """Return a datetime.timezone object for the given IANA tz name."""
    try:
        from zoneinfo import ZoneInfo
        return ZoneInfo(tz_name)
    except ImportError:
        # Python < 3.9 fallback via pytz if installed.
        try:
            import pytz
            return pytz.timezone(tz_name)
        except ImportError:
            log.warning("Neither zoneinfo nor pytz available; defaulting to UTC+10 (Brisbane).")
            return timezone(timedelta(hours=10))


def _utc_offset_seconds(tz_obj, when: datetime) -> int:
    """Return the UTC offset in seconds for a tz at a given (naive UTC) moment."""
    try:
        # zoneinfo path
        from zoneinfo import ZoneInfo
        aware = when.replace(tzinfo=timezone.utc).astimezone(tz_obj)
        return int(aware.utcoffset().total_seconds())
    except Exception:
        pass
    try:
        # pytz path
        utcoffset = tz_obj.utcoffset(when)
        return int(utcoffset.total_seconds())
    except Exception:
        return 36000  # Brisbane default


def _now_local(tz_obj) -> datetime:
    """Return current naive local datetime in tz_obj."""
    utc_now = datetime.now(timezone.utc)
    try:
        from zoneinfo import ZoneInfo
        local = utc_now.astimezone(tz_obj)
        return local.replace(tzinfo=None)
    except Exception:
        pass
    try:
        local = utc_now.astimezone(tz_obj)
        return local.replace(tzinfo=None)
    except Exception:
        # Fallback: apply offset manually.
        offset_s = _utc_offset_seconds(tz_obj, utc_now.replace(tzinfo=None))
        return (utc_now + timedelta(seconds=offset_s)).replace(tzinfo=None)


def _local_to_epoch(naive_local: datetime, tz_obj) -> int:
    """Convert a naive local datetime to UTC epoch seconds."""
    try:
        from zoneinfo import ZoneInfo
        aware = naive_local.replace(tzinfo=tz_obj)
        return int(aware.timestamp())
    except Exception:
        pass
    try:
        # pytz path
        aware = tz_obj.localize(naive_local, is_dst=None)
        return int(aware.timestamp())
    except Exception:
        # Fallback: use stored offset.
        offset_s = _utc_offset_seconds(tz_obj, naive_local)
        utc = naive_local - timedelta(seconds=offset_s)
        return int((utc - datetime(1970, 1, 1)).total_seconds())


# ---------------------------------------------------------------------------
# Calendar helpers (mirrors server/gtfs_static.py logic)
# ---------------------------------------------------------------------------

_DAY_COLS = ["monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"]


def _is_service_active(schedule: dict, service_id: str, check_date: date) -> bool:
    """Return True if service_id runs on check_date."""
    date_str = check_date.strftime("%Y%m%d")
    day_col = _DAY_COLS[check_date.weekday()]

    # calendar_dates overrides first.
    for cd in schedule.get("calendar_dates", []):
        if cd["service_id"] == service_id and cd["date"] == date_str:
            return cd["exception_type"] == 1  # 1=added, 2=removed

    # Regular calendar.
    cal = schedule.get("calendar", {}).get(service_id)
    if not cal:
        return False
    runs = bool(cal.get(day_col, 0))
    in_range = cal.get("start_date", "") <= date_str <= cal.get("end_date", "")
    return runs and in_range


def _active_services_for_date(schedule: dict, check_date: date) -> set:
    """Return set of service_ids active on check_date."""
    date_str = check_date.strftime("%Y%m%d")
    day_col = _DAY_COLS[check_date.weekday()]

    # Collect additions and removals from calendar_dates.
    added = set()
    removed = set()
    for cd in schedule.get("calendar_dates", []):
        if cd["date"] == date_str:
            if cd["exception_type"] == 1:
                added.add(cd["service_id"])
            elif cd["exception_type"] == 2:
                removed.add(cd["service_id"])

    # Regular calendar services active that day.
    regular = set()
    for svc_id, cal in schedule.get("calendar", {}).items():
        if (bool(cal.get(day_col, 0))
                and cal.get("start_date", "") <= date_str <= cal.get("end_date", "")):
            regular.add(svc_id)

    return (regular | added) - removed


# ---------------------------------------------------------------------------
# GTFS time parsing
# ---------------------------------------------------------------------------

def _parse_gtfs_time(time_str: str, base_date: date) -> datetime:
    """
    Parse a GTFS departure_time (HH:MM:SS; HH may exceed 23 for after-midnight
    services) into a naive local datetime on base_date.
    """
    parts = time_str.strip().split(":")
    if len(parts) != 3:
        raise ValueError(f"Bad GTFS time: {time_str!r}")
    h, m, s = int(parts[0]), int(parts[1]), int(parts[2])
    return datetime(base_date.year, base_date.month, base_date.day) + timedelta(
        hours=h, minutes=m, seconds=s
    )


# ---------------------------------------------------------------------------
# Text helpers
# ---------------------------------------------------------------------------

def _ascii_fold(text: str) -> str:
    """Normalize unicode text to ASCII-safe representation."""
    nfkd = unicodedata.normalize("NFKD", text)
    return "".join(c for c in nfkd if not unicodedata.combining(c))


def _truncate(text: str, max_chars: int) -> str:
    text = text.strip()
    if len(text) <= max_chars:
        return text
    return text[: max_chars - 1].rstrip() + "…"  # …


# ---------------------------------------------------------------------------
# RT feed fetch (mirrors server/gtfs_realtime.py; bundled here so cloud/ is
# standalone and does not depend on the server/ directory being present)
# ---------------------------------------------------------------------------

_TRIP_SR_CANCELED = 3
_STOP_SR_SKIPPED = 1


def _fetch_rt(url: str, timeout: int) -> dict:
    """
    Fetch GTFS-RT TripUpdates. Returns {} on any error.

    Result keyed by trip_id:
    {
      trip_id: {
        "cancelled": bool,
        "stops": {
          stop_id: {
            "departure_epoch": float|None,
            "arrival_epoch": float|None,
            "schedule_relationship": int,
          }
        }
      }
    }
    """
    try:
        resp = requests.get(url, timeout=timeout)
        resp.raise_for_status()
    except Exception as exc:
        log.warning("RT fetch failed (non-fatal): %s", exc)
        return {}

    try:
        from google.transit import gtfs_realtime_pb2
        feed = gtfs_realtime_pb2.FeedMessage()
        feed.ParseFromString(resp.content)
    except Exception as exc:
        log.warning("RT parse failed (non-fatal): %s", exc)
        return {}

    result = {}
    for entity in feed.entity:
        if not entity.HasField("trip_update"):
            continue
        tu = entity.trip_update
        trip_id = tu.trip.trip_id
        if not trip_id:
            continue
        is_cancelled = (tu.trip.schedule_relationship == _TRIP_SR_CANCELED)
        stops = {}
        for stu in tu.stop_time_update:
            sid = stu.stop_id
            if not sid:
                continue
            dep_epoch = float(stu.departure.time) if stu.HasField("departure") and stu.departure.time else None
            arr_epoch = float(stu.arrival.time) if stu.HasField("arrival") and stu.arrival.time else None
            stops[sid] = {
                "departure_epoch": dep_epoch,
                "arrival_epoch": arr_epoch,
                "schedule_relationship": stu.schedule_relationship,
            }
        result[trip_id] = {"cancelled": is_cancelled, "stops": stops}

    log.info("Parsed %d RT trip updates", len(result))
    return result


# ---------------------------------------------------------------------------
# Build departures
# ---------------------------------------------------------------------------

def build_departures(schedule: dict, rt_map: dict, now_local: datetime,
                     now_epoch: int, tz_obj, config: dict) -> list:
    """
    Merge static schedule + realtime overlay into the Data Contract v2 list.

    Returns list of departure dicts sorted ascending by "time" (epoch seconds).
    """
    max_rows = int(config.get("max_rows", 8))
    route_filter = set(config.get("route_filter") or [])
    stop_ids = set(str(s) for s in config.get("stop_ids", []))

    # Filter window: from (now - 60s) onward.
    cutoff_epoch = now_epoch - 60

    # Build lookup maps for speed.
    routes = schedule.get("routes", {})   # route_id → route_short_name
    trips = schedule.get("trips", {})     # trip_id → {route_id, service_id, headsign}

    # Build a dict of stop_times by trip_id for quick lookup.
    # stop_times_by_trip: trip_id → list of {stop_id, departure_time, stop_sequence}
    stop_times_by_trip: dict[str, list] = {}
    for st in schedule.get("stop_times", []):
        stop_times_by_trip.setdefault(st["trip_id"], []).append(st)

    # Determine today's date and yesterday's (for after-midnight services).
    today = now_local.date()
    dates_to_check = [today - timedelta(days=1), today]

    raw: list[dict] = []

    for check_date in dates_to_check:
        active_services = _active_services_for_date(schedule, check_date)
        if not active_services:
            continue

        for trip_id, trip in trips.items():
            if trip["service_id"] not in active_services:
                continue

            route_id = trip["route_id"]
            route_short = routes.get(route_id, route_id)
            headsign = trip.get("headsign", "")

            if route_filter and route_short not in route_filter:
                continue

            for st in stop_times_by_trip.get(trip_id, []):
                if st["stop_id"] not in stop_ids:
                    continue
                dep_time_str = st.get("departure_time", "")
                if not dep_time_str:
                    continue
                try:
                    sched_dt = _parse_gtfs_time(dep_time_str, check_date)
                except Exception:
                    continue

                # Convert scheduled datetime to epoch.
                sched_epoch = _local_to_epoch(sched_dt, tz_obj)

                # Apply RT overlay.
                cancelled = False
                live = False
                dep_epoch = sched_epoch

                rt_trip = rt_map.get(trip_id)
                if rt_trip:
                    if rt_trip["cancelled"]:
                        cancelled = True
                    else:
                        rt_stop = rt_trip["stops"].get(st["stop_id"])
                        if rt_stop:
                            rt_dep = rt_stop.get("departure_epoch") or rt_stop.get("arrival_epoch")
                            if rt_dep:
                                dep_epoch = int(rt_dep)
                                live = True
                            if rt_stop.get("schedule_relationship") == _STOP_SR_SKIPPED:
                                cancelled = True

                # Apply cutoff.
                if dep_epoch < cutoff_epoch:
                    continue

                dest = _ascii_fold(_truncate(headsign, 28))
                raw.append({
                    "route": _truncate(route_short, 6),
                    "dest": dest,
                    "time": dep_epoch,
                    "live": live,
                    "cancelled": cancelled,
                })

    # Sort ascending by time, deduplicate, keep max_rows.
    raw.sort(key=lambda x: x["time"])

    # Deduplicate: same trip can appear via both check_dates — use (trip_id, stop_id) key is
    # unavailable here, but same (route, dest, time) is effectively unique.
    seen = set()
    deduped = []
    for d in raw:
        key = (d["route"], d["dest"], d["time"])
        if key not in seen:
            seen.add(key)
            deduped.append(d)

    return deduped[:max_rows]


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description="Update public/departures.json from RT feed.")
    parser.add_argument("--config", default=None, help="Path to config.yaml")
    parser.add_argument("--schedule", default=None, help="Path to stop_schedule.json")
    parser.add_argument("--out", default=None, help="Output path for departures.json")
    args = parser.parse_args()

    here = Path(__file__).parent
    config_path = _find_config(args.config)
    log.info("Config: %s", config_path)
    config = _load_config(config_path)

    schedule_path = Path(args.schedule) if args.schedule else here / "stop_schedule.json"
    if not schedule_path.exists():
        log.error("stop_schedule.json not found at %s — run build_schedule.py first.", schedule_path)
        return 1

    log.info("Loading %s …", schedule_path)
    with open(schedule_path, encoding="utf-8") as f:
        schedule = json.load(f)

    tz_name = config.get("timezone", "Australia/Brisbane")
    tz_obj = _get_tz(tz_name)

    now_utc = datetime.now(timezone.utc)
    now_local = _now_local(tz_obj)
    now_epoch = int(now_utc.timestamp())
    utc_offset = _utc_offset_seconds(tz_obj, now_utc.replace(tzinfo=None))

    log.info("Now UTC: %s  Local: %s  Offset: %+ds", now_utc.isoformat(), now_local.isoformat(), utc_offset)

    # Fetch RT (non-fatal on error).
    rt_url = config.get("rt_trip_updates_url", "")
    timeout = int(config.get("feed_timeout_seconds", 15))
    rt_map = {}
    if rt_url:
        log.info("Fetching RT from %s …", rt_url)
        rt_map = _fetch_rt(rt_url, timeout)
    else:
        log.warning("No rt_trip_updates_url configured; using scheduled only.")

    deps = build_departures(schedule, rt_map, now_local, now_epoch, tz_obj, config)

    # Build output.
    stop_label = config.get("stop_label", schedule.get("_meta", {}).get("stop_label", ""))
    output = {
        "stop_label": stop_label,
        "generated_at": now_epoch,
        "utc_offset_seconds": utc_offset,
        "departures": deps,
    }

    out_path = Path(args.out) if args.out else here / "public" / "departures.json"
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(output, f, separators=(",", ":"), ensure_ascii=True)

    sz = out_path.stat().st_size
    log.info("Written %s (%d bytes, %d departures)", out_path, sz, len(deps))
    return 0


if __name__ == "__main__":
    sys.exit(main())
