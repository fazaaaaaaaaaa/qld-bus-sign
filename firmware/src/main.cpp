// =============================================================================
// main.cpp — QLD Bus Departure Sign — Xteink X4 Firmware v3 (JSON/NTP edition)
//
// One wake cycle:
//   1. Init Serial, SPI, e-ink panel.
//   2. Connect Wi-Fi (via captive-portal setup on first boot, or saved creds).
//   3. Sync NTP (get accurate UTC clock).
//   4. HTTPS GET {DATA_URL}?t=<epoch> → parse departures.json v3 with ArduinoJson.
//   5. Version check (v!=3 → show notice, sleep).
//   6. Battery critical check (Feature 7 — "Plug me in" screen).
//   7. Stop selection (Feature 5 — time-of-day, §D).
//   8. Content hash (Feature 1a — skip redraw if unchanged).
//   9. OTA update check (Feature 4 — from firmware block in JSON).
//  10. Render departure board on-device (board_render.h).
//  11. Deep-sleep for REFRESH_MINUTES, then restart from step 1.
//
// Contract v3 JSON shape:
//   { version, generated_at, utc_offset_seconds,
//     firmware?: { version, bin_url, sha256 },
//     alerts: [ { id, header, severity, effect, routes } ],
//     stops: [ { stop_id, stop_label, departures: [ { route, dest, time, live, cancelled } ] } ]
//   }
//
// All QoL features from v2 are PRESERVED:
//   - WiFi portal (configures data_url, refresh_m, rotation, stop_switch_h)
//   - NVS cache for offline/stale rendering
//   - Battery read + glyph
//   - Smart sleep (late-night + no-service intervals)
// =============================================================================

// ---- Arduino-ESP32 / IDF headers -------------------------------------------
#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiMulti.h>          // multi-network auto-join (v3.1; in arduino-esp32 core)
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_sleep.h>
#include <esp_sntp.h>
#include "esp_netif.h"          // explicit DNS resolver override (applyFallbackDNS)
#include <Preferences.h>

// ---- OTA (Feature 4) -------------------------------------------------------
// Update.h and mbedTLS are both in the ESP32 Arduino core (no lib_deps needed).
#include <Update.h>
#include "mbedtls/sha256.h"     // mbedtls_sha256_{init,starts,update,finish,free}

// ---- WiFiManager (tzapu) ---------------------------------------------------
#include <WiFiManager.h>

// ---- ArduinoJson v7 --------------------------------------------------------
#include <ArduinoJson.h>

// ---- GxEPD2_4G display driver (4-level greyscale) --------------------------
#include <GxEPD2_4G_4G.h>

// ---- Project headers -------------------------------------------------------
#include "config.h"
#include "board_render.h"

// =============================================================================
// Display object — 4-level greyscale
// =============================================================================
#define GxEPD2_DRIVER_CLASS  GxEPD2_426_GDEQ0426T82
#define MAX_PAGE_HEIGHT       8    // rows per GxEPD2_4G internal stripe (2 bpp → 1600 B/page)

GxEPD2_4G_4G<GxEPD2_426_GDEQ0426T82, MAX_PAGE_HEIGHT>
    display(GxEPD2_426_GDEQ0426T82(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

// =============================================================================
// RTC_DATA_ATTR — survives deep sleep, reset to 0 on power-on reset.
//
// wifiFailCount:      consecutive Wi-Fi failures (triggers portal re-open).
// fullRefreshCounter: counts wakes since last full EPD refresh (Feature 1b).
//                     Resets to 0 after every forced full refresh.
//                     When it reaches FULL_REFRESH_EVERY, a full refresh is forced.
// =============================================================================
RTC_DATA_ATTR static int  wifiFailCount       = 0;
RTC_DATA_ATTR static int  fullRefreshCounter  = 0;  // Feature 1b: ghost-clear cycle counter
RTC_DATA_ATTR static bool panelEverPainted    = false;  // FIX 1: true once the panel has been painted at least once

// Feature 8 (v3.2): manual stop override set by a LEFT/RIGHT button press.
//   -1 = none (use normal time-of-day selection)
//    0 = force HOME_STOP_ID  (LEFT  — morning/home stop, route 320 from Bonney Ave)
//    1 = force CITY_STOP_ID  (RIGHT — return/city stop)
// Stored in RTC memory so a press that WAKES the device survives into the stop-
// selection block this same cycle.  It is cleared back to -1 immediately after
// being applied, so the NEXT scheduled (timer) wake returns to normal behaviour.
RTC_DATA_ATTR static int  manualStopOverride  = -1;

// =============================================================================
// g_pSetupDisplay — file-scope pointer used by the WiFiManager AP callback.
// =============================================================================
static GxEPD2_4G_4G<GxEPD2_426_GDEQ0426T82, MAX_PAGE_HEIGHT>* g_pSetupDisplay = nullptr;

// =============================================================================
// Captive-portal branding (v3.1, Feature B)
//
// PORTAL_HEAD — a compact, self-contained stylesheet injected into every
//   WiFiManager page via wm.setCustomHeadElement().  No external resources
//   (no web fonts / CDN) so it works offline on the device's own AP.
//   Translink-style maroon accent (#9e0b0f), dark text on light cards,
//   rounded 10px touch-friendly inputs/buttons, centered max-width ~480px.
//   Kept well under ~2 KB.
//
// g_portalMenuHtml — buffer for wm.setCustomMenuHTML().  WiFiManager stores the
//   const char* pointer WITHOUT copying, so this MUST be a file-scope static
//   that outlives the portal (a stack temporary would dangle).  It is filled in
//   setup() just before startConfigPortal() with the branded header plus the
//   list of currently-remembered SSIDs.
// =============================================================================
static const char PORTAL_HEAD[] =
  "<style>"
  ":root{--mar:#9e0b0f;--ink:#1b1b1b;--bg:#f3f4f6;--card:#ffffff;--line:#d9dbe0;}"
  "*{box-sizing:border-box;}"
  "body{margin:0;padding:16px 12px;background:var(--bg);color:var(--ink);"
  "font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;"
  "font-size:16px;line-height:1.45;}"
  ".wrap,body>div{max-width:480px;margin:0 auto;}"
  "h1,h2,h3{color:var(--mar);font-weight:700;margin:.2em 0 .5em;}"
  "div,form{background:var(--card);border:1px solid var(--line);border-radius:12px;"
  "padding:16px;margin:12px auto;max-width:480px;}"
  "input,button,select{width:100%;padding:13px 12px;margin:7px 0;border-radius:10px;"
  "border:1px solid var(--line);font-size:16px;background:#fff;color:var(--ink);}"
  "input:focus,select:focus{outline:none;border-color:var(--mar);}"
  "button,input[type='submit']{background:var(--mar);color:#fff;border:0;font-weight:700;"
  "cursor:pointer;min-height:48px;}"
  "button:hover,input[type='submit']:hover{filter:brightness(1.08);}"
  "a{color:var(--mar);}"
  ".qbs-hdr{font-size:18px;font-weight:700;color:var(--mar);}"
  ".qbs-sub{font-size:14px;color:#555;}"
  "</style>";

static char g_portalMenuHtml[640];   // lives for the portal's lifetime (see note above)

// =============================================================================
// NVS namespace: "busign"
//
// Keys:
//   "data_url"      (String)  — HTTPS URL for departures.json fetch
//   "refresh_m"     (Int)     — refresh interval minutes 1–60
//   "rotation"      (Int)     — EPD rotation 0–3
//   "stop_switch_h" (Int)     — local hour 0–23 for stop switch (Feature 5)
//   "last_json"     (String)  — last successfully fetched JSON body (offline cache)
//   "last_epoch"    (ULong)   — UTC epoch of last successful fetch (offline cache)
//   "last_hash"     (ULong)   — FNV-1a hash of last rendered content (Feature 1a)
//   "last_stop_id"  (String)  — stop_id rendered last wake (layout-change detect)
//   "last_has_alert"(Int)     — 1 if last wake had alert banner (layout-change detect)
//   "last_stale"    (Int)     — 1 if last wake was stale (layout-change detect)
//   "wnet_cnt"      (Int)     — number of remembered Wi-Fi networks (0..MAX_WIFI_NETS) (v3.1)
//   "wnet_ssid{i}"  (String)  — SSID  of remembered network i (0-based) (v3.1)
//   "wnet_pass{i}"  (String)  — pass  of remembered network i (0-based) (v3.1)
// =============================================================================

// =============================================================================
// Runtime settings — loaded from NVS on each boot.
// =============================================================================
static String gDataUrl;        // effective DATA_URL
static int    gRefreshMin;     // effective REFRESH_MINUTES
static int    gRotation;       // effective EPD_ROTATION
static int    gStopSwitchHour; // effective STOP_SWITCH_HOUR (Feature 5)

// =============================================================================
// loadRuntimeSettings()
// =============================================================================
static void loadRuntimeSettings(Preferences& prefs)
{
    gDataUrl        = prefs.getString("data_url",      DATA_URL);
    gRefreshMin     = prefs.getInt("refresh_m",        REFRESH_MINUTES);
    gRotation       = prefs.getInt("rotation",         EPD_ROTATION);
    gStopSwitchHour = prefs.getInt("stop_switch_h",    STOP_SWITCH_HOUR);

    if (gRefreshMin < 1 || gRefreshMin > 60)       gRefreshMin     = REFRESH_MINUTES;
    if (gRotation < 0   || gRotation   > 3)        gRotation       = EPD_ROTATION;
    if (gDataUrl.length() == 0)                    gDataUrl        = DATA_URL;
    if (gStopSwitchHour < 0 || gStopSwitchHour > 23) gStopSwitchHour = STOP_SWITCH_HOUR;

    // v3.2.2: one-time migration of existing devices off the old GitHub Pages
    // feed onto the new default (Cloudflare Worker) so reliable refresh kicks in
    // without re-running the setup portal.  Only rewrites the exact old URL.
    if (gDataUrl == "https://fazaaaaaaaaaa.github.io/qld-bus-sign/departures.json") {
        gDataUrl = DATA_URL;
        prefs.putString("data_url", gDataUrl);
        Serial.println("[NVS]  data_url migrated to new default (Cloudflare Worker)");
    }

    Serial.printf("[NVS]  data_url=%s  refresh=%d min  rotation=%d  stop_switch_h=%d\n",
                  gDataUrl.c_str(), gRefreshMin, gRotation, gStopSwitchHour);
}

// =============================================================================
// Multi-network Wi-Fi memory (v3.1)
//
// The sign remembers up to MAX_WIFI_NETS (SSID, password) pairs in NVS so it can
// auto-join whichever remembered network is in range (via WiFiMulti).  Schema:
//   "wnet_cnt"      (Int)     — count, clamped 0..MAX_WIFI_NETS
//   "wnet_ssid{i}"  (String)  — SSID  of entry i (0-based)
//   "wnet_pass{i}"  (String)  — pass  of entry i (0-based)
//
// loadWifiNetworks(): read the stored list into caller arrays; returns count.
// saveWifiNetworks(): persist a list of n entries (and the count).
// addWifiNetwork():   load -> update-if-present / append / evict-oldest -> save.
// =============================================================================
static int loadWifiNetworks(Preferences& p, String ssids[], String passs[], int maxN)
{
    int cnt = p.getInt("wnet_cnt", 0);
    if (cnt < 0)      cnt = 0;
    if (cnt > maxN)   cnt = maxN;            // never read past caller's buffers
    if (cnt > MAX_WIFI_NETS) cnt = MAX_WIFI_NETS;

    char keyS[16], keyP[16];
    for (int i = 0; i < cnt; i++) {
        snprintf(keyS, sizeof(keyS), "wnet_ssid%d", i);
        snprintf(keyP, sizeof(keyP), "wnet_pass%d", i);
        ssids[i] = p.getString(keyS, "");
        passs[i] = p.getString(keyP, "");
    }
    return cnt;
}

static void saveWifiNetworks(Preferences& p, const String ssids[], const String passs[], int n)
{
    if (n < 0)              n = 0;
    if (n > MAX_WIFI_NETS)  n = MAX_WIFI_NETS;

    char keyS[16], keyP[16];
    // Write the kept entries.
    for (int i = 0; i < n; i++) {
        snprintf(keyS, sizeof(keyS), "wnet_ssid%d", i);
        snprintf(keyP, sizeof(keyP), "wnet_pass%d", i);
        p.putString(keyS, ssids[i]);
        p.putString(keyP, passs[i]);
    }
    // Remove any stale higher-index entries left over from a previously longer list.
    for (int i = n; i < MAX_WIFI_NETS; i++) {
        snprintf(keyS, sizeof(keyS), "wnet_ssid%d", i);
        snprintf(keyP, sizeof(keyP), "wnet_pass%d", i);
        p.remove(keyS);
        p.remove(keyP);
    }
    p.putInt("wnet_cnt", n);
    Serial.printf("[NVS]  Saved %d remembered Wi-Fi network(s)\n", n);
}

static void addWifiNetwork(Preferences& p, const String& ssid, const String& pass)
{
    if (ssid.length() == 0) {                // ignore empty SSID
        Serial.println("[NVS]  addWifiNetwork: empty SSID — ignored");
        return;
    }

    String ssids[MAX_WIFI_NETS];
    String passs[MAX_WIFI_NETS];
    int n = loadWifiNetworks(p, ssids, passs, MAX_WIFI_NETS);

    // If already present: only write when the password actually changed to a
    // new non-empty value.  This keeps the "remember on every connect" refresh
    // call cheap (no NVS wear) and prevents a momentarily-empty psk() from
    // clobbering a good stored password.
    for (int i = 0; i < n; i++) {
        if (ssids[i] == ssid) {
            if (pass.length() == 0 || passs[i] == pass) {
                return;                       // unchanged / nothing safe to update
            }
            passs[i] = pass;
            saveWifiNetworks(p, ssids, passs, n);
            Serial.printf("[NVS]  Updated remembered network \"%s\"\n", ssid.c_str());
            return;
        }
    }

    if (n < MAX_WIFI_NETS) {
        // Room to append.
        ssids[n] = ssid;
        passs[n] = pass;
        n++;
    } else {
        // Full — drop oldest (index 0), shift down, append new at the end.
        for (int i = 1; i < MAX_WIFI_NETS; i++) {
            ssids[i - 1] = ssids[i];
            passs[i - 1] = passs[i];
        }
        ssids[MAX_WIFI_NETS - 1] = ssid;
        passs[MAX_WIFI_NETS - 1] = pass;
        n = MAX_WIFI_NETS;
        Serial.println("[NVS]  Network list full — evicted oldest entry");
    }

    saveWifiNetworks(p, ssids, passs, n);
    Serial.printf("[NVS]  Remembered new network \"%s\" (%d total)\n", ssid.c_str(), n);
}

// =============================================================================
// applyFallbackDNS() — force a public DNS resolver (8.8.8.8 / 1.1.1.1).
//
// Some routers hand out a DHCP lease with no usable DNS server (or one the
// device can't reach), which makes EVERY hostname lookup fail
// ("hostByName(): DNS Failed for ...") even though Wi-Fi is connected and an IP
// was assigned — NTP and the departures fetch both die with that.  Setting the
// resolver explicitly after the IP is up sidesteps a broken router DNS while
// keeping the DHCP-assigned IP/gateway.  Call once, right after Wi-Fi connects.
// =============================================================================
static void applyFallbackDNS()
{
    esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta == nullptr) {
        Serial.println("[DNS]  STA netif not found — leaving resolver as-is");
        return;
    }

    esp_netif_dns_info_t mainDns = {};
    mainDns.ip.type            = ESP_IPADDR_TYPE_V4;
    mainDns.ip.u_addr.ip4.addr = esp_ip4addr_aton("8.8.8.8");
    esp_netif_set_dns_info(sta, ESP_NETIF_DNS_MAIN, &mainDns);

    esp_netif_dns_info_t backupDns = {};
    backupDns.ip.type            = ESP_IPADDR_TYPE_V4;
    backupDns.ip.u_addr.ip4.addr = esp_ip4addr_aton("1.1.1.1");
    esp_netif_set_dns_info(sta, ESP_NETIF_DNS_BACKUP, &backupDns);

    Serial.println("[DNS]  Resolver forced to 8.8.8.8 / 1.1.1.1");
}

// =============================================================================
// savePortalSettings()
// =============================================================================
static void savePortalSettings(Preferences& prefs,
                                const char* urlVal,
                                const char* refreshVal,
                                const char* rotationVal,
                                const char* stopSwitchVal)
{
    // ---- Data URL -----------------------------------------------------------
    String newUrl(urlVal);
    newUrl.trim();
    if (newUrl.length() > 8 && newUrl.startsWith("https://")) {
        prefs.putString("data_url", newUrl);
        gDataUrl = newUrl;
        Serial.printf("[NVS]  Saved data_url=%s\n", newUrl.c_str());
    } else {
        Serial.printf("[NVS]  Invalid URL '%s' — keeping previous\n", urlVal);
    }

    // ---- Refresh minutes ---------------------------------------------------
    int newRefresh = String(refreshVal).toInt();
    if (newRefresh >= 1 && newRefresh <= 60) {
        prefs.putInt("refresh_m", newRefresh);
        gRefreshMin = newRefresh;
        Serial.printf("[NVS]  Saved refresh_m=%d\n", newRefresh);
    } else {
        Serial.printf("[NVS]  Invalid refresh '%s' — keeping previous\n", refreshVal);
    }

    // ---- Rotation ----------------------------------------------------------
    int newRot = String(rotationVal).toInt();
    if (newRot >= 0 && newRot <= 3) {
        prefs.putInt("rotation", newRot);
        gRotation = newRot;
        Serial.printf("[NVS]  Saved rotation=%d\n", newRot);
    } else {
        Serial.printf("[NVS]  Invalid rotation '%s' — keeping previous\n", rotationVal);
    }

    // ---- Stop switch hour (Feature 5) --------------------------------------
    int newHour = String(stopSwitchVal).toInt();
    if (newHour >= 0 && newHour <= 23) {
        prefs.putInt("stop_switch_h", newHour);
        gStopSwitchHour = newHour;
        Serial.printf("[NVS]  Saved stop_switch_h=%d\n", newHour);
    } else {
        Serial.printf("[NVS]  Invalid stop_switch_h '%s' — keeping previous\n", stopSwitchVal);
    }
}

// =============================================================================
// showStatusMessage() — draw a short status string on the panel.
// =============================================================================
static void showStatusMessage(const char* line1, const char* line2 = nullptr)
{
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.setTextColor(GxEPD_BLACK);
        display.setFont(nullptr);
        display.setTextSize(2);
        display.setCursor(20, 40);
        display.print(line1);
        if (line2) {
            display.setCursor(20, 80);
            display.print(line2);
        }
        display.setTextSize(1);
        display.setCursor(20, 130);
        display.printf("Retrying in %d min",
                       (gRefreshMin > 0) ? gRefreshMin : REFRESH_MINUTES);
    } while (display.nextPage());
    display.hibernate();
}

// =============================================================================
// Feature 8 (v3.2) — Physical LEFT / RIGHT button helpers
//
// Hardware recap (see config.h FEATURE 8 for full notes):
//   - LEFT and RIGHT are read from a resistor ladder on ADC pin GPIO1
//     (BTN_ADC_LEFT_PIN == BTN_ADC_RIGHT_PIN == 1).  Idle rests HIGH (~3632 on
//     the 12-bit scale); LEFT pulls it to ~1380, RIGHT pulls it to ~4.
//   - POWER (GPIO3) is a direct active-LOW digital pin, used only as an extra
//     deep-sleep wake source.
//
// ButtonId — what readButton() returns.
// =============================================================================
enum ButtonId { BTN_NONE = 0, BTN_LEFT = 1, BTN_RIGHT = 2 };

// readButton() — sample the ADC ladder and classify LEFT / RIGHT / none.
//
// Debounce: take a few quick reads and require the SAME classification twice in
// a row before accepting it (cheap rejection of a transient mid-press voltage
// while the ladder settles).  Always prints the raw ADC values so the user can
// calibrate the windows in config.h by watching the serial monitor.
//
// Returns BTN_LEFT / BTN_RIGHT / BTN_NONE.
static ButtonId classifyAdc(int adc)
{
    if (adc >= BTN_RIGHT_ADC_MIN && adc <= BTN_RIGHT_ADC_MAX) return BTN_RIGHT;
    if (adc >= BTN_LEFT_ADC_MIN  && adc <= BTN_LEFT_ADC_MAX)  return BTN_LEFT;
    return BTN_NONE;
}

static ButtonId readButton()
{
    // BTN_ADC_LEFT_PIN and BTN_ADC_RIGHT_PIN are the same physical pin on the X4
    // (GPIO1).  Read each (in case a future board splits them) and prefer a
    // definite LEFT/RIGHT over none.  adc2 is the second ladder pin (Up/Down);
    // it carries no LEFT/RIGHT info but is printed to aid calibration/debug.
    ButtonId result = BTN_NONE;
    int adc1 = 0, adc1b = 0, adc2 = 0;

    for (int i = 0; i < 3 && result == BTN_NONE; i++) {
        adc1  = analogRead(BTN_ADC_LEFT_PIN);
        adc1b = (BTN_ADC_RIGHT_PIN != BTN_ADC_LEFT_PIN)
                    ? analogRead(BTN_ADC_RIGHT_PIN) : adc1;
        adc2  = analogRead(BTN_ADC_2_PIN);

        ButtonId c1 = classifyAdc(adc1);
        ButtonId c2 = classifyAdc(adc1b);
        ButtonId cand = (c1 != BTN_NONE) ? c1 : c2;

        if (cand != BTN_NONE) {
            // Debounce: confirm with one more read of the LEFT pin.
            delay(8);
            int adcConfirm = analogRead(BTN_ADC_LEFT_PIN);
            if (classifyAdc(adcConfirm) == cand) {
                result = cand;
            }
        } else {
            delay(4);
        }
    }

    const char* nm = (result == BTN_LEFT) ? "LEFT"
                   : (result == BTN_RIGHT) ? "RIGHT" : "none";
    Serial.printf("[BTN] adc1=%4d adc2=%4d -> %s\n", adc1, adc2, nm);
    return result;
}

// armButtonWakeup() — enable deep-sleep wake on the button pins, in ADDITION to
// the timer wake that goToSleep() always configures.
//
// DESIGN / TRADEOFF (documented per requirement):
//   The LEFT/RIGHT buttons sit on an ADC resistor ladder (GPIO1) that rests
//   HIGH (~2.9 V) when idle and is pulled DOWN by a press (LEFT→~1.1 V,
//   RIGHT→~0 V — both well below the digital-HIGH threshold).  A deep-sleep
//   GPIO wake treats the pad digitally, so a LOW-level wake on GPIO1 fires when
//   ANY ladder button (Left/Right/Back/Confirm) is pressed.  That is exactly
//   what we want for "wake on a button": the wake just needs to start a fresh
//   cycle; readButton() then disambiguates LEFT vs RIGHT from the ADC value.
//
//   Resistor note: for a LOW-level wake the IDF auto-resistor feature
//   (ESP_SLEEP_GPIO_ENABLE_INTERNAL_RESISTORS, on by default) adds an internal
//   pull-UP to define the idle state as HIGH.  That does NOT fight our wiring —
//   the external ladder already rests GPIO1 HIGH and GPIO3 has a hardware
//   pull-up, so the internal pull-up merely reinforces idle-HIGH while a press
//   still pulls the pin firmly LOW through the ladder.  We therefore leave the
//   default behaviour in place (Espressif recommends an external pull-up for
//   low-level wake; the ladder/power-button provide one).
//
//   GPIO3 (POWER, active-LOW, direct) is added as a SECOND low-level wake pin
//   for robustness.  A POWER-button wake produces no LEFT/RIGHT ADC signature,
//   so on that wake readButton() returns none and the device simply refreshes
//   the time-of-day stop — a harmless, useful "wake & refresh now" gesture.
//
//   GPIO1/GPIO3 are both RTC-capable (RTC GPIOs are 0..5 on the C3), so both
//   qualify for esp_deep_sleep_enable_gpio_wakeup().  The timer wake remains
//   enabled alongside, so the normal REFRESH_MINUTES schedule is untouched.
static void armButtonWakeup()
{
    // v3.2.1: deep-sleep GPIO button wake is DISABLED for stability.
    // On the real X4 unit the GPIO1 ladder idle level / ADC thresholds did not
    // match the CircuitPython reference values, so a LOW-level wake on GPIO1
    // fired spuriously (and the post-render poll saw phantom presses) — which
    // made the device restart-loop into the WiFi portal.  Until the buttons are
    // calibrated on-device (watch the "[BTN] adc1=.. adc2=.." serial readout and
    // report the values), we wake on the TIMER only, exactly like the stable
    // v3.1 — so the sign cannot loop.  No GPIO wake is armed here on purpose.
    (void)0;
}

// =============================================================================
// goToSleep()
// =============================================================================
static void goToSleep(int sleepMin = 0)
{
    if (sleepMin <= 0) {
        sleepMin = (gRefreshMin > 0) ? gRefreshMin : REFRESH_MINUTES;
    }
    uint64_t sleep_us = (uint64_t)sleepMin * 60ULL * 1000000ULL;
    Serial.printf("[SLEEP] Deep-sleeping for %d min (%llu us)\n",
                  sleepMin, (unsigned long long)sleep_us);
    Serial.flush();
    esp_sleep_enable_timer_wakeup(sleep_us);
    armButtonWakeup();   // Feature 8: ADD button wake alongside the timer wake
    esp_deep_sleep_start();
}

// =============================================================================
// Battery helpers (Feature 7 + existing monitor)
// =============================================================================
#if ENABLE_BATTERY_MONITOR
static int readBatteryPct()
{
    uint32_t adcMv  = (uint32_t)analogReadMilliVolts(0);
    uint32_t vbatMv = (uint32_t)((float)adcMv * VBAT_DIVIDER_RATIO);

    Serial.printf("[BATT] Raw ADC: %lu mV  Vbat: %lu mV  (ratio=%.2f)\n",
                  (unsigned long)adcMv, (unsigned long)vbatMv,
                  (float)VBAT_DIVIDER_RATIO);

    if (vbatMv == 0) return -1;

    if (vbatMv <= VBAT_LIPO_MIN_MV) return 0;
    if (vbatMv >= VBAT_LIPO_MAX_MV) return 100;

    int pct = (int)(((float)(vbatMv - VBAT_LIPO_MIN_MV) /
                     (float)(VBAT_LIPO_MAX_MV - VBAT_LIPO_MIN_MV)) * 100.0f);
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}
#endif // ENABLE_BATTERY_MONITOR

// =============================================================================
// FNV-1a 32-bit hash — Feature 1a (change-skip)
//
// fnv1a_update(): feed bytes one at a time or block at a time.
// Produces a 32-bit hash over: selected stop_id + each visible departure's
// route/dest/etaNum + alert header concatenation + stale flag + battery bucket.
// The hash is deterministic across boots for the same content.
//
// FNV-1a (offset_basis=2166136261, prime=16777619) — public domain.
// =============================================================================
static inline uint32_t fnv1a_update(uint32_t h, const uint8_t* data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        h ^= (uint32_t)data[i];
        h *= 16777619UL;
    }
    return h;
}

static inline uint32_t fnv1a_str(uint32_t h, const char* s)
{
    if (!s) return h;
    return fnv1a_update(h, (const uint8_t*)s, strlen(s));
}

// computeContentHash(): build a FNV-1a hash over the content that will be
// rendered on the board.  Called before AND after any possible OTA so we
// hash the content the device is about to show, not the raw JSON.
//
// Inputs:
//   selectedStopId   — selected stop's stop_id string
//   departures       — JsonArrayConst for the selected stop
//   alerts           — top-level alerts array
//   now_utc          — NTP epoch (used for etaNum computation)
//   hasAlerts        — whether alerts banner will show (SHOW_ALERTS gate)
//   stale            — true if data from NVS cache
//   batteryPct       — -1 or actual %; hashed in 10% buckets to avoid
//                      frequent trivial changes
//
// We compute etaNum for each departure so the hash changes on minute boundaries
// (when a departure crosses "Now" or disappears), matching what the display shows.
//
// v3.3.0 (Feature A — "always looks alive"): we ALSO fold the current LOCAL
// MINUTE into the hash.  The live "Updated H:MMam" footer line ticks every
// minute, so each new minute must count as "changed" and force a redraw — even
// when the departures content is byte-for-byte identical.  Without this term, a
// quiet evening with no departure changes would skip the redraw (Feature 1a) and
// the on-screen time would freeze, making the sign look dead.
//   TRADE-OFF: this makes the panel do (at most) one extra partial refresh per
//   minute when nothing else changed.  That is fine for an always-on / USB sign
//   (the whole point of the live clock) and the periodic FULL refresh / ghost-
//   clear cadence (FULL_REFRESH_EVERY) is unchanged, so panel health is intact.
//   Only the partial-update skip decision is affected.
//   utc_offset is passed for parity with the display: the local-minute value and
//   the far-future clock-time etaNum are both computed with the same offset.
static uint32_t computeContentHash(const char*    selectedStopId,
                                   JsonArrayConst departures,
                                   JsonArrayConst alerts,
                                   time_t         now_utc,
                                   bool           hasAlerts,
                                   bool           stale,
                                   int            batteryPct,
                                   int32_t        utc_offset)
{
    uint32_t h = 2166136261UL;  // FNV-1a offset basis

    // Stop identity
    h = fnv1a_str(h, selectedStopId);

    // v3.3.0 (Feature A): current LOCAL minute — ticks the hash once a minute so
    // the live "Updated H:MMam" line repaints and the sign always looks alive.
    // Only meaningful when NTP succeeded (now_utc > 0); when NTP failed the time
    // shows "Updated --:--" and there is nothing to tick, so we skip this term
    // (preserving the pre-v3.3.0 skip-when-unchanged behaviour on NTP failure).
    if (now_utc > 0) {
        int64_t localMinute = ((int64_t)now_utc + (int64_t)utc_offset) / 60LL;  // e.g. (now_utc+utc_offset)/60
        h = fnv1a_update(h, (const uint8_t*)&localMinute, sizeof(localMinute));
    }

    // Departure rows (matching the effectiveMaxRows logic in drawBoard)
    const int effectiveMaxRows = hasAlerts ? (MAX_ROWS - 1) : MAX_ROWS;
    int rowsAdded = 0;
    for (JsonObjectConst dep : departures) {
        if (rowsAdded >= effectiveMaxRows) break;

        const char* route     = dep["route"]     | "";
        const char* dest      = dep["dest"]      | "";
        time_t      dep_time  = (time_t)dep["time"].as<long long>();
        bool        cancelled = dep["cancelled"] | false;

        char etaNum[8], etaUnit[8];
        bool isGone = false;
        // Reuse etaString from board_render.h — it is a static function in the
        // header so it is visible here (header included in main.cpp via board_render.h).
        // v3.3.0 (Feature B): pass utc_offset so a far-future departure's hashed
        // etaNum is the SAME clock-time string the board renders (e.g. "6:02a").
        etaString(dep_time, now_utc, cancelled,
                  etaNum,  sizeof(etaNum),
                  etaUnit, sizeof(etaUnit),
                  &isGone,
                  utc_offset);
        if (isGone) continue;

        h = fnv1a_str(h, route);
        h = fnv1a_str(h, dest);
        h = fnv1a_str(h, etaNum);
        rowsAdded++;
    }

    // Alert headers (only if SHOW_ALERTS is on and alerts are present)
#if SHOW_ALERTS
    if (hasAlerts) {
        for (JsonObjectConst a : alerts) {
            const char* hdr = a["header"] | "";
            h = fnv1a_str(h, hdr);
        }
    }
#endif

    // Layout flags
    uint8_t flags = (stale ? 1 : 0) | (hasAlerts ? 2 : 0);
    h = fnv1a_update(h, &flags, 1);

    // Battery in 10% buckets (so minor ADC noise doesn't trigger a redraw)
    int8_t battBucket = (batteryPct >= 0) ? (int8_t)((batteryPct / 10) * 10) : (int8_t)-1;
    h = fnv1a_update(h, (const uint8_t*)&battBucket, 1);

    return h;
}

// =============================================================================
// performOTA() — Feature 4
//
// Called after a successful fetch+parse when the firmware block is present.
// Downloads bin_url, streams into Update while computing SHA-256,
// verifies digest == manifest sha256, then calls Update.end(true) + ESP.restart().
//
// On ANY error: Update.abort(), log, return false — caller continues normally.
// The device NEVER bricks: if SHA-256 doesn't match we abort before committing.
//
// Safety gates (caller must have verified):
//   - firmware.version != FW_VERSION
//   - bin_url starts "https://"
//   - sha256 is 64 hex chars
//   - power is safe (USB or battery healthy)
//
// RAM note: OTA download runs AFTER the JSON parse + board render, and we
// explicitly call doc.clear() before starting so the JsonDocument heap is free.
// The TLS client + Update stream + SHA-256 context fit within the ESP32-C3's
// ~400 KB usable SRAM (~40 KB TLS, ~32 KB Update write buffer, ~300 B SHA-256).
//
// mbedTLS SHA-256 API (from mbedtls/sha256.h, available in ESP-IDF):
//   mbedtls_sha256_context ctx;
//   mbedtls_sha256_init(&ctx);
//   mbedtls_sha256_starts(&ctx, 0);  // 0 = SHA-256 (not SHA-224)
//   mbedtls_sha256_update(&ctx, data, len);
//   mbedtls_sha256_finish(&ctx, output[32]);
//   mbedtls_sha256_free(&ctx);
//
// Update API (Arduino-ESP32 v3, from Update.h, built into core):
//   Update.begin(size)            — start OTA partition write; size = total bytes
//   Update.writeStream(Stream&)   — stream bytes from a client/stream object
//   Update.end(true)              — commit + verify; true = MD5 check (built-in)
//   Update.abort()                — cancel without committing
//   Update.errorString()          — human-readable error
//
// IMPORTANT: We do NOT use Update.writeStream() here because we need to
// intercept every byte for the SHA-256.  Instead we use a manual read loop with
// Update.write(buf, len), which writes a chunk at a time.
// =============================================================================
#if ENABLE_OTA
static bool performOTA(const char* bin_url, const char* expected_sha256)
{
    Serial.printf("[OTA]  Starting OTA from %s\n", bin_url);

    WiFiClientSecure otaClient;
    otaClient.setInsecure();  // same trust model as departures fetch

    HTTPClient otaHttp;
    if (!otaHttp.begin(otaClient, bin_url)) {
        Serial.println("[OTA]  begin() failed");
        return false;
    }
    otaHttp.setTimeout(60000);  // 60 s timeout for large binary
    otaHttp.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

    int httpCode = otaHttp.GET();
    Serial.printf("[OTA]  HTTP response: %d\n", httpCode);
    if (httpCode != 200) {
        Serial.printf("[OTA]  HTTP error %d — aborting\n", httpCode);
        otaHttp.end();
        return false;
    }

    int contentLen = otaHttp.getSize();
    Serial.printf("[OTA]  Content-Length: %d bytes\n", contentLen);

    // We need contentLen > 0 for Update.begin(size).
    // If the server returns -1 (chunked encoding), contentLen is -1 and
    // Update.begin(UPDATE_SIZE_UNKNOWN) handles it — but we can't verify the
    // full SHA-256 deterministically in that case (we still try).
    // In practice GitHub Pages always sends Content-Length.
    size_t updateSize = (contentLen > 0) ? (size_t)contentLen : UPDATE_SIZE_UNKNOWN;

    if (!Update.begin(updateSize)) {
        Serial.printf("[OTA]  Update.begin() failed: %s\n", Update.errorString());
        otaHttp.end();
        return false;
    }

    // ---- Stream download + SHA-256 in one pass --------------------------------
    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);  // 0 = SHA-256 (returns void on this mbedTLS build)

    WiFiClient* stream = otaHttp.getStreamPtr();
    if (!stream) {
        Serial.println("[OTA]  No stream pointer — aborting");
        Update.abort();
        mbedtls_sha256_free(&sha_ctx);
        otaHttp.end();
        return false;
    }

    static uint8_t otaBuf[512];  // static: keep off stack (RAM-safe on ESP32-C3)
    size_t totalWritten = 0;
    bool streamError    = false;

    uint32_t dlStart = millis();
    while (otaHttp.connected() &&
           (contentLen < 0 || totalWritten < (size_t)contentLen)) {

        // Timeout guard: abort if download stalls for > 60 s
        if (millis() - dlStart > 60000UL) {
            Serial.println("[OTA]  Download timeout — aborting");
            streamError = true;
            break;
        }

        size_t avail = stream->available();
        if (avail == 0) {
            delay(10);
            continue;
        }

        size_t toRead = (avail < sizeof(otaBuf)) ? avail : sizeof(otaBuf);
        // Cap to remaining expected bytes
        if (contentLen > 0) {
            size_t remaining = (size_t)contentLen - totalWritten;
            if (toRead > remaining) toRead = remaining;
        }

        int bytesRead = stream->readBytes(otaBuf, toRead);
        if (bytesRead <= 0) {
            delay(5);
            continue;
        }

        // Feed to SHA-256 (returns void on this mbedTLS build)
        mbedtls_sha256_update(&sha_ctx, otaBuf, (size_t)bytesRead);

        // Feed to Update (OTA partition writer)
        size_t written = Update.write(otaBuf, (size_t)bytesRead);
        if (written != (size_t)bytesRead) {
            Serial.printf("[OTA]  Update.write() wrote %u of %u bytes — aborting\n",
                          (unsigned)written, (unsigned)bytesRead);
            streamError = true;
            break;
        }

        totalWritten += (size_t)bytesRead;
        dlStart = millis();  // reset timeout on progress
    }

    otaHttp.end();

    if (streamError) {
        Update.abort();
        mbedtls_sha256_free(&sha_ctx);
        return false;
    }

    // ---- Verify SHA-256 BEFORE committing ------------------------------------
    uint8_t digest[32];
    mbedtls_sha256_finish(&sha_ctx, digest);  // returns void on this mbedTLS build
    mbedtls_sha256_free(&sha_ctx);

    // Convert digest to lowercase hex
    char computedHex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(computedHex + i*2, 3, "%02x", digest[i]);
    }
    computedHex[64] = '\0';

    Serial.printf("[OTA]  Computed SHA-256: %s\n", computedHex);
    Serial.printf("[OTA]  Expected SHA-256: %s\n", expected_sha256);

    if (strcmp(computedHex, expected_sha256) != 0) {
        Serial.println("[OTA]  SHA-256 MISMATCH — aborting (firmware NOT flashed)");
        Update.abort();
        return false;
    }

    Serial.println("[OTA]  SHA-256 verified OK — committing update...");

    // ---- Commit update -------------------------------------------------------
    // Update.end(true) finalises the partition and sets the boot flag.
    // The bool arg enables MD5 check built into the Update library (independent
    // of our SHA-256; belt-and-suspenders).
    if (!Update.end(true)) {
        Serial.printf("[OTA]  Update.end() failed: %s\n", Update.errorString());
        Update.abort();
        return false;
    }

    Serial.println("[OTA]  Update committed — restarting...");
    Serial.flush();
    delay(200);
    ESP.restart();
    // Never reached
    return true;
}
#endif // ENABLE_OTA

// =============================================================================
// isHexSha256() — validate that a string is exactly 64 lowercase hex chars.
// Used for OTA safety gate.
// =============================================================================
static bool isHexSha256(const char* s)
{
    if (!s || strlen(s) != 64) return false;
    for (int i = 0; i < 64; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

// =============================================================================
// setup() — runs once per wake cycle.
// =============================================================================
void setup()
{
    // ---- 1. Serial ---------------------------------------------------------
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0 < 2000)) { /* wait */ }
    Serial.println("\n[BOOT] QLD Bus Sign — Xteink X4 v3 (JSON/NTP)");

    // ---- 1b. NVS / Preferences ---------------------------------------------
    Preferences prefs;
    prefs.begin("busign", /*readOnly=*/false);
    loadRuntimeSettings(prefs);

    // ---- 1c. Button wake detection (Feature 8, v3.2) -----------------------
    // The deep-sleep wake could have come from the timer (normal schedule) OR
    // from a physical button (LEFT/RIGHT ladder on GPIO1, or the POWER button on
    // GPIO3).  Set up the ADC pins, print the raw values for calibration, and —
    // if this wake was a GPIO (button) wake showing a LEFT/RIGHT signature — set
    // manualStopOverride so the stop-selection block (§12) shows that stop with
    // a FRESH fetch (the fetch happens naturally later in this same cycle).
    //
    // Pin init for the C3 ADC: explicit INPUT + 12-bit resolution + 11 dB
    // attenuation so the full 0–3.3 V ladder range maps onto analogRead()'s
    // 0–4095 scale (matches the thresholds derived in config.h).
    pinMode(BTN_ADC_LEFT_PIN,  INPUT);
    pinMode(BTN_ADC_RIGHT_PIN, INPUT);
    pinMode(BTN_ADC_2_PIN,     INPUT);
    pinMode(BTN_POWER_PIN,     INPUT_PULLUP);   // direct active-LOW power button
    analogReadResolution(12);                   // 0..4095 (matches config.h windows)
    analogSetAttenuation(ADC_11db);             // full 0–3.3 V span (chip-wide; default is already 11db)

    esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
    bool gpioWake = (wakeCause == ESP_SLEEP_WAKEUP_GPIO);
    Serial.printf("[BTN] wake cause=%d  gpioWake=%d  (gpio status=0x%llx)\n",
                  (int)wakeCause, (int)gpioWake,
                  (unsigned long long)esp_sleep_get_gpio_wakeup_status());

    // Always sample + print on boot so the user can calibrate by watching serial.
    ButtonId bootBtn = readButton();

    // Only adopt the press as an override when it actually woke us AND reads as
    // LEFT/RIGHT.  (A POWER-button wake reads as none → no override → normal
    // time-of-day stop with a fresh fetch, which is a fine "refresh now" action.)
    if (gpioWake && bootBtn == BTN_LEFT) {
        manualStopOverride = 0;   // HOME / morning stop
        Serial.println("[BTN] Boot wake = LEFT  -> manualStopOverride=0 (HOME)");
    } else if (gpioWake && bootBtn == BTN_RIGHT) {
        manualStopOverride = 1;   // CITY / return stop
        Serial.println("[BTN] Boot wake = RIGHT -> manualStopOverride=1 (CITY)");
    }

    // ---- 2. SPI bus init ---------------------------------------------------
    SPI.begin(EPD_SCK, EPD_MISO, EPD_MOSI, /*ss=*/-1);
    Serial.println("[SPI]  Bus initialised");

    // ---- 3. E-ink display init + rotation ----------------------------------
    display.init(115200, /*initial=*/true, /*reset_duration=*/10);
    display.setRotation(gRotation);
    Serial.printf("[EPD]  Panel initialised, rotation=%d (portrait %dx%d)\n",
                  gRotation, display.width(), display.height());

    // ---- 4. Wi-Fi connection -----------------------------------------------
    if (strlen(WIFI_SSID) > 0) {
        // Path A: hardcoded credentials
        Serial.printf("[WIFI] Hardcoded SSID mode — connecting to \"%s\"...\n", WIFI_SSID);
        WiFi.mode(WIFI_STA);
        WiFi.begin(WIFI_SSID, WIFI_PASS);
        {
            uint32_t wifi_start = millis();
            while (WiFi.status() != WL_CONNECTED) {
                if (millis() - wifi_start > WIFI_TIMEOUT_MS) {
                    Serial.println("[WIFI] Timeout (hardcoded SSID)");
                    wifiFailCount++;
                    showStatusMessage("WiFi failed", WIFI_SSID);
                    prefs.end();
                    goToSleep();
                    return;
                }
                delay(250);
                Serial.print('.');
            }
        }
        wifiFailCount = 0;
        Serial.printf("\n[WIFI] Connected — IP: %s\n", WiFi.localIP().toString().c_str());

    } else {
        // Path B: phone captive-portal flow
        Serial.println("[WIFI] Portal mode — trying saved credentials...");
        WiFi.mode(WIFI_STA);

        bool wifiConnected = false;

        // ---- Multi-network auto-join (v3.1, Feature A) ----------------------
        // Before the legacy single-creds connect/portal block, try every Wi-Fi
        // network the user has remembered and join whichever is in range.
        // (Migration of a prior 3.0 single saved network into this list happens
        // after a successful connect below, where its credentials are readable.)
        {
            String wnSsids[MAX_WIFI_NETS];
            String wnPasss[MAX_WIFI_NETS];
            int wnCount = loadWifiNetworks(prefs, wnSsids, wnPasss, MAX_WIFI_NETS);

            if (wnCount > 0) {
                Serial.printf("[WIFI] Multi mode — %d remembered network(s), scanning...\n",
                              wnCount);
                WiFiMulti wifiMulti;
                for (int i = 0; i < wnCount; i++) {
                    if (wnSsids[i].length() == 0) continue;
                    wifiMulti.addAP(wnSsids[i].c_str(), wnPasss[i].c_str());
                }
                if (wifiMulti.run(WIFI_TIMEOUT_MS) == WL_CONNECTED) {
                    wifiConnected = true;
                    wifiFailCount = 0;
                    Serial.printf("[WIFI] Multi connected — IP: %s  (SSID \"%s\")\n",
                                  WiFi.localIP().toString().c_str(),
                                  WiFi.SSID().c_str());
                } else {
                    Serial.println("[WIFI] Multi: no remembered network in range");
                }
            }
        }

        // ---- Legacy single saved-credentials attempt -----------------------
        // Only runs if the multi attempt above did not connect (e.g. no list, or
        // none of the remembered networks were reachable this wake).
        if (!wifiConnected) {
            WiFi.begin();

            uint32_t wifi_start = millis();
            while (millis() - wifi_start < WIFI_TIMEOUT_MS) {
                if (WiFi.status() == WL_CONNECTED) {
                    wifiConnected = true;
                    break;
                }
                delay(250);
                Serial.print('.');
            }
            Serial.println();
        }

        if (wifiConnected) {
            wifiFailCount = 0;
            Serial.printf("[WIFI] Connected (saved creds) — IP: %s\n",
                          WiFi.localIP().toString().c_str());

            // Remember whatever we just connected to.  Covers the 3.0 -> 3.1
            // migration (the single saved network now has readable creds) and
            // refreshes the stored entry.  addWifiNetwork() is a no-op when the
            // network is already stored identically, so this is safe to run on
            // every wake without wearing NVS.
            addWifiNetwork(prefs, WiFi.SSID(), WiFi.psk());

        } else {
            bool noSavedCreds   = (WiFi.SSID().length() == 0);
            bool shouldOpenPortal = noSavedCreds || (wifiFailCount >= WIFI_PORTAL_AFTER_FAILS);

            if (shouldOpenPortal) {
                Serial.printf("[WIFI] Opening config portal — AP: \"%s\", timeout: %d s\n",
                              WIFI_PORTAL_AP_NAME, WIFI_PORTAL_TIMEOUT_S);

                WiFi.disconnect(false);
                delay(100);

                WiFiManager wm;
                wm.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_S);

                // ---- Portal branding (v3.1, Feature B) -----------------------
                // Title, self-contained stylesheet (offline-safe), and a branded
                // header that also lists any networks already remembered.
                wm.setTitle("QLD Bus Sign");
                wm.setCustomHeadElement(PORTAL_HEAD);

                {
                    // Build the "Remembered networks: ..." line into the file-scope
                    // static buffer (setCustomMenuHTML keeps the pointer, no copy).
                    String wnSsids[MAX_WIFI_NETS];
                    String wnPasss[MAX_WIFI_NETS];
                    int wnCount = loadWifiNetworks(prefs, wnSsids, wnPasss, MAX_WIFI_NETS);

                    String remembered;
                    for (int i = 0; i < wnCount; i++) {
                        if (wnSsids[i].length() == 0) continue;
                        if (remembered.length() > 0) remembered += ", ";
                        remembered += wnSsids[i];
                    }
                    if (remembered.length() == 0) remembered = "none yet";

                    snprintf(g_portalMenuHtml, sizeof(g_portalMenuHtml),
                             "<div class='qbs-hdr'>&#128652; QLD Bus Sign &mdash; WiFi setup</div>"
                             "<div class='qbs-sub'>Pick your Wi-Fi below; the sign will remember it "
                             "and auto-join wherever you take it.</div>"
                             "<div class='qbs-sub'>Remembered networks: %s</div>",
                             remembered.c_str());
                    wm.setCustomMenuHTML(g_portalMenuHtml);
                }

                // ---- Portal custom parameters --------------------------------
                char curRefreshStr[4];
                snprintf(curRefreshStr, sizeof(curRefreshStr), "%d", gRefreshMin);
                char curRotationStr[2];
                snprintf(curRotationStr, sizeof(curRotationStr), "%d", gRotation);
                char curSwitchHStr[3];
                snprintf(curSwitchHStr, sizeof(curSwitchHStr), "%d", gStopSwitchHour);

                WiFiManagerParameter paramUrl(
                    "data_url", "Data URL (https://...)", gDataUrl.c_str(), 200);
                WiFiManagerParameter paramRefresh(
                    "refresh_m", "Refresh minutes (1-60)", curRefreshStr, 3);
                WiFiManagerParameter paramRotation(
                    "rotation", "Display rotation (0-3)", curRotationStr, 1);
                WiFiManagerParameter paramStopSwitch(
                    "stop_switch_h",
                    "Stop switch hour 0-23 (< = home, >= = city)",
                    curSwitchHStr, 3);

                wm.addParameter(&paramUrl);
                wm.addParameter(&paramRefresh);
                wm.addParameter(&paramRotation);
                wm.addParameter(&paramStopSwitch);

                g_pSetupDisplay = &display;

                wm.setAPCallback([](WiFiManager* /*wm*/) {
                    String ipStr = WiFi.softAPIP().toString();
                    Serial.printf("[WIFI] Portal AP started — IP: %s\n", ipStr.c_str());
                    g_pSetupDisplay->init(115200, /*initial=*/false, /*reset_duration=*/10);
                    g_pSetupDisplay->setRotation(gRotation);
                    drawSetupScreen(*g_pSetupDisplay, WIFI_PORTAL_AP_NAME, ipStr.c_str());
                });

                bool portalConnected = wm.startConfigPortal(WIFI_PORTAL_AP_NAME);

                if (portalConnected) {
                    wifiFailCount = 0;
                    wifiConnected = true;
                    Serial.printf("[WIFI] Portal connected — IP: %s\n",
                                  WiFi.localIP().toString().c_str());

                    savePortalSettings(prefs,
                                       paramUrl.getValue(),
                                       paramRefresh.getValue(),
                                       paramRotation.getValue(),
                                       paramStopSwitch.getValue());

                    // Remember the freshly-configured network (v3.1, Feature A)
                    // so future wakes auto-join it via WiFiMulti.
                    addWifiNetwork(prefs, wm.getWiFiSSID(), wm.getWiFiPass());

                    display.init(115200, /*initial=*/false, /*reset_duration=*/10);
                    display.setRotation(gRotation);

                } else {
                    Serial.println("[WIFI] Portal timed out — sleeping to retry");
                    prefs.end();
                    goToSleep();
                    return;
                }

            } else {
                wifiFailCount++;
                Serial.printf("[WIFI] Transient failure — fail count now %d/%d\n",
                              wifiFailCount, WIFI_PORTAL_AFTER_FAILS);

                char failMsg[40];
                snprintf(failMsg, sizeof(failMsg), "Retry %d/%d next wake",
                         wifiFailCount, WIFI_PORTAL_AFTER_FAILS);
                showStatusMessage("WiFi unavailable", failMsg);
                prefs.end();
                goToSleep();
                return;
            }
        }

        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[WIFI] Not connected after all paths — sleeping");
            wifiFailCount++;
            showStatusMessage("WiFi error", "sleeping");
            prefs.end();
            goToSleep();
            return;
        }
        Serial.printf("[WIFI] Ready — IP: %s  (wifiFailCount=%d)\n",
                      WiFi.localIP().toString().c_str(), wifiFailCount);
    }

    // ---- 4b. Force a usable DNS resolver -----------------------------------
    // Wi-Fi is connected here (via either path).  Guarantee name resolution
    // works even if the router's DHCP handed out no/broken DNS — otherwise NTP
    // and the departures fetch both fail with "DNS Failed".
    applyFallbackDNS();

    // ---- 5. NTP time sync --------------------------------------------------
    Serial.printf("[NTP]  Syncing from %s / %s ...\n", NTP_SERVER_1, NTP_SERVER_2);
    configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);

    time_t now_utc   = 0;
    bool   ntp_valid = false;
    uint32_t ntp_start = millis();

    while (millis() - ntp_start < (uint32_t)NTP_TIMEOUT_MS) {
        time(&now_utc);
        if (now_utc > 1700000000L) {
            ntp_valid = true;
            break;
        }
        delay(200);
    }

    if (ntp_valid) {
        Serial.printf("[NTP]  Time synced: epoch %lld\n", (long long)now_utc);
    } else {
        Serial.println("[NTP]  Sync failed — countdowns will show \"?\"");
        now_utc = 0;
    }

    // ---- 6. HTTPS GET departures.json --------------------------------------
    char url[256];
    snprintf(url, sizeof(url), "%s?t=%lld", gDataUrl.c_str(), (long long)now_utc);
    Serial.printf("[HTTP] GET %s\n", url);

    WiFiClientSecure client;
#if USE_INSECURE_TLS
    client.setInsecure();
    Serial.println("[TLS]  Certificate verification SKIPPED (USE_INSECURE_TLS=1)");
#else
    Serial.println("[TLS]  Certificate verification ENABLED");
#endif

    bool   fetchOk    = false;
    time_t fetchEpoch = 0;
    String jsonBody;

    // FIX (v3.1, Feature C): on the very first request after Wi-Fi comes up, DNS
    // and the TLS stack sometimes aren't ready yet, which surfaced as a spurious
    // "No connection" on first boot.  A one-time short settle here fixes it.
    delay(600);  // DNS / TLS settle — once, before the first GET

    // FIX (v3.1, Feature C): retry the departures GET up to 3 times before
    // falling through to the cache/no-connection path.  Each attempt re-begins
    // the request exactly as the single-shot code did; we only break early on a
    // good (200 + acceptable body) fetch.  Parse/cache/render below is unchanged.
    for (int attempt = 1; attempt <= 3; attempt++) {
        HTTPClient http;
        if (!http.begin(client, url)) {
            Serial.println("[HTTP] begin() failed — will try cache");
            http.end();
        } else {
            http.setTimeout(HTTP_TIMEOUT_MS);
            http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
            http.addHeader("Cache-Control", "no-cache");

            int httpCode = http.GET();
            Serial.printf("[HTTP] Response code: %d\n", httpCode);

            if (httpCode == 200) {
                int contentLen = http.getSize();
                // FIX 3: simplified early reject — catches known-too-large Content-Length.
                // Does NOT reject chunked (contentLen==-1); those are caught post-getString below.
                if (contentLen > 8192) {
                    Serial.printf("[HTTP] Body too large (%d B, Content-Length) — rejecting\n", contentLen);
                    http.end();
                } else {
                    jsonBody = http.getString();
                    http.end();
                    // FIX 3: post-getString guard catches chunked responses and any other
                    // case where Content-Length was absent or misleading.  An empty body or
                    // a body >8192 B is rejected; we fall through to the NVS cache path.
                    if (jsonBody.length() == 0 || jsonBody.length() > 8192) {
                        Serial.printf("[HTTP] Body rejected after getString: %d bytes\n",
                                      jsonBody.length());
                        fetchOk = false;
                    } else {
                        fetchOk    = true;
                        fetchEpoch = now_utc;
                        Serial.printf("[HTTP] Body length: %d bytes\n", jsonBody.length());
                    }
                }
            } else {
                Serial.printf("[HTTP] Error %d — will try cache\n", httpCode);
                http.end();
            }
        }

        if (fetchOk) break;  // got a good body — stop retrying

        if (attempt < 3) {
            Serial.printf("[HTTP] retry %d/3\n", attempt);
            delay(800);
        }
    }
    WiFi.disconnect(true);

    // ---- 7. Parse JSON (Contract v3) ---------------------------------------
    // Filter: version + generated_at + utc_offset_seconds + firmware block +
    //         alerts[] + stops[](stop_id, stop_label, departures[]).
    //
    // The filter shape must match the v3 JSON structure exactly.
    // arrays: [0] describes element shape; ArduinoJson uses the first element
    // as the template for all elements.
    JsonDocument filter;
    filter["version"]                                         = true;
    filter["generated_at"]                                    = true;
    filter["utc_offset_seconds"]                              = true;
    filter["firmware"]["version"]                             = true;
    filter["firmware"]["bin_url"]                             = true;
    filter["firmware"]["sha256"]                              = true;
    filter["alerts"][0]["header"]                             = true;
    filter["alerts"][0]["id"]                                 = true;
    filter["alerts"][0]["severity"]                           = true;
    filter["alerts"][0]["effect"]                             = true;
    filter["alerts"][0]["routes"][0]                          = true;
    filter["stops"][0]["stop_id"]                             = true;
    filter["stops"][0]["stop_label"]                          = true;
    filter["stops"][0]["departures"][0]["route"]              = true;
    filter["stops"][0]["departures"][0]["dest"]               = true;
    filter["stops"][0]["departures"][0]["time"]               = true;
    filter["stops"][0]["departures"][0]["live"]               = true;
    filter["stops"][0]["departures"][0]["cancelled"]          = true;

    JsonDocument doc;
    bool stale = false;

    if (fetchOk) {
        DeserializationError err = deserializeJson(
            doc, jsonBody,
            DeserializationOption::Filter(filter)
        );

        // v3 required fields: version + stops[] + alerts[] (FIX 2: live and cache
        // checks now both require alerts to be a JsonArray, preventing a malformed
        // live body missing "alerts" from being accepted and cached).
        if (err || !doc["version"].is<int>() || !doc["stops"].is<JsonArray>() ||
            !doc["alerts"].is<JsonArray>()) {
            Serial.printf("[JSON] Parse error: %s — will try cache\n",
                          err ? err.c_str() : "missing v3 fields");
            fetchOk = false;
            doc.clear();
        } else {
            Serial.printf("[JSON] Parsed OK — version=%d, stops=%d\n",
                          (int)doc["version"],
                          (int)doc["stops"].as<JsonArray>().size());

            // Cache to NVS (guard: ≤4000 bytes, Preferences limit)
            if (jsonBody.length() <= 4000) {
                prefs.putString("last_json",  jsonBody);
                prefs.putULong64("last_epoch", (uint64_t)fetchEpoch);
                Serial.printf("[NVS]  Cached %d bytes, epoch=%lld\n",
                              jsonBody.length(), (long long)fetchEpoch);
            } else {
                Serial.printf("[NVS]  JSON too large (%d B) — cache skipped\n",
                              jsonBody.length());
            }
        }
    }

    if (!fetchOk) {
        // ---- Try NVS cache ------------------------------------------------
        String cachedJson         = prefs.getString("last_json",  "");
        unsigned long long cachedEpochUL = prefs.getULong64("last_epoch", 0ULL);
        fetchEpoch = (time_t)cachedEpochUL;

        if (cachedJson.length() > 0) {
            Serial.printf("[NVS]  Loading cached JSON (%d bytes, epoch=%lld)\n",
                          cachedJson.length(), (long long)fetchEpoch);

            DeserializationError err = deserializeJson(
                doc, cachedJson,
                DeserializationOption::Filter(filter)
            );

            if (err || !doc["version"].is<int>() || !doc["stops"].is<JsonArray>() ||
                !doc["alerts"].is<JsonArray>()) {
                Serial.printf("[NVS]  Cache parse error: %s\n",
                              err ? err.c_str() : "missing v3 fields");
                // v3.3.0 (Feature A): pass now_utc so the no-data screen shows the
                // live "Updated H:MMam" line (offset defaults to AEST; utc_offset
                // from JSON is not available on this error path).
                drawNoDataScreen(display, now_utc);
                prefs.end();
                goToSleep();
                return;
            }

            stale = true;
            Serial.println("[NVS]  Rendering with STALE cached data");
        } else {
            Serial.println("[NVS]  No cached data — rendering no-data screen");
            drawNoDataScreen(display, now_utc);   // v3.3.0: live "Updated H:MMam" line
            prefs.end();
            goToSleep();
            return;
        }
    }

    // ---- 8. Version check (Contract §E) ------------------------------------
    int jsonVersion = doc["version"] | 0;
    if (jsonVersion != 3) {
        Serial.printf("[JSON] Version %d != 3 — showing version error screen\n", jsonVersion);
        drawVersionErrorScreen(display);
        prefs.end();
        goToSleep();
        return;
    }

    // ---- 9. Battery read ---------------------------------------------------
    int batteryPct = -1;
#if ENABLE_BATTERY_MONITOR
    batteryPct = readBatteryPct();
    Serial.printf("[BATT] Battery: %d%%\n", batteryPct);
#endif

    // ---- 10. Low-battery critical check (Feature 7) ------------------------
    // Only fires when monitor is enabled AND reading is valid AND below threshold.
    // Hysteresis: we only resume when batteryPct > BATT_RESUME_PCT (15%).
    // We use a single NVS key "batt_crit" (int: 1=in critical mode, 0=normal).
    // - If we enter critical mode (pct <= BATT_CRITICAL_PCT): set key=1, show screen, long sleep.
    // - If key==1 and pct > BATT_RESUME_PCT: clear key=0, resume normally.
    // - If key==1 and pct still <= BATT_RESUME_PCT: show screen, long sleep.
    // - If key==0 and pct > BATT_CRITICAL_PCT: no action (normal).
    // When battery monitor is disabled or reading is -1: this block is never entered.
#if ENABLE_BATTERY_MONITOR
    if (batteryPct >= 0) {
        int battCritState = prefs.getInt("batt_crit", 0);

        bool enterCritical = (batteryPct <= BATT_CRITICAL_PCT);
        bool stillCritical = (battCritState == 1) && (batteryPct <= BATT_RESUME_PCT);

        if (enterCritical || stillCritical) {
            prefs.putInt("batt_crit", 1);
            Serial.printf("[BATT] CRITICAL — battery %d%% <= %d%% — showing plug screen\n",
                          batteryPct, BATT_CRITICAL_PCT);
            drawLowBatteryScreen(display, batteryPct);
            prefs.end();
            goToSleep(BATT_PROTECT_SLEEP_MIN);
            return;
        } else if (battCritState == 1 && batteryPct > BATT_RESUME_PCT) {
            // Recovered — clear the critical state flag
            prefs.putInt("batt_crit", 0);
            Serial.printf("[BATT] Recovered from critical (%d%% > %d%%) — resuming\n",
                          batteryPct, BATT_RESUME_PCT);
        }
    }
#endif // ENABLE_BATTERY_MONITOR

    // ---- 11. Extract common doc fields ------------------------------------
    int32_t     utc_offset     = doc["utc_offset_seconds"] | 36000;
    JsonArray   stopsArr       = doc["stops"].as<JsonArray>();
    JsonArray   alertsArr      = doc["alerts"].as<JsonArray>();
    // alertsArr may be a null array if key absent (shouldn't happen in v3) —
    // JsonArrayConst will simply iterate zero elements in that case.

    // ---- 12. Stop selection (Feature 5, Contract §D) ----------------------
    // Compute local hour with negative-wrap fix.
    // gStopSwitchHour is configurable via portal (default: STOP_SWITCH_HOUR=14).
    const char* selectedStopId    = nullptr;
    const char* selectedStopLabel = nullptr;
    JsonArray   selectedDeps;     // departures[] of the chosen stop

    // Determine target stop_id
    const char* targetStopId = nullptr;

    // Feature 8 (v3.2): a physical-button press takes PRIORITY over the
    // time-of-day choice for this one wake cycle.  manualStopOverride was set in
    // setup() step 1c (from a button wake) or in the post-render live poll (see
    // below) which then esp_restart()s back through here.  After applying it we
    // clear it to -1 so the NEXT scheduled (timer) wake returns to normal
    // time-based behaviour.  The data is still freshly fetched above, so a press
    // always shows up-to-the-minute timings for the chosen stop.
    if (manualStopOverride >= 0) {
        targetStopId = (manualStopOverride == 0) ? HOME_STOP_ID : CITY_STOP_ID;
        Serial.printf("[BTN] manual stop override -> %s (%s)\n",
                      targetStopId,
                      (manualStopOverride == 0) ? "HOME/LEFT" : "CITY/RIGHT");
        manualStopOverride = -1;   // one-shot: next scheduled wake is normal again
    } else if (now_utc > 0) {
        int64_t localEpoch = (int64_t)now_utc + (int64_t)utc_offset;
        int localHour = (int)((localEpoch / 3600LL) % 24LL);
        if (localHour < 0) localHour += 24;  // negative-wrap fix
        targetStopId = (localHour < gStopSwitchHour) ? HOME_STOP_ID : CITY_STOP_ID;
        Serial.printf("[STOP] localHour=%d  switch=%d  target=%s\n",
                      localHour, gStopSwitchHour, targetStopId);
    } else {
        // NTP failed — default to HOME stop
        targetStopId = HOME_STOP_ID;
        Serial.printf("[STOP] NTP failed — defaulting to HOME_STOP_ID=%s\n", targetStopId);
    }

    // Find the matching stops[] entry; fallback to stops[0]
    bool found = false;
    for (JsonObject stop : stopsArr) {
        const char* sid = stop["stop_id"] | "";
        if (strcmp(sid, targetStopId) == 0) {
            selectedStopId    = stop["stop_id"]    | targetStopId;
            selectedStopLabel = stop["stop_label"] | "Unknown stop";
            selectedDeps      = stop["departures"].as<JsonArray>();
            found = true;
            break;
        }
    }
    if (!found && stopsArr.size() > 0) {
        JsonObject first = stopsArr[0].as<JsonObject>();
        selectedStopId    = first["stop_id"]    | "?";
        selectedStopLabel = first["stop_label"] | "Unknown stop";
        selectedDeps      = first["departures"].as<JsonArray>();
        Serial.printf("[STOP] target '%s' not found — using stops[0] '%s'\n",
                      targetStopId, selectedStopId);
    } else if (!found) {
        // Empty stops array — render no-data
        Serial.println("[STOP] No stops in JSON — rendering no-data screen");
        drawNoDataScreen(display, now_utc, utc_offset);   // v3.3.0: live "Updated H:MMam" line
        prefs.end();
        goToSleep();
        return;
    }

    Serial.printf("[STOP] Selected: id=%s  label=%s  deps=%d\n",
                  selectedStopId, selectedStopLabel,
                  (int)selectedDeps.size());

    // ---- 13. Alert presence check -----------------------------------------
    // Determine if alerts banner will be shown (mirrors board_render.h logic).
    bool hasAlerts = false;
#if SHOW_ALERTS
    {
        int alertCount = 0;
        for (JsonObjectConst a : alertsArr) {
            (void)a;
            alertCount++;
        }
        hasAlerts = (alertCount > 0);
    }
#endif

    // ---- 14. Content hash — Feature 1a (change-skip) ----------------------
    // v3.3.0 (Feature A): utc_offset is now passed so the hash includes the
    // current LOCAL minute — each new minute counts as "changed" and forces a
    // redraw so the live "Updated H:MMam" footer line ticks (sign looks alive).
    uint32_t newHash = computeContentHash(
        selectedStopId,
        selectedDeps,
        alertsArr,
        now_utc,
        hasAlerts,
        stale,
        batteryPct,
        utc_offset
    );

    uint32_t lastHash = prefs.getULong("last_hash", 0xFFFFFFFFUL);
    bool contentChanged = (newHash != lastHash);

    Serial.printf("[HASH] new=0x%08lX  last=0x%08lX  changed=%d\n",
                  (unsigned long)newHash, (unsigned long)lastHash, (int)contentChanged);

    // Detect layout changes that require a full refresh (Feature 1b):
    //   - stop_id changed (stop switch occurred this wake)
    //   - alert presence changed (banner appeared or disappeared)
    //   - stale flag changed
    String lastStopId     = prefs.getString("last_stop_id",   "");
    bool   lastHasAlert   = (prefs.getInt("last_has_alert",   0) != 0);
    bool   lastStaleFlag  = (prefs.getInt("last_stale",       0) != 0);

    bool layoutChanged = (lastStopId != selectedStopId) ||
                         (lastHasAlert != hasAlerts)    ||
                         (lastStaleFlag != stale);

    // ---- 15. OTA update check (Feature 4) ---------------------------------
    // Runs AFTER content hash but BEFORE rendering; on success ESP.restart()
    // is called and we never reach the render code.
    // Only when: fetch was live (not stale), firmware block present, safe power.
#if ENABLE_OTA
    if (!stale && doc["firmware"].is<JsonObject>()) {
        const char* fwVersion  = doc["firmware"]["version"] | "";
        const char* fwBinUrl   = doc["firmware"]["bin_url"] | "";
        const char* fwSha256   = doc["firmware"]["sha256"]  | "";

        bool versionDiffers = (strlen(fwVersion) > 0 && strcmp(fwVersion, FW_VERSION) != 0);
        bool urlSafe        = (strncmp(fwBinUrl, "https://", 8) == 0);
        bool sha256Valid    = isHexSha256(fwSha256);

        // Power safety gate: if battery monitor enabled, require batteryPct >= OTA_MIN_BATT_PCT.
        // If monitor disabled (batteryPct == -1) → assume on USB, allow OTA.
        bool powerSafe;
#if ENABLE_BATTERY_MONITOR
        powerSafe = (batteryPct < 0 || batteryPct >= OTA_MIN_BATT_PCT);
#else
        powerSafe = true;
#endif

        Serial.printf("[OTA]  fw_version=%s  cur=%s  url_safe=%d  sha_valid=%d  power=%d\n",
                      fwVersion, FW_VERSION, (int)urlSafe, (int)sha256Valid, (int)powerSafe);

        if (versionDiffers && urlSafe && sha256Valid && powerSafe) {
            Serial.printf("[OTA]  Triggering OTA: %s -> %s\n", FW_VERSION, fwVersion);
            // Copy URL + SHA-256 into local buffers BEFORE clearing the JsonDocument,
            // since fwBinUrl and fwSha256 are const char* into doc's internal pool.
            char otaBinUrl[256];
            char otaSha256[65];
            strncpy(otaBinUrl, fwBinUrl,  sizeof(otaBinUrl)  - 1); otaBinUrl[sizeof(otaBinUrl)-1]  = '\0';
            strncpy(otaSha256, fwSha256,  sizeof(otaSha256)  - 1); otaSha256[sizeof(otaSha256)-1]  = '\0';
            // Free the JsonDocument before OTA to reclaim heap for the OTA stream.
            doc.clear();
            // FIX 4: null dangling pointers that pointed into doc's internal pool,
            // so they cannot be accidentally dereferenced before the post-OTA re-parse
            // re-assigns them.
            selectedStopId    = nullptr;
            selectedStopLabel = nullptr;
            // performOTA() calls ESP.restart() on success; on failure returns false.
            bool otaOk = performOTA(otaBinUrl, otaSha256);
            if (!otaOk) {
                Serial.println("[OTA]  Failed — continuing with normal render");
                // Re-parse is not possible (doc cleared); try cache path to get departures.
                // Reload from NVS cache for this cycle.
                String cachedJson = prefs.getString("last_json", "");
                if (cachedJson.length() == 0) {
                    // No cache available — cannot render anything safe.
                    Serial.println("[OTA]  No cached JSON after OTA failure — no-data screen");
                    drawNoDataScreen(display, now_utc, utc_offset);   // v3.3.0: live "Updated H:MMam"
                    prefs.end();
                    goToSleep();
                    return;
                }
                DeserializationError reParseErr = deserializeJson(
                    doc, cachedJson, DeserializationOption::Filter(filter));
                if (reParseErr || !doc["version"].is<int>() ||
                    !doc["stops"].is<JsonArray>() || !doc["alerts"].is<JsonArray>()) {
                    Serial.printf("[OTA]  Cache re-parse failed: %s — no-data screen\n",
                                  reParseErr ? reParseErr.c_str() : "missing v3 fields");
                    drawNoDataScreen(display, now_utc, utc_offset);   // v3.3.0: live "Updated H:MMam"
                    prefs.end();
                    goToSleep();
                    return;
                }
                stale = true;
                // Re-assign all pointers from freshly re-parsed doc
                stopsArr   = doc["stops"].as<JsonArray>();
                alertsArr  = doc["alerts"].as<JsonArray>();
                found = false;
                for (JsonObject stop : stopsArr) {
                    const char* sid = stop["stop_id"] | "";
                    if (strcmp(sid, targetStopId) == 0) {
                        selectedStopId    = stop["stop_id"]    | targetStopId;
                        selectedStopLabel = stop["stop_label"] | "Unknown stop";
                        selectedDeps      = stop["departures"].as<JsonArray>();
                        found = true;
                        break;
                    }
                }
                if (!found && stopsArr.size() > 0) {
                    JsonObject first = stopsArr[0].as<JsonObject>();
                    selectedStopId    = first["stop_id"]    | "?";
                    selectedStopLabel = first["stop_label"] | "Unknown stop";
                    selectedDeps      = first["departures"].as<JsonArray>();
                } else if (!found) {
                    Serial.println("[OTA]  Cache re-parse: no stops — no-data screen");
                    drawNoDataScreen(display, now_utc, utc_offset);   // v3.3.0: live "Updated H:MMam"
                    prefs.end();
                    goToSleep();
                    return;
                }
                // Re-hash with restored data (v3.3.0: pass utc_offset for the
                // local-minute tick term, matching the primary hash above).
                newHash = computeContentHash(
                    selectedStopId,
                    selectedDeps,
                    alertsArr,
                    now_utc, hasAlerts, stale, batteryPct, utc_offset);
                contentChanged = (newHash != lastHash);
            }
        }
    }
#endif // ENABLE_OTA

    // ---- 16. Partial-refresh counter logic (Feature 1b) --------------------
    // Counter semantics (B2 + M6):
    //   panelEverPainted == false → first boot after power-on reset.
    //     Force a full redraw unconditionally (B2: panel is uninitialised).
    //     Using a dedicated RTC sentinel avoids conflating "first boot" with
    //     the counter==0 state that follows every ghost-clear reset (FIX 1).
    //   Counter advances ONLY when a redraw actually happens (M6: unchanged
    //     wakes don't count toward the ghost-clear cycle).
    //   Every FULL_REFRESH_EVERY redraws → one full ghost-clear refresh, then
    //     reset counter to 0 so the next redraw is partial again.
    //   layoutChanged always triggers a full refresh (stop switch, alert toggle).
    //
    // Wake cases:
    //   First boot (!panelEverPainted): force contentChanged=true; full refresh; panelEverPainted→true; counter→1
    //   Changed wake (panelEverPainted, counter>0): partial or full (per ghostClear/layoutChanged); counter++
    //   Unchanged wake (panelEverPainted):          skip redraw; counter stays unchanged
    //   Ghost-clear wake (counter>=FULL_REFRESH_EVERY): full refresh; counter→0
    //   Wake after ghost-clear (counter==0, panelEverPainted==true): NOT first boot; partial refresh

    bool firstBoot = !panelEverPainted;  // FIX 1: decoupled from fullRefreshCounter
    if (firstBoot) {
        // Force full redraw even if hash matches (panel is uninitialised after power-on)
        contentChanged = true;
        Serial.println("[EPD]  First boot — forcing full redraw");
    }

    // Determine refresh type for this redraw (only meaningful when contentChanged)
    bool ghostClear       = (fullRefreshCounter >= FULL_REFRESH_EVERY);
    bool forceFullRefresh = firstBoot || layoutChanged || ghostClear;

    if (forceFullRefresh) {
        Serial.printf("[EPD]  Full refresh (firstBoot=%d layout=%d ghost=%d counter=%d)\n",
                      (int)firstBoot, (int)layoutChanged, (int)ghostClear, fullRefreshCounter);
    } else {
        Serial.printf("[EPD]  Partial refresh eligible (counter=%d)\n", fullRefreshCounter);
    }

    // ---- 17. Render (or skip) departure board ------------------------------
    if (!contentChanged) {
        Serial.println("[EPD]  Content unchanged (hash match) — skipping redraw");
        // counter intentionally NOT incremented (M6: unchanged wakes don't count)
        // Call drawBoard with skipRedraw=true so it just hibernates the panel.
        drawBoard(display,
                  selectedDeps,
                  selectedStopLabel,
                  alertsArr,
                  now_utc,
                  stale,
                  fetchEpoch,
                  batteryPct,
                  utc_offset,
                  forceFullRefresh,
                  /*skipRedraw=*/true);
    } else {
        Serial.println("[EPD]  Rendering board...");
        drawBoard(display,
                  selectedDeps,
                  selectedStopLabel,
                  alertsArr,
                  now_utc,
                  stale,
                  fetchEpoch,
                  batteryPct,
                  utc_offset,
                  forceFullRefresh,
                  /*skipRedraw=*/false);
        Serial.println("[EPD]  Board rendered");

        // Mark panel as ever-painted (FIX 1: sentinel for first-boot detection)
        panelEverPainted = true;

        // Advance counter only on actual redraws (M6)
        if (ghostClear) {
            fullRefreshCounter = 0;  // ghost-clear done; reset cycle (next wake: counter==0 but panelEverPainted==true → NOT first boot)
        } else {
            fullRefreshCounter++;    // count toward next ghost-clear
        }

        // Persist new hash + layout state
        prefs.putULong("last_hash",       newHash);
        prefs.putString("last_stop_id",   selectedStopId ? selectedStopId : "");
        prefs.putInt("last_has_alert",    hasAlerts ? 1 : 0);
        prefs.putInt("last_stale",        stale     ? 1 : 0);
    }

    // ---- 17b. Live button poll (Feature 8, v3.2) ---------------------------
    // The board is now on-screen.  Briefly watch the LEFT/RIGHT buttons so a
    // press made WHILE the screen is awake also switches the view — not only a
    // press that wakes the device from deep sleep.
    //
    // Why esp_restart() instead of re-running fetch+render inline?
    //   esp_restart() replays the entire, already-tested wake path (fresh Wi-Fi,
    //   NTP, HTTPS fetch+retry, parse, OTA gate, hash, render) with zero code
    //   duplication and no risk of dangling JsonDocument pointers.  Crucially,
    //   RTC_DATA_ATTR memory SURVIVES a software reset (it is only cleared on a
    //   power-on reset), so manualStopOverride set here carries into the
    //   restarted cycle's stop-selection block (§12), which applies and then
    //   clears it.  The result is a clean fresh fetch + render of the chosen
    //   stop, exactly like a deep-sleep button wake.
    //
    // Guard against self-trigger: we skip this poll on a cycle that was itself a
    // button wake (gpioWake) — the wake-press may still be held/settling and we
    // don't want an immediate restart loop.  We also only act when the press
    // selects a DIFFERENT stop than the one just rendered.
#if BUTTON_HOLD_OVERRIDE > 0
    if (!gpioWake) {
        Serial.printf("[BTN] Live poll for %d ms (press LEFT/RIGHT to switch stop)\n",
                      (int)BUTTON_HOLD_OVERRIDE);
        uint32_t pollStart = millis();
        while (millis() - pollStart < (uint32_t)BUTTON_HOLD_OVERRIDE) {
            ButtonId b = readButton();
            int wantOverride = (b == BTN_LEFT)  ? 0
                             : (b == BTN_RIGHT) ? 1 : -1;
            if (wantOverride >= 0) {
                const char* wantStopId = (wantOverride == 0) ? HOME_STOP_ID : CITY_STOP_ID;
                // Only switch if it differs from what is already shown.
                if (!selectedStopId || strcmp(selectedStopId, wantStopId) != 0) {
                    manualStopOverride = wantOverride;   // survives esp_restart()
                    Serial.printf("[BTN] Live press -> override=%d (%s) — restarting for fresh fetch\n",
                                  wantOverride, wantStopId);
                    prefs.end();
                    Serial.flush();
                    delay(50);
                    esp_restart();   // replay full wake path; §12 applies the override
                }
            }
            delay(40);   // light debounce / poll pacing
        }
        Serial.println("[BTN] Live poll window closed — no stop switch");
    }
#endif // BUTTON_HOLD_OVERRIDE

    // ---- 18. Smart sleep decision ------------------------------------------
    int sleepMinutes = gRefreshMin;  // default

    if (utc_offset != 0 && now_utc > 0) {
        int64_t localEpoch = (int64_t)now_utc + (int64_t)utc_offset;
        int localHour = (int)((localEpoch / 3600LL) % 24LL);
        if (localHour < 0) localHour += 24;

        Serial.printf("[SLEEP] Local hour: %d  (utc_offset=%d)\n", localHour, (int)utc_offset);

        bool isLateNight = (LATE_NIGHT_START_HOUR < LATE_NIGHT_END_HOUR)
            ? (localHour >= LATE_NIGHT_START_HOUR && localHour < LATE_NIGHT_END_HOUR)
            : (localHour >= LATE_NIGHT_START_HOUR || localHour < LATE_NIGHT_END_HOUR);

        if (isLateNight) {
            sleepMinutes = LATE_NIGHT_SLEEP_MIN;
            Serial.printf("[SLEEP] Late-night window — sleeping %d min\n", sleepMinutes);
        } else {
            // Count upcoming departures from the selected stop
            int upcomingCount = 0;
            for (JsonObjectConst dep : selectedDeps) {
                time_t depTime = (time_t)dep["time"].as<long long>();
                if ((int64_t)depTime >= (int64_t)now_utc - 60LL) {
                    upcomingCount++;
                }
            }
            Serial.printf("[SLEEP] Upcoming departures: %d\n", upcomingCount);

            // FIX 5: only apply no-service long sleep on fresh (non-stale) data.
            // When stale, an empty board is an artifact of the stale cache, not
            // reality — applying NO_SERVICE_SLEEP_MIN would delay reconnection by
            // up to 20 min.  On stale data we sleep the normal gRefreshMin to retry soon.
            if (upcomingCount == 0 && !stale) {
                sleepMinutes = NO_SERVICE_SLEEP_MIN;
                Serial.printf("[SLEEP] No service — sleeping %d min\n", sleepMinutes);
            } else {
                Serial.printf("[SLEEP] Normal interval — sleeping %d min\n", sleepMinutes);
            }
        }
    } else {
        Serial.printf("[SLEEP] utc_offset unknown or NTP failed — sleeping %d min\n",
                      sleepMinutes);
    }

    // ---- 19. Deep sleep ----------------------------------------------------
    prefs.end();
    goToSleep(sleepMinutes);
}

// =============================================================================
// loop() — intentionally empty.
// setup() always ends with esp_deep_sleep_start(); loop() is never reached.
// =============================================================================
void loop() {}
