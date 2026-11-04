# Commands (write path) and door alarm

Sources: `csv/superfrost_{enable,disable}_hex.csv`, `csv/bottletimer_{enable,disable}_hex.csv`,
`csv/partymode_{enable,disable}_hex.csv`, `csv/door_open_alarm_hex.csv`, plus the two set-point captures.
Tooling: `src/crc.py` (checksum), `src/protocol.py` (frame builder `write_param()` and the `SUPERFROST_ON` ... helpers; `tests/test_generator.py` and
`tests/test_captures.py` check every command below byte for byte against the captures). Run `just test`, `just demo`.

## Checksum: solved
**CRC-16/XMODEM** (poly 0x1021, init 0x0000, no reflection, xorout 0), computed over **all bytes after the command
byte** (i.e. starting at `FF`/`81`/`82`, excluding the CRC itself), stored **low byte first**.
Holds for every complete frame in every capture (0B, 01, 02, 81, 82, FF; 554 frames). The only failure is one
frame cut off by the end of a capture. **[ok]**

## Write command format [ok: 6 distinct commands match]
    00(framing err)  02  ||  FF 02 24 01  <len>  <grp> <id> <flag> <value...>  <crc lo> <crc hi>
    \___ freezer: BREAK + slot 02 ___/  \_______________ box (node 02) ______________________/
`len` = 3 + value size. `flag` = `20` (1 byte value) or `40` (2 byte value, big endian). Same envelope as a read request
(`02 FF 02 03 01 ...`), but service `24` instead of `03` marks a write and the record carries a value. `FF` = to the
freezer (node `7F`), `02` = from the box (node `02`). **[ok, see `05-bus-roles.md`]**
The leading `00` and `02` are sent by the freezer, not the box: the box only sends from `FF` onwards, in the slot the
freezer grants it every 570 ms. The frames in the table below include the slot byte `02` (as `write_param()` returns them).

## Commands
| Function | Param (grp/id) | Value | Full frame after the leading `00` | Confidence |
|----------|----------------|-------|-----------------------------------|------------|
| SuperFrost **on**  | `0C/00` (1 byte) | `04` | `02 FF 02 24 01 04 0C 00 20 04 5D 69` | seen in 2 captures, **[ok]** |
| SuperFrost **off** | `0C/00` | `01` | `02 FF 02 24 01 04 0C 00 20 01 F8 39` | seen in 2 captures, **[ok]** |
| PartyMode **on**   | `08/02` (1 byte) | `02` | `02 FF 02 24 01 04 08 02 20 02 0A AD` | **[ok]** |
| PartyMode **off**  | `08/02` | `00` | `02 FF 02 24 01 04 08 02 20 00 48 8D` | **[ok]** |
| Set-point (e.g. -17 / -16 °C) | `0C/06` (2 bytes) | `00 <int8 °C>` | `02 FF 02 24 01 05 0C 06 40 00 EF D8 99` / `... F0 06 7A` | **[ok]** |
| BottleTimer on / off | - | - | **no dedicated command found** | see below |

State readback: node `01`'s periodic `01`/`81` read returns param `0C/00` = `04` while SuperFrost is active and `01` when it is not.
Param `08/02` shows up in a separate `01/81` pair (`08 15, 08 29, 08 2C, 08 02`) while party mode is being changed.
Note: the `01`/`81` traffic belongs to node `01` (a device in the freezer, see `05-bus-roles.md`), not to the box. The box
reads with `02`/`82`.

### PartyMode also switches SuperFrost on/off (done by the freezer, not the box)
Only the `08/02` write is on the wire. Afterwards the `81` status reads `0C/00 = 04` (enable) or `01` (disable).
The README titles say the same ("auto-activates / auto-disables superfrost"). **[ok]**

### BottleTimer: NOT SURE
Enabling the BottleTimer (41 min) puts exactly the SuperFrost-on command on the wire (`0C/00 = 04`, identical bytes);
disabling puts the SuperFrost-off command on the wire. I found **no frame that carries the 41 minutes** or any
bottle-timer flag, in either capture. Likely the timer lives in the SmartDevice box / cloud and the box merely sets
SuperFrost and switches it off when the timer expires. Needs confirmation (see questions below). **[?]**

## Door state and door-open alarm (freezer -> box, not settable)
No command; the freezer reports it. Captures: `door_short_open` (open, closed again quickly, no alarm),
`door_short_closed`, `door_alarm_on`, `door_alarm_off` (alarm ends + door closed) and the long `door_open_alarm`.
You said the alarm started/ended "somewhere in the middle" of the two alarm captures and that the freezer beeps; the
event flag below sits at 2.17 s of 4.3 s (alarm on) and 1.45 s of 3.3 s (alarm off), which matches.

### Door open / closed **[ok]**
| Signal | Door open | Door closed |
|--------|-----------|-------------|
| param `0C/31` in the `01`/`81` poll | `01` | `00` |
| broadcast by node `0B`: `00 0B 80 0B 24 01 04 14 04 20 xx` | `01` (`... DA 11`) | `00` (`... FB 01`) |
| then node `0B` to the freezer: `FF 0B 24 01 05 14 00 40 xx xx` | `01 00` | `00 00` |
| status byte `58/07` pushed by node `0B` | `01` | `11` |

### Door-open alarm (> 1 minute) **[ok on signalling, hyp on the exact wording]**
Sequence in `door_alarm_on` and in the long capture:
1. Door is open, `0C/31 = 01`, `0C/09 = 00`.
2. The freezer sets the **event-flag block** `00/00 = 00000100`, `00/01 = 08000000` (one `FF 80 7F 24 01 20 ...` frame).
3. Node `01` (not the box) reads 18 params with an `01` request (`0C/4C 47 09 11 0D 0F 46 4B 12 2E 0C 2F 0E 30 45 58 49 0B`).
4. The `81` reply has **`0C/09 = 01`**: alarm active (the freezer is beeping).
When the alarm ends (door closed, `door_alarm_off`, long capture at 6.87 s) the same flag block appears again and the
same read returns **`0C/09 = 00`**. So: `0C/09 = 01` is "door open longer than a minute" and the flag word
`00000100`/`08000000` is the notification that it changed. The door alone (short capture) never sets `0C/09` or the flag.
The group-`0x24` variants A/B (`24/00`, `24/06`) are **not** the alarm: they also appear in both short captures.
Everything else in the 18 param read (`0C/4C`, `47`, `11`, `0D`, ... ) stayed 0, probably other alarms (temperature, ...).

## Open questions
1. BottleTimer stays out of scope (probably implemented in the cloud/app).
2. Which of the other 17 params in the alarm read are other alarm types? Needs captures of those alarms.
3. A write gets no reply slot (`05-bus-roles.md` section 5); after a write only the event flags change.
4. The box does not send the leading break; the freezer does. The box answers ~0.4 ms after the slot byte `02`.
