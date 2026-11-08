# Dev boards

One folder per board we own or are considering, so the wiring, the quirks and the measurement results stay with the
board instead of being repeated in every change proposal.

| Board | Role | Status | Notes |
| ----- | ---- | ------ | ----- |
| [ESP32-C3 SuperMini](esp32-c3-supermini/README.md) | bench, 6 units in hand | documented, not measured | smallest and cheapest; weak on-board antenna, so not for the installed unit |
| Amica NodeMCU v2 (ESP8266EX / ESP-12E) | phases 0 and 1, passive listener | used | pins and console hack in `openspec/changes/esp-poc-phase1-listener/design.md` D1, wiring in `protocol-decoding/esp-*-emulator-poc/WIRING.md` |
| Seeed XIAO ESP32-C3 / C6 | candidate | not bought | same size class, but an IPEX connector for an external antenna |
| ESP32-C3-MINI-1U / C6-MINI-1U on a custom PCB | installed unit | planned | all parts rated -40 C, conformal-coated, external antenna |

Which board goes where is decided by `../hardware-requirements.md` (H1-H17): the protocol filters nothing, size, Wi-Fi
range in a steel cabinet and the -18 C environment decide the form factor.
