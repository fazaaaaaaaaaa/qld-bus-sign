# QLD Bus Sign — Deno Deploy Backend

A TypeScript/Deno port of the Python `update_departures.py` cloud backend.
Serves live Brisbane/SEQ bus departure data at `/departures.json`, refreshed
every **1 minute** via `Deno.cron`.

## Difference from the GitHub Actions backend

| | GitHub Actions backend | This Deno Deploy backend |
|---|---|---|
| Refresh rate | Every 5 minutes | Every 1 minute |
| Hosting | GitHub Actions + Pages/raw | Deno Deploy (always-on) |
| Static GTFS | Downloaded + processed by the workflow | Not touched — schedule fetched from `STOP_SCHEDULE_URL` |
| State sharing | File on disk / git commit | Deno KV (shared between cron and HTTP isolates) |
| Cold-start latency | N/A | First request triggers an on-demand refresh |

## Architecture

The heavy weekly job (download 28 MB GTFS zip → build `stop_schedule.json`)
**stays on the existing GitHub repo** (via `build_schedule.py` + the GitHub
Actions workflow). This Deno service only consumes that output:

```
GitHub repo (weekly) → stop_schedule.json (raw URL)
                              ↓ (fetched every 12 h by this service)
                        Deno KV ["schedule_cache"]
                              ↓
Translink GTFS-RT (every 1 min) → merge → Deno KV ["departures"]
                                                  ↓
                              HTTP GET /departures.json  ← firmware
```

**Important:** `STOP_SCHEDULE_URL` defaults to the `fazaaaaaaaaaa/qld-bus-sign`
GitHub repo. If you move `stop_schedule.json` elsewhere (e.g. a private CDN),
set the env var or edit `CONFIG.STOP_SCHEDULE_URL` in `main.ts`.

## Output shape (Data Contract v2)

```json
{
  "stop_label": "Adelaide St, Stop 24 near Edward St",
  "generated_at": 1718870400,
  "utc_offset_seconds": 36000,
  "departures": [
    { "route": "320", "dest": "Eight Mile Plains", "time": 1718870460, "live": true, "cancelled": false },
    { "route": "322", "dest": "Eight Mile Plains", "time": 1718870700, "live": false, "cancelled": false }
  ]
}
```

## Local development

```bash
cd deno/
deno task dev
# or directly:
deno run --unstable-cron --unstable-kv -A main.ts
```

The cron fires every minute. On local Deno you need `--unstable-cron` and
`--unstable-kv`; on Deno Deploy those are enabled automatically by the
`"unstable": ["kv", "cron"]` entry in `deno.json`.

## Deploy to Deno Deploy

### Steps (console.deno.com — the current deploy flow)

1. Go to [console.deno.com](https://console.deno.com) and sign in with GitHub.
2. Create an **organization** (if you don't have one yet).
3. Click **New App** inside your organization.
4. Choose **Deploy from GitHub** and select the `qld-bus-sign` repository.
5. Set the **Entrypoint** to `deno/main.ts`.
6. Click **Create App** — Deno Deploy will build and deploy immediately, and
   will redeploy automatically on every push to `main`.

After deploy your service is live at:

```
https://<app-slug>.<org-slug>.deno.net/departures.json
```

For example: `https://qld-bus-sign.my-org.deno.net/departures.json`

> **Note:** The URL pattern is `.deno.net` (NOT `.deno.dev`). The old
> `dash.deno.com` dashboard and `deployctl` CLI are no longer the primary
> deploy path — use `console.deno.com` as described above.

### Environment variables

Set these in **console.deno.com → your app → Settings → Environment Variables**
(or edit the `CONFIG` block at the top of `main.ts` directly).

| Env var | Default | Description |
|---|---|---|
| `STOP_SCHEDULE_URL` | GitHub raw URL | Where to fetch `stop_schedule.json` |
| `RT_URL` | Translink SEQ TripUpdates | GTFS-RT endpoint |
| `STOP_IDS` | `24,000024` | Comma-separated GTFS stop IDs |
| `ROUTE_FILTER` | `320,322` | Comma-separated route_short_names (blank = all) |
| `STOP_LABEL` | `Adelaide St, Stop 24 near Edward St` | Header text on sign |
| `TIMEZONE` | `Australia/Brisbane` | IANA timezone name |
| `MAX_ROWS` | `7` | Max departure rows in response |
| `SCHEDULE_TTL_HOURS` | `12` | Hours before re-fetching schedule |
| `FEED_TIMEOUT_MS` | `15000` | RT fetch timeout in ms |

## deno.json requirements

The file must contain `"unstable": ["kv", "cron"]`. Both `Deno.openKv` and
`Deno.cron` are unstable APIs — without this entry the deploy will crash at
warm-up with `Deno.openKv is not a function`. The `deno.json` in this repo
already has this set correctly.

The npm import pin is `npm:gtfs-realtime-bindings@1.1.1` — do not change it
to 1.2.x (those versions do not exist).

## Wire up the firmware

Once deployed, set `DATA_URL` in `../firmware/include/config.h` to your live
`.deno.net` URL:

```c
#define DATA_URL "https://<app-slug>.<org-slug>.deno.net/departures.json"
```

## Endpoints

| Path | Description |
|---|---|
| `GET /departures.json` | Data Contract v2 JSON — what the firmware polls |
| `GET /` | Same as `/departures.json` |
| `GET /health` | Returns `ok` — use for uptime checks |
