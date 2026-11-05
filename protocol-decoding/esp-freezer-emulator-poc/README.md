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
- A set-point write from the box (`0C/06`, `00 <int8 °C>`) is logged as `# SETPOINT <t_us> box wrote target temperature -17 degC`
  (every write, also an unchanged re-send; the table value is updated, the current temperature `0C/14` stays at -18 °C).
- Everything the emulator does not know is logged for protocol discovery, see "Discovery" below.
- Every slot is looped back through the UART and logged; our own bytes are compared with the echo (`E echo mismatch` = another
  driver on the bus).

## Discovery (what the box asks for that we do not know)
The point of this emulator is to learn more about the init sequence and the protocol, so it records what it cannot answer:
- `# UNKNOWN <t> box READ 77/01 (answered flag 00 = not available)`: first read of a parameter not in the table.
- `# UNKNOWN <t> box WRITE 77/01 flag+value: ...`: first write of an unknown parameter (it is stored in the table afterwards).
- `# UNKNOWN <t> box SERVICE 55 (no reply) frame: ...`: every message with a service the emulator does not handle
  (first 32 raw bytes, from the dst byte on, CRC included). Unhandled services get no reply.
- Key `k` lists everything the box has read (`R`), written (`W`) or sent as service (`S`) with counts; `*` = unknown to us.
  The `M` lines stay the complete raw record; these are the digest.

## Exploration console
Active probing, to see how the box reacts. Lines end with Enter; queued messages leave in the next free reply slot `82`,
after pending replies. The hex arguments may contain spaces.

| Command | Effect |
|---|---|
| `x <svc> [payload]` | message to the box, length byte and CRC added (e.g. `x 06` = version request) |
| `X <dst src svc flag [len] payload...>` | raw body, only the CRC is added |
| `a 1` / `a 0` | door alarm on/off: sets `0C/31` + `0C/09` and event flags `00000100 / 08000000` (the captured door-alarm change, `../02-commands.md`) |
| `t <degC>` | current temperature `0C/14`, event flags `01000000 / 08000000` (**hypothesis**: only door, party, set-point, SuperFrost changes were captured) |
| `e <w0> <w1>` | OR event flag words `00/00` and `00/01` into the next 1 s event block (8 hex bytes), to find the unknown bits |
| `p <grp id flag value>` | set a parameter without an event (e.g. `p 0C 06 40 00 EF`); announce it with `e` |

Only the door alarm is known. The other 17 parameters of the alarm read (`0C/4C 47 11 0D 0F 46 4B 12 2E 0C 2F 0E 30 45 58 49 0B`,
`../04-open-questions.md` item 5) are unnamed: set one with `p`, raise `e 00000100 08000000`, and watch whether and how the box
reads it. An unexpected `X` frame may confuse the box; `r` restarts the handshake.

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

## Console (57600, `just monitor`)
Single keys (no Enter): `g` start master (it starts by itself at boot), `s` stop, `r` restart handshake, `v` output quiet → normal → full (quiet is the default: routine reads, replies and idle acks hidden)
(bare slots), `k` list what the box asked for, `?` help. Command lines (`x X a t e p`): see "Exploration console".

## Log format
Same as the sibling: `M <t_us> <slot> <lat_us|-> <crc 1|0|-> <body hex> | <text>`, `S` every 10 s (master state, `hs=`,
`box_answers`, `echo_err`, `hellos`, `replies`, `events`, `reads`, `writes`, `unknown`), `E` errors, `#` free text (`# SETPOINT`, `# UNKNOWN`, console acknowledgements). Run `just log`
and capture the bus with the analyzer (Logic 2 via sub-agent, see `CLAUDE.md`).

## Status
Host tests pass (8 cases: schedule, handshake replay, identity read, write + event block, set-point, unknown notes, injection + alarm, hello retry) and the firmware compiles. Bench, no box (2026-11-05): TX on D4/GPIO2 (UART1, open-drain, 47 Ω, 4.7 kΩ pull-up) and RX on
D7 read back every slot unchanged (`echo_err=0 framing=0 overruns=0` over 420+ slots per 10 s, body 250 µs after the slot
byte); the console works on the USB pins. Analyzer: BREAK 315.5 µs, slot byte 405 µs after it, body 250 µs after the
slot byte, bit time 17.50 µs.

**Against the real box (2026-11-05): works.** Read replies come from `lib/freezer_master/captured_params.h` (285 values
the captured freezer returned, `just params` regenerates it from `../csv`), `08/04`/`08/2A` tick once per second, and the
emulator answers the box's version request to node `01` in slot `01`. After a **box power cycle with the emulator
running**, the box does its full start-up and the app's SuperFrost on/off and set-point arrive as the writes in
`../02-commands.md` (`logs/box_run6.txt`). If the bus is without a master for minutes (ESP crashed or unplugged,
master stopped) the box drops into a degraded mode (only its short `08/14` list, app reports "Something went wrong");
short outages (reflash, `L 30000`) are harmless. Recovery: power-cycle the box with the emulator running. Unplugging the box's bus/GND can disturb the ESP's USB link; switch only the box's
supply.
