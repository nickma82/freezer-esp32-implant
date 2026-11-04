# Open questions, gaps and feasibility

Status after 16 captures (incl. the full bootup, see `03-bootup.md`). See `01-protocol.md` (spec), `02-commands.md` (commands),
`05-bus-roles.md` (who schedules and who answers), `src/protocol.py` (code), `justfile` (entrypoint), `tests/` (68 tests).

**Update 2026-10-30:** the freezer is the scheduler, the box is the slave node `02`. Questions 2, 3, 11 and 12 are answered
or changed by that (marked below).

## Can we build a parser and a generator?
**Parser: yes, for everything observed.** `src/protocol.py` + `tests/test_captures.py` over all captures (`just test`):
- 1,336 bare messages (`0C`/`7E`/`7F`), 451 acks, 85 read-request lists, 217 value messages: **0 parse errors**,
  CRC correct on every complete frame (1 failure = a frame cut off by the end of a capture), and
  `encode(decode(frame)) == frame` for every frame (byte exact).
- `interpret()` turns known parameters and the event flags into a readable timeline (`just timeline csv/<file>`), e.g. for
  `door_alarm_on`: `door_open=True` ... `event=door_alarm_changed` (2.17 s) ... `door_alarm=True` (2.56 s).
- What it can name: mode (normal/SuperFrost), party mode, set-point, current temperature, door open, door-open alarm,
  and the three event-flag pairs. Everything else is parsed structurally (group/id/value) but has no meaning yet.

**Generator: yes for the writes we captured; a bus *master* needs more.**
- Byte-exact reproduction of 7 captured frames: SuperFrost on/off, party on/off, set-point -17/-16, and the 7-param
  read request. `write_param()` builds any other single-parameter write in the same format; the CRC is solved.
- Not proven: that the freezer *accepts* a frame we generate. Needs a test on the bench (see below). Writes for
  parameters we have never seen (other set-points, other modes) are extrapolations from the frame format.
- Missing for a real transmitter: electrical side and timing (below). The replacement does **not** have to be a poll
  master: the freezer sends every BREAK and slot byte, and the box only answers in slot `02` (every 570 ms, within
  ~0.4 ms). The generator's frames include the slot byte `02`; on the wire the box sends only the bytes after it.

## Open questions (priority order)
1. **Does the freezer accept a generated write?** Test: with the box disconnected, wait for `BREAK` + slot byte `02`
   and answer with the body of `SUPERFROST_ON` (everything after the `02`) within ~0.4 ms; watch for the event flag
   `value_changed` and `0C/00 = 04`. Needs confirming: answer-window tolerance, 3.3 V logic level, half-duplex handling,
   and whether the freezer requires the start-up handshake first.
2. ~~**Who is the bus master and what is the poll schedule?**~~ **Answered** (`05-bus-roles.md`): the freezer (node `7F`)
   sends every BREAK and slot byte (clock fingerprint); `0C`, `7E`, `7F` are slots nobody answers; the box owns slot `02`.
   Still open: what slots `7E`/`7F` are for, and what node `01`/`0B` physically is (display board?).
   Hint kept: in the -17 to -16 set-point capture the box wrote twice, about 0.6 s apart (= one box slot of 570 ms); the first write re-sent -17 unchanged, the second sent -16.
3. **Reply to a write.** Answered in part: the freezer opens no reply slot for a write (slot `82` only follows reads).
   The only confirmation is the event flag and the next read.
4. **Meaning of the flag words** in the 1 s event block (`00/00`, `00/01`, `00/02`, `00/03`). Only three combinations seen.
   Bit assignments for other events (temperature alarm, power failure, door alarm cleared, ...) are unknown.
5. **Other alarms.** The alarm read lists 18 params (`0C/4C 47 09 11 0D 0F 46 4B 12 2E 0C 2F 0E 30 45 58 49 0B`); only
   `0C/09` (door) changed. Capture the other alarms (temperature too high, power failure, ...) to name the rest.
6. **Group `24` and `58` records.** `24/00,06,0F,10,12,05` alternate between `0000`/`01` and `FFFF`/`11` while the door is
   open and are not the door alarm; `58/00..0C` changes between open (`58/02=0000`, `58/0C=00`) and closed
   (`FFFF`, `64`). Meaning unknown. `58/07` (status byte of the `0B` poll) is `01` with door open, `11` otherwise.
7. **Parameters without a name:** `0C/07` (`FF9A` / `0167`), `0C/2B`, `0C/10`, `0C/08`, `0C/14` high byte (`00`/`30`/`50`/`70`),
   the 18/41/62-parameter dumps (`08/xx`, `0C/xx`, `04/xx`, `60/xx`) and the 4-byte values in them.
   Strategy: capture a state change per feature (display brightness, units, vacation mode, ...) and diff the dumps.
8. **Temperature encoding.** Set-point and current temperature are `00 <int8 degC>`; confirmed only for -18..-16. Check
   half-degrees, positive values, the allowed range, and what the high byte means.
9. **BottleTimer.** No dedicated bus command; enabling/disabling puts the SuperFrost on/off writes on the bus. Likely cloud/box
   side. Out of scope on request.
10. **Cabling.** Pins 1 (0 V) and 3 (3.3 V) never changed; they are probably supply/strap. Confirm before connecting a
    transmitter (do not drive them).
11. ~~**Break tolerance.**~~ Not needed: the box never sends a break. Replaced by: **answer-window tolerance**. The box answers
    426 us after the start of the slot byte, node `01`/`0B` after 604 us, or 648-689 us with edges up to 1.6 us off
    in its low-power mode, and the freezer accepts all of these (`05-bus-roles.md` §2). So the window is at least
    ~430-690 us, at least for slots `01`/`0B`. How late may node `02` start before the freezer ignores it? Must the
    answer be finished before the next slot (~17.8 ms)?
12. ~~**Unverified assumptions.**~~ **Answered:** the `0x80` bit marks a destination (`dst = 0x80 | node`), not a reply. The
    `0B` "exception" is node `0B` sending to the freezer (`FF`) or broadcasting (`80`). Every frame in every capture fits
    (`tests/test_bus_roles.py`).
13. **Is node 7F physically the freezer and node 01/0B inside the freezer?** Strong evidence, not proven. See the cheap
    tests in `potential-next-steps.md`. Settled meanwhile: node `01`/`0B` is a separate MCU from node `7F` (it changes
    its clock while `7F` does not) and runs a low-power clock while the door stays closed, waking on the door (all 16
    captures, `05-bus-roles.md` §2; timeout 5-6 s after the door closes). That fits a display/door board in the freezer.
14. **Does the freezer need the start-up handshake from node 02** (hello reply `03 01 00 00 00 00`, table `13 00 00 00`,
    acks to `21`/`22`/`23`) before it accepts writes? Observe by plugging the replacement in while the freezer runs.

## Known limits of the data
- 14 short captures (2-9 s), one freezer, one firmware. No temperature changes, no power-up sequence, no error states.
- CSVs were decoded with Logic's Async Serial analyzer; the break shows as a framing error `0x00`. For a bit-exact
  transmitter use `raw-captures/digital.csv` style exports, not the decoded CSV.
- Raw edges exist for the first 3.2 s capture (`digital.csv`) and the full bootup (`freezer-bootup-fullduration.csv`);
  the clock fingerprint in `05-bus-roles.md` rests on both. The other captures are decoded exports only. All captures are digital only (24 MS/s), so they cannot show which device pulls the line low (needs an analog capture).
- Exporting raw data from the `.sal` files through the Logic 2 MCP crashes Logic 2.4.48; use the GUI export.

## Added by the bootup capture
- **Device IDs `<box-id-1>` / `<box-id-2>`:** not on the bus. Only freezer numbers (`1234 56789`, `9935672-01`, `9876 54321`) appear. Open: where the IDs come from (box firmware, cloud).
- **Startup handshake** (services `01`/`11` hello, `02`/`12` table, `06`/`16` version, `21`-`23` init): pairing understood
  (`05-bus-roles.md` section 6), purpose of each step unknown; see question 14.
- **Meaning of groups `48` (state values, MAC guess) and `4C`**. The two `SW` versions are answered: `1-86-7` = freezer, `6-2-0` = node `01`.
