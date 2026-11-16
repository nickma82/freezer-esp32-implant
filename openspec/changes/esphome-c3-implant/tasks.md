## 1. Board doc and hardware requirements corrections

- [x] 1.1 `docs/dev-boards/esp32-c3-supermini/README.md` section A:
  - bare ESP32-C3 QFN with in-package flash (marking `FH4…`), not a module; 40 MHz crystal; silkscreen `V1601`;
  - regulator `73L33` (SOT-89-like, not the schematic's ME6211);
  - Schottky "S" VBUS→VSYS;
  - two LEDs: red power, and blue on GPIO8, active-low via 5.1 kΩ;
  - tag each item as photo/schematic evidence.
- [x] 1.2 Section B: native USB-Serial/JTAG confirmed by the schematic (D-/D+ → GPIO18/19) and the photo (no bridge IC).
  The listing's BOOT+RST sequence is the generic strap method. Keep `ttyACM*`/`esptool` as the confirming bench item.
  Rename "USB CDC" to USB-Serial/JTAG (ESPHome `hardware_uart: USB_SERIAL_JTAG`).
- [x] 1.3 Section C pad table: physical order from the silkscreen (left from USB: `5V, G, 3.3, 4, 3, 2, 1, 0`; right:
  `5, 6, 7, 8, 9, 10, 20, 21`). IO4–IO7 = JTAG (MTMS/MTDI/MTCK/MTDO), not boot-related. Schematic 10 kΩ pull-ups on
  GPIO8, GPIO9 and CHIP_EN. ADC2 (GPIO5) not usable with Wi-Fi.
- [x] 1.4 Sections D/E/F:
  - BLE 5.0;
  - drop H2 (sending a BREAK) as a win for the implant, since the freezer sends every BREAK; keep it for the freezer emulator only;
  - answer window 250–689 µs observed;
  - bus on UART1 (not UART0), TX+RX on GPIO3 open-drain, 100 Ω series;
  - note that `71RSgyqvxoL` shows an RP2040-Zero pinout in its top half.
- [x] 1.5 `photos/README.md`: list the existing photos and what each shows; rename the `signal-*` files to `bottom.jpg` /
  `top.jpg` only if the user agrees.
- [x] 1.6 `docs/hardware-requirements.md`, without losing the user's uncommitted edits:
  - "box is master and polls every ~285 ms" → the freezer schedules, node `02` answers slot `02` every 570 ms;
  - H2 "not needed for node 02";
  - H6 rewritten as the 250–600 µs answer window;
  - H17 resolved (native USB).

## 2. Project skeleton

- [x] 2.1 Create `src/esp32-c3-supermini--esphome/` with:
  - `justfile` recipes `build`, `flash`, `logs`, `test`, `check-core`, `clean`;
  - ESPHome run as `uvx esphome==<pinned>`, the newest release at least two weeks old;
  - recipes written as functions/best-practice shell.
- [x] 2.2 `freezer-implant.yaml`:
  - `esp32: board: esp32-c3-devkitm-1, framework: esp-idf`;
  - `logger: hardware_uart: USB_SERIAL_JTAG`;
  - `wifi`, `api` (encryption), `ota`;
  - `external_components` (local).
- [x] 2.3 `secrets.yaml.example` and `.gitignore` for `secrets.yaml`, `.esphome/` and build output.
- [x] 2.4 `components/freezer_bus/` with relative symlinks to `protocol-decoding/esp-smartdevice-emulator-poc/lib/freezer_bus/freezer_bus.{h,cpp}`.
  `just check-core` verifies that the build tree copy equals the source.
- [x] 2.5 Smoke build: an empty component compiles. `just build` passes.

## 3. Node-02 engine (host-tested)

- [x] 3.1 Engine skeleton: frame input with a µs timestamp, `next_answer()` for slot `02`, a queue with priorities
  handshake > write > read > idle, and a command and state-snapshot interface.
- [x] 3.2 Handshake: hello → `11`, `21`/`22`/`23` → acks, table request `13 00 00 00` and the `12` answer. Unit tests
  against the frames in `03-bootup.md`.
- [x] 3.3 Start-up: version requests to `7F` and node `01` (2 s timeout), `3C` paged identity reads and string assembly,
  first status read.
- [x] 3.4 Group `48` writes (D7): text chunking, IP LE32, MAC, `03`→`04`→`05` timing, rewrite on IP change, option off.
- [x] 3.5 State model and status read (D6): the eight params with their captured request flags, flag `00` = unknown,
  10 s period, a re-read on non-zero event flags, passive updates from `81` replies and node `0B`'s `14/04`.
- [x] 3.6 Writes: SuperFrost, party and set-point (−28…−15). Bodies byte-identical to `02-commands.md`. Refusal rules.
  Confirmation via event plus read-back within 3 s, no resend.
- [x] 3.7 Recovery: a hello in any state restarts start-up and clears the queue. Bus-alive tracking (2 s).
- [x] 3.8 Host test harness (`just test`) and the closed loop against `fm::Master` (D9): handshake → running, status equals
  `captured_params.h`, write reaches the master's table, event re-read, master reset → restart. Also replay captured
  `csv/*_hex.csv` door/alarm frames for the passive paths.

## 4. ESP32-C3 bus driver

- [x] 4.1 UART1 at 57600 8N1 on the configured pin: input + open-drain output routed through the GPIO matrix,
  open-drain applied after routing, no internal pull-up (D4).
- [x] 4.2 Bus task: event-queue reader, BREAK/framing-error slot delimiting, dropping the break `00`, CRC check and counters.
- [x] 4.3 Reply path: pre-built answer, timed start (default 420 µs, configurable 250–600 µs), skip and count when late.
- [x] 4.4 Echo comparison during our own slot. Auto-disable TX on mismatch, with a reason.
- [x] 4.5 Latency statistics (median and max over a window), bus-alive flag.
- [ ] 4.6 Bench latency check under Wi-Fi load (API log streaming plus an OTA upload). If any answer is > 600 µs or missed
  slots exceed 1 %, switch to the HAL ISR fallback (D4) and record the decision in `design.md`.

## 5. ESPHome component and entities

- [x] 5.1 `__init__.py` schema: `bus_pin` (rejects GPIO8/9/18/19), `reply_delay_us` (250–600), `status_interval`,
  `network_writes`. Codegen for the component.
- [x] 5.2 Component glue: start the driver in `setup()`, a thread-safe command queue, and a state snapshot published in `loop()`.
- [x] 5.3 Control entities: switches SuperFrost and Party mode, number Set-point (−28…−15, step 1). No optimistic state;
  snap back on refusal (D8).
- [x] 5.4 Status entities: temperature, power-failure temperature, door, door alarm, power-failure alarm, bus connected,
  freezer/node-01 versions, model. Unknown while the bus is down or start-up is incomplete.
- [x] 5.5 Diagnostics: Bus transmit switch (on at boot, follows auto-disable), start-up state, latency median/max, CRC/echo/missed counters.
- [x] 5.6 Logging (D10): INFO state changes and writes, DEBUG `M` lines compatible with `tools/compare.py`. No secrets in logs.
- [x] 5.7 `README.md`: purpose, entities table, `just` recipes, flashing via USB (BOOT held at reset if the port does not
  show up), configuration options.

## 6. Wiring documentation

- [x] 6.1 `WIRING.md` in the style of the PoC WIRING files:
  - ASCII diagram of SuperMini GPIO3 → 100 Ω → bus, GND common, logic analyzer CH0;
  - a pad table with the silkscreen names;
  - pins never to use (IO8, IO9, 20/21 reserved).
- [x] 6.2 Bench section: SuperMini ↔ NodeMCU freezer emulator (its D4 47 Ω TX, D7 4.7 kΩ RX, 4.7 kΩ pull-up to 3V3), both
  USB-powered from one host, common GND.
- [x] 6.3 Freezer section (for the follow-up): pad 2 data, GND, pads 1/3 never connected, pull-up only if H14 shows the
  freezer has none (footprint), supply still open (H12).
- [x] 6.4 Safety notes: measure GND offset first, power the board before connecting the bus, BOOT button must never be
  pressed while attached if IO9 were ever used (it is not).

## 7. Bench test against the freezer emulator

- [ ] 7.1 Board-doc checklist items 1, 4, 5 on our unit: port name, flash ID, LED pin, console. Record results in the board README.
- [ ] 7.2 Wire per `WIRING.md`. Start the emulator, boot the implant: the handshake completes, the emulator logs the box as
  running, HA shows all entities.
- [ ] 7.3 Logic 2 capture ≥ 60 s: every implant byte only in slot `02`, latency within 250–600 µs, valid CRCs
  (`protocol-decoding` `just slots`).
- [ ] 7.4 From HA: SuperFrost on/off, party on/off, set-point −17/−16 and one value outside −18…−16. The emulator logs
  byte-identical frames, and the entities follow the read-back.
- [ ] 7.5 Emulator console: `a 1`/`a 0` (door alarm), `t -20` (temperature), `p 0C 11 20 01` + `p 0C 12 40 00 C9` +
  `e 00000100 08000000` (power failure). HA reflects each within the spec times.
- [ ] 7.6 Recovery: emulator reset, `L 5000`, `L 30000`, and the emulator stopped for 5 min. The implant restarts start-up
  each time without its own reset, and the entities go unknown and then come back.
- [ ] 7.7 Safety: reset and re-flash the implant while attached. No low pulse from the implant on the analyzer. Switching
  Bus transmit off leaves slot `02` empty.
- [ ] 7.8 1 h soak with HA connected: no resets, missed slots < 0.1 %, zero echo mismatches.

## 8. Documentation follow-up

- [ ] 8.1 `protocol-decoding/01-protocol.md` §4: point to the implant and record bench findings (answer latency, the
  handshake being sufficient).
- [ ] 8.2 `protocol-decoding/potential-next-steps.md`: tick the emulator bench and list the next step (real freezer with the box unplugged).
- [x] 8.3 Repo `README.md` layout table: add `src/esp32-c3-supermini--esphome/`.
