# HAMLET PANCETTA

![Pancetta reviews the radio beat](wiki/images/caseboard-noir.png)

Portable WiFi and Bluetooth Low Energy security-research firmware for M5Stack CoreS3 SE and Core2 v1.1.

HAMLET PANCETTA observes the 2.4 GHz band, collects authorized WiFi evidence, tracks defensive WiFi/BLE indicators, writes coordinate-backed field records, exports captures, and works with optional C5, GPS, and Meshtastic hardware. The [capability wiki](wiki/Home.md) explains what each subsystem does, what it transmits, and where its evidence stops being certain.

The repository ships a deliberately narrow release surface:

- essential firmware and build source;
- the capability-only wiki and its commissioned illustrations;
- one current merged CoreS3 SE production binary.

CoreS3 SE is the primary production target. Core2 v1.1 remains the compatibility target. Both use a 320×240 touch display, 16 MiB flash, 8 MiB PSRAM, and onboard 2.4 GHz WiFi/BLE. Neither board has native 5 GHz, GPS, or LoRa; those capabilities require explicitly attached hardware.

## Production image

The current [merged CoreS3 SE image](firmware/hamlet-pancetta-v0.1.0-cores3se.bin) and its source receipt are recorded on the [hardware and power page](wiki/Hardware-and-Power.md).

## Authorization

Use this firmware only on systems and radio environments you own or are explicitly authorized to test. Some modes transmit, create local access points, collect identifiers or coordinates, delete files, and send data to configured external services. Capabilities are not permission.

A fresh install starts with automatic Hunt, Hunt transmitters, and FLOCKNOW disarmed. Enabling a saved control records operator intent; it does not supply authorization. Upgrades retain explicit stored choices, so review `TUN3 P1G` before field use.

The city is the band. The operator still owns the warrant.
