# esp-moistureSensor

ESP32-C3 capacitive soil moisture node: sample every **10 minutes**, deep sleep between wakes, upload a batch of **exactly 6** readings every **~1 hour** to mcp-home-assist (`POST /api/moisture/ingest`). SoftAP calibration portal (hold BOOT).

Server/TDB moisture storage is deferred — firmware posts the contract below; endpoint may 404 until the server is extended.

## Hardware

| Signal                                       | Pin                                 |
| -------------------------------------------- | ----------------------------------- |
| Blue LED (on while Wi‑Fi/SoftAP active)      | GPIO **8**                          |
| Sensor AOUT                                  | GPIO **4**                          |
| Sensor VCC (GPIO-powered only while reading) | GPIO **3**                          |
| Sensor GND                                   | GND                                 |
| Enter calib                                  | Hold **BOOT** (GPIO 9) ~2 s at wake |

Board: `esp32-c3-devkitm-1` (C3 SuperMini / DevKit). Sensor: Capacitive Soil Moisture Sensor v2.0 (3-pin).

## Power

- **Battery**: 1000 mAh Li-ion/LiPo
- **Charge / boost**: TP4056 (USB 5 V in) → cell → DC‑DC boost to **5 V** → board `5V` pin

### Estimated current (default 10 min / 60 min)

| State                     | Current                       | Duty        |
| ------------------------- | ----------------------------- | ----------- |
| Deep sleep (board @ 5 V)  | ~0.7 mA                       | ~99.6%      |
| Sample (sensor on ~0.4 s) | ~20–25 mA                     | 6×/h        |
| Wi‑Fi ingest              | ~80 mA avg (TX peaks ~300 mA) | ~5–10 s / h |

**Average ≈ 0.9–1.2 mA** → ~**3–6 weeks** on 1000 mAh. Sleep dominates; failed uploads (retry every 10 min) cut life toward ~2 weeks. SoftAP portal holds ~80 mA while open.

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

**Cold boot / reset** (not timer wake): if SoftAP did not open, the node stays awake ~**60 s**, prints NVS + sensor state, and runs a Wi‑Fi + ingest `GET` health check so USB serial can attach before deep sleep. Timer wakes skip this window.

First upload waits until one full batch has accumulated. Ingest must be a multiple of read; batch size ≤ 24.

### ADC damping

On each read: VCC on → 300 ms settle → discard 4 samples → trimmed mean of 16 → VCC off. Duty-cycle path also applies 1% hysteresis vs last stored %.

## SoftAP setup portal

Opens automatically if home Wi‑Fi is not configured yet, or when you hold **BOOT** ~2 s at wake.

1. On the phone, **connect** to SoftAP `Moisture-XXXX` (**open**, no password; `XXXX` = last 2 MAC bytes). Selecting the SSID alone is not enough — wait until it shows Connected / no internet.
2. Open the setup page: phone may show **Sign in to network**, or manually open `http://192.168.4.1/` in the browser. Stay on this Wi‑Fi (“use without internet” if asked).
3. **Home Wi‑Fi**: tap **Scan networks**, pick SSID, enter password → **Apply Wi‑Fi** (needed so the device can reach the ingest server on your LAN).
4. **Intervals**: read / ingest minutes (defaults 10 / 60) → **Apply intervals**.
5. If calib never Saved: banner **“Calibration not set…”**.
6. **Calibration** panel: **Dry** / **Wet** (20 s each; button locks to Completed). **Save** (both required) writes NVS. **Recalibrate** unlocks Dry/Wet again (NVS unchanged until Save).
7. **Exit** → resume duty cycle.

SoftAP stages: **before** home Wi‑Fi is saved → SoftAP opens automatically. **After** Wi‑Fi is saved → SoftAP only on **BOOT** hold (~2 s within ~5 s of wake). Idle exit after **3 min** with no phone joined; stays up while a client is connected. Hourly ingest uses home Wi‑Fi (STA).

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
