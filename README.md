# Project JANUS: Interlock Automation

![ESP8266](https://img.shields.io/badge/Platform-ESP8266-blue?logo=espressif)
![Framework](https://img.shields.io/badge/Framework-Arduino-teal?logo=arduino)
![Version](https://img.shields.io/badge/Firmware-v5.0-informational)
![Status](https://img.shields.io/badge/Status-Active-success)
![Security](https://img.shields.io/badge/Security-SharedKey%20%2B%20IP%20Validation-green)

---

## Overview

Project JANUS is a **distributed interlock door control system** built using **ESP8266 (NodeMCU)** devices.

The system allows multiple door controllers to communicate with each other over a local Wi-Fi network using **UDP broadcast messages** authenticated with a shared key. Each device exchanges status information and commands to ensure that **only one door can be opened at a time**, implementing a safety interlocking mechanism.

This type of system is commonly used in:

- Laboratories and clean rooms
- Controlled access areas and security entrances
- Industrial access points requiring physical interlock

---

## System Architecture

<div align = "center">
  <pre>
 ┌──────────────────┐
 │     PORTEIRO     │
 │ MASTER  (prio 3) │
 │  Buttons/Bypass  │
 └────────┬─────────┘
│
 ─── UDP Broadcast, port 4210 (SHARED_KEY authenticated) ───
│
 ┌──────────────┴───────────────┐
 │                              │
 ┌──────────────────┐          ┌──────────────────┐
 │     PORTA_A      │          │     PORTA_B      │
 │   Door Control   │          │   Door Control   │
 │    (prio 2)      │          │     (prio 1)     │
 └──────────────────┘          └──────────────────┘
│                              │
 Relay + Sensor                 Relay + Sensor
 Facial Recognition             Facial Recognition
  </pre>

| Device | Role | Priority |
|---|---|---|
| **PORTEIRO** | Central control panel — buttons, bypass switch, default master | 3 (highest) |
| **PORTA_A** | Controls door A | 2 |
| **PORTA_B** | Controls door B | 1 |
</div>

---

## Demo

> 📹 **Video demonstration coming soon.**


---

## Installation

### Requirements

- [Arduino IDE](https://www.arduino.cc/en/software) 1.8+ or 2.x
- ESP8266 board support package ([install guide](https://arduino-esp8266.readthedocs.io/en/latest/installing.html))

**Required libraries** — all included with the ESP8266 board package, no external libraries needed:

<div align = "center">
  
| Library | Purpose |
|---|---|
| `ESP8266WiFi` | Wi-Fi connection and AP management |
| `WiFiUdp` | UDP broadcast communication |
| `ESP8266WebServer` | Configuration and status web portals |
| `EEPROM` | Persistent credential storage |

</div>

### Steps

**1.** Clone or download this repository.

**2.** Open `Node_Porta_A.ino` in Arduino IDE.

**3.** Set the role of this device by editing line 24:

```cpp
#define DEVICE_NAME  "PORTA_A"   // "PORTA_A", "PORTA_B" or "PORTEIRO"
```

**4.** Set the shared key — **must be identical on all three devices**:

```cpp
#define SHARED_KEY "WIRES2025"   // change before flashing in production
```

**5.** If desired, change the fallback AP password before flashing:

```cpp
#define FALLBACK_AP_PASS "porteiro123"
```

**6.** Select your board: **Tools → Board → NodeMCU 1.0 (ESP-12E Module)**

**7.** Select the correct port under **Tools → Port**.

**8.** Upload the firmware.

**9.** On first boot:
   - **PORTEIRO**: creates AP `PORTEIRO_CONFIG`. Connect to it and open `192.168.4.1` to configure Wi-Fi credentials.
   - **PORTA_A / PORTA_B**: automatically connects to `PORTEIRO_AP` and waits to receive credentials from the PORTEIRO via UDP. No manual configuration needed.

Repeat steps 3–8 for each device, changing `DEVICE_NAME` and keeping `SHARED_KEY` identical.

### Resetting Wi-Fi credentials

To force a device back into CONFIG mode, call `eepromClearCredentials()` once in `setup()` and reflash. Remove the call afterwards.

---

## Circuit Diagram

### PORTA_A / PORTA_B

```
NodeMCU (ESP8266)
        │
        ├── D1 (GPIO 5)  ────────────── Facial Recognition / Button
        │                               (other pin → GND, INPUT_PULLUP)
        │
        ├── D5 (GPIO 14) ────────────── Door Magnetic Sensor
        │                               (other pin → GND, INPUT_PULLUP)
        │
        ├── D6 (GPIO 12) ────────────── Relay IN1 → Magnetic Lock
        │
        └── D7 (GPIO 13) ────────────── Relay IN2 → Push/Pull Actuator


Relay Module (active LOW)
        │
        ├── IN1 (RELAY_PIN) ─── Magnetic door lock
        ├── IN2 (PUPE_PIN)  ─── Push/Pull actuator
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
        ├── D3 (GPIO 0)  ────────────── Bypass toggle switch ⚠️ see note below
        │                               (other pin → GND, INPUT_PULLUP)
        │
        ├── D5 (GPIO 14) ────────────── Door sensor (if PORTEIRO has a door)
        │
        ├── D6 (GPIO 12) ────────────── Relay IN1 (if applicable)
        │
        └── D7 (GPIO 13) ────────────── Relay IN2 (if applicable)
```

> ⚠️ **GPIO0 (D3) — Bypass Pin Boot Warning**
>
> GPIO0 controls the ESP8266 boot mode. If this pin is LOW when the device powers on, the chip enters firmware-flashing mode and **will not run the firmware**.
>
> **Hardware fix (recommended):** solder a 10 kΩ pull-up resistor between D3 and 3.3 V. This guarantees HIGH at boot regardless of switch position.
>
> **Software mitigation (already implemented):** the firmware detects and logs a warning if GPIO0 is LOW at boot, and ignores the bypass pin for the first 2 seconds to prevent a closed switch from being misread as bypass-active on startup.

> **Note:** All inputs use `INPUT_PULLUP`. Connect the switch between the pin and GND.

---

## Power Supply Recommendations

For reliable 24/7 operation:

- Use a **5 V / 2 A minimum** power supply per NodeMCU.
- Place a **100 µF electrolytic capacitor** between VIN and GND as close to the NodeMCU as possible. Relay switching generates current spikes that can cause spurious resets without it.
- Connect the **positive leg to VIN**, negative leg to GND.

---

## Wi-Fi Modes

The firmware selects its Wi-Fi mode automatically at boot:

<div align = "center">
  
| Mode | Condition | Behavior |
|---|---|---|
| **NORMAL** | Credentials saved in EEPROM, router reachable | Connects to the configured router |
| **CONFIG** | No credentials saved — PORTEIRO only | Creates AP `PORTEIRO_CONFIG`, serves web portal at `192.168.4.1` |
| **FALLBACK** | Router unreachable for 20 s | PORTEIRO creates AP `PORTEIRO_AP`; PORTA_A/B reconnect to it automatically |

</div>

### First Boot — Zero-Touch Credential Distribution

Only the **PORTEIRO** requires manual configuration:

1. PORTEIRO boots with no credentials → creates AP `PORTEIRO_CONFIG`.
2. Connect to it and open `192.168.4.1` → enter router SSID and password → submit.
3. PORTEIRO saves credentials to EEPROM and restarts, connecting to the router.
4. PORTA_A and PORTA_B boot without credentials → automatically connect to `PORTEIRO_AP`.
5. PORTEIRO detects unconfigured nodes (identified by their `192.168.4.x` IP) and broadcasts `WIFICFG|ssid|pass` every 30 s.
6. Each door node receives credentials, saves to EEPROM, and restarts — connecting directly to the router.

No manual Wi-Fi configuration is needed on PORTA_A or PORTA_B.

> **Security:** WIFICFG is accepted only by nodes with no stored credentials, and only from `192.168.4.x` source addresses (the PORTEIRO AP subnet). Already-configured nodes discard all WIFICFG messages entirely.

### Fallback Mode — Emergency Network

If the router goes offline:

1. After 20 s without a connection, PORTEIRO creates AP `PORTEIRO_AP`.
2. PORTA_A and PORTA_B detect the loss and connect to `PORTEIRO_AP` automatically.
3. Full interlock operation continues on the local emergency network.
4. A live status page at `192.168.4.1` shows door states and node connectivity, auto-refreshing every 5 s.
5. When the router returns, all devices reconnect automatically.

---

## Security

All UDP messages are authenticated with a **shared key** prepended to every packet:

```
Format:  SHARED_KEY|COMMAND|...
Example: WIRES2025|STATUS|PORTA_A|OPEN
```

On receive, any packet without the correct key prefix is silently discarded before any processing. This prevents commands from unknown devices, IP spoofing, and accidental interference from other equipment on the same network.

In addition, all sensitive commands are validated against a **known-IP table** (`deviceKnownByIP()`). Only devices that have previously announced themselves via DISCOVERY are accepted.

<div align = "center">
  
| Protection | Commands covered |
|---|---|
| Shared key verification | All commands |
| Known-IP validation | OPEN, BYPASS, LOCK, UNLOCK, STATUS, ALERT |
| Source-IP restriction | WIFICFG (only `192.168.4.x`, only when unconfigured) |

</div>

> **To change the key:** update `#define SHARED_KEY` to the same value on all three devices before flashing. Devices with mismatched keys will not communicate.

---

## Communication Protocol

All messages are prefixed with `SHARED_KEY|` in transit. The table below shows logical payloads:

<div align = "center">
  
| Message | Direction | Purpose |
|---|---|---|
| `DISCOVERY\|DEV\|IP` | Broadcast | Announces presence |
| `CONFIRM\|DEV\|IP` | Broadcast | Confirms registration |
| `HELLO\|DEV\|PRIO\|IP\|ROLE\|LEASE` | Broadcast | Master election heartbeat (every 15 s) |
| `PING\|DEV\|IP` | Broadcast | Keepalive (every 15 s) |
| `PONG\|DEV\|IP` | Unicast reply | Response to PING |
| `STATUS\|DEV\|OPEN\|CLOSED` | Broadcast | Door state sync (every 15 s and on change) |
| `LOCK\|DEV` | Broadcast | Relay activated — blocks the other door immediately |
| `UNLOCK\|DEV` | Broadcast | Relay finished — releases the other door |
| `REQ_OPEN\|PORTA\|ORIGIN\|TOKEN` | Broadcast | Node requests open permission |
| `ALLOW\|PORTA\|TOKEN` | Broadcast | Master grants permission |
| `DENY\|PORTA` | Broadcast | Master denies permission |
| `ACK_OPEN\|PORTA` | Broadcast | Node confirms relay activated |
| `OPEN\|PORTA[\|M]` | Broadcast | Direct open from master; `M` flag = master override |
| `BYPASS\|ON\|OFF` | Broadcast | Enable / disable bypass |
| `WIFICFG\|SSID\|PASS` | Broadcast | Credential distribution (bootstrap only) |
| `ALERT\|TYPE\|ORIGIN` | Broadcast | Door open too long |

</div>

---

## Master Election

- Fixed priorities: PORTEIRO (3) > PORTA_A (2) > PORTA_B (1).
- All devices broadcast `HELLO` every 15 s.
- Highest priority wins. Equal priority: higher IP address wins.
- If no `HELLO` from the master within 20 s, any node assumes the role.
- **Split-brain prevention:** a higher-priority master appearing causes the current master to step down immediately.
- All time comparisons use `millis() - lastTime > interval` — safe beyond the 49-day `millis()` rollover.

---

## Interlock Logic

A door can only open when **all** of the following are true:

1. The other door is confirmed **closed** (recent `STATUS` message).
2. **Communication with the other node is active** — `STATUS` received within 10 s. If stale, the exact time since last contact is logged and opening is blocked.
3. No `LOCK` is active from the other door.
4. The **other door's relay is not active** — tracked by `portaXRelayAtivo`, set the moment the relay energises, before the physical sensor moves.
5. The system is not in the **2 s post-open cooldown**.

If communication is lost, the system **blocks by default** for safety.

### Relay-Aware Flags

```cpp
bool portaARelayAtivo;   // true from relay ON → OFF on PORTA_A
bool portaBRelayAtivo;   // true from relay ON → OFF on PORTA_B
```

Set on `LOCK` receipt, cleared on `UNLOCK`. These close the race window between relay activation and physical door movement.

### Token System

- `masterBusy` is set **immediately** on any open request, before validation — eliminating the concurrent-request race.
- Token expires only when: timeout elapsed **and** no relay is active anywhere.
- Token is explicitly invalidated when the relay turns off — no micro-window remains.
- `MASTER_BUSY_TIMEOUT` is derived from `RELAY_TIME + 1000 ms` — changing relay duration automatically updates the timeout.

### Request Flow (node is not master)

```
Node  ──REQ_OPEN──▶  Master
      (retries up to 3× at 1 s intervals if no reply)
Node  ◀──ALLOW────   Master
Node  activates relay
Node  ──ACK_OPEN──▶  Master
Master releases token
```

### Fast Path (PORTEIRO is master)

```
PORTEIRO  locks masterBusy immediately
PORTEIRO  checks interlock locally
PORTEIRO  activates relay directly   (no network roundtrip)
PORTEIRO  ──ACK_OPEN──▶  Network
```

Eliminates the REQ_OPEN/ALLOW roundtrip, reducing latency by 20–80 ms.

### Master Override

The PORTEIRO can force a door open regardless of interlock state. Sends `OPEN|PORTA_X|M`; the receiving node skips `podeAbrir()` entirely. Used for emergencies or maintenance.

---

## Bypass Mode

When bypass is active:

- Sensor state, lock flags, and communication-loss checks are ignored.
- **Relay-active protection is still enforced** — a door whose relay is physically moving cannot be overridden by bypass. This prevents mechanical damage.
- Only the PORTEIRO's master override (`OPEN|...|M`) bypasses relay-active protection.
- Bypass state is broadcast to all devices on change.
- Controlled by a physical switch on the PORTEIRO (GPIO 0).
- Ignored for the first 2 s after boot (GPIO0 boot guard).

---

## Hardware

**Controller:** NodeMCU (ESP8266)

### Inputs

<div align = "center">

| Constant | NodeMCU Pin | GPIO | Function |
|---|---|---|---|
| `BTN1_PIN` | D1 | 5 | Facial recognition trigger / button 1 (PORTEIRO) |
| `BTN2_PIN` | D2 | 4 | Button 2 — opens PORTA_B (PORTEIRO only) |
| `BYPASS_PIN` | D3 | 0 | Bypass toggle switch ⚠️ GPIO0 boot-sensitive |
| `SENSOR_PIN` | D5 | 14 | Magnetic door sensor (`HIGH` = open with `INPUT_PULLUP`) |

</div>

### Outputs

<div align = "center">
  
| Constant | NodeMCU Pin | GPIO | Function |
|---|---|---|---|
| `RELAY_PIN` | D6 | 12 | Relay IN1 — magnetic door lock |
| `PUPE_PIN` | D7 | 13 | Relay IN2 — push/pull actuator |

</div>

Relay outputs are **active LOW** — driven `HIGH` (off) by default, `LOW` to activate.

---

## Door Operation Flow

1. User is validated by the **facial recognition device** or button.
2. The node checks interlock status via the master.
3. If the other door is confirmed closed and all relay flags are clear, the master grants permission.
4. **Relay activates**: lock and actuator energise for `RELAY_TIME` (5 s).
5. `LOCK` is broadcast immediately — before the sensor registers movement.
6. `STATUS` is broadcast on every state change.
7. When the door closes: relay de-energises, `UNLOCK` is broadcast, token and busy flag are released.
8. If the door stays open for more than 5 minutes, an `ALERT` is broadcast.

---

## Reliability Features

<div align = "center">
  
| Feature | Detail |
|---|---|
| EEPROM persistence | Wi-Fi credentials survive power cycles |
| Zero-touch credential distribution | PORTA_A/B self-configure via UDP from the PORTEIRO |
| Fallback AP | Full interlock operation if router goes offline |
| Live status web page | `192.168.4.1` in fallback mode, auto-refresh every 5 s |
| Master election | Automatic failover; split-brain prevention |
| REQ_OPEN retry | Up to 3 retries at 1 s intervals |
| Relay-aware interlock | `portaXRelayAtivo` blocks the other door from relay activation |
| State-aware token | Token only expires when timeout elapsed **and** relay idle |
| Immediate masterBusy lock | Set before any validation — race condition closed |
| Token invalidated on relay off | Explicit release at shutdown — no micro-window |
| `MASTER_BUSY_TIMEOUT` auto-derived | `RELAY_TIME + 1000 ms` — stays correct if relay time changes |
| Device table LRU eviction | Full table replaces least-recently-seen device instead of blocking |
| STATUS updates keepalive | Receiving STATUS counts as a ping — reduces false "inactive" removals |
| Hardware watchdog | `ESP.wdtEnable(8000)` + `wdtFeed()` in blocking loops |
| Logic watchdog | `APP_WATCHDOG_TIMEOUT` (8 s) — restarts on silent logic freeze |
| Sensor debounce | 100 ms on door sensor |
| Post-open cooldown | 2 s after any open event |
| millis() overflow safety | All comparisons use `millis() - lastTime > interval` — correct beyond 49 days |
| Heap fragmentation prevention | No dynamic `String` in hot paths; streaming `client.print(F(...))` in portal; `snprintf` into fixed buffers for IPs |
| GPIO0 boot guard | 2 s delay + log warning if bypass pin is LOW at boot |

</div>

---

## Security Features

<div align = "center">
  
| Feature | Detail |
|---|---|
| Shared key authentication | `SHARED_KEY` prefix required on every UDP packet; mismatches silently discarded |
| Known-IP validation | `deviceKnownByIP()` on OPEN, BYPASS, LOCK, UNLOCK, STATUS, ALERT |
| WIFICFG bootstrap protection | Only accepted when unconfigured and source is `192.168.4.x` |
| WIFICFG ignored after config | Configured nodes discard credential messages entirely |
| Master override scoped | `OPEN|...|M` only meaningful when issued by the PORTEIRO (priority 3) |

</div>

---

## EEPROM Layout

<div align = "center">
  
| Address | Content |
|---|---|
| 0 – 63 | Wi-Fi SSID (null-terminated) |
| 64 – 127 | Wi-Fi password (null-terminated) |
| 128 | Validity flag (`0xAA` = credentials present) |

</div>

---

## Configuration Reference

<div align = "center">
  
| Constant | Default | Description |
|---|---|---|
| `DEVICE_NAME` | `"PORTA_A"` | Role of this node — change per device |
| `SHARED_KEY` | `"WIRES2025"` | Authentication key — must match on all devices |
| `FALLBACK_AP_SSID` | `"PORTEIRO_AP"` | SSID of the emergency AP |
| `FALLBACK_AP_PASS` | `"porteiro123"` | Password of the emergency AP |
| `UDP_PORT` | `4210` | UDP port for all communication |
| `RELAY_TIME` | `5000` ms | Duration relay stays active |
| `MASTER_BUSY_TIMEOUT` | `RELAY_TIME + 1000` ms | Master lock timeout (auto-derived from `RELAY_TIME`) |
| `TOKEN_TIMEOUT` | `10000` ms | Token expiry when relay is idle |
| `PORTA_TIMEOUT` | `300000` ms | Alert threshold for door left open (5 min) |
| `MASTER_TIMEOUT` | `20000` ms | Silence before a master is replaced |
| `MASTER_LEASE_TIME` | `15000` ms | Lease renewal interval |
| `APP_WATCHDOG_TIMEOUT` | `8000` ms | Logic watchdog stall threshold |
| `MAX_DEVICES` | `10` | Discovery table capacity |
| `REQ_MAX_RETRIES` | `3` | Open request retry limit |
| `REQ_RETRY_INTERVAL` | `1000` ms | Interval between retries |
| `WIFI_CONNECT_TIMEOUT` | `15000` ms | Max wait for router at boot |
| `WIFI_RECONNECT_TIMEOUT` | `20000` ms | Time before fallback AP activates |
| `WIFICFG_INTERVAL` | `30000` ms | PORTEIRO credential re-broadcast interval |

</div>

---

## Future Improvements

- ESP-NOW as a secondary communication channel (no router dependency)
- Persistent event log (SPIFFS / LittleFS)
- HTTPS configuration portal with authentication
- Over-the-air (OTA) firmware updates
- MQTT / cloud integration for remote monitoring
- Multi-door expansion beyond two interlocked doors
- Mobile app or web dashboard

---

## Author

Developed by **Davi Han Ko**
