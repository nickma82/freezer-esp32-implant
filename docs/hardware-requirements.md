# ESP32 hardware: requirements and board decision

Status: draft. Requirements come from `protocol-decoding/01-protocol.md` and `04-open-questions.md`.
**[ok]** = measured/captured, **[hyp]** = hypothesis, **[?]** = must be measured before ordering/finalizing.

Per-board specs, pinouts and measurement results live in `dev-boards/`; this file only holds the requirements and the
decision logic.

## The bus is not an exotic protocol

There is no public name for it. It is a proprietary **single-wire, half-duplex UART**:

- 57600 baud, 8N1, LSB first, idle high (3.3 V), open-drain with pull-up **[ok / hyp for open-drain]**
- every message starts with a **BREAK** (~18 bit times low, ~316 us), then the slot byte; the freezer sends both
- frame: `BREAK slot [payload] crc_lo crc_hi`, CRC-16/XMODEM **[ok]**
- the freezer (node `7F`) is the scheduler and sends every BREAK + slot byte; the box (node `02`, our implant's role) only answers slot `02`, every 570 ms

It looks LIN-like (break-delimited UART frames), but it is not LIN: no sync byte, no protected ID, CRC-16 instead of
a LIN checksum. Because it is plain UART plus break, every ESP32 variant can speak it. No special peripheral is needed.

## Requirements

| ID | Requirement | Why | Status |
| -- | ----------- | --- | ------ |
| H1 | 1 UART, 57600 8N1, any GPIO via the GPIO matrix | the bus is plain UART | [ok] |
| H2 | Generate a BREAK: **not needed for node `02`**, only for a freezer emulator (ESP-IDF: `uart_write_bytes_with_break`) | the freezer sends every BREAK | [hyp] works, test on bench (emulator only) |
| H3 | Detect a BREAK on RX (ESP-IDF: `UART_BREAK` / framing-error event) | needed to frame the freezer's replies | [hyp] |
| H4 | TX as **open-drain** on the same pin as RX (`GPIO_MODE_INPUT_OUTPUT_OD`), reading back our own transmission | line is shared and only pulled low; push-pull could fight the freezer MCU | [hyp] |
| H5 | 3.3 V GPIO, no level shifter | idle level 3.2-3.3 V | [ok] |
| H6 | Timing: answer slot `02` within the 250-600 us window after the slot byte (observed on the bus: 250-689 us); trivial for any ESP32 | CPU load is negligible | [ok] |
| H7 | Wi-Fi 2.4 GHz (+ BLE optional), MQTT or ESPHome API | talk to Home Assistant locally | goal |
| H8 | Fits where the SmartDeviceBox sits (size budget TBD) | the user wants it inside the appliance | [?] measure cavity |
| H9 | Operating range covers the install location: -40..+85 C module rating; the freezer interior is about -18 C | ESP32 modules are rated -40 C, but cheap dev boards use unrated parts (LDO, USB chip, LED) | [?] pick a board with rated parts or a bare module |
| H10 | Condensation / frost tolerant: conformal coat, no exposed USB port in service | humidity cycling inside a freezer | [?] |
| H11 | Antenna: external/IPEX or a position outside the metal cabinet | steel cabinet attenuates Wi-Fi heavily | [?] test RSSI |
| H12 | Supply: know what pin 1/3 provide (3.3 V on pin 3 **[hyp]**) and the available current; Wi-Fi TX peaks 250-350 mA | do not brown out the freezer's supply | [?] measure |
| H13 | Never drive pins 1 and 3 until understood | possible supply/strap lines | [ok] |
| H14 | Pull-up on the data line: the freezer may or may not have one | decides whether we must add ~4.7-10 k to 3.3 V | [?] measure |
| H15 | Series resistor (~100-330 ohm) on the data pin, ESD protection if it leaves the enclosure | protects the GPIO and the freezer MCU | design |
| H16 | Supported by ESPHome (ESP-IDF framework) for the later HA integration | avoids writing the Wi-Fi/HA side | goal |
| H17 | Native USB or a USB-UART bridge for flashing, accessible during development | iteration speed | design; SuperMini: native USB-Serial/JTAG, no bridge [schematic/photo], port still to be confirmed (`ls /dev/ttyACM*`) |

## Decision logic

1. **Protocol support filters nothing**: H1-H6 hold for ESP32, S2, S3, C3, C6, H2. Do not choose on this.
2. **Hard filters**: Wi-Fi 2.4 GHz (H7), ESPHome support (H16), 3.3 V I/O (H5), small enough (H8). This leaves
   ESP32-C3, C6, S3 and the classic ESP32.
3. **Size and cost break the tie**: single-core RISC-V C3/C6 are the smallest and cheapest, and the workload is tiny.
   S3/classic ESP32 only add size, power and heat.
4. **Install conditions (H9-H12) decide the board form**, not the chip:
   - prefer a board with an **external antenna connector** (IPEX) so the antenna can sit outside the metal box
   - prefer a **bare module on a small custom PCB** (e.g. ESP32-C3-MINI-1U / C6-MINI-1U) for the final unit, since
     all its parts are rated to -40 C and the PCB can be coated
   - the dev board is for the bench only
5. **Pick C3 vs C6**: C6 adds Wi-Fi 6, Thread/Zigbee and Matter, costs about the same, and is the newer ESPHome target.
   C3 has the longer track record. Neither is needed by the protocol. Recommendation below.

## Candidates

| Board | Size (mm) | Antenna | Notes |
| ----- | --------- | ------- | ----- |
| [ESP32-C3 SuperMini](dev-boards/esp32-c3-supermini/README.md) | 22.52 x 18 | ceramic chip antenna (CrossAir CA-C03) | **6 in hand, bench now.** Cheapest and smallest; the antenna sits too close to the ground plane, so weak Wi-Fi - fine on the bench, poor inside a steel cabinet |
| Seeed XIAO ESP32-C3 | ~21 x 17.8 | IPEX connector | small, good RF, castellated pads, common. **First prototype if the SuperMini's RSSI is not enough** |
| Seeed XIAO ESP32-C6 | ~21 x 17.8 | IPEX connector | same footprint, Wi-Fi 6 + 802.15.4 |
| ESP32-C3-DevKitM-1 | ~25 x 55 | PCB antenna | official, reliable, too large for installation |
| ESP32-C3-MINI-1U / C6-MINI-1U (bare module) | 13.2 x 12.5 | IPEX | **final unit** on a custom PCB |

## Recommendation

- **Bench now**: the ESP32-C3 SuperMini we already have. Specs, pin allocation and the open measurements are in
  [`dev-boards/esp32-c3-supermini/README.md`](dev-boards/esp32-c3-supermini/README.md); GPIO3 is the bus pin, the console
  runs over USB, and IO8/IO9 stay untouched (straps, and IO9 is the BOOT button, which would look like a BREAK if the bus were on it).
- **Prototype step**: Seeed XIAO ESP32-C3 (or C6 if you want Thread/Matter later) as soon as the antenna question (H11)
  matters, because it has an IPEX connector.
- **Final**: a small custom PCB with a C3/C6-MINI-1U module, 3.3 V from the appliance (after H12 is measured), open-drain data
  line with footprint for an optional pull-up (H14), external antenna.
- Avoid the SuperMini for the installed unit.

## To measure before finalizing

1. Is the data line pulled up by the freezer? (scope one rising edge; resistance to 3.3 V with the box removed) -> H4, H14
2. Voltage and available current on pin 3 (and pin 1) under load -> H12
3. Cavity dimensions where the box sits -> H8
4. Wi-Fi RSSI from the install location with the SuperMini's chip antenna vs an external one -> H11
5. Break length tolerance and the mark-after-break (`04-open-questions.md` #11) -> H2
6. Bench: send a captured `SUPERFROST_ON` frame from the ESP32 with the box disconnected (`04-open-questions.md` #1)
7. Board-level checks on the SuperMini before it goes near the freezer: USB bridge or native USB, BREAK generation and
   detection on its UART, regulator current, whether the `5V` pad clashes with USB -> items 1-3 and 6-12 in
   [`dev-boards/esp32-c3-supermini/README.md`](dev-boards/esp32-c3-supermini/README.md)
