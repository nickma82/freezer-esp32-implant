# Findings from the freezer emulator, to integrate here

Source: bench work of 2026-11-05 with `../esp-freezer-emulator-poc` (an ESP8266 playing the **freezer**, node `7F`, towards
the **real SmartDeviceBox**, no freezer attached). The box accepted it: full start-up, SuperFrost and set-point from the
app, temperature and door alarm shown in the app. Everything below was observed there; protocol details are also in
`../01-protocol.md` §3, `../02-commands.md`, `../03-bootup.md`. The raw logs stay local (`../esp-freezer-emulator-poc/logs/`,
git-ignored: they contain the WiFi name, IP and MAC the box writes to the bus).

## 1. Hardware: transmitting from an ESP8266 without a MOSFET [ok, analyzer-verified]
- **TX = UART1 TXD on GPIO2 (D4), open-drain, 47 Ω to the bus; 4.7 kΩ pull-up to 3V3.** Open-drain must be set *after*
  routing, because `pinMode()` clears it: `pinMode(2, SPECIAL); GPC(2) |= (1 << GPCD);` (see `bus_master.cpp` `begin()`).
  Measured with the stock `Serial1.begin(57102)` divisor: bit 17.50 µs, clean edges, no glitches.
  Push-pull is not an option: the freezer and the box share the wire.
- **RX = UART0 RXD swapped to GPIO13 (D7)** through 4.7 kΩ, as in phase 1. It also hears our own TX (echo).
- **Do not put D8 (GPIO15) on the bus.** It is a boot strap pin (must be low at reset); the bus pull-up through 47 Ω
  pulls it high, and the board neither boots nor flashes. GPIO2 must be high at reset, which the pull-up guarantees.
- **The phase-1 console jumper D4 ↔ TX must go** once D4 is the bus TX: it shorts the bus to the console output (seen as
  `FF EF` on the bus every 10 s, console silent).
- At every reset the boot ROM writes ~30 ms of 74880-baud text on GPIO2, i.e. onto the bus (longest low 80 µs, never a
  BREAK), then one 4.7 µs glitch. Harmless so far. The onboard blue LED (GPIO2) flickers with bus traffic.
- UART TX FIFO is 128 bytes; the longest freezer reply is 134 bytes (slot + body). Refill while sending
  (`while (((USS(1) >> USTXC) & 0xFF) >= 127) {}`). The box's own longest message is ~88 bytes (`03` list of 27 records).

## 2. Console and diagnostics [ok]
- With UART0 swapped, the console is **SoftwareSerial on GPIO1/GPIO3**. Every TX byte blocks interrupts
  (`enableIntTx(false)`, needed against the UART0 ISR), and a key arriving then is lost. Measured: at **115200** 16-18 of
  20 keys/lines arrived, at **57600** 20/20. Use 57600, print little by default (a "quiet" mode that hides routine reads,
  replies and idle acks), and pause printing ~300 ms after a received key.
- Ignore non-printable bytes in the line buffer, otherwise one noise byte (port open/close) blocks all single-key commands.
- The core's crash dump goes to U0TXD = the unused GPIO15: print `ESP.getResetInfoPtr()` (exccause, epc1..3, excvaddr)
  in the banner and decode with `xtensa-lx106-elf-addr2line` (`just crash <addr>` in the freezer emulator). One unexplained
  `Exception` reset happened in ~2 h of running; not reproduced yet.
- Echo check: a slot is only closed by the **next** BREAK, so the echo of what we sent arrives one slot later. Keep a
  small queue of expected echoes and re-sync on the slot byte (a glitch otherwise shifts it for good).
- Unplugging the box's bus/GND while running disturbed the NodeMCU's USB link (CP2102 dead until replugged). Switch the
  box's supply instead.

## 3. What the real box does as node `02` (to copy) [ok, bench logs `box_run6.txt` and earlier]
- **Start-up after power-on** (same order as `../03-bootup.md`): hello reply `11` (`03 01 00 00 00 00`), acks of `21`/`22`,
  table `02` (`13 00 00 00`, freezer answers `13 00 00 04`), read `48/11 48/01 08/07 08/57`, write `48/00 = 03` +
  `48/02..20`, `4C/01..03`, `48/10/12/0C/0D`, read `04/00 40/06`, **version requests to `7F` (`FF 02 06`) and to node `01`
  (`81 02 06`)**, the `08` lists, the `0C` lists, `3C` identification; ~15 s later the `<ip>@<ssid>` text + `48/14`, then
  `48/00 = 04`, ~5 s later `48/00 = 05` (twice) and a read-back of `48/00`.
- **Steady state** (every slot `02`, 570 ms): cycles through the `08/15` list (17 entries), `0C/15` list (20), `08/14`
  list (**22**), `0C/15` short list (17), `0C/14` list (27), `0C/3B` list (6), `0C/45` list (12), in between the idle ack
  `FF 02 30 01 02 00 00 0B 3A`. Answer pause: **424-430 µs** after the slot byte (freezer: 250 µs).
- **Group `48` written by the box**: `48/00` connection state `03` → `04` → **`05`** (new); `48/03` small flag (`00`, `01`,
  `03` seen); `48/04..0B` = `<ip>@<ssid>` text, or the SSID alone right after start; `48/14` = IP as little-endian 32 bit;
  `48/0C..0D` = 6 bytes, the box MAC; `48/10 = 02`, `48/12 = 00 00 01 03`; `4C/01` = `84 8C B3 A0 24` (constant for this
  box), `4C/02/03` = 0. A replacement box should write plausible values in the same order.
- **Commands from the app arrive as exactly the frames in `../02-commands.md`** (SuperFrost on/off, set-point; verified
  byte for byte incl. CRC). Party mode not bench-tested yet.
- **Freezer → app path**: after an event block with flags (`01000000/08000000` "value changed", `00000100/08000000`
  door alarm) the box re-reads, and the app shows the new `0C/14` temperature, the door alarm (`0C/09 = 01`, `0C/31 = 01`)
  and its end (`00`). So a replacement box must watch the event block and re-read the affected params.

## 4. What the freezer answers (useful as test vectors) [ok]
- `../esp-freezer-emulator-poc/lib/freezer_master/captured_params.h`: **285** answers of the captured freezer, keyed by
  **(grp, id, requested flag)** (`0C/01 20` → `03`, `0C/01 21` → `00`; `08/90` asked with `40` is answered with `20`).
  Regenerate with `just params` there. Many params are answered `00` ("not available") by the real freezer itself.
- `08/04` and `08/2A` are 32-bit little-endian **seconds counters** (+1/s); `08/20` drifts. A host-side freezer model
  should tick them.
- `fm::Master` (`../esp-freezer-emulator-poc/lib/freezer_master/`) is chip-independent C++17 with host tests: it can serve
  as the simulated freezer in this project's native tests (schedule, handshake, read replies, event blocks).

## 5. Original box behaviour NOT to copy [ok observation, exact timeout ?]
- If the freezer side disappears for **minutes** (between 30 s and 4 min; 8 s and 30 s are fine), the original box comes
  back **degraded**: handshake and `48`/`4C` writes only, then only a short 17-entry `08/14` list, no `0C` reads, and the
  app reports "Something went wrong" for every command. Nothing on the bus repairs it (hello restart, all event flags,
  `48/00` reset, a bus-low "power-off"); only a box power cycle does. Same if its start-up reads are answered `00`.
- A replacement box should instead **redo its start-up reads whenever the freezer's hello appears again**.

## 6. Bench partner
The freezer emulator replaces the freezer on the bench (two NodeMCUs on one wire, one pull-up). It starts its master at
boot, logs every slot at 57600, and has console commands for testing a box: `t <degC>` temperature, `a 1|0` door alarm,
`p <grp id flag value>` any param, `e <w0> <w1>` event flags, `x`/`X` inject messages, `L <ms>` hold the bus low
(freezer power-off), `k` list everything the box read/wrote. A set-point written by the box moves the simulated
temperature 1 °C every 15 s towards it.

## 7. Checklist for this project
- [ ] Phase 2 TX stage: D4/GPIO2 open-drain UART1 + 47 Ω (section 1) instead of D8; remove the D4 ↔ TX jumper.
- [ ] Console: SoftwareSerial on GPIO1/GPIO3 at 57600, quiet default, RX hold, printable-only line buffer (section 2).
- [ ] Port from `../esp-freezer-emulator-poc/lib/bus_master_esp8266/`: UART1 open-drain init, FIFO refill, echo queue
      with re-sync; drop the freezer-only BREAK generation (the box never sends a BREAK).
- [ ] Crash registers in the banner (section 2).
- [ ] Start-up and steady-state behaviour of section 3, incl. `48/00` = `03` → `04` → `05` and the idle ack.
- [ ] Re-run the start-up reads after a freezer outage (section 5).
- [ ] Native tests against `fm::Master` / `captured_params.h` (section 4).
- [ ] Bench test against the freezer emulator before the real freezer (section 6).
