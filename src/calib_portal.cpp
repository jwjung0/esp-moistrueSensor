#include "calib_portal.h"

#include "board_pins.h"
#include "calib_store.h"
#include "led.h"
#include "moisture_sensor.h"

#include <WebServer.h>
#include <WiFi.h>
#include <math.h>

static WebServer server(80);

static const uint32_t kCaptureMs = 20000;
static const uint32_t kIgnoreMs = 3000;
static const uint32_t kIdleTimeoutMs = 10UL * 60UL * 1000UL;

enum CaptureMode { CAP_NONE = 0, CAP_DRY, CAP_WET };

static volatile bool gExit = false;
static CaptureMode gMode = CAP_NONE;
static uint32_t gCaptureStartMs = 0;
static double gSum = 0;
static uint32_t gSumCount = 0;
static int gLiveRaw = 0;
static int gDraftDry = -1;
static int gDraftWet = -1;
static String gMessage;
static uint32_t gLastActivityMs = 0;

static int effectiveDry() {
  return gDraftDry >= 0 ? gDraftDry : calibAdcDry();
}

static int effectiveWet() {
  return gDraftWet >= 0 ? gDraftWet : calibAdcWet();
}

static const char *PAGE_HTML = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>Moisture Calib</title>
<style>
  :root { --bg:#0f1410; --fg:#e8f0e6; --muted:#8aa08a; --acc:#3d9a5f; --warn:#c9a227; --bad:#c44; }
  * { box-sizing:border-box; }
  body { margin:0; font-family:ui-sans-serif,system-ui,sans-serif; background:var(--bg); color:var(--fg); }
  main { max-width:420px; margin:0 auto; padding:1.25rem; }
  h1 { font-size:1.25rem; margin:0 0 .75rem; font-weight:600; }
  .banner { background:#3a2e0a; color:#ffe9a8; border:1px solid var(--warn); padding:.75rem; border-radius:8px; margin-bottom:1rem; font-size:.9rem; }
  .ok { background:#14301c; color:#b6f0c8; border-color:var(--acc); }
  .card { background:#171d18; border:1px solid #2a332b; border-radius:10px; padding:1rem; margin-bottom:1rem; }
  .row { display:flex; gap:.5rem; flex-wrap:wrap; }
  button { flex:1; min-width:120px; padding:.7rem .9rem; border-radius:8px; border:0; font-size:.95rem; cursor:pointer; background:#2a332b; color:var(--fg); }
  button.primary { background:var(--acc); color:#04140a; font-weight:600; }
  button:disabled { opacity:.45; cursor:not-allowed; }
  .meta { color:var(--muted); font-size:.85rem; line-height:1.5; }
  .big { font-size:1.6rem; font-variant-numeric:tabular-nums; }
  .err { color:#ffb4b4; font-size:.85rem; margin-top:.5rem; }
  label { display:block; color:var(--muted); font-size:.8rem; margin:.5rem 0 .25rem; }
  input[type=number] { width:100%; padding:.55rem .65rem; border-radius:8px; border:1px solid #2a332b; background:#0f1410; color:var(--fg); font-size:1rem; }
</style>
</head>
<body>
<main>
  <h1>Soil moisture setup</h1>
  <div id="banner" class="banner" hidden></div>
  <div class="card">
    <div class="meta"><strong>Intervals</strong> (minutes)</div>
    <label for="readMin">Data read interval</label>
    <input id="readMin" type="number" min="1" max="180" step="1"/>
    <label for="ingestMin">Data ingest interval</label>
    <input id="ingestMin" type="number" min="1" max="1440" step="1"/>
    <div class="meta" id="batchHint" style="margin-top:.5rem"></div>
    <div class="row" style="margin-top:.75rem">
      <button class="primary" id="btnIntervals" onclick="applyIntervals()">Apply intervals</button>
    </div>
  </div>
  <div class="card">
    <div class="meta">Live ADC</div>
    <div class="big" id="live">—</div>
    <div class="meta" id="livePct"></div>
    <div class="meta" id="timer"></div>
  </div>
  <div class="card">
    <div class="meta">Draft / applied</div>
    <div class="meta">Dry ADC: <strong id="dry">—</strong></div>
    <div class="meta">Wet ADC: <strong id="wet">—</strong></div>
    <div class="meta">Preview % (live): <strong id="pct">—</strong></div>
  </div>
  <div class="row" style="margin-bottom:.75rem">
    <button id="btnDry" onclick="start('dry')">Calibrate Dry</button>
    <button id="btnWet" onclick="start('wet')">Calibrate Wet</button>
  </div>
  <div class="row" style="margin-bottom:.75rem">
    <button class="primary" id="btnApply" onclick="apply()">Apply calibration</button>
    <button id="btnExit" onclick="exitPortal()">Exit</button>
  </div>
  <div class="meta">Dry: probe in air. Wet: to max line in water (not electronics). Each run averages ~20s (skips first 3s). Ingest must be a multiple of read (batch ≤ 24).</div>
  <div class="err" id="err"></div>
</main>
<script>
let intervalsLoaded=false;
async function refresh(){
  try{
    const r=await fetch('/api/status');
    const j=await r.json();
    const b=document.getElementById('banner');
    if(!j.initiated){
      b.hidden=false; b.className='banner';
      b.textContent='Calibration not set — run Dry and Wet, then Apply.';
    }else{
      b.hidden=false; b.className='banner ok';
      b.textContent='Calibration saved in device memory.';
    }
    if(!intervalsLoaded){
      document.getElementById('readMin').value=j.read_interval_min;
      document.getElementById('ingestMin').value=j.ingest_interval_min;
      intervalsLoaded=true;
    }
    document.getElementById('batchHint').textContent=
      'Batch size: '+j.batch_count+' samples per upload (defaults 10 / 60 → 6).';
    document.getElementById('live').textContent=j.live_raw;
    document.getElementById('dry').textContent=j.draft_dry>=0?j.draft_dry:j.adc_dry;
    document.getElementById('wet').textContent=j.draft_wet>=0?j.draft_wet:j.adc_wet;
    document.getElementById('pct').textContent=j.preview_pct.toFixed(1)+'%';
    const busy=j.capturing;
    document.getElementById('btnDry').disabled=busy;
    document.getElementById('btnWet').disabled=busy;
    document.getElementById('btnApply').disabled=busy;
    document.getElementById('btnIntervals').disabled=busy;
    if(busy){
      document.getElementById('timer').textContent='Capturing '+j.mode+'… '+(j.remain_s)+'s left (avg so far '+j.running_avg+')';
    }else{
      document.getElementById('timer').textContent=j.message||'';
    }
    document.getElementById('err').textContent=j.error||'';
  }catch(e){}
}
async function start(mode){
  document.getElementById('err').textContent='';
  await fetch('/api/capture?mode='+mode,{method:'POST'});
  refresh();
}
async function apply(){
  const r=await fetch('/api/apply',{method:'POST'});
  const j=await r.json();
  if(!j.ok) document.getElementById('err').textContent=j.error||'Apply failed';
  refresh();
}
async function applyIntervals(){
  document.getElementById('err').textContent='';
  const read_min=parseInt(document.getElementById('readMin').value,10);
  const ingest_min=parseInt(document.getElementById('ingestMin').value,10);
  const r=await fetch('/api/intervals?read_min='+read_min+'&ingest_min='+ingest_min,{method:'POST'});
  const j=await r.json();
  if(!j.ok){
    document.getElementById('err').textContent=j.error||'Intervals apply failed';
  }else{
    intervalsLoaded=false;
  }
  refresh();
}
async function exitPortal(){
  await fetch('/api/exit',{method:'POST'});
  document.getElementById('err').textContent='Exiting… you can disconnect.';
}
setInterval(refresh,500);
refresh();
</script>
</body>
</html>
)HTML";

static void touchActivity() { gLastActivityMs = millis(); }

static void handleRoot() {
  touchActivity();
  server.send(200, "text/html", PAGE_HTML);
}

static void handleStatus() {
  touchActivity();
  const int dry = effectiveDry();
  const int wet = effectiveWet();
  const float preview = moistureSensorRawToPct(gLiveRaw, dry, wet);

  uint32_t remainS = 0;
  int runningAvg = 0;
  const bool capturing = gMode != CAP_NONE;
  if (capturing) {
    const uint32_t elapsed = millis() - gCaptureStartMs;
    remainS = elapsed >= kCaptureMs ? 0 : (kCaptureMs - elapsed + 999) / 1000;
    if (gSumCount > 0) {
      runningAvg = (int)lround(gSum / (double)gSumCount);
    }
  }

  String json = "{";
  json += "\"initiated\":";
  json += calibIsInitiated() ? "true" : "false";
  json += ",\"adc_dry\":";
  json += String(calibAdcDry());
  json += ",\"adc_wet\":";
  json += String(calibAdcWet());
  json += ",\"draft_dry\":";
  json += String(gDraftDry);
  json += ",\"draft_wet\":";
  json += String(gDraftWet);
  json += ",\"live_raw\":";
  json += String(gLiveRaw);
  json += ",\"preview_pct\":";
  json += String(preview, 2);
  json += ",\"capturing\":";
  json += capturing ? "true" : "false";
  json += ",\"mode\":\"";
  json += gMode == CAP_DRY ? "dry" : (gMode == CAP_WET ? "wet" : "");
  json += "\",\"remain_s\":";
  json += String(remainS);
  json += ",\"running_avg\":";
  json += String(runningAvg);
  json += ",\"read_interval_min\":";
  json += String(calibReadIntervalMin());
  json += ",\"ingest_interval_min\":";
  json += String(calibIngestIntervalMin());
  json += ",\"batch_count\":";
  json += String((unsigned)calibBatchCount());
  json += ",\"message\":\"";
  json += gMessage;
  json += "\",\"error\":\"\"}";
  server.send(200, "application/json", json);
}

static void handleCapture() {
  touchActivity();
  if (gMode != CAP_NONE) {
    server.send(409, "application/json",
                "{\"ok\":false,\"error\":\"capture in progress\"}");
    return;
  }
  String mode = server.hasArg("mode") ? server.arg("mode") : "";
  if (mode != "dry" && mode != "wet") {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"mode=dry|wet required\"}");
    return;
  }
  gMode = mode == "dry" ? CAP_DRY : CAP_WET;
  gCaptureStartMs = millis();
  gSum = 0;
  gSumCount = 0;
  gMessage = "";
  moistureSensorPower(true);
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleApply() {
  touchActivity();
  const int dry = effectiveDry();
  const int wet = effectiveWet();
  if (dry <= wet) {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"dry ADC must be greater than wet "
                "ADC\"}");
    return;
  }
  if (!calibApply(dry, wet)) {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"apply failed\"}");
    return;
  }
  gDraftDry = dry;
  gDraftWet = wet;
  gMessage = "Applied to NVS";
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleIntervals() {
  touchActivity();
  if (!server.hasArg("read_min") || !server.hasArg("ingest_min")) {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"read_min and ingest_min required\"}");
    return;
  }
  const int readMin = server.arg("read_min").toInt();
  const int ingestMin = server.arg("ingest_min").toInt();
  if (!calibApplyIntervals(readMin, ingestMin)) {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"invalid intervals (ingest must be a "
                "multiple of read; batch 1–24)\"}");
    return;
  }
  gMessage = "Intervals saved";
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleExit() {
  touchActivity();
  gExit = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

static void finishCapture() {
  int avg = gSumCount > 0 ? (int)lround(gSum / (double)gSumCount) : gLiveRaw;
  if (gMode == CAP_DRY) {
    gDraftDry = avg;
    gMessage = "Dry draft updated";
  } else if (gMode == CAP_WET) {
    gDraftWet = avg;
    gMessage = "Wet draft updated";
  }
  gMode = CAP_NONE;
  moistureSensorPower(false);
}

static void captureTick() {
  if (gMode == CAP_NONE) {
    // Idle live peek: brief powered read occasionally is heavy; keep VCC off
    // and do a full damped read every status poll path instead via timer.
    return;
  }
  const uint32_t elapsed = millis() - gCaptureStartMs;
  // Sample ~every 50ms while powered.
  static uint32_t lastSampleMs = 0;
  if (millis() - lastSampleMs < 50) {
    return;
  }
  lastSampleMs = millis();

  const int raw = analogRead(MOISTURE_AOUT_PIN);
  gLiveRaw = raw;
  if (elapsed >= kIgnoreMs) {
    gSum += raw;
    gSumCount++;
  }
  if (elapsed >= kCaptureMs) {
    finishCapture();
  }
}

static void idleLiveTick() {
  if (gMode != CAP_NONE) {
    return;
  }
  static uint32_t lastMs = 0;
  if (millis() - lastMs < 1000) {
    return;
  }
  lastMs = millis();
  gLiveRaw = moistureSensorReadRaw();
}

void calibPortalRun() {
  gExit = false;
  gMode = CAP_NONE;
  gDraftDry = -1;
  gDraftWet = -1;
  gMessage = "";
  gLastActivityMs = millis();

  ledSetWifiActive(true);
  moistureSensorBegin();

  WiFi.mode(WIFI_AP);
  uint8_t mac[6];
  WiFi.softAPmacAddress(mac);
  char ssid[32];
  snprintf(ssid, sizeof(ssid), "Moisture-%02X%02X", mac[4], mac[5]);
  WiFi.softAP(ssid);
  Serial.printf("Calib SoftAP \"%s\"  http://%s/\n", ssid,
                WiFi.softAPIP().toString().c_str());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/capture", HTTP_POST, handleCapture);
  server.on("/api/apply", HTTP_POST, handleApply);
  server.on("/api/intervals", HTTP_POST, handleIntervals);
  server.on("/api/exit", HTTP_POST, handleExit);
  server.begin();

  while (!gExit) {
    server.handleClient();
    captureTick();
    idleLiveTick();
    if ((millis() - gLastActivityMs) > kIdleTimeoutMs) {
      Serial.println("Calib portal idle timeout");
      break;
    }
    delay(2);
  }

  if (gMode != CAP_NONE) {
    moistureSensorPower(false);
    gMode = CAP_NONE;
  }
  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  ledSetWifiActive(false);
  Serial.println("Calib portal exited");
}
