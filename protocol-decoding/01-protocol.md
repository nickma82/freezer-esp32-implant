# Freezer SmartDevice box <-> freezer MCU: protocol notes

Consolidated state of knowledge. Sources: all captures in `raw-captures/` (decoded to `csv/*_hex.csv`), tools in this folder
(`src/frames.py` burst splitter, `src/crc.py`, `src/protocol.py` parser/generator, `src/timeline.py`; `tests/`; run `just` for the recipes).
Confidence: **[ok]** seen consistently / verified, **[hyp]** hypothesis, **[?]** unknown.
Commands and their frames: `02-commands.md`. Who sends what, and the evidence for it: **`05-bus-roles.md`**.

> **Correction (2026-10-30):** earlier versions of this file said the box is the bus master and polls the freezer.
> That is wrong. The freezer (node `7F`) sends every BREAK and slot byte; the box is node `02` and may only send in
> slot `02`. Sections 1, 2 and 4 below are updated; the evidence is in `05-bus-roles.md`.

## 0. Capture setup
Logic 2, Async Serial on CH0: 57600 bit/s, 8N1, LSB first, non-inverted. The first capture ran 3.2 s.

| Logic ch | Cable colour | Device pin | Observed |
|----------|--------------|------------|----------|
| GND | Blue | GND | - |
| CH0 | Brown | 2 | all traffic (5744 transitions in the first capture) |
| CH1 | Green+White | 1 | constant 0 |
| CH2 | Orange+White | 3 | constant 1 |

CH1 and CH2 never toggle in the short captures, so their exports were empty and were deleted. In the bootup capture all
channels glitch at power-up (2.69 s) and pin 3 rises from 0 V to 3.3 V at 2.705 s, together with the bus (`05-bus-roles.md`
section 7). Pin 1 and 3 are probably power/enable/strap lines.

### Electrical nature of the data line (pin 2)
The data line is **open-drain (open-collector) with a pull-up**: both the box and the freezer MCU only pull the line low and
never drive it high; the pull-up resistor makes the idle level (~3.2-3.3 V, high). This is why both sides can share one wire
without a driver conflict, and why requests and replies appear on the same channel.
- Consistent with the captures: idle is high at ~3.2-3.3 V, the BREAK is a long low pulse, and nobody ever drives a hard high.
  The rising edges (rise time through the pull-up) have not been measured; an analog capture of one edge would confirm it. **[hyp]**
- Consequence for a replacement controller (ESP32): use an **open-drain output** for TX (or a GPIO that is switched between
  output-low and input/high-Z), never push-pull high, and read the same pin back for RX (the controller hears its own
  transmission). A push-pull driver could fight the freezer MCU or the pull-up.
- Level is 3.3 V, so a 3.3 V ESP32 needs no level shifter. Whether the pull-up sits in the freezer or must be added is unknown. **[?]**

## 1. Line layer
- Single wire, half-duplex, brown cable = device pin 2. Pin 1 stays 0 V, pin 3 stays 3.3 V (no data). **[ok]**
- 57600 bit/s nominal, 8N1, LSB first, idle high (~3.2-3.3 V). Measured: the freezer runs at ~57090 bit/s, the box at
  ~57130 bit/s (-0.9 % / -0.8 %, well inside UART tolerance). **[ok]**
- **Every slot starts with a BREAK, sent by the freezer.** All bursts begin with a low pulse of 18.2 bit times
  (315.4 us +- 0.2 us), then ~5 bit times (~90 us) high, then the slot byte. A UART decoder shows the break as a `0x00` byte
  with a framing error. **[ok]** The box never sends a break. **[ok, clock fingerprint]**
- After the slot byte there is a pause of 250 us (freezer answers), 426 us (box) or 604 us (node `01`/`0B`; 648-689 us
  while that device is in its low-power mode, `05-bus-roles.md` §2) before the body; inside the body the bytes are back to back. **[ok]**
- Slots sit on a ~17.8 ms grid (some 35 ms long). **[ok]**

## 2. Slot and message layer
    BREAK  <slot>  ||  <dst> <src> <svc> <flag> [<len> <payload>]  <crc lo> <crc hi>
    \__ freezer __/      \______________ owner of the slot ________________________/
- `<slot>` gives the send right: below `0x80` node `<slot>` may send; `0x80 | n` = the freezer sends to node `n`;
  `FF` = freezer broadcast. Nobody answers slots `0C`, `7E`, `7F`. **[ok]**
- `<dst>` = `0x80 | node` (`FF` = the freezer, node `7F`; `80` = broadcast), `<src>` = sending node. **[ok]**
  (`protocol.py` calls these `marker` and `second`.)
- **Checksum: CRC-16/XMODEM** (poly 0x1021, init 0, no reflection, xorout 0) over `<dst>` up to the CRC, stored low byte
  first. It excludes the slot byte because another device sends it. Verified on 554 of 554 complete frames. **[ok]**
- Nodes: `7F` freezer (scheduler), `02` SmartDeviceBox, `01` + `0B` a third device (display board?) **[hyp]**, `0C` absent.

### Schedule **[ok]**
| slot | period | owner's idle answer (nothing to send) |
|------|--------|---------------------------------------|
| `0B` | 142.5 ms | `FF 0B 30 00 02 00 00 5D 04` (or a `24` push, e.g. status byte `58/07`) |
| `0C` | 142.5 ms | none (node absent) |
| `7F`, `7E` | 2x / ~1x per 142.5 ms | none **[?]** |
| `01` | 285 ms | `FF 01 30 00 02 00 00 5F 82` |
| `02` (box) | **570 ms** | `FF 02 30 01 02 00 00 0B 3A`; in steady state it also re-reads its `08/14 ... 04/02` list every 2-3 s (door_open_close_nop30) |
| `81`, `82` | on demand | freezer's reply to node `01` / `02`, one 285 ms step after the request |
| `FF` | ~1 s | freezer broadcast (event block) |
Service `30` = ack, or 'nothing to send' when the payload is `00 00`. Requests and replies are paired, reply = request + `0x10`
(`03`/`13` read, `06`/`16` version, `01`/`11` hello, `02`/`12` table); `24` = write/push without reply. See `05-bus-roles.md` section 6.

### Parameter records **[ok]**
Record = `<grp> <id> <flag> [value]`. Groups seen: `00`, `04`, `08`, `0C`, `14`, `24`, `58`, `60`. The flag gives the value width:
`20`/`21` = 1 byte, `40`/`41` = 2 bytes (big endian), `80` = 4 bytes, **`00` = no value (parameter not available)**, e.g. the
reply `0C F9 00` to a request for `0C F9 20`. A read-request list carries only the 3-byte headers.
`protocol.py` parses all 1,900+ non-bare messages of the captures with this rule; re-encoding gives the identical bytes.
(An earlier note here said the record layout was only partly understood; that was a bug in the first (removed) `parse.py`, which
refused group `04`/`60` and tried to parse request lists as values. Use `protocol.py`.)

### Message types
| Form (slot byte first) | From -> to | Meaning |
|------|-----------|---------|
| `02 FF 02 03 01 <len> <grp id flag>... <crc>` | box -> freezer | **read request**: list of params (no values) |
| `82 82 7F 13 01 <len> <rec+value>... <crc>` | freezer -> box | values for the requested list, 285 ms later **[ok]** |
| `01 FF 01 03 01 <len> <grp id flag>... <crc>` | node 01 -> freezer | node 01's read request, same shape (not the box) |
| `81 81 7F 13 01 <len> <rec+value>... <crc>` | freezer -> node 01 | values for the `01` request, e.g. `0C/00` mode, `0C/06` set-point |
| `02 FF 02 24 01 <len> <grp id flag value> <crc>` | box -> freezer | **write one parameter** (see 02) |
| `FF 80 7F 24 01 20 ...` | freezer -> all | **event-flag block**, every ~1 s (below) |
| `0B 80 0B 24 01 04 <grp id flag value>` | node 0B -> all | pushed single value (door switch `14/04`) |
| `0B FF 0B 24 01 <len> ...` | node 0B -> freezer | pushed values: `58/07`, `24/06`, `14/00` |
| `81 81 7F 24 01 <len> 24/xx ...` / `58/xx` | freezer -> node 01 | group-24 / group-58 records pushed to node 01 |

### Event-flag block (cmd `FF`, every ~1 s) **[ok structure, hyp meaning]**
    FF 80 7F 24 01 20  00 00 80 <v4>  00 01 80 <v4>  00 02 80 <v4>  00 03 80 <v4>  00 04 20 <ctr>  <crc>
Params `00/00`..`00/03` are 4-byte flag words, `00/04` is a counter whose value rises by 0x10 every second (wraps at 0xF0->0x00).
Normally all flag words are zero. When something changes the freezer sets flags for one block, then clears them again:

| `00/00` | `00/01` | Observed together with | Seen in |
|---------|---------|------------------------|---------|
| `01000000` | `08000000` | any value change (SuperFrost, party mode, set-point write) | superfrost, partymode, setpoint, bottletimer |
| `00800000` | `04000000` | party-mode change | partymode enable/disable |
| `00000100` | `08000000` | **door-alarm state changed** (on and off) | door_alarm_on (2.17 s of 4.3), door_alarm_off (1.45 s of 3.3), door_open_alarm (2.88 s and 6.87 s) |
Nodes `01` and `02` seem to react to a set flag by reading the affected params. **[hyp]**

## 3. Parameters identified
| grp/id | width | meaning | evidence |
|--------|-------|---------|----------|
| `0C/00` | 1 | mode: `01` normal, `04` SuperFrost active (write the same values) | 8 captures **[ok]** |
| `08/02` | 1 | party mode: `02` on, `00` off (write); SuperFrost is switched by the freezer | 2 captures **[ok]** |
| `0C/06` | 2 | **set-point**, `00 <int8 degC>` (EE=-18, EF=-17, F0=-16) | 2 captures **[ok]** |
| `0C/14` | 2 | current temperature; low byte int8 degC (EE=-18); high byte varies (`00`,`30`,`50`,`70`) **[?]** | |
| `0C/31` | 1 | **door open**: `01` open, `00` closed | door captures **[ok]** |
| `0C/09` | 1 | **door-open alarm** (> 1 min): `01` active, `00` inactive | door_alarm_on/off/open_alarm **[ok]** |
| `14/04` (1 B), `14/00` (2 B) | | door pushed by node `0B`: `01` / `0100` open, `00` / `0000` closed | door_short_* **[ok]** |
| `58/07` | 1 | status byte pushed by node `0B` to the freezer: `11` normal, `01` while the door is open | **[hyp]** |
| `10/00` | 2 | pushed by node `0B` to the freezer, rarely (4 times in all captures, always while node `0B` is in low-power mode): `4217`, `5617`, `2617`, `0A17`; low byte always `17` | **[?]** |
| `0C/07`, `0C/2B`, `0C/10`, `0C/08`, `24/xx`, `58/xx` | | unknown (see 04) | **[?]** |
| `0C/11` | 1 | **power-failure alarm**: `01` active. The app shows "power supply" warning together with "freezing zone <`0C/12`> °C" | bench 2026-11-05: set to `01` with event flags `00000100/08000000`, app warning within the minute **[ok]** |
| `0C/12` | 2 | temperature reported with the power-failure alarm, `00 <int8 degC>` (captured `00 C9` = -55, shown as "-55 °C" in the app); probably the warmest temperature during the outage **[hyp]** | same test |
| `0C/2E` | 2 | read together with `0C/11`/`0C/12`, captured `00 BD` (-67) **[?]** | |
| `0C/0B`, `0C/0D`, `0C/0F` | 1 | alarm-like flags the box polls (`0C/0D` with `0C/0C` = -55 and `0C/2F` = -67, `0C/0F` with `0C/0E`/`0C/30` = 0); set to `01` one at a time with the alarm event flags: **no reaction in the app** | bench 2026-11-05 **[ok]**, meaning **[?]** |
| `08/04` | 4 | **seconds counter**, 32-bit **little endian**, +1 per second (`76 9C 6A 05` at 13.8 s, `8A 9C 6A 05` at 33.7 s, `9F 9C 6A 05` at 54.6 s) | bootup, door_open_alarm, door_open_close_nop30 **[ok]** |
| `08/2A` | 4 | second seconds counter, little endian, +1 per second, other start value (`E6 56 39 08` at 14.4 s) | same 3 captures **[ok]**, meaning **[?]** |
| `08/20` | 2 | drifts slowly, high byte `16`, low byte `BA` → `D6`, `F5`, `8C`, `A6`, `8A`: a measurement, not a counter | **[?]** |

### Values for everything the box reads **[ok]**
`esp-freezer-emulator-poc/tools/gen_params.py` pairs every read request with the freezer's reply in all captures: **285**
distinct answers in groups `04`, `08`, `0C`, `10`, `14`, `24`, `3C`, `40`, `48`, `58`, `60` (generated table
`esp-freezer-emulator-poc/lib/freezer_master/captured_params.h`). Findings from it:
- The answer depends on the **requested flag**, not only on grp/id: `0C/01 20` returns `03`, `0C/01 21` returns `00`;
  `08/90` requested with `40` is answered with flag `20` (1 byte). A replacement freezer must key its table on (grp, id, flag).
- Many requested params are answered with flag `00` by the real freezer itself (e.g. `0C/E4`, `0C/F9`, `60/0C`, `60/0D`,
  `08/9E`, `08/9F`).
- Bench test 2026-11-05 (ESP8266 freezer emulator + real box, no freezer; logs in `esp-freezer-emulator-poc/logs/box_run*.txt`):
  - With only ~10 params known and everything else answered `00`, the box never read group `0C`: it re-read a short
    **17-entry** `08/14 ... 04/02` list every 1.14 s and the app reported "Something went wrong" for SuperFrost and set-point.
  - With the captured table and the box **power-cycled while the emulator was running** (`box_run6.txt`), the box ran
    the full start-up of `03-bootup.md` (handshake, versions from `7F` and `01`, `08`/`0C` lists, `3C`, WiFi writes,
    `48/00` = `03` → `04` → `05`), then the steady-state cycle (`08/15` list, `0C` lists, **22-entry** `08/14` list), and
    SuperFrost on/off and set-point -16 °C from the app arrived as the writes in `02-commands.md`, byte for byte. **[ok]**
  - Once healthy, the box **stays** healthy when the freezer side goes away: two ESP resets (bus idle high ~8 s) and the bus
    held **low** for 5 s and 30 s (`L <ms>` console command, like a freezer switched off), each followed by the hello and
    handshake, left it reading its `0C` lists (`logs/restart_exp_*`). **[ok]**
  - But after the freezer side was **gone for ~4 min** (ESP crash, master stopped until restarted by hand) the box came
    back **degraded**: handshake and `48`/`4C` writes only, then the 17-entry list, no `0C` reads, app commands fail.
    `box_run5.txt` is the same case (ESP unplugged for minutes). So the trigger is a freezer outage longer than somewhere
    between 30 s and 4 min, or start-up reads answered `00`. **[ok observation; exact timeout ?]**
  - Nothing tried brought a degraded box back: hello/handshake restart, all event flags `FFFFFFFF FFFFFFFF`, writing the
    box's `48/00` back to `00`, a 10 s bus-low "power-off" of the freezer. Only a **box power cycle** with a working freezer
    on the bus does. A real freezer never disappears for minutes while the box is powered (they share the supply).

## 4. What a replacement box needs (summary)
The replacement acts as **node `02`**. It never sends a BREAK or a slot byte.
1. Detect `BREAK` + slot byte `02` (every 570 ms) and answer within ~0.4 ms with one message: `FF 02 ...` + CRC.
   With nothing to say, answer `FF 02 30 01 02 00 00 0B 3A`.
2. Write mode/party/set-point by answering slot `02` with `FF 02 24 01 04 <grp> <id> 20 <val> <crc>` (see 02).
3. Read params by answering slot `02` with a `03` list; the values arrive in slot `82` one 285 ms step later.
4. Watch the event-flag block in slot `FF`; on a set flag read `0C/09` (alarm), `0C/31` (door), `0C/00` (mode).
5. Door-open alarm = flag word `00000100`/`08000000`, then `0C/09 = 01`. Cleared the same way with `0C/09 = 00`.
6. Whether the freezer insists on the start-up handshake (hello, table exchange, `21`-`23`) is open (`03-bootup.md`).

The reverse direction (replacing the **freezer** towards a real box) works on the bench: `esp-freezer-emulator-poc/`. It
needs the slot schedule, the handshake (`01`/`11`, `21`-`23`, `02`/`12`), the version reply `16`, node `01`'s version
reply (the box asks node `01` with `81 02 06 01 00` and waits for `82 01 16 ...` in slot `01`), answers to every read
(section 3, "Values for everything the box reads") and the ticking counters `08/04`/`08/2A`.

## 5. Open points
- A write gets no reply slot; the only confirmation seen is the event flag and the next read.
- Group `24` records (`24/00`, `24/06`, ...) alternate between two variants (`0000`/`01` and `FFFF`/`11`) while the door is open; they are *not* the alarm. **[?]**
- Group `58` block, params `0C/07`, `0C/2B`, `0C/10`, `0C/08`, bit meanings of the flag words.
- Purpose of slots `7E` and `7F` (never answered), and what node `01`/`0B` physically is.
- BottleTimer: no bus command (probably implemented in the cloud/app, user's call to leave it alone).
