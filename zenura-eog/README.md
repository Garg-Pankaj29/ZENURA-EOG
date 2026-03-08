# ZENURA — EOG Eye-Controlled Assistive Dashboard

<div align="center">

![ZENURA Banner](https://img.shields.io/badge/ZENURA-EOG%20Neural%20Interface-22d3ee?style=for-the-badge&labelColor=000000)
![ESP32](https://img.shields.io/badge/ESP32-Dual%20Node-blue?style=for-the-badge&logo=espressif&logoColor=white)
![Arduino](https://img.shields.io/badge/Arduino%20IDE-2.3.8-teal?style=for-the-badge&logo=arduino&logoColor=white)
![License](https://img.shields.io/badge/License-MIT-green?style=for-the-badge)

**A low-cost, non-invasive eye-movement controlled smart dashboard for paralysed or speech-impaired patients.**  
Built with an AD8232 EOG sensor, two ESP32 microcontrollers, and a real-time browser dashboard with Telegram notifications.

</div>

---

## 📽 Demo

> Patient looks LEFT → cursor locks to left column  
> Patient blinks ONCE → NURSE tile triggers → Doctor receives Telegram instantly  
> Patient looks RIGHT + blinks TWICE → SOS fires → Doctor + Family both notified

---

## 🧠 How It Works

```
Electrodes on face
      ↓
AD8232 reads eye voltage (EOG signal)
      ↓
ESP32 Sensing Node processes signal:
  • Moving average filter (noise removal)
  • Baseline tracking (auto-calibrates)
  • Eye direction detection (left / right)
  • Blink detection (single / double)
      ↓  ESP-NOW wireless (no router needed)  ↓
ESP32 Receiver Node:
  • Creates "ZENURA-EOG" WiFi hotspot
  • Connects to internet via mobile hotspot
  • Streams data to browser via WebSocket
      ↓
Browser Dashboard:
  • Cyan cursor snaps to left/right column
  • Single blink = top tile | Double blink = bottom tile
  • Stare 1.5s = dwell trigger (no blink needed)
  • Demo Mode: mouse controls cursor for presentations
      ↓
Telegram Bot:
  • ESP32 sends notifications directly
  • No internet needed on the laptop
```

---

## 🏗 System Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        PATIENT FACE                         │
│   🔴 Red (Right temple)  🟢 Green (Forehead)  🟡 Yellow (Left temple) │
└───────────────────┬─────────────────────────────────────────┘
                    │ Analog signal
              ┌─────▼──────┐
              │   AD8232   │  ECG/EOG amplifier
              └─────┬──────┘
                    │ OUTPUT pin
        ┌───────────▼───────────────┐
        │   ESP32 SENSING NODE      │  (Sender — COM15)
        │   • MAF filter            │
        │   • Blink detection       │
        │   • Eye position calc     │
        │   • ESP-NOW TX @ 50Hz     │
        └───────────┬───────────────┘
                    │ ESP-NOW (wireless, no router)
        ┌───────────▼───────────────┐
        │   ESP32 RECEIVER NODE     │  (Receiver — COM14)
        │   • WiFi AP "ZENURA-EOG"  │◄── Laptop connects here
        │   • WiFi STA "Skip"       │◄── Internet for Telegram
        │   • WebSocket server      │
        │   • HTTPClient → Telegram │
        └───────────┬───────────────┘
                    │ WebSocket
        ┌───────────▼───────────────┐
        │   BROWSER DASHBOARD       │
        │   http://192.168.4.1      │
        │   • EOG + Demo modes      │
        │   • Real-time cursor      │
        │   • 4-tile interface      │
        └───────────────────────────┘
```

---

## 🛠 Hardware Required

| Component | Quantity | Purpose |
|-----------|----------|---------|
| DOIT ESP32 DEVKIT V1 | 2 | Sensing node + Receiver node |
| AD8232 ECG/EOG Module | 1 | Amplifies eye movement signal |
| Ag/AgCl Electrode Pads | 3 | Skin contact for EOG |
| 3-lead electrode cable | 1 | Connects pads to AD8232 |
| Breadboard + jumper wires | — | Connections |
| USB cables (micro) | 2 | Programming + power |

**Total estimated cost: ₹500–800**

---

## ⚡ Wiring

### AD8232 → ESP32 Sensing Node

| AD8232 Pin | ESP32 Pin |
|-----------|-----------|
| OUTPUT    | GPIO 34   |
| LO+       | GPIO 35   |
| LO-       | GPIO 32   |
| VCC       | 3.3V      |
| GND       | GND       |
| SDN       | 3.3V      |

### Electrode Placement

```
         🟢 Green
      (Center forehead)
           REF / RL

🔴 Red              🟡 Yellow
(Right temple)    (Left temple)
  RA / IN+           LA / IN-
outer corner of   outer corner of
  right eye          left eye
```

---

## 💻 Software Setup

### Prerequisites

- [Arduino IDE 2.x](https://www.arduino.cc/en/software)
- ESP32 board package installed (Tools → Board Manager → search `esp32` by Espressif, install **3.x**)

### Libraries (install via Library Manager)

| Library | Author | For |
|---------|--------|-----|
| ESP Async WebServer | ESP32Async (mathieucarbou) | Receiver web server |
| AsyncTCP | ESP32Async | Receiver async TCP |
| ArduinoJson | Benoit Blanchon | JSON parsing on ESP32 |

> ⚠️ Make sure you install **ESP32Async** versions — NOT the me-no-dev versions.

---

## 🚀 Flashing Instructions

### Step 1 — Flash the Receiver first

1. Open `zenura_receiver_V5.cpp` in Arduino IDE
2. Select board: **DOIT ESP32 DEVKIT V1**
3. Select port for receiver ESP32 (COM14 / ttyUSB1)
4. Upload
5. Open Serial Monitor (115200 baud)
6. Wait for:
   ```
   ╔══════════════════════════════════════╗
   ║ SoftAP MAC → 88:57:21:79:00:F9      ║
   ╚══════════════════════════════════════╝
   ╔══════════════════════════════════════╗
   ║  >>> SENDER MUST USE CHANNEL 6 <<<  ║
   ╚══════════════════════════════════════╝
   ```
7. **Note the SoftAP MAC and the channel number**

### Step 2 — Configure the Sensing Node

Open `zenura_sensing_V5.cpp` and update two things:

```cpp
// Line 21 — set to channel printed by receiver
#define WIFI_CHANNEL   6   // ← change this

// Line 28 — set to SoftAP MAC printed by receiver
uint8_t RECEIVER_MAC[] = { 0x88, 0x57, 0x21, 0x79, 0x00, 0xF9 };
```

### Step 3 — Flash the Sensing Node

1. Select port for sender ESP32 (COM15 / ttyUSB0)
2. Upload
3. Open Serial Monitor — you should see:
   ```
   >>> CALIBRATING — look straight ahead, DON'T blink <<<
   >>> Baseline: 1915  (good = 1600–2400)
   >>> READY — Look left/right and blink <<<
   X:240   F:2155   B:1915 | BLINK:0 LO:0 TX:OK   RIGHT >>>
   ```

### Step 4 — Access Dashboard

1. Turn on your phone's **hotspot named "Skip"** (password: `mynameisash`) for internet
2. Connect laptop WiFi to **ZENURA-EOG** (password: `zenura123`)
3. Open browser → `http://192.168.4.1`

---

## 🎛 Dashboard Controls

### EOG Mode (real eye control)
| Eye Action | Result |
|-----------|--------|
| Look LEFT | Cursor snaps to left column (NURSE / LIGHTS) |
| Look RIGHT | Cursor snaps to right column (FAMILY / SOS) |
| Look CENTER | Cursor rests in center zone — nothing triggers |
| Single Blink | Fires **top** tile of current column |
| Double Blink | Fires **bottom** tile of current column |
| Stare 1.5s | Dwell-triggers whichever tile cursor is on |

### Demo Mode (mouse control — for presentations)
| Mouse Action | Result |
|-------------|--------|
| Hover over tile | Tile highlights, dwell bar fills |
| Hover 1.5s | Auto-triggers tile |
| Click tile | Instantly triggers tile |

> Switch between modes using the button at bottom-right of dashboard.

---

## 📱 Telegram Notifications

The ESP32 sends Telegram messages directly (no internet needed on the laptop).

| Tile | Recipients | Message |
|------|-----------|---------|
| NURSE 🏥 | Doctor | Medical assistance requested |
| FAMILY 📞 | Family | Your loved one wants to reach you |
| SOS 🆘 | Doctor + Family | CRITICAL — immediate response needed |
| LIGHTS 💡 | — | No notification (local control) |

### Setup your own bot

1. Message `@BotFather` on Telegram → `/newbot` → get your token
2. Have each recipient message your bot
3. Visit `https://api.telegram.org/bot{TOKEN}/getUpdates` → get their `"id"`
4. Update in `zenura_receiver_V5.cpp`:
   ```cpp
   const char* TG_TOKEN  = "your_token_here";
   const char* DOCTOR_ID = "recipient_chat_id";
   const char* FAMILY_ID = "recipient_chat_id";
   ```

---

## 📁 Repository Structure

```
zenura-eog/
├── sensing_node/
│   └── zenura_sensing_V5.cpp      # Sender ESP32 firmware
├── receiver_node/
│   └── zenura_receiver_V5.cpp     # Receiver ESP32 firmware + dashboard HTML
├── docs/
│   └── wiring_diagram.png         # Hardware wiring reference
└── README.md
```

---

## 🔧 Tuning Parameters

In `zenura_sensing_V5.cpp`:

```cpp
#define BLINK_THRESHOLD   80   // lower = easier to detect blinks (try 60–120)
#define EOG_SENSITIVITY    1   // divide eye_x by this (1 = max sensitivity)
#define EOG_DEADZONE      10   // noise filter (lower = more responsive)
#define SNAP_THRESHOLD    30   // eye_x units to snap to column
```

In dashboard JavaScript (inside receiver .cpp HTML):
```javascript
const DWELL_MS = 1500;   // ms to hold gaze before auto-trigger
const SNAP = 40;         // eye_x threshold for column snap
```

---

## ⚠️ Troubleshooting

| Problem | Fix |
|---------|-----|
| TX:FAIL in Serial Monitor | Channel mismatch — check receiver serial for correct channel, update `#define WIFI_CHANNEL` in sender |
| Dashboard shows OFFLINE | Wrong SoftAP MAC in sender, or laptop not connected to ZENURA-EOG |
| Telegram: 0 sent · 2 failed | Laptop has no internet — this is normal. ESP32 uses phone hotspot instead |
| Baseline > 2700 or < 1400 | Check ADC_11db attenuation, check electrode connections |
| Always LO:1 | Leads off — press electrodes firmly, check GPIO 35/32 wiring |
| Blink not detected | Lower `BLINK_THRESHOLD` to 60, ensure electrodes are on temples not cheeks |
| Can't open 192.168.4.1 | Type `http://` explicitly — browser may assume https |

---

## 👥 Team

Made with ❤️ for a hackathon — **ZENURA** stands for  
*Zero-effort Neural User Response Assistant*

| Name | Role |
|------|------|
| Pankaj | Hardware, firmware, system architecture |
| [Teammate] | [Role] |

---

## 📄 License

MIT License — free to use, modify, and distribute with attribution.

---

<div align="center">
<sub>Built on ESP32 · AD8232 · ESP-NOW · WebSocket · Telegram Bot API</sub>
</div>
