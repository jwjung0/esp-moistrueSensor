#pragma once

void moistureSensorBegin();

// Power sensor, settle + damp, power off. Returns trimmed-mean raw ADC.
int moistureSensorReadRaw();

// Same as ReadRaw but leaves VCC on (for calib live sampling). Caller must
// call moistureSensorPower(false) when done.
void moistureSensorPower(bool on);
int moistureSensorReadRawPowered();

float moistureSensorRawToPct(int rawAdc, int adcDry, int adcWet);
