#pragma once

#include <stdbool.h>

// LED on, STA connect, NTP, POST exactly 6 samples, LED/radio off.
// Returns true on HTTP 2xx.
bool wifiUploadBatch();

// LED on, STA connect, NTP, GET INGEST_URL, LED/radio off.
// Returns true if any HTTP status was received (endpoint reachable).
bool wifiHealthCheck();
