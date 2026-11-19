#include "calib_portal.h"

#include "board_pins.h"
#include "calib_store.h"
#include "led.h"
#include "moisture_sensor.h"
#include "wifi_creds.h"

#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include <math.h>

static DNSServer dnsServer;
static WebServer server(80);

static const uint32_t kCaptureMs = 20000;
static const uint32_t kIgnoreMs = 3000;
// No phone associated + no HTTP → exit. Timer resets while a station is joined.
static const uint32_t kIdleTimeoutMs = 3UL * 60UL * 1000UL;

static char gApSsid[32] = {0};

// SoftAP-only. Tear down STA first — after home Wi‑Fi was used, leftover STA
// state is what made SoftAP invisible on reopen (BOOT path).
// C3 SuperMini: lower TX power *after* softAP().
static void softApStart() {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(200);
  WiFi.mode(WIFI_AP);
  delay(100);
  const bool ok = WiFi.softAP(gApSsid);
  delay(50);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  Serial.printf("SoftAP start \"%s\" ok=%d ip=%s tx=%d\n", gApSsid, (int)ok,
                WiFi.softAPIP().toString().c_str(), (int)WiFi.getTxPower());
}

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

static int effectiveDry() { return gDraftDry >= 0 ? gDraftDry : calibAdcDry(); }

static int effectiveWet() { return gDraftWet >= 0 ? gDraftWet : calibAdcWet(); }

static String jsonEscape(const String &in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); ++i) {
    const char c = in[i];
    if (c == '"' || c == '\\') {
      out += '\\';
    }
    out += c;
  }
  return out;
}

static const char *PAGE_HTML = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>Moisture Setup</title>
<style>
  :root { --bg:#0f1410; --fg:#e8f0e6; --muted:#8aa08a; --acc:#3d9a5f; --warn:#c9a227; }
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
  button.primary:disabled {
    background:#2a332b; color:var(--muted); font-weight:600; opacity:1;
  }
  .meta { color:var(--muted); font-size:.85rem; line-height:1.5; }
  .big { font-size:1.6rem; font-variant-numeric:tabular-nums; }
  .err { color:#ffb4b4; font-size:.85rem; margin-top:.5rem; }
  label { display:block; color:var(--muted); font-size:.8rem; margin:.5rem 0 .25rem; }
  input[type=number], input[type=text], input[type=password], select {
    width:100%; padding:.55rem .65rem; border-radius:8px; border:1px solid #2a332b;
    background:#0f1410; color:var(--fg); font-size:1rem;
  }
</style>
</head>
<body>
<main>
  <h1>Soil moisture setup</h1>
  <div id="wifiBanner" class="banner" hidden></div>
  <div id="banner" class="banner" hidden></div>
  <div class="card">
    <div class="meta"><strong>Home Wi‑Fi</strong> (for ingest uploads)</div>
    <label for="ssid">SSID</label>
    <select id="ssid"><option value="">Scanning…</option></select>
    <div class="row" style="margin-top:.5rem">
      <button id="btnScan" onclick="scanWifi()">Scan networks</button>
    </div>
    <label for="wpass">Password</label>
    <input id="wpass" type="password" maxlength="63" autocomplete="off"/>
    <div class="meta" id="wifiHint" style="margin-top:.5rem"></div>
    <div class="row" style="margin-top:.75rem">
      <button class="primary" id="btnWifi" onclick="applyWifi()">Apply Wi‑Fi</button>
    </div>
  </div>
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
    <div class="meta"><strong>Calibration</strong></div>
    <div class="meta">Live ADC</div>
    <div class="big" id="live">—</div>
    <div class="meta">Preview %: <strong id="pct">—</strong></div>
    <div class="meta" id="timer" style="margin-top:.5rem"></div>
    <div class="row" style="margin-top:.75rem;align-items:center">
      <button class="primary" id="btnDry" onclick="start('dry')">Dry</button>
      <div class="meta" id="dryStatus">—</div>
    </div>
    <div class="row" style="margin-top:.5rem;align-items:center">
      <button class="primary" id="btnWet" onclick="start('wet')">Wet</button>
      <div class="meta" id="wetStatus">—</div>
    </div>
    <div class="row" style="margin-top:.75rem">
      <button class="primary" id="btnSave" onclick="saveCalib()">Save</button>
      <button id="btnRecal" onclick="recalibrate()">Recalibrate</button>
    </div>
  </div>
  <div class="row" style="margin-bottom:.75rem">
    <button id="btnExit" onclick="exitPortal()">Exit</button>
  </div>
  <div class="meta">Join this SoftAP from your phone, set home Wi‑Fi for uploads, then calibrate. Ingest must be a multiple of read (batch ≤ 24).</div>
  <div class="err" id="err"></div>
</main>
<script>
let intervalsLoaded=false;
let savedSsid='';
let scanning=false;
function fillSsidSelect(networks, prefer){
  const sel=document.getElementById('ssid');
  const prev=sel.value||prefer||'';
  sel.innerHTML='';
  if(!networks.length){
    const o=document.createElement('option');
    o.value=''; o.textContent='No networks found — tap Scan';
    sel.appendChild(o);
    return;
  }
  networks.forEach(n=>{
    const o=document.createElement('option');
    o.value=n.ssid;
    o.textContent=n.ssid+' ('+n.rssi+' dBm'+(n.secure?', locked':'')+')';
    sel.appendChild(o);
  });
  if(prev){
    const hit=[...sel.options].some(o=>o.value===prev);
    if(hit) sel.value=prev;
    else{
      const o=document.createElement('option');
      o.value=prev; o.textContent=prev+' (saved)';
      sel.insertBefore(o, sel.firstChild);
      sel.value=prev;
    }
  }
}
async function scanWifi(){
  if(scanning) return;
  scanning=true;
  document.getElementById('err').textContent='';
  document.getElementById('btnScan').disabled=true;
  document.getElementById('wifiHint').textContent='Scanning…';
  try{
    const r=await fetch('/api/scan');
    const j=await r.json();
    if(!j.ok){
      document.getElementById('err').textContent=j.error||'Scan failed';
      fillSsidSelect([], savedSsid);
    }else{
      fillSsidSelect(j.networks||[], savedSsid);
      document.getElementById('wifiHint').textContent=
        (j.networks||[]).length+' network(s) — select SSID, enter password, Apply.';
    }
  }catch(e){
    document.getElementById('err').textContent='Scan failed';
  }
  scanning=false;
  document.getElementById('btnScan').disabled=false;
}
async function refresh(){
  try{
    const r=await fetch('/api/status');
    const j=await r.json();
    const wb=document.getElementById('wifiBanner');
    if(!j.wifi_configured){
      wb.hidden=false; wb.className='banner';
      wb.textContent='Home Wi‑Fi not set — select SSID, enter password, Apply Wi‑Fi.';
    }else{
      wb.hidden=false; wb.className='banner ok';
      wb.textContent='Home Wi‑Fi saved ('+j.wifi_ssid+').';
    }
    const b=document.getElementById('banner');
    if(!j.initiated){
      b.hidden=false; b.className='banner';
      b.textContent='Calibration not set — run Dry and Wet, then Save.';
    }else{
      b.hidden=false; b.className='banner ok';
      b.textContent='Calibration saved in device memory.';
    }
    savedSsid=j.wifi_ssid||'';
    if(!intervalsLoaded){
      document.getElementById('readMin').value=j.read_interval_min;
      document.getElementById('ingestMin').value=j.ingest_interval_min;
      intervalsLoaded=true;
    }
    document.getElementById('batchHint').textContent=
      'Batch size: '+j.batch_count+' samples per upload (defaults 10 / 60 → 6).';
    document.getElementById('live').textContent=j.live_raw;
    document.getElementById('pct').textContent=j.preview_pct.toFixed(1)+'%';

    const dryDone=j.draft_dry>=0;
    const wetDone=j.draft_wet>=0;
    const dryVal=dryDone?j.draft_dry:j.adc_dry;
    const wetVal=wetDone?j.draft_wet:j.adc_wet;
    document.getElementById('dryStatus').textContent=
      dryDone?('Completed — ADC '+dryVal):(j.initiated?('Saved ADC '+j.adc_dry):'Not captured');
    document.getElementById('wetStatus').textContent=
      wetDone?('Completed — ADC '+wetVal):(j.initiated?('Saved ADC '+j.adc_wet):'Not captured');
    document.getElementById('btnDry').textContent=dryDone?'Completed':'Dry';
    document.getElementById('btnWet').textContent=wetDone?'Completed':'Wet';

    const capturing=!!j.capturing;
    const busy=capturing||scanning;
    document.getElementById('btnDry').disabled=busy||dryDone;
    document.getElementById('btnWet').disabled=busy||wetDone;
    document.getElementById('btnSave').disabled=
      busy||!dryDone||!wetDone||!(j.draft_dry>j.draft_wet);
    document.getElementById('btnRecal').disabled=
      busy||!(dryDone||wetDone||j.initiated);
    document.getElementById('btnIntervals').disabled=busy;
    document.getElementById('btnWifi').disabled=busy;
    document.getElementById('btnScan').disabled=busy;

    if(capturing){
      document.getElementById('timer').textContent=
        'Capturing '+j.mode+'… '+j.remain_s+'s left (avg '+j.running_avg+')';
    }else{
      document.getElementById('timer').textContent=j.message||'';
    }
  }catch(e){}
}
async function start(mode){
  document.getElementById('err').textContent='';
  const r=await fetch('/api/capture?mode='+mode,{method:'POST'});
  const j=await r.json();
  if(!j.ok) document.getElementById('err').textContent=j.error||'Capture failed';
  refresh();
}
async function saveCalib(){
  document.getElementById('err').textContent='';
  const r=await fetch('/api/apply',{method:'POST'});
  const j=await r.json();
  if(!j.ok) document.getElementById('err').textContent=j.error||'Save failed';
  refresh();
}
async function recalibrate(){
  document.getElementById('err').textContent='';
  const r=await fetch('/api/recalibrate',{method:'POST'});
  const j=await r.json();
  if(!j.ok) document.getElementById('err').textContent=j.error||'Recalibrate failed';
  refresh();
}
async function applyWifi(){
  document.getElementById('err').textContent='';
  const ssid=document.getElementById('ssid').value;
  if(!ssid){
    document.getElementById('err').textContent='Select an SSID from the list';
    return;
  }
  const pass=encodeURIComponent(document.getElementById('wpass').value);
  const r=await fetch('/api/wifi?ssid='+encodeURIComponent(ssid)+'&pass='+pass,{method:'POST'});
  const j=await r.json();
  if(!j.ok){
    document.getElementById('err').textContent=j.error||'Wi‑Fi apply failed';
  }else{
    savedSsid=ssid;
    document.getElementById('wpass').value='';
  }
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
// Do not auto-scan on load — scan channel-hops and can drop SoftAP on C3.
</script>
</body>
</html>
)HTML";

static void touchActivity() { gLastActivityMs = millis(); }

static void handleRoot() {
  touchActivity();
  server.send(200, "text/html", PAGE_HTML);
}

// Phones probe these URLs after join; serve the portal so a "Sign in" sheet
// appears.
static void handleCaptive() {
  touchActivity();
  server.sendHeader("Cache-Control", "no-cache");
  if (server.method() == HTTP_GET) {
    server.send(200, "text/html", PAGE_HTML);
  } else {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  }
}

static void handleNotFound() {
  touchActivity();
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
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
  json += ",\"wifi_configured\":";
  json += wifiCredsConfigured() ? "true" : "false";
  json += ",\"wifi_ssid\":\"";
  json += jsonEscape(wifiCredsSsid());
  json += "\",\"adc_dry\":";
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
  json += jsonEscape(gMessage);
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
  if (mode == "dry" && gDraftDry >= 0) {
    server.send(409, "application/json",
                "{\"ok\":false,\"error\":\"dry already captured — Recalibrate "
                "to redo\"}");
    return;
  }
  if (mode == "wet" && gDraftWet >= 0) {
    server.send(409, "application/json",
                "{\"ok\":false,\"error\":\"wet already captured — Recalibrate "
                "to redo\"}");
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
  if (gDraftDry < 0 || gDraftWet < 0) {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"capture both Dry and Wet first\"}");
    return;
  }
  const int dry = gDraftDry;
  const int wet = gDraftWet;
  if (dry <= wet) {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"dry ADC must be greater than wet "
                "ADC\"}");
    return;
  }
  if (!calibApply(dry, wet)) {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"save failed\"}");
    return;
  }
  gMessage = "Saved to NVS";
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleRecalibrate() {
  touchActivity();
  if (gMode != CAP_NONE) {
    server.send(409, "application/json",
                "{\"ok\":false,\"error\":\"capture in progress\"}");
    return;
  }
  gDraftDry = -1;
  gDraftWet = -1;
  gMessage = "Recalibrate — run Dry and Wet, then Save";
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleWifi() {
  touchActivity();
  if (!server.hasArg("ssid")) {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"ssid required\"}");
    return;
  }
  const String ssid = server.arg("ssid");
  const String pass = server.hasArg("pass") ? server.arg("pass") : "";
  if (!wifiCredsApply(ssid, pass)) {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"invalid SSID/password\"}");
    return;
  }
  gMessage = "Wi-Fi saved";
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleScan() {
  touchActivity();
  // AP-only SoftAP can't scan; briefly enable STA, then restore WIFI_AP.
  WiFi.mode(WIFI_AP_STA);
  delay(50);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  const int n = WiFi.scanNetworks(/*async=*/false, /*hidden=*/false);

  String json;
  if (n < 0) {
    json = "{\"ok\":false,\"error\":\"scan failed\"}";
  } else {
    json = "{\"ok\":true,\"networks\":[";
    int outCount = 0;
    for (int i = 0; i < n; ++i) {
      const String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) {
        continue;
      }
      bool seen = false;
      for (int j = 0; j < i; ++j) {
        if (WiFi.SSID(j) == ssid) {
          seen = true;
          break;
        }
      }
      if (seen) {
        continue;
      }
      int bestRssi = WiFi.RSSI(i);
      wifi_auth_mode_t bestEnc = WiFi.encryptionType(i);
      for (int j = i + 1; j < n; ++j) {
        if (WiFi.SSID(j) == ssid && WiFi.RSSI(j) > bestRssi) {
          bestRssi = WiFi.RSSI(j);
          bestEnc = WiFi.encryptionType(j);
        }
      }
      if (outCount > 0) {
        json += ',';
      }
      json += "{\"ssid\":\"";
      json += jsonEscape(ssid);
      json += "\",\"rssi\":";
      json += String(bestRssi);
      json += ",\"secure\":";
      json += (bestEnc != WIFI_AUTH_OPEN) ? "true" : "false";
      json += '}';
      ++outCount;
      if (outCount >= 24) {
        break;
      }
    }
    json += "]}";
    WiFi.scanDelete();
  }

  softApStart();
  if (n < 0) {
    server.send(500, "application/json", json);
  } else {
    server.send(200, "application/json", json);
  }
}

static void handleIntervals() {
  touchActivity();
  if (!server.hasArg("read_min") || !server.hasArg("ingest_min")) {
    server.send(
        400, "application/json",
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
    gMessage = "Dry completed";
  } else if (gMode == CAP_WET) {
    gDraftWet = avg;
    gMessage = "Wet completed";
  }
  gMode = CAP_NONE;
  moistureSensorPower(false);
}

static void captureTick() {
  if (gMode == CAP_NONE) {
    return;
  }
  const uint32_t elapsed = millis() - gCaptureStartMs;
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

  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(gApSsid, sizeof(gApSsid), "Moisture-%02X%02X", mac[4], mac[5]);
  softApStart();
  Serial.println(
      "1) Join SoftAP (open, no password)  2) Open http://192.168.4.1/ "
      "(or phone Sign-in sheet)");

  // Captive DNS: any hostname → AP IP so the phone opens the portal.
  const IPAddress apIp = WiFi.softAPIP();
  dnsServer.start(53, "*", apIp);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/scan", HTTP_GET, handleScan);
  server.on("/api/capture", HTTP_POST, handleCapture);
  server.on("/api/apply", HTTP_POST, handleApply);
  server.on("/api/recalibrate", HTTP_POST, handleRecalibrate);
  server.on("/api/wifi", HTTP_POST, handleWifi);
  server.on("/api/intervals", HTTP_POST, handleIntervals);
  server.on("/api/exit", HTTP_POST, handleExit);
  // Android / iOS / Windows connectivity checks
  server.on("/generate_204", HTTP_ANY, handleCaptive);
  server.on("/gen_204", HTTP_ANY, handleCaptive);
  server.on("/hotspot-detect.html", HTTP_ANY, handleCaptive);
  server.on("/library/test/success.html", HTTP_ANY, handleCaptive);
  server.on("/connecttest.txt", HTTP_ANY, handleCaptive);
  server.on("/ncsi.txt", HTTP_ANY, handleCaptive);
  server.on("/fwlink", HTTP_ANY, handleCaptive);
  server.onNotFound(handleNotFound);
  server.begin();

  uint8_t lastSta = 0;
  while (!gExit) {
    dnsServer.processNextRequest();
    server.handleClient();
    captureTick();
    idleLiveTick();

    const uint8_t sta = WiFi.softAPgetStationNum();
    if (sta > 0) {
      // Phone associated — do not idle-timeout mid-setup.
      gLastActivityMs = millis();
      if (sta != lastSta) {
        Serial.printf("SoftAP clients=%u\n", (unsigned)sta);
      }
    } else if (lastSta > 0) {
      // Last client left — restart idle window.
      gLastActivityMs = millis();
      Serial.println("SoftAP clients=0 — idle timer restarted");
    } else if ((millis() - gLastActivityMs) > kIdleTimeoutMs) {
      Serial.println("Setup portal idle timeout");
      break;
    }
    lastSta = sta;
    delay(2);
  }

  if (gMode != CAP_NONE) {
    moistureSensorPower(false);
    gMode = CAP_NONE;
  }
  server.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  ledSetWifiActive(false);
  Serial.println("Setup portal exited");
}
