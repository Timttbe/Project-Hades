# Project HADES: Interlock Automation

![ESP8266](https://img.shields.io/badge/Platform-ESP8266-blue?logo=espressif)
![Framework](https://img.shields.io/badge/Framework-Arduino-teal?logo=arduino)
![Version](https://img.shields.io/badge/Firmware-v4.0-informational)
![Status](https://img.shields.io/badge/Status-Active-success)

---

## Overview

Project HADES is a **distributed interlock door control system** built using **ESP8266 (NodeMCU)** devices.

The system allows multiple door controllers to communicate with each other over a local Wi-Fi network using **UDP broadcast messages**.

Each device exchanges status information and commands to ensure that **only one door can be opened at a time**, implementing a safety **interlocking mechanism**.

This type of system is commonly used in:

- Laboratories
- Controlled access areas
- Clean rooms
- Security entrances

---

## System Architecture

```
                    ┌──────────────────┐
                    │    PORTEIRO      │
                    │  MASTER  (prio 3)│
                    │  Buttons/Bypass  │
                    └────────┬─────────┘
                             │
              ───── UDP Broadcast (port 4210) ─────
                             │
              ┌──────────────┴──────────────┐
              │                             │
   ┌──────────────────┐           ┌──────────────────┐
   │     PORTA_A      │           │     PORTA_B      │
   │   Door Control   │           │   Door Control   │
   │    (prio 2)      │           │    (prio 1)      │
   └──────────────────┘           └──────────────────┘
         │                                │
   Relay + Sensor                   Relay + Sensor
   Facial Recognition               Facial Recognition
```

The system is composed of three ESP8266 devices, each assigned a fixed role:

| Device | Role |
|---|---|
| **PORTA_A** | Controls door A (priority 2) |
| **PORTA_B** | Controls door B (priority 1) |
| **PORTEIRO** | Central control panel — buttons, bypass switch, master node (priority 3) |

Devices communicate via **UDP broadcast** over the local network. A lightweight **master election protocol** ensures one device always acts as the authority for interlock decisions. The PORTEIRO holds the highest priority and is the default master.

---

## Demo

> 📹 **Video demonstration coming soon.**
>
> *This section will show a recording of the physical system in operation: button press on the PORTEIRO triggering PORTA_A, and PORTA_B blocked by the interlock while PORTA_A is open.*
>
> To contribute a demo, record the following scenario and open a PR:
> 1. PORTA_A opens on button press
> 2. Attempt to open PORTA_B while PORTA_A is open → blocked
> 3. PORTA_A closes → PORTA_B now opens successfully

---

## Installation

### Requirements

- [Arduino IDE](https://www.arduino.cc/en/software) 1.8+ or 2.x
- ESP8266 board support package ([install guide](https://arduino-esp8266.readthedocs.io/en/latest/installing.html))

**Required libraries** (all available via Arduino Library Manager):

| Library | Purpose |
|---|---|
| `ESP8266WiFi` | Included with ESP8266 board package |
| `WiFiUdp` | Included with ESP8266 board package |
| `ESP8266WebServer` | Included with ESP8266 board package |
| `EEPROM` | Included with ESP8266 board package |

No external libraries are required.

### Steps

**1.** Clone or download this repository.

**2.** Open `Node_Porta_A.ino` in Arduino IDE.

**3.** Set the role of this device by editing line 24:

```cpp
#define DEVICE_NAME  "PORTA_A"   // "PORTA_A", "PORTA_B" or "PORTEIRO"
```

**4.** If desired, change the fallback AP password before flashing:

```cpp
#define FALLBACK_AP_PASS "porteiro123"
```

**5.** Select your board: **Tools → Board → NodeMCU 1.0 (ESP-12E Module)**

**6.** Select the correct port under **Tools → Port**.

**7.** Upload the firmware.

**8.** On first boot, the device creates a Wi-Fi AP named `<DEVICE_NAME>_CONFIG`.
Connect to it and open `192.168.4.1` to configure the Wi-Fi credentials.

Repeat steps 3–8 for each device, changing `DEVICE_NAME` each time.

### Resetting Wi-Fi credentials

To force a device back into CONFIG mode, call `eepromClearCredentials()` once in `setup()` and reflash. Remove the call afterwards.

---

## Circuit Diagram

### PORTA_A / PORTA_B

```
NodeMCU (ESP8266)
        │
        ├── D1 (GPIO 5)  ────────────── Facial Recognition / Button
        │
        ├── D5 (GPIO 14) ────────────── Door Magnetic Sensor
        │                               (other pin → GND)
        │
        ├── D6 (GPIO 12) ────────────── Relay IN1 → Magnetic Lock
        │
        └── D7 (GPIO 13) ────────────── Relay IN2 → Push/Pull Actuator


Relay Module (active LOW)
        │
        ├── IN1 (RELAY_PIN) ─── Magnetic door lock
        ├── IN2 (PUPE_PIN)  ─── Push/Pull actuator
        ├── IN3             ─── (not used)
        ├── IN4             ─── (not used)
        ├── VCC  ────────────── 5V
        └── GND  ────────────── GND
```

### PORTEIRO

```
NodeMCU (ESP8266)
        │
        ├── D1 (GPIO 5)  ────────────── Button 1 → opens PORTA_A
        │                               (other pin → GND, INPUT_PULLUP)
        │
        ├── D2 (GPIO 4)  ────────────── Button 2 → opens PORTA_B
        │                               (other pin → GND, INPUT_PULLUP)
        │
        ├── D3 (GPIO 0)  ────────────── Bypass toggle switch
        │                               (other pin → GND, INPUT_PULLUP)
        │
        ├── D5 (GPIO 14) ────────────── Door sensor (if PORTEIRO has a door)
        │
        ├── D6 (GPIO 12) ────────────── Relay IN1 (if applicable)
        │
        └── D7 (GPIO 13) ────────────── Relay IN2 (if applicable)
```

> **Note:** All button and sensor inputs use internal `INPUT_PULLUP`. Connect the switch between the pin and GND — the firmware detects `LOW` as active.

---

## Wi-Fi Modes

The firmware selects its Wi-Fi mode automatically at boot:

| Mode | Condition | Behavior |
|---|---|---|
| **NORMAL** | Credentials saved in EEPROM, router reachable | Connects to the configured router |
| **CONFIG** | No credentials saved (first boot) | Creates AP `<DEVICE>_CONFIG`, serves a web configuration portal at `192.168.4.1` |
| **FALLBACK** | Router unreachable for 20 s | PORTEIRO creates AP `PORTEIRO_AP`; PORTA_A and PORTA_B reconnect to it automatically |

### First Boot — Configuration Portal

1. Device boots with no saved credentials.
2. Creates a Wi-Fi access point named `PORTA_A_CONFIG` (or `PORTA_B_CONFIG`, `PORTEIRO_CONFIG`).
3. Connect to this AP and open `192.168.4.1` in a browser.
4. Fill in the router SSID and password and submit the form.
5. Credentials are saved to EEPROM and the device restarts, connecting normally.

### Fallback Mode — Emergency Network

If the router goes offline while the system is running:

1. After 20 s without a connection, the PORTEIRO creates the AP `PORTEIRO_AP`.
2. PORTA_A and PORTA_B detect the router loss and connect to `PORTEIRO_AP` automatically.
3. Full interlock operation continues on the local emergency network.
4. A status page at `192.168.4.1` shows the state of all doors in real time (auto-refreshes every 5 s).
5. When the router returns, all devices reconnect to it automatically.

---

## Communication Protocol

Devices communicate using **UDP broadcast** on port `4210`.

| Message | Direction | Purpose |
|---|---|---|
| `DISCOVERY\|DEV\|IP` | Broadcast | Announces device presence on the network |
| `CONFIRM\|DEV\|IP` | Broadcast | Confirms device registration |
| `HELLO\|DEV\|PRIO\|IP\|ROLE\|LEASE` | Broadcast | Master election heartbeat |
| `PING\|DEV\|IP` | Broadcast | Keepalive sent every 15 s |
| `PONG\|DEV\|IP` | Unicast reply | Response to PING |
| `STATUS\|DEV\|OPEN\|CLOSED` | Broadcast | Door state synchronization |
| `REQ_OPEN\|PORTA\|ORIGIN\|TOKEN` | Broadcast | Node requests permission to open a door |
| `ALLOW\|PORTA\|TOKEN` | Broadcast | Master grants permission |
| `DENY\|PORTA` | Broadcast | Master denies permission |
| `ACK_OPEN\|PORTA` | Broadcast | Door confirms it opened |
| `OPEN\|PORTA` | Broadcast | Direct open command (master to node) |
| `LOCK\|DEV` | Broadcast | Signals door is open — blocks the other |
| `UNLOCK\|DEV` | Broadcast | Signals door closed — releases the other |
| `BYPASS\|ON\|OFF` | Broadcast | Enables or disables interlock bypass |
| `ALERT\|TYPE\|ORIGIN` | Broadcast | Door open too long warning |

---

## Master Election

The system uses a **priority-based master election** protocol:

- Each device has a fixed priority: PORTEIRO (3) > PORTA_A (2) > PORTA_B (1).
- All devices broadcast `HELLO` every 15 s with their priority and a lease duration.
- The device with the highest priority becomes master.
- If two devices have equal priority, the one with the higher IP address wins.
- If no `HELLO` is received from the master within 20 s, any node can assume the master role.
- Split-brain is prevented: if a higher-priority master appears, the current master immediately steps down.

---

## Interlock Logic

A door can only open if:

- The **other door is closed** (confirmed by its STATUS messages).
- **Communication with the other controller is active** (STATUS received within the last 10 s).
- No **LOCK** signal is active from the other door.
- The system is not in a 2 s **post-open cooldown**.

If communication is lost, the system **blocks door opening by default** for safety.

### Request Flow (node is not master)

```
Node  ──REQ_OPEN──▶  Master
Node  ◀──ALLOW────   Master   (interlock OK)
Node  opens relay
Node  ──ACK_OPEN──▶  Master
```

If no reply is received, the node retries up to 3 times with a 1 s interval before giving up.

### Fast Path (PORTEIRO is master)

```
PORTEIRO  checks interlock locally
PORTEIRO  activates relay directly   (no network roundtrip)
PORTEIRO  ──ACK_OPEN──▶  Network
```

Eliminates the REQ_OPEN/ALLOW roundtrip, reducing open latency by 20–80 ms.

---

## Bypass Mode

When bypass is active:

- Doors can open regardless of the other door's state.
- The bypass status is broadcast to all devices.
- Controlled by a dedicated physical switch on the PORTEIRO (`BYPASS_PIN`).

Useful for maintenance, emergency situations, or manual override.

---

## Hardware

**Controller:** NodeMCU (ESP8266)

**Inputs**

| Constant | NodeMCU Pin | GPIO | Function |
|---|---|---|---|
| `BTN1_PIN` | D1 | 5 | Facial recognition trigger / button 1 (PORTEIRO) |
| `BTN2_PIN` | D2 | 4 | Button 2 — opens PORTA_B (PORTEIRO only) |
| `BYPASS_PIN` | D3 | 0 | Bypass toggle switch (PORTEIRO only) |
| `SENSOR_PIN` | D5 | 14 | Magnetic door sensor |

**Outputs**

| Constant | NodeMCU Pin | GPIO | Function |
|---|---|---|---|
| `RELAY_PIN` | D6 | 12 | Relay IN1 — magnetic door lock |
| `PUPE_PIN` | D7 | 13 | Relay IN2 — push/pull actuator |

Relay outputs are **active LOW** — firmware drives them `HIGH` (off) by default and `LOW` to activate.

---

## Door Operation Flow

1. User is validated by the **facial recognition device** (or button press).
2. Controller checks the **interlock status** via the master node.
3. If the other door is confirmed closed, the master issues permission.
4. **Relay** activates: magnetic lock and push/pull actuator energise for `RELAY_TIME` (5 s).
5. The **door sensor** monitors whether the door actually opened.
6. System broadcasts `LOCK` and updated `STATUS` to the network.
7. When the door closes, `UNLOCK` and updated `STATUS` are broadcast.
8. If the door remains open for more than 5 minutes, an `ALERT` is broadcast.

---

## Reliability Features

| Feature | Detail |
|---|---|
| EEPROM persistence | Wi-Fi credentials survive power cycles |
| Fallback AP | Full operation if router goes offline |
| Master election | Automatic failover if master node is lost |
| REQ_OPEN retry | Up to 3 retries with 1 s interval on no response |
| Device expiry | Nodes not seen for 30 s are removed from the table |
| Hardware watchdog | `ESP.wdtFeed()` prevents spurious resets during blocking Wi-Fi loops |
| Sensor debounce | 100 ms debounce on door sensor input |
| Post-open cooldown | 2 s lock after any open event to prevent rapid re-triggering |
| Master busy timeout | System auto-releases after 10 s if ACK_OPEN is never received |

---

## EEPROM Layout

| Address | Content |
|---|---|
| 0 – 63 | Wi-Fi SSID (null-terminated) |
| 64 – 127 | Wi-Fi password (null-terminated) |
| 128 | Validity flag (`0xAA` = credentials present) |

To reset credentials and re-enter CONFIG mode, call `eepromClearCredentials()` once in `setup()` and reflash.

---

## Configuration Reference

Key constants at the top of the firmware:

| Constant | Default | Description |
|---|---|---|
| `DEVICE_NAME` | `"PORTA_A"` | Role of this node |
| `FALLBACK_AP_SSID` | `"PORTEIRO_AP"` | SSID of the emergency AP |
| `FALLBACK_AP_PASS` | `"porteiro123"` | Password of the emergency AP |
| `UDP_PORT` | `4210` | UDP port for all communication |
| `RELAY_TIME` | `5000` ms | Duration relay stays active |
| `PORTA_TIMEOUT` | `300000` ms | Alert threshold for door left open |
| `MASTER_TIMEOUT` | `20000` ms | Time before a silent master is replaced |
| `MAX_DEVICES` | `10` | Maximum devices in the discovery table |
| `REQ_MAX_RETRIES` | `3` | Open request retry limit |
| `WIFI_RECONNECT_TIMEOUT` | `20000` ms | Time before fallback AP activates |

---

## Future Improvements

- ESP-NOW as a secondary communication channel (no router dependency)
- Persistent event log (SPIFFS)
- HTTPS portal with authentication
- Over-the-air (OTA) firmware updates
- Multi-door expansion beyond two interlocked doors
- Access control integration (badge / biometric records)

---

## Author

Developed by **Davi Han Ko**
