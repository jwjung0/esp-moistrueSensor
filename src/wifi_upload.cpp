#include "wifi_upload.h"

#include "calib_store.h"
#include "config.h"
#include "led.h"
#include "sample_store.h"
#include "wifi_creds.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <time.h>

static const uint32_t kStaTimeoutMs = 20000;
static const uint32_t kNtpTimeoutMs = 10000;

static String iso8601Utc(time_t t) {
  struct tm tmUtc;
  gmtime_r(&t, &tmUtc);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmUtc);
  return String(buf);
}

static bool connectSta() {
  if (!wifiCredsConfigured()) {
    Serial.println("WiFi creds missing — use SoftAP portal");
    return false;
  }
  const String ssid = wifiCredsSsid();
  const String pass = wifiCredsPass();
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < kStaTimeoutMs) {
    delay(100);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi OK ssid=%s IP=%s\n", ssid.c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.println("WiFi connect failed");
  return false;
}

static bool syncNtp() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  const uint32_t start = millis();
  while ((millis() - start) < kNtpTimeoutMs) {
    time_t now = time(nullptr);
    if (now > 1700000000) {
      sampleStoreSetWallUnix((int64_t)now);
      Serial.printf("NTP OK unix=%ld\n", (long)now);
      return true;
    }
    delay(200);
  }
  Serial.println("NTP sync failed");
  return false;
}

static void radioOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  ledSetWifiActive(false);
}

bool wifiUploadBatch() {
  const size_t need = sampleStoreCapacity();
  if (sampleStoreCount() != need || need == 0) {
    Serial.printf("upload skip: count=%u need=%u\n",
                  (unsigned)sampleStoreCount(), (unsigned)need);
    return false;
  }

  ledSetWifiActive(true);

  if (!connectSta()) {
    radioOff();
    return false;
  }

  if (syncNtp()) {
    const int64_t intervalSec = (int64_t)calibReadIntervalMin() * 60;
    sampleStoreBackfillTimestamps(sampleStoreWallUnix(), intervalSec);
  }

  String body = "{\"device_id\":\"";
  body += DEVICE_ID;
  body += "\",\"readings\":[";
  for (size_t i = 0; i < need; ++i) {
    const MoistureSample *s = sampleStoreAt(i);
    if (i > 0) {
      body += ',';
    }
    time_t t = (time_t)s->measured_at_unix;
    if (t < 1700000000) {
      t = time(nullptr);
    }
    body += "{\"moisture_pct\":";
    body += String(s->moisture_pct, 2);
    body += ",\"measured_at\":\"";
    body += iso8601Utc(t);
    body += "\"}";
  }
  body += "]}";

  Serial.printf("POST %s\n%s\n", INGEST_URL, body.c_str());

  HTTPClient http;
  http.setTimeout(8000);
  http.begin(INGEST_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Token", DEVICE_INGEST_TOKEN);
  const int code = http.POST(body);
  http.end();

  Serial.printf("POST status=%d\n", code);
  const bool ok = code >= 200 && code < 300;
  radioOff();
  return ok;
}
