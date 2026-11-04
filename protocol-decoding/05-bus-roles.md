# Bus roles: who schedules, who answers, what kind of protocol this is

Re-evaluation of `01`-`04` against all captures (2026-10-30). Tools: `src/slots.py` (`just slots`, over the decoded
exports), `src/clocks.py` (`just clocks`, over the raw edges), `tests/test_bus_roles.py`.
Confidence tags as in `01-protocol.md`: **[ok]** verified on the captures, **[hyp]** hypothesis, **[?]** unknown.

## Summary
The bus is **not** "the box polls the freezer". It is a **master-scheduled, single-wire slot bus**:

1. **Slot layer (LIN-like).** One node, the **scheduler**, sends `BREAK` + one **slot byte** on a fixed time grid. The
   node that owns that slot answers after a short pause, or the slot stays empty.
2. **Message layer.** The slot owner sends `<dst> <src> <svc> <flag> <len> <payload> <crc lo> <crc hi>`.
   `dst` = `0x80 | node`, `src` = node. The CRC covers exactly this part and not the slot byte, because the slot
   byte comes from a different device.
3. **Application layer.** Request/reply services (reply = request + `0x10`) carrying parameter records
   `<grp> <id> <type> [value]`, i.e. a small object dictionary.

The scheduler is node **`7F`**, and node `7F` is the **freezer's main controller** **[ok for "scheduler = node 7F", hyp
for "7F sits in the freezer"]**. The SmartDeviceBox is node **`02`**, a slave that may only talk in slot `02`. There is a
**third device** on the bus, nodes `01` + `0B`, most likely the freezer's display/user-interface board **[hyp]**.

It is not LIN (no `0x55` sync byte, no protected-ID parity bits, 57600 bit/s, CRC-16), not 1-Wire, and not any public
bus we know of. Treat it as proprietary, built from a stock UART with break support.

## 1. The byte after the BREAK is not part of the message **[ok]**
The old notes say "bytes inside a message are back to back". They are not. There is a pause after the first byte, and
its length depends on who answers (`just slots`, all 16 captures):

| Slot | Body sent by | Pause, slot byte start to first body byte | Spread (sd) |
|------|--------------|--------------------------------------------|-------------|
| `81`, `82`, `8B`, `8C`, `FF` | node `7F` | 250 us (= 1 byte + 75 us) | 6-15 us |
| `02` | node `02` (box) | 426 us | 2 us |
| `01`, `0B` | node `01` / `0B` | 604 us awake, 648-689 us (median 668) in low-power mode, see §2 | 1 us / 6 us |

Inside the body, bytes follow each other at 175 us (10 bits). Three different pause profiles mean three different
responders. The bare `0C`, `7E` and `7F` frames are slots in which nobody answers (2,830 of them, never a body).

## 2. The scheduler is node 7F: clock fingerprint **[ok]**
Every device has its own oscillator, so its bit period is slightly different. `src/clocks.py` measures it per sender
from the raw edges (24 MS/s), using falling edges only (the line is pulled low actively but rises slowly through the
pull-up, so rising edges are biased). Two raw exports exist: `raw-captures/digital.csv` (the first 3.2 s capture) and
`raw-captures/freezer-bootup-fullduration.csv` (the whole 58 s bootup, 2,321 BREAKs). The table is from the bootup:

| Bytes | Bit period (median) | sd per byte |
|-------|---------------------|-------------|
| slot bytes (after every BREAK) | **17.5156 us** (57092 baud) | 0.006 |
| bodies from node `7F` | **17.5156 us** | 0.006 |
| bodies from node `02` (box) | 17.5052 us (57126 baud) | 0.004 |
| bodies from nodes `01` / `0B`, awake | 17.5156 us | 0.005 |
| bodies from nodes `01` / `0B`, low-power mode | ~17.557 us (mean) | 0.07 (jittery) |

Same byte value, different sender: `0C` as a slot byte 17.5166 us, from node `7F` 17.5166 us, from node `02` 17.5082 us.
The slot bytes (and the BREAKs, which are all 315.5 us +- 0.25 us) are generated with node `7F`'s clock. The box's clock
is 0.06 % faster and clearly separable (thousands of bytes per group, both captures agree). **So node 7F sends every
BREAK and slot byte.**

### Node 01/0B has two clock states **[ok for the states, hyp for the cause]**
`just clocks` also reports, per sender, whether every falling edge of a body lies within 0.2 us of the master's bit grid
("clean") or not ("jitter"). Nodes `7F` and `02` are clean in both captures, all the time. Nodes `01` and `0B` switch
**together** between two states:

| | awake | low-power |
|---|---|---|
| pause after slot byte | 603-609 us (median 604, sd 0.7) | 648-689 us (median 668, sd 6) |
| bit period | 17.5156 us, same as node `7F` | ~17.557 us mean (0.24 % slow), per byte 17.29-17.85 us |
| falling-edge error against an ideal clock | +-2 samples (+-83 ns) | up to +-1.6 us (9 % of a bit) |
| single-bit low pulse | 17.71 us | 17.25 us, sd 0.59 |

The pause alone separates the states (threshold 630 us), so the decoded exports show them too (`just slots`, last
section, `ui_states()`), in all 16 captures:

| Capture | Door (node `0B` push `14/04`) | Node `01`/`0B` |
|---------|-------------------------------|----------------|
| `door_short_open` | opens at 1.41 s | low-power until 1.55 s, awake from 1.70 s |
| `door_open_close_nop30` (made for this question) | opens 1.00 s, closes 4.56 s, then 30 s without input | low-power until 1.14 s, awake 1.28-9.55 s, low-power 9.69 s to the end (35.3 s) |
| `door_alarm_on`, `door_open_alarm`, `door_alarm_off`, `door_short_closed` | open; where it closes, 1.7-2.4 s before the end | awake throughout |
| `freezer_bootup` | `00` at 9.76 s (probably the initial state), open 28.00 s, closed 33.27 s, open 54.78 s | awake 3.2-38.82 s, low-power 38.97-54.92 s, awake from 55.07 s |
| all app-command captures (SuperFrost, party, set-point, bottle timer), first capture | closed, no push | low-power throughout |

So the device sleeps while the door is closed and wakes when it opens; the door push itself and the message after it
still go out on the slow clock, the next one (285 ms later) is clean. Box writes (SuperFrost, party mode, set-point) do
not wake it, and neither do the box's continuous reads. **Timeout: 5-6 s after the door closes** (5.0-5.1 s in
`door_open_close_nop30`, 5.6-5.7 s in `freezer_bootup`, measured from the `14/04` = `00` push; the spread suggests a
timer with a ~1 s tick **[hyp]**). Once asleep it stays asleep: 26 s without input, no periodic wake-up. Only after
power-up it stayed awake much longer (at least 25 s with the door closed); something else keeps it awake there
(power-up, the box start-up, or the Wi-Fi writes at 21.5 s and 25.8 s) **[?]**. The first capture
(`digital.csv`) is entirely in low-power mode, which is why it showed 17.48 us and wide pause spreads.

What this tells us:
- Node `01`/`0B` is **its own MCU**, not a second address of node `7F`: its clock changes while `7F`'s does not. Both
  addresses switch together, so `01` and `0B` are one device **[ok]**.
- It runs from the same nominal clock as `7F` when awake (same model of MCU and crystal, or a shared clock source **[?]**)
  and from a slower, unstable clock (internal RC/DCO oscillator) in low-power mode **[hyp]**.
- A door-dependent power state fits a display/UI board with the door switch **[hyp]**.
- The master accepts answers that start up to 689 us after the slot byte and bytes whose edges are 1.6 us off: a first
  lower bound for the answer-window tolerance (`04-open-questions.md` #11). The box itself answers at 426 us.

Limits: the other `.sal` captures are not exported as raw edges yet. Exporting needs the Logic 2 GUI (the Logic MCP
crashes on raw export).

## 3. Node 7F is the freezer **[hyp, strong]**
- It answers reads of group `3C` with the freezer's model, part and serial numbers, and sends the 1 s event block.
- At power-up it enumerates the other nodes (hello, service `01`) and keeps scheduling slot `02` while the box is still
  silent (the box first answers at 5.8 s, 2.7 s after the schedule starts).
- It schedules slot `0C` and `7E` forever without an answer; a box would have no reason to poll absent freezer nodes.
- Version string `TOD 5-600|SW 1-86-7|BUS 1-4-0` comes from node `7F`.
Definitive proof: the "box unplugged" test in `potential-next-steps.md`.

## 4. Nodes and addressing **[ok]**
The two bytes after the slot byte are destination and source. Every message in every capture fits:

| Slot byte | Owner (sender) | dst / src seen |
|-----------|----------------|----------------|
| `01` | node `01` | `FF`/`01` (to master), once `82`/`01` (to node 02) |
| `02` | node `02` = **box** | `FF`/`02` (to master), once `81`/`02` (to node 01) |
| `0B` | node `0B` | `FF`/`0B` (to master), `80`/`0B` (broadcast) |
| `0C` | node `0C` | never answers: node absent on this freezer |
| `7E`, `7F` | ? | never answered **[?]** (free slots or the master's own) |
| `81`, `82`, `8B`, `8C` | node `7F` | `8x`/`7F`: master to node `x` |
| `FF` | node `7F` | `80`/`7F`: master broadcast (the 1 s event block) |

Rules: a slot byte below `0x80` gives the send right to that node; `0x80 | n` is a slot in which the master sends to node `n`.
Destination `0x80 | n` addresses node `n`, `FF` therefore addresses the master (`7F`), and `80` is broadcast. Slaves can
address each other directly (`02` asks `01` for its version, `01` answers to `82`). One anomaly: at 4.0 s in the bootup
capture slot `01` carried a frame with source `0B`, which supports "01 and 0B are the same device".

### Nodes 01/0B and 02 are different devices **[ok]**
| | node `01` + `0B` | node `02` |
|---|---|---|
| pause after slot byte | 604 us awake, 648-689 us low-power | 426 us, sd 2 |
| UART clock | 17.516 us/bit awake, ~17.56 us/bit jittery low-power | 17.505 us/bit, stable |
| hello payload (svc `11`) | `43 01 02 03 00 01` | `03 01 00 00 00 00` |
| first answer after power-up | 3.2 s | 5.8 s |
| version (svc `16`) | `SW 6-2-0` | never asked |
| what it sends | parameter reads, door switch, status `58/07` | all app writes, Wi-Fi data (group `48`), reads |

Node `02` is the box: it writes the Wi-Fi data and all 16 app commands (SuperFrost, party mode, set-point). Node `0B`
pushes the door switch (`14/04`, `14/00`) and `58/07`, and node `01` reads the 18 alarm parameters after an alarm event;
a display/UI board with door switch and beeper fits that **[hyp]**. The box never sends door state.

## 5. Schedule **[ok]**
Fixed grid of ~17.8 ms per slot, some slots double length (35 ms, after slots that may carry long bodies).
A minor cycle of 142.5 ms repeats `0C 7F <flex> <flex> 7F 0B`; the flexible positions carry `7E`, `01`/`02` and the
master's on-demand slots.

| Slot | Period |
|------|--------|
| `0B`, `0C` | 142.5 ms |
| `7F` | 2 per 142.5 ms |
| `01` | 285 ms |
| `02` (box) | **570 ms** |
| `81`, `82` | on demand, only when the master has a reply pending |
| `FF` | ~1 s (event block) |

Consequences: the box can send **one message per 570 ms**. A read request from the box in slot `02` is answered by the
master in slot `82` exactly one 285 ms step later (102 of 102 reads). A write (`svc 24`) gets no reply slot; the box's
next slot-`02` message follows normally and the change shows up in the event block and later reads.

## 6. Services **[ok for the pairs, hyp for the names]**
`<svc> <flag>`: the flag is `01` in all messages except the slave "nothing to send" reply `30 00`. Replies are the request + `0x10`.

| Request | Reply | Direction | Meaning |
|---------|-------|-----------|---------|
| `01` | `11` | master to slave, at power-up | hello / identify; reply `<6 bytes>` per device type |
| `02` | `12` | slave to master | table exchange at start-up (4-byte entries; the master fills in the last byte) |
| `03` | `13` | slave to master | read parameter list / values |
| `06` | `16` | any to any | version string |
| `21`, `22`, `23` | `30` ack `<svc> 00` | master to slave, at start-up | init steps, acknowledged with status `00` |
| `24` | none | any | write / push values (box writes, `0B` pushes, master broadcast event block) |
| `30` | | slave to master | ack, or `30 00 02 00 00` / `30 01 02 00 00` = nothing to send |

## 7. Power-up **[ok]**
In `freezer-bootup-fullduration` pad 3 (CH2) is at 0 V at the start and rises to 3.3 V at 2.705 s; all channels glitch
together at 2.69 s. The two "breaks" at 2.69 s in the decoded export are those glitches, not traffic. The first real
frame is at 3.063 s (master hello to node `0B`). So this capture is a power-up of the whole appliance (or of the box
supply on pad 3), not of the box alone. CH1 (pad 1) only glitches at 2.69-2.70 s and is 0 V otherwise.

## 8. What changed against the earlier notes
| Earlier statement | Now |
|-------------------|-----|
| The box is master and polls `01`, `0B`, `02` every 285 ms | The freezer (node `7F`) schedules all slots; the box only owns slot `02`, every 570 ms |
| `<cmd>` is the request from the box, reply = cmd \| 0x80 | Slot byte = send right; reply comes in slot `0x80 \| node` from the master |
| `0C`, `7E`, `7F` are bare 1-byte messages of unknown purpose | Empty slots; node `0C` does not exist on this freezer |
| `01` and `02` are two read channels of the box | Two different devices; `01`/`0B` is not the box |
| The 18-parameter alarm read is done by the box | It is done by node `01` |
| Door state is pushed by the freezer | It is pushed by node `0B` (broadcast) |
| Bytes in a message are back to back | 250-660 us pause after the slot byte, then back to back |
| `SW 1-86-7` then `SW 6-2-0` = loader then main program | `SW 1-86-7` = node `7F`, `SW 6-2-0` = node `01` |
| Bootup scan `8B 8C FF 81 82` is "who is there" by the box | It is the master's hello to nodes `0B`, `0C`, `7F`, `01`, `02` |
| The ESP32 must generate breaks and act as poll master | The ESP32 must act as node `02`: wait for `BREAK` + `02`, answer within ~0.4 ms, never send a break |
| Pad 1 and pad 3 never toggle | They glitch at power-up; pad 3 comes up 3.3 V together with the bus |
| Node `01`/`0B` has a jittery UART (different hardware) | Same bit rate as `7F` when awake; slow, jittery clock in a low-power mode while the door stays closed, woken by the door |

Unaffected: the CRC, the record format, the parameter meanings, the write frames (they are the body the box sends after
slot byte `02`), the event block and the parser (`protocol.py` calls `dst` `marker` and `src` `second`).

## 9. ESP listener (phase 1 results, 2026-10-31) **[ok]**
Amica NodeMCU (ESP8266), UART0 RX moved to GPIO13/D7 through 4.7 kΩ, receive only, box and logic analyzer attached.
- **BREAK handling:** every BREAK pushes one `0x00` into the RX FIFO and fires `UIBD` exactly once (421 of 421 in 40 s). No spurious or split slots.
- **Pitfall:** Arduino `pinMode(13, FUNCTION_4)` switches the pad's internal pull-up on. With the 4.7 kΩ series resistor that held the D7 node flat and the UART saw nothing. The driver clears `GPFPU`/`GPFPD` after the mux setup. The GPIO input register also reads 0 in `FUNCTION_4`, so it is not usable as a level probe there.
- **Decoding vs Logic 2 (simultaneous, 300 s, door open/close and SuperFrost on/off):** 12635 of 12636 analyzer messages byte-identical (99.992 %), 0 missing, 0 extra. The one difference is a lone trailing `FF` that Logic reports and the UART never receives (a sub-byte glitch seen by the probe only; the same type once as `7E FF` in a 70 s run, 2 in about 6 minutes). 0 CRC errors, 0 framing errors, 0 overruns, 0 ring drops.
- **Latency measured by the board** (median, board minus Logic): node `01` 666 µs (-0.2), `0B` 666 µs (-0.2), `02` 425 µs (-0.5), `7F` 250 µs (-0.5). In the door-open phase node `0B`/`01` answers at about 604 µs (S lines), in line with §2.
- **Slot rates** (90 s, full output): `0B` 7.0/s, `01` 3.5/s, `02` 2.1/s (2 of 3 carry a data read `03 01 ...`, the rest are idle acks), `82` 1.4/s, `FF` 1.0/s, `0C`/`7E` 7.0/s empty, `7F` 13/s empty.
- **Not intrusive:** with and without D7 attached, break to slot-byte start is 405.3 µs in both, node `02` latency is 426 µs in both, schedule and slot counts are identical.
- **Reset and flashing:** a 50 s capture with a board reset and a re-flash showed no framing errors, no gap longer than 35 ms and no unanswered slots.
- **Long run (task 5.4):** 360 `S` lines over 3650 s, one boot banner, steady 10 s cadence. During the first 44.7 min of live traffic (39600 valid messages, about 151 per 10 s): 0 CRC, decode and framing errors, 0 overruns, 0 ring or log drops, slot-`02` period only 285/570 ms, node-`02` latency 425 µs. The bus was then unplugged by hand; the board stayed up for the remaining 16 min without a reset.
