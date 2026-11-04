# ESP8266 freezer emulator PoC (not bench-tested)

An Amica NodeMCU v2 (ESP8266EX) plays the **freezer** (node `7F`, the bus scheduler) and tries to talk to the real
SmartDeviceBox, with the freezer disconnected. Sibling of `../esp-smartdevice-emulator-poc` (which plays the box). Entry
point: `just` lists the recipes. Wiring and the open power question: `WIRING.md`.

## What it does
- Sends BREAK (315 µs low) + slot byte on the captured grid: 12 slots / 16 ticks of 17.8 ms per half cycle, `02` in every
  second half, nodes `01`, `0B`, `0C` and the `7E`/`7F` slots stay bare (those nodes are not emulated).
- Start-up handshake with node `02`: hello in slot `82`, then reply `21`, `22`, table `12`, `23` as captured
  (`../03-bootup.md`). The hello is repeated every 5 s while the box is silent.
- Replies to the box's reads (`03` → `13`) from a small parameter table (mode, set-point, temperature, door, alarm, party,
  group `3C` identification). Unknown parameters get flag `00` ("not available"). Version request → `TOD 5-600|SW 1-86-7|BUS 1-4-0`.
- Box writes (`24`) update the table and raise the event flags in the next 1 s `FF` event block.
- Every slot is looped back through the UART and logged; our own bytes are compared with the echo (`E echo mismatch` = another
  driver on the bus).

## Experiment goals
1. Does the box answer in slot `02` (hello reply `11`) with only this master and no node `01`/`0B`?
2. Does the handshake complete and does the box start its normal reads (`03`) and Wi-Fi writes (group `48`)?
3. Does the box tolerate the missing nodes `01`/`0B`, the missing 1 s event block timing, truncated replies (reply cap is
   109 bytes, paging of long reads is a guess)?
4. Timing: break/slot/answer window as seen by the box (analyzer: `cd .. && just slots <csv>`).

## Layout
```
platformio.ini         env:nodemcuv2 (firmware), env:native (host tests); shares ../esp-smartdevice-emulator-poc/lib (lb core)
lib/freezer_master/    chip-independent schedule, handshake and replies (host-tested)
lib/bus_master_esp8266/UART0 RX ring (copy of the phase-1 driver) + TX with BREAK generation
src/main.cpp           scheduler loop, logging, console
test/test_master/      Unity tests replaying the captured handshake
logs/                  board logs (git-ignored)
```

## Console (115200, `just monitor`)
`g` start master (off at boot, TX disabled), `s` stop, `r` restart handshake, `v` full output (bare slots), `?` help.

## Log format
Same as the sibling: `M <t_us> <slot> <lat_us|-> <crc 1|0|-> <body hex> | <text>`, `S` every 10 s (master state, `hs=`,
`box_answers`, `echo_err`, `hellos`, `replies`, `events`, `reads`, `writes`, `unknown`), `E` errors, `#` free text. Run `just log`
and capture the bus with the analyzer (Logic 2 via sub-agent, see `CLAUDE.md`).

## Status
Host tests pass and the firmware compiles. Nothing has run on hardware: BREAK length, the 405 µs / 250 µs timing, the
GPIO15 stage and the loopback of our own break (RX shares the baud divisor, so the ISR uses a `self_break` flag) must be
checked with the analyzer first, with the box unconnected.
