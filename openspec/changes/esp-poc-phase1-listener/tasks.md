## 1. Toolchain and project skeleton

- [x] 1.1 Install PlatformIO (`uv tool install platformio` or `pipx install platformio`). Check that `pio --version` works and that `pio pkg install -p espressif8266` succeeds
- [x] 1.2 Create `protocol-decoding/esp-smartdevice-emulator-poc/` with the layout of design D8: `platformio.ini` with `env:nodemcuv2` (160 MHz) and `env:native`, plus empty `lib/`, `src/`, `test/`, `tools/`, and `logs/` with a `.gitignore`
- [x] 1.3 Write `esp-smartdevice-emulator-poc/justfile` with recipes `setup`, `build`, `flash`, `monitor`, `log`, `test`, `vectors`, `compare`, `clean`. Bash recipes use functions and `set -euo pipefail`. `just` lists them
- [x] 1.4 Write `esp-smartdevice-emulator-poc/README.md`: purpose, phase overview (links to the three changes), the wiring diagram and safety notes from design D9, and the bench procedure from D10
- [x] 1.5 `just build` compiles an empty sketch for `nodemcuv2`, and `just test` runs an empty Unity test natively

## 2. Protocol core (`esp-bus-core`)

- [x] 2.1 Port `crc16_xmodem` from `src/crc.py`. Test: `FF 02 30 01 02 00 00` gives `0x3A0B`
- [x] 2.2 Port the record parser and encoder with the full `WIDTH` table (`00`, `20`, `21`, `40`, `41`, `80`, `82`, `84`) and the error cases (unknown flag, truncated record, length mismatch)
- [x] 2.3 Port slot decode and encode: empty slot, ack, list, values, unknown pass-through, CRC check, and dst/src node mapping (`FF`, `80`, `0x80|n`)
- [x] 2.4 Port `interpret`: mode, party, set-point, current temperature, door (`0C/31`, `14/04`), door alarm, and the event block with `EVENT_FLAGS` and the raw output for unknown flags
- [x] 2.5 Implement the node-02 builders: idle, `write_param`, `read_request`, hello reply, init ack, table request, version request (to `FF` and to `81`)
- [x] 2.6 Write `tools/make_vectors.py`. It reuses `../src/slots.py` and `../src/protocol.py`, skips groups `3C`/`48`, dedupes, and writes `test/test_core/vectors.h`. Wire it to `just vectors` and commit the result
- [x] 2.7 Write the Unity tests: CRC, every vector decodes as expected, round-trip encode is byte-identical, the golden frames of `tests/test_generator.py` and `02-commands.md`, and the interpret cases from the spec scenarios
- [x] 2.8 Write the schedule/latency tracker (periods per slot byte, latency per source, median plus spread over a window) with host tests on synthetic timestamps, including cycle-counter wrap extension

## 3. UART0 receive driver (`lib/bus_uart_esp8266/`)

- [x] 3.1 Configure UART0 by register: 57600 8N1, RX swapped to GPIO13 (IO swap plus `FUNCTION_4`), GPIO15 left as an input, RX-FIFO threshold 1, interrupts for RX-full, break detect, frame error and overflow
- [x] 3.2 Write the ISR in IRAM: timestamp every byte from the 64-bit-extended cycle counter, open a slot on break detect, set flags for frame errors and overruns, and push completed slots into a lock-free ring buffer
- [x] 3.3 Switch Wi-Fi off at boot and set up the console: UART1 TX on GPIO2 with GPIO1 forced to high-Z input, optional RX-only EspSoftwareSerial on GPIO3, and a log ring buffer with a drop counter
- [x] 3.4 Bench check against the logic analyzer: does a BREAK push a `0x00` into the FIFO, and does `UIBD` fire exactly once per BREAK? Document the finding in the driver header and adapt the framing (design D2 fallback if needed)

## 4. Listener firmware (`esp-bus-listener`)

- [x] 4.1 `main.cpp`: drain the ring buffer, decode with the core, and print `M` lines in the format of design D6, with idle and empty slots filtered by default and the `v` console command toggling full output
- [x] 4.2 Print `S` statistics lines every 10 s (slot counts, empty, CRC ok/bad, framing errors, overruns, log drops, slot-`02` periods, latency median and spread per source) and `E` lines for errors
- [x] 4.3 Write `tools/compare.py` (design D7): parse `M` lines, read the Logic export with `bursts()`, align with `difflib`, report matched/missing/extra/mismatch and the latency difference per source, and exit non-zero on failed thresholds. Wire it to `just compare <log> <csv>`
- [x] 4.4 Test `compare.py` on the host: feed it a synthetic board log generated from `../csv/door_short_open_hex.csv` (one message dropped, one byte changed) and check that it reports exactly those differences

## 5. Bench evaluation (box and logic analyzer stay connected)

- [x] 5.1 Measure the voltage between appliance GND and USB GND, then wire as in design D9 (4.7 kΩ to D7, D8 open, pads 1/3 open, D4 to TX jumper)
- [x] 5.2 Do a simultaneous 60 s run (board log plus Logic capture, door open/close, one SuperFrost toggle from the app). Run `just compare`; it must pass the spec thresholds
- [x] 5.3 Capture 10 s with and without the board attached, then run `cd .. && just slots <csv>` on both and confirm that BREAK length, schedule and box latency (426 µs) are unchanged
- [x] 5.4 Run for 1 hour; check the `S` lines for overruns, drops and resets
- [x] 5.5 Write the results into `protocol-decoding/05-bus-roles.md` (new section "ESP listener": latencies measured by the board, break handling finding) and tick Phase 1 in `protocol-decoding/potential-next-steps.md`

## Results (2026-10-31)
- 3.4: every BREAK pushes one `0x00` into the RX FIFO and fires `UIBD` once (421 of 421). Documented in `bus_uart.h`.
- Pitfall: Arduino `pinMode(13, FUNCTION_4)` enables the internal pull-up, which together with the 4.7 kΩ resistor held D7 flat. The driver clears `GPFPU`/`GPFPD` (`WIRING.md`, `05-bus-roles.md` §9).
- 5.2: 300 s run with door and SuperFrost: 12635 of 12636 identical (99.992 %), 0 missing, 0 extra, latency difference at most 0.5 µs. The one difference is a lone trailing `FF` that Logic sees and the UART does not (probe glitch).
- 5.3: break to slot byte 405.3 µs and node-02 latency 426 µs, with and without the board. A board reset and re-flash caused no disturbance.
- 5.4: 1 h run (board log `logs/board_1h.log`, 360 `S` lines, 3650 s uptime): live bus traffic for the first ~2683 s (44.7 min, 39600 messages with a valid CRC, 0 CRC/decode/framing errors, 0 overruns, 0 ring/log drops, 0 `E` lines, slot-02 period 285/570 ms only, node-02 latency 425 µs). The bus was then disconnected by hand (a Logic capture confirmed a dead line): the board kept running through the 16 min of silence without a reset or stall. Accepted as passed on the 44.7 min of live traffic.
