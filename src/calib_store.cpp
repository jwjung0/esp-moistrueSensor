#include "calib_store.h"

#if defined(INGEST_ENV_LOCAL)
#include "config_local.h"
#elif defined(INGEST_ENV_PROD)
#include "config_prod.h"
#else
#error "Build with INGEST_ENV_LOCAL or INGEST_ENV_PROD"
#endif

#include <Preferences.h>

static Preferences prefs;
static const char *kNs = "moist_cal";
static const char *kKeyInit = "init";
static const char *kKeyDry = "dry";
static const char *kKeyWet = "wet";
static const char *kKeyReadMin = "read_m";
static const char *kKeyIngestMin = "ing_m";

static bool gInitiated = false;
static int gDry = ADC_DRY_DEFAULT;
static int gWet = ADC_WET_DEFAULT;
static int gReadMin = kDefaultReadIntervalMin;
static int gIngestMin = kDefaultIngestIntervalMin;

static bool intervalsValid(int readMin, int ingestMin) {
  if (readMin < 1 || readMin > 180) {
    return false;
  }
  if (ingestMin < readMin || ingestMin > 24 * 60) {
    return false;
  }
  if ((ingestMin % readMin) != 0) {
    return false;
  }
  const int batch = ingestMin / readMin;
  return batch >= 1 && batch <= kMaxBatchCount;
}

void calibStoreBegin() {
  prefs.begin(kNs, true);
  gInitiated = prefs.getBool(kKeyInit, false);
  gDry = prefs.getInt(kKeyDry, ADC_DRY_DEFAULT);
  gWet = prefs.getInt(kKeyWet, ADC_WET_DEFAULT);
  gReadMin = prefs.getInt(kKeyReadMin, kDefaultReadIntervalMin);
  gIngestMin = prefs.getInt(kKeyIngestMin, kDefaultIngestIntervalMin);
  prefs.end();

  if (gDry <= gWet) {
    gDry = ADC_DRY_DEFAULT;
    gWet = ADC_WET_DEFAULT;
  }
  if (!intervalsValid(gReadMin, gIngestMin)) {
    gReadMin = kDefaultReadIntervalMin;
    gIngestMin = kDefaultIngestIntervalMin;
  }
}

bool calibIsInitiated() { return gInitiated; }

int calibAdcDry() { return gDry; }

int calibAdcWet() { return gWet; }

bool calibApply(int adcDry, int adcWet) {
  if (adcDry <= adcWet) {
    return false;
  }
  prefs.begin(kNs, false);
  prefs.putInt(kKeyDry, adcDry);
  prefs.putInt(kKeyWet, adcWet);
  prefs.putBool(kKeyInit, true);
  prefs.end();
  gDry = adcDry;
  gWet = adcWet;
  gInitiated = true;
  return true;
}

int calibReadIntervalMin() { return gReadMin; }

int calibIngestIntervalMin() { return gIngestMin; }

size_t calibBatchCount() {
  return (size_t)(gIngestMin / gReadMin);
}

bool calibApplyIntervals(int readIntervalMin, int ingestIntervalMin) {
  if (!intervalsValid(readIntervalMin, ingestIntervalMin)) {
    return false;
  }
  prefs.begin(kNs, false);
  prefs.putInt(kKeyReadMin, readIntervalMin);
  prefs.putInt(kKeyIngestMin, ingestIntervalMin);
  prefs.end();
  gReadMin = readIntervalMin;
  gIngestMin = ingestIntervalMin;
  return true;
}
