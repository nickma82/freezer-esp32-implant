## Why

The protocol is decoded far enough to replace the SmartDeviceBox, and the reverse direction (freezer emulator against the
real box) already works on the bench. The ESP8266 node-02 phases (`esp-poc-phase2-node02`, `esp-poc-phase3-full-protocol`)
were specified but never built. The actual goal is an ESP32 that sits where the box sits and connects the freezer to Home
Assistant locally. We have ESP32-C3 SuperMini boards in hand. ESPHome gives us Wi-Fi, OTA and the HA API for free.
The board doc that the hardware choice rests on was written from web sources, and our photos and the vendor schematic
contradict parts of it.

## What Changes

- **Correct `docs/dev-boards/esp32-c3-supermini/README.md`** against the photos and schematic:
  - it is a bare ESP32-C3FH4 chip (in-package flash), not a module;
  - it has native USB-Serial/JTAG and no bridge chip;
  - the regulator is marked `73L33` and there is a VBUS Schottky diode;
  - the on-board pull-ups, the active-low LED on GPIO8 and the real pad order from the silkscreen;
  - IO4–IO7 are JTAG pins, not boot pins;
  - sending a BREAK is not needed for node `02`;
  - one listing image shows an RP2040-Zero.
  
  Also fix the stale "the box is master and polls" statements in `docs/hardware-requirements.md` (H2, H6, H17).
- **New ESPHome firmware** in `src/esp32-c3-supermini--esphome/`. It is an external component `freezer_bus` that acts
  as bus node `02` (the box's role, as specified for `esp-smartdevice-emulator-poc`), built from:
  - the existing chip-independent `lb` protocol core;
  - a single-pin open-drain UART driver on GPIO3;
  - a host-tested node-02 engine. It handles the handshake, start-up reads, group `48` network writes with real values,
    the periodic status read, re-reads on event flags, writes, and restarting start-up when the freezer's hello reappears.
- **Home Assistant entities**:
  - SuperFrost and party-mode switches;
  - a set-point number (fixed −28…−15 °C);
  - current temperature;
  - door, door alarm and power-failure alarm (with its temperature);
  - versions and model;
  - a TX-enable switch (on at boot, auto-off on collision);
  - bus diagnostics.
- **`WIRING.md`** for the SuperMini: GPIO3 through 100 Ω to bus pad 2, GND common, pads 1/3 never connected. Bench
  wiring against the ESP8266 freezer emulator.
- **Bench procedure** against the freezer emulator (`protocol-decoding/esp-freezer-emulator-poc`) before any contact with
  the real freezer.

## Capabilities

### New Capabilities
- `c3-bus-driver`: the electrical and timing layer on the ESP32-C3:
  - one open-drain pin for TX and RX;
  - BREAK-delimited framing;
  - answers in slot `02` only, inside the observed reply window;
  - echo check and TX enable/auto-disable;
  - safe pin state during boot and flash.
- `node02-engine`: protocol behaviour as node `02`:
  - idle ack and handshake;
  - start-up reads, versions, identity and the `48` writes;
  - the status model kept current by periodic reads, event flags and passive door pushes;
  - verified write frames with confirmation and no automatic resend;
  - the set-point range;
  - recovery when the freezer restarts.
- `ha-freezer-entities`: what Home Assistant sees and can control through ESPHome, availability while the bus is down,
  and the diagnostics.

### Modified Capabilities
None. There are no archived specs under `openspec/specs/`. The open ESP8266 changes stay as they are. This change
reuses their requirements on a new target instead of modifying them.

## Impact

- **New code**: `src/esp32-c3-supermini--esphome/`, containing:
  - the ESPHome YAML;
  - `components/freezer_bus/`;
  - host tests;
  - a `justfile`;
  - `README.md` and `WIRING.md`.
- **Reused code**:
  - `protocol-decoding/esp-smartdevice-emulator-poc/lib/freezer_bus/` (protocol core, unchanged);
  - `protocol-decoding/esp-freezer-emulator-poc/lib/freezer_master/` (simulated freezer for the host tests).
- **Docs**:
  - the board README;
  - `docs/hardware-requirements.md` (user has uncommitted edits there; merge, don't overwrite);
  - `protocol-decoding/01-protocol.md` §4 and `potential-next-steps.md` after the bench run.
- **Tooling**: ESPHome (pinned, run via `uvx`), ESP-IDF framework, board `esp32-c3-devkitm-1`.
- **Hardware**:
  - a SuperMini, a 100 Ω resistor and the existing NodeMCU freezer emulator with its 4.7 kΩ pull-up;
  - a logic analyzer for the timing check.
- **Out of scope**:
  - installation in the freezer (supply on pads 1/3, cavity size, antenna, cold: H8–H12);
  - BottleTimer;
  - the `4C` writes;
  - a custom PCB;
  - the real freezer, which is a follow-up after the emulator bench passes.
- **Risk**:
  - the freezer's reply-time tolerance is only known from observation (250–689 µs);
  - set-points outside −18…−16 °C were never seen on the bus;
  - the board's regulator current is unknown.
