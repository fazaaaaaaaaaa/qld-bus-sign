/**
 * main.ts — QLD Bus Departure Sign, Deno Deploy backend
 *
 * Architecture:
 *   - stop_schedule.json (heavy static data) is fetched from STOP_SCHEDULE_URL and cached
 *     in Deno KV. It is rebuilt weekly by the companion GitHub Actions workflow and served
 *     from the repo's raw URL. This service never touches the 28 MB GTFS zip.
 *   - Every 1 minute (Deno.cron) the GTFS-RT TripUpdates protobuf is fetched, merged with
 *     the cached schedule, and the resulting departures are stored in Deno KV.
 *   - The HTTP handler reads from Deno KV, so the cron isolate and handler isolate share
 *     state without needing in-process globals (required on Deno Deploy).
 *
 * Data Contract v2 output (GET /departures.json):
 *   { stop_label, generated_at, utc_offset_seconds, departures: [{route,dest,time,live,cancelled}] }
 */

import { FeedMessage } from "npm:gtfs-realtime-bindings@1.2.1";

// ---------------------------------------------------------------------------
// CONFIG — edit these values directly, or override via Deno.env
// ---------------------------------------------------------------------------

const CONFIG = {
  /** URL to the precomputed stop_schedule.json from the GitHub repo */
  STOP_SCHEDULE_URL: Deno.env.get("STOP_SCHEDULE_URL") ??
    "https://raw.githubusercontent.com/fazaaaaaaaaaa/qld-bus-sign/main/stop_schedule.json",

  /** GTFS-RT TripUpdates protobuf endpoint (public, no key required) */
  RT_URL: Deno.env.get("RT_URL") ??
    "https://gtfsrt.api.translink.com.au/api/realtime/SEQ/TripUpdates",

  /** GTFS stop IDs to monitor */
  STOP_IDS: (Deno.env.get("STOP_IDS") ?? "24,000024").split(",").map((s) =>
    s.trim()
  ).filter(Boolean),

  /** Only show these route_short_names (empty = show all) */
  ROUTE_FILTER: (Deno.env.get("ROUTE_FILTER") ?? "320,322").split(",").map(
    (s) => s.trim(),
  ).filter(Boolean),

  /** Label shown in the sign header */
  STOP_LABEL: Deno.env.get("STOP_LABEL") ??
    "Adelaide St, Stop 24 near Edward St",

  /** IANA timezone name for the stop */
  TIMEZONE: Deno.env.get("TIMEZONE") ?? "Australia/Brisbane",

  /** Maximum departure rows returned */
  MAX_ROWS: parseInt(Deno.env.get("MAX_ROWS") ?? "7", 10),

  /** How many hours before re-fetching stop_schedule.json */
  SCHEDULE_TTL_HOURS: parseFloat(Deno.env.get("SCHEDULE_TTL_HOURS") ?? "12"),

  /** Network timeout in ms for feed fetches */
  FEED_TIMEOUT_MS: parseInt(Deno.env.get("FEED_TIMEOUT_MS") ?? "15000", 10),
};

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

interface CalendarEntry {
  monday: 0 | 1;
  tuesday: 0 | 1;
  wednesday: 0 | 1;
  thursday: 0 | 1;
  friday: 0 | 1;
  saturday: 0 | 1;
  sunday: 0 | 1;
  start_date: string; // "YYYYMMDD"
  end_date: string; // "YYYYMMDD"
}

interface CalendarDateEntry {
  service_id: string;
  date: string; // "YYYYMMDD"
  exception_type: 1 | 2; // 1=added, 2=removed
}

interface TripEntry {
  route_id: string;
  service_id: string;
  headsign: string;
}

interface StopTimeEntry {
  trip_id: string;
  stop_id: string;
  departure_time: string; // "HH:MM:SS" — HH may exceed 23
  stop_sequence: number;
}

interface StopSchedule {
  routes: Record<string, string>; // route_id → route_short_name
  calendar: Record<string, CalendarEntry>; // service_id → CalendarEntry
  calendar_dates: CalendarDateEntry[];
  trips: Record<string, TripEntry>; // trip_id → TripEntry
  stop_times: StopTimeEntry[];
  _meta?: Record<string, unknown>;
}

interface RtStopInfo {
  departure_epoch: number | null;
  arrival_epoch: number | null;
  schedule_relationship: number;
}

interface RtTripInfo {
  cancelled: boolean;
  stops: Record<string, RtStopInfo>; // stop_id → RtStopInfo
}

type RtMap = Record<string, RtTripInfo>; // trip_id → RtTripInfo

interface Departure {
  route: string;
  dest: string;
  time: number; // absolute UTC epoch seconds
  live: boolean;
  cancelled: boolean;
}

interface DeparturesPayload {
  stop_label: string;
  generated_at: number; // epoch seconds
  utc_offset_seconds: number;
  departures: Departure[];
}

interface ScheduleCache {
  schedule: StopSchedule;
  fetched_at: number; // epoch seconds
}

// ---------------------------------------------------------------------------
// KV keys
// ---------------------------------------------------------------------------

const KV_KEY_DEPARTURES = ["departures"] as const;
const KV_KEY_SCHEDULE = ["schedule_cache"] as const;

// ---------------------------------------------------------------------------
// Timezone helpers
// ---------------------------------------------------------------------------

const DAY_NAMES = [
  "sunday",
  "monday",
  "tuesday",
  "wednesday",
  "thursday",
  "friday",
  "saturday",
] as const;

type DayName = (typeof DAY_NAMES)[number];

/** Get UTC offset in seconds for the configured timezone at the given epoch ms. */
function getUtcOffsetSeconds(epochMs: number): number {
  const tz = CONFIG.TIMEZONE;
  // Use Intl.DateTimeFormat to find the local time components and compare to UTC.
  // This handles DST correctly (Brisbane has none, but the code is robust).
  const fmt = new Intl.DateTimeFormat("en-AU", {
    timeZone: tz,
    year: "numeric",
    month: "2-digit",
    day: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
    hour12: false,
  });
  const parts = Object.fromEntries(
    fmt.formatToParts(epochMs).map((p) => [p.type, p.value]),
  );
  // Reconstruct a UTC timestamp from local parts and subtract to get offset.
  const localEpochMs = Date.UTC(
    parseInt(parts.year),
    parseInt(parts.month) - 1,
    parseInt(parts.day),
    parseInt(parts.hour) % 24, // hour can be 24 at midnight
    parseInt(parts.minute),
    parseInt(parts.second),
  );
  return Math.round((localEpochMs - epochMs) / 1000);
}

/**
 * Return { year, month, day, hour, minute, second } in local time for the
 * given UTC epoch milliseconds, using the configured TIMEZONE.
 */
function localDateParts(epochMs: number): {
  year: number;
  month: number;
  day: number;
  hour: number;
  minute: number;
  second: number;
  weekday: DayName;
} {
  const tz = CONFIG.TIMEZONE;
  const fmt = new Intl.DateTimeFormat("en-AU", {
    timeZone: tz,
    year: "numeric",
    month: "2-digit",
    day: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
    hour12: false,
    weekday: "long",
  });
  const parts = Object.fromEntries(
    fmt.formatToParts(epochMs).map((p) => [p.type, p.value]),
  );
  const hour = parseInt(parts.hour) % 24; // Intl can emit "24" for midnight
  return {
    year: parseInt(parts.year),
    month: parseInt(parts.month),
    day: parseInt(parts.day),
    hour,
    minute: parseInt(parts.minute),
    second: parseInt(parts.second),
    weekday: parts.weekday.toLowerCase() as DayName,
  };
}

/** Format a date as "YYYYMMDD" */
function dateToStr(year: number, month: number, day: number): string {
  return `${year}${String(month).padStart(2, "0")}${String(day).padStart(2, "0")}`;
}

/**
 * Parse a GTFS departure_time "HH:MM:SS" (HH may exceed 23) on a given local
 * calendar date (as year/month/day), and return UTC epoch seconds.
 *
 * The local date + HH:MM:SS is converted to UTC by computing the local wall-
 * clock time, resolving its UTC equivalent via offset, and returning epoch.
 *
 * Brisbane has no DST so offset is constant, but the approach is robust.
 */
function gtfsTimeToEpoch(
  timeStr: string,
  baseYear: number,
  baseMonth: number,
  baseDay: number,
  utcOffsetSeconds: number,
): number {
  const parts = timeStr.split(":");
  if (parts.length !== 3) throw new Error(`Bad GTFS time: ${timeStr}`);
  const h = parseInt(parts[0], 10);
  const m = parseInt(parts[1], 10);
  const s = parseInt(parts[2], 10);

  // Represent the base date as a UTC midnight (we'll offset it).
  // Date.UTC gives us the "midnight UTC" for the given calendar date.
  const baseMidnightUtc = Date.UTC(baseYear, baseMonth - 1, baseDay);

  // Local midnight = UTC midnight − utcOffsetSeconds (local is ahead of UTC for +10)
  // Wall-clock moment in UTC = local midnight (UTC) + h*3600 + m*60 + s
  // But: local midnight in UTC = baseMidnightUtc − utcOffsetSeconds*1000
  const epochMs = baseMidnightUtc - utcOffsetSeconds * 1000 +
    (h * 3600 + m * 60 + s) * 1000;
  return Math.floor(epochMs / 1000);
}

// ---------------------------------------------------------------------------
// Calendar helpers
// ---------------------------------------------------------------------------

function activeServicesForDate(
  schedule: StopSchedule,
  year: number,
  month: number,
  day: number,
): Set<string> {
  const dateStr = dateToStr(year, month, day);
  // Compute weekday from a Date object (JS months are 0-indexed).
  const jsDate = new Date(Date.UTC(year, month - 1, day));
  const weekday = DAY_NAMES[jsDate.getUTCDay()];

  const added = new Set<string>();
  const removed = new Set<string>();

  for (const cd of schedule.calendar_dates) {
    if (cd.date === dateStr) {
      if (cd.exception_type === 1) added.add(cd.service_id);
      else if (cd.exception_type === 2) removed.add(cd.service_id);
    }
  }

  const regular = new Set<string>();
  for (const [svcId, cal] of Object.entries(schedule.calendar)) {
    const runs = Boolean(cal[weekday]);
    const inRange = cal.start_date <= dateStr && dateStr <= cal.end_date;
    if (runs && inRange) regular.add(svcId);
  }

  const result = new Set([...regular, ...added]);
  for (const id of removed) result.delete(id);
  return result;
}

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

/** Strip combining diacritics (ASCII fold). Mirrors Python unicodedata.normalize("NFKD"). */
function asciiFold(text: string): string {
  // Remove Unicode combining characters (U+0300–U+036F Combining Diacritical Marks).
  return text.normalize("NFKD").replace(/\p{M}/gu, "");
}

/** Truncate with ellipsis if over max_chars. */
function truncate(text: string, maxChars: number): string {
  const t = text.trim();
  if (t.length <= maxChars) return t;
  return t.slice(0, maxChars - 1).trimEnd() + "…";
}

// ---------------------------------------------------------------------------
// RT fetch + parse
// ---------------------------------------------------------------------------

const TRIP_SR_CANCELED = 3;
const STOP_SR_SKIPPED = 1;

async function fetchRt(url: string): Promise<RtMap> {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), CONFIG.FEED_TIMEOUT_MS);
  let buf: ArrayBuffer;
  try {
    const resp = await fetch(url, { signal: controller.signal });
    if (!resp.ok) {
      console.warn(`RT fetch HTTP ${resp.status} — using schedule-only`);
      return {};
    }
    buf = await resp.arrayBuffer();
  } catch (err) {
    console.warn(`RT fetch failed (non-fatal): ${err}`);
    return {};
  } finally {
    clearTimeout(timeout);
  }

  let feed: ReturnType<typeof FeedMessage.decode>;
  try {
    feed = FeedMessage.decode(new Uint8Array(buf));
  } catch (err) {
    console.warn(`RT parse failed (non-fatal): ${err}`);
    return {};
  }

  const result: RtMap = {};
  for (const entity of feed.entity) {
    const tu = entity.tripUpdate;
    if (!tu) continue;
    const tripId = tu.trip?.tripId;
    if (!tripId) continue;

    const isCancelled = tu.trip?.scheduleRelationship === TRIP_SR_CANCELED;
    const stops: Record<string, RtStopInfo> = {};

    for (const stu of tu.stopTimeUpdate ?? []) {
      const sid = stu.stopId;
      if (!sid) continue;
      const depTime = stu.departure?.time;
      const arrTime = stu.arrival?.time;
      // gtfs-realtime-bindings encodes int64 as Long objects; coerce to number.
      const depEpoch = depTime != null ? Number(depTime) || null : null;
      const arrEpoch = arrTime != null ? Number(arrTime) || null : null;
      stops[sid] = {
        departure_epoch: depEpoch,
        arrival_epoch: arrEpoch,
        schedule_relationship: stu.scheduleRelationship ?? 0,
      };
    }

    result[tripId] = { cancelled: isCancelled, stops };
  }

  console.info(`Parsed ${Object.keys(result).length} RT trip updates`);
  return result;
}

// ---------------------------------------------------------------------------
// Schedule fetch + cache
// ---------------------------------------------------------------------------

async function fetchSchedule(kv: Deno.Kv): Promise<StopSchedule> {
  const url = CONFIG.STOP_SCHEDULE_URL;
  console.info(`Fetching stop_schedule.json from ${url} …`);
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 60_000); // 60s — it's ~several MB
  try {
    const resp = await fetch(url, { signal: controller.signal });
    if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
    const schedule = await resp.json() as StopSchedule;
    const cache: ScheduleCache = {
      schedule,
      fetched_at: Math.floor(Date.now() / 1000),
    };
    await kv.set(KV_KEY_SCHEDULE, cache);
    console.info(`stop_schedule.json cached (${schedule.stop_times.length} stop_times)`);
    return schedule;
  } finally {
    clearTimeout(timeout);
  }
}

async function getSchedule(kv: Deno.Kv): Promise<StopSchedule | null> {
  const entry = await kv.get<ScheduleCache>(KV_KEY_SCHEDULE);
  if (!entry.value) return null;
  const ageHours = (Date.now() / 1000 - entry.value.fetched_at) / 3600;
  if (ageHours > CONFIG.SCHEDULE_TTL_HOURS) {
    console.info(`Schedule cache stale (${ageHours.toFixed(1)}h old), will refresh`);
    return null;
  }
  return entry.value.schedule;
}

async function ensureSchedule(kv: Deno.Kv): Promise<StopSchedule> {
  let schedule = await getSchedule(kv);
  if (!schedule) {
    schedule = await fetchSchedule(kv);
  }
  return schedule;
}

// ---------------------------------------------------------------------------
// Build departures — faithful port of update_departures.py::build_departures()
// ---------------------------------------------------------------------------

function buildDepartures(
  schedule: StopSchedule,
  rtMap: RtMap,
  nowEpochMs: number,
): Departure[] {
  const maxRows = CONFIG.MAX_ROWS;
  const routeFilter = new Set(CONFIG.ROUTE_FILTER);
  const stopIds = new Set(CONFIG.STOP_IDS.map(String));

  const nowEpoch = Math.floor(nowEpochMs / 1000);
  const cutoffEpoch = nowEpoch - 60; // mirror the Python now-60s lower bound

  const utcOffsetSeconds = getUtcOffsetSeconds(nowEpochMs);

  // Pre-compute local "today" date parts.
  const todayLocal = localDateParts(nowEpochMs);

  // Yesterday in local time = subtract 1 day from local date.
  const yesterdayJs = new Date(
    Date.UTC(todayLocal.year, todayLocal.month - 1, todayLocal.day - 1),
  );
  const yesterdayYear = yesterdayJs.getUTCFullYear();
  const yesterdayMonth = yesterdayJs.getUTCMonth() + 1;
  const yesterdayDay = yesterdayJs.getUTCDate();

  // Check both yesterday and today (for after-midnight services).
  const datesToCheck: [number, number, number][] = [
    [yesterdayYear, yesterdayMonth, yesterdayDay],
    [todayLocal.year, todayLocal.month, todayLocal.day],
  ];

  const routes = schedule.routes; // route_id → route_short_name
  const trips = schedule.trips; // trip_id → TripEntry

  // Index stop_times by trip_id for O(1) lookup per trip.
  const stopTimesByTrip = new Map<string, StopTimeEntry[]>();
  for (const st of schedule.stop_times) {
    let arr = stopTimesByTrip.get(st.trip_id);
    if (!arr) {
      arr = [];
      stopTimesByTrip.set(st.trip_id, arr);
    }
    arr.push(st);
  }

  const raw: Departure[] = [];

  for (const [baseYear, baseMonth, baseDay] of datesToCheck) {
    const activeServices = activeServicesForDate(
      schedule,
      baseYear,
      baseMonth,
      baseDay,
    );
    if (activeServices.size === 0) continue;

    for (const [tripId, trip] of Object.entries(trips)) {
      if (!activeServices.has(trip.service_id)) continue;

      const routeShort = routes[trip.route_id] ?? trip.route_id;

      if (routeFilter.size > 0 && !routeFilter.has(routeShort)) continue;

      const stList = stopTimesByTrip.get(tripId);
      if (!stList) continue;

      for (const st of stList) {
        if (!stopIds.has(st.stop_id)) continue;
        if (!st.departure_time) continue;

        let schedEpoch: number;
        try {
          schedEpoch = gtfsTimeToEpoch(
            st.departure_time,
            baseYear,
            baseMonth,
            baseDay,
            utcOffsetSeconds,
          );
        } catch {
          continue;
        }

        // Apply RT overlay.
        let cancelled = false;
        let live = false;
        let depEpoch = schedEpoch;

        const rtTrip = rtMap[tripId];
        if (rtTrip) {
          if (rtTrip.cancelled) {
            cancelled = true;
          } else {
            const rtStop = rtTrip.stops[st.stop_id];
            if (rtStop) {
              const rtDep = rtStop.departure_epoch ?? rtStop.arrival_epoch;
              if (rtDep) {
                depEpoch = Math.round(rtDep);
                live = true;
              }
              if (rtStop.schedule_relationship === STOP_SR_SKIPPED) {
                cancelled = true;
              }
            }
          }
        }

        // Apply cutoff filter.
        if (depEpoch < cutoffEpoch) continue;

        const dest = asciiFold(truncate(trip.headsign ?? "", 28));
        raw.push({
          route: truncate(routeShort, 6),
          dest,
          time: depEpoch,
          live,
          cancelled,
        });
      }
    }
  }

  // Sort ascending by departure time.
  raw.sort((a, b) => a.time - b.time);

  // Deduplicate: same (route, dest, time) — mirrors Python logic.
  // The same trip can appear from both check dates when after-midnight.
  const seen = new Set<string>();
  const deduped: Departure[] = [];
  for (const d of raw) {
    const key = `${d.route}|${d.dest}|${d.time}`;
    if (!seen.has(key)) {
      seen.add(key);
      deduped.push(d);
    }
  }

  return deduped.slice(0, maxRows);
}

// ---------------------------------------------------------------------------
// Cron refresh job
// ---------------------------------------------------------------------------

// Open KV once at module level — both the cron and the handler share this
// handle. On Deno Deploy, state must go through KV, not in-process variables.
const kv = await Deno.openKv();

async function refresh(): Promise<void> {
  try {
    const schedule = await ensureSchedule(kv);

    console.info("Fetching GTFS-RT …");
    const rtMap = await fetchRt(CONFIG.RT_URL);

    const nowMs = Date.now();
    const departures = buildDepartures(schedule, rtMap, nowMs);
    const utcOffsetSeconds = getUtcOffsetSeconds(nowMs);

    const payload: DeparturesPayload = {
      stop_label: CONFIG.STOP_LABEL,
      generated_at: Math.floor(nowMs / 1000),
      utc_offset_seconds: utcOffsetSeconds,
      departures,
    };

    await kv.set(KV_KEY_DEPARTURES, payload);
    console.info(
      `Departures updated: ${departures.length} rows, generated_at=${payload.generated_at}`,
    );
  } catch (err) {
    // Never throw out of cron — log and continue.
    console.error(`refresh() error (non-fatal): ${err}`);
  }
}

// Register the 1-minute cron.
// On Deno Deploy, --unstable-cron is implicit. Locally, pass --unstable-cron.
Deno.cron("refresh-departures", "* * * * *", refresh);

// ---------------------------------------------------------------------------
// HTTP handler
// ---------------------------------------------------------------------------

const CORS_HEADERS: Record<string, string> = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "GET, OPTIONS",
  "Access-Control-Allow-Headers": "*",
};

async function handler(req: Request): Promise<Response> {
  const url = new URL(req.url);
  const path = url.pathname;

  if (req.method === "OPTIONS") {
    return new Response(null, { status: 204, headers: CORS_HEADERS });
  }

  if (path === "/health") {
    return new Response("ok", {
      headers: { "Content-Type": "text/plain", ...CORS_HEADERS },
    });
  }

  if (path === "/departures.json" || path === "/") {
    // Attempt to read from KV (set by the cron).
    let entry = await kv.get<DeparturesPayload>(KV_KEY_DEPARTURES);

    if (!entry.value) {
      // Cold start: KV is empty (first deploy before first cron tick).
      // Compute on-demand so the first request isn't blank.
      console.info("KV empty — computing departures on demand (cold start)");
      await refresh();
      entry = await kv.get<DeparturesPayload>(KV_KEY_DEPARTURES);
    }

    if (!entry.value) {
      // Still empty — schedule probably failed to load; return a helpful stub.
      const stub: DeparturesPayload = {
        stop_label: CONFIG.STOP_LABEL,
        generated_at: Math.floor(Date.now() / 1000),
        utc_offset_seconds: 36000,
        departures: [],
      };
      return new Response(JSON.stringify(stub), {
        status: 200,
        headers: {
          "Content-Type": "application/json",
          "Cache-Control": "no-store",
          ...CORS_HEADERS,
        },
      });
    }

    return new Response(JSON.stringify(entry.value), {
      status: 200,
      headers: {
        "Content-Type": "application/json",
        "Cache-Control": "no-store",
        ...CORS_HEADERS,
      },
    });
  }

  return new Response("Not Found", { status: 404, headers: CORS_HEADERS });
}

console.info("QLD Bus Sign — Deno Deploy backend starting");
console.info(`STOP_IDS: ${CONFIG.STOP_IDS.join(", ")}`);
console.info(`ROUTE_FILTER: ${CONFIG.ROUTE_FILTER.join(", ") || "(all)"}`);
console.info(`STOP_LABEL: ${CONFIG.STOP_LABEL}`);

Deno.serve(handler);
