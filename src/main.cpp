#include "board_pins.h"
#include "calib_portal.h"
#include "calib_store.h"
#include "led.h"
#include "moisture_sensor.h"
#include "sample_store.h"
#include "wifi_upload.h"

#include <Arduino.h>
#include <esp_sleep.h>
#include <math.h>
#include <time.h>

static const uint32_t kBootHoldMs = 2000;
static const float kHysteresisPct = 1.0f;

static bool bootHeldForCalib() {
  pinMode(BOARD_BOOT_PIN, INPUT_PULLUP);
  // BOOT is active-low on ESP32-C3.
  if (digitalRead(BOARD_BOOT_PIN) != LOW) {
    return false;
  }
  const uint32_t start = millis();
  while (digitalRead(BOARD_BOOT_PIN) == LOW) {
    if ((millis() - start) >= kBootHoldMs) {
      return true;
    }
    delay(10);
  }
  return false;
}

static int64_t readIntervalSec() {
  return (int64_t)calibReadIntervalMin() * 60;
}

static int64_t nextMeasuredAtUnix() {
  time_t now = time(nullptr);
  if (now > 1700000000) {
    sampleStoreSetWallUnix((int64_t)now);
    return (int64_t)now;
  }
  int64_t wall = sampleStoreWallUnix();
  if (wall > 1700000000) {
    wall += readIntervalSec();
    sampleStoreSetWallUnix(wall);
    return wall;
  }
  // Unknown clock — stamp 0; upload path backfills after NTP.
  return 0;
}

static void takeSample() {
  const int raw = moistureSensorReadRaw();
  float pct = moistureSensorRawToPct(raw, calibAdcDry(), calibAdcWet());

  if (sampleStoreHasLastPct()) {
    const float last = sampleStoreLastPct();
    if (fabsf(pct - last) < kHysteresisPct) {
      pct = last;
    }
  }

  const int64_t ts = nextMeasuredAtUnix();
  sampleStorePush(ts, pct);
  Serial.printf(
      "sample raw=%d pct=%.2f count=%u/%u ts=%lld read=%dm ingest=%dm\n", raw,
      pct, (unsigned)sampleStoreCount(), (unsigned)sampleStoreCapacity(),
      (long long)ts, calibReadIntervalMin(), calibIngestIntervalMin());
}

static void goDeepSleep() {
  const uint64_t sleepUs =
      (uint64_t)calibReadIntervalMin() * 60ULL * 1000000ULL;
  Serial.printf("deep sleep %d min\n", calibReadIntervalMin());
  Serial.flush();
  esp_sleep_enable_timer_wakeup(sleepUs);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("esp-moistureSensor boot");

  ledBegin();
  moistureSensorBegin();
  calibStoreBegin();
  sampleStoreBegin();

  if (bootHeldForCalib()) {
    Serial.println("BOOT held — calibration portal");
    calibPortalRun();
    sampleStoreSyncCapacity();
    sampleStoreReset();
  }

  takeSample();

  if (sampleStoreCount() == sampleStoreCapacity()) {
    if (wifiUploadBatch()) {
      Serial.println("upload OK — clear ring");
      const int64_t wall = sampleStoreWallUnix();
      sampleStoreReset();
      sampleStoreSetWallUnix(wall);
    } else {
      Serial.println("upload failed — keep ring for retry");
    }
  }

  goDeepSleep();
}

void loop() {
  // Unreachable — deep sleep restarts into setup().
}
