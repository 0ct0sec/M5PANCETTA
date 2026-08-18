# HAMLET PANCETTA

![Pancetta reviews the radio beat](wiki/images/caseboard-noir.png)

Portable WiFi and Bluetooth Low Energy security-research firmware for M5Stack CoreS3 SE and Core2 v1.1.

HAMLET PANCETTA observes the 2.4 GHz band, collects authorized WiFi evidence, tracks defensive WiFi/BLE indicators, writes coordinate-backed field records, exports captures, and works with optional C5, GPS, and Meshtastic hardware. The [capability wiki](wiki/Home.md) explains what each subsystem does, what it transmits, and where its evidence stops being certain.

Its peer layer implements the openly published [RFC-F0690-v3: NOWFLOCK Peer Coordination Protocol](rfc/NOWFLOCK_STANDARD.md). The specification text is licensed under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/): anyone may read, copy, adapt, redistribute, and implement FNOW/3, including commercially. Copied or adapted specification text keeps attribution and marks changes; compatible implementations owe no protocol fee. That openness covers the RFC, not separately copyrighted firmware, and it still does not turn radio capability into authorization. The packet escaped. The warranty paperwork did not.

The repository ships a deliberately narrow release surface:

- essential firmware and build source;
- the open RFC-F0690-v3 specification and revision history;
- the capability-only wiki and its commissioned illustrations;
- one current merged CoreS3 SE production binary.

CoreS3 SE is the primary production target. Core2 v1.1 remains the compatibility target. Both use a 320×240 touch display, 16 MiB flash, 8 MiB PSRAM, and onboard 2.4 GHz WiFi/BLE. Neither board has native 5 GHz, GPS, or LoRa; those capabilities require explicitly attached hardware.

## Hardware and support

These are referral links. A qualifying purchase may send a commission back to the project. That money buys boards, cables, and fresh opportunities for the serial console to explain what we misunderstood. The cat remains upper management.

- [M5Stack CoreS3 SE](https://shop.m5stack.com/products/m5stack-cores3-se-iot-controller-w-o-battery-bottom?ref=xqezhcga) — the primary production target. No battery bottom. No native 5 GHz. No electrical fan fiction.
- [M5Stack Core2 v1.3](https://shop.m5stack.com/products/m5stack-core2-esp32-iot-development-kit-v1-3?ref=xqezhcga) — the current retail Core2. It keeps the ESP32/AXP192 architecture and replaces the MPU6886 with a BMI270; this repository's documented compatibility evidence remains Core2 v1.1.
- [Unit CardKB v1.1](https://shop.m5stack.com/products/cardkb-mini-keyboard-programmable-unit-v1-1-mega8a?ref=xqezhcga) — an optional 50-key I2C keyboard on Port A. On Core2 it goes dormant while the C5 bridge owns the shared pins. One bus. One owner. The smoke stays theoretical.

Already equipped? [Keep the night shift caffeinated](https://buymeacoffee.com/0ct0). Purchase or pass; the license stays where it is. The source tree has no shopping-cart dependency because it already has enough state to manage.

## Production image

The current [merged CoreS3 SE image](firmware/hamlet-pancetta-v0.1.0-cores3se.bin) and its source receipt are recorded on the [hardware and power page](wiki/Hardware-and-Power.md).

## Authorization

Use this firmware only on systems and radio environments you own or are explicitly authorized to test. Some modes transmit, create local access points, collect identifiers or coordinates, delete files, and send data to configured external services. Capabilities are not permission.

A fresh install starts with automatic Hunt, Hunt transmitters, and FLOCKNOW disarmed. Enabling a saved control records operator intent; it does not supply authorization. Upgrades retain explicit stored choices, so review `TUN3 P1G` before field use.

The city is the band. The operator still owns the warrant.
