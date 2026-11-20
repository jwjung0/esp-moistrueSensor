#include "board_pins.h"
#include "calib_portal.h"
#include "calib_store.h"
#include "led.h"
#include "moisture_sensor.h"
#include "sample_store.h"
#include "wifi_creds.h"
#include "wifi_upload.h"

#include <Arduino.h>
#include <esp_sleep.h>
#include <math.h>
#include <time.h>

static const uint32_t kBootHoldMs = 2000;
static const uint32_t kBootWindowMs = 5000;
static const uint32_t kBootDiagMs = 60000;
static const float kHysteresisPct = 1.0f;

// After Wi‑Fi is saved, SoftAP only opens via BOOT. Allow a short post-wake
// window so the hold does not have to start before the chip finishes booting.
static bool bootHeldForCalib() {
  pinMode(BOARD_BOOT_PIN, INPUT_PULLUP);
  Serial.printf("Hold BOOT %lus within %lus to open SoftAP…\n",
                (unsigned long)(kBootHoldMs / 1000UL),
                (unsigned long)(kBootWindowMs / 1000UL));
  const uint32_t windowStart = millis();
  uint32_t heldStart = 0;
  while ((millis() - windowStart) < kBootWindowMs) {
    if (digitalRead(BOARD_BOOT_PIN) == LOW) {
      if (heldStart == 0) {
        heldStart = millis();
        Serial.println("BOOT pressed…");
      }
      if ((millis() - heldStart) >= kBootHoldMs) {
        return true;
      }
    } else {
      heldStart = 0;
    }
    delay(10);
  }
  Serial.println("No BOOT hold — skip SoftAP");
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

// Cold-boot only: print NVS/sensor state, optional API health, stay awake
// so USB-CDC serial can attach before deep sleep.
static void bootDiagnostics() {
  const uint32_t start = millis();
  Serial.println("--- boot diagnostics ---");

  Serial.printf("wifi configured=%d", wifiCredsConfigured() ? 1 : 0);
  if (wifiCredsConfigured()) {
    Serial.printf(" ssid=%s", wifiCredsSsid().c_str());
  }
  Serial.println();
  Serial.printf("calib initiated=%d dry=%d wet=%d\n",
                calibIsInitiated() ? 1 : 0, calibAdcDry(), calibAdcWet());
  Serial.printf("intervals read=%dm ingest=%dm batch=%u\n",
                calibReadIntervalMin(), calibIngestIntervalMin(),
                (unsigned)calibBatchCount());
  Serial.printf("ring count=%u/%u\n", (unsigned)sampleStoreCount(),
                (unsigned)sampleStoreCapacity());

  const int raw = moistureSensorReadRaw();
  const float pct = moistureSensorRawToPct(raw, calibAdcDry(), calibAdcWet());
  Serial.printf("sensor raw=%d pct=%.2f (not stored)\n", raw, pct);

  if (wifiCredsConfigured()) {
    (void)wifiHealthCheck();
  } else {
    Serial.println("health: skip — WiFi not configured");
  }

  while ((millis() - start) < kBootDiagMs) {
    const uint32_t remainingMs = kBootDiagMs - (millis() - start);
    Serial.printf("diag remaining %lus\n",
                  (unsigned long)((remainingMs + 999UL) / 1000UL));
    const uint32_t chunk = remainingMs < 5000UL ? remainingMs : 5000UL;
    delay(chunk);
  }
  Serial.println("--- boot diagnostics done ---");
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("esp-moistureSensor boot");

  ledBegin();
  moistureSensorBegin();
  calibStoreBegin();
  wifiCredsBegin();
  sampleStoreBegin();

  // Stage A: no home Wi‑Fi yet → SoftAP always.
  // Stage B: Wi‑Fi already saved → SoftAP only if BOOT held (re-setup).
  const bool needPortal = !wifiCredsConfigured();
  const bool bootPortal = needPortal ? false : bootHeldForCalib();
  bool portalRan = false;
  if (needPortal || bootPortal) {
    if (needPortal) {
      Serial.println("WiFi not configured — opening SoftAP portal");
    } else {
      Serial.println("BOOT held — SoftAP portal (WiFi already saved)");
    }
    calibPortalRun();
    sampleStoreSyncCapacity();
    sampleStoreReset();
    portalRan = true;
  }

  // Cold boot / reset only: stay awake ~60 s for serial diagnostics.
  // Skip on timer wake (battery) and when SoftAP already ran this boot.
  const bool timerWake = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
  if (!timerWake && !portalRan) {
    bootDiagnostics();
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
