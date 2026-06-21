#pragma once
// =============================================================================
// board_render.h — Departure board layout renderer (Translink SmartStop replica)
// QLD Bus Departure Sign — Xteink X4 Firmware (JSON/NTP edition)
//
// Renders a 4-level-greyscale departure board on the 480×800 (portrait) panel
// using GxEPD2_4G + Adafruit GFX FreeSans fonts.
//
// Public API:
//   drawBoard(display, departures, stop_label, alerts, now_utc, stale,
//             fetchEpoch, batteryPct, utc_offset_seconds)
//     display             — reference to GxEPD2_4G<...> display object
//     departures          — JsonArrayConst of the SELECTED stop's departures[]
//     stop_label          — C-string label for the selected stop
//     alerts              — JsonArrayConst of top-level alerts[] (may be empty)
//     now_utc             — current UTC epoch seconds from NTP (0 if failed)
//     stale               — true when rendering from NVS cache
//     fetchEpoch          — UTC epoch of last successful fetch (0 = unknown)
//     batteryPct          — 0–100 or -1 to suppress battery glyph
//     utc_offset_seconds  — from JSON (default 36000 = AEST)
//
//   drawSetupScreen(display, apName, ip)
//   drawNoDataScreen(display)
//   drawVersionErrorScreen(display)          — new: v!=3 notice
//   drawLowBatteryScreen(display, batteryPct) — new: "Plug me in"; batteryPct = 0–100 or -1
//
// Design: Translink SmartStop replica (design/design_translink.py is authoritative)
//   PORTRAIT 480 wide x 800 tall.  White background throughout.
//   4 grey levels matching the panel's quantised steps:
//     GxEPD_BLACK     (0)   — primary text, route number in chip
//     GxEPD_DARKGREY  (85)  — route chip fill, secondary text ("min", date), signal arc
//     GxEPD_LIGHTGREY (170) — column-header band, thin row separators, footer strip
//     GxEPD_WHITE     (255) — background, route number text in chip
//
// Column anchors (shared by header band AND every row — required for alignment):
//   COL_ROUTE_X = 18   left edge of route chip / "Route" label
//   COL_DEST_X  = 104  left edge of destination text / "Destination" label
//   COL_DEP_R   = 462  right edge of departing value / "Departing" label
//
// Vertical regions (portrait, y = 0..799):
//   y=0..83         Header: stop name (top-left, wraps 2 lines) + clock top-right
//   y=84..116       Column header band (light grey, labels centred at y=100)
//   [y=117..149]    Alert banner (dark strip, 33 px) — ONLY when alerts present
//   y=122/155..+n*RH Departure rows (ROW_H=92; top shifts when banner present)
//   y=...+8         Services-at-this-stop footer strip (light grey)
//   y=...           Footer route chips + real-time label + updated/battery footer
//
// Font sizes (FreeSans family, Adafruit GFX):
//   FreeSansBold24pt7b  — stop name (wraps), ETA big number
//   FreeSans18pt7b      — clock, "min" unit
//   FreeSansBold12pt7b  — route number in chip (narrow-ish bold)
//   FreeSans12pt7b      — destination text, date under clock
//   FreeSans9pt7b       — column headers, footer text, "Real-time info", updated/battery
//
// =============================================================================

#include <Arduino.h>            // time_t on ESP32
#include <GxEPD2_4G_4G.h>      // 4-grey template wrapper (zinggjm/GxEPD2_4G)
                                // Also pulls in GxEPD_BLACK/DARKGREY/LIGHTGREY/WHITE
                                // and the panel class GxEPD2_426_GDEQ0426T82 (self-contained
                                // in GxEPD2_4G's own src/gdeq/ — NOT from a separate GxEPD2 lib)

// Adafruit GFX FreeSans fonts — Helvetica-style, matches Translink typeface
#include <Fonts/FreeSans9pt7b.h>          // column header labels, footer text
#include <Fonts/FreeSans12pt7b.h>         // destination text, date
#include <Fonts/FreeSans18pt7b.h>         // clock time, "min" unit
#include <Fonts/FreeSansBold12pt7b.h>     // route number in dark chip
#include <Fonts/FreeSansBold24pt7b.h>     // ETA big number, stop name

#include <ArduinoJson.h>
#include "config.h"

// =============================================================================
// Grey level mapping
//
// The 4 panel levels (design_translink.py literal values) map to GxEPD2_4G
// named constants (defined in src/GxEPD2_4G.h as RGB565 uint16_t values).
// The GxEPD2_4G_4G drawPixel() maps RGB565 brightness to 4 panel levels:
//
//   Constant           RGB565    Panel level   Python grey
//   GxEPD_BLACK        0x0000    0 (black)     0
//   GxEPD_DARKGREY     0x7BEF    1 (dark)      85
//   GxEPD_LIGHTGREY    0xC618    2 (light)     170
//   GxEPD_WHITE        0xFFFF    3 (white)     255
//
// Use ONLY these four values for colour parameters — any other RGB565 value
// will be quantised to one of these four levels by brightness threshold.
// =============================================================================

// =============================================================================
// Panel dimensions (portrait orientation after setRotation())
// The physical panel is 800 px (horizontal) x 480 px (vertical) in landscape.
// setRotation(1) or setRotation(3) maps it to a 480 wide x 800 tall canvas.
// =============================================================================
static constexpr int16_t PANEL_W = 480;   // canvas width in portrait
static constexpr int16_t PANEL_H = 800;   // canvas height in portrait

// =============================================================================
// Shared column anchors — MUST be used for BOTH header labels and row content.
// Values taken directly from design_translink.py.
// =============================================================================
static constexpr int16_t COL_ROUTE_X = 18;   // left edge of chip / "Route" label
static constexpr int16_t COL_DEST_X  = 104;  // left edge of dest / "Destination" label
static constexpr int16_t COL_DEP_R   = 462;  // right edge of value / "Departing" label
static constexpr int16_t DEST_MAXW   = 270;  // max dest text width (to ~374 px, leaves room for dep block)

// =============================================================================
// Route chip geometry (design: CHIP_W=64, CHIP_H=40, radius=7)
// =============================================================================
static constexpr int16_t CHIP_W      = 64;
static constexpr int16_t CHIP_H      = 40;
static constexpr int16_t CHIP_RADIUS = 7;

// =============================================================================
// Header layout
// =============================================================================
static constexpr int16_t HDR_STOP_TOP  = 18;   // y of first stop-name text line
static constexpr int16_t HDR_LINE_H    = 25;   // line height for 2-line stop name
// Clock: right-aligned to COL_DEP_R, baseline at y=38 (24pt ascent≈33 → top near y=5)
static constexpr int16_t HDR_CLOCK_Y   = 38;   // baseline for clock (FreeSansBold24pt7b)
// Date: right-aligned to COL_DEP_R, baseline below clock
static constexpr int16_t HDR_DATE_Y    = 62;   // baseline for date (FreeSans12pt7b)

// =============================================================================
// Column header band
// =============================================================================
static constexpr int16_t COLHDR_TOP    = 84;   // band top
static constexpr int16_t COLHDR_BOT    = 116;  // band bottom (32 px tall)
static constexpr int16_t COLHDR_MID_Y  = 100;  // vertical midpoint for label baseline

// =============================================================================
// Alert banner geometry (Feature 3)
// Drawn immediately below the column header band when alerts[] is non-empty.
//   ALERT_BANNER_TOP = COLHDR_BOT = 116  (starts right below col header)
//   ALERT_BANNER_H   = 33 px             (dark strip)
//   ALERT_BANNER_BOT = 149
//   ALERT_BANNER_TEXT_Y baseline = 136   (FreeSans9pt7b ascent≈13, centred in 33 px)
// =============================================================================
static constexpr int16_t ALERT_BANNER_TOP    = COLHDR_BOT;       // 116
static constexpr int16_t ALERT_BANNER_H      = 33;
static constexpr int16_t ALERT_BANNER_BOT    = ALERT_BANNER_TOP + ALERT_BANNER_H; // 149
static constexpr int16_t ALERT_BANNER_TEXT_Y = ALERT_BANNER_TOP + 10 + 13; // 139 (top+pad+ascent)

// =============================================================================
// Row layout
// ROW_TOP_BASE: first row top pixel when no alert banner is shown.
// ROW_TOP_ALERT: first row top pixel when the alert banner is shown (+33 px).
// =============================================================================
static constexpr int16_t ROW_TOP_BASE  = 122;  // normal: right after COLHDR_BOT+6
static constexpr int16_t ROW_TOP_ALERT = ALERT_BANNER_BOT + 6;  // 155 when banner present
static constexpr int16_t ROW_H         = 92;   // row height (6 rows × 92 = 552 px)
// Legacy alias kept so the footer constant below still compiles.
static constexpr int16_t ROW_TOP = ROW_TOP_BASE;

// Signal-arc placement (top-right of the dep block, above the number)
static constexpr int16_t ARC_OFFSET_X = 4;     // inset from COL_DEP_R
static constexpr int16_t ARC_OFFSET_Y = 22;    // above row centreline

// =============================================================================
// Footer strip (services at this stop)
//
// The footer is anchored at ROW_TOP_BASE + MAX_ROWS * ROW_H + 8 = 682 in the
// base (no-alert) case.  When the alert banner is shown we render one fewer
// row, so the last row still ends at 155 + 5*92 = 615, well above 682.
// The footer position is therefore always computed from the BASE row top,
// keeping the footer strip at a stable y position regardless of banner state.
// =============================================================================
static constexpr int16_t FOOTER_STRIP_TOP = ROW_TOP_BASE + MAX_ROWS * ROW_H + 8;
// FOOTER_STRIP_TOP with MAX_ROWS=6, ROW_H=92: 122+552+8 = 682 (≤762 ✓)
static constexpr int16_t FOOTER_STRIP_H   = 28;

// =============================================================================
// drawSignalArc()
//
// Renders the Translink real-time "signal arc" icon — three concentric arcs
// drawn from 300° to 350° (upper-right fan) at radii 4, 8, 12 px.
// Adafruit GFX has no arc primitive, so we approximate each arc with a short
// set of filled circles (pixels) along its circumference.
//
// cx, cy  — arc centre point
// fill    — colour (use GxEPD_DARKGREY per the design)
// =============================================================================
template <typename GxEPD2_Type>
static void drawSignalArc(GxEPD2_Type& display, int16_t cx, int16_t cy, uint16_t fill)
{
    // Draw 3 arcs: radii 4, 8, 12 px, spanning 300° to 350°
    // step in degrees; smaller step = smoother arc (2° is fine at these radii)
    const int8_t radii[] = {4, 8, 12};
    for (uint8_t ri = 0; ri < 3; ri++) {
        float r = radii[ri];
        for (int16_t deg = 300; deg <= 350; deg += 2) {
            // Convert to radians.  Note: GFX Y-axis points down, so sin sign flips.
            float rad = deg * 3.14159f / 180.0f;
            int16_t px = cx + (int16_t)(r * cosf(rad));
            int16_t py = cy - (int16_t)(r * sinf(rad));  // subtract: screen Y grows down
            display.drawPixel(px,   py, fill);
            display.drawPixel(px+1, py, fill);  // 2-px wide so arc is visible on e-ink
        }
    }
}

// =============================================================================
// drawChip()
//
// Draws a dark-grey filled rounded rectangle with WHITE route number centred.
// Mirrors design_translink.py: chip() function.
//
// x_left — left edge of chip rectangle
// cy     — vertical centre of the chip
// route  — route number string
// w, h   — chip width/height (default CHIP_W, CHIP_H)
// =============================================================================
template <typename GxEPD2_Type>
static void drawChip(GxEPD2_Type& display,
                     int16_t x_left, int16_t cy,
                     const char* route,
                     int16_t w = CHIP_W, int16_t h = CHIP_H)
{
    // Fill rounded rect in DARK grey
    display.fillRoundRect(x_left, cy - h/2, w, h, CHIP_RADIUS, GxEPD_DARKGREY);

    // Route number: FreeSansBold12pt7b, WHITE, centred inside chip
    display.setFont(&FreeSansBold12pt7b);
    display.setTextColor(GxEPD_WHITE);

    // Measure text to centre it horizontally
    int16_t bx, by;
    uint16_t bw, bh;
    display.getTextBounds(route, 0, 0, &bx, &by, &bw, &bh);

    // Horizontal centre: x_left + (w - bw) / 2 - bx  (bx is left bearing, may be negative)
    int16_t tx = x_left + (w - (int16_t)bw) / 2 - bx;
    // Vertical centre: chip centre + half of cap-height.
    // FreeSansBold12pt7b: ascent≈17px. Baseline = cy + ascent/2 ≈ cy + 8
    int16_t ty = cy + 8;
    display.setCursor(tx, ty);
    display.print(route);
}

// =============================================================================
// drawTextRightAligned()
//
// Places text so its visual right edge lands exactly at anchor_x.
// Uses getTextBounds() which returns (bx, by, bw, bh) at origin.
// The rendered string's visual right = cursor_x + bx + bw.
// Solving for cursor_x: anchor_x - bw - bx.
// =============================================================================
template <typename GxEPD2_Type>
static void drawTextRightAligned(GxEPD2_Type& display,
                                 int16_t anchor_x, int16_t baseline_y,
                                 const char* text, uint16_t color)
{
    int16_t bx, by;
    uint16_t bw, bh;
    display.getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
    int16_t cx = anchor_x - (int16_t)bw - bx;
    display.setTextColor(color);
    display.setCursor(cx, baseline_y);
    display.print(text);
}

// =============================================================================
// wrapText()
//
// Splits text into up to maxLines lines that each fit within maxW pixels.
// Returns number of lines written.  lines[] must hold maxLines char* pointers;
// caller provides buffer storage via lineBufs.
//
// This is a simplified word-wrap that mirrors design_translink.py's wrap().
// =============================================================================
static int wrapText(const char* src, int16_t maxW, int maxLines,
                    char lineBufs[][80], int lineBufSize)
{
    // We need a display reference for getTextBounds, but FreeSans12pt7b glyph widths
    // are not available here without the display.  Instead we use a character-count
    // heuristic: FreeSans12pt7b average char width ≈ 9 px; maxW ≈ 270 → ~30 chars/line.
    // For accurate splitting, the drawing code will use getTextBounds after setting font.
    // Here we do a best-effort split by character limit.
    //
    // NOTE: For the actual rendering we call this helper to pre-split, then measure
    //       per-line with getTextBounds to compute the baseline offsets.
    // 12pt FreeSans: average character advance ~9 px at 12 pt.
    const int maxCharsPerLine = (maxW / 9) + 2;  // +2 for safety

    int nLines = 0;
    const char* p = src;
    while (*p && nLines < maxLines) {
        // Skip leading spaces
        while (*p == ' ') p++;
        if (!*p) break;

        // Find how many characters fit within maxCharsPerLine (word-boundary aware)
        int charCount = 0;
        const char* lineStart = p;
        const char* lastSpace = nullptr;

        while (*p && charCount < maxCharsPerLine) {
            if (*p == ' ') lastSpace = p;
            p++;
            charCount++;
        }

        // If we haven't reached end of string and there's a word boundary, break there
        if (*p && lastSpace && lastSpace > lineStart) {
            p = lastSpace;
            charCount = (int)(lastSpace - lineStart);
        }

        // Copy line to buffer
        int copyLen = (charCount < lineBufSize - 1) ? charCount : lineBufSize - 1;
        strncpy(lineBufs[nLines], lineStart, copyLen);
        lineBufs[nLines][copyLen] = '\0';
        nLines++;

        // If last line and more text remains, add ellipsis
        if (nLines == maxLines && *p) {
            int len = strlen(lineBufs[nLines-1]);
            if (len > 3) {
                lineBufs[nLines-1][len-3] = '.';
                lineBufs[nLines-1][len-2] = '.';
                lineBufs[nLines-1][len-1] = '.';
                lineBufs[nLines-1][len]   = '\0';
            }
        }
    }
    return nLines;
}

// =============================================================================
// formatLocalTime12h()
//
// Converts UTC epoch + offset to "H:MM AM/PM" string (12-hour clock).
// Mirrors design_translink.py CLOCK format "6:55 PM".
// =============================================================================
static void formatLocalTime12h(time_t  utc_epoch,
                               int32_t utc_offset_seconds,
                               char*   buf,
                               size_t  bufLen)
{
    int64_t local_epoch = (int64_t)utc_epoch + (int64_t)utc_offset_seconds;
    int32_t day_sec = (int32_t)(local_epoch % 86400LL);
    if (day_sec < 0) day_sec += 86400;
    int hh24 = day_sec / 3600;
    int mm   = (day_sec % 3600) / 60;

    const char* period = (hh24 < 12) ? "AM" : "PM";
    int hh12 = hh24 % 12;
    if (hh12 == 0) hh12 = 12;  // 0 → 12 (midnight/noon)

    snprintf(buf, bufLen, "%d:%02d %s", hh12, mm, period);
}

// =============================================================================
// formatLocalDate()
//
// Converts UTC epoch + offset to "Day DDth Mon" string, e.g. "Wed 18th Mar".
// =============================================================================
static void formatLocalDate(time_t  utc_epoch,
                            int32_t utc_offset_seconds,
                            char*   buf,
                            size_t  bufLen)
{
    int64_t local_epoch = (int64_t)utc_epoch + (int64_t)utc_offset_seconds;
    // We need struct tm — use gmtime on the locally-adjusted epoch.
    time_t local_t = (time_t)(local_epoch);
    struct tm* t = gmtime(&local_t);
    if (!t) { buf[0] = '\0'; return; }

    const char* days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                            "Jul","Aug","Sep","Oct","Nov","Dec"};

    int day = t->tm_mday;
    const char* suffix = "th";
    if (day == 1 || day == 21 || day == 31) suffix = "st";
    else if (day == 2 || day == 22)         suffix = "nd";
    else if (day == 3 || day == 23)         suffix = "rd";

    snprintf(buf, bufLen, "%s %d%s %s",
             days[t->tm_wday], day, suffix, months[t->tm_mon]);
}

// =============================================================================
// etaString()
//
// Derives ETA display fields from absolute UTC departure time and now_utc.
// Identical logic to the existing board_render.h (data layer untouched).
//
// numBuf  — receives "Now", "CNCL", "---", ">90", or a digit string
// unitBuf — receives "min" or ""
// isGone  — set true when the entry should be dropped (>60 s past)
// =============================================================================
static void etaString(time_t dep_utc, time_t now_utc, bool cancelled,
                      char* numBuf, size_t numLen,
                      char* unitBuf, size_t unitLen,
                      bool* isGone)
{
    *isGone = false;

    if (cancelled) {
        snprintf(numBuf,  numLen,  "CNCL");
        unitBuf[0] = '\0';
        return;
    }

    if (now_utc == 0) {
        snprintf(numBuf,  numLen,  "?");
        unitBuf[0] = '\0';   // empty unit: routes to the lone-word render path
        return;
    }

    int64_t delta_s = (int64_t)dep_utc - (int64_t)now_utc;

    if (delta_s < -60LL) {
        *isGone = true;
        snprintf(numBuf,  numLen,  "---");
        unitBuf[0] = '\0';
        return;
    }

    if (delta_s <= 30LL) {
        snprintf(numBuf,  numLen,  "Now");
        unitBuf[0] = '\0';
        return;
    }

    int64_t delta_min = delta_s / 60LL;
    if (delta_min > 90LL) {
        snprintf(numBuf,  numLen,  ">90");
        snprintf(unitBuf, unitLen, "min");
        return;
    }

    snprintf(numBuf,  numLen,  "%lld", (long long)delta_min);
    snprintf(unitBuf, unitLen, "min");
}

// =============================================================================
// drawBoard()
//
// Full Translink SmartStop layout in 4-level greyscale, portrait 480×800.
// Drives GxEPD2_4G's paged refresh loop (setFullWindow / firstPage / nextPage)
// OR a partial window update (setPartialWindow / displayWindow) per Feature 1.
//
// Parameters:
//   display            — GxEPD2_4G display reference
//   departures         — JsonArrayConst of the SELECTED stop's departures array
//   stop_label         — string label for the selected stop (header)
//   alerts             — JsonArrayConst of top-level alerts[] (empty = no banner)
//   now_utc            — NTP UTC epoch (0 if NTP failed)
//   stale              — true when doc came from NVS cache
//   fetchEpoch         — UTC epoch of last successful fetch (0 = unknown)
//   batteryPct         — 0–100 or -1 (disabled/unknown)
//   utc_offset_seconds — timezone offset in seconds (from JSON; default 36000 = AEST)
//   forceFullRefresh   — when true, always do a full-screen refresh even if
//                        USE_PARTIAL_REFRESH is 1 (used on layout change / ghosting)
//   skipRedraw         — when true, no drawing at all (content unchanged; caller
//                        determined hash matches) — we still call hibernate() so the
//                        panel is in a low-power state before deep sleep
// =============================================================================
template <typename GxEPD2_Type>
void drawBoard(GxEPD2_Type&        display,
               JsonArrayConst      departures,
               const char*         stop_label,
               JsonArrayConst      alerts,
               time_t              now_utc,
               bool                stale            = false,
               time_t              fetchEpoch       = 0,
               int                 batteryPct       = -1,
               int32_t             utc_offset_seconds = 36000,
               bool                forceFullRefresh = true,
               bool                skipRedraw       = false)
{
    // ---- Short-circuit if nothing changed (Feature 1a) -----------------------
    if (skipRedraw) {
        // Panel stays showing its previous image — just ensure it is hibernated.
        display.hibernate();
        return;
    }

    // ---- Determine alert presence + visible row count -----------------------
    bool hasAlerts = false;
    char alertBuf[ALERT_MAX_CHARS + 12];  // alert text + " (+N)" worst-case suffix
    alertBuf[0] = '\0';

#if SHOW_ALERTS
    {
        int alertCount = 0;
        for (JsonObjectConst a : alerts) {
            (void)a;
            alertCount++;
        }
        if (alertCount > 0) {
            hasAlerts = true;
            // Build banner text: first alert header, truncated, with (+N-1) suffix
            const char* firstHeader = alerts[0]["header"] | "";
            char truncBuf[ALERT_MAX_CHARS + 1];
            strncpy(truncBuf, firstHeader, ALERT_MAX_CHARS);
            truncBuf[ALERT_MAX_CHARS] = '\0';
            if (alertCount > 1) {
                snprintf(alertBuf, sizeof(alertBuf), "%s (+%d)", truncBuf, alertCount - 1);
            } else {
                strncpy(alertBuf, truncBuf, sizeof(alertBuf) - 1);
                alertBuf[sizeof(alertBuf) - 1] = '\0';
            }
        }
    }
#endif // SHOW_ALERTS

    // Effective row capacity: reduce by 1 when alert banner is shown
    const int effectiveMaxRows = hasAlerts ? (MAX_ROWS - 1) : MAX_ROWS;

    // Row-area top Y: shifted down by banner height when banner present
    const int16_t rowAreaTop = hasAlerts ? ROW_TOP_ALERT : ROW_TOP_BASE;

    // ---- Pre-compute header strings (reused every page) -----------------------
    char clockBuf[12];   // "6:55 PM"
    char dateBuf[20];    // "Wed 18th Mar"
    if (now_utc > 0) {
        formatLocalTime12h(now_utc, utc_offset_seconds, clockBuf, sizeof(clockBuf));
        formatLocalDate(now_utc, utc_offset_seconds, dateBuf, sizeof(dateBuf));
    } else {
        strncpy(clockBuf, "--:--", sizeof(clockBuf));
        clockBuf[sizeof(clockBuf)-1] = '\0';
        strncpy(dateBuf, "", sizeof(dateBuf));
    }

    // ---- Pre-compute "Updated H:MM AM/PM" string -----------------------------
    char updatedBuf[32];
    {
        time_t updateTime = 0;
        if (fetchEpoch > 0) {
            updateTime = fetchEpoch;
        }
        if (updateTime > 0) {
            char timeBuf[12];
            formatLocalTime12h(updateTime, utc_offset_seconds, timeBuf, sizeof(timeBuf));
            snprintf(updatedBuf, sizeof(updatedBuf), "Updated %s", timeBuf);
        } else {
            strncpy(updatedBuf, "Updated: unknown", sizeof(updatedBuf));
            updatedBuf[sizeof(updatedBuf)-1] = '\0';
        }
    }

    // ---- Pre-build per-row data (ETA, done before paged loop) ----------------
    struct RowData {
        char route[12];
        char dest[80];
        char etaNum[8];   // "5", "Now", "CNCL", ">90", "---", "?"
        char etaUnit[8];  // "min" or ""
        bool live;
        bool cancelled;
    };
    RowData rows[MAX_ROWS];
    int numRows = 0;

    for (JsonObjectConst dep : departures) {
        if (numRows >= effectiveMaxRows) break;

        const char* route     = dep["route"]     | "?";
        const char* dest      = dep["dest"]      | "";
        time_t      dep_time  = (time_t)dep["time"].as<long long>();
        bool        live      = dep["live"]      | false;
        bool        cancelled = dep["cancelled"] | false;

        bool isGone = false;
        char etaNum[8], etaUnit[8];
        etaString(dep_time, now_utc, cancelled,
                  etaNum,  sizeof(etaNum),
                  etaUnit, sizeof(etaUnit),
                  &isGone);
        if (isGone) continue;

        RowData& r = rows[numRows++];
        strncpy(r.route,   route,   sizeof(r.route)   - 1); r.route[sizeof(r.route)-1]    = '\0';
        strncpy(r.dest,    dest,    sizeof(r.dest)    - 1); r.dest[sizeof(r.dest)-1]      = '\0';
        strncpy(r.etaNum,  etaNum,  sizeof(r.etaNum)  - 1); r.etaNum[sizeof(r.etaNum)-1]  = '\0';
        strncpy(r.etaUnit, etaUnit, sizeof(r.etaUnit) - 1); r.etaUnit[sizeof(r.etaUnit)-1] = '\0';
        r.live      = live;
        r.cancelled = cancelled;
    }

    // =========================================================================
    // Partial-refresh departures region (Feature 1b)
    //
    // When USE_PARTIAL_REFRESH==1 AND NOT forceFullRefresh: we use
    // setPartialWindow + displayWindow restricted to the rows area + alert
    // banner area.  This region starts at COLHDR_BOT and extends to
    // FOOTER_STRIP_TOP (just before the stable footer).
    //
    // When forceFullRefresh (first boot, layout change, ghost-clear cycle, or
    // USE_PARTIAL_REFRESH==0): we do a full setFullWindow refresh.
    //
    // IMPORTANT: In both cases we draw into the same page loop with the same
    // drawing commands.  The only difference is which window region GxEPD2 sends
    // to the panel.  Drawing outside a partial window is clipped by the driver.
    //
    // The partial window chosen is the full panel width for simplicity (avoids
    // needing to clip the battery/stale badge which can span x).  We restrict
    // only in Y: from y=COLHDR_BOT (top of alert banner / rows) to y=FOOTER_STRIP_TOP.
    //
    // Rationale for conservative Y region:
    //   - Header (y=0..COLHDR_BOT): stop label + clock/date — stable, don't flash
    //   - Rows + alert (y=COLHDR_BOT..FOOTER_STRIP_TOP): changes every cycle
    //   - Footer (y=FOOTER_STRIP_TOP..PANEL_H): route chips, updated time — changes
    //     with stale state but not every departure cycle.  We include it in partial
    //     update so the updated-time stays current.
    //   For a more aggressive split you could do rows-only; this is the safe choice.
    //
    // NOTE: GxEPD2 partial window for 4-grey panels: the library's
    //   setPartialWindow(x, y, w, h) + displayWindow() path sends a partial
    //   OTP command sequence to the SSD1677.  Whether this produces visible
    //   artefacts depends on the panel batch; set USE_PARTIAL_REFRESH 0 to disable.
    // =========================================================================

    // The full-window path draws everything; the partial path also draws everything
    // but GxEPD2 clips to the partial window rect.  Since the header is stable we
    // only save panel-wear, not drawing time, in the partial path.

    // Partial window: full width, from just below header to panel bottom
    // (include footer so updated-time refreshes; header excluded to reduce flash).
    constexpr int16_t PARTIAL_Y = COLHDR_BOT;   // 116
    constexpr int16_t PARTIAL_H = PANEL_H - COLHDR_BOT;  // 684

#if USE_PARTIAL_REFRESH
    if (!forceFullRefresh) {
        display.setPartialWindow(0, PARTIAL_Y, PANEL_W, PARTIAL_H);
    } else {
        display.setFullWindow();
    }
#else
    display.setFullWindow();
#endif

    display.firstPage();
    do {

        // ================================================================
        // BACKGROUND — white fill (full panel; clipped to partial window)
        // ================================================================
        display.fillScreen(GxEPD_WHITE);

        // ================================================================
        // SECTION 1 — Header
        // (drawn in both full and partial paths; in partial mode the header
        //  region y=0..COLHDR_BOT is outside the partial window and the
        //  driver discards those pixels — the existing header image remains.)
        // ================================================================

        // Clock (FreeSansBold24pt7b, BLACK, right-aligned to COL_DEP_R)
        display.setFont(&FreeSansBold24pt7b);
        drawTextRightAligned(display, COL_DEP_R, HDR_CLOCK_Y, clockBuf, GxEPD_BLACK);

        // Date (FreeSans12pt7b, DARK grey, right-aligned to COL_DEP_R)
        display.setFont(&FreeSans12pt7b);
        drawTextRightAligned(display, COL_DEP_R, HDR_DATE_Y, dateBuf, GxEPD_DARKGREY);

        // Stop name — word-wrap up to 2 lines, left at COL_ROUTE_X
        {
            char lineBufs[2][80];
            int nLines = wrapText(stop_label, 240, 2, lineBufs, 80);
            display.setFont(&FreeSans12pt7b);
            display.setTextColor(GxEPD_BLACK);
            for (int li = 0; li < nLines; li++) {
                int16_t yb = HDR_STOP_TOP + 17 + li * HDR_LINE_H;
                display.setCursor(COL_ROUTE_X, yb);
                display.print(lineBufs[li]);
            }
        }

        // ================================================================
        // SECTION 2 — Column header band (light grey)
        // ================================================================
        display.fillRect(0, COLHDR_TOP, PANEL_W, COLHDR_BOT - COLHDR_TOP, GxEPD_LIGHTGREY);

        display.setFont(&FreeSans9pt7b);
        display.setTextColor(GxEPD_DARKGREY);

        constexpr int16_t COLHDR_BL = 106;  // label baseline (midpoint centred)

        display.setCursor(COL_ROUTE_X, COLHDR_BL);
        display.print("Route");

        display.setCursor(COL_DEST_X, COLHDR_BL);
        display.print("Destination");

        drawTextRightAligned(display, COL_DEP_R, COLHDR_BL, "Departing", GxEPD_DARKGREY);

        // ================================================================
        // SECTION 2b — Alert banner (Feature 3)
        //
        // A thin dark strip between the column header and the first row.
        // 33 px tall (ALERT_BANNER_H), dark grey background, white text.
        // FreeSans9pt7b; truncated first alert header + " (+N)" if >1 alert.
        // Only drawn when hasAlerts is true.
        // ================================================================
        if (hasAlerts) {
            // Dark strip background
            display.fillRect(0, ALERT_BANNER_TOP, PANEL_W, ALERT_BANNER_H, GxEPD_DARKGREY);

            // White alert text, left-padded 18 px from COL_ROUTE_X
            display.setFont(&FreeSans9pt7b);
            display.setTextColor(GxEPD_WHITE);
            display.setCursor(COL_ROUTE_X, ALERT_BANNER_TEXT_Y);
            display.print(alertBuf);
        }

        // ================================================================
        // SECTION 3 — Departure rows
        //
        // Row top shifts when alert banner is present (rowAreaTop).
        // Visible row count is effectiveMaxRows (MAX_ROWS or MAX_ROWS-1).
        // ================================================================
        for (int i = 0; i < numRows; i++) {
            const RowData& r = rows[i];

            // Row centreline (adjusted for alert banner offset)
            int16_t cy = rowAreaTop + i * ROW_H + ROW_H / 2;

            // ---- Route chip (dark grey filled, white text, centred on cy) ---
            drawChip(display, COL_ROUTE_X, cy, r.route);

            // ---- Destination (2-line wrap, centred on cy) -------------------
            {
                char lineBufs[2][80];
                int nLines = wrapText(r.dest, DEST_MAXW, 2, lineBufs, 80);

                display.setFont(&FreeSans12pt7b);
                display.setTextColor(GxEPD_BLACK);

                const int16_t ascent = 17;
                const int16_t lh     = 25;

                int16_t blockH = (nLines == 1) ? ascent : (nLines - 1) * lh + ascent;
                int16_t topBL  = cy - blockH / 2 + ascent;

                for (int li = 0; li < nLines; li++) {
                    int16_t yb = topBL + li * lh;
                    display.setCursor(COL_DEST_X, yb);
                    display.print(lineBufs[li]);

                    // Strikethrough for cancelled
                    if (r.cancelled) {
                        int16_t bx, by; uint16_t bw, bh;
                        display.getTextBounds(lineBufs[li], COL_DEST_X, yb, &bx, &by, &bw, &bh);
                        display.drawFastHLine(bx, yb - 8, (int16_t)bw, GxEPD_BLACK);
                    }
                }
            }

            // ---- Signal arc (if live) ----------------------------------------
            if (r.live) {
                drawSignalArc(display,
                              COL_DEP_R - ARC_OFFSET_X,
                              cy - ARC_OFFSET_Y,
                              GxEPD_DARKGREY);
            }

            // ---- Departing value (right-aligned to COL_DEP_R, centred on cy) -
            bool isNow  = (strcmp(r.etaNum, "Now")  == 0);
            bool isCncl = (strcmp(r.etaNum, "CNCL") == 0);
            bool hasUnit = (r.etaUnit[0] != '\0');

            if (isNow || isCncl) {
                display.setFont(&FreeSansBold24pt7b);
                drawTextRightAligned(display, COL_DEP_R, cy + 12, r.etaNum, GxEPD_BLACK);

            } else if (hasUnit) {
                display.setFont(&FreeSans18pt7b);
                display.setTextColor(GxEPD_DARKGREY);
                int16_t ux, uy; uint16_t uw, uh;
                display.getTextBounds("min", 0, 0, &ux, &uy, &uw, &uh);
                int16_t minBaseline = cy + 12;
                int16_t minCursor = COL_DEP_R - (int16_t)uw - ux;
                display.setCursor(minCursor, minBaseline);
                display.print("min");

                // FIX 7: numAnchorX is the right edge of the ETA number, which must sit
                // 6 px left of the "min" glyph's visual left edge.
                // The "min" glyph's visual left edge = minCursor + ux.
                // The old code used (uw + ux) which double-counted ux (bearing included
                // twice: once when computing minCursor, once here).
                int16_t numAnchorX  = minCursor + ux - 6;
                display.setFont(&FreeSansBold24pt7b);
                drawTextRightAligned(display, numAnchorX, minBaseline, r.etaNum, GxEPD_BLACK);

            } else {
                // "?" (NTP failed) — treat like "Now"
                display.setFont(&FreeSansBold24pt7b);
                drawTextRightAligned(display, COL_DEP_R, cy + 12, r.etaNum, GxEPD_BLACK);
            }

            // ---- Thin row separator (light grey, 1 px, between rows) ---------
            if (i < numRows - 1) {
                int16_t sep_y = rowAreaTop + (i + 1) * ROW_H;
                display.drawFastHLine(COL_ROUTE_X, sep_y,
                                      COL_DEP_R - COL_ROUTE_X, GxEPD_LIGHTGREY);
            }
        }

        // ---- Empty state -------------------------------------------------------
        if (numRows == 0) {
            display.setFont(&FreeSans9pt7b);
            display.setTextColor(GxEPD_BLACK);
            display.setCursor(COL_DEST_X, rowAreaTop + 50);
            display.print("No departures");
        }

        // ================================================================
        // SECTION 4 — Services-at-this-stop footer
        //
        // Footer is anchored at the BASE row top (not alert-shifted) so it
        // stays at a stable y position regardless of banner state.
        // ================================================================
        const int16_t fy = ROW_TOP_BASE + MAX_ROWS * ROW_H + 8;  // = FOOTER_STRIP_TOP = 682

        display.fillRect(0, fy, PANEL_W, FOOTER_STRIP_H, GxEPD_LIGHTGREY);

        display.setFont(&FreeSans9pt7b);
        display.setTextColor(GxEPD_DARKGREY);
        display.setCursor(COL_ROUTE_X, fy + 20);
        display.print("Services at this stop");

        // Collect unique route numbers from the rows we've drawn
        {
            char uniqueRoutes[3][12];
            int  nUnique = 0;
            for (int i = 0; i < numRows && nUnique < 3; i++) {
                bool found = false;
                for (int u = 0; u < nUnique; u++) {
                    if (strcmp(uniqueRoutes[u], rows[i].route) == 0) { found = true; break; }
                }
                if (!found) {
                    strncpy(uniqueRoutes[nUnique++], rows[i].route, 11);
                    uniqueRoutes[nUnique-1][11] = '\0';
                }
            }

            for (int k = 0; k < nUnique; k++) {
                int16_t chipLeft = COL_ROUTE_X + k * 72;
                int16_t chipCy   = fy + 56;
                display.fillRoundRect(chipLeft, chipCy - 16, 60, 32, 6, GxEPD_DARKGREY);
                display.setFont(&FreeSansBold12pt7b);
                display.setTextColor(GxEPD_WHITE);
                int16_t bx, by; uint16_t bw, bh;
                display.getTextBounds(uniqueRoutes[k], 0, 0, &bx, &by, &bw, &bh);
                int16_t tx = chipLeft + (60 - (int16_t)bw) / 2 - bx;
                display.setCursor(tx, chipCy + 8);
                display.print(uniqueRoutes[k]);
            }
        }

        display.setFont(&FreeSans9pt7b);
        display.setTextColor(GxEPD_DARKGREY);
        display.setCursor(COL_ROUTE_X, fy + 86);
        display.print("Real-time info");

        drawSignalArc(display, COL_ROUTE_X + 150, fy + 78, GxEPD_DARKGREY);

        // ================================================================
        // SECTION 5 — Bottom info bar (updated/stale badge + battery)
        // ================================================================
        constexpr int16_t INFO_Y  = 792;
        constexpr int16_t INFO_LH = 13;

        display.setFont(&FreeSans9pt7b);

        if (stale) {
            char staleBuf[48];
            snprintf(staleBuf, sizeof(staleBuf), "OFFLINE -- %s", updatedBuf);
            int16_t bx, by; uint16_t bw, bh;
            display.getTextBounds(staleBuf, 0, 0, &bx, &by, &bw, &bh);
            int16_t badgeX = COL_ROUTE_X - 4;
            int16_t badgeY = INFO_Y - INFO_LH - 4;
            int16_t badgeW = (int16_t)bw + 8;
            int16_t badgeH = INFO_LH + 8;
            if (badgeX + badgeW > PANEL_W - 2) badgeW = PANEL_W - 2 - badgeX;
            if (badgeW < 1) badgeW = 1;
            display.fillRoundRect(badgeX, badgeY, badgeW, badgeH, 3, GxEPD_DARKGREY);
            display.setTextColor(GxEPD_WHITE);
            display.setCursor(COL_ROUTE_X - bx, INFO_Y);
            display.print(staleBuf);
            display.setTextColor(GxEPD_DARKGREY);
        } else {
            display.setTextColor(GxEPD_DARKGREY);
            display.setCursor(COL_ROUTE_X, INFO_Y);
            display.print(updatedBuf);
        }

        // ---- Battery glyph (right-aligned to COL_DEP_R) ----------------------
        if (batteryPct >= 0) {
            int pct = batteryPct;
            if (pct > 100) pct = 100;
            if (pct < 0)   pct = 0;

            char pctBuf[8];
            snprintf(pctBuf, sizeof(pctBuf), "%d%%", pct);
            int16_t bx, by; uint16_t bw, bh;
            display.setFont(&FreeSans9pt7b);
            display.getTextBounds(pctBuf, 0, 0, &bx, &by, &bw, &bh);

            constexpr int16_t BATT_W    = 24;
            constexpr int16_t BATT_H    = 12;
            constexpr int16_t BATT_NUB  = 3;
            constexpr int16_t BATT_NUB_H= 6;
            constexpr int16_t BATT_GAP  = 4;
            int16_t totalW = BATT_W + BATT_NUB + BATT_GAP + (int16_t)bw;

            int16_t textRight  = COL_DEP_R;
            int16_t textCursor = textRight - (int16_t)bw - bx;
            int16_t glyphLeft  = textRight - (int16_t)totalW;

            int16_t glyphCY = INFO_Y - 7;  // FIX 6: centre on text cap-height (was INFO_Y - INFO_LH/2 = INFO_Y-6, ~2 px low)
            int16_t glyphY  = glyphCY - BATT_H / 2;

            if (glyphLeft < 0) glyphLeft = 0;

            display.drawRect(glyphLeft, glyphY, BATT_W, BATT_H, GxEPD_BLACK);
            int16_t nubY = glyphCY - BATT_NUB_H / 2;
            display.fillRect(glyphLeft + BATT_W, nubY, BATT_NUB, BATT_NUB_H, GxEPD_BLACK);

            int16_t fillMaxW = BATT_W - 2;
            int16_t fillW    = (int16_t)((fillMaxW * pct) / 100);
            if (fillW > 0) {
                uint16_t fillCol = (pct >= 20) ? GxEPD_DARKGREY : GxEPD_BLACK;
                display.fillRect(glyphLeft + 1, glyphY + 1, fillW, BATT_H - 2, fillCol);
            }

            display.setTextColor(GxEPD_DARKGREY);
            display.setCursor(textCursor, INFO_Y);
            display.print(pctBuf);
        }

    } while (display.nextPage());

    display.hibernate();
}

// =============================================================================
// drawSetupScreen()
//
// Renders the Wi-Fi captive-portal setup instructions on the e-ink panel.
// Called from main.cpp via the WiFiManager setAPCallback hook, i.e. right
// before the portal AP becomes active.
//
// Layout (portrait 480×800, same 4-grey aesthetic as drawBoard):
//
//   y=  0..80   — Light grey header bar
//   y= 30       — "Wi-Fi Setup" heading (FreeSansBold24pt7b, BLACK)
//
//   y=100..360  — Numbered steps block (dark grey rounded rect background)
//     y=130     — "1. On your phone, join the Wi-Fi network:"  (FreeSans12pt7b)
//     y=175     — <apName> in a dark-grey chip (FreeSansBold12pt7b, WHITE)
//     y=230     — "2. A setup page opens automatically — pick your"
//     y=258     —    "Wi-Fi and enter its password."  (FreeSans9pt7b)
//     y=310     — "3. This sign will connect and start automatically."
//                   (FreeSans9pt7b)
//
//   y=440..540  — Footer note block (light grey background)
//     y=470     — "If the page doesn't open, visit:"  (FreeSans9pt7b DARK)
//     y=500     — "http://<ip>" (FreeSansBold12pt7b BLACK centred)
//     y=530     — "in a browser on the same phone."  (FreeSans9pt7b DARK)
//
// Parameters:
//   display — reference to the GxEPD2_4G_4G<...> display object.
//   apName  — AP name to show (WIFI_PORTAL_AP_NAME from config.h).
//   ip      — captive portal IP address string (e.g. "192.168.4.1").
// =============================================================================
template <typename GxEPD2_Type>
void drawSetupScreen(GxEPD2_Type& display, const char* apName, const char* ip)
{
    char urlBuf[40];
    snprintf(urlBuf, sizeof(urlBuf), "http://%s", ip);

    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);

        display.fillRect(0, 0, PANEL_W, 82, GxEPD_LIGHTGREY);

        display.setFont(&FreeSansBold24pt7b);
        display.setTextColor(GxEPD_BLACK);
        {
            int16_t bx, by; uint16_t bw, bh;
            display.getTextBounds("Wi-Fi Setup", 0, 0, &bx, &by, &bw, &bh);
            int16_t tx = (PANEL_W - (int16_t)bw) / 2 - bx;
            display.setCursor(tx, 52);
            display.print("Wi-Fi Setup");
        }

        constexpr int16_t BLK_X  = 18;
        constexpr int16_t BLK_Y  = 96;
        constexpr int16_t BLK_W  = PANEL_W - 2 * BLK_X;
        constexpr int16_t BLK_H  = 295;
        constexpr int16_t BLK_R  = 12;
        display.fillRoundRect(BLK_X, BLK_Y, BLK_W, BLK_H, BLK_R, GxEPD_LIGHTGREY);

        constexpr int16_t STP_X  = BLK_X + 16;
        constexpr int16_t STP_MW = BLK_W - 32;

        display.setFont(&FreeSans12pt7b);
        display.setTextColor(GxEPD_BLACK);
        display.setCursor(STP_X, 130);
        display.print("1. On your phone, join:");

        {
            display.setFont(&FreeSansBold12pt7b);
            int16_t bx, by; uint16_t bw, bh;
            display.getTextBounds(apName, 0, 0, &bx, &by, &bw, &bh);

            int16_t chipW = (int16_t)bw + 28;
            if (chipW > STP_MW) chipW = STP_MW;
            int16_t chipH  = 40;
            int16_t chipX  = BLK_X + (BLK_W - chipW) / 2;
            int16_t chipCY = 172;

            display.fillRoundRect(chipX, chipCY - chipH / 2, chipW, chipH, 7, GxEPD_DARKGREY);

            display.setTextColor(GxEPD_WHITE);
            int16_t tx = chipX + (chipW - (int16_t)bw) / 2 - bx;
            display.setCursor(tx, chipCY + 8);
            display.print(apName);
        }

        display.setFont(&FreeSans9pt7b);
        display.setTextColor(GxEPD_BLACK);
        display.setCursor(STP_X, 222);
        display.print("2. A setup page opens automatically.");
        display.setCursor(STP_X, 242);
        display.print("   Pick your Wi-Fi and enter its password.");

        display.setCursor(STP_X, 278);
        display.print("3. This sign will connect and start");
        display.setCursor(STP_X, 298);
        display.print("   automatically.");

        display.drawFastHLine(BLK_X, 403, BLK_W, GxEPD_LIGHTGREY);

        display.fillRoundRect(BLK_X, 418, BLK_W, 108, BLK_R, GxEPD_LIGHTGREY);

        display.setFont(&FreeSans9pt7b);
        display.setTextColor(GxEPD_DARKGREY);
        display.setCursor(STP_X, 444);
        display.print("If the page doesn't open, visit:");

        display.setFont(&FreeSansBold12pt7b);
        display.setTextColor(GxEPD_BLACK);
        {
            int16_t bx, by; uint16_t bw, bh;
            display.getTextBounds(urlBuf, 0, 0, &bx, &by, &bw, &bh);
            int16_t tx = BLK_X + (BLK_W - (int16_t)bw) / 2 - bx;
            display.setCursor(tx, 474);
            display.print(urlBuf);
        }

        display.setFont(&FreeSans9pt7b);
        display.setTextColor(GxEPD_DARKGREY);
        display.setCursor(STP_X, 504);
        display.print("in a browser on the same phone.");

        {
            const char* hint = "Screen stays on until Wi-Fi is configured.";
            int16_t bx, by; uint16_t bw, bh;
            display.getTextBounds(hint, 0, 0, &bx, &by, &bw, &bh);
            int16_t tx = (PANEL_W - (int16_t)bw) / 2 - bx;
            display.setCursor(tx, 750);
            display.print(hint);
        }

    } while (display.nextPage());

    display.hibernate();
}

// =============================================================================
// drawNoDataScreen()
//
// Renders a minimal "no data / no connection" screen for the case where:
//   - a live fetch failed, AND
//   - there is no cached JSON in NVS (first boot or NVS erased).
// =============================================================================
template <typename GxEPD2_Type>
void drawNoDataScreen(GxEPD2_Type& display)
{
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);

        display.fillRect(0, 0, PANEL_W, 82, GxEPD_LIGHTGREY);

        display.setFont(&FreeSansBold24pt7b);
        display.setTextColor(GxEPD_BLACK);
        {
            const char* title = "Bus Departures";
            int16_t bx, by; uint16_t bw, bh;
            display.getTextBounds(title, 0, 0, &bx, &by, &bw, &bh);
            int16_t tx = (PANEL_W - (int16_t)bw) / 2 - bx;
            display.setCursor(tx, 52);
            display.print(title);
        }

        display.fillRoundRect(18, 150, PANEL_W - 36, 200, 12, GxEPD_LIGHTGREY);

        display.setFont(&FreeSansBold24pt7b);
        display.setTextColor(GxEPD_BLACK);
        {
            const char* msg = "No connection";
            int16_t bx, by; uint16_t bw, bh;
            display.getTextBounds(msg, 0, 0, &bx, &by, &bw, &bh);
            int16_t tx = (PANEL_W - (int16_t)bw) / 2 - bx;
            display.setCursor(tx, 220);
            display.print(msg);
        }

        display.setFont(&FreeSans12pt7b);
        display.setTextColor(GxEPD_DARKGREY);
        {
            const char* sub = "Waiting for data...";
            int16_t bx, by; uint16_t bw, bh;
            display.getTextBounds(sub, 0, 0, &bx, &by, &bw, &bh);
            int16_t tx = (PANEL_W - (int16_t)bw) / 2 - bx;
            display.setCursor(tx, 275);
            display.print(sub);
            const char* sub2 = "Will retry automatically.";
            display.getTextBounds(sub2, 0, 0, &bx, &by, &bw, &bh);
            tx = (PANEL_W - (int16_t)bw) / 2 - bx;
            display.setCursor(tx, 305);
            display.print(sub2);
        }

        display.setFont(&FreeSans9pt7b);
        display.setTextColor(GxEPD_DARKGREY);
        {
            const char* hint = "Check Wi-Fi credentials via the setup portal.";
            int16_t bx, by; uint16_t bw, bh;
            display.getTextBounds(hint, 0, 0, &bx, &by, &bw, &bh);
            int16_t tx = (PANEL_W - (int16_t)bw) / 2 - bx;
            display.setCursor(tx, 750);
            display.print(hint);
        }

    } while (display.nextPage());
    display.hibernate();
}

// =============================================================================
// drawVersionErrorScreen()  — Feature: v3 contract
//
// Called when JSON top-level "version" != 3.
// Renders a small centred notice, then the caller sleeps normally.
// =============================================================================
template <typename GxEPD2_Type>
void drawVersionErrorScreen(GxEPD2_Type& display)
{
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);

        // Light grey header strip
        display.fillRect(0, 0, PANEL_W, 82, GxEPD_LIGHTGREY);

        display.setFont(&FreeSansBold24pt7b);
        display.setTextColor(GxEPD_BLACK);
        {
            const char* title = "Bus Departures";
            int16_t bx, by; uint16_t bw, bh;
            display.getTextBounds(title, 0, 0, &bx, &by, &bw, &bh);
            display.setCursor((PANEL_W - (int16_t)bw) / 2 - bx, 52);
            display.print(title);
        }

        // Message block
        display.fillRoundRect(18, 140, PANEL_W - 36, 230, 12, GxEPD_LIGHTGREY);

        display.setFont(&FreeSansBold24pt7b);
        display.setTextColor(GxEPD_BLACK);
        {
            const char* msg = "Update needed";
            int16_t bx, by; uint16_t bw, bh;
            display.getTextBounds(msg, 0, 0, &bx, &by, &bw, &bh);
            display.setCursor((PANEL_W - (int16_t)bw) / 2 - bx, 210);
            display.print(msg);
        }

        display.setFont(&FreeSans12pt7b);
        display.setTextColor(GxEPD_DARKGREY);
        {
            const char* l1 = "Data format updated.";
            const char* l2 = "Please re-flash firmware.";
            int16_t bx, by; uint16_t bw, bh;
            display.getTextBounds(l1, 0, 0, &bx, &by, &bw, &bh);
            display.setCursor((PANEL_W - (int16_t)bw) / 2 - bx, 265);
            display.print(l1);
            display.getTextBounds(l2, 0, 0, &bx, &by, &bw, &bh);
            display.setCursor((PANEL_W - (int16_t)bw) / 2 - bx, 295);
            display.print(l2);
        }

        display.setFont(&FreeSans9pt7b);
        display.setTextColor(GxEPD_DARKGREY);
        {
            const char* hint = "Will retry automatically.";
            int16_t bx, by; uint16_t bw, bh;
            display.getTextBounds(hint, 0, 0, &bx, &by, &bw, &bh);
            display.setCursor((PANEL_W - (int16_t)bw) / 2 - bx, 750);
            display.print(hint);
        }

    } while (display.nextPage());
    display.hibernate();
}

// =============================================================================
// drawLowBatteryScreen()  — Feature 7
//
// "Plug me in" screen rendered when battery % <= BATT_CRITICAL_PCT.
// 4-grey aesthetic: white background, dark rounded-rect battery glyph block,
// centred message.  The device deep-sleeps for BATT_PROTECT_SLEEP_MIN after.
// =============================================================================
template <typename GxEPD2_Type>
void drawLowBatteryScreen(GxEPD2_Type& display, int batteryPct)
{
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);

        // Light grey header
        display.fillRect(0, 0, PANEL_W, 82, GxEPD_LIGHTGREY);

        display.setFont(&FreeSansBold24pt7b);
        display.setTextColor(GxEPD_BLACK);
        {
            const char* title = "Bus Departures";
            int16_t bx, by; uint16_t bw, bh;
            display.getTextBounds(title, 0, 0, &bx, &by, &bw, &bh);
            display.setCursor((PANEL_W - (int16_t)bw) / 2 - bx, 52);
            display.print(title);
        }

        // Central message block (light grey pill)
        display.fillRoundRect(18, 160, PANEL_W - 36, 320, 16, GxEPD_LIGHTGREY);

        // Battery-empty glyph — large outline rect + nub, centred at y≈270
        {
            constexpr int16_t GW = 80;   // glyph body width
            constexpr int16_t GH = 40;   // glyph body height
            constexpr int16_t GN =  8;   // nub width
            constexpr int16_t GNH= 20;   // nub height
            constexpr int16_t GR =  4;   // border thickness

            int16_t gx = (PANEL_W - GW - GN) / 2;  // left edge of body
            int16_t gy = 205;                        // top of body

            // Outer outline (dark, 4 px thick via nested rects)
            display.fillRoundRect(gx, gy, GW, GH, 5, GxEPD_DARKGREY);
            display.fillRoundRect(gx + GR, gy + GR, GW - 2*GR, GH - 2*GR, 3, GxEPD_WHITE);
            // Nub
            display.fillRect(gx + GW, gy + (GH - GNH)/2, GN, GNH, GxEPD_DARKGREY);

            // Single thin fill bar at left edge — very low charge indicator (dark)
            constexpr int16_t fillW = 8;
            display.fillRect(gx + GR, gy + GR, fillW, GH - 2*GR, GxEPD_DARKGREY);
        }

        // "Plug me in" heading
        display.setFont(&FreeSansBold24pt7b);
        display.setTextColor(GxEPD_BLACK);
        {
            const char* msg = "Plug me in";
            int16_t bx, by; uint16_t bw, bh;
            display.getTextBounds(msg, 0, 0, &bx, &by, &bw, &bh);
            display.setCursor((PANEL_W - (int16_t)bw) / 2 - bx, 310);
            display.print(msg);
        }

        // Sub-message
        display.setFont(&FreeSans12pt7b);
        display.setTextColor(GxEPD_DARKGREY);
        {
            const char* l1 = "Battery critically low.";
            int16_t bx, by; uint16_t bw, bh;
            display.getTextBounds(l1, 0, 0, &bx, &by, &bw, &bh);
            display.setCursor((PANEL_W - (int16_t)bw) / 2 - bx, 358);
            display.print(l1);

            char pctBuf[48];
            if (batteryPct >= 0) {
                snprintf(pctBuf, sizeof(pctBuf), "Battery: %d%%  Please charge now.", batteryPct);
            } else {
                strncpy(pctBuf, "Please connect a charger.", sizeof(pctBuf) - 1);
                pctBuf[sizeof(pctBuf)-1] = '\0';
            }
            display.getTextBounds(pctBuf, 0, 0, &bx, &by, &bw, &bh);
            display.setCursor((PANEL_W - (int16_t)bw) / 2 - bx, 388);
            display.print(pctBuf);
        }

        // Footer hint
        display.setFont(&FreeSans9pt7b);
        display.setTextColor(GxEPD_DARKGREY);
        {
            const char* hint = "Sign will sleep to protect the battery.";
            int16_t bx, by; uint16_t bw, bh;
            display.getTextBounds(hint, 0, 0, &bx, &by, &bw, &bh);
            display.setCursor((PANEL_W - (int16_t)bw) / 2 - bx, 750);
            display.print(hint);
        }

    } while (display.nextPage());
    display.hibernate();
}
