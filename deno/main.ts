/**
 * main.ts — QLD Bus Departure Sign, Deno Deploy backend
 *
 * Architecture:
 *   - stop_schedule.json (heavy static data, v3 shape) is fetched from
 *     STOP_SCHEDULE_URL and cached in Deno KV.  It is rebuilt weekly by the
 *     companion GitHub Actions workflow and served from the repo's raw URL.
 *     This service never touches the 28 MB GTFS zip.
 *   - Every 1 minute (Deno.cron) the GTFS-RT TripUpdates + ServiceAlerts
 *     protobufs are fetched, merged with the cached schedule, and the resulting
 *     departures.json v3 payload is stored in Deno KV.
 *   - The HTTP handler reads from Deno KV, so the cron isolate and handler
 *     isolate share state without needing in-process globals (required on
 *     Deno Deploy).
 *
 * Data Contract v3 output (GET /departures.json):
 *   {
 *     version: 3, generated_at, utc_offset_seconds,
 *     alerts: [{id, header, severity, effect, routes}],
 *     stops: [{stop_id, stop_label, departures:[{route,dest,time,live,cancelled}]}],
 *     firmware?: {version, bin_url, sha256}   // omitted when not available
 *   }
 *
 * GET / → live HTML health page (auto-refreshes every 30 s)
 * GET /health → JSON status object
 * GET /departures.json → Data Contract v3 JSON
 */

// gtfs-realtime-bindings is a CommonJS package; import the namespace and handle
// both default-export and namespace shapes for robust Deno/npm interop.
import * as GtfsRtNS from "npm:gtfs-realtime-bindings@1.1.1";
// deno-lint-ignore no-explicit-any
const GtfsRt: any = (GtfsRtNS as any).default ?? GtfsRtNS;
const FeedMessage = GtfsRt.transit_realtime.FeedMessage;

// ---------------------------------------------------------------------------
// CONFIG — mirrors CONTRACT §C (Deno variant)
// Edit these values directly or override via Deno.env.
// ---------------------------------------------------------------------------

interface StopConfig {
  key: string;       // stable stop key (= stop_id in departures.json output)
  label: string;     // human-readable label
  routes: string[];  // route_short_names to show for this stop
  /** Raw GTFS stop_id(s) that appear in RT TripUpdates stopTimeUpdate.stopId.
   *  The key is always tried first; add extras when GTFS uses a different id
   *  (e.g. "000024" for key "24", or "11180" for key "011180"). */
  rtStopIds?: string[];
}

const CONFIG = {
  /** Two stops: home (inbound) and city (outbound). */
  STOPS: (() => {
    const raw = Deno.env.get("STOPS_JSON");
    if (raw) {
      try { return JSON.parse(raw) as StopConfig[]; } catch { /* fall through */ }
    }
    return [
      // rtStopIds: raw GTFS stop_ids that appear in RT TripUpdates.
      // "011180" is usually correct; "11180" is the zero-stripped variant.
      { key: "011180", label: "Bonney Ave at Victoria Parade, Stop 24, Clayfield", routes: ["320"],         rtStopIds: ["011180", "11180"] },
      // "24" in config.yaml but GTFS static may use "000024".
      { key: "24",     label: "Adelaide St, Stop 24 near Edward St",               routes: ["320", "322"], rtStopIds: ["24", "000024"] },
    ] satisfies StopConfig[];
  })(),

  /** Max departure rows per stop in the response. */
  MAX_ROWS: parseInt(Deno.env.get("MAX_ROWS") ?? "6", 10),

  /** URL to the precomputed v3 stop_schedule.json from the GitHub repo. */
  STOP_SCHEDULE_URL: Deno.env.get("STOP_SCHEDULE_URL") ??
    "https://raw.githubusercontent.com/fazaaaaaaaaaa/qld-bus-sign/main/stop_schedule.json",

  /** GTFS-RT TripUpdates protobuf endpoint (public, no key required). */
  RT_TRIP_UPDATES_URL: Deno.env.get("RT_TRIP_UPDATES_URL") ??
    "https://gtfsrt.api.translink.com.au/api/realtime/SEQ/TripUpdates",

  /** GTFS-RT ServiceAlerts protobuf endpoint. */
  RT_SERVICE_ALERTS_URL: Deno.env.get("RT_SERVICE_ALERTS_URL") ??
    "https://gtfsrt.api.translink.com.au/api/realtime/SEQ/ServiceAlerts",

  /** OTA firmware manifest URL (JSON {version,bin_url,sha256}). Empty = omit firmware block. */
  FIRMWARE_MANIFEST_URL: Deno.env.get("FIRMWARE_MANIFEST_URL") ??
    "https://fazaaaaaaaaaa.github.io/qld-bus-sign/firmware.json",

  /** IANA timezone name for the stop (Brisbane has no DST). */
  TIMEZONE: Deno.env.get("TIMEZONE") ?? "Australia/Brisbane",

  /** How many hours before re-fetching stop_schedule.json. */
  SCHEDULE_TTL_HOURS: parseFloat(Deno.env.get("SCHEDULE_TTL_HOURS") ?? "12"),

  /** Network timeout in ms for all external fetches. */
  FEED_TIMEOUT_MS: parseInt(Deno.env.get("FEED_TIMEOUT_MS") ?? "15000", 10),
};

// ---------------------------------------------------------------------------
// Types — v3 stop_schedule.json (§B of the contract)
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
  end_date: string;   // "YYYYMMDD"
}

interface CalendarDateEntry {
  date: string;          // "YYYYMMDD"
  exception_type: 1 | 2; // 1=added, 2=removed
}

/** One trip entry inside a stop's trips array (§B). */
interface TripEntry {
  trip_id: string;
  route: string;   // route_short_name
  dest: string;    // headsign
  service_id: string;
  dep: string;     // "HH:MM:SS" in local time; HH may exceed 23 for after-midnight
}

/** One stop's data inside stop_schedule.json v3 (§B). */
interface StopData {
  label: string;
  routes: string[];   // route_short_names this stop was built for
  trips: TripEntry[];
}

/**
 * v3 stop_schedule.json produced by build_schedule.py (§B).
 *
 * Fields consumed by this service:
 *   .version          — must be 3
 *   .stops            — Record<key, StopData>  (keyed by config key)
 *   .calendar         — Record<service_id, CalendarEntry>
 *   .calendar_dates   — Record<service_id, CalendarDateEntry[]>
 */
interface StopScheduleV3 {
  version: number;
  built_at: number;
  timezone: string;
  stops: Record<string, StopData>;
  calendar: Record<string, CalendarEntry>;
  calendar_dates: Record<string, CalendarDateEntry[]>;
}

// ---------------------------------------------------------------------------
// Types — runtime / KV
// ---------------------------------------------------------------------------

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
  time: number;      // absolute UTC epoch seconds
  live: boolean;
  cancelled: boolean;
}

interface Alert {
  id: string;
  header: string;    // truncated ≤96, ascii-folded
  severity: string;  // INFO | WARNING | SEVERE | UNKNOWN
  effect: string;    // GTFS effect enum name
  routes: string[];  // configured route_short_names this alert touches
}

interface FirmwareBlock {
  version: string;
  bin_url: string;
  sha256: string;
}

/** v3 departures.json — what is stored in KV and served to the device (§A). */
interface DeparturesPayloadV3 {
  version: 3;
  generated_at: number;
  utc_offset_seconds: number;
  alerts: Alert[];
  stops: {
    stop_id: string;
    stop_label: string;
    departures: Departure[];
  }[];
  firmware?: FirmwareBlock;
}

interface ScheduleCache {
  schedule: StopScheduleV3;
  fetched_at: number; // epoch seconds
}

/** RT status written to KV after each cron so the health page can display it. */
interface RtStatus {
  trip_updates_ok: boolean;
  alerts_ok: boolean;
  last_error: string;   // "" when none
  last_run_at: number;  // epoch seconds
}

// ---------------------------------------------------------------------------
// KV keys
// ---------------------------------------------------------------------------

const KV_KEY_DEPARTURES = ["departures_v3"] as const;
const KV_KEY_SCHEDULE   = ["schedule_cache_v3"] as const;
const KV_KEY_RT_STATUS  = ["rt_status"] as const;

// ---------------------------------------------------------------------------
// Timezone helpers  (unchanged from v2)
// ---------------------------------------------------------------------------

const DAY_NAMES = [
  "sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday",
] as const;
type DayName = (typeof DAY_NAMES)[number];

/** Get UTC offset in seconds for the configured timezone at the given epoch ms. */
function getUtcOffsetSeconds(epochMs: number): number {
  const tz = CONFIG.TIMEZONE;
  const fmt = new Intl.DateTimeFormat("en-AU", {
    timeZone: tz,
    year: "numeric", month: "2-digit", day: "2-digit",
    hour: "2-digit", minute: "2-digit", second: "2-digit",
    hour12: false,
  });
  const parts = Object.fromEntries(fmt.formatToParts(epochMs).map((p) => [p.type, p.value]));
  const rawHour = parseInt(parts.hour);
  const hourForUtc = rawHour % 24;
  const midnightCarry = rawHour === 24 ? 86400000 : 0;
  const localEpochMs = Date.UTC(
    parseInt(parts.year), parseInt(parts.month) - 1, parseInt(parts.day),
    hourForUtc, parseInt(parts.minute), parseInt(parts.second),
  ) + midnightCarry;
  return Math.round((localEpochMs - epochMs) / 1000);
}

function localDateParts(epochMs: number): {
  year: number; month: number; day: number;
  hour: number; minute: number; second: number;
  weekday: DayName;
} {
  const tz = CONFIG.TIMEZONE;
  const fmt = new Intl.DateTimeFormat("en-AU", {
    timeZone: tz,
    year: "numeric", month: "2-digit", day: "2-digit",
    hour: "2-digit", minute: "2-digit", second: "2-digit",
    hour12: false, weekday: "long",
  });
  const parts = Object.fromEntries(fmt.formatToParts(epochMs).map((p) => [p.type, p.value]));
  return {
    year: parseInt(parts.year), month: parseInt(parts.month), day: parseInt(parts.day),
    hour: parseInt(parts.hour) % 24, minute: parseInt(parts.minute), second: parseInt(parts.second),
    weekday: parts.weekday.toLowerCase() as DayName,
  };
}

function dateToStr(year: number, month: number, day: number): string {
  return `${year}${String(month).padStart(2, "0")}${String(day).padStart(2, "0")}`;
}

/**
 * Parse a GTFS `dep` string "HH:MM:SS" (HH may exceed 23) on a given local
 * calendar date, and return UTC epoch seconds.
 */
function gtfsTimeToEpoch(
  timeStr: string, baseYear: number, baseMonth: number, baseDay: number,
  utcOffsetSeconds: number,
): number {
  const parts = timeStr.split(":");
  if (parts.length !== 3) throw new Error(`Bad GTFS time: ${timeStr}`);
  const h = parseInt(parts[0], 10);
  const m = parseInt(parts[1], 10);
  const s = parseInt(parts[2], 10);
  const baseMidnightUtc = Date.UTC(baseYear, baseMonth - 1, baseDay);
  const epochMs = baseMidnightUtc - utcOffsetSeconds * 1000 + (h * 3600 + m * 60 + s) * 1000;
  return Math.floor(epochMs / 1000);
}

// ---------------------------------------------------------------------------
// Calendar helpers  (adapted for v3 calendar_dates shape)
// ---------------------------------------------------------------------------

/**
 * Compute active service_ids for a local calendar date.
 *
 * v3 calendar_dates is Record<service_id, [{date, exception_type}]>  (§B).
 */
function activeServicesForDate(
  schedule: StopScheduleV3,
  year: number, month: number, day: number,
): Set<string> {
  const dateStr = dateToStr(year, month, day);
  const jsDate = new Date(Date.UTC(year, month - 1, day));
  const weekday = DAY_NAMES[jsDate.getUTCDay()];

  const added   = new Set<string>();
  const removed = new Set<string>();

  for (const [svcId, exceptions] of Object.entries(schedule.calendar_dates)) {
    for (const ex of exceptions) {
      if (ex.date === dateStr) {
        if (ex.exception_type === 1) added.add(svcId);
        else if (ex.exception_type === 2) removed.add(svcId);
      }
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
// Text helpers  (unchanged)
// ---------------------------------------------------------------------------

function asciiFold(text: string): string {
  return text.normalize("NFKD").replace(/\p{M}/gu, "");
}

function truncate(text: string, maxChars: number): string {
  const t = text.trim();
  if (t.length <= maxChars) return t;
  return t.slice(0, maxChars - 1).trimEnd() + "…";
}

// ---------------------------------------------------------------------------
// RT constants
// ---------------------------------------------------------------------------

const TRIP_SR_CANCELED = 3;
const STOP_SR_SKIPPED  = 1;

// Mapping from GTFS effect enum integer → name string.
// Source: GTFS-realtime spec §Effect.
const EFFECT_NAMES: Record<number, string> = {
  0: "UNKNOWN_EFFECT",
  1: "NO_SERVICE",
  2: "REDUCED_SERVICE",
  3: "SIGNIFICANT_DELAY",
  4: "DETOUR",
  5: "ADDITIONAL_SERVICE",
  6: "MODIFIED_SERVICE",
  7: "OTHER_EFFECT",
  8: "STOP_MOVED",
  9: "NO_EFFECT",
  10: "ACCESSIBILITY_ISSUE",
};

// Mapping from GTFS severity_level integer → name string.
const SEVERITY_NAMES: Record<number, string> = {
  1: "INFO",
  2: "WARNING",
  3: "SEVERE",
};

// ---------------------------------------------------------------------------
// RT fetch helpers
// ---------------------------------------------------------------------------

/** Fetch and decode a GTFS-RT FeedMessage.  Returns null on any failure. */
async function fetchFeedMessage(url: string): Promise<ReturnType<typeof FeedMessage.decode> | null> {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), CONFIG.FEED_TIMEOUT_MS);
  let buf: ArrayBuffer;
  try {
    const resp = await fetch(url, { signal: controller.signal });
    if (!resp.ok) {
      console.warn(`RT fetch HTTP ${resp.status} from ${url}`);
      return null;
    }
    buf = await resp.arrayBuffer();
  } catch (err) {
    console.warn(`RT fetch failed (${url}): ${err}`);
    return null;
  } finally {
    clearTimeout(timeout);
  }

  try {
    return FeedMessage.decode(new Uint8Array(buf));
  } catch (err) {
    console.warn(`RT protobuf parse failed (${url}): ${err}`);
    return null;
  }
}

/** Fetch TripUpdates feed and build an RtMap. */
async function fetchTripUpdates(url: string): Promise<{ rtMap: RtMap; ok: boolean }> {
  const feed = await fetchFeedMessage(url);
  if (!feed) return { rtMap: {}, ok: false };

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
      const depRaw = stu.departure?.time != null ? Number(stu.departure.time) : 0;
      const arrRaw = stu.arrival?.time   != null ? Number(stu.arrival.time)   : 0;
      stops[sid] = {
        departure_epoch: depRaw > 0 ? depRaw : null,
        arrival_epoch:   arrRaw > 0 ? arrRaw : null,
        schedule_relationship: stu.scheduleRelationship ?? 0,
      };
    }
    result[tripId] = { cancelled: isCancelled, stops };
  }

  console.info(`Parsed ${Object.keys(result).length} RT trip updates`);
  return { rtMap: result, ok: true };
}

/**
 * Fetch ServiceAlerts feed and return filtered Alert objects.
 *
 * An alert is kept only when at least one informed_entity has:
 *   - route_id whose short name is in allRoutes, OR
 *   - stop_id that is one of our configured stop keys.
 *
 * On any failure returns {alerts: [], ok: false}.
 */
async function fetchAlerts(url: string): Promise<{ alerts: Alert[]; ok: boolean }> {
  // Build a set of all configured route_short_names and stop ids (key + rtStopIds variants).
  const allRoutes = new Set(CONFIG.STOPS.flatMap((s) => s.routes));
  // Include both the stable key and any raw GTFS id variants for stop matching.
  const allStopKeys = new Set(CONFIG.STOPS.flatMap((s) => [s.key, ...(s.rtStopIds ?? [])]));
  // Map: any stop id → owning StopConfig (for route lookup on stop-matched alerts).
  const stopIdToConfig = new Map<string, StopConfig>();
  for (const stop of CONFIG.STOPS) {
    stopIdToConfig.set(stop.key, stop);
    for (const rid of stop.rtStopIds ?? []) stopIdToConfig.set(rid, stop);
  }

  const feed = await fetchFeedMessage(url);
  if (!feed) return { alerts: [], ok: false };

  const alerts: Alert[] = [];

  for (const entity of feed.entity) {
    const a = entity.alert;
    if (!a) continue;

    const id = entity.id ?? "";

    // Collect header text (first English translation, or first available).
    let headerRaw = "";
    const translations: { text?: string; language?: string }[] =
      a.headerText?.translation ?? [];
    const enTrans = translations.find((t) => !t.language || t.language === "en");
    headerRaw = (enTrans ?? translations[0])?.text ?? "";
    if (!headerRaw) continue; // skip alerts with no header

    // Severity: GTFS severity_level integer → name.
    const severityNum = Number(a.severityLevel ?? 0);
    const severity = SEVERITY_NAMES[severityNum] ?? "UNKNOWN";

    // Effect: GTFS effect integer → name.
    const effectNum = Number(a.effect ?? 0);
    const effect = EFFECT_NAMES[effectNum] ?? "UNKNOWN_EFFECT";

    // Determine which configured routes this alert touches.
    const touchedRoutes = new Set<string>();
    let routeOrStopMatch = false;

    for (const entity_ of a.informedEntity ?? []) {
      // Route match: route_id field contains the GTFS route_id.
      // Translink GTFS route_short_name often equals or is embedded in route_id.
      // We match route_id against our route_short_names directly (common for SEQ).
      const routeId = entity_.routeId ?? "";
      if (routeId && allRoutes.has(routeId)) {
        touchedRoutes.add(routeId);
        routeOrStopMatch = true;
      }
      // Stop match (checks both stable key and rtStopId variants).
      const stopId = entity_.stopId ?? "";
      if (stopId && allStopKeys.has(stopId)) {
        routeOrStopMatch = true;
        // Include all routes for that stop.
        const stopCfg = stopIdToConfig.get(stopId);
        if (stopCfg) {
          for (const r of stopCfg.routes) touchedRoutes.add(r);
        }
      }
    }

    if (!routeOrStopMatch) continue;

    alerts.push({
      id,
      header: asciiFold(truncate(headerRaw, 96)),
      severity,
      effect,
      routes: [...touchedRoutes].sort(),
    });
  }

  console.info(`Parsed ${alerts.length} relevant service alerts`);
  return { alerts, ok: true };
}

// ---------------------------------------------------------------------------
// Firmware manifest fetch (best-effort)
// ---------------------------------------------------------------------------

async function fetchFirmware(): Promise<FirmwareBlock | undefined> {
  const url = CONFIG.FIRMWARE_MANIFEST_URL;
  if (!url) return undefined;

  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), CONFIG.FEED_TIMEOUT_MS);
  try {
    const resp = await fetch(url, { signal: controller.signal });
    if (!resp.ok) {
      console.warn(`Firmware manifest HTTP ${resp.status} — omitting firmware block`);
      return undefined;
    }
    const json = await resp.json() as Partial<FirmwareBlock>;
    if (!json.version || !json.bin_url || !json.sha256) {
      console.warn("Firmware manifest missing required fields — omitting firmware block");
      return undefined;
    }
    return { version: String(json.version), bin_url: String(json.bin_url), sha256: String(json.sha256) };
  } catch (err) {
    console.warn(`Firmware manifest fetch failed (non-fatal): ${err}`);
    return undefined;
  } finally {
    clearTimeout(timeout);
  }
}

// ---------------------------------------------------------------------------
// Schedule fetch + cache
// ---------------------------------------------------------------------------

async function fetchSchedule(kv: Deno.Kv): Promise<StopScheduleV3> {
  const url = CONFIG.STOP_SCHEDULE_URL;
  console.info(`Fetching stop_schedule.json v3 from ${url} …`);
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 60_000); // 60s for multi-MB file
  try {
    const resp = await fetch(url, { signal: controller.signal });
    if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
    const schedule = await resp.json() as StopScheduleV3;
    if (schedule.version !== 3) {
      throw new Error(`stop_schedule.json version=${schedule.version} — expected 3`);
    }
    const totalTrips = Object.values(schedule.stops).reduce((n, s) => n + s.trips.length, 0);
    const cache: ScheduleCache = { schedule, fetched_at: Math.floor(Date.now() / 1000) };
    await kv.set(KV_KEY_SCHEDULE, cache);
    console.info(`stop_schedule.json v3 cached (${Object.keys(schedule.stops).length} stops, ${totalTrips} trips)`);
    return schedule;
  } finally {
    clearTimeout(timeout);
  }
}

async function getSchedule(kv: Deno.Kv): Promise<StopScheduleV3 | null> {
  const entry = await kv.get<ScheduleCache>(KV_KEY_SCHEDULE);
  if (!entry.value) return null;
  const ageHours = (Date.now() / 1000 - entry.value.fetched_at) / 3600;
  if (ageHours > CONFIG.SCHEDULE_TTL_HOURS) {
    console.info(`Schedule cache stale (${ageHours.toFixed(1)}h old), will refresh`);
    return null;
  }
  return entry.value.schedule;
}

async function ensureSchedule(kv: Deno.Kv): Promise<StopScheduleV3> {
  let schedule = await getSchedule(kv);
  if (!schedule) schedule = await fetchSchedule(kv);
  return schedule;
}

// ---------------------------------------------------------------------------
// Build departures for ONE stop — v3 shape (§B consumer)
// ---------------------------------------------------------------------------

/**
 * Compute active departures for a single configured stop.
 *
 * Reads from v3 stop_schedule.json:
 *   schedule.stops[stopKey].trips[]  — each trip has {trip_id, route, dest, service_id, dep}
 *   schedule.calendar                — service_id → weekday flags + date range
 *   schedule.calendar_dates          — service_id → [{date, exception_type}]
 *
 * Logic: same as v2 (active service_ids for yesterday+today, after-midnight
 * carry via >24:00 dep strings, now-60s cutoff, RT TripUpdates overlay,
 * dedup, sort, cap MAX_ROWS).
 */
function buildStopDepartures(
  stopKey: string,
  stopData: StopData,
  schedule: StopScheduleV3,
  rtMap: RtMap,
  nowEpochMs: number,
  rtStopIds: string[],  // raw GTFS stop_ids to check in the RT feed (may differ from key)
): Departure[] {
  const maxRows = CONFIG.MAX_ROWS;
  const routeFilter = new Set(stopData.routes);

  const nowEpoch  = Math.floor(nowEpochMs / 1000);
  const cutoff    = nowEpoch - 60;
  const utcOffset = getUtcOffsetSeconds(nowEpochMs);

  const todayLocal    = localDateParts(nowEpochMs);
  const yesterdayJs   = new Date(Date.UTC(todayLocal.year, todayLocal.month - 1, todayLocal.day - 1));
  const datesToCheck: [number, number, number][] = [
    [yesterdayJs.getUTCFullYear(), yesterdayJs.getUTCMonth() + 1, yesterdayJs.getUTCDate()],
    [todayLocal.year, todayLocal.month, todayLocal.day],
  ];

  const raw: Departure[] = [];

  for (const [baseYear, baseMonth, baseDay] of datesToCheck) {
    const activeServices = activeServicesForDate(schedule, baseYear, baseMonth, baseDay);
    if (activeServices.size === 0) continue;

    for (const trip of stopData.trips) {
      if (!activeServices.has(trip.service_id)) continue;
      if (routeFilter.size > 0 && !routeFilter.has(trip.route)) continue;

      let schedEpoch: number;
      try {
        schedEpoch = gtfsTimeToEpoch(trip.dep, baseYear, baseMonth, baseDay, utcOffset);
      } catch {
        continue;
      }

      // RT overlay.
      let cancelled = false;
      let live      = false;
      let depEpoch  = schedEpoch;

      const rtTrip = rtMap[trip.trip_id];
      if (rtTrip) {
        if (rtTrip.cancelled) {
          cancelled = true;
        } else {
          // The RT feed uses raw GTFS stop_ids (e.g. "000024", "11180") which
          // may differ from our stable key.  Try each rtStopId in order.
          let rtStop: RtStopInfo | undefined;
          for (const rsid of rtStopIds) {
            rtStop = rtTrip.stops[rsid];
            if (rtStop) break;
          }
          if (rtStop) {
            const rtDep = rtStop.departure_epoch ?? rtStop.arrival_epoch;
            if (rtDep) { depEpoch = Math.round(rtDep); live = true; }
            if (rtStop.schedule_relationship === STOP_SR_SKIPPED) cancelled = true;
          }
        }
      }

      if (depEpoch < cutoff) continue;

      raw.push({
        route:     truncate(trip.route, 6),
        dest:      asciiFold(truncate(trip.dest, 28)),
        time:      depEpoch,
        live,
        cancelled,
      });
    }
  }

  // Sort, dedupe, cap.
  raw.sort((a, b) => a.time - b.time);
  const seen = new Set<string>();
  const out: Departure[] = [];
  for (const d of raw) {
    const key = `${d.route}|${d.dest}|${d.time}`;
    if (!seen.has(key)) { seen.add(key); out.push(d); }
  }
  return out.slice(0, maxRows);
}

// ---------------------------------------------------------------------------
// Cron refresh job
// ---------------------------------------------------------------------------

// Open KV once at module level — both the cron and the handler share this handle.
const kv = await Deno.openKv();

async function refresh(): Promise<void> {
  const errors: string[] = [];
  let tuOk    = false;
  let alertOk = false;

  try {
    // 1. Ensure schedule is cached / fresh.
    const schedule = await ensureSchedule(kv);

    // 2. Fetch TripUpdates (best-effort).
    console.info("Fetching GTFS-RT TripUpdates …");
    const { rtMap, ok: rtOk } = await fetchTripUpdates(CONFIG.RT_TRIP_UPDATES_URL);
    tuOk = rtOk;
    if (!rtOk) errors.push("TripUpdates fetch failed");

    // 3. Fetch ServiceAlerts (best-effort — never blocks departures).
    console.info("Fetching GTFS-RT ServiceAlerts …");
    const { alerts, ok: aOk } = await fetchAlerts(CONFIG.RT_SERVICE_ALERTS_URL);
    alertOk = aOk;
    if (!aOk) errors.push("ServiceAlerts fetch failed");

    // 4. Fetch firmware manifest (best-effort — omit key on failure).
    const firmware = await fetchFirmware();

    // 5. Build per-stop departures.
    const nowMs = Date.now();
    const utcOffsetSeconds = getUtcOffsetSeconds(nowMs);

    const stopsOut: DeparturesPayloadV3["stops"] = [];
    for (const stopCfg of CONFIG.STOPS) {
      const stopData = schedule.stops[stopCfg.key];
      if (!stopData) {
        console.warn(`stop_schedule.json has no entry for key "${stopCfg.key}" — emitting empty departures`);
        stopsOut.push({ stop_id: stopCfg.key, stop_label: stopCfg.label, departures: [] });
        continue;
      }
      const rtStopIds = stopCfg.rtStopIds?.length ? stopCfg.rtStopIds : [stopCfg.key];
      const departures = buildStopDepartures(stopCfg.key, stopData, schedule, rtMap, nowMs, rtStopIds);
      stopsOut.push({ stop_id: stopCfg.key, stop_label: stopData.label, departures });
    }

    // 6. Assemble and store v3 payload.
    const payload: DeparturesPayloadV3 = {
      version: 3,
      generated_at: Math.floor(nowMs / 1000),
      utc_offset_seconds: utcOffsetSeconds,
      alerts,
      stops: stopsOut,
      ...(firmware !== undefined ? { firmware } : {}),
    };

    await kv.set(KV_KEY_DEPARTURES, payload);

    const totalDep = stopsOut.reduce((n, s) => n + s.departures.length, 0);
    console.info(
      `Departures v3 updated: ${totalDep} total rows across ${stopsOut.length} stops, ` +
      `${alerts.length} alert(s), firmware=${firmware ? firmware.version : "omitted"}, ` +
      `generated_at=${payload.generated_at}`,
    );
  } catch (err) {
    errors.push(String(err));
    console.error(`refresh() error (non-fatal): ${err}`);
  }

  // 7. Persist RT status for the health page — even on error.
  const rtStatus: RtStatus = {
    trip_updates_ok: tuOk,
    alerts_ok: alertOk,
    last_error: errors.join("; "),
    last_run_at: Math.floor(Date.now() / 1000),
  };
  try {
    await kv.set(KV_KEY_RT_STATUS, rtStatus);
  } catch { /* ignore KV write failure for status */ }
}

// Register the 1-minute cron.
// On Deno Deploy --unstable-cron is implicit. Locally, pass --unstable-cron.
Deno.cron("refresh-departures", "* * * * *", refresh);

// ---------------------------------------------------------------------------
// Health HTML page builder
// ---------------------------------------------------------------------------

/** Format epoch seconds as a local Brisbane time string. */
function fmtLocalTime(epochSec: number): string {
  const d = new Date(epochSec * 1000);
  return d.toLocaleString("en-AU", {
    timeZone: "Australia/Brisbane",
    year: "numeric", month: "short", day: "numeric",
    hour: "2-digit", minute: "2-digit", second: "2-digit",
    hour12: false,
  });
}

/** Format seconds-until as "Xm Ys" or "Xs". */
function fmtRelSec(diffSec: number): string {
  if (diffSec < 0) return `${Math.abs(diffSec)}s ago`;
  const m = Math.floor(diffSec / 60);
  const s = diffSec % 60;
  if (m > 0) return `${m}m ${s}s`;
  return `${s}s`;
}

/** Format minutes-until for a departure (mm:ss or "now"). */
function fmtDepMinutes(epochSec: number, nowSec: number): string {
  const diff = epochSec - nowSec;
  if (diff <= 0) return "now";
  const m = Math.floor(diff / 60);
  const s = diff % 60;
  return `${m}:${String(s).padStart(2, "0")}`;
}

function buildHealthPage(
  payload: DeparturesPayloadV3 | null,
  rtStatus: RtStatus | null,
  nowSec: number,
): string {
  const buildTime  = payload ? fmtLocalTime(payload.generated_at) : "—";
  const buildAge   = payload ? fmtRelSec(nowSec - payload.generated_at) : "—";
  const lastRunAt  = rtStatus ? fmtLocalTime(rtStatus.last_run_at) : "—";
  const tuOk       = rtStatus?.trip_updates_ok ?? false;
  const aOk        = rtStatus?.alerts_ok ?? false;
  const lastErr    = rtStatus?.last_error ?? "";

  const tick   = "&#10003;";
  const cross  = "&#10007;";
  const rtBadge = (ok: boolean) =>
    ok
      ? `<span class="badge ok">${tick} OK</span>`
      : `<span class="badge err">${cross} FAIL</span>`;

  // Alerts section.
  const alertRows = (payload?.alerts ?? []).map((a) =>
    `<tr>
      <td class="mono">${esc(a.id)}</td>
      <td>${esc(a.header)}</td>
      <td><span class="badge sev-${a.severity.toLowerCase()}">${esc(a.severity)}</span></td>
      <td>${esc(a.effect)}</td>
      <td>${esc(a.routes.join(", "))}</td>
    </tr>`
  ).join("\n");

  const alertsHtml = alertRows
    ? `<table>
        <thead><tr><th>ID</th><th>Header</th><th>Severity</th><th>Effect</th><th>Routes</th></tr></thead>
        <tbody>${alertRows}</tbody>
      </table>`
    : `<p class="muted">No active alerts.</p>`;

  // Per-stop departure tables.
  const stopsHtml = (payload?.stops ?? []).map((stop) => {
    const depRows = stop.departures.map((d) => {
      const minStr  = fmtDepMinutes(d.time, nowSec);
      const liveTag = d.live ? '<span class="badge ok">LIVE</span>' : '<span class="badge muted">SCHED</span>';
      const canTag  = d.cancelled ? '<span class="badge err">CNCL</span>' : "";
      return `<tr class="${d.cancelled ? "cancelled" : ""}">
        <td class="mono">${esc(d.route)}</td>
        <td>${esc(d.dest)}</td>
        <td class="time">${minStr}</td>
        <td>${liveTag}${canTag}</td>
      </tr>`;
    }).join("\n");

    const tableHtml = depRows
      ? `<table>
          <thead><tr><th>Route</th><th>Dest</th><th>In</th><th>Status</th></tr></thead>
          <tbody>${depRows}</tbody>
        </table>`
      : `<p class="muted">No upcoming departures.</p>`;

    return `<section class="stop-card">
      <h2>${esc(stop.stop_label)}<small class="stop-id"> [${esc(stop.stop_id)}]</small></h2>
      ${tableHtml}
    </section>`;
  }).join("\n");

  const firmwareHtml = payload?.firmware
    ? `<p>OTA firmware available: <strong>${esc(payload.firmware.version)}</strong>
       &nbsp;<a href="${esc(payload.firmware.bin_url)}">${esc(payload.firmware.bin_url)}</a></p>`
    : `<p class="muted">No firmware block in current payload.</p>`;

  return `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="refresh" content="30">
<title>QLD Bus Sign — Health</title>
<style>
  :root{--bg:#0d1117;--card:#161b22;--border:#30363d;--text:#e6edf3;--muted:#8b949e;
        --ok:#3fb950;--err:#f85149;--warn:#d29922;--info:#388bfd;--accent:#388bfd}
  *{box-sizing:border-box;margin:0;padding:0}
  body{background:var(--bg);color:var(--text);font:14px/1.5 system-ui,sans-serif;padding:12px}
  h1{font-size:1.15rem;margin-bottom:12px}
  h2{font-size:.95rem;margin-bottom:8px;color:var(--accent)}
  small.stop-id{font-size:.75rem;color:var(--muted);font-weight:normal}
  section.status-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:8px;margin-bottom:16px}
  .card{background:var(--card);border:1px solid var(--border);border-radius:8px;padding:12px}
  .card label{display:block;font-size:.75rem;color:var(--muted);margin-bottom:2px}
  .card .val{font-size:.9rem}
  .stop-card{background:var(--card);border:1px solid var(--border);border-radius:8px;padding:12px;margin-bottom:12px}
  table{width:100%;border-collapse:collapse;font-size:.85rem}
  th{text-align:left;padding:4px 8px;color:var(--muted);border-bottom:1px solid var(--border);font-weight:normal}
  td{padding:4px 8px;border-bottom:1px solid var(--border)}
  tr:last-child td{border-bottom:none}
  tr.cancelled td{color:var(--muted);text-decoration:line-through}
  .badge{display:inline-block;border-radius:4px;padding:1px 6px;font-size:.75rem;font-weight:600}
  .badge.ok{background:#1a3a20;color:var(--ok)}
  .badge.err{background:#3a1a1a;color:var(--err)}
  .badge.muted{background:#21262d;color:var(--muted)}
  .badge.sev-warning{background:#3a2d00;color:var(--warn)}
  .badge.sev-severe{background:#3a1a1a;color:var(--err)}
  .badge.sev-info{background:#1a2a3a;color:var(--info)}
  .badge.sev-unknown{background:#21262d;color:var(--muted)}
  .mono{font-family:monospace}
  .time{font-family:monospace;font-weight:600;color:var(--ok)}
  .muted{color:var(--muted)}
  p{margin:8px 0}
  .err-box{background:#3a1a1a;border:1px solid var(--err);border-radius:6px;padding:8px;font-size:.8rem;color:var(--err);margin-top:8px;word-break:break-word}
  h3{font-size:.85rem;color:var(--muted);margin:12px 0 6px}
  .refresh-note{font-size:.75rem;color:var(--muted);margin-top:12px;text-align:right}
  a{color:var(--accent)}
</style>
</head>
<body>
<h1>QLD Bus Sign — Live Health</h1>

<section class="status-grid">
  <div class="card">
    <label>Last build time (AEST)</label>
    <div class="val">${buildTime}</div>
  </div>
  <div class="card">
    <label>Build age</label>
    <div class="val">${buildAge}</div>
  </div>
  <div class="card">
    <label>Last cron run (AEST)</label>
    <div class="val">${lastRunAt}</div>
  </div>
  <div class="card">
    <label>TripUpdates RT</label>
    <div class="val">${rtBadge(tuOk)}</div>
  </div>
  <div class="card">
    <label>ServiceAlerts RT</label>
    <div class="val">${rtBadge(aOk)}</div>
  </div>
  <div class="card">
    <label>Active alerts</label>
    <div class="val">${payload?.alerts.length ?? 0}</div>
  </div>
</section>

${lastErr ? `<div class="err-box">Last error: ${esc(lastErr)}</div>` : ""}

<h3>Stops</h3>
${stopsHtml || '<p class="muted">No data yet.</p>'}

<h3>Service Alerts</h3>
${alertsHtml}

<h3>Firmware OTA</h3>
${firmwareHtml}

<p class="refresh-note">Auto-refreshes every 30 s &nbsp;|&nbsp; <a href="/departures.json">/departures.json</a> &nbsp;|&nbsp; <a href="/health">/health</a></p>
</body>
</html>`;
}

/** HTML-escape a string (prevent injection in health page). */
function esc(s: string): string {
  return String(s)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}

// ---------------------------------------------------------------------------
// HTTP handler
// ---------------------------------------------------------------------------

const CORS_HEADERS: Record<string, string> = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "GET, OPTIONS",
  "Access-Control-Allow-Headers": "*",
};

async function handler(req: Request): Promise<Response> {
  const url  = new URL(req.url);
  const path = url.pathname;

  if (req.method === "OPTIONS") {
    return new Response(null, { status: 204, headers: CORS_HEADERS });
  }

  // ----- GET /health (JSON) -----
  if (path === "/health") {
    const rtStatus = (await kv.get<RtStatus>(KV_KEY_RT_STATUS)).value;
    const payload  = (await kv.get<DeparturesPayloadV3>(KV_KEY_DEPARTURES)).value;
    const status = {
      ok: true,
      generated_at: payload?.generated_at ?? null,
      alerts_count: payload?.alerts.length ?? 0,
      trip_updates_ok: rtStatus?.trip_updates_ok ?? null,
      alerts_ok: rtStatus?.alerts_ok ?? null,
      last_error: rtStatus?.last_error ?? null,
      last_run_at: rtStatus?.last_run_at ?? null,
    };
    return new Response(JSON.stringify(status), {
      headers: { "Content-Type": "application/json", ...CORS_HEADERS },
    });
  }

  // ----- GET /departures.json -----
  if (path === "/departures.json") {
    let entry = await kv.get<DeparturesPayloadV3>(KV_KEY_DEPARTURES);

    if (!entry.value) {
      // Cold start: KV empty — compute on-demand.
      console.info("KV empty — computing departures on demand (cold start)");
      await refresh();
      entry = await kv.get<DeparturesPayloadV3>(KV_KEY_DEPARTURES);
    }

    if (!entry.value) {
      // Still empty (schedule failed to load) — return a minimal valid v3 stub.
      const stub: DeparturesPayloadV3 = {
        version: 3,
        generated_at: Math.floor(Date.now() / 1000),
        utc_offset_seconds: 36000,
        alerts: [],
        stops: CONFIG.STOPS.map((s) => ({ stop_id: s.key, stop_label: s.label, departures: [] })),
      };
      return new Response(JSON.stringify(stub), {
        status: 200,
        headers: { "Content-Type": "application/json", "Cache-Control": "no-store", ...CORS_HEADERS },
      });
    }

    return new Response(JSON.stringify(entry.value), {
      status: 200,
      headers: { "Content-Type": "application/json", "Cache-Control": "no-store", ...CORS_HEADERS },
    });
  }

  // ----- GET / → HTML health page -----
  if (path === "/") {
    const [depEntry, statusEntry] = await Promise.all([
      kv.get<DeparturesPayloadV3>(KV_KEY_DEPARTURES),
      kv.get<RtStatus>(KV_KEY_RT_STATUS),
    ]);
    const nowSec = Math.floor(Date.now() / 1000);
    const html = buildHealthPage(depEntry.value, statusEntry.value, nowSec);
    return new Response(html, {
      status: 200,
      headers: { "Content-Type": "text/html; charset=utf-8", "Cache-Control": "no-store" },
    });
  }

  return new Response("Not Found", { status: 404, headers: CORS_HEADERS });
}

console.info("QLD Bus Sign — Deno Deploy backend starting (Data Contract v3)");
console.info(`STOPS: ${CONFIG.STOPS.map((s) => `${s.key}(${s.routes.join(",")})`).join(", ")}`);
console.info(`MAX_ROWS: ${CONFIG.MAX_ROWS}`);
console.info(`STOP_SCHEDULE_URL: ${CONFIG.STOP_SCHEDULE_URL}`);
console.info(`RT_SERVICE_ALERTS_URL: ${CONFIG.RT_SERVICE_ALERTS_URL}`);
console.info(`FIRMWARE_MANIFEST_URL: ${CONFIG.FIRMWARE_MANIFEST_URL || "(none)"}`);

Deno.serve(handler);
