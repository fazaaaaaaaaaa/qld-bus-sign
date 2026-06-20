#!/usr/bin/env python3
"""
selftest.py — OFFLINE self-test for the cloud backend.

Feeds a small FAKE stop_schedule + FAKE realtime overlay through the same code
paths that build_departures() uses, writes a sample public/departures.json,
and asserts the Data Contract v2 is met.

NO NETWORK ACCESS IS REQUIRED.

Run with:
    python selftest.py

Exit 0 = PASS, non-zero = FAIL.
"""

import json
import os
import sys
import time
from datetime import datetime, timezone, timedelta
from pathlib import Path

# ---------------------------------------------------------------------------
# Ensure cloud/ is on the path so we can import update_departures directly.
# ---------------------------------------------------------------------------
HERE = Path(__file__).parent
sys.path.insert(0, str(HERE))

from update_departures import (
    build_departures,
    _get_tz,
    _now_local,
    _local_to_epoch,
    _utc_offset_seconds,
    _active_services_for_date,
)

PASS = "\033[32mPASS\033[0m"
FAIL = "\033[31mFAIL\033[0m"
failures = []


def check(name: str, condition: bool, detail: str = "") -> None:
    status = PASS if condition else FAIL
    msg = f"  [{status}] {name}"
    if not condition and detail:
        msg += f"  ({detail})"
    print(msg)
    if not condition:
        failures.append(name)


# ---------------------------------------------------------------------------
# Build a FAKE stop_schedule that mimics the real structure.
# Uses TODAY's date so the calendar check passes.
# ---------------------------------------------------------------------------

tz_name = "Australia/Brisbane"
tz_obj = _get_tz(tz_name)
now_utc = datetime.now(timezone.utc)
now_local = _now_local(tz_obj)
now_epoch = int(now_utc.timestamp())
utc_offset = _utc_offset_seconds(tz_obj, now_utc.replace(tzinfo=None))

today = now_local.date()
today_str = today.strftime("%Y%m%d")
# Weekday name for calendar
_DAY_COLS = ["monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"]
today_col = _DAY_COLS[today.weekday()]

# Service that runs today via the regular calendar.
SERVICE_A = "SVC_TODAY"
# Service that runs today via a calendar_dates addition (no regular calendar entry).
SERVICE_B = "SVC_ADDED_TODAY"
# Service that is removed today via calendar_dates (should NOT appear).
SERVICE_C = "SVC_REMOVED_TODAY"

# Fake trips.
TRIPS = {
    "TRIP_001": {"route_id": "R412", "service_id": SERVICE_A, "headsign": "City Botanic Gardens"},
    "TRIP_002": {"route_id": "R130", "service_id": SERVICE_A, "headsign": "Roma Street Station"},
    "TRIP_003": {"route_id": "R66",  "service_id": SERVICE_B, "headsign": "Carindale"},
    "TRIP_004": {"route_id": "R412", "service_id": SERVICE_C, "headsign": "Should Not Appear"},
    "TRIP_005": {"route_id": "R412", "service_id": SERVICE_A, "headsign": "City Botanic Gardens"},
}

ROUTES = {
    "R412": "412",
    "R130": "130",
    "R66":  "66",
}

# Build departure times relative to now so they fall in the near future.
# GTFS time = local HH:MM:SS on today's date.
def _future_gtfs_time(minutes_from_now: int) -> str:
    """Return a GTFS departure_time string for (now_local + minutes_from_now)."""
    dep = now_local + timedelta(minutes=minutes_from_now)
    # GTFS time may represent hours >= 24 for post-midnight — here we stay same-day.
    return dep.strftime("%H:%M:%S")

STOP_TIMES = [
    {"trip_id": "TRIP_001", "stop_id": "600029", "departure_time": _future_gtfs_time(3),  "stop_sequence": 5},
    {"trip_id": "TRIP_002", "stop_id": "600029", "departure_time": _future_gtfs_time(8),  "stop_sequence": 3},
    {"trip_id": "TRIP_003", "stop_id": "600029", "departure_time": _future_gtfs_time(12), "stop_sequence": 7},
    {"trip_id": "TRIP_004", "stop_id": "600029", "departure_time": _future_gtfs_time(2),  "stop_sequence": 2},  # should be excluded
    {"trip_id": "TRIP_005", "stop_id": "600029", "departure_time": _future_gtfs_time(20), "stop_sequence": 5},
]

# Calendar: SERVICE_A runs today (and all week in a broad date range).
CALENDAR = {
    SERVICE_A: {
        "monday": 1, "tuesday": 1, "wednesday": 1,
        "thursday": 1, "friday": 1, "saturday": 1, "sunday": 1,
        "start_date": "20000101",
        "end_date": "20991231",
    },
    SERVICE_C: {
        "monday": 1, "tuesday": 1, "wednesday": 1,
        "thursday": 1, "friday": 1, "saturday": 1, "sunday": 1,
        "start_date": "20000101",
        "end_date": "20991231",
    },
}

# calendar_dates: add SERVICE_B today, remove SERVICE_C today.
CALENDAR_DATES = [
    {"service_id": SERVICE_B, "date": today_str, "exception_type": 1},
    {"service_id": SERVICE_C, "date": today_str, "exception_type": 2},
]

FAKE_SCHEDULE = {
    "_meta": {
        "stop_ids": ["600029"],
        "stop_label": "Queen St Bus Station, Stop A",
        "built_at_epoch": now_epoch,
        "gtfs_zip_url": "https://example.com/FAKE_GTFS.zip",
    },
    "routes": ROUTES,
    "calendar": CALENDAR,
    "calendar_dates": CALENDAR_DATES,
    "trips": TRIPS,
    "stop_times": STOP_TIMES,
}

CONFIG = {
    "stop_ids": ["600029"],
    "stop_label": "Queen St Bus Station, Stop A",
    "max_rows": 8,
    "timezone": tz_name,
    "route_filter": [],
    "rt_trip_updates_url": "",  # blank = no RT in selftest
    "feed_timeout_seconds": 10,
}

# ---------------------------------------------------------------------------
# Fake RT overlay: TRIP_002 gets a predicted departure 2 min earlier than
# scheduled, TRIP_001 gets cancelled.
# ---------------------------------------------------------------------------
TRIP_002_RT_EPOCH = int(_local_to_epoch(now_local + timedelta(minutes=6), tz_obj))

FAKE_RT: dict = {
    "TRIP_001": {
        "cancelled": True,
        "stops": {},
    },
    "TRIP_002": {
        "cancelled": False,
        "stops": {
            "600029": {
                "departure_epoch": float(TRIP_002_RT_EPOCH),
                "arrival_epoch": None,
                "schedule_relationship": 0,
            }
        },
    },
}

# ---------------------------------------------------------------------------
# Run build_departures
# ---------------------------------------------------------------------------

print()
print("=" * 60)
print("QLD Bus Sign Cloud Backend — selftest")
print("=" * 60)
print(f"  Now local : {now_local.isoformat()}")
print(f"  Now epoch : {now_epoch}")
print(f"  UTC offset: {utc_offset}s ({utc_offset//3600:+d}h)")
print(f"  Today     : {today_str} ({today_col})")
print()

deps = build_departures(
    schedule=FAKE_SCHEDULE,
    rt_map=FAKE_RT,
    now_local=now_local,
    now_epoch=now_epoch,
    tz_obj=tz_obj,
    config=CONFIG,
)

# Build the full output contract.
output = {
    "stop_label": CONFIG["stop_label"],
    "generated_at": now_epoch,
    "utc_offset_seconds": utc_offset,
    "departures": deps,
}

# Write sample departures.json.
out_path = HERE / "public" / "departures.json"
out_path.parent.mkdir(parents=True, exist_ok=True)
with open(out_path, "w", encoding="utf-8") as f:
    json.dump(output, f, indent=2)

print(f"Sample departures.json written to: {out_path}")
print()

# ---------------------------------------------------------------------------
# Assertions
# ---------------------------------------------------------------------------

print("Assertions:")

# 1. Valid JSON (we just wrote it, but re-parse to confirm).
with open(out_path, encoding="utf-8") as f:
    parsed = json.load(f)
check("valid JSON", True)

# 2. Required top-level keys.
for key in ("stop_label", "generated_at", "utc_offset_seconds", "departures"):
    check(f"top-level key '{key}' present", key in parsed)

# 3. generated_at is an integer.
check("generated_at is int", isinstance(parsed["generated_at"], int),
      f"got {type(parsed['generated_at'])}")

# 4. utc_offset_seconds is an integer and plausible for Brisbane.
check("utc_offset_seconds is int", isinstance(parsed["utc_offset_seconds"], int))
check("utc_offset_seconds == 36000 (Brisbane +10h)", parsed["utc_offset_seconds"] == 36000,
      f"got {parsed['utc_offset_seconds']}")

# 5. departures is a list.
check("departures is list", isinstance(parsed["departures"], list))

# 6. len <= max_rows.
check(f"len(departures) <= max_rows ({CONFIG['max_rows']})",
      len(parsed["departures"]) <= CONFIG["max_rows"],
      f"got {len(parsed['departures'])}")

# 7. Each departure has required fields with correct types.
required_dep_keys = {"route": str, "dest": str, "time": int, "live": bool, "cancelled": bool}
for i, dep in enumerate(parsed["departures"]):
    for k, t in required_dep_keys.items():
        check(f"dep[{i}] '{k}' is {t.__name__}",
              k in dep and isinstance(dep[k], t),
              f"got {dep.get(k)!r} ({type(dep.get(k)).__name__})")

# 8. Times are absolute epoch integers (not relative minutes).
for i, dep in enumerate(parsed["departures"]):
    t = dep.get("time", 0)
    # A valid epoch for 2020–2040 is between ~1577836800 and ~2208988800.
    check(f"dep[{i}] time looks like epoch (>= 1_500_000_000)",
          isinstance(t, int) and t >= 1_500_000_000,
          f"got {t}")

# 9. Sorted ascending by time.
times = [dep["time"] for dep in parsed["departures"]]
check("departures sorted ascending by time",
      times == sorted(times),
      f"times={times}")

# 10. route <= 6 chars, dest <= 28 chars (with ellipsis headroom).
for i, dep in enumerate(parsed["departures"]):
    check(f"dep[{i}] route len <= 6", len(dep.get("route", "")) <= 6, dep.get("route"))
    check(f"dep[{i}] dest len <= 29", len(dep.get("dest", "")) <= 29, dep.get("dest"))

# 11. Verify RT overlay worked: TRIP_002 should have live=True and time == TRIP_002_RT_EPOCH.
rt_dep = next((d for d in parsed["departures"] if d["route"] == "130"), None)
check("TRIP_002 (route 130) present in output", rt_dep is not None)
if rt_dep:
    check("TRIP_002 live=True (RT overlay applied)", rt_dep["live"] is True, f"live={rt_dep['live']}")
    check(f"TRIP_002 time == RT epoch ({TRIP_002_RT_EPOCH})",
          rt_dep["time"] == TRIP_002_RT_EPOCH, f"time={rt_dep['time']}")

# 12. Verify TRIP_001 (route 412, +3 min) is cancelled.
cancelled_dep = next((d for d in parsed["departures"] if d["route"] == "412" and d["cancelled"]), None)
check("TRIP_001 (route 412) present and cancelled=True (RT cancellation applied)",
      cancelled_dep is not None)

# 13. Verify SERVICE_C trip was removed (TRIP_004 route 412 headsign "Should Not Appear").
bad_dep = next((d for d in parsed["departures"] if d.get("dest") == "Should Not Appear"), None)
check("TRIP_004 (SERVICE_C removed by calendar_dates) NOT in output", bad_dep is None,
      "calendar_dates removal failed")

# 14. Verify SERVICE_B (added via calendar_dates) trip IS present (route 66).
added_dep = next((d for d in parsed["departures"] if d["route"] == "66"), None)
check("TRIP_003 (SERVICE_B added by calendar_dates) IS in output", added_dep is not None)

# 15. Consumer simulation: compute minutes from absolute epoch.
print()
print("Consumer minutes-from-now simulation:")
for dep in parsed["departures"]:
    mins_from_now = (dep["time"] - now_epoch) // 60
    marker = "LIVE" if dep["live"] else "sched"
    cncl = " CANCELLED" if dep["cancelled"] else ""
    print(f"  Route {dep['route']:<6}  {dep['dest']:<30}  {mins_from_now:+4d} min  [{marker}]{cncl}")

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

print()
print("=" * 60)
if failures:
    print(f"RESULT: {FAIL} — {len(failures)} assertion(s) failed:")
    for f_name in failures:
        print(f"  - {f_name}")
    print("=" * 60)
    sys.exit(1)
else:
    print(f"RESULT: {PASS} — all assertions passed.")
    print()
    print("Sample departures.json:")
    print(json.dumps(output, indent=2))
    print("=" * 60)
    sys.exit(0)
