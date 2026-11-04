## Context

See `proposal.md` (Why). Shared base: `openspec/changes/esp-poc-phase1-listener/design.md` covers:
- the reference documents;
- the protocol reference card;
- the board and pins (D1);
- the RX driver (D2);
- determinism (D3);
- the core (D4);
- the log format (D6);
- the compare tool (D7);
- the layout (D8).

Once phase 1 is archived, the same content lives in `openspec/specs/esp-bus-core` and `esp-bus-listener` plus the
firmware README. This design only adds the transmit side.

Facts this design relies on (all paths relative to `protocol-decoding/`):
- **Answer latency:** the box answers **426 µs** after the start of slot byte `02`, which is about 252 µs after its stop bit (`05-bus-roles.md` §1). The freezer also accepted 604-689 µs from node `01`/`0B` (§2).
- **Timing:** slot `02` comes every 570 ms, sometimes 285 ms apart. Replies come in slot `82` 285 ms after a request. The slot grid is 17.8 ms, and the longest box body is 94 bytes (16.3 ms).
- **Handshake** as captured (`03-bootup.md`, "New message types"; bootup capture 3.2-8.1 s): hello at 3.26 s, first answer by the box at 5.83 s, then `21` → ack, table request, `22` → ack, `23` → no ack, then the identity read.
- **Open questions:** where the pull-up sits (`../docs/hardware-requirements.md` H14, `potential-next-steps.md` Phase 0 test 3), and whether the handshake is needed (`04-open-questions.md` #14).

## Goals / Non-Goals

**Goals:**
- Reproduce the box's slot-`02` answers bit- and timing-exact, under analyzer supervision.
- Make it impossible, by hardware and firmware design, to disturb the bus outside slot `02` or during boot.
- Settle `04-open-questions.md` #11 (answer window, for slot `02`) and #14 (handshake).

**Non-Goals:**
- Writes (SuperFrost, party, set-point), the state model and periodic status reads. These are phase 3 (`esp-poc-phase3-full-protocol`). The phase-2 queue and builders already support them, but no CLI exposes them.
- Group `48`/`4C` network records, version and identity reads (phase 3), Wi-Fi.

## Decisions

### D1 Transmit stage: inverted UART TX on GPIO15 driving an N-MOSFET
```
 GPIO15/D8 ──100 Ω──┬── gate   N-MOSFET (2N7002 / BSS138, Vgs(th) < 2 V)
                    100 kΩ         drain ──100 Ω── appliance pad 2 (bus)
                    │              source ── GND
                   GND
 GPIO13/D7 ──4.7 kΩ── pad 2 (RX, unchanged from phase 1, also reads back our own TX)
 optional: 4.7-10 kΩ pad 2 -> NodeMCU 3V3, ONLY if Phase 0 test 3 shows no pull-up in the freezer
```
- **Inversion:** set the UART0 TX-invert bit (`UCTXI` in `USC0`, see `esp8266_peri.h`). TX idle (mark) becomes GPIO15 low, so the MOSFET is off and the bus released. A 0-bit becomes GPIO15 high, the MOSFET on, and the bus low.
- **Boot safety:** GPIO15 is a strap pin that must be low at boot. The 100 kΩ gate pull-down and the board's own pull-down keep it low. The MOSFET stays off while unpowered, resetting or flashing.
- **Why not drive the bus from the GPIO** (open-drain pad mode): the bus pull-up would hold GPIO15 high at reset and stop the ESP8266 from booting. The pad's open-drain mode under the UART function is also undocumented.
- **Alternative rejected:** an NPN transistor. It works, but the MOSFET needs no base current and is the common open-drain stage.
- **Clear the internal pull-up on GPIO15 (phase 1 finding, `esp-smartdevice-emulator-poc/WIRING.md`):** Arduino's `pinMode(pin, FUNCTION_x)` switches the pad's internal pull-up on (mux bit `GPFPU`). On D7 this held the bus node flat in phase 1. On GPIO15 it would pull the MOSFET gate high through the 100 Ω and turn the stage on, driving the bus low. After every mux change on GPIO15 (including routing U0TXD to it), clear it with `GPF(15) &= ~((1 << GPFPU) | (1 << GPFPD));` and verify with the analyzer that the bus stays idle with `tx off`. Do the same for the inversion setup order: GPIO15 must never float or be pulled high between mux setup and the clear.
- Route the TX function to GPIO15 only after the inversion bit is set. Enable it only together with `tx on`. When disabled, GPIO15 is a GPIO output held low.

### D2 Timed reply without busy-waiting
- The RX ISR (phase-1 D2) sees the slot byte `02` right after a BREAK. Its timestamp `t_slot` is the end of the stop bit.
- It arms **timer1** (`timer1_attachInterrupt`, `timer1_write`, `TIM_DIV1`, single shot) for `delay_us − 10 bit times − ISR overhead`. The default targets 426 µs after the slot-byte start.
- The timer1 ISR (IRAM) copies the prepared body into the TX FIFO (128 bytes). Every body, at most 94 bytes, fits completely, so no refill interrupt is needed.
- The body is prepared beforehand: `loop()` keeps a "next answer" buffer (idle, or the queue head), with CRC already computed. The ISR takes it with an atomic pointer swap. If nothing is prepared, it sends the static idle message.
- **Calibration:** `delay <us>` on the console. First run, compare the analyzer latency (`cd .. && just slots`) with the board's own measurement, then fix a constant offset in firmware.
- **Guards before arming:**
  - transmit enabled;
  - the slot byte had no frame error;
  - the BREAK was seen and was at least 250 µs long;
  - the gap BREAK end → slot byte is at most 300 µs;
  - no previous answer still in progress.

### D3 Read-back and auto-disable
- Our own bytes come back on GPIO13 as the body of slot `02`. While transmitting, the RX ISR compares each received byte with the sent one.
- On a mismatch or a frame error it: resets the TX FIFO (stops the rest), forces GPIO15 low (MOSFET off), clears `tx_enabled`, and queues an `E <t> collision slot=02 idx=<n> sent=<xx> got=<yy>` line.
- The first read-back byte's timestamp gives the board-measured latency for the `T` log line.

### D4 Node-02 responder state machine (pure C++ in the core, host-tested)
- Input: decoded slots. Output: the queue of bodies to send.
- Messages to node `02` (slot `82`, src `7F`) drive the handshake table from the spec. The FIFO queue reproduces the captured order naturally:
  1. `21` queues ack `21` and the table request;
  2. `22` arrives meanwhile and queues ack `22` behind them.
- **One message per slot `02`.** Queue depth is 8, and on overflow the oldest non-handshake request is dropped and logged.
- **Read correlation:** a read sets `pending_read`. The next service-`13` message in slot `82` with src `7F` is the reply. Timeout 1 s, which is 3-4 slot steps.
- Host tests replay the bootup capture's slot-`82` messages (`csv/freezer_bootup_hex.csv` 3.0-8.2 s) through the state machine. The resulting answer sequence must equal the box's captured slot-`02` bodies in that window.

### D5 Console CLI (RX via EspSoftwareSerial on GPIO3, as phase-1 D1)
| Command | Effect |
|---|---|
| `tx on` / `tx off` | enable / disable transmit (never persisted) |
| `delay <us>` | answer delay, 300-690 µs, default 426 |
| `read` | queue the status read (`0C/00`, `0C/06`, `0C/14`, `0C/31`, `0C/09`, `08/02`) |
| `hs` | print handshake state and the last table reply |
| `stats` | print an `S` line now |
| `v` | toggle full output (phase 1) |

- New log line: `T <t_us> <lat_us> <body hex>` for each own transmission.
- `S` gains `tx=<on|off> answers=<n> lat_med=<us> lat_sd=<us> autodisable=<n>`.
- `compare.py` treats `T` lines as messages of node `02`.

### D6 Bench procedure (box unplugged, logic analyzer attached)
1. **Prerequisite:** Phase 0 tests 2 and 3 from `potential-next-steps.md`.
   - Test 2: capture with the box unplugged. Slot `02` must stay bare, and the schedule must continue.
   - Test 3: pull-up check.
   - If the bus has no pull-up without the box, fit the optional pull-up first.
2. **TX off, board attached:** the capture must equal step 1 (the listener does not disturb).
3. **`tx on`, idle only, 2 minutes captured:**
   - `just slots` gives the src-`02` latency 426 ± 10 µs;
   - 0 CRC errors;
   - the schedule is unchanged;
   - the freezer display shows no error.
4. **`delay` sweep** (optional, answers #11 for slot `02`): 350, 500, 650 µs. Record which delays the freezer still accepts. Accepted means the slot `82` replies keep coming for reads.
5. **`read`:** the reply arrives in slot `82` 285 ms later, and the decoded values match the freezer display.
6. **Freezer power cycle with board attached and `tx on` immediately after boot:** the handshake order matches the spec. Capture all of it.
7. **Hot-plug:** start the freezer without the board, attach it after 30 s, `tx on`. Record whether a hello for node `02` appears, and whether reads work without a handshake. This answers #14.
8. Write the results into `05-bus-roles.md` (new section "ESP as node 02"), `04-open-questions.md` #11/#14 and `potential-next-steps.md` Phase 2.

## Risks / Trade-offs

- **[Freezer bus has no pull-up without the box]** → the board would see a constantly low line. Phase 0 test 3 is a prerequisite, with the optional pull-up footprint in D1.
- **[Timer1 or ISR latency jitter pushes the answer late or early]** → Wi-Fi off, a short SoftwareSerial RX ISR, calibration by `delay`. The spec allows ±10 µs, and the freezer is known to accept up to 689 µs.
- **[MOSFET threshold too high for 3.3 V gate drive]** → use a logic-level part (Vgs(th) at most 2 V, e.g. 2N7002 or BSS138, or an AO3400 for margin). Check the low level on the analyzer: it must read as low with margin (below 0.8 V).
- **[Internal pull-up on GPIO15 turns the MOSFET on]** → clear `GPFPU`/`GPFPD` after each `pinMode`/mux call on GPIO15 (D1), checked by task 4.2 (no low pulses caused by the board) and by a capture with `tx off`.
- **[Collision with the box if someone forgets to unplug it]** → auto-disable on the first collided byte (D3) and transmit off at boot.
- **[The handshake replay is incomplete]** (e.g. the freezer expects an ack to `23` this time) → the log shows the freezer's following messages; the responder table is data-driven and can be extended without changing the specs.
- **[Freezer marks node 02 faulty after silent minutes]** → the hot-plug step D6.7 observes it; recovery is a freezer power cycle.
- **Trade-off:** exact replay of observed behaviour instead of understanding each handshake step. Accepted for a PoC; it is the lowest-risk path.

## Migration Plan

Builds on the phase-1 firmware, so phase 1 must be done (or at least tasks 1-4) first. Rollback: `tx off`, or remove the
MOSFET stage; the board is then the phase-1 listener again. Re-plug the box to restore the original setup.

## Open Questions

- Exact acceptance window for slot `02` (the D6.4 sweep). Deferrable: the default of 426 µs is known to work.
- Whether periodic reads by node `02` are required for the freezer to keep node `02` "alive". To be observed during D6.3 and D6.7; phase 3 adds periodic reads anyway.
