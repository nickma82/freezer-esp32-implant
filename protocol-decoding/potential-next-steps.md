# Potential next steps: ESP32 implementation

## Where we stand

The captures decode cleanly with the existing parser (0 errors, CRC matches on all 554 complete frames). Since
2026-10-30 the bus roles are understood as well, see `05-bus-roles.md`:

- **Line layer:** single-wire, half-duplex UART, 57600 baud nominal, 8N1, LSB first, on pad 2 at 3.3 V. The line is probably open-drain with a pull-up.
- **Slot layer:** the **freezer** (node `7F`) sends every BREAK (~316 µs) and one slot byte, on a ~17.8 ms grid. The owner
  of the slot answers after a short pause, or the slot stays empty. Proven by the clock fingerprint (`just clocks`).
- **Message layer:** `<dst> <src> <svc> <flag> <len> <payload> <crc_lo> <crc_hi>`, CRC-16/XMODEM, `dst = 0x80 | node`.
- **Roles:** the SmartDeviceBox is **node `02`**, a slave. It gets one send slot every 570 ms and answers ~426 µs after the
  slot byte. A third device (nodes `01` + `0B`, probably the display board) also answers. The freezer replies to the box in slot `82`.

What kind of bus this is: LIN-like scheduling (master sends break + header, the slot owner sends the response), with an
addressed request/reply layer and an object-dictionary parameter model on top. Not LIN (no `0x55` sync, no ID parity,
CRC-16, 57.6 kbit/s), not Dallas 1-Wire, not "UART 1-Wire". Proprietary, but a stock UART with break detection can speak it.

**Consequence for the ESP32:** it replaces node `02`. It must **never send a break or a slot byte**. It listens for
`BREAK` + `02` and answers within a fraction of a millisecond. That is simpler than being the master, but the answer
window is the new hard timing requirement.

## Phase 0: cheap tests (settle the remaining doubts before building)

Ordered by cost. Tests 1-3 need no new hardware.

1. **Raw export of the other captures (no hardware).** In the Logic 2 GUI (not the MCP, it crashes), export raw data
   (CSV) into `raw-captures/` and run `just clocks raw-captures/<file>.csv`.
   - **Done for the bootup** (`freezer-bootup-fullduration.csv`, 2026-10-30): slot bytes and node `7F` share one bit
     period, the box has another. Unexpected bonus: node `01`/`0B` switches between an awake clock (same rate as `7F`)
     and a jittery low-power clock depending on the door (`05-bus-roles.md` §2).
   - The door dependence is already confirmed on all 16 decoded exports through the answer pause (`just slots`), so
     more raw exports are optional; they would only repeat the box/master fingerprint.
   - **Done (2026-10-31):** `door-open--2sek--door-close--NOP-30sek.sal` gives a timeout of 5-6 s after the door
     closes and no wake-up during 26 s without input (`05-bus-roles.md` §2).
2. **Box unplugged, freezer running.** Remove the box, keep the logic analyzer on the freezer-side contacts (pad 2 + GND),
   capture 10 s.
   - Expected if the analysis is right: BREAK + slot bytes continue on the same schedule, nodes `01`/`0B` still answer,
     slot `02` stays empty (bare `02`).
   - If the bus goes silent: the box is the scheduler or powers the bus; `05-bus-roles.md` must be revisited.
   - If slots continue but `01`/`0B` are silent too: node `01`/`0B` lives in the box (a second MCU), not in the freezer.
3. **Static measurements with the box unplugged (multimeter).** Pads 1, 2, 3 against GND:
   - pad 2 idle voltage, and resistance pad 2 to pad 3 (unpowered) -> is the pull-up in the freezer? (H14)
   - pad 3 voltage, and pad 1 voltage. A 2021 Hackaday comment reported 12 V on one pad of a similar box; our pad 1 reads
     0 V on the logic analyzer. Check before connecting anything (H12, H13).
4. **Box hot-plug capture.** Freezer running, start the capture, then plug the box in. Shows whether the freezer greets a
   newly appearing node `02` (hello `01`, init `21`-`23`, table `02`/`12`) or whether the box can just start answering.
   Answers `04-open-questions.md` #14: does the ESP32 need to replay the handshake?
5. **Driver identification per byte (needs an oscilloscope or an analog-capable analyzer).** Put ~100 Ω in series in the
   data line between box and freezer, probe both sides. The side that pulls low shows the lower low level. Confirms
   per byte who sends the BREAK and slot byte, independently of the clock argument. Also gives the rise time and the
   pull-up value (open-drain question).

## Phase 1: ESP32 as passive listener (zero risk) **[done 2026-10-31, ESP8266 NodeMCU, results in `05-bus-roles.md` §9]**

- **Hardware:** Seeed XIAO ESP32-C3 or C6 (see `docs/hardware-requirements.md`). RX only on pad 2, through a 1-10 kΩ
  series resistor. Nothing drives the line.
- **Firmware:** UART 57600 8N1 with break detection (ESP-IDF `UART_BREAK` event), then: slot byte, pause, body, CRC check.
  Port `src/protocol.py` and `src/crc.py`. Log slots, nodes and decoded parameters.
- **Check:** the ESP32 log matches the Logic capture of the same moment; per-slot timing matches `just slots`.

## Phase 2: ESP32 as node 02 (box removed)

- **Hardware:** open-drain TX on the same pin as RX, 100-330 Ω series resistor, 3.3 V. Do not drive pads 1 and 3.
- **Firmware, step by step:**
  1. On `BREAK` + `02`, answer `FF 02 30 01 02 00 00 0B 3A` (nothing to send) ~426 µs after the slot byte started.
     Check with the analyzer that the freezer keeps scheduling normally.
  2. Receive slot `82` (the freezer's replies to node `02`) and `FF` (event block); never transmit in them.
  3. Send a read (`03` list), expect the values in slot `82` one 285 ms step later.
  4. If Phase 0 test 4 showed a handshake: answer hello with `03 01 00 00 00 00`, ack `21`-`23`, send the `13 00 00 00` table.
  5. Write the set-point (`0C/06`), then SuperFrost, and watch the event block and `0C/00`.
- **Check:** captures of the ESP32 run decode with 0 errors and look like the original box's slot-`02` traffic.

## Phase 3: only if Phase 2 fails

Build a lab setup to find the difference:

- ESP32 #1 emulates the freezer (scheduler: BREAK + slot bytes on the recorded schedule, canned replies).
- ESP32 #2 runs the node-`02` firmware.
- A logic analyzer verifies both; compare timing with the original captures (`just slots`, `just clocks`).

## Repo hygiene

- Add a firmware directory (`tools/` is empty today).
- Add a justfile recipe for build and flash, following `protocol-decoding/justfile`.
- `docs/hardware-requirements.md` still describes the box as poll master (H2, H6); update it to the node-`02` role.

## Acceptance criteria

- `just test` passes in `protocol-decoding/`.
- Phase 0 tests 2 and 3 are done and recorded in `05-bus-roles.md`.
- The ESP32's slot-`02` answers decode with 0 errors and start inside the freezer's answer window.
- The freezer answers an ESP32 read in slot `82` and applies an ESP32 write.
