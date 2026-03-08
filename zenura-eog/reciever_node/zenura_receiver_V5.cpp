// ================================================================
//  ZENURA — RECEIVER NODE  ★ V4 — ESP32 SENDS TELEGRAM ★
//  Flash to: Receiver ESP32 (COM14 / ttyUSB1)
//
//  HOW IT WORKS:
//    ESP32 runs in AP+STA mode simultaneously:
//      • Creates  "ZENURA-EOG" hotspot  → laptop connects here
//      • Connects to "Skip" hotspot     → internet for Telegram
//    Telegram is sent from ESP32 using HTTPClient.
//    Laptop browser needs NO internet at all.
//
//  LIBRARIES NEEDED:
//    • ESP Async WebServer  by ESP32Async
//    • AsyncTCP             by ESP32Async
// ================================================================

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ── AP (dashboard hotspot) ───────────────────────────────
const char* AP_SSID  = "ZENURA-EOG";
const char* AP_PASS  = "zenura123";
#define     AP_CHAN  1

// ── STA (your mobile hotspot for internet) ───────────────
const char* STA_SSID = "Skip";
const char* STA_PASS = "mynameisash";

// ── Telegram ─────────────────────────────────────────────
const char* TG_TOKEN   = "8634029440:AAEByDZln-NtKqBeDddGCJF6QziBl-vE2BY";
const char* DOCTOR_ID  = "990222805";
const char* FAMILY_ID  = "8017432443";

// ── Web server + WebSocket ────────────────────────────────
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// ── ESP-NOW packet (must match sensing node) ─────────────
typedef struct {
  int16_t  eye_x;
  int16_t  eye_y;
  uint8_t  blink_type;
  bool     leads_off;
  uint16_t raw_adc;
  uint16_t filtered_adc;
} EOGPacket;

volatile int16_t  g_eyeX     = 0;
volatile int16_t  g_eyeY     = 0;
volatile uint8_t  g_blink    = 0;
volatile bool     g_leadsOff = false;
volatile uint16_t g_raw      = 2048;
volatile uint16_t g_filtered = 2048;
volatile bool     g_newData  = false;

void onDataReceived(const esp_now_recv_info_t *info,
                    const uint8_t *data, int len) {
  if (len != sizeof(EOGPacket)) return;
  const EOGPacket* p = (const EOGPacket*)data;
  g_eyeX     = p->eye_x;
  g_eyeY     = p->eye_y;
  g_blink    = p->blink_type;
  g_leadsOff = p->leads_off;
  g_raw      = p->raw_adc;
  g_filtered = p->filtered_adc;
  g_newData  = true;
}

// ── Telegram sender (runs on ESP32, uses STA interface) ───
bool sendTelegramMsg(const char* chatId, const String& text) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("TG SKIP: WiFi not connected");
    return false;
  }
  HTTPClient http;
  String url = "https://api.telegram.org/bot";
  url += TG_TOKEN;
  url += "/sendMessage";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  // Build JSON body
  StaticJsonDocument<512> doc;
  doc["chat_id"]    = chatId;
  doc["text"]       = text;
  doc["parse_mode"] = "Markdown";
  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  bool ok  = (code == 200);
  Serial.printf("TG → %s  HTTP:%d  %s\n", chatId, code, ok?"OK":"FAIL");
  http.end();
  return ok;
}

// Tile class → who to notify, what message to send
// Returns JSON string: {"ok":true/false,"sent":N,"failed":N,"target":"..."}
String dispatchTelegram(const String& tileClass, const String& source) {
  String ts = "";   // timestamp from source string is fine
  int sent = 0, failed = 0;

  if (tileClass == "emerg") {
    String msg = "🆘 *CRITICAL SOS — ZENURA*\n\n"
                 "⚠️ *Patient needs IMMEDIATE help\\!*\n\n"
                 "📱 Input: " + source + "\n"
                 "📍 ZENURA Neural Interface\n\n"
                 "_Please respond immediately\\._";
    if (sendTelegramMsg(DOCTOR_ID, msg)) sent++; else failed++;
    if (sendTelegramMsg(FAMILY_ID, msg)) sent++; else failed++;

  } else if (tileClass == "nurse") {
    String msg = "🏥 *Medical Assistance — ZENURA*\n\n"
                 "Patient has requested a nurse\\.\n\n"
                 "📱 Input: " + source + "\n"
                 "📍 ZENURA Neural Interface";
    if (sendTelegramMsg(DOCTOR_ID, msg)) sent++; else failed++;

  } else if (tileClass == "family") {
    String msg = "📞 *Family Alert — ZENURA*\n\n"
                 "Your loved one wants to reach you\\.\n\n"
                 "📱 Input: " + source + "\n"
                 "📍 ZENURA Neural Interface";
    if (sendTelegramMsg(FAMILY_ID, msg)) sent++; else failed++;
  }
  // lights — no Telegram

  String result = "{\"type\":\"tg_result\",\"cls\":\"" + tileClass + "\","
                  "\"sent\":" + sent + ",\"failed\":" + failed + "}";
  return result;
}

// ── WebSocket events ─────────────────────────────────────
// Browser sends: {"action":"tile","cls":"nurse","src":"Mouse click (demo)"}
void onWsEvent(AsyncWebSocket *s, AsyncWebSocketClient *c,
               AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("Browser connected: #%u\n", c->id());
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("Browser disconnected: #%u\n", c->id());
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    if (info->final && info->index == 0 && info->len == len) {
      String msg = "";
      for (size_t i = 0; i < len; i++) msg += (char)data[i];

      // Parse JSON action from browser
      StaticJsonDocument<256> doc;
      if (deserializeJson(doc, msg) == DeserializationError::Ok) {
        if (doc["action"] == "tile") {
          String cls = doc["cls"].as<String>();
          String src = doc["src"].as<String>();
          Serial.printf("Tile fired: %s  src: %s\n", cls.c_str(), src.c_str());
          // Send Telegram and reply to browser
          String result = dispatchTelegram(cls, src);
          c->text(result);
        }
      }
    }
  }
}

// ─────────────────────────────────────────────────────────
// ─────────────────  DASHBOARD HTML  ─────────────────────
// ─────────────────────────────────────────────────────────
const char DASHBOARD_HTML[] PROGMEM = R"=====(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>ZENURA</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
<style>
@import url('https://fonts.googleapis.com/css2?family=Space+Mono:wght@400;700&family=Syne:wght@700;800&display=swap');
*{margin:0;padding:0;box-sizing:border-box}
:root{
  --bg:#000;--card:#080808;--border:#1a1a1a;
  --cyan:#22d3ee;--green:#10b981;--blue:#3b82f6;
  --amber:#f59e0b;--red:#ef4444;--w:#fff;--dim:#555;
}
html,body{width:100%;height:100%;overflow:hidden;background:var(--bg);color:var(--w)}
body{font-family:'Space Mono',monospace}
body::after{content:'';pointer-events:none;position:fixed;inset:0;z-index:99999;
  background:repeating-linear-gradient(0deg,transparent,transparent 2px,rgba(0,0,0,.025) 2px,rgba(0,0,0,.025) 4px)}

/* ── AMBER DEMO RING ── */
#ring{
  position:fixed;left:-100px;top:-100px;
  width:40px;height:40px;border-radius:50%;
  border:2.5px solid var(--amber);
  background:rgba(245,158,11,.18);
  box-shadow:0 0 22px rgba(245,158,11,.55),inset 0 0 10px rgba(245,158,11,.1);
  transform:translate(-50%,-50%);
  pointer-events:none;z-index:99998;
  transition:width .1s,height .1s,background .1s,box-shadow .1s;
}
#ring.pulse{
  width:56px;height:56px;
  background:rgba(245,158,11,.8);
  box-shadow:0 0 60px var(--amber),0 0 110px rgba(245,158,11,.3);
}

/* ── EOG CYAN RING ── */
#eog-ring{
  position:fixed;left:50%;top:50%;
  width:34px;height:34px;border-radius:50%;
  border:2.5px solid var(--cyan);
  background:rgba(34,211,238,.15);
  box-shadow:0 0 18px rgba(34,211,238,.5);
  transform:translate(-50%,-50%);
  pointer-events:none;z-index:99998;display:none;
  transition:width .08s,height .08s,background .08s;
}
#eog-ring.b1{width:52px;height:52px;background:rgba(34,211,238,.9);box-shadow:0 0 60px var(--cyan)}
#eog-ring.b2{width:56px;height:56px;background:rgba(245,158,11,.9);border-color:var(--amber);box-shadow:0 0 60px var(--amber)}

/* ── DWELL RING ── */
#dsvg{
  position:fixed;left:-200px;top:-200px;
  transform:translate(-50%,-50%);
  pointer-events:none;z-index:99997;
}
#dsvg circle{fill:none;stroke-width:3;stroke-linecap:round;
  stroke-dasharray:172;stroke-dashoffset:172;
  transform-origin:27px 27px;transform:rotate(-90deg)}
body.demo-mode #dsvg circle{stroke:var(--amber);filter:drop-shadow(0 0 6px var(--amber))}
body.eog-mode  #dsvg circle{stroke:var(--cyan); filter:drop-shadow(0 0 6px var(--cyan))}

/* ── REST ZONE ── */
#rest{
  position:fixed;border-radius:50%;
  border:1px dashed rgba(255,255,255,.08);
  background:rgba(255,255,255,.02);
  pointer-events:none;z-index:4;
  transform:translate(-50%,-50%);
  display:flex;align-items:center;justify-content:center;
}
#rest span{font-size:.46rem;letter-spacing:3px;color:rgba(255,255,255,.1);font-weight:700}
#rest.lit{border-color:rgba(34,211,238,.3);background:rgba(34,211,238,.04)}
#rest.lit span{color:rgba(34,211,238,.35)}

/* ── MODE UI ── */
#mode-btn{
  position:fixed;bottom:148px;right:16px;z-index:10010;
  font-family:'Space Mono',monospace;font-size:.58rem;font-weight:700;
  letter-spacing:2px;padding:8px 16px;border-radius:20px;cursor:pointer;
  border:1.5px solid var(--amber);background:#050505;color:var(--amber);transition:all .2s;
}
#mode-btn:hover{background:var(--amber);color:#000}
body.eog-mode #mode-btn{border-color:var(--cyan);color:var(--cyan)}
body.eog-mode #mode-btn:hover{background:var(--cyan);color:#000}
#mode-banner{
  position:fixed;top:64px;left:0;right:0;z-index:10005;
  padding:7px;text-align:center;font-size:.58rem;letter-spacing:2.5px;pointer-events:none;
}
body.demo-mode #mode-banner{background:rgba(245,158,11,.1);border-bottom:1px solid rgba(245,158,11,.3);color:var(--amber)}
body.eog-mode  #mode-banner{background:rgba(34,211,238,.07);border-bottom:1px solid rgba(34,211,238,.2);color:var(--cyan)}

/* ── ALERTS ── */
#leads-alert{display:none;position:fixed;top:0;left:0;right:0;z-index:10000;
  padding:9px;background:#2a0000;border-bottom:1px solid var(--red);
  color:var(--red);text-align:center;font-size:.65rem;letter-spacing:3px;
  animation:lf .5s step-start infinite}
#leads-alert.on{display:block}
@keyframes lf{50%{background:#0f0000}}
#sos-bg{display:none;position:fixed;inset:0;z-index:9000;pointer-events:none;
  background:rgba(239,68,68,.06);animation:sob .8s ease-in-out infinite alternate}
#sos-bg.on{display:block}
@keyframes sob{to{background:rgba(239,68,68,.14)}}

/* ── TOAST ── */
#toast{
  position:fixed;bottom:152px;left:50%;
  transform:translateX(-50%) translateY(130px);
  background:var(--w);color:#000;padding:12px 34px;border-radius:40px;
  font-family:'Syne',sans-serif;font-weight:800;font-size:1.1rem;
  transition:transform .4s cubic-bezier(.175,.885,.32,1.275),opacity .3s;
  z-index:9997;white-space:nowrap;opacity:0;pointer-events:none;
}
#toast.on{transform:translateX(-50%) translateY(0);opacity:1}

/* ── TELEGRAM POPUP ── */
#tg{
  position:fixed;top:80px;right:20px;z-index:10002;width:285px;border-radius:14px;
  padding:15px 17px;background:#060f0a;border:1px solid var(--green);
  box-shadow:0 0 40px rgba(16,185,129,.12);
  transform:translateX(320px);transition:transform .45s cubic-bezier(.175,.885,.32,1.275);
}
#tg.on{transform:translateX(0)}
#tg.sos{background:#0f0606;border-color:var(--red);box-shadow:0 0 40px rgba(239,68,68,.15)}
#tg.fam{background:#06080f;border-color:var(--blue);box-shadow:0 0 40px rgba(59,130,246,.12)}
.tg-h{display:flex;align-items:center;gap:9px;font-size:.6rem;letter-spacing:2.5px;font-weight:700;color:var(--green);margin-bottom:10px}
#tg.sos .tg-h{color:var(--red)} #tg.fam .tg-h{color:var(--blue)}
.tg-dot{width:8px;height:8px;border-radius:50%;flex-shrink:0;background:var(--green);
  box-shadow:0 0 8px var(--green);animation:pd 1s ease-in-out infinite}
#tg.sos .tg-dot{background:var(--red);box-shadow:0 0 8px var(--red)}
#tg.fam .tg-dot{background:var(--blue);box-shadow:0 0 8px var(--blue)}
@keyframes pd{0%,100%{opacity:1}50%{opacity:.3}}
.tg-b{font-size:.6rem;color:#8bb8cc;line-height:1.8;margin-bottom:6px}
.tg-b b{color:var(--w)}
.tg-st{font-size:.55rem;margin-top:8px;letter-spacing:1px;color:var(--dim);
  display:flex;align-items:center;gap:6px;border-top:1px solid #111;padding-top:8px}
.tg-st.ok{color:var(--green)}.tg-st.fail{color:var(--red)}.tg-st.wait{color:var(--amber)}
.tg-spin{width:8px;height:8px;border-radius:50%;border:1.5px solid var(--amber);
  border-top-color:transparent;animation:spin .8s linear infinite;flex-shrink:0;display:none}
@keyframes spin{to{transform:rotate(360deg)}}
#tg-wifi{font-size:.5rem;color:var(--green);letter-spacing:1.5px;
  border:1px solid rgba(16,185,129,.3);border-radius:10px;padding:2px 8px;margin-left:auto}

/* ── LAYOUT ── */
.app{display:flex;flex-direction:column;height:100vh}
.hdr{height:64px;flex-shrink:0;background:#020202;border-bottom:1px solid var(--border);
  display:flex;align-items:center;justify-content:space-between;padding:0 26px}
.logo{display:flex;align-items:center;gap:12px}
.lz{font-family:'Syne',sans-serif;font-weight:800;background:var(--cyan);color:#000;
  padding:4px 10px;border-radius:3px;font-size:1rem;font-style:italic}
.ln{font-family:'Syne',sans-serif;font-weight:800;font-size:1.1rem;letter-spacing:-.5px}
.ls{font-size:.5rem;color:var(--dim);letter-spacing:3px;margin-top:1px}
.hr{display:flex;align-items:center;gap:14px}
.pill{border:1px solid var(--border);border-radius:20px;padding:5px 13px;
  font-size:.65rem;color:var(--dim);letter-spacing:1px;display:flex;align-items:center;gap:7px}
.pill .pv{color:var(--w);font-weight:700;min-width:22px;text-align:right}
.pill .pd2{width:6px;height:6px;border-radius:50%;background:var(--border)}
.pill.live .pd2{background:var(--cyan);box-shadow:0 0 6px var(--cyan);animation:pd 1.5s ease-in-out infinite}
#gp{border:1px solid var(--border);border-radius:20px;padding:5px 14px;
  font-size:.65rem;font-weight:700;letter-spacing:2px;color:var(--dim);background:#0a0a0a;transition:all .2s}
#gp.on{background:var(--w);color:#000;border-color:var(--w)}
#gp.b1{background:var(--cyan);color:#000;border-color:var(--cyan)}
#gp.b2{background:var(--amber);color:#000;border-color:var(--amber)}
#uptime{font-size:.6rem;color:var(--dim);letter-spacing:2px}
#cp{display:flex;align-items:center;gap:8px;border:1px solid var(--border);border-radius:20px;
  padding:5px 13px;font-size:.65rem;font-weight:700;letter-spacing:2px;
  transition:all .4s;background:#0a0a0a;color:var(--dim)}
#cp .cd{width:7px;height:7px;border-radius:50%;background:#2a2a2a;flex-shrink:0;transition:all .4s}
#cp.online{border-color:rgba(16,185,129,.4);color:var(--green);background:rgba(16,185,129,.06)}
#cp.online .cd{background:var(--green);box-shadow:0 0 8px var(--green);animation:pd 2s ease-in-out infinite}
#cp.offline{border-color:rgba(239,68,68,.3);color:var(--red);background:rgba(239,68,68,.05)}
#cp.offline .cd{background:var(--red);box-shadow:0 0 6px var(--red)}

/* ── GRID ── */
.grid{flex:1;display:grid;grid-template-columns:1fr 1fr;grid-template-rows:1fr 1fr;
  gap:16px;padding:16px;position:relative;min-height:0}

/* ── TILES ── */
.tile{background:var(--card);border:1.5px solid var(--border);border-radius:20px;
  position:relative;overflow:hidden;display:flex;flex-direction:column;
  align-items:center;justify-content:center;gap:8px;
  transition:border-color .2s,background .2s,box-shadow .2s,transform .15s}
.tile::before,.tile::after{content:'';position:absolute;width:18px;height:18px;
  border-color:var(--border);border-style:solid;transition:border-color .2s}
.tile::before{top:12px;left:12px;border-width:1px 0 0 1px;border-radius:3px 0 0 0}
.tile::after{bottom:12px;right:12px;border-width:0 1px 1px 0;border-radius:0 0 3px 0}
.ti{font-size:2.6rem;transition:transform .2s,filter .2s;filter:grayscale(1) brightness(.4)}
.tn{font-family:'Syne',sans-serif;font-weight:800;font-size:2rem;letter-spacing:-2px}
.ts{font-size:.58rem;color:var(--dim);letter-spacing:3px}
.th3{font-size:.46rem;letter-spacing:2px;color:rgba(255,255,255,.18);margin-top:4px}
.tl{font-size:.52rem;color:var(--dim);letter-spacing:1px;
  position:absolute;bottom:12px;left:0;right:0;text-align:center;opacity:0;transition:opacity .3s}
.fill{position:absolute;bottom:0;left:0;width:100%;height:0%;transition:height .05s linear}
.nurse  .fill{background:linear-gradient(0deg,rgba(16,185,129,.35),transparent)}
.family .fill{background:linear-gradient(0deg,rgba(59,130,246,.35),transparent)}
.lights .fill{background:linear-gradient(0deg,rgba(245,158,11,.3),transparent)}
.emerg  .fill{background:linear-gradient(0deg,rgba(239,68,68,.4),transparent)}
.tile.h{background:#0d0d0d;transform:scale(1.015)}
.tile.h .ti{filter:grayscale(0) brightness(1);transform:scale(1.12)}
body.demo-mode .tile.h::before,body.demo-mode .tile.h::after{border-color:var(--amber)}
body.eog-mode  .tile.h::before,body.eog-mode  .tile.h::after{border-color:var(--cyan)}
.nurse.h {border-color:var(--green);box-shadow:0 0 40px rgba(16,185,129,.12) inset}
.family.h{border-color:var(--blue); box-shadow:0 0 40px rgba(59,130,246,.12) inset}
.lights.h{border-color:var(--amber);box-shadow:0 0 40px rgba(245,158,11,.15) inset}
.emerg.h {border-color:var(--red);  box-shadow:0 0 40px rgba(239,68,68,.18) inset}
.tile.fired{animation:tf .5s ease-out}
@keyframes tf{0%{filter:brightness(2.8)}50%{filter:brightness(1.5)}100%{filter:brightness(1)}}
.tile.fired .tl{opacity:1}

/* ── FOOTER ── */
.footer{height:128px;flex-shrink:0;background:#020202;border-top:1px solid var(--border);
  display:flex;padding:12px 18px;gap:16px;align-items:stretch}
.fm{width:160px;flex-shrink:0;border-right:1px solid var(--border);
  display:flex;flex-direction:column;justify-content:space-between;padding-right:15px}
.fm .fl{font-size:.44rem;color:var(--cyan);letter-spacing:3px;font-weight:700}
.fm .fv{font-size:1.5rem;line-height:1}
.fm .fb{font-size:.52rem;color:var(--dim)}
.fst-w{width:125px;flex-shrink:0;border-right:1px solid var(--border);
  display:flex;flex-direction:column;justify-content:space-around;padding:0 13px}
.fst{display:flex;flex-direction:column;gap:1px}
.fst .fsl{font-size:.4rem;color:var(--dim);letter-spacing:2px}
.fst .fsv{font-size:.82rem}
.cw{flex:1;position:relative;min-width:0}
.lp{width:175px;flex-shrink:0;border-left:1px solid var(--border);
  display:flex;flex-direction:column;padding-left:13px;overflow:hidden}
.lp .lt{font-size:.4rem;color:var(--cyan);letter-spacing:3px;margin-bottom:5px;font-weight:700}
.ll{display:flex;flex-direction:column;gap:4px;flex:1;overflow:hidden}
.le{font-size:.5rem;color:var(--dim);display:flex;gap:5px;align-items:baseline;animation:lai .3s ease-out}
.le .let{color:#333;flex-shrink:0;font-size:.44rem}
.le .leg{font-weight:700;flex-shrink:0}
.leg.nurse{color:var(--green)}.leg.family{color:var(--blue)}.leg.lights{color:var(--amber)}.leg.emerg{color:var(--red)}
@keyframes lai{from{opacity:0;transform:translateX(6px)}to{opacity:1;transform:none}}
</style>
</head>
<body class="demo-mode">

<div id="leads-alert">⚠ ELECTRODE DETACHMENT — Check wires</div>
<div id="sos-bg"></div>
<div id="ring"></div>
<div id="eog-ring"></div>
<div id="dsvg">
  <svg width="54" height="54" viewBox="0 0 54 54">
    <circle cx="27" cy="27" r="22" stroke-width="3" id="dcirc"/>
  </svg>
</div>
<div id="rest"><span>REST</span></div>
<div id="mode-banner">🖱  DEMO MODE — Move mouse over tiles · Click or hover 1.5 s to trigger · Telegram sent by ESP32</div>
<div id="toast"></div>

<div id="tg">
  <div class="tg-h">
    <div class="tg-dot"></div>
    <span id="tg-title">ALERT SENT</span>
    <span id="tg-wifi">VIA ESP32 ✓</span>
  </div>
  <div class="tg-b" id="tg-body"></div>
  <div class="tg-st" id="tg-st">
    <div class="tg-spin" id="tg-spin"></div>
    <span id="tg-stxt">Sending via ESP32…</span>
  </div>
</div>

<button id="mode-btn" onclick="toggleMode()">👁 SWITCH TO EOG</button>

<div class="app">
  <header class="hdr">
    <div class="logo">
      <div class="lz">Z</div>
      <div><div class="ln">ZENURA</div><div class="ls">EOG NEURAL INTERFACE V1.0</div></div>
    </div>
    <div class="hr">
      <div id="uptime">00:00:00</div>
      <div id="cp" class="offline"><div class="cd"></div><span id="ct">ESP32 OFFLINE</span></div>
      <div class="pill" id="blpill"><span class="pd2"></span>BLINKS<span class="pv" id="blcnt">0</span></div>
      <div class="pill" id="latpill"><span class="pd2"></span>LATENCY<span class="pv" id="latv">--</span><span style="font-size:.48rem;color:var(--dim)">ms</span></div>
      <div id="gp">DEMO MODE</div>
    </div>
  </header>

  <main class="grid">
    <div class="tile nurse" data-lbl="NURSE"  data-cls="nurse"  data-row="0" data-col="0" data-msg="Medical assistance requested">
      <div class="ti">🏥</div><div class="tn">NURSE</div><div class="ts">MEDICAL AID</div>
      <div class="th3">TOP-LEFT · SINGLE BLINK · HOVER 1.5s</div>
      <div class="tl" id="tl-nurse">—</div><div class="fill"></div>
    </div>
    <div class="tile family" data-lbl="FAMILY" data-cls="family" data-row="0" data-col="1" data-msg="Family notification sent">
      <div class="ti">📞</div><div class="tn">FAMILY</div><div class="ts">CONTACT HOME</div>
      <div class="th3">TOP-RIGHT · SINGLE BLINK · HOVER 1.5s</div>
      <div class="tl" id="tl-family">—</div><div class="fill"></div>
    </div>
    <div class="tile lights" data-lbl="LIGHTS" data-cls="lights" data-row="1" data-col="0" data-msg="Smart lighting toggled">
      <div class="ti">💡</div><div class="tn">LIGHTS</div><div class="ts">ENVIRONMENT</div>
      <div class="th3">BTM-LEFT · DOUBLE BLINK · HOVER 1.5s</div>
      <div class="tl" id="tl-lights">—</div><div class="fill"></div>
    </div>
    <div class="tile emerg" data-lbl="SOS" data-cls="emerg" data-row="1" data-col="1" data-msg="CRITICAL SOS BROADCASTED">
      <div class="ti">🆘</div><div class="tn">SOS</div><div class="ts">EMERGENCY</div>
      <div class="th3">BTM-RIGHT · DOUBLE BLINK · HOVER 1.5s</div>
      <div class="tl" id="tl-sos">—</div><div class="fill"></div>
    </div>
  </main>

  <footer class="footer">
    <div class="fm">
      <span class="fl">EOG SIGNAL</span>
      <span class="fv" id="sig-val">---</span>
      <span class="fb" id="sig-sub">X: 0 · STABLE</span>
    </div>
    <div class="fst-w">
      <div class="fst"><span class="fsl">ACTIONS</span><span class="fsv" id="f-act">0</span></div>
      <div class="fst"><span class="fsl">QUALITY</span><span class="fsv" id="f-ql">—</span></div>
      <div class="fst"><span class="fsl">PKT/s</span><span class="fsv" id="f-pkt">—</span></div>
    </div>
    <div class="cw"><canvas id="eog-canvas"></canvas></div>
    <div class="lp">
      <div class="lt">ACTION LOG</div>
      <div class="ll" id="log-list">
        <div class="le"><span class="let">--:--:--</span><span class="leg" style="color:#1f1f1f">SYS</span><span>Hover a tile to begin</span></div>
      </div>
    </div>
  </footer>
</div>

<script>
'use strict';

// ═══════════════════════════════════════════════════
//  WEBSOCKET — also receives Telegram results from ESP32
// ═══════════════════════════════════════════════════
let wsOk=false, wsConn=null, pktCount=0, lastPktTime=0;

function connectWS() {
  const cp=document.getElementById('cp'), ct=document.getElementById('ct');
  cp.className='connecting'; ct.textContent='CONNECTING...';
  wsConn = new WebSocket('ws://'+location.hostname+'/ws');
  wsConn.onopen  = ()=>{ wsOk=true;  cp.className='online';  ct.textContent='ESP32 ONLINE';  document.getElementById('blpill').classList.add('live'); document.getElementById('latpill').classList.add('live'); };
  wsConn.onclose = ()=>{ wsOk=false; cp.className='offline'; ct.textContent='ESP32 OFFLINE'; document.getElementById('blpill').classList.remove('live'); document.getElementById('latpill').classList.remove('live'); setTimeout(connectWS,2000); };
  wsConn.onerror = ()=> wsConn.close();
  wsConn.onmessage = e => {
    try {
      const d = JSON.parse(e.data);
      if (d.type === 'tg_result') {
        handleTgResult(d);  // Telegram reply from ESP32
      } else {
        handleEOG(d);       // sensor data
        pktCount++;
      }
    } catch(_) {}
  };
}
connectWS();

// Send tile action to ESP32 (ESP32 sends Telegram)
function notifyESP32(cls, src) {
  if (wsOk && wsConn) {
    wsConn.send(JSON.stringify({ action:'tile', cls:cls, src:src }));
  }
}

// ── Uptime / rate ─────────────────────────────────
const appStart=Date.now();
setInterval(()=>{
  const s=Math.floor((Date.now()-appStart)/1000);
  document.getElementById('uptime').textContent=
    [Math.floor(s/3600),Math.floor(s%3600/60),s%60].map(n=>String(n).padStart(2,'0')).join(':');
},1000);
setInterval(()=>{ document.getElementById('f-pkt').textContent=pktCount+'/s'; pktCount=0; },1000);
setInterval(()=>{ if(lastPktTime) document.getElementById('latv').textContent=Math.min(Date.now()-lastPktTime,999)+''; },100);

// ═══════════════════════════════════════════════════
//  TELEGRAM POPUP  (result comes back from ESP32)
// ═══════════════════════════════════════════════════
const TG_META = {
  emerg:  { e:'🆘', t:'CRITICAL SOS',  m:'sos', to:'Doctor + Family' },
  nurse:  { e:'🏥', t:'NURSE CALLED',  m:'',    to:'Doctor'          },
  family: { e:'📞', t:'FAMILY ALERT',  m:'fam', to:'Family'          },
  lights: { e:'💡', t:'LIGHTS',        m:'',    to:'—'               },
};
let tgTimer=null;

function showTgSending(cls, src) {
  const meta = TG_META[cls] || TG_META.nurse;
  const pop  = document.getElementById('tg');
  pop.className = 'on ' + meta.m;
  document.getElementById('tg-title').textContent = meta.e + '  ' + meta.t;
  document.getElementById('tg-body').innerHTML =
    `<b>Trigger:</b> ${src}<br><b>To:</b> ${meta.to}`;
  const st=document.getElementById('tg-st'), sp=document.getElementById('tg-spin');
  st.className='tg-st wait'; sp.style.display='block';
  document.getElementById('tg-stxt').textContent='ESP32 sending via Skip WiFi…';
  clearTimeout(tgTimer);
}

function handleTgResult(d) {
  const sp=document.getElementById('tg-spin'), st=document.getElementById('tg-st');
  sp.style.display='none';
  if (d.failed===0) {
    st.className='tg-st ok';
    document.getElementById('tg-stxt').textContent='✓ Delivered to '+d.sent+' recipient(s)';
  } else {
    st.className='tg-st fail';
    document.getElementById('tg-stxt').textContent=d.sent+' sent · '+d.failed+' failed';
  }
  clearTimeout(tgTimer);
  tgTimer=setTimeout(()=>document.getElementById('tg').classList.remove('on'),6000);
}

// ═══════════════════════════════════════════════════
//  EOG CHART + DATA
// ═══════════════════════════════════════════════════
const CHART_LEN=200; let chartData=new Array(CHART_LEN).fill(2048);
const eogChart=new Chart(document.getElementById('eog-canvas').getContext('2d'),{
  type:'line',
  data:{labels:new Array(CHART_LEN).fill(''),datasets:[{data:chartData,borderColor:'#22d3ee',borderWidth:1.5,pointRadius:0,tension:.3,fill:{target:'origin',above:'rgba(34,211,238,0.04)',below:'rgba(34,211,238,0.04)'}}]},
  options:{responsive:true,maintainAspectRatio:false,animation:false,scales:{y:{display:false,min:800,max:3200},x:{display:false}},plugins:{legend:{display:false}}}
});
let qBuf=[];
function sigQ(r,f){ qBuf.push(Math.abs(r-f)); if(qBuf.length>50)qBuf.shift(); const a=qBuf.reduce((x,y)=>x+y,0)/qBuf.length; return a<15?'EXCELLENT':a<35?'GOOD':a<80?'FAIR':'POOR'; }

let eogCol=-1, blinkTot=0;
const SNAP=40;

function handleEOG(d) {
  lastPktTime=Date.now();
  chartData.push(d.filtered_adc); chartData.shift();
  eogChart.data.datasets[0].data=chartData; eogChart.update('none');
  document.getElementById('sig-val').textContent=d.raw_adc;
  document.getElementById('sig-sub').textContent='X: '+d.eye_x+(d.leads_off?'  ⚠ LEADS OFF':'  · STABLE');
  document.getElementById('f-ql').textContent=d.leads_off?'NO SIGNAL':sigQ(d.raw_adc,d.filtered_adc);
  if(d.leads_off) document.getElementById('leads-alert').classList.add('on');
  else document.getElementById('leads-alert').classList.remove('on');

  if(isDemo) return;

  const gr=document.querySelector('.grid').getBoundingClientRect(), midY=gr.top+gr.height*.5;
  if(d.eye_x < -SNAP){ eogTX=gr.left+gr.width*.25; eogTY=midY; eogCol=0; }
  else if(d.eye_x > SNAP){ eogTX=gr.left+gr.width*.75; eogTY=midY; eogCol=1; }
  else{ eogTX=window.innerWidth/2; eogTY=midY; eogCol=-1; }

  const er=document.getElementById('eog-ring');
  if(d.blink_type===1){
    blinkTot++; document.getElementById('blcnt').textContent=blinkTot;
    er.className='b1'; setTimeout(()=>er.className='',380);
    setGP('SINGLE BLINK',false,true,false); setTimeout(()=>setGP('TRACKING EYES',true,false,false),700);
    if(eogCol>=0){ const t=allTiles.find(x=>+x.dataset.col===eogCol&&+x.dataset.row===0); if(t) fireTile(t,'Eye single blink'); }
  } else if(d.blink_type===2){
    blinkTot+=2; document.getElementById('blcnt').textContent=blinkTot;
    er.className='b2'; setTimeout(()=>er.className='',380);
    setGP('DOUBLE BLINK',false,false,true); setTimeout(()=>setGP('TRACKING EYES',true,false,false),700);
    if(eogCol>=0){ const t=allTiles.find(x=>+x.dataset.col===eogCol&&+x.dataset.row===1); if(t) fireTile(t,'Eye double blink'); }
  }
}

// ═══════════════════════════════════════════════════
//  FIRE TILE
// ═══════════════════════════════════════════════════
let actTot=0;
function fireTile(tile, src) {
  const lbl=tile.dataset.lbl, cls=tile.dataset.cls, msg=tile.dataset.msg;
  actTot++; document.getElementById('f-act').textContent=actTot;
  tile.classList.add('fired'); setTimeout(()=>tile.classList.remove('fired'),500);
  const ts=new Date().toTimeString().slice(0,8);
  const tId='tl-'+(lbl==='SOS'?'sos':lbl.toLowerCase());
  const tEl=document.getElementById(tId);
  if(tEl){ tEl.textContent='Last: '+ts; tEl.style.opacity='1'; }
  if(cls==='emerg'){ document.getElementById('sos-bg').classList.add('on'); setTimeout(()=>document.getElementById('sos-bg').classList.remove('on'),3500); }
  // Show "sending" popup immediately, then wait for ESP32 result
  if(cls!=='lights') showTgSending(cls, src);
  notifyESP32(cls, src);   // ESP32 sends Telegram, replies with tg_result
  showToast(lbl+' — '+msg);
  addLog(cls,lbl,msg);
  beep(cls==='emerg');
}

function showToast(m){ const t=document.getElementById('toast'); t.textContent=m; t.classList.add('on'); clearTimeout(window._tt); window._tt=setTimeout(()=>t.classList.remove('on'),3000); }
function addLog(c,l,m){ const ll=document.getElementById('log-list'),ts=new Date().toTimeString().slice(0,8),el=document.createElement('div'); el.className='le'; el.innerHTML=`<span class="let">${ts}</span><span class="leg ${c}">${l}</span><span>${m}</span>`; ll.insertBefore(el,ll.firstChild); while(ll.children.length>6)ll.removeChild(ll.lastChild); }
function setGP(t,a,b,d){ const p=document.getElementById('gp'); p.textContent=t; p.className=d?'b2':b?'b1':a?'on':''; }
function beep(e=false){ try{ const a=new(window.AudioContext||window.webkitAudioContext)(); const freqs=e?[880,1100]:[880]; freqs.forEach((f,i)=>{ const o=a.createOscillator(),g=a.createGain(); o.connect(g);g.connect(a.destination);o.type=e?'square':'sine';o.frequency.value=f; const t=a.currentTime+i*.16; g.gain.setValueAtTime(0,t);g.gain.linearRampToValueAtTime(.07,t+.02);g.gain.exponentialRampToValueAtTime(.001,t+.2); o.start(t);o.stop(t+.2); }); }catch(_){} }

// ═══════════════════════════════════════════════════
//  MODE TOGGLE
// ═══════════════════════════════════════════════════
let isDemo=true;
function toggleMode(){
  isDemo=!isDemo;
  const btn=document.getElementById('mode-btn'), banner=document.getElementById('mode-banner');
  const ring=document.getElementById('ring'), er=document.getElementById('eog-ring');
  if(isDemo){
    document.body.className='demo-mode';
    btn.textContent='👁 SWITCH TO EOG';
    banner.textContent='🖱  DEMO MODE — Move mouse over tiles · Click or hover 1.5 s to trigger · Telegram sent by ESP32';
    ring.style.display='block'; er.style.display='none';
    setGP('DEMO MODE',false,false,false);
    allTiles.forEach(t=>{t.classList.remove('h');t.querySelector('.fill').style.height='0%';});
    stopDwell();
  } else {
    document.body.className='eog-mode';
    btn.textContent='🖱 SWITCH TO DEMO';
    banner.textContent='👁  EOG MODE — Look LEFT/RIGHT · Single blink = top · Double blink = bottom · Stare 1.5 s = dwell';
    ring.style.display='none'; er.style.display='block';
    setGP('TRACKING EYES',true,false,false);
    allTiles.forEach(t=>{t.classList.remove('h');t.querySelector('.fill').style.height='0%';});
    stopDwell();
    curX=window.innerWidth/2; curY=window.innerHeight/2; eogTX=curX; eogTY=curY;
  }
}

// ═══════════════════════════════════════════════════
//  CURSOR + REST ZONE + DWELL
// ═══════════════════════════════════════════════════
const allTiles = Array.from(document.querySelectorAll('.tile'));
const ringEl   = document.getElementById('ring');
const dsvg     = document.getElementById('dsvg');
const dcirc    = document.getElementById('dcirc');
const restEl   = document.getElementById('rest');
const SMOOTH=0.12, DWELL_MS=1500;
let curX=window.innerWidth/2, curY=window.innerHeight/2, eogTX=curX, eogTY=curY;
let hoveredTile=null, dwellActive=false, dwellStart=0, dwellBlocked=false;

function positionRest(){
  const gr=document.querySelector('.grid');
  if(!gr) return;
  const r=gr.getBoundingClientRect();
  const sz=Math.min(r.width,r.height)*.22;
  restEl.style.left=(r.left+r.width/2)+'px';
  restEl.style.top=(r.top+r.height/2)+'px';
  restEl.style.width=sz+'px'; restEl.style.height=sz+'px';
}
positionRest();
window.addEventListener('resize',positionRest);

function inRest(x,y){
  const r=restEl.getBoundingClientRect();
  const dx=x-(r.left+r.width/2), dy=y-(r.top+r.height/2);
  return Math.sqrt(dx*dx+dy*dy) < r.width/2;
}
function tileAt(x,y){
  for(const t of allTiles){ const r=t.getBoundingClientRect(); if(x>=r.left&&x<=r.right&&y>=r.top&&y<=r.bottom) return t; }
  return null;
}
function stopDwell(){
  dwellActive=false; dwellBlocked=false;
  dsvg.style.left='-200px'; dsvg.style.top='-200px';
  dcirc.style.strokeDashoffset='172';
  if(hoveredTile){ hoveredTile.classList.remove('h'); hoveredTile.querySelector('.fill').style.height='0%'; hoveredTile=null; }
}

// ── Demo mode mouse events ─────────────────────────
document.addEventListener('mousemove', e=>{
  if(!isDemo) return;
  // Always move ring to exact cursor position — no CSS transition on position
  ringEl.style.left = e.clientX + 'px';
  ringEl.style.top  = e.clientY + 'px';

  const rest = inRest(e.clientX, e.clientY);
  restEl.classList.toggle('lit', rest);

  const tile = tileAt(e.clientX, e.clientY);
  if(tile !== hoveredTile){
    if(hoveredTile){ hoveredTile.classList.remove('h'); hoveredTile.querySelector('.fill').style.height='0%'; }
    hoveredTile=tile;
    dwellActive=false; dwellBlocked=false;
    dsvg.style.left='-200px'; dcirc.style.strokeDashoffset='172';
    if(hoveredTile && !rest){ hoveredTile.classList.add('h'); dwellActive=true; dwellStart=performance.now(); }
  } else if(hoveredTile && !rest && !dwellActive && !dwellBlocked){
    dwellActive=true; dwellStart=performance.now();
  }
  if(rest && dwellActive){ dwellActive=false; dsvg.style.left='-200px'; if(hoveredTile) hoveredTile.querySelector('.fill').style.height='0%'; }
});

document.addEventListener('click', e=>{
  if(!isDemo) return;
  const tile=tileAt(e.clientX, e.clientY);
  if(!tile || inRest(e.clientX,e.clientY)) return;
  ringEl.classList.add('pulse'); setTimeout(()=>ringEl.classList.remove('pulse'),300);
  fireTile(tile,'Mouse click (demo)');
  dwellActive=false; dwellBlocked=true;
  dsvg.style.left='-200px';
  if(hoveredTile) hoveredTile.querySelector('.fill').style.height='0%';
});

// ── Animation loop ─────────────────────────────────
function loop(now){
  if(isDemo){
    if(dwellActive && hoveredTile && !dwellBlocked){
      const p=Math.min((now-dwellStart)/DWELL_MS,1);
      dcirc.style.strokeDashoffset=(172*(1-p)).toString();
      const rc=hoveredTile.getBoundingClientRect();
      dsvg.style.left=(rc.left+rc.width/2)+'px'; dsvg.style.top=(rc.top+rc.height/2)+'px';
      hoveredTile.querySelector('.fill').style.height=(p*100)+'%';
      if(p>=1){
        ringEl.classList.add('pulse'); setTimeout(()=>ringEl.classList.remove('pulse'),300);
        fireTile(hoveredTile,'Mouse dwell 1.5 s (demo)');
        dwellActive=false; dwellBlocked=true;
        dsvg.style.left='-200px'; hoveredTile.querySelector('.fill').style.height='0%';
      }
    }
  } else {
    // EOG mode smooth cursor
    curX+=(eogTX-curX)*(1-SMOOTH); curY+=(eogTY-curY)*(1-SMOOTH);
    const er=document.getElementById('eog-ring');
    er.style.left=curX+'px'; er.style.top=curY+'px';
    const rest=inRest(curX,curY);
    restEl.classList.toggle('lit',rest);
    const tile=rest?null:tileAt(curX,curY);
    if(tile!==hoveredTile){
      if(hoveredTile){hoveredTile.classList.remove('h');hoveredTile.querySelector('.fill').style.height='0%';}
      hoveredTile=tile; dwellActive=!!tile; dwellBlocked=false;
      dsvg.style.left='-200px'; dcirc.style.strokeDashoffset='172';
      if(hoveredTile){ hoveredTile.classList.add('h'); dwellStart=now; }
    }
    if(dwellActive&&hoveredTile){
      const p=Math.min((now-dwellStart)/DWELL_MS,1);
      dcirc.style.strokeDashoffset=(172*(1-p)).toString();
      const rc=hoveredTile.getBoundingClientRect();
      dsvg.style.left=(rc.left+rc.width/2)+'px'; dsvg.style.top=(rc.top+rc.height/2)+'px';
      hoveredTile.querySelector('.fill').style.height=(p*100)+'%';
      if(p>=1){ fireTile(hoveredTile,'Eye dwell 1.5 s'); dwellActive=false; dwellBlocked=true; dsvg.style.left='-200px'; hoveredTile.querySelector('.fill').style.height='0%'; }
    }
  }
  requestAnimationFrame(loop);
}
requestAnimationFrame(loop);
</script>
</body>
</html>
)=====";

// ════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("=========================================");
  Serial.println("  ZENURA RECEIVER V5");
  Serial.println("=========================================");

  // AP+STA mode — AP for laptop, STA for internet/Telegram
  WiFi.mode(WIFI_AP_STA);

  // Start AP on channel 1 FIRST
  WiFi.softAP(AP_SSID, AP_PASS, AP_CHAN);
  delay(300);

  Serial.println("");
  Serial.println("╔══════════════════════════════════════╗");
  Serial.print  ("║ SoftAP MAC → ");
  Serial.print  (WiFi.softAPmacAddress());
  Serial.println(" ║");
  Serial.println("║ ← Copy this into sensing node       ║");
  Serial.println("╚══════════════════════════════════════╝");

  Serial.printf("\nConnecting to hotspot: %s\n", STA_SSID);
  WiFi.begin(STA_SSID, STA_PASS);

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500); Serial.print("."); tries++;
  }
  Serial.println("");

  uint8_t actualChan = WiFi.channel();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("✓ Internet connected: " + String(STA_SSID));
    Serial.print  ("  STA IP: "); Serial.println(WiFi.localIP());
    Serial.printf ("  WiFi landed on channel: %d\n", actualChan);
  } else {
    Serial.println("✗ Hotspot not found — Telegram disabled");
    Serial.println("  Check phone hotspot is ON and name/pass correct");
    actualChan = AP_CHAN;
  }

  // ╔═══════════════════════════════════════════════════╗
  // ║  THIS IS THE KEY LINE — tells sender what channel ║
  Serial.println("");
  Serial.println("╔══════════════════════════════════════╗");
  Serial.printf ("║  >>> SENDER MUST USE CHANNEL %d <<<   ║\n", actualChan);
  Serial.println("║  Set #define WIFI_CHANNEL in sender  ║");
  Serial.println("╚══════════════════════════════════════╝");
  Serial.println("");
  // ╚═══════════════════════════════════════════════════╝

  // Force ESP-NOW to use actual channel
  esp_wifi_set_channel(actualChan, WIFI_SECOND_CHAN_NONE);
  delay(100);

  // ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("ERROR: esp_now_init failed — restarting");
    delay(3000); ESP.restart();
  }
  esp_now_register_recv_cb(onDataReceived);
  Serial.printf("ESP-NOW ready on channel %d\n", actualChan);

  // Web server
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req){
    req->send_P(200, "text/html", DASHBOARD_HTML);
  });
  server.begin();

  Serial.println("Web server started ✓");
  Serial.printf("Dashboard: connect laptop to %s / %s\n", AP_SSID, AP_PASS);
  Serial.println("Then open: http://192.168.4.1");
}

void loop() {
  if (g_newData) {
    g_newData = false;
    char json[160];
    snprintf(json, sizeof(json),
      "{\"eye_x\":%d,\"eye_y\":%d,\"blink_type\":%u,"
      "\"leads_off\":%s,\"raw_adc\":%u,\"filtered_adc\":%u}",
      (int)g_eyeX, (int)g_eyeY, (unsigned)g_blink,
      g_leadsOff ? "true" : "false",
      (unsigned)g_raw, (unsigned)g_filtered
    );
    ws.textAll(json);
    g_blink = 0;
  }
  ws.cleanupClients();
  delay(1);
}
