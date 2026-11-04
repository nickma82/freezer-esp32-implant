## Why

The single-wire bus between the freezer and the SmartDeviceBox is decoded on the PC: `protocol-decoding/src/protocol.py`
handles every captured frame, and 68 tests pass. Nothing runs on a microcontroller yet. Before the box can be replaced, we
need proof that a cheap board on hand (Amica NodeMCU, ESP8266) can receive and decode the live bus reliably and with the
right timing, without disturbing the original box. This is the first PoC scope. Phases 2 (`esp-poc-phase2-node02`) and 3
(`esp-poc-phase3-full-protocol`) build on it.

## What Changes

- New PlatformIO firmware project in `protocol-decoding/esp-smartdevice-emulator-poc/`, targeting the Amica NodeMCU v2 (ESP8266EX, ESP-12E).
  This change targets the ESP8266 only.
- A chip-independent C++ protocol core that ports `src/crc.py` and `src/protocol.py`:
  - CRC-16/XMODEM;
  - frame decode and encode;
  - parameter records;
  - semantics of the known parameters and the event block;
  - frame builders for writes and reads.
  It is verified by host-side unit tests against golden vectors generated from `csv/*_hex.csv`.
- A passive bus listener. The board is connected in parallel to the SmartDeviceBox and the logic analyzer, receives only,
  never drives the line, and logs every message with timing and decoded meaning.
- Tooling to compare the board's log against a Logic 2 export of the same moment. This is the evaluation of Phase 1.
- A `justfile` entrypoint for the firmware project, plus a README with wiring and safety notes.

Not changed:
- the bus traffic itself (nothing is transmitted);
- the Python decoding tools;
- the existing tests.

## Capabilities

### New Capabilities
- `esp-bus-core`: the chip-independent protocol core:
  - CRC;
  - framing of a slot (slot byte plus body);
  - record encoding and decoding;
  - parameter semantics;
  - builders for the node-02 messages;
  - host tests against the captured traffic.
- `esp-bus-listener`: passive reception on the NodeMCU:
  - break-delimited framing with per-byte timestamps;
  - a machine-readable message log;
  - periodic statistics;
  - the evaluation procedure against the logic analyzer.

### Modified Capabilities
None. `openspec/specs/` is empty.

## Impact

- **New directory** `protocol-decoding/esp-smartdevice-emulator-poc/` with firmware, host tests, tools, `justfile` and README.
- **New developer dependency:** PlatformIO (espressif8266 platform, Arduino framework, Unity for native tests). It is not installed yet.
- **Hardware:** one Amica NodeMCU, a 4.7 kΩ series resistor, a GND wire to the freezer's GND pad, and the existing Logic 2 setup.
- **Docs:** after the bench run, the measured results go into `protocol-decoding/05-bus-roles.md` and `potential-next-steps.md` (Phase 1).
- **Risk to the appliance:** minimal. The data pin is input-only behind a series resistor, and pads 1 and 3 are never connected.
