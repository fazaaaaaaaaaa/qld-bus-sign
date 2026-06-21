// =============================================================================
// main.cpp — TEMPORARY DIAGNOSTIC MINIMAL FIRMWARE (not the real bus sign)
//
// Purpose: the full firmware boot-loops on the Xteink X4 *before* setup() runs
// (the reset loop is ~6 ms, far shorter than the 2 s wait-for-serial at the top
// of the real setup()). That points at a crash in C++ global construction or
// the USB/core init — i.e. before any of our code can log.
//
// This strips EVERYTHING down to: bring up Serial, wait so the USB host can
// attach, and print a heartbeat. It is built and flashed exactly like the real
// firmware (same platformio.ini, same partitions, same USB flags, same merge).
//
//   * If this boots and you can read the banner below  -> the board + flash
//     pipeline are fine, and the crash is in the real app's startup objects
//     (most likely the global GxEPD2_4G display). We then fix that.
//   * If this ALSO boot-loops -> the problem is the build/USB config itself.
//
// The real firmware is restored after this test.
// =============================================================================
#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  // Give the USB-Serial host (miniterm / esp-web-tools console) time to attach
  // so the lines below actually flush out instead of being lost on a fast reset.
  delay(4000);
  Serial.println();
  Serial.println(F("##########################################"));
  Serial.println(F("####  QLD BUS SIGN - DIAG MINIMAL     ####"));
  Serial.println(F("####  If you can read this, the X4    ####"));
  Serial.println(F("####  board + flash pipeline are OK.  ####"));
  Serial.println(F("##########################################"));
  Serial.flush();
}

void loop() {
  Serial.printf("#### alive: %lu ms (free heap %u)\n",
                (unsigned long)millis(), (unsigned)ESP.getFreeHeap());
  Serial.flush();
  delay(1000);
}
