## Context

See `proposal.md` (Why). This design is also the **shared technical base** for `esp-poc-phase2-node02` and
`esp-poc-phase3-full-protocol`; they reference it instead of repeating it.

### Where the knowledge lives (read these first)
All paths are relative to `protocol-decoding/`.

| Topic | Document / code |
|---|---|
| Line layer, slot/message layer, schedule, records, message types | `01-protocol.md` §1-4 |
| Write commands byte for byte, door and alarm signalling | `02-commands.md` |
| Start-up handshake, group `3C` identity, group `48` network | `03-bootup.md` |
| Who sends what (evidence), answer latency per node, node `01`/`0B` sleep | `05-bus-roles.md` (§1, §2, §4-6) |
| Open points that matter for the ESP | `04-open-questions.md` #11 (answer window), #13, #14 (handshake needed?) |
| Original ESP plan, Phase 0 bench tests | `potential-next-steps.md` |
| Generic HW requirements | `../docs/hardware-requirements.md`. H4, H5 and H12-H15 still apply. **H2 and H6 are outdated**: the box sends no BREAK and is not master |
| Reference implementation to port | `src/crc.py`, `src/protocol.py` (`decode`, `encode`, `WIDTH`, `write_param`, `read_request`, `interpret`, `EVENT_FLAGS`, `identity`, `text`) |
| Capture tooling to reuse from host tools | `src/slots.py` (`bursts`, `ui_states`, `door_pushes`), `src/frames.py` |
| Golden frames | `tests/test_generator.py`, `tests/test_captures.py` |
| Captured traffic (vectors, comparison format) | `csv/*_hex.csv` (Logic 2 Async Serial data table, hex radix) |
| Logic 2 MCP quirks | Raw-data export crashes Logic. Use an Async Serial analyzer plus `export_data_table_csv` with radix 3, exported to a path inside the repo |

### Protocol reference card [ok unless marked]
- **Line:** pad 2, single wire, open-drain with pull-up, idle high at 3.2-3.3 V. 57600 8N1, LSB first. The freezer runs at ≈ 57092 bit/s, the box at ≈ 57126 bit/s.
- **Slot:** `BREAK (315 µs low) · ~90 µs high · <slot byte>` is sent by the **freezer (node `7F`)**, then the slot owner's body follows: `<dst> <src> <svc> <flag> [<len> <payload>] <crc lo> <crc hi>`.
- **CRC:** CRC-16/XMODEM over dst..payload, low byte first. It excludes the slot byte.
- **Slot byte:**
  - `< 0x80`: node n may send;
  - `0x80|n`: the freezer sends to node n;
  - `FF`: the freezer broadcasts (event block, about every 1 s);
  - `0C`, `7E`, `7F` are never answered.
- **Addressing:** `dst = 0x80|node`, `FF` = freezer, `80` = broadcast. Nodes:
  - `7F` freezer/scheduler;
  - `02` SmartDeviceBox;
  - `01`/`0B` a third MCU (display/door board [hyp]);
  - `0C` absent.
- **Answer latency** (start of slot byte to start of first body byte):
  - node `7F`: 250 µs;
  - node `02` (box): **426 µs**;
  - node `01`/`0B`: 604 µs awake, 648-689 µs while asleep.

  The freezer accepted 430-690 µs. After the start delay, body bytes follow back to back at 10 bit times.
- **Slot 02:** every 570 ms, sometimes 285 ms apart. Slot `82` (the freezer's reply to the box) comes 285 ms after a request. The longest box message is 95 bytes including the slot byte, 16.3 ms on the wire. The slot grid is 17.8 ms.
- **Box idle reply:** `FF 02 30 01 02 00 00 0B 3A`. Services, with reply = request + `0x10`:
  - `01`/`11` hello;
  - `02`/`12` table;
  - `03`/`13` read;
  - `06`/`16` version;
  - `21`/`22`/`23` init, acked with `30 01 02 <svc> 00`;
  - `24` write/push, no reply;
  - `30` ack or nothing to send.
- **Records** `<grp> <id> <flag> [value]`:
  - flag `20`/`21`: 1 byte;
  - flag `40`/`41`: 2 bytes, big endian;
  - flag `80`/`82`/`84`: 4 bytes;
  - flag `00`: not available.

## Goals / Non-Goals

**Goals:**
- A protocol core that is tested on the PC and reused unchanged by phases 2 and 3.
- Interrupt-driven, break-aware reception with µs timestamps, accurate enough to reproduce the latencies in `05-bus-roles.md`.
- Clean separation of the hardware-specific parts (UART driver, console, pins) from the protocol logic.
- A repeatable evaluation: board log versus logic-analyzer export.

**Non-Goals:**
- Any transmission on the bus. The TX path, timer-driven replies and the MOSFET stage belong to phase 2.
- Wi-Fi, MQTT, Home Assistant, OTA. Wi-Fi is switched off.
- ESP32 support. The core stays portable, but only the ESP8266 drivers are written.

## Decisions

### D1 Board and pins: Amica NodeMCU v2 (ESP8266EX, ESP-12E, CP2102), CPU at 160 MHz
| Function | Pin | Notes |
|---|---|---|
| Bus RX | **GPIO13 / D7** | Pad routed to UART0 RX via the IO swap (`IOSWAP` U0 bit plus pin function `FUNCTION_4`, as `uart_swap()` in the core's `uart.cpp` does). Connected to pad 2 through a **4.7 kΩ** series resistor |
| Bus TX | GPIO15 / D8 | **Not wired in phase 1.** Strap pin: it must be low at boot. Phase 2 drives a MOSFET from it, never the bus directly |
| Console out | **GPIO2 / D4**, hardware UART1 TX | Jumper wire D4 → the board's `TX` header pin (GPIO1). GPIO1 stays a high-Z input, so UART1 output reaches the CP2102 and USB |
| Console in (optional in phase 1) | GPIO3 / `RX` header | CP2102 TX. Read RX-only with EspSoftwareSerial, same baud as the console |
| GND | GND | To the appliance GND pad (blue cable) |
| Pads 1 and 3 | – | **Never connected** |

**Why GPIO13 on UART0:** the ESP8266 has only one UART with RX, and it is the only one with hardware break detection and a 128-byte FIFO. GPIO3 (the default RX) is driven by the CP2102 and would fight the bus.

**Why the console goes via UART1 plus a jumper:** a hardware FIFO costs no interrupt time. A bit-banged TX (SoftwareSerial) disables interrupts for about 87 µs per byte, which would ruin the µs timestamps here and the reply timing in phase 2.

**Fallback** if the jumper trick misbehaves: an external 3.3 V USB-UART on D4, log only.

**Alternatives rejected:**
- SoftwareSerial for the bus: no break detection, jittery.
- Bus on default UART0 pins: conflicts with the CP2102.

### D2 Register-level UART0 receive driver
- Use the register macros from `esp8266_peri.h`:
  - `USC0`/`USC1` for configuration;
  - `USIE`/`USIS`/`USIC` for interrupt enable, status and clear;
  - `USF` for the FIFO;
  - `USS` for status.
- Interrupts: RX-FIFO-full with threshold **1** (`UIFF`), break detect (`UIBD`), frame error (`UIFR`), overflow (`UIOF`).
- Attach with `ETS_UART_INTR_ATTACH`. Do not use `HardwareSerial`: it hides break events and frame errors.
- The ISR timestamps each byte with the CPU cycle counter (`ESP.getCycleCount()`, 6.25 ns at 160 MHz) and extends it to 64 bits; it wraps every 26.8 s.
- A byte's interrupt fires after its stop bit. So the latency from slot byte to body = t(body0) − t(slot), and it equals start-to-start.
- On `UIBD` the driver closes the current slot and opens a new one. A `0x00` with frame error that belongs to the BREAK is discarded, not stored. **Verify on the bench** whether the ESP8266 pushes that `0x00` into the FIFO.
- Fallback framing: if `UIBD` turns out unreliable, use a `0x00` with frame error after more than 1 ms of idle.
- Divisor: 80 MHz / 57600 ≈ 1389, giving 57595 bit/s. The ±0.9 % mismatch with the freezer is well within tolerance.
- Slots travel through a lock-free ring buffer (single producer in the ISR, single consumer in `loop()`). Each entry holds: slot byte, up to 160 bytes, per-byte timestamps, error flags.
- No heap use in the ISR. ISR code is in IRAM (`IRAM_ATTR`).

### D3 Determinism
- Wi-Fi off at boot (`WiFi.mode(WIFI_OFF)`, `WiFi.forceSleepBegin()`).
- No flash writes at runtime.
- `loop()` never blocks longer than 1 ms and calls `yield()`.
- Target ISR entry jitter is below 5 µs; the evaluation (spec: ±10 µs) proves it.

### D4 Protocol core (`lib/freezer_bus/`)
- Pure C++17 with no Arduino or SDK headers, fixed-size buffers and no heap.
- It mirrors `src/protocol.py`:
  - `crc16_xmodem`;
  - slot decode into slot, dst, src, svc, flag, len, payload, crc_ok, kind (empty/ack/list/values/unknown);
  - record parse and encode with the `WIDTH` table;
  - `interpret` for the known params and the event block, with `EVENT_FLAGS`;
  - builders: idle, write_param, read_request, hello reply, init ack, table request, version request;
  - a schedule/latency tracker: periods per slot byte, latency per source.
- Names may follow C++ style, but every function cites its Python counterpart in a one-line comment so reviewers can cross-check.

### D5 Tests and vectors
- PlatformIO `env:native` with Unity runs the core tests on the PC.
- `tools/make_vectors.py` imports `../src/slots.py` and `../src/protocol.py`. It writes `test/vectors.h` with deduplicated slots from `../csv/*_hex.csv` and the expected decode for each: kind, crc_ok, records count, interpreted values. It **skips groups `3C` and `48`** (identification and network data), so no serial numbers, SSID or IP are copied.
- Generated once and committed. The `vectors` recipe regenerates it.
- The golden frames from `tests/test_generator.py` are hand-copied test cases.

### D6 Log format (stable, parsed by `tools/compare.py`)
```
M <t_us> <slot hex> <lat_us|-> <crc 1|0|-> <body hex, space separated> | <decoded text>
S <t_us> <key>=<value> ...                              # statistics, every 10 s
E <t_us> <kind> <detail>                                # framing error, overrun, damaged slot, log drop
# free text                                              # banner, config
```
- The body hex excludes the slot byte; `<slot hex>` holds it.
- Defaults: empty slots and idle replies (`30 00`/`30 01` with payload `00 00`) are counted in `S` but not printed. The console command `v` toggles full output. Full output is needed for `compare.py`.
- Console at 115200 by default, the same baud for in and out, because the CP2102 uses one port. A log ring buffer counts dropped lines in `S`.

### D7 Evaluation tool (`tools/compare.py`)
- Reads the board log (`M` lines) and a Logic 2 hex export.
- For the export it reuses `bursts()` from `../src/slots.py`, so both sides use the same framing.
- Aligns the two sequences of (slot, body bytes) with `difflib.SequenceMatcher`. Clocks differ, so it does not align on time.
- Reports matched, missing, extra and mismatched messages, and the median latency per source on both sides.
- Exit code ≠ 0 if the spec thresholds (99.9 %, 0 extra, ±10 µs) fail.

### D8 Project layout and entrypoint
```
protocol-decoding/esp-smartdevice-emulator-poc/
  platformio.ini        env:nodemcuv2 (espressif8266, arduino, board_build.f_cpu=160000000L), env:native (unity)
  justfile              setup, build, flash, monitor, log, test, vectors, compare, clean
  README.md             wiring diagram, safety notes, phase overview, how to evaluate
  lib/freezer_bus/     protocol core (D4)
  lib/bus_uart_esp8266/ UART0 RX driver, cycle-counter timestamps (D2)
  src/main.cpp          listener: ring buffer -> core decode -> log/stats (D6)
  test/test_core/       Unity tests, vectors.h (D5)
  tools/                make_vectors.py, compare.py (D5, D7)
  logs/                 board logs from bench runs (git-ignored)
```
- `just` lists the recipes. Bash recipes use functions and `set -euo pipefail`, following `protocol-decoding/justfile`.
- `setup` checks for `pio` and prints the install hint `uv tool install platformio` or `pipx install platformio`.

### D9 Wiring (phase 1)
```
 appliance pad 2 (bus) ──┬── box (unchanged)
                         ├── logic analyzer CH0
                         └── 4.7 kΩ ── D7 (GPIO13)  NodeMCU
 appliance GND pad ──────┴── LA GND ── NodeMCU GND
 NodeMCU D4 (GPIO2) ── jumper ── NodeMCU TX pin (GPIO1)      USB -> laptop
 pads 1, 3: not connected                                 D8: not connected
```
**Safety:**
- Measure the voltage between appliance GND and laptop/USB GND before connecting. For the first contact, run the laptop on battery.
- Power the NodeMCU before plugging in D7. The series resistor limits back-feed below 1 mA if it is not.

### D10 Bench procedure (evaluation)
1. Flash, then attach as in D9 and start `just log`, with full output (`v`).
2. Start a Logic 2 capture of at least 60 s at the same moment. During the capture: open and close the door, and send one app command (SuperFrost on/off).
3. Export the Async Serial data table as hex into `logs/`, from the GUI or the Logic MCP. Then run `just compare logs/<board>.log logs/<la>.csv`.
4. Capture 10 s without the board attached and compare the latencies with `cd .. && just slots <csv>` (spec: bus unaffected).
5. Do a 1-hour run, checking `S` lines for overruns and resets.
6. Record the results in `05-bus-roles.md` (new section "ESP listener") and tick Phase 1 in `potential-next-steps.md`.

## Risks / Trade-offs

- **[ESP8266 break handling differs from expectation]** (extra `0x00` in the FIFO, `UIBD` per break or per byte) → step D10.1 checks this against the analyzer first. Fallback framing is in D2.
- **[Console jumper or SoftwareSerial RX disturbs timing]** → console RX is optional in phase 1. Its ISR is short and only active while typing. The evaluation measures the jitter.
- **[Log bandwidth at 115200 too low for full output]** → the drop counter makes this visible. If drops appear in full mode, raise both sides to 230400, or log only non-idle messages and let `compare.py` ignore idle ones.
- **[Ground loop or potential difference between laptop and appliance]** → measure first, laptop on battery (D9).
- **[Accidentally driving the bus]** → D8 stays unwired and GPIO13 is input-only; the spec scenario "bus unaffected" checks it.
- **[Cycle-counter wrap and 64-bit math in the ISR]** → unit-test the wrap extension on the host with synthetic values.
- **Trade-off:** register-level UART code is chip-specific and less portable. Accepted, because break detection and timing are the whole point. The core stays portable for a later ESP32 port.

## Migration Plan

New directory, nothing to migrate. Rollback: unplug the board. The appliance and box keep working, as the spec requires.

## Open Questions

- Does the freezer side have the pull-up? Not needed for listening; phase 2 needs it (`potential-next-steps.md` Phase 0 test 3).
- Exact value of the NodeMCU's on-board GPIO15 pull-down. Not needed in phase 1; it is relevant for the phase-2 MOSFET gate.
