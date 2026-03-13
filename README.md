# {PROJECT HADES: INTERLOCK AUTOMATION}

## First Version:

This is the first version of the **ESP-01 relay communication firmware**.<br>

This project demonstrates a **wireless communication system between relays** using the **ESP-01 module (ESP8266)** without the need for a router or external Wi-Fi network.<br>

The relays communicate directly with each other using their **MAC addresses** through the **ESP-NOW protocol**.

---

## How It Works

Each relay is connected to an **ESP-01 module**.  
The ESP modules exchange messages directly using **ESP-NOW**, allowing the relays to synchronize their states.

This means that when one relay changes state, the other relay can update automatically.

The system works **without a traditional Wi-Fi network**, which means it keeps working even if:

- the router is turned off
- the internet connection fails
- there is no network infrastructure available

---

## Communication Method

The project uses **ESP-NOW**, a wireless protocol developed for ESP8266 and ESP32 devices.

ESP-NOW allows devices to communicate by sending small data packets directly between MAC addresses.

Advantages of ESP-NOW:

- No router required
- Very low latency
- Low power consumption
- Reliable device-to-device communication

---

## Current Implementation

This initial version demonstrates communication between **two relays working as synchronized devices**.

However, the system can be expanded to support:

- Multiple relays
- Status synchronization between devices
- Toggle or mirrored relay behavior
- Distributed relay control

---

## Hardware Used

- NodeMCU (ESP8266)
- 4 Channels Relay module
- Power supply
- Control buttons (optional)

---

## Author

Developed by **Davi Han Ko**