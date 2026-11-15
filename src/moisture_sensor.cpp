#include "moisture_sensor.h"

#include "board_pins.h"

#include <Arduino.h>

static const uint32_t kSettleMs = 300;
static const int kDiscardReads = 4;
static const int kSampleCount = 16;
static const int kTrimEachSide = 2;
static const uint32_t kSampleGapMs = 3;

void moistureSensorBegin() {
  pinMode(MOISTURE_VCC_PIN, OUTPUT);
  digitalWrite(MOISTURE_VCC_PIN, LOW);
  analogReadResolution(12);
  pinMode(MOISTURE_AOUT_PIN, INPUT);
}

void moistureSensorPower(bool on) {
  digitalWrite(MOISTURE_VCC_PIN, on ? HIGH : LOW);
  if (on) {
    delay(kSettleMs);
  }
}

static int trimmedMean(int *samples, int n, int trim) {
  // Insertion sort (n is small).
  for (int i = 1; i < n; ++i) {
    const int key = samples[i];
    int j = i - 1;
    while (j >= 0 && samples[j] > key) {
      samples[j + 1] = samples[j];
      --j;
    }
    samples[j + 1] = key;
  }
  long sum = 0;
  const int start = trim;
  const int end = n - trim;
  for (int i = start; i < end; ++i) {
    sum += samples[i];
  }
  return (int)(sum / (end - start));
}

int moistureSensorReadRawPowered() {
  for (int i = 0; i < kDiscardReads; ++i) {
    (void)analogRead(MOISTURE_AOUT_PIN);
    delay(kSampleGapMs);
  }

  int samples[kSampleCount];
  for (int i = 0; i < kSampleCount; ++i) {
    samples[i] = analogRead(MOISTURE_AOUT_PIN);
    delay(kSampleGapMs);
  }
  return trimmedMean(samples, kSampleCount, kTrimEachSide);
}

int moistureSensorReadRaw() {
  moistureSensorPower(true);
  const int raw = moistureSensorReadRawPowered();
  moistureSensorPower(false);
  return raw;
}

float moistureSensorRawToPct(int rawAdc, int adcDry, int adcWet) {
  if (adcDry <= adcWet) {
    return 0.0f;
  }
  float pct =
      100.0f * (float)(adcDry - rawAdc) / (float)(adcDry - adcWet);
  if (pct < 0.0f) {
    pct = 0.0f;
  }
  if (pct > 100.0f) {
    pct = 100.0f;
  }
  return pct;
}
