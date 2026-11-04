## 1. Prerequisites (bench, no new firmware)

- [ ] 1.1 Confirm that `esp-poc-phase1-listener` tasks 1-4 are done and that the listener evaluation (phase 1 task 5.2) passed
- [ ] 1.2 Run `potential-next-steps.md` Phase 0 test 2: capture 10 s with the box unplugged and check with `cd .. && just slots` that the schedule continues and slot `02` stays bare
- [ ] 1.3 Run Phase 0 test 3: the multimeter pull-up check on pad 2 with the box unplugged. Decide whether the optional pull-up (design D1) is needed, and note the result in `05-bus-roles.md`

## 2. Responder logic in the core (host-tested)

- [ ] 2.1 Write the answer queue (depth 8, FIFO, one body per slot `02`, overflow policy per design D4) and the static idle body
- [ ] 2.2 Write the handshake responder table (hello, `21`, `22`, `23`) and the handshake state for logging
- [ ] 2.3 Write read-request correlation: one outstanding read, matched to the next `13` message in slot `82` from `7F`, with a 1 s timeout
- [ ] 2.4 Host test: replay the slot-`82` messages of `csv/freezer_bootup_hex.csv` 3.0-8.2 s (via a generated vector, no group `3C`/`48` payloads). The produced answers must equal the box's slot-`02` bodies of that window in order: hello reply, ack `21`, table request, ack `22`
- [ ] 2.5 Host test: the status read body equals `FF 02 03 01 12 0C 00 20 0C 06 40 0C 14 40 0C 31 20 0C 09 20 08 02 20 9B 42`

## 3. Transmit path in the UART driver

- [ ] 3.1 Set up TX inversion (`UCTXI`) and route U0TXD to GPIO15 only on `tx on`, clearing the GPIO15 internal pull-up (`GPFPU`/`GPFPD`) after every mux change (design D1). Otherwise GPIO15 is a GPIO output held low
- [ ] 3.2 Write the timer1 single-shot reply: armed from the RX ISR on slot byte `02` with all guards of design D2; the timer ISR copies the prepared body into the TX FIFO
- [ ] 3.3 Write read-back comparison during transmission, auto-disable on mismatch or frame error (design D3), and the board-measured latency from the first echoed byte
- [ ] 3.4 Write the console CLI `tx on|off`, `delay <us>`, `read`, `hs`, `stats`, `v` (design D5), plus the `T` log lines and the new `S` fields. Extend `tools/compare.py` to accept `T` lines

## 4. Hardware

- [ ] 4.1 Build the MOSFET stage of design D1 on a breadboard (logic-level N-MOSFET, 100 Ω gate, 100 kΩ gate pull-down, 100 Ω drain). Check that GPIO15 reads low at boot and the board still boots and flashes with the stage attached
- [ ] 4.2 With the box still unplugged and the freezer running: reset and re-flash the board while capturing. The analyzer must show no low pulses caused by the board

## 5. Bench verification (box unplugged, logic analyzer attached)

- [ ] 5.1 TX off: a capture with the board attached equals the capture from task 1.2
- [ ] 5.2 `tx on`, idle only, capture 2 minutes: src-`02` latency 426 ± 10 µs (`just slots`), 0 CRC errors, schedule unchanged, no error on the freezer display. Calibrate the delay offset if needed
- [ ] 5.3 `read`: the reply arrives in slot `82` about 285 ms later and the values match the freezer display
- [ ] 5.4 Optional delay sweep (350/500/650 µs): note which delays the freezer accepts
- [ ] 5.5 Collision test: attach the box as well with `tx on`. The board must auto-disable in the first slot and log the collision. Unplug the box again
- [ ] 5.6 Freezer power cycle with the board attached and `tx on`: capture the handshake and compare the order with the spec table
- [ ] 5.7 Hot-plug: freezer running without the board for 30 s, then attach it and `tx on`. Record whether a hello to node `02` appears and whether reads work without a handshake

## 6. Documentation

- [ ] 6.1 Write the results into `protocol-decoding/05-bus-roles.md` (new section "ESP as node 02": latency, delay sweep, handshake observations)
- [ ] 6.2 Update `protocol-decoding/04-open-questions.md` #11 and #14 with the findings, and tick Phase 2 in `potential-next-steps.md`
- [ ] 6.3 Extend `esp-smartdevice-emulator-poc/README.md` with the phase-2 wiring (MOSFET stage), the CLI and the safety rules (box unplugged, `tx` off by default)
