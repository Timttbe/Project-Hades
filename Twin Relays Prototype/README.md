# Project HADES – Twin Relays Prototype

## Overview

The **Twin Relays Prototype** is an early experimental version of **Project HADES**, developed to explore wireless relay control using **ESP-01 (ESP8266)** modules.

This prototype implements a simple distributed relay control system composed of two devices communicating directly using **ESP-NOW**, a low-latency peer-to-peer protocol provided by the ESP8266 platform.

The main goal of this version was to experiment with:

- device-to-device wireless communication
- remote relay triggering
- automatic device discovery
- embedded web interfaces on microcontrollers
- timed relay activation

Although this prototype predates the current **NodeMCU + UDP distributed architecture used in Project HADES**, it served as the foundation that validated the communication concepts later used in the main project.

---

## System Architecture

The prototype consists of two ESP-01 devices with different roles.

| Device | Role |
|------|------|
| **Lobby** | Web interface and command sender |
| **Gate** | Relay controller |

The **lobby device** acts as a small control panel. It creates a Wi-Fi network and hosts a web interface that allows the user to trigger the relay remotely.

The **gate device** listens for commands and activates the relay when instructed.

```
User (Phone / PC)
        │
        │ WiFi
        ▼
   LOBBY (ESP-01)
 Web Server + Control
        │
        │ ESP-NOW
        ▼
   GATE (ESP-01)
   Relay Controller
```

---

## Communication

Communication between the devices is implemented using **ESP-NOW**, a wireless protocol that allows ESP8266 devices to send data directly to each other without requiring a Wi-Fi router.

This approach provides:

- very low latency
- minimal communication overhead
- direct peer-to-peer messaging
- simple configuration

The firmware implements a lightweight message structure used to exchange commands and discovery information between devices.

| Message | Purpose |
|------|------|
| `DISCOVERY` | Search for relay devices |
| `DISCOVERY_RESPONSE` | Relay responds to the controller |
| `RELAY_COMMAND` | Command to activate the relay |

This mechanism allows the controller device to automatically detect relay devices without requiring manual configuration.

---

## Web Interface

The **Portaria device** hosts a lightweight web server that provides a simple control interface.

The ESP-01 operates as a **Wi-Fi Access Point**, allowing users to connect directly using a **smartphone or computer**.

Example network configuration:

```
SSID: ESP-Relay-Control
Password: 12345678
```

Once connected, the interface can be accessed at:

```
http://192.168.4.1
```

Through this interface the user can trigger the relay, which sends a command via **ESP-NOW** to the relay controller.

This interface was implemented to test how embedded devices could provide simple remote control capabilities without requiring external infrastructure.

---

## Relay Control Logic

The **Gate device** is responsible for controlling the relay hardware.

When a command is received:

1. The relay is activated.
2. A timer starts.
3. After the configured delay, the relay is automatically deactivated.

Example relay operation flow:

```
Command received
      ↓
Relay ON
      ↓
Timer started
      ↓
Delay elapsed
      ↓
Relay OFF
```

This timed activation mechanism is useful for applications such as:

- electric gate triggers
- remote switches
- automation prototypes
- access control systems

---

## Hardware

The prototype uses simple hardware components.

### Controller

- ESP-01 (ESP8266)

### Relay Device

- ESP-01 (ESP8266)
- 1-channel relay module

### GPIO Usage

| Pin | Function |
|------|------|
| GPIO0 | Relay control |
| GPIO2 | Status LED |

---

## Purpose of This Prototype

The Twin Relays prototype represents the **first experimental stage** of the HADES project and was developed to validate several core concepts that would later evolve into the full **Project HADES interlock system**, including:

- wireless communication between ESP devices
- embedded web control interfaces
- remote relay automation
- device discovery mechanisms
- distributed control logic

The lessons learned from this prototype directly influenced the development of the **current NodeMCU-based distributed architecture used in Project HADES**.

---

## Author

Developed by **Davi Han Ko**