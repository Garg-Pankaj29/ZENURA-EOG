// ================================================================
//  ZENURA — SENSING NODE  ★ FINAL V5 ★
//  Flash to: Sender ESP32 (COM15 / ttyUSB0)
//
//  ★ STEP 1: Flash RECEIVER first, note the channel it prints
//  ★ STEP 2: Set WIFI_CHANNEL below to match
//  ★ STEP 3: Flash this sender
//
//  ELECTRODE WIRING:
//    🔴 RED    → Right temple (outer eye corner) → RA on AD8232
//    🟡 YELLOW → Left  temple (outer eye corner) → LA on AD8232
//    🟢 GREEN  → Center forehead (reference)     → RL on AD8232
//
//  AD8232 → ESP32:
//    OUTPUT→GPIO34  LO+→GPIO35  LO-→GPIO32
//    VCC→3.3V  GND→GND  SDN→3.3V
// ================================================================

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// ╔══════════════════════════════════════════════════╗
// ║  ★ SET THIS TO MATCH WHAT RECEIVER PRINTS ★     ║
// ║  Receiver prints: ">>> SENDER USE CHANNEL X <<<"║
// ║  Default 1 — change if TX keeps failing         ║
#define WIFI_CHANNEL   1
// ╚══════════════════════════════════════════════════╝

// ── Receiver SoftAP MAC (from receiver Serial Monitor) ───────
uint8_t RECEIVER_MAC[] = { 0x88, 0x57, 0x21, 0x79, 0x00, 0xF9 };

// ── AD8232 Pins ───────────────────────────────────────────────
#define PIN_EOG      34
#define PIN_LO_PLUS  35
#define PIN_LO_MINUS 32

// ╔══════════════════════════════════════════════════╗
// ║  TUNING — adjust if needed                      ║
#define BLINK_THRESHOLD   80    // lower = easier blink detect (try 60-120)
#define BLINK_MIN_MS      30    // min blink duration ms
#define BLINK_MAX_MS     300    // max blink duration ms
#define BLINK_REFRACTORY 250    // ms gap before next blink counts
#define DOUBLE_BLINK_MS  650    // window for second blink
#define EOG_SENSITIVITY    1    // divide eye_x by this (1=max range)
#define EOG_DEADZONE      10    // noise deadzone (lower=more sensitive)
#define SNAP_THRESHOLD    30    // eye_x units to snap column (lower=easier)
// ╚══════════════════════════════════════════════════╝

// ── Packet struct (must match receiver EXACTLY) ───────────────
typedef struct {
  int16_t  eye_x;
  int16_t  eye_y;
  uint8_t  blink_type;    // 0=none 1=single 2=double
  bool     leads_off;
  uint16_t raw_adc;
  uint16_t filtered_adc;
} EOGPacket;

EOGPacket           txPacket;
esp_now_peer_info_t peerInfo;

// ── Moving Average Filter (size 8 = faster response) ─────────
#define MAF_SIZE 8
uint16_t mafBuf[MAF_SIZE] = {0};
uint8_t  mafIdx = 0;
uint32_t mafSum = 0;
bool     mafReady = false;

uint16_t maf(uint16_t s) {
  mafSum -= mafBuf[mafIdx];
  mafBuf[mafIdx] = s;
  mafSum += s;
  mafIdx = (mafIdx + 1) % MAF_SIZE;
  if (mafIdx == 0) mafReady = true;
  return (uint16_t)(mafSum / (mafReady ? MAF_SIZE : max((int)mafIdx, 1)));
}

// ── Baseline (slow drift tracker — stops during blinks) ───────
int32_t baseline = 2048;
bool    blinkActive = false;

void updateBaseline(uint16_t s) {
  if (blinkActive) return;  // freeze baseline during blink
  // Very slow drift — 1/512 weight
  baseline = (baseline * 511 + (int32_t)s) >> 9;
}

// ── Blink Detector ────────────────────────────────────────────
bool     inSpike      = false;
uint32_t spikeStartMs = 0;
uint32_t lastBlinkMs  = 0;
uint8_t  blinkCount   = 0;
uint32_t firstBlinkMs = 0;

uint8_t detectBlink(uint16_t filtered) {
  int32_t  delta  = abs((int32_t)filtered - baseline);
  uint32_t now    = millis();
  uint8_t  result = 0;

  if (!inSpike && delta > BLINK_THRESHOLD) {
    inSpike = true;
    blinkActive = true;
    spikeStartMs = now;
  }

  if (inSpike && delta < (BLINK_THRESHOLD / 2)) {
    uint32_t dur = now - spikeStartMs;
    inSpike = false;
    blinkActive = false;

    if (dur >= BLINK_MIN_MS && dur <= BLINK_MAX_MS &&
        (now - lastBlinkMs) > BLINK_REFRACTORY) {
      lastBlinkMs = now;
      if (blinkCount == 0) {
        blinkCount = 1;
        firstBlinkMs = now;
      } else if (blinkCount == 1 &&
                 (now - firstBlinkMs) < DOUBLE_BLINK_MS) {
        blinkCount = 0;
        result = 2;   // DOUBLE BLINK
      }
    }
  }

  // Single blink window expired
  if (blinkCount == 1 && (millis() - firstBlinkMs) >= DOUBLE_BLINK_MS) {
    blinkCount = 0;
    result = 1;   // SINGLE BLINK
  }

  return result;
}

// ── Eye position ──────────────────────────────────────────────
int16_t eyePos(uint16_t filtered) {
  int32_t offset = (int32_t)filtered - baseline;
  if (abs(offset) < EOG_DEADZONE) return 0;
  return (int16_t)constrain(offset / EOG_SENSITIVITY, -512, 512);
}

// ── TX callback (removed — using esp_now_send return value instead) ──

// ── Timing ────────────────────────────────────────────────────
#define SAMPLE_HZ  500
#define SEND_HZ     50

uint32_t lastSampleUs = 0;
uint32_t lastSendMs   = 0;
uint8_t  pendingBlink = 0;
uint8_t  txFailCount  = 0;

// ── Recalibration (send 'c' in Serial Monitor anytime X sticks at 512) ──
void doCalibration() {
  mafSum = 0; mafIdx = 0; mafReady = false;
  memset(mafBuf, 0, sizeof(mafBuf));
  inSpike = false; blinkActive = false;
  blinkCount = 0; pendingBlink = 0;
  Serial.println("");
  Serial.println(">>> CALIBRATING — look straight ahead, don't blink <<<");
  uint32_t calSum = 0;
  for (int i = 0; i < 1500; i++) {
    calSum += analogRead(PIN_EOG);
    delayMicroseconds(2000);
  }
  baseline = calSum / 1500;
  for (int i = 0; i < MAF_SIZE; i++) {
    mafBuf[i] = (uint16_t)baseline; mafSum += baseline;
  }
  mafReady = true;
  Serial.printf(">>> Baseline: %d", (int)baseline);
  if      (baseline >= 1600 && baseline <= 2400) Serial.println("  GOOD - electrodes OK");
  else if (baseline < 800)  Serial.println("  BAD - check electrode wires");
  else if (baseline > 2800) Serial.println("  BAD - check ADC_11db setting");
  else Serial.println("  MARGINAL - press electrodes firmly");
  Serial.println(">>> Type 'c' anytime to recalibrate");
  Serial.println("");
}

// ════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("==========================================");
  Serial.println("   ZENURA SENSING NODE  V5");
  Serial.println("==========================================");
  Serial.printf ("   Using WiFi channel: %d\n", WIFI_CHANNEL);
  Serial.printf ("   Receiver MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
    RECEIVER_MAC[0],RECEIVER_MAC[1],RECEIVER_MAC[2],
    RECEIVER_MAC[3],RECEIVER_MAC[4],RECEIVER_MAC[5]);
  Serial.println("==========================================");

  pinMode(PIN_LO_PLUS,  INPUT);
  pinMode(PIN_LO_MINUS, INPUT);

  // ADC setup — 11db for full 0-3.3V range (AD8232 ~1.5V bias)
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  // Warm up ADC
  for (int i = 0; i < 64; i++) analogRead(PIN_EOG);
  delay(50);

  // WiFi — force to target channel
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  delay(100);

  // ESP-NOW init
  if (esp_now_init() != ESP_OK) {
    Serial.println("ERROR: esp_now_init FAILED — restarting");
    delay(3000); ESP.restart();
  }
  // (no send callback needed — using esp_now_send return value)

  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, RECEIVER_MAC, 6);
  peerInfo.channel = 0;       // 0 = use current forced channel
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("ERROR: add peer failed — check RECEIVER_MAC");
  }

  Serial.println("Type 'c' in Serial Monitor anytime to recalibrate");
  Serial.println("Type 'b' to force a test blink");
  Serial.println("");
  doCalibration();
  Serial.println("Format: X  Filtered  Baseline | BLINK  LO  TX");

  lastSampleUs = micros();
  lastSendMs   = millis();
}

// ════════════════════════════════════════════════════════════════
void loop() {
  // ── Serial commands: 'c' = recalibrate, 'b' = test blink ─────
  if (Serial.available()) {
    char cmd = Serial.read();
    if (cmd == 'c' || cmd == 'C') doCalibration();
    else if (cmd == 'b' || cmd == 'B') { pendingBlink = 1; Serial.println("  [Manual blink test]"); }
  }
  // ── Sample at SAMPLE_HZ ───────────────────────────────────────
  if ((micros() - lastSampleUs) < (1000000UL / SAMPLE_HZ)) return;
  lastSampleUs = micros();

  bool leadsOff = (digitalRead(PIN_LO_PLUS)  == HIGH) ||
                  (digitalRead(PIN_LO_MINUS) == HIGH);

  uint16_t raw      = leadsOff ? (uint16_t)baseline : analogRead(PIN_EOG);
  uint16_t filtered = maf(raw);

  if (!leadsOff) updateBaseline(filtered);

  uint8_t blink = 0;
  if (!leadsOff) {
    blink = detectBlink(filtered);
    if (blink == 1) Serial.println("  >>> SINGLE BLINK detected <<<");
    if (blink == 2) Serial.println("  >>> DOUBLE BLINK detected <<<");
    if (blink > 0) pendingBlink = blink;
  }

  int16_t eyeX = leadsOff ? 0 : eyePos(filtered);

  // ── Transmit at SEND_HZ ───────────────────────────────────────
  uint32_t now = millis();
  if ((now - lastSendMs) < (1000UL / SEND_HZ)) return;
  lastSendMs = now;

  txPacket.eye_x        = eyeX;
  txPacket.eye_y        = 0;
  txPacket.blink_type   = pendingBlink;
  txPacket.leads_off    = leadsOff;
  txPacket.raw_adc      = raw;
  txPacket.filtered_adc = filtered;

  esp_err_t r = esp_now_send(RECEIVER_MAC,
                             (uint8_t*)&txPacket, sizeof(txPacket));
  bool sent = (r == ESP_OK);

  if (!sent) {
    txFailCount++;
    // After 20 consecutive fails, try re-adding peer
    if (txFailCount > 20) {
      txFailCount = 0;
      esp_now_del_peer(RECEIVER_MAC);
      delay(5);
      esp_now_add_peer(&peerInfo);
      Serial.println("  [Re-added peer after TX failures]");
    }
  } else {
    txFailCount = 0;
  }

  // Direction indicator for Serial Monitor
  const char* dir = "CENTER";
  if (eyeX < -SNAP_THRESHOLD)       dir = "<<< LEFT";
  else if (eyeX > SNAP_THRESHOLD)   dir = "RIGHT >>>";

  Serial.printf("X:%-5d F:%-5d B:%-5d | BLINK:%d LO:%d TX:%-4s  %s\n",
    eyeX, filtered, (int)baseline,
    pendingBlink, (int)leadsOff,
    sent ? "OK" : "FAIL",
    dir);

  pendingBlink = 0;
}
