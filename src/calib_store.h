#pragma once

#include <stdbool.h>
#include <stddef.h>

static const int kDefaultReadIntervalMin = 10;
static const int kDefaultIngestIntervalMin = 60;
static const int kMaxBatchCount = 24;

void calibStoreBegin();

bool calibIsInitiated();
int calibAdcDry();
int calibAdcWet();

// Returns false if dry <= wet.
bool calibApply(int adcDry, int adcWet);

int calibReadIntervalMin();
int calibIngestIntervalMin();
size_t calibBatchCount(); // ingest / read (always >= 1)

// Persist intervals. Requires ingest % read == 0, batch <= kMaxBatchCount.
bool calibApplyIntervals(int readIntervalMin, int ingestIntervalMin);
