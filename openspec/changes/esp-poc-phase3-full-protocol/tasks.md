## 1. Prerequisites

- [ ] 1.1 Confirm that `esp-poc-phase2-node02` passed its bench verification (tasks 5.2, 5.3 and 5.6/5.7) and that the box stays unplugged with the logic analyzer attached

## 2. Application logic in the core (host-tested)

- [ ] 2.1 Write the command layer: map `superfrost`, `party` and `setpoint` to write bodies, with the set-point range constant (default −18…−16 °C), and set the queue priority to handshake, then writes, then reads (design D1)
- [ ] 2.2 Host test: every command produces exactly the body in the spec table, and out-of-range set-points and commands while tx is off or the handshake is running are refused
- [ ] 2.3 Write the state model with per-field age and the update sources: `13` replies to node `02` and node `01`, and node `0B`'s `14/04` broadcast. Write the `STATE` line formatter with `?` for unknown values (design D4)
- [ ] 2.4 Write the event reaction: any non-zero flag word queues the standard status read, coalesced to at most one read queued or pending, plus the periodic read with a configurable interval (design D2)
- [ ] 2.5 Write the write-confirmation state machine (3 s window, event flag, then read-back, then confirmed or unconfirmed, no automatic resend; design D3)
- [ ] 2.6 Host tests: replay event and reply frames from `csv/superfrost_enable_hex.csv`, `csv/partymode_enable_hex.csv`, `csv/door_alarm_on_hex.csv` and `csv/door_short_open_hex.csv` (generated vectors, no group `3C`/`48`). Check the state transitions, the door/alarm updates and the confirmation outcomes
- [ ] 2.7 Write version and identity correlation: service `16` from `7F` (1 s timeout) and from node `01` (3 s); the paged `3C` lists rebuilt with `read_request`; string assembly as `text()` does (design D5). Host test with synthetic replies only

## 3. Firmware integration

- [ ] 3.1 Add the console commands of design D7 (`superfrost`, `party`, `setpoint`, `status`, `interval`, `version`, `identity`, `debug`, `raw`) and the new log lines `STATE`, `C`, `W`
- [ ] 3.2 Guard `raw`: require `debug on`, restrict the destination to `FF`/`81`, force source `02`, refuse during the handshake, and log a warning (design D6)
- [ ] 3.3 Make sure identification strings are only printed and never written to flash or log files in the repository (`logs/` stays git-ignored)

## 4. Bench verification (box unplugged, logic analyzer attached)

- [ ] 4.1 Each write command once (`superfrost on/off`, `party on/off`, `setpoint -17/-16/-18`): the analyzer shows bytes identical to `02-commands.md`, the freezer display changes, and the console reports "confirmed"
- [ ] 4.2 Out-of-range set-point and commands with tx off: refused, and nothing appears on the analyzer
- [ ] 4.3 Door open/close: `STATE` shows the door within 1 s. Door open for more than 1 minute: the alarm becomes active within 2 s of the event block; door closed: the alarm becomes inactive
- [ ] 4.4 Change SuperFrost at the freezer's own panel: the `STATE` line follows within 2 s
- [ ] 4.5 `version` and `identity` print the expected strings (compare with `03-bootup.md`; do not paste serial numbers into repository docs)
- [ ] 4.6 30-minute run with periodic reads: no auto-disable, no CRC errors on the analyzer export, no resets

## 5. Documentation

- [ ] 5.1 Write a command reference, the `STATE` format and the safety notes into `esp-smartdevice-emulator-poc/README.md`
- [ ] 5.2 Record the confirmation behaviour and timings in `protocol-decoding/02-commands.md` and `05-bus-roles.md`, and tick Phase 2/3 items and the acceptance criteria in `potential-next-steps.md`
- [ ] 5.3 Note the follow-ups as candidate changes in `potential-next-steps.md`: Wi-Fi/MQTT bridge consuming `STATE`, ESP32 (XIAO C3/C6) port of the drivers, and verifying the app's set-point range
