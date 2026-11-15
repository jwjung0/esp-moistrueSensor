# esp-moistureSensor

ESP32-C3 capacitive soil moisture node: sample every **10 minutes**, deep sleep between wakes, upload a batch of **exactly 6** readings every **~1 hour** to mcp-home-assist (`POST /api/moisture/ingest`). SoftAP calibration portal (hold BOOT).

Server/TDB moisture storage is deferred — firmware posts the contract below; endpoint may 404 until the server is extended.

## Hardware

| Signal | Pin |
|--------|-----|
| Blue LED (on while Wi‑Fi/SoftAP active) | GPIO **8** |
| Sensor AOUT | GPIO **4** |
| Sensor VCC (GPIO-powered only while reading) | GPIO **3** |
| Sensor GND | GND |
| Enter calib | Hold **BOOT** (GPIO 9) ~2 s at wake |

Board: `esp32-c3-devkitm-1` (C3 SuperMini / DevKit). Sensor: Capacitive Soil Moisture Sensor v2.0 (3-pin).

## Setup

Create gitignored `include/config.h` with:

`INGEST_URL`, `DEVICE_ID`, `DEVICE_INGEST_TOKEN`, `ADC_DRY_DEFAULT`, `ADC_WET_DEFAULT`.

Home Wi‑Fi SSID/password are **not** in `config.h` — set them from your phone on the SoftAP portal (saved in NVS).

```bash
./scripts/build.sh
./scripts/flash.sh
pio device monitor
```

## Duty cycle

Defaults: **read every 10 min**, **ingest every 60 min** (batch of 6). Both are configurable in the SoftAP portal (NVS).

1. Wake → damped ADC read → append to RTC ring (capacity = ingest ÷ read).
2. If ring not full → deep sleep for the read interval.
3. If full → LED on → Wi‑Fi STA → NTP → `POST` full batch → LED/Wi‑Fi off → clear ring on success → deep sleep.

First upload waits until one full batch has accumulated. Ingest must be a multiple of read; batch size ≤ 24.

### ADC damping

On each read: VCC on → 300 ms settle → discard 4 samples → trimmed mean of 16 → VCC off. Duty-cycle path also applies 1% hysteresis vs last stored %.

## SoftAP setup portal

Opens automatically if home Wi‑Fi is not configured yet, or when you hold **BOOT** ~2 s at wake.

1. Join SoftAP `Moisture-XXXX`, open `http://192.168.4.1/`.
2. **Home Wi‑Fi**: enter SSID/password → **Apply Wi‑Fi** (needed so the device can reach the ingest server on your LAN).
3. **Intervals**: read / ingest minutes (defaults 10 / 60) → **Apply intervals**.
4. If calib never Applied: banner **“Calibration not set…”**.
5. **Calibrate Dry** / **Wet** (20 s each) → **Apply calibration**.
6. **Exit** → resume duty cycle.

SoftAP is only for phone setup. Hourly ingest uses home Wi‑Fi (STA).

## Ingest contract

```http
POST /api/moisture/ingest
Content-Type: application/json
X-Device-Token: <DEVICE_INGEST_TOKEN>
```

```json
{
  "device_id": "moisture01",
  "readings": [
    { "moisture_pct": 42.5, "measured_at": "2026-11-15T15:00:00Z" },
    { "moisture_pct": 42.0, "measured_at": "2026-11-15T15:10:00Z" },
    { "moisture_pct": 41.5, "measured_at": "2026-11-15T15:20:00Z" },
    { "moisture_pct": 41.0, "measured_at": "2026-11-15T15:30:00Z" },
    { "moisture_pct": 40.5, "measured_at": "2026-11-15T15:40:00Z" },
    { "moisture_pct": 40.0, "measured_at": "2026-11-15T15:50:00Z" }
  ]
}
```

Batch length = ingest_interval ÷ read_interval (default **6**). `moisture_pct` is 0–100 (on-device conversion).
