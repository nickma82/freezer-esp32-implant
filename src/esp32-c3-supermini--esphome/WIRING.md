# Wiring: ESP32-C3 SuperMini as node 02 (implant)

ESP32-C3 SuperMini plays the SmartDeviceBox (node `02`) on the bus. The freezer (node `7F`) sends every BREAK and slot byte;
we only answer slot `02`. Board facts and pad table: `../../docs/dev-boards/esp32-c3-supermini/README.md`.
Status: **bench wiring not yet tested; freezer wiring is a follow-up change, not tested.**

Pads are counted from the USB end, bottom side (silkscreen) facing you: left column `L1..L8` = `5V, G, 3.3, 4, 3, 2, 1, 0`,
right column `R1..R8` = `5, 6, 7, 8, 9, 10, 20, 21`.

## Implant

```
 Bus (freezer pad 2 / bench net)           ESP32-C3 SuperMini
 ───────────────────────────────           ──────────────────
                                                ┌──[ USB-C ]──┐  <-- to laptop (power + console)
  pad 2 (BUS) ──┬──[ 100 Ω ]──────────────────● pad "3" (L5, GPIO3)
                │                               │             │   UART1 TX+RX on one pin, open-drain
                └── Logic analyzer CH0          │             │
  GND ──────────┬── Logic analyzer GND          │             │
                └───────────────────────────────● pad "G" (L2)
  pad 1, pad 3 ── NEVER connected               │             │
  pull-up to 3V3: provided by the bus side      └─────────────┘  (the SuperMini adds none)
```

| SuperMini pad | Connect to | Why |
|---|---|---|
| `3` (L5, GPIO3) | bus through **100 Ω** | UART1 TX and RX on one open-drain pin; hears its own TX |
| `G` (L2) | bus GND, analyzer GND | common reference |
| `3.3`, `5V` | nothing | USB powers the board on the bench |

Pins never to use:

| Pad | GPIO | Why |
|---|---|---|
| `8` (R4) | IO8 | strap pin, drives the blue LED |
| `9` (R5) | IO9 | boot-mode strap and the `BOOT` button |
| - | IO18/19 | USB D-/D+, not on a pad |
| `20`, `21` (R7, R8) | IO20/21 | reserved: UART0 / ROM boot log |

## Bench: SuperMini <-> NodeMCU freezer emulator

Emulator side exactly as in `../../protocol-decoding/esp-freezer-emulator-poc/WIRING.md`. Both boards USB-powered from one host.

```
                    3V3 (NodeMCU) ──[ 4.7 kΩ ]──┐   bus pull-up, from the emulator side
                                                │
  bus ──┬─────────────────────────────────────┤
        ├──[  47 Ω ]── NodeMCU D4 (GPIO2)   bus TX, open-drain
        ├──[ 4.7 kΩ ]─ NodeMCU D7 (GPIO13)  bus RX
        ├──[ 100 Ω ]── SuperMini pad "3" (GPIO3)
        └── Logic analyzer CH0
  GND ──┬── NodeMCU GND ── SuperMini pad "G" ── Logic analyzer GND
        └── (common through the shared USB host as well)
```

| Item | Detail |
|---|---|
| Pull-up | only the emulator's 4.7 kΩ to 3V3. The SuperMini adds none; its internal pull-up stays off |
| Analyzer | CH0 on the bus net, GND on the common ground |
| Power | each board on its own USB cable, same host |

## Freezer (follow-up change, not tested)

| Item | Detail |
|---|---|
| Data | freezer pad 2 through 100 Ω to SuperMini pad `3` |
| Ground | freezer GND pad to SuperMini `G` |
| Pads 1 and 3 | **NEVER connected** (H13) |
| Pull-up | 4.7-10 kΩ to 3V3 **only if** the H14 measurement shows the freezer has none; keep a footprint |
| Supply | open (H12): what the appliance provides is unknown. Stay USB-powered until then. The `73L33` regulator's current rating is unknown |

## Safety
- Measure the voltage between bus GND and laptop/USB GND before connecting; run the laptop on battery for first contact.
- Power the SuperMini over USB before connecting the bus.
- GPIO3 is high-Z through reset, boot and flash, and the firmware only ever sets it open-drain, never push-pull.
  Reset/flash while attached is safe by design, but verify once on the analyzer (bench task 7.7).
- The `BOOT` button is on IO9, not on the bus pin, so pressing it does not touch the bus; it only matters at reset (download mode).
