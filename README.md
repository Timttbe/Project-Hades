# {PROJECT HADES: INTERLOCK AUTOMATION}

## Overview

Project HADES is a **distributed interlock door control system** built using **ESP8266 (NodeMCU)** devices.

The system allows multiple door controllers to communicate with each other over a local Wi-Fi network using **UDP broadcast messages**.

Each device exchanges status information and commands to ensure that **only one door can be opened at a time**, implementing a safety **interlocking mechanism**.

This type of system is commonly used in:

- laboratories
- controlled access areas
- clean rooms
- security entrances

---

## System Architecture

The system is composed of multiple ESP8266 devices, each one assigned a role:

| Device | Function |
|------|------|
| **PORTA_A** | Controls door A |
| **PORTA_B** | Controls door B |
| **PORTEIRO** | Central control panel (buttons / bypass control) |

Each controller communicates with the others using **UDP broadcast messages**, allowing automatic device discovery and status synchronization.

---

## Communication

Devices communicate using **UDP broadcast packets** over the local network.

The firmware implements a lightweight communication protocol including:

| Message | Purpose |
|------|------|
| `DISCOVERY` | Device discovery on the network |
| `CONFIRM` | Confirmation of device registration |
| `PING` / `PONG` | Device health monitoring |
| `STATUS` | Door state synchronization |
| `OPEN` | Command to open a specific door |
| `BYPASS` | Enable/disable interlock bypass mode |

This architecture allows the system to dynamically detect devices and maintain communication without manual configuration.

---

## Interlock Logic

The core feature of the system is the **interlock safety mechanism**.

A door can only open if:

- the other door is **closed**
- communication with the other controller is **active**

If communication is lost, the system **blocks door opening for safety**.

This prevents both doors from being opened simultaneously.

---

## Bypass Mode

The system includes a **bypass mode** that temporarily disables the interlock logic.

When bypass is active:

- doors can open regardless of the other door state
- the system broadcasts the bypass status to all devices

This feature is useful for:

- maintenance
- emergency situations
- manual override

---

## Hardware

Each door controller is built using:

**Controller**
- NodeMCU (ESP8266)

**Inputs**
- Facial recognition device
- Door magnetic sensor

**Outputs**
- 4-channel relay module

Currently the system uses:

| Relay | Function |
|------|------|
| IN1 | Magnetic door lock |
| IN2 | Push/Pull actuator |
| IN3 | Not used |
| IN4 | Not used |

---

## Door Operation Flow

1. A user is validated by the **facial recognition device**.
2. The controller checks the **interlock status**.
3. If the other door is closed, the relay is activated.
4. The **magnetic lock** and **push/pull actuator** are enabled.
5. The **door sensor** monitors the door state.
6. The system broadcasts the updated door status to the network.

---

## Network Features

The firmware includes several features for reliability:

- Automatic device discovery
- Periodic heartbeat monitoring
- Status synchronization
- Inactive device detection
- Broadcast command system

---

## Firmware Features

- Distributed multi-node architecture
- UDP-based device discovery
- Interlocking safety logic
- Relay control with timed activation
- Door state monitoring
- Bypass safety mode
- Serial debug logging

---

## Future Improvements

Possible future upgrades include:

- Web interface for monitoring
- ESP-NOW communication
- Event logging
- Remote configuration
- Access control integration
- Multi-door expansion

---

## Author

Developed by **Davi Han Ko**
