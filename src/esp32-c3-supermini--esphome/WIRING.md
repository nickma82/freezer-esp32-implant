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

## ESP32-WROOM-32

A bare ESP32-WROOM-32 module (classic ESP32, `freezer-implant-wroom.yaml`) in place of the SuperMini. The bus side is
the same as above: one GPIO through **100 Ω** to the bus, common GND, pads 1 and 3 of the freezer never connected, the
pull-up from the bus side. Status: **bus on GPIO16 running on a Liebherr `RBsdc 525i` (2026-11-22)**; module pin numbers are from the Espressif datasheet,
check them against your module (`../../docs/dev-boards/esp32-wroom-32/README.md`).

```
 Bus (freezer pad 2 / bench net)          ESP32-WROOM-32 module
 ───────────────────────────────          ─────────────────────
  pad 2 (BUS) ──┬──[ 100 Ω ]────────────── IO16 (module pin 27)   UART1 TX+RX, open-drain
                └── Logic analyzer CH0
  GND ──────────┬── Logic analyzer GND
                └──────────────────────── GND (pins 1, 15, 38)

 Freezer's onboard Wi-Fi (newer freezers only)
  its EN ──┬───────────────────────────── IO17 (module pin 28)   held low, open-drain
           └─[ pull-down ]── GND                                  keeps it off while IO17 floats (reset, flashing,
                                                                  safe mode); stronger than that EN's own pull-up

 3.3 V supply (>= 500 mA) ───┬──────────── 3V3 (pin 2), 10 µF + 100 nF close to the pin
                             ├─[ 10 kΩ ]── EN (pin 3) ──[ 1 µF ]── GND    power-on reset delay
                             └─[ 10 kΩ ]── IO0 (pin 25)                   normal boot; low at reset = download

 USB-UART adapter (3.3 V)
  TX ───────────────────────────────────── RXD0 / IO3 (pin 34)
  RX ───────────────────────────────────── TXD0 / IO1 (pin 35)    console and ROM log
  GND ──────────────────────────────────── GND
  (optional: DTR/RTS auto-reset circuit to EN and IO0, as on dev boards)
```

| Module pin | Connect to | Why |
|---|---|---|
| IO16 (27) | bus through **100 Ω** | the default `bus_pin`; any other GPIO the schema accepts works (`substitutions: bus_pin`) |
| IO17 (28) | EN of the freezer's onboard Wi-Fi, plus a pull-down to GND | only on freezers with Wi-Fi on their own board; otherwise leave it open. Keeps the freezer off the vendor cloud, so nobody can change its settings from there (e.g. holiday mode). The pull-down is the part that holds it off whenever the module does not (unpowered, reset, flashing, safe mode); it must win against that EN's own pull-up (e.g. 1 kΩ against 10 kΩ) |
| GND (1, 15, 38) | bus GND, analyzer GND, adapter GND | common reference |
| 3V3 (2) | 3.3 V, at least 500 mA, with decoupling | Wi-Fi TX peaks; supply from the freezer still open (H12) |
| EN (3) | 10 kΩ to 3V3, 1 µF to GND | the chip starts only after its supply is stable |
| IO0 (25) | 10 kΩ to 3V3, a button or jumper to GND for flashing | strap: low at reset = download mode |
| TXD0 / RXD0 (35 / 34) | USB-UART adapter | flashing and the log; never the bus |

Leave unconnected or only pulled as the module expects: IO2 (strap, must not be high when flashing), IO12 (strap: high at
reset selects 1.8 V flash and the module does not boot), IO5 and IO15 (straps), the flash pins SD0-SD3/CMD/CLK
(module pins 17-22, GPIO6-11). IO34-IO39 are input-only and cannot carry the bus.

Flashing: hold IO0 low, pulse EN low, release IO0, then `CONFIG=freezer-implant-wroom.yaml PORT=/dev/ttyUSB0 just flash`.
Updates after that go over Wi-Fi. The bus pin is high-Z through reset and download mode, as on the C3; the firmware
only ever sets it open-drain. Verify once on the analyzer that reset and flashing leave the bus alone (as bench task 7.7).

Bench with the NodeMCU freezer emulator: wire as in the SuperMini bench section, with the module's bus pin in place of
SuperMini pad `3`. The emulator's port is `/dev/ttyUSB0` by default, and the module's USB-UART adapter is likely
`/dev/ttyUSB*` as well: set `EMU_PORT` for the testbench and `PORT` for `just` explicitly.
