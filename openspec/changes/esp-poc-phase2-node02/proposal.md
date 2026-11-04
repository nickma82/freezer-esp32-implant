## Why

Phase 1 (`esp-poc-phase1-listener`) proves the NodeMCU can receive and decode the bus. To replace the SmartDeviceBox, the
board must take over its role as **node `02`**: answer the freezer's slot `02` in time, with a valid message. This is the
critical timing step: the box answers 426 µs after the slot byte. It can only be tested with the box unplugged, while the
logic analyzer verifies every byte.

## What Changes

- An open-drain transmit stage. The UART TX on GPIO15 is inverted in hardware and drives an N-MOSFET to pad 2. The bus
  is only ever pulled low, and booting, flashing and power-off cannot disturb it.
- A timed responder for slot `02`. After BREAK plus slot byte `02` the board starts its body at a configurable delay
  (default 426 µs after the start of the slot byte), from a buffer prepared in advance. It answers no other slot and never
  sends a BREAK or a slot byte.
- Transmit safety: disabled at every boot, enabled only by a console command. It switches itself off on a collision
  (echo mismatch) or an error during its own transmission.
- Node-02 behaviour:
  - the idle reply;
  - the start-up handshake replayed in the captured order (hello reply, init acks, table request);
  - a read request whose values are taken from slot `82`.
- A minimal console CLI (`tx`, `delay`, `read`, `stats`, `v`) and transmit logging with the measured start latency.
- A bench procedure with the box removed and the logic analyzer attached. It also answers `04-open-questions.md` #14:
  does the freezer require the handshake?

## Capabilities

### New Capabilities
- `esp-node02-responder`: acting as bus node `02`:
  - electrical open-drain transmit;
  - timed answers in slot `02` only;
  - transmit enable and auto-disable safety;
  - idle reply, handshake responder, read request with reply correlation;
  - transmit logging.

### Modified Capabilities
None. It builds on `esp-bus-core` and `esp-bus-listener` from `esp-poc-phase1-listener`, whose requirements stay unchanged.

## Impact

- **Firmware** in `protocol-decoding/esp-smartdevice-emulator-poc/`:
  - TX path and timer-driven reply in `lib/bus_uart_esp8266/`;
  - a responder state machine and console CLI in `src/`;
  - new host tests for the responder logic.
- **Hardware:** an N-MOSFET (2N7002, BSS138 or similar), 100 Ω gate resistor, 100 kΩ gate pull-down, 100 Ω drain resistor. Optionally a 4.7-10 kΩ pull-up, if Phase 0 test 3 shows the freezer has none.
- **Prerequisite:** `potential-next-steps.md` Phase 0 tests 2 (box unplugged capture) and 3 (multimeter, pull-up) are done.
- **Docs:** the results go into `05-bus-roles.md`, `04-open-questions.md` (#11, #14) and `potential-next-steps.md` (Phase 2).
- **Risk to the appliance:** the board now drives the bus, so collisions or wrong timing could disturb the freezer's
  communication with node `01`/`0B`. This is mitigated by: transmit off by default, transmitting in slot `02` only,
  auto-disable on collision, and analyzer supervision.
