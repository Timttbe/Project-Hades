# Wires Access Control

![Main Device](https://img.shields.io/badge/Platform-ESP8266-blue?logo=espressif)
![Framework](https://img.shields.io/badge/Framework-Arduino-teal?logo=arduino)
![Version](https://img.shields.io/badge/Firmware-v6.0-informational)
![Status](https://img.shields.io/badge/Status-Active-success)

---

## Overview

**Wires Access Control** is a distributed interlock door control system built on **ESP8266 (NodeMCU)** devices communicating over UDP.

The system ensures **only one door can be open at a time**, implementing a physical safety interlock. The PORTEIRO acts as the central controller — it generates its own Wi-Fi AP, manages door commands and bypass, and exposes a web dashboard for monitoring and control.

Typical use cases:

- Security entrances and airlocks
- Controlled access areas (labs, clean rooms, server rooms)
- Industrial access points requiring physical interlock

---

## System Architecture

<div align="center">
<pre>
 ┌──────────────────────────────┐
 │          PORTEIRO            │
 │  WiFi AP  ·  Web Dashboard   │
 │  BTN1 · BTN2 · Bypass Switch │
 └──────────────┬───────────────┘
│
UDP Broadcast · 192.168.4.255:4210
│
┌────────────┴────────────┐
│                         │
┌───────────────┐         ┌───────────────┐
│    PORTA_A    │         │    PORTA_B    │
│  Relay+Sensor │         │  Relay+Sensor │
│  Local Button │         │  Local Button │
└───────────────┘         └───────────────┘
</pre>

| Device | Role |
|---|---|
| **PORTEIRO** | Central controller — buttons, bypass, AP host, web dashboard |
| **PORTA_A** | Door A controller — relay, sensor, local button |
| **PORTA_B** | Door B controller — relay, sensor, local button |

</div>

---

## Network Design

The PORTEIRO always runs in **`WIFI_AP_STA` mode**:

- **AP side** (`PORTEIRO_AP`, fixed IP `192.168.4.1`) — PORTA_A and PORTA_B always connect here. All UDP interlock communication happens in this subnet. This never changes and is not affected by the router.
- **STA side** (optional) — connects to the local router so you can access the web dashboard from your LAN by IP. Configured via the web portal. If the router is unavailable, the AP side continues operating normally.

This design means **the interlock never depends on a router**. The router connection is purely for convenience (web access from LAN).

---

## Demo

> 📹 **Video demonstration coming soon.**

---

## Installation

### Requirements

- [Arduino IDE](https://www.arduino.cc/en/software) 1.8+ or 2.x
- ESP8266 board support package ([install guide](https://arduino-esp8266.readthedocs.io/en/latest/installing.html))

**Required libraries** — all included with the ESP8266 board package, no external libraries needed:

<div align="center">

| Library | Purpose |
|---|---|
| `ESP8266WiFi` | Wi-Fi AP and STA management |
| `WiFiUdp` | UDP broadcast communication |
| `ESP8266WebServer` | Web dashboard (PORTEIRO only) |
| `EEPROM` | Persistent router credential storage |

</div>

### Steps

**1.** Clone or download this repository.

**2.** Open `Node_Porta_A.ino` in Arduino IDE.

**3.** Set the role of this device by editing line 6:

```cpp
#define DEVICE_NAME "PORTA_A"  // "PORTA_A", "PORTA_B" or "PORTEIRO"
```

**4.** If desired, change the AP credentials before flashing (must match on all three devices):

```cpp
#define AP_SSID "PORTEIRO_AP"
#define AP_PASS "porteiro123"
```

**5.** Select your board: **Tools → Board → NodeMCU 1.0 (ESP-12E Module)**

**6.** Select the correct port under **Tools → Port**.

**7.** Upload. Repeat steps 3–7 for each device, changing only `DEVICE_NAME`.

### First Boot

- **PORTA_A / PORTA_B** — connect automatically to `PORTEIRO_AP` and are ready. No configuration needed.
- **PORTEIRO** — AP is immediately active at `192.168.4.1`. Connect to `PORTEIRO_AP` and open `http://192.168.4.1` to access the dashboard.

### Connecting the PORTEIRO to your local network (optional)

1. Connect to `PORTEIRO_AP`.
2. Open `http://192.168.4.1`.
3. Scroll to the **WiFi Local** section and click **CONFIGURAR WIFI**.
4. Enter your router's SSID and password and save.
5. The PORTEIRO connects to the router. The AP stays active — the doors are unaffected.
6. The router-assigned IP is shown in the dashboard topbar. Use it to access the dashboard from your LAN.

### Resetting router credentials

- **Via dashboard:** open `http://192.168.4.1` → WiFi Local section → **TROCAR REDE**.
- Credentials are stored only on the PORTEIRO's EEPROM. PORTA_A and PORTA_B never store credentials.

---

## Web Dashboard

Accessible at `http://192.168.4.1` (AP) or the router-assigned IP (LAN). PORTEIRO only.

<div align="center">

| Section | Contents |
|---|---|
| **Topbar** | AP IP, LAN IP (if connected), Wires branding |
| **PORTEIRO card** | Online status, AP name, connected router, bypass toggle button |
| **PORTAS grid** | Per-door card: online/offline dot, door open/closed state, IP, Open button |
| **WiFi Local** | Router SSID, LAN IP, signal strength, option to change network |

</div>

The page auto-refreshes every 4 seconds.

---

## Circuit Diagram

### PORTA_A / PORTA_B

```
NodeMCU (ESP8266)
        │
        ├── D1 (GPIO 5)  ── Facial recognition trigger / local button
        │                   (other pin → GND, INPUT_PULLUP)
        │
        ├── D5 (GPIO 14) ── Magnetic door sensor  (HIGH = open)
        │                   (other pin → GND, INPUT_PULLUP)
        │
        ├── D6 (GPIO 12) ── Relay IN1 → Magnetic lock   (active LOW)
        │
        └── D7 (GPIO 13) ── Relay IN2 → Push/pull LED   (active LOW)
```

### PORTEIRO

```
NodeMCU (ESP8266)
        │
        ├── D1 (GPIO 5)  ── Button 1 → opens PORTA_A
        │                   (other pin → GND, INPUT_PULLUP)
        │
        ├── D2 (GPIO 4)  ── Button 2 → opens PORTA_B
        │                   (other pin → GND, INPUT_PULLUP)
        │
        ├── D4 (GPIO 2)  ── Bypass toggle switch  ⚠️ see note below
        │                   (other pin → GND, INPUT_PULLUP)
        │
        ├── D5 (GPIO 14) ── Door sensor (if PORTEIRO has a door)
        │
        ├── D6 (GPIO 12) ── Relay IN1 (if applicable)
        │
        └── D7 (GPIO 13) ── Relay IN2 (if applicable)
```

> ⚠️ **GPIO2 (D4) — Bypass Pin Boot Warning**
>
> GPIO2 must be **HIGH at boot** or the ESP8266 may not start correctly. Do not connect the bypass switch to GND while the device is powering on.
>
> **Recommended:** use a normally-open switch. The firmware reads `INPUT_PULLUP`, so the pin is HIGH by default (bypass off) and goes LOW when the switch is pressed (bypass on).

> **Note:** All inputs use `INPUT_PULLUP`. Connect switches and sensors between the pin and GND.

---

## Power Supply Recommendations

For reliable 24/7 operation:

- Use a **5 V / 2 A minimum** power supply per NodeMCU.
- Place a **100 µF electrolytic capacitor** between VIN and GND close to the NodeMCU. Relay switching generates current spikes that can cause spurious resets without it.

---

## Interlock Logic

A door open request is approved only when **all** of the following are true:

1. The other door's sensor reports **closed** (recent `STATUS` message, within 10 s).
2. The other door's **relay is not active** — tracked from the moment the relay energises, before the physical sensor moves. This closes the race window on rapid double-presses.
3. The system is not in the **2 s post-open cooldown**.

If bypass mode is active, conditions 1 and 2 are skipped and the door opens immediately.

If communication with the other door is lost (no `STATUS` in 10 s), opening is **blocked by default** for safety.

---

## Bypass Mode

When bypass is active:

- The interlock check is fully skipped — doors can be opened regardless of the other door's state.
- Controlled by the physical switch on PORTEIRO (D4/GPIO2) **and** by the toggle button on the web dashboard.
- State is broadcast to all devices on every change so all nodes stay in sync.

---

## UDP Message Reference

All messages are plain-text, pipe-delimited, broadcast to `192.168.4.255:4210`.

<div align="center">

| Message | Direction | Description |
|---|---|---|
| `DISCOVERY\|name\|ip` | All → All | Announces presence on boot and every 5 s |
| `CONFIRM\|name\|ip` | All → All | Acknowledgement of a new device discovery |
| `PING\|name\|ip` | All → All | Keepalive every 10 s |
| `PONG\|name\|ip` | Response | Reply to PING |
| `STATUS\|name\|OPEN\|CLOSED` | All → All | Door state, sent every 3 s and on change |
| `OPEN\|name` | PORTEIRO → Door | Command to open a specific door |
| `REQ_OPEN\|door\|requester` | Door → PORTEIRO | Door requests permission to open |
| `BYPASS\|ON\|OFF` | PORTEIRO → All | Broadcast bypass state change |
| `LOCK\|name` | Door → All | Relay just activated (before sensor moves) |
| `UNLOCK\|name` | Door → All | Door closed, relay off |

</div>

---

## Hardware Reference

<div align="center">

### Inputs

| Constant | NodeMCU Pin | GPIO | Function |
|---|---|---|---|
| `BTN1_PIN` | D1 | 5 | Facial recognition / Button 1 |
| `BTN2_PIN` | D2 | 4 | Button 2 — opens PORTA_B (PORTEIRO only) |
| `BYPASS_PIN` | D4 | 2 | Bypass toggle switch ⚠️ HIGH at boot required |
| `SENSOR_PIN` | D5 | 14 | Magnetic door sensor (`HIGH` = open) |

### Outputs

| Constant | NodeMCU Pin | GPIO | Function |
|---|---|---|---|
| `RELAY_PIN` | D6 | 12 | Relay IN1 — magnetic lock (active LOW) |
| `PUPE_PIN` | D7 | 13 | Relay IN2 — push/pull indicator (active LOW) |

</div>

---

## Configuration Reference

<div align="center">

| Constant | Default | Description |
|---|---|---|
| `DEVICE_NAME` | `"PORTA_A"` | Role of this node — change per device |
| `AP_SSID` | `"PORTEIRO_AP"` | SSID of the PORTEIRO's AP |
| `AP_PASS` | `"porteiro123"` | Password of the PORTEIRO's AP |
| `UDP_PORT` | `4210` | UDP port for all communication |
| `RELAY_TIME` | `5000` ms | Duration relay stays active |
| `PORTA_TIMEOUT` | `300000` ms | Alert threshold for door left open (5 min) |

</div>

Router credentials (SSID + password) are stored in EEPROM at runtime via the web portal — no hardcoded credentials in the firmware.

---

## EEPROM Layout

<div align="center">

| Address | Content |
|---|---|
| 0 – 63 | Router SSID (null-terminated) |
| 64 – 127 | Router password (null-terminated) |
| 128 | Validity flag (`0xAA` = credentials present) |

</div>

Used by the PORTEIRO only. PORTA_A and PORTA_B never write to EEPROM.

---

## Future Improvements

- OTA (over-the-air) firmware updates
- Persistent event log (SPIFFS / LittleFS)
- HTTPS dashboard with login
- MQTT / cloud integration for remote monitoring
- Mobile app
- Multi-door expansion beyond two interlocked doors
- ESP-NOW as a secondary communication channel

---

## Author

Developed by **Davi Han Ko** — [Wires](https://github.com/wires)
