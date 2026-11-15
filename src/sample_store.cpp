#include "sample_store.h"

#include "calib_store.h"

#include <Arduino.h>
#include <string.h>

static const uint32_t kRtcMagic = 0x4D4F5337; // 'MOS7' — variable capacity

struct RtcState {
  uint32_t magic;
  uint8_t count;
  uint8_t head;
  uint8_t capacity;
  uint8_t pad;
  MoistureSample samples[kSampleCapacityMax];
  int64_t wall_unix;
  float last_pct;
  uint8_t has_last_pct;
  uint8_t pad2[3];
};

RTC_DATA_ATTR static RtcState rtc;

static bool rtcValid() { return rtc.magic == kRtcMagic; }

static uint8_t clampCapacity(size_t n) {
  if (n < 1) {
    return 1;
  }
  if (n > kSampleCapacityMax) {
    return (uint8_t)kSampleCapacityMax;
  }
  return (uint8_t)n;
}

void sampleStoreReset() {
  const uint8_t cap = rtc.capacity ? rtc.capacity : clampCapacity(calibBatchCount());
  memset(&rtc, 0, sizeof(rtc));
  rtc.magic = kRtcMagic;
  rtc.count = 0;
  rtc.head = 0;
  rtc.capacity = cap;
  rtc.wall_unix = 0;
  rtc.last_pct = 0;
  rtc.has_last_pct = 0;
}

void sampleStoreSyncCapacity() {
  const uint8_t want = clampCapacity(calibBatchCount());
  if (!rtcValid()) {
    rtc.capacity = want;
    sampleStoreReset();
    return;
  }
  if (rtc.capacity != want) {
    rtc.capacity = want;
    sampleStoreReset();
  }
}

void sampleStoreBegin() {
  if (!rtcValid()) {
    rtc.capacity = clampCapacity(calibBatchCount());
    sampleStoreReset();
  } else {
    sampleStoreSyncCapacity();
  }
}

size_t sampleStoreCapacity() {
  return rtc.capacity ? rtc.capacity : clampCapacity(calibBatchCount());
}

void sampleStorePush(int64_t measuredAtUnix, float moisturePct) {
  const uint8_t cap = (uint8_t)sampleStoreCapacity();
  MoistureSample s;
  s.measured_at_unix = measuredAtUnix;
  s.moisture_pct = moisturePct;

  if (rtc.count < cap) {
    rtc.samples[rtc.count] = s;
    rtc.count++;
  } else {
    rtc.samples[rtc.head] = s;
    rtc.head = (uint8_t)((rtc.head + 1) % cap);
    rtc.count = cap;
  }
  rtc.last_pct = moisturePct;
  rtc.has_last_pct = 1;
}

size_t sampleStoreCount() { return rtc.count; }

const MoistureSample *sampleStoreAt(size_t index) {
  if (index >= rtc.count) {
    return nullptr;
  }
  const uint8_t cap = (uint8_t)sampleStoreCapacity();
  if (rtc.count < cap) {
    return &rtc.samples[index];
  }
  const size_t physical = (rtc.head + index) % cap;
  return &rtc.samples[physical];
}

int64_t sampleStoreWallUnix() { return rtc.wall_unix; }

void sampleStoreSetWallUnix(int64_t unixSec) { rtc.wall_unix = unixSec; }

float sampleStoreLastPct() { return rtc.last_pct; }

bool sampleStoreHasLastPct() { return rtc.has_last_pct != 0; }

void sampleStoreSetLastPct(float pct) {
  rtc.last_pct = pct;
  rtc.has_last_pct = 1;
}

void sampleStoreBackfillTimestamps(int64_t endUnix, int64_t intervalSec) {
  if (rtc.count == 0 || endUnix < 1700000000LL || intervalSec <= 0) {
    return;
  }
  bool anyInvalid = false;
  for (uint8_t i = 0; i < rtc.count; ++i) {
    const MoistureSample *s = sampleStoreAt(i);
    if (!s || s->measured_at_unix < 1700000000LL) {
      anyInvalid = true;
      break;
    }
  }
  if (!anyInvalid) {
    return;
  }

  MoistureSample copy[kSampleCapacityMax];
  const uint8_t n = rtc.count;
  for (uint8_t i = 0; i < n; ++i) {
    copy[i] = *sampleStoreAt(i);
    copy[i].measured_at_unix = endUnix - (int64_t)(n - 1 - i) * intervalSec;
  }
  const float lastPct = rtc.last_pct;
  const uint8_t hasLast = rtc.has_last_pct;
  const uint8_t cap = rtc.capacity;
  sampleStoreReset();
  rtc.capacity = cap;
  for (uint8_t i = 0; i < n; ++i) {
    sampleStorePush(copy[i].measured_at_unix, copy[i].moisture_pct);
  }
  rtc.last_pct = lastPct;
  rtc.has_last_pct = hasLast;
  sampleStoreSetWallUnix(endUnix);
}
