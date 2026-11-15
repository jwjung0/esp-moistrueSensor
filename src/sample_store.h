#pragma once

#include <stddef.h>
#include <stdint.h>

static const size_t kSampleCapacityMax = 24;

struct MoistureSample {
  int64_t measured_at_unix;
  float moisture_pct;
};

void sampleStoreBegin();
void sampleStoreReset();

// Sync ring capacity to calibBatchCount(); resets ring if capacity changed.
void sampleStoreSyncCapacity();

size_t sampleStoreCapacity();

// Append; if already full, overwrite oldest (ring stays at capacity).
void sampleStorePush(int64_t measuredAtUnix, float moisturePct);

size_t sampleStoreCount();
const MoistureSample *sampleStoreAt(size_t index);

int64_t sampleStoreWallUnix();
void sampleStoreSetWallUnix(int64_t unixSec);

float sampleStoreLastPct();
bool sampleStoreHasLastPct();
void sampleStoreSetLastPct(float pct);

// If any stamp is invalid, assign chronological unix times ending at endUnix
// spaced by intervalSec. No-op if all stamps already look valid.
void sampleStoreBackfillTimestamps(int64_t endUnix, int64_t intervalSec);
