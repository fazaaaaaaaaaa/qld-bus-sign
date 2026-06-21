#!/usr/bin/env python3
"""
update_departures.py — Frequent job (runs every 5 min via GitHub Actions).

Loads stop_schedule.json (precomputed by build_schedule.py), determines which
trips run today per stop, fetches GTFS-RT TripUpdates, fetches GTFS-RT
ServiceAlerts, merges them, and writes cloud/public/departures.json conforming
to Data Contract v3.

The job NEVER hard-fails on a transient feed error — if RT is unavailable,
scheduled-only data (live=false) is emitted and the process exits 0 so the
sign keeps showing something useful.  Alerts failures similarly degrade to
alerts:[] rather than blocking the file.

Data Contract v3 output shape:
{
  "version": 3,
  "generated_at": <epoch int>,
  "utc_offset_seconds": 36000,
  "alerts": [
    {
      "id": "alert_123",
      "header": "Route 322 detouring via Ann St",
      "severity": "WARNING",
      "effect": "DETOUR",
      "routes": ["322"]
    }
  ],
  "stops": [
    {
      "stop_id": "011180",
      "stop_label": "Bonney Ave at Victoria Parade, Stop 24, Clayfield",
      "departures": [
        { "route": "320", "dest": "City", "time": <epoch int>,
          "live": <bool>, "cancelled": <bool> }
      ]
    },
    ...
  ],
  "firmware": {                     // optional; omitted when not published
    "version": "1.3.0",
    "bin_url": "https://...",
    "sha256": "<64 hex chars>"
  }
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
# Calendar helpers (mirrors build_schedule.py logic)
# ---------------------------------------------------------------------------

_DAY_COLS = ["monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"]


def _active_services_for_date(schedule: dict, check_date: date) -> set:
    """Return set of service_ids active on check_date.

    Works with both the v3 calendar_dates format (dict keyed by service_id)
    and the legacy v2 list format, for safety.
    """
    date_str = check_date.strftime("%Y%m%d")
    day_col = _DAY_COLS[check_date.weekday()]

    # Collect additions and removals from calendar_dates.
    added: set = set()
    removed: set = set()
    raw_cd = schedule.get("calendar_dates", {})
    if isinstance(raw_cd, dict):
        # v3 shape: {service_id: [{date, exception_type}]}
        for svc_id, entries in raw_cd.items():
            for cd in entries:
                if cd["date"] == date_str:
                    if cd["exception_type"] == 1:
                        added.add(svc_id)
                    elif cd["exception_type"] == 2:
                        removed.add(svc_id)
    else:
        # v2 shape (legacy): [{service_id, date, exception_type}]
        for cd in raw_cd:
            if cd["date"] == date_str:
                if cd["exception_type"] == 1:
                    added.add(cd["service_id"])
                elif cd["exception_type"] == 2:
                    removed.add(cd["service_id"])

    # Regular calendar services active that day.
    regular: set = set()
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
# RT TripUpdates feed fetch
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
        log.warning("RT TripUpdates fetch failed (non-fatal): %s", exc)
        return {}

    try:
        from google.transit import gtfs_realtime_pb2
        feed = gtfs_realtime_pb2.FeedMessage()
        feed.ParseFromString(resp.content)
    except Exception as exc:
        log.warning("RT TripUpdates parse failed (non-fatal): %s", exc)
        return {}

    result: dict = {}
    for entity in feed.entity:
        if not entity.HasField("trip_update"):
            continue
        tu = entity.trip_update
        trip_id = tu.trip.trip_id
        if not trip_id:
            continue
        is_cancelled = (tu.trip.schedule_relationship == _TRIP_SR_CANCELED)
        stops: dict = {}
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
# RT ServiceAlerts feed fetch
# ---------------------------------------------------------------------------

# GTFS severity_level values (proto enum SeverityLevel).
_SEVERITY_MAP = {
    0: "UNKNOWN",
    1: "INFO",
    2: "WARNING",
    3: "SEVERE",
}

# Collect all configured routes across all stops (used for alert filtering).
def _all_configured_routes(stops_config: list) -> set:
    all_routes: set = set()
    for s in stops_config:
        for r in s.get("routes", []):
            all_routes.add(str(r))
    return all_routes


def _all_configured_stop_keys(stops_config: list) -> set:
    return set(s["key"] for s in stops_config)


def _fetch_alerts(url: str, timeout: int, stops_config: list) -> list:
    """
    Fetch GTFS-RT ServiceAlerts and return a list of alert dicts in v3 shape.

    An alert is kept only if at least one of its informed_entity entries
    matches a configured route OR a configured stop key.

    Returns [] on any failure (404, timeout, parse error, missing field, etc.).
    Never raises.
    """
    if not url:
        return []

    all_routes = _all_configured_routes(stops_config)
    all_stop_keys = _all_configured_stop_keys(stops_config)

    try:
        resp = requests.get(url, timeout=timeout)
        resp.raise_for_status()
    except Exception as exc:
        log.warning("RT ServiceAlerts fetch failed (non-fatal): %s — alerts: []", exc)
        return []

    try:
        from google.transit import gtfs_realtime_pb2
        feed = gtfs_realtime_pb2.FeedMessage()
        feed.ParseFromString(resp.content)
    except Exception as exc:
        log.warning("RT ServiceAlerts parse failed (non-fatal): %s — alerts: []", exc)
        return []

    alerts: list[dict] = []
    try:
        for entity in feed.entity:
            if not entity.HasField("alert"):
                continue
            alert = entity.alert

            # Determine which configured routes this alert touches.
            matched_routes: list[str] = []
            stop_match = False
            for ie in alert.informed_entity:
                r = ie.route_id.strip() if ie.route_id else ""
                s = ie.stop_id.strip() if ie.stop_id else ""
                if r and r in all_routes:
                    if r not in matched_routes:
                        matched_routes.append(r)
                if s and s in all_stop_keys:
                    stop_match = True

            # Keep only if it's relevant to us.
            if not matched_routes and not stop_match:
                continue

            # Header text: prefer English translation, fall back to first.
            header = ""
            try:
                for trans in alert.header_text.translation:
                    if not header or trans.language in ("en", "en-AU", "en-GB"):
                        header = trans.text
                        if trans.language in ("en", "en-AU", "en-GB"):
                            break
            except Exception:
                pass
            header = _ascii_fold(_truncate(header, 96))
            # Skip alerts with no usable header text (matches Deno behaviour).
            if not header:
                continue

            # Severity.
            try:
                severity = _SEVERITY_MAP.get(alert.severity_level, "UNKNOWN")
            except Exception:
                severity = "UNKNOWN"

            # Effect: GTFS effect enum NAME.
            # Try multiple approaches across protobuf API versions.
            try:
                effect_int = alert.effect
                from google.transit import gtfs_realtime_pb2 as _pb2
                try:
                    # protobuf >= 4: use descriptor
                    effect = _pb2.Alert.DESCRIPTOR.fields_by_name["effect"].enum_type.values_by_number[effect_int].name
                except (KeyError, AttributeError):
                    try:
                        # protobuf 3: class method
                        effect = _pb2.Alert.Effect.Name(effect_int)
                    except Exception:
                        effect = str(effect_int)
            except Exception:
                effect = "UNKNOWN_EFFECT"

            alerts.append({
                "id": entity.id,
                "header": header,
                "severity": severity,
                "effect": effect,
                "routes": matched_routes,
            })
    except Exception as exc:
        log.warning("Unexpected error processing ServiceAlerts (non-fatal): %s — alerts: []", exc)
        return []

    log.info("Parsed %d relevant service alerts", len(alerts))
    return alerts


# ---------------------------------------------------------------------------
# Build departures for a single stop (v3 stop entry from stop_schedule.json)
# ---------------------------------------------------------------------------

def build_stop_departures(stop_entry: dict, rt_map: dict, now_local: datetime,
                          now_epoch: int, tz_obj, max_rows: int,
                          schedule: dict) -> list:
    """
    Compute active departures for one stop using the v3 stop entry shape:
    {
      "label": "...",
      "routes": ["320"],
      "gtfs_stop_ids": ["011180", "11180"],   # GTFS stop_id values from stops.txt
      "trips": [
        { "trip_id": "...", "route": "320", "dest": "City",
          "service_id": "...", "dep": "07:42:00" }
      ]
    }

    gtfs_stop_ids is used to look up the correct StopTimeUpdate entry in the
    RT TripUpdates feed — each entry's stop_id is the raw GTFS stop_id.

    Returns a sorted, deduped, capped list of departure dicts.
    """
    cutoff_epoch = now_epoch - 60
    today = now_local.date()
    dates_to_check = [today - timedelta(days=1), today]

    trips_list = stop_entry.get("trips", [])
    # GTFS stop_ids for this logical stop; used for RT overlay lookup.
    gtfs_stop_ids: set = set(stop_entry.get("gtfs_stop_ids", []))

    raw: list[dict] = []

    for check_date in dates_to_check:
        active_services = _active_services_for_date(schedule, check_date)
        if not active_services:
            continue

        for trip in trips_list:
            if trip["service_id"] not in active_services:
                continue

            route_short = trip.get("route", "")
            headsign = trip.get("dest", "")
            trip_id = trip["trip_id"]
            dep_time_str = trip.get("dep", "")

            if not dep_time_str:
                continue

            try:
                sched_dt = _parse_gtfs_time(dep_time_str, check_date)
            except Exception:
                continue

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
                    # Find the StopTimeUpdate entry for this stop.
                    # Look up by each known GTFS stop_id for this logical stop.
                    rt_stop = None
                    for gs_id in gtfs_stop_ids:
                        if gs_id in rt_trip["stops"]:
                            rt_stop = rt_trip["stops"][gs_id]
                            break
                    # If gtfs_stop_ids is unknown/empty, fall back to any entry
                    # that provides a departure time (graceful degradation).
                    if rt_stop is None and not gtfs_stop_ids:
                        for s_data in rt_trip["stops"].values():
                            if s_data.get("departure_epoch") or s_data.get("arrival_epoch"):
                                rt_stop = s_data
                                break
                    if rt_stop:
                        rt_dep = rt_stop.get("departure_epoch") or rt_stop.get("arrival_epoch")
                        if rt_dep:
                            dep_epoch = int(rt_dep)
                            live = True
                        if rt_stop.get("schedule_relationship") == _STOP_SR_SKIPPED:
                            cancelled = True

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
    seen: set = set()
    deduped: list = []
    for d in raw:
        k = (d["route"], d["dest"], d["time"])
        if k not in seen:
            seen.add(k)
            deduped.append(d)

    return deduped[:max_rows]


# ---------------------------------------------------------------------------
# Firmware block
# ---------------------------------------------------------------------------

def _load_firmware_block(config: dict, here: Path) -> dict | None:
    """
    Return a firmware dict {version, bin_url, sha256} or None.

    Priority:
      1. firmware_manifest_url in config (fetch remote JSON).
      2. cloud/public_assets/firmware.json on-disk (written by build-firmware workflow).
      3. None → firmware key is omitted from departures.json.

    Any failure at either source → None (best-effort, never blocks the file).
    """
    timeout = int(config.get("feed_timeout_seconds", 15))

    # 1. Remote manifest URL.
    manifest_url = (config.get("firmware_manifest_url") or "").strip()
    if manifest_url:
        try:
            resp = requests.get(manifest_url, timeout=timeout)
            resp.raise_for_status()
            data = resp.json()
            version = str(data["version"])
            bin_url = str(data["bin_url"])
            sha256 = str(data["sha256"])
            log.info("Firmware block from remote manifest: version=%s", version)
            return {"version": version, "bin_url": bin_url, "sha256": sha256}
        except Exception as exc:
            log.warning("Firmware manifest fetch failed (non-fatal, omitting firmware): %s", exc)
            return None

    # 2. On-disk fallback (written by build-firmware CI workflow).
    local_fw = here / "public_assets" / "firmware.json"
    if local_fw.exists():
        try:
            with open(local_fw, encoding="utf-8") as f:
                data = json.load(f)
            version = str(data["version"])
            bin_url = str(data["bin_url"])
            sha256 = str(data["sha256"])
            log.info("Firmware block from on-disk public_assets/firmware.json: version=%s", version)
            return {"version": version, "bin_url": bin_url, "sha256": sha256}
        except Exception as exc:
            log.warning("On-disk firmware.json unreadable (non-fatal, omitting firmware): %s", exc)
            return None

    # 3. Neither source available.
    return None


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description="Update public/departures.json (v3) from RT feed.")
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

    # v3 check: warn if version mismatch (we still try to proceed).
    sched_version = schedule.get("version", 2)
    if sched_version != 3:
        log.warning(
            "stop_schedule.json has version=%s, expected 3 — run build_schedule.py to rebuild.",
            sched_version,
        )

    tz_name = config.get("timezone", "Australia/Brisbane")
    tz_obj = _get_tz(tz_name)

    now_utc = datetime.now(timezone.utc)
    now_local = _now_local(tz_obj)
    now_epoch = int(now_utc.timestamp())
    utc_offset = _utc_offset_seconds(tz_obj, now_utc.replace(tzinfo=None))

    log.info("Now UTC: %s  Local: %s  Offset: %+ds", now_utc.isoformat(), now_local.isoformat(), utc_offset)

    # ---- Fetch RT TripUpdates (non-fatal on error) --------------------------
    rt_url = config.get("rt_trip_updates_url", "")
    timeout = int(config.get("feed_timeout_seconds", 15))
    rt_map: dict = {}
    if rt_url:
        log.info("Fetching RT TripUpdates from %s …", rt_url)
        rt_map = _fetch_rt(rt_url, timeout)
    else:
        log.warning("No rt_trip_updates_url configured; using scheduled-only data.")

    # ---- Fetch RT ServiceAlerts (best-effort; always yields a list) ---------
    alerts_url = config.get("rt_service_alerts_url", "")
    stops_config = config.get("stops", [])
    alerts: list[dict] = _fetch_alerts(alerts_url, timeout, stops_config)

    # ---- Build departures for each configured stop --------------------------
    max_rows = int(config.get("max_rows", 6))
    stops_schedule = schedule.get("stops", {})  # keyed by stable key

    stops_out: list[dict] = []
    for stop_cfg in stops_config:
        key = stop_cfg["key"]
        label = stop_cfg.get("label", key)
        stop_entry = stops_schedule.get(key, {})
        if not stop_entry:
            log.warning("Stop %r not found in stop_schedule.json — emitting empty departures.", key)
            # Emit an empty stop so the contract always has ALL configured stops.
            stop_entry = {"label": label, "routes": [], "trips": []}

        deps = build_stop_departures(
            stop_entry=stop_entry,
            rt_map=rt_map,
            now_local=now_local,
            now_epoch=now_epoch,
            tz_obj=tz_obj,
            max_rows=max_rows,
            schedule=schedule,
        )
        log.info("Stop %r (%s): %d departures", key, label, len(deps))
        stops_out.append({
            "stop_id": key,
            "stop_label": label,
            "departures": deps,
        })

    # ---- Firmware block (optional) -----------------------------------------
    firmware_block = _load_firmware_block(config, here)

    # ---- Assemble v3 output -------------------------------------------------
    output: dict = {
        "version": 3,
        "generated_at": now_epoch,
        "utc_offset_seconds": utc_offset,
        "alerts": alerts,
        "stops": stops_out,
    }
    if firmware_block is not None:
        output["firmware"] = firmware_block

    out_path = Path(args.out) if args.out else here / "public" / "departures.json"
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(output, f, separators=(",", ":"), ensure_ascii=True)

    sz = out_path.stat().st_size
    total_deps = sum(len(s["departures"]) for s in stops_out)
    log.info(
        "Written %s (%d bytes, %d stops, %d total departures, %d alerts, firmware=%s)",
        out_path, sz, len(stops_out), total_deps, len(alerts),
        "yes" if firmware_block else "omitted",
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
