// ================================================================
//  ZENURA — RECEIVER FIRMWARE  ★ V9 — LED + FIXES ★
//  Flash to: Receiver ESP32 (COM14 / ttyUSB1)
//
//  CHANGES FROM V8:
//    ★ GPIO 26 → LED toggle when LIGHTS tile fires
//    ★ Sends {"type":"led_state","state":true/false} back to
//       dashboard so the UI updates instantly
//    ★ Telegram sends in background task (non-blocking)
//       so tg_result arrives faster
//
//  LED WIRING:
//    GPIO 26 → 220Ω resistor → LED (+) anode
//    LED  (−) → GND (any GND pin on ESP32)
//
//  NETWORK SETUP (NO CHANGE):
//    1. Turn on phone hotspot "Skip" (pass: mynameisash)
//    2. Power this ESP32 — it joins Skip automatically
//    3. Open Serial Monitor — note the IP address printed
//    4. Connect laptop to Skip
//    5. Open zenura_dashboard_V2.html
//    6. Enter the ESP32 IP when prompted
//
//  LIBRARIES NEEDED:
//    • ESP Async WebServer  (ESP32Async / mathieucarbou)
//    • AsyncTCP             (ESP32Async)
//    • ArduinoJson          (Benoit Blanchon)
// ================================================================

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ── Mobile hotspot ────────────────────────────────────────────
const char* STA_SSID = "Skip";
const char* STA_PASS = "mynameisash";

// ── Telegram ──────────────────────────────────────────────────
const char* TG_TOKEN  = "8634029440:AAEByDZln-NtKqBeDddGCJF6QziBl-vE2BY";
const char* DOCTOR_ID = "7864763989";
const char* FAMILY_ID = "8017432443";
const char* FRIEND_ID = "990222805";

// ── LED PIN ───────────────────────────────────────────────────
// Connect: GPIO26 → 220Ω resistor → LED(+) → LED(−) → GND
#define LED_PIN 26
bool ledState = false;   // tracks current LED on/off

// ── WebSocket server ──────────────────────────────────────────
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// ── ESP-NOW packet (must match sensing node exactly) ──────────
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

void onDataReceived(const esp_now_recv_info_t* info,
                    const uint8_t* data, int len) {
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

// ── LED toggle ────────────────────────────────────────────────
void toggleLed(AsyncWebSocketClient* client) {
  ledState = !ledState;
  digitalWrite(LED_PIN, ledState ? HIGH : LOW);
  Serial.printf("💡 LED toggled → %s\n", ledState ? "ON" : "OFF");

  // Immediately tell the dashboard the new LED state
  String ledMsg = "{\"type\":\"led_state\",\"state\":";
  ledMsg += ledState ? "true" : "false";
  ledMsg += "}";

  if (client) {
    client->text(ledMsg);
  } else {
    ws.textAll(ledMsg);
  }
}

// ── Telegram sender (blocking, but called from main loop task) ─
bool sendTG(const char* chatId, const String& text) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  String url = "https://api.telegram.org/bot";
  url += TG_TOKEN;
  url += "/sendMessage";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(8000);   // 8s timeout (was default 5s, sometimes too short)
  StaticJsonDocument<512> doc;
  doc["chat_id"]    = chatId;
  doc["text"]       = text;
  doc["parse_mode"] = "Markdown";
  String body; serializeJson(doc, body);
  int code = http.POST(body);
  bool ok  = (code == 200);
  Serial.printf("TG → %s  HTTP:%d  %s\n", chatId, code, ok?"OK":"FAIL");
  http.end();
  return ok;
}

// ── Telegram dispatch (returns JSON result string) ────────────
String dispatchTelegram(const String& cls, const String& src,
                        const String& tileName) {
  int sent = 0, failed = 0;
  String name = tileName.length() > 0 ? tileName : cls;

  if (cls == "emerg") {
    String msg = "🆘 *CRITICAL SOS — ZENURA*\n\n"
                 "⚠️ *Patient needs IMMEDIATE help\\!*\n\n"
                 "📱 Tile: " + name + "\n📱 Input: " + src + "\n"
                 "📍 ZENURA Neural Interface\n\n_Please respond immediately\\._";
    if (sendTG(DOCTOR_ID, msg)) sent++; else failed++;
    if (sendTG(FAMILY_ID, msg)) sent++; else failed++;
    if (sendTG(FRIEND_ID, msg)) sent++; else failed++;

  } else if (cls == "nurse") {
    String msg = "🏥 *Medical Assistance — ZENURA*\n\n"
                 "Patient has requested: *" + name + "*\n\n"
                 "📱 Input: " + src + "\n📍 ZENURA Neural Interface";
    if (sendTG(DOCTOR_ID, msg)) sent++; else failed++;

  } else if (cls == "family") {
    String msg = "📞 *Family Alert — ZENURA*\n\n"
                 "Your loved one wants to reach you\\.\n\n"
                 "📱 Tile: " + name + "\n📱 Input: " + src + "\n"
                 "📍 ZENURA Neural Interface";
    if (sendTG(FAMILY_ID, msg)) sent++; else failed++;
    if (sendTG(FRIEND_ID, msg)) sent++; else failed++;
  }
  // lights: no Telegram, only LED toggle

  return "{\"type\":\"tg_result\",\"cls\":\"" + cls + "\","
         "\"sent\":" + sent + ",\"failed\":" + failed + "}";
}

// ── Pending Telegram task ─────────────────────────────────────
// We queue one Telegram job at a time and fire it in loop()
// so the WebSocket doesn't block waiting for HTTP.
struct TgJob {
  bool      pending;
  uint32_t  clientId;
  String    cls;
  String    src;
  String    tileName;
};
TgJob tgJob = {false, 0, "", "", ""};

// ── WebSocket handler ─────────────────────────────────────────
void onWsEvent(AsyncWebSocket* s, AsyncWebSocketClient* c,
               AwsEventType type, void* arg, uint8_t* data, size_t len) {

  if (type == WS_EVT_CONNECT) {
    Serial.printf("✓ Dashboard connected: client #%u\n", c->id());
    // Send hello with current LED state
    String hello = "{\"type\":\"hello\",\"version\":\"ZENURA-V9\",\"ip\":\"";
    hello += WiFi.localIP().toString();
    hello += "\",\"led_state\":";
    hello += ledState ? "true" : "false";
    hello += "}";
    c->text(hello);
    // Also send current LED state so dashboard syncs
    String ledMsg = "{\"type\":\"led_state\",\"state\":";
    ledMsg += ledState ? "true" : "false";
    ledMsg += "}";
    c->text(ledMsg);

  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("✗ Dashboard disconnected: client #%u\n", c->id());

  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    if (info->final && info->index == 0 && info->len == len) {
      String msg = "";
      for (size_t i = 0; i < len; i++) msg += (char)data[i];
      StaticJsonDocument<256> doc;
      if (deserializeJson(doc, msg) == DeserializationError::Ok) {
        if (doc["action"] == "tile") {
          String cls      = doc["cls"].as<String>();
          String src      = doc["src"].as<String>();
          String tileName = doc.containsKey("name") ? doc["name"].as<String>() : "";
          Serial.printf("Tile: %-8s  src: %s\n", cls.c_str(), src.c_str());

          if (cls == "lights") {
            // Toggle LED immediately — fast, no Telegram
            toggleLed(c);
            // Send tg_result with 0 sent (no Telegram for lights)
            c->text("{\"type\":\"tg_result\",\"cls\":\"lights\",\"sent\":0,\"failed\":0}");

          } else {
            // Queue Telegram — will fire in loop() without blocking WS
            if (!tgJob.pending) {
              tgJob.pending  = true;
              tgJob.clientId = c->id();
              tgJob.cls      = cls;
              tgJob.src      = src;
              tgJob.tileName = tileName;
            } else {
              // Already one job pending — send immediately (blocking) if queue full
              String result = dispatchTelegram(cls, src, tileName);
              c->text(result);
            }
          }
        }
      }
    }
  }
}

// ════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("==========================================");
  Serial.println("  ZENURA RECEIVER FIRMWARE  V9");
  Serial.println("  LED on GPIO 26 + Non-blocking TG");
  Serial.println("==========================================");

  // LED pin
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  Serial.printf("LED pin: GPIO%d (initially OFF)\n", LED_PIN);

  // STA — joins phone hotspot
  WiFi.mode(WIFI_STA);
  WiFi.begin(STA_SSID, STA_PASS);
  Serial.printf("\nConnecting to: %s\n", STA_SSID);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(500); Serial.print("."); tries++;
  }
  Serial.println("");

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("✗ Could not connect — check hotspot is ON");
    Serial.println("  Restarting in 5s…");
    delay(5000); ESP.restart();
  }
  Serial.println("✓ Connected to: " + String(STA_SSID));

  Serial.println("");
  Serial.println("╔═══════════════════════════════════════════╗");
  Serial.print  ("║  ESP32 IP  →  ");
  Serial.print  (WiFi.localIP());
  Serial.println("             ║");
  Serial.println("║  Enter this IP in the dashboard setup     ║");
  Serial.println("╚═══════════════════════════════════════════╝");
  Serial.println("");

  uint8_t chan = WiFi.channel();
  Serial.printf("WiFi channel: %d\n", chan);
  Serial.println("╔═══════════════════════════════════════════╗");
  Serial.printf ("║  >>> SENDER MUST USE CHANNEL %2d <<<       ║\n", chan);
  Serial.println("╚═══════════════════════════════════════════╝");
  Serial.println("");

  esp_wifi_set_channel(chan, WIFI_SECOND_CHAN_NONE);
  delay(100);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ERROR: esp_now_init failed — rebooting");
    delay(3000); ESP.restart();
  }
  esp_now_register_recv_cb(onDataReceived);
  Serial.printf("ESP-NOW ready on channel %d ✓\n", chan);

  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "*");

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "text/plain",
      "ZENURA V9 firmware OK\nWebSocket: ws://"
      + WiFi.localIP().toString() + "/ws\nLED: GPIO 26");
  });
  server.on("/ping", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "application/json",
      "{\"status\":\"ok\",\"version\":\"V9\",\"ip\":\""
      + WiFi.localIP().toString()
      + "\",\"led\":" + (ledState?"true":"false") + "}");
  });
  // Manual LED toggle via browser URL (handy for testing)
  server.on("/led", HTTP_GET, [](AsyncWebServerRequest* req) {
    toggleLed(nullptr);
    req->send(200, "text/plain",
      String("LED is now ") + (ledState ? "ON" : "OFF"));
  });
  server.begin();

  Serial.println("WebSocket server started ✓");
  Serial.println("");
  Serial.println("── READY ────────────────────────────────────");
  Serial.println("  1. Connect laptop to: Skip");
  Serial.println("  2. Open zenura_dashboard_V2.html in browser");
  Serial.printf ("  3. Enter IP %s when prompted\n", WiFi.localIP().toString().c_str());
  Serial.println("  4. Wire LED: GPIO26 → 220Ω → LED(+) → GND");
  Serial.println("─────────────────────────────────────────────");
}

// ════════════════════════════════════════════════════════════════
void loop() {
  // ── Forward EOG packets to WebSocket clients ──────────────────
  if (g_newData) {
    g_newData = false;
    char json[180];
    snprintf(json, sizeof(json),
      "{\"type\":\"eog\","
      "\"eye_x\":%d,\"eye_y\":%d,"
      "\"blink_type\":%u,"
      "\"leads_off\":%s,"
      "\"raw_adc\":%u,"
      "\"filtered_adc\":%u}",
      (int)g_eyeX, (int)g_eyeY,
      (unsigned)g_blink,
      g_leadsOff ? "true" : "false",
      (unsigned)g_raw, (unsigned)g_filtered
    );
    ws.textAll(json);
    g_blink = 0;
  }

  // ── Process pending Telegram job (non-blocking from WS handler) ─
  if (tgJob.pending) {
    tgJob.pending = false;
    String result = dispatchTelegram(tgJob.cls, tgJob.src, tgJob.tileName);
    // Find the client and reply
    AsyncWebSocketClient* client = ws.client(tgJob.clientId);
    if (client && client->status() == WS_CONNECTED) {
      client->text(result);
    } else {
      // Client gone — broadcast result anyway
      ws.textAll(result);
    }
  }

  ws.cleanupClients();
  delay(1);
}
