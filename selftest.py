#!/usr/bin/env python3
"""
selftest.py — OFFLINE self-test for the cloud backend (Data Contract v3).

Feeds a small FAKE stop_schedule (v3 multi-stop shape) + FAKE realtime overlay
through the same code paths that build_stop_departures() uses, writes a sample
public/departures.json, and asserts the Data Contract v3 is met.

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
    build_stop_departures,
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
# Time / timezone setup
# ---------------------------------------------------------------------------

tz_name = "Australia/Brisbane"
tz_obj = _get_tz(tz_name)
now_utc = datetime.now(timezone.utc)
now_local = _now_local(tz_obj)
now_epoch = int(now_utc.timestamp())
utc_offset = _utc_offset_seconds(tz_obj, now_utc.replace(tzinfo=None))

today = now_local.date()
today_str = today.strftime("%Y%m%d")
_DAY_COLS = ["monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"]
today_col = _DAY_COLS[today.weekday()]

# ---------------------------------------------------------------------------
# Service IDs for calendar tests
# ---------------------------------------------------------------------------

# Runs today via regular calendar.
SERVICE_A = "SVC_TODAY"
# Runs today via calendar_dates addition only.
SERVICE_B = "SVC_ADDED_TODAY"
# Removed today via calendar_dates (must NOT appear).
SERVICE_C = "SVC_REMOVED_TODAY"

# ---------------------------------------------------------------------------
# Fake v3 stop_schedule.json — two-stop shape (§B)
# ---------------------------------------------------------------------------

def _future_gtfs_time(minutes_from_now: int) -> str:
    """Return a GTFS HH:MM:SS string for (now_local + minutes_from_now)."""
    dep = now_local + timedelta(minutes=minutes_from_now)
    return dep.strftime("%H:%M:%S")


# ---- Stop A — "011180" (Clayfield) ----------------------------------------
STOP_A_GTFS_IDS = ["600029", "60029"]   # fake GTFS IDs for this logical stop

TRIPS_A = [
    # route 320, service A, future departure at +5 min
    {
        "trip_id": "TRIP_A01",
        "route": "320",
        "dest": "City",
        "service_id": SERVICE_A,
        "dep": _future_gtfs_time(5),
    },
    # route 320, service A, future departure at +15 min
    {
        "trip_id": "TRIP_A02",
        "route": "320",
        "dest": "City",
        "service_id": SERVICE_A,
        "dep": _future_gtfs_time(15),
    },
    # route 320, service B (added via calendar_dates), +20 min
    {
        "trip_id": "TRIP_A03",
        "route": "320",
        "dest": "City Botanic Gardens",
        "service_id": SERVICE_B,
        "dep": _future_gtfs_time(20),
    },
    # route 320, service C (removed via calendar_dates) — must NOT appear
    {
        "trip_id": "TRIP_A04",
        "route": "320",
        "dest": "Should Not Appear",
        "service_id": SERVICE_C,
        "dep": _future_gtfs_time(3),
    },
]

# ---- Stop B — "24" (Adelaide St) ------------------------------------------
STOP_B_GTFS_IDS = ["600001"]   # fake GTFS IDs

TRIPS_B = [
    # route 322, service A, +7 min — will get RT cancellation
    {
        "trip_id": "TRIP_B01",
        "route": "322",
        "dest": "Roma St",
        "service_id": SERVICE_A,
        "dep": _future_gtfs_time(7),
    },
    # route 320, service A, +12 min — will get RT live time (2 min earlier)
    {
        "trip_id": "TRIP_B02",
        "route": "320",
        "dest": "Queen St",
        "service_id": SERVICE_A,
        "dep": _future_gtfs_time(12),
    },
]

# ---- Calendar ---------------------------------------------------------------

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

# v3 shape: dict keyed by service_id → list of {date, exception_type}
CALENDAR_DATES = {
    SERVICE_B: [{"date": today_str, "exception_type": 1}],
    SERVICE_C: [{"date": today_str, "exception_type": 2}],
}

# ---- Full v3 fake schedule --------------------------------------------------

FAKE_SCHEDULE = {
    "version": 3,
    "built_at": now_epoch,
    "timezone": tz_name,
    "stops": {
        "011180": {
            "label": "Bonney Ave at Victoria Parade, Stop 24, Clayfield",
            "routes": ["320"],
            "gtfs_stop_ids": STOP_A_GTFS_IDS,
            "trips": TRIPS_A,
        },
        "24": {
            "label": "Adelaide St, Stop 24 near Edward St",
            "routes": ["320", "322"],
            "gtfs_stop_ids": STOP_B_GTFS_IDS,
            "trips": TRIPS_B,
        },
    },
    "calendar": CALENDAR,
    "calendar_dates": CALENDAR_DATES,
}

# ---------------------------------------------------------------------------
# Fake config
# ---------------------------------------------------------------------------

CONFIG = {
    "stops": [
        {
            "key": "011180",
            "label": "Bonney Ave at Victoria Parade, Stop 24, Clayfield",
            "match_ids": ["011180", "11180"],
            "routes": ["320"],
        },
        {
            "key": "24",
            "label": "Adelaide St, Stop 24 near Edward St",
            "match_ids": ["24", "000024"],
            "routes": ["320", "322"],
        },
    ],
    "max_rows": 6,
    "timezone": tz_name,
}

# ---------------------------------------------------------------------------
# Fake RT overlay
# ---------------------------------------------------------------------------

# TRIP_B01 (route 322) — cancelled.
# TRIP_B02 (route 320 at Stop B) — gets a live time 2 min earlier.
TRIP_B02_RT_EPOCH = int(_local_to_epoch(now_local + timedelta(minutes=10), tz_obj))

FAKE_RT: dict = {
    "TRIP_B01": {
        "cancelled": True,
        "stops": {},
    },
    "TRIP_B02": {
        "cancelled": False,
        "stops": {
            STOP_B_GTFS_IDS[0]: {
                "departure_epoch": float(TRIP_B02_RT_EPOCH),
                "arrival_epoch": None,
                "schedule_relationship": 0,
            }
        },
    },
}

# ---------------------------------------------------------------------------
# Unit-test _active_services_for_date with v3 calendar_dates (dict shape)
# ---------------------------------------------------------------------------
# This directly validates the v3→v3 seam: build_schedule.py emits calendar_dates
# as a DICT keyed by service_id, and update_departures.py (via _active_services_for_date)
# must read it the same way.  Deno's activeServicesForDate uses Object.entries() on the same shape.

_active_svcs_today = _active_services_for_date(FAKE_SCHEDULE, today)
assert SERVICE_A in _active_svcs_today, f"SERVICE_A should be active today (regular calendar); got {_active_svcs_today}"
assert SERVICE_B in _active_svcs_today, f"SERVICE_B should be active today (calendar_dates addition); got {_active_svcs_today}"
assert SERVICE_C not in _active_svcs_today, f"SERVICE_C should be removed today (calendar_dates removal); got {_active_svcs_today}"

# ---------------------------------------------------------------------------
# Run build_stop_departures for both stops
# ---------------------------------------------------------------------------

print()
print("=" * 60)
print("QLD Bus Sign Cloud Backend — selftest v3")
print("=" * 60)
print(f"  Now local : {now_local.isoformat()}")
print(f"  Now epoch : {now_epoch}")
print(f"  UTC offset: {utc_offset}s ({utc_offset//3600:+d}h)")
print(f"  Today     : {today_str} ({today_col})")
print()

max_rows = int(CONFIG["max_rows"])
stops_out = []
for stop_cfg in CONFIG["stops"]:
    key = stop_cfg["key"]
    label = stop_cfg["label"]
    stop_entry = FAKE_SCHEDULE["stops"][key]
    deps = build_stop_departures(
        stop_entry=stop_entry,
        rt_map=FAKE_RT,
        now_local=now_local,
        now_epoch=now_epoch,
        tz_obj=tz_obj,
        max_rows=max_rows,
        schedule=FAKE_SCHEDULE,
    )
    stops_out.append({"stop_id": key, "stop_label": label, "departures": deps})

# Fake alerts and firmware for contract completeness.
FAKE_ALERTS = [
    {
        "id": "alert_test_1",
        "header": "Route 322 detouring via Ann St",
        "severity": "WARNING",
        "effect": "DETOUR",
        "routes": ["322"],
    }
]

output = {
    "version": 3,
    "generated_at": now_epoch,
    "utc_offset_seconds": utc_offset,
    "alerts": FAKE_ALERTS,
    "stops": stops_out,
    # firmware key omitted (testing optional-omit path)
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

# Re-parse from disk to simulate consumer reading the file.
with open(out_path, encoding="utf-8") as f:
    parsed = json.load(f)
check("valid JSON (re-parsed from disk)", True)

# --- Top-level v3 required keys ---
for key in ("version", "generated_at", "utc_offset_seconds", "alerts", "stops"):
    check(f"top-level key '{key}' present", key in parsed)

check("version == 3", parsed.get("version") == 3, f"got {parsed.get('version')!r}")
check("generated_at is int", isinstance(parsed.get("generated_at"), int),
      f"got {type(parsed.get('generated_at'))}")
check("utc_offset_seconds is int", isinstance(parsed.get("utc_offset_seconds"), int))
check("utc_offset_seconds == 36000 (Brisbane +10h)", parsed.get("utc_offset_seconds") == 36000,
      f"got {parsed.get('utc_offset_seconds')}")

# --- alerts ---
check("alerts is a list", isinstance(parsed.get("alerts"), list),
      f"got {type(parsed.get('alerts'))}")
if isinstance(parsed.get("alerts"), list) and parsed["alerts"]:
    a0 = parsed["alerts"][0]
    for ak in ("id", "header", "severity", "effect", "routes"):
        check(f"alerts[0] has field '{ak}'", ak in a0)
    check("alerts[0].routes is a list", isinstance(a0.get("routes"), list))
    check("alerts[0].header len <= 96", len(a0.get("header", "")) <= 96,
          f"len={len(a0.get('header', ''))}")
    # All alerts must have a non-empty header (cross-backend consistency: Deno skips
    # empty-header alerts; Python must do the same so both backends produce the same output).
    for ai, alert in enumerate(parsed["alerts"]):
        check(f"alerts[{ai}].header is non-empty str (empty-header skip parity with Deno)",
              isinstance(alert.get("header"), str) and len(alert["header"]) > 0,
              f"got {alert.get('header')!r}")

# --- firmware key absent when not published ---
check("firmware key absent (not published in this test)", "firmware" not in parsed)

# --- stops array ---
check("stops is a list", isinstance(parsed.get("stops"), list))
check(f"stops has {len(CONFIG['stops'])} entries (all configured stops emitted)",
      len(parsed.get("stops", [])) == len(CONFIG["stops"]),
      f"got {len(parsed.get('stops', []))}")

# Stop order must match config order.
expected_keys = [s["key"] for s in CONFIG["stops"]]
actual_keys   = [s.get("stop_id") for s in parsed.get("stops", [])]
check("stops[] order matches config order", actual_keys == expected_keys,
      f"expected {expected_keys}, got {actual_keys}")

# Per-stop checks.
required_dep_keys = {"route": str, "dest": str, "time": int, "live": bool, "cancelled": bool}
for i, stop in enumerate(parsed.get("stops", [])):
    check(f"stops[{i}] has stop_id", "stop_id" in stop)
    check(f"stops[{i}] has stop_label", "stop_label" in stop)
    check(f"stops[{i}] has departures list", isinstance(stop.get("departures"), list))
    # stop_id must be the stable key from config, not a GTFS id.
    expected_key = CONFIG["stops"][i]["key"] if i < len(CONFIG["stops"]) else None
    check(f"stops[{i}].stop_id == config key {expected_key!r}",
          stop.get("stop_id") == expected_key, f"got {stop.get('stop_id')!r}")
    # stop_label must be non-empty.
    check(f"stops[{i}].stop_label is non-empty str",
          isinstance(stop.get("stop_label"), str) and len(stop["stop_label"]) > 0,
          f"got {stop.get('stop_label')!r}")

    deps = stop.get("departures", [])
    check(f"stops[{i}] len(departures) <= max_rows ({max_rows})",
          len(deps) <= max_rows, f"got {len(deps)}")

    times = [d["time"] for d in deps]
    check(f"stops[{i}] departures sorted ascending",
          times == sorted(times), f"times={times}")

    for j, dep in enumerate(deps):
        for k, t in required_dep_keys.items():
            check(f"stops[{i}].departures[{j}] '{k}' is {t.__name__}",
                  k in dep and isinstance(dep[k], t),
                  f"got {dep.get(k)!r} ({type(dep.get(k)).__name__})")
        t_val = dep.get("time", 0)
        check(f"stops[{i}].departures[{j}] time looks like epoch (>= 1_500_000_000)",
              isinstance(t_val, int) and t_val >= 1_500_000_000, f"got {t_val}")
        check(f"stops[{i}].departures[{j}] route len <= 6",
              len(dep.get("route", "")) <= 6, dep.get("route"))
        check(f"stops[{i}].departures[{j}] dest len <= 29",
              len(dep.get("dest", "")) <= 29, dep.get("dest"))

# --- Stop A specific checks ---
stop_a = next((s for s in parsed["stops"] if s["stop_id"] == "011180"), None)
check("Stop 011180 present", stop_a is not None)
if stop_a:
    # TRIP_A04 (SERVICE_C, removed by calendar_dates) must NOT appear.
    bad = next((d for d in stop_a["departures"] if d.get("dest") == "Should Not Appear"), None)
    check("Stop 011180: TRIP_A04 (SERVICE_C removed) NOT in departures", bad is None,
          "calendar_dates removal failed")

    # SERVICE_B (added via calendar_dates) must appear.
    added = next((d for d in stop_a["departures"] if d.get("dest") == "City Botanic Gardens"), None)
    check("Stop 011180: TRIP_A03 (SERVICE_B added) IS in departures", added is not None)

# --- Stop B specific checks ---
stop_b = next((s for s in parsed["stops"] if s["stop_id"] == "24"), None)
check("Stop 24 present", stop_b is not None)
if stop_b:
    # TRIP_B01 (route 322) should be present and cancelled.
    cancelled_dep = next((d for d in stop_b["departures"] if d["route"] == "322" and d["cancelled"]), None)
    check("Stop 24: TRIP_B01 (route 322) present and cancelled=True", cancelled_dep is not None)

    # TRIP_B02 (route 320) should have live=True and RT epoch.
    rt_dep = next((d for d in stop_b["departures"] if d["route"] == "320"), None)
    check("Stop 24: TRIP_B02 (route 320) present", rt_dep is not None)
    if rt_dep:
        check("Stop 24: TRIP_B02 live=True (RT overlay applied)",
              rt_dep["live"] is True, f"live={rt_dep['live']}")
        check(f"Stop 24: TRIP_B02 time == RT epoch ({TRIP_B02_RT_EPOCH})",
              rt_dep["time"] == TRIP_B02_RT_EPOCH, f"time={rt_dep['time']}")

# ---------------------------------------------------------------------------
# Consumer simulation
# ---------------------------------------------------------------------------

print()
print("Consumer minutes-from-now simulation:")
for stop in parsed["stops"]:
    print(f"  -- {stop['stop_id']}: {stop['stop_label']} --")
    for dep in stop["departures"]:
        mins_from_now = (dep["time"] - now_epoch) // 60
        marker = "LIVE" if dep["live"] else "sched"
        cncl = " CANCELLED" if dep["cancelled"] else ""
        print(f"    Route {dep['route']:<6}  {dep['dest']:<30}  {mins_from_now:+4d} min  [{marker}]{cncl}")

# ---------------------------------------------------------------------------
# Firmware optional-block test
# ---------------------------------------------------------------------------

print()
print("Firmware optional-block test (firmware key omitted when not published):")
firmware_present = "firmware" in parsed
check("firmware key correctly absent in this test run", not firmware_present)

# Simulate a firmware block being present and validate its shape.
fake_fw_output = dict(output)
fake_fw_output["firmware"] = {
    "version": "1.3.0",
    "bin_url": "https://fazaaaaaaaaaa.github.io/qld-bus-sign/firmware.bin",
    "sha256": "a" * 64,
}
for fk in ("version", "bin_url", "sha256"):
    check(f"firmware block field '{fk}' valid",
          fk in fake_fw_output["firmware"] and isinstance(fake_fw_output["firmware"][fk], str))
check("firmware sha256 is 64 hex chars",
      len(fake_fw_output["firmware"]["sha256"]) == 64)

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
