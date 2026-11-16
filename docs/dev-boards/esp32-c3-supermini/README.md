# ESP32-C3 SuperMini: board spec sheet

Status: partly checked against photos of our unit and the vendor schematic, not yet measured on the bench.
**[ok]** = measured/confirmed on *our* unit, **[photo]** = read from our photos in `photos/`, **[schematic]** = from the
vendor schematic image (`photos/71YZVbTXyKL._AC_SL1500_.jpg`; a generic SuperMini schematic, our unit differs in at
least the regulator), **[src]** = datasheet or web source, not verified against the physical board, **[?]** = must be
checked on our unit before it is used in a build.

Requirement IDs (`H1`-`H17`) refer to `../../hardware-requirements.md`. Protocol references are relative to
`protocol-decoding/`.

## A. Our unit

| Item | Value | Status |
| --- | --- | --- |
| Purchase | Gerui 6-pack, amazon.de, ASIN `B0F4MXZ7Q3` | [ok] |
| Sold as | "ESP32 C3 SuperMini WiFi Bluetooth", generic clone | [src] |
| SoC | bare ESP32-C3 QFN chip, not a module; marking line starts `FH4` = **ESP32-C3FH4** (single-core RISC-V, 160 MHz, 4 MB flash in the package) | [photo] |
| Flash | 4 MB, inside the chip package (no separate flash chip visible) | [photo], size [?] until `flash_id` |
| Crystal | 40 MHz, marked `40.000 MHz` | [photo] |
| PCB | silkscreen `V1601`, `ESP32-C3`, `Super Mini`; 16 pads (2 x 8), 2.54 mm pitch | [photo] |
| Board size | 22.52 x 18 mm | [src] |
| USB | USB-C, native USB-Serial/JTAG, **no bridge chip** (see B) | [ok], [schematic], [photo] |
| Regulator | marked `73L33`, 3-pin SOT-89-like package next to the left pad row. The schematic shows an ME6211 in SOT-23-5, so our unit differs. Current rating unknown | [photo], rating [?] |
| VBUS diode | Schottky marked `S` (schematic: BAT60J), VBUS -> VSYS (the `5V` pad side) | [photo], [schematic] |
| Buttons | `BOOT` (GPIO9) and `RST` (CHIP_EN) | [photo], [schematic] |
| LEDs | red power LED (always on, not GPIO driven); blue user LED on **GPIO8, active low** (schematic: 3V3 -> 5.1 kOhm -> LED -> GPIO8, so the pin must pull it low to light it) | [photo], [schematic], GPIO [?] until blinked |
| Antenna | ceramic chip antenna, on-board (red-tinted pad area at the board end in the photo); part number CA-C03 only from web sources | [photo], [src] |

Photos: see `photos/README.md`. Still wanted: a close-up of the `73L33` package and the chip marking (to read the full
line and the date code).

## B. USB: native USB-Serial/JTAG (confirmed on our unit)

The schematic wires USB D-/D+ directly to GPIO18/19 (`USBD_N`/`USBD_P`), and the photo shows no USB-UART bridge IC
(no CH340/CP2102 footprint near the USB-C connector). The ESP32-C3 has USB-Serial/JTAG built in, so the port is a CDC-ACM
device. **[schematic], [photo]**

The listing's "hold BOOT, press RESET, release RESET, release BOOT" is the generic ESP32 strap method for download mode
(GPIO9 low at reset). It does not imply a bridge chip.

| Where | Name for the same thing |
| --- | --- |
| ESPHome | `logger: hardware_uart: USB_SERIAL_JTAG` |
| Arduino IDE | "USB CDC On Boot: Enabled" (`HWCDC`) |
| Linux | `/dev/ttyACM*` |

Bench confirmation **[ok]** (2026-11-08): our unit enumerates as USB `303a:1001` (Espressif USB-Serial/JTAG) on
`/dev/ttyACM0`; esptool flashed and verified the freezer-implant firmware over it, and the ESPHome log arrives on the same
port. A blank board (as delivered) boot-loops with `invalid header: 0xffffffff` / `TG0WDT_SYS_RST` and re-enumerates
every ~3 s; that is normal for empty flash, not a fault. Flash size/model from `esptool flash_id` still to note.
`ttyACM*` = native, `ttyUSB*` would mean a bridge after all. Consequence: GPIO20/21 (UART0) are not used by the console
and stay free.

Also unverified: that the blue LED really is on GPIO8 (blink it) **[?]**.

## C. Pinout

Silkscreen from our bottom-side photo **[photo]**, GPIO duties from the ESP32-C3 datasheet and the schematic. Pads are
counted from the **USB end** of the board, in each column. `L1..L8` = left column, `R1..R8` = right column, with the
bottom side (silkscreen side) facing you and the USB-C connector at the top. Note that the top-side photo shows the same
board mirrored. Silkscreen on the bottom shows only the number (for GPIOs), `5V`, `G`, `3.3`.

| Pad | Silkscreen | GPIO | Duty on the ESP32-C3 | Use here |
| --- | --- | --- | --- | --- |
| L1 | `5V` | - | 5 V, to the regulator and the VBUS diode (listing: 3.3-6 V; not together with USB) | supply |
| L2 | `G` | - | ground | ground |
| L3 | `3.3` | - | regulator output | 3.3 V out if H12 allows |
| L4 | `4` | 4 | ADC1_CH4, SPI, PWM; **JTAG MTMS** | spare |
| L5 | `3` | 3 | plain GPIO, ADC1_CH3, PWM | **bus data** |
| L6 | `2` | 2 | ADC1_CH2, PWM; **strapping** (needs a defined level at reset) | avoid |
| L7 | `1` | 1 | plain GPIO, ADC1_CH1, PWM | spare |
| L8 | `0` | 0 | plain GPIO, ADC1_CH0, PWM | spare |
| R1 | `5` | 5 | ADC2_CH0 (**unusable while Wi-Fi is on**), SPI, PWM; **JTAG MTDI** | spare, no ADC |
| R2 | `6` | 6 | SPI, PWM; **JTAG MTCK** | spare |
| R3 | `7` | 7 | SPI, PWM; **JTAG MTDO** | spare |
| R4 | `8` | 8 | I2C SDA, PWM; **strapping**, 10 kOhm pull-up, blue LED (active low) | never |
| R5 | `9` | 9 | I2C SCL, PWM; **strapping, boot-mode select** (LOW at reset = UART download), `BOOT` button, 10 kOhm pull-up | never |
| R6 | `10` | 10 | plain GPIO, PWM | spare |
| R7 | `20` | 20 | UART0 RXD (default), ROM log | reserved |
| R8 | `21` | 21 | UART0 TXD (default), ROM log | reserved |

On-board pull-ups **[schematic]**: 10 kOhm on GPIO8, on GPIO9 (BOOT button) and on CHIP_EN (with an RC to ground).
GPIO2 is a strap pin without an on-board pull in the schematic, so leave it unconnected.

IO4-IO7 are the JTAG pins (MTMS/MTDI/MTCK/MTDO), not boot-related. Free of boot/system duty: **IO0, IO1, IO3, IO10**.
GPIO18/19 (USB D-/D+) are wired to the USB-C connector, not to the pads. GPIO11-17 (flash) are not broken out.

Verify each pad with a continuity test against the silkscreen before relying on this table **[?]**.

## D. ESP32-C3 chip spec (relevant parts only)

| Item | Value |
| --- | --- |
| CPU | single-core 32-bit RISC-V, up to 160 MHz |
| Memory | 400 KB SRAM, 384 KB ROM, no PSRAM |
| Flash | 4 MB in the ESP32-C3FH4 package, QIO boot |
| Wi-Fi | 802.11 b/g/n, 2.4 GHz, STA / SoftAP / STA+SoftAP |
| Bluetooth | Bluetooth 5 (LE), certified 5.0 (the listing says 5.0; the 5.4 on espboards is not relevant here) |
| Peripherals | 2x UART, 1x I2C, 1x SPI, 6x LEDC (PWM), ADC1 5 ch + ADC2 (not usable with Wi-Fi), RMT, TWAI, temperature sensor, USB-Serial/JTAG |
| Pin routing | GPIO matrix: any GPIO can be UART TX and/or RX; USB is fixed on GPIO18/19 |
| Security | AES-128/256, SHA, RSA, HMAC, secure boot, flash encryption |
| Deep sleep | about 43 uA (chip level, vendor claim); the whole board with LDO and LEDs is higher [?] - irrelevant, the unit is permanently powered |
| Toolchain | ESP-IDF target `esp32c3`; Arduino core; PlatformIO/ESPHome board `esp32-c3-devkitm-1` |

Corrections to the Amazon listing: max frequency is **160 MHz** (the "150 MHz" sentence contradicts its own "160 MHz"
one), deep sleep is **43 uA** (the listing's "43MA" is a typo), and `27.178 x 38.227 x 1.524 mm` is the **packaging**,
the board is 22.52 x 18 mm. "Wi-Fi 4" is the Wi-Fi Alliance name for 802.11n.

## E. Fit against the project requirements

The freezer (node `7F`) sends every BREAK and slot byte; the implant is node `02` and only answers in slot `02`
(`protocol-decoding/05-bus-roles.md`). That shapes which requirements matter.

**Wins over the ESP8266 Amica used in phases 0/1** (`openspec/changes/esp-poc-phase1-listener/design.md` D1):

| Req | On ESP8266 | On ESP32-C3 |
| --- | --- | --- |
| H3 detect a BREAK on RX | framing error only, by hand | UART RX break / framing-error event |
| H4 open-drain, TX and RX on one pad | one UART, IO swap (`IOSWAP`) + `FUNCTION_4` hack | GPIO matrix: route a UART's TX *and* RX to the same GPIO, `GPIO_MODE_INPUT_OUTPUT_OD`; the echo read-back is then plain RX traffic |
| Boot straps | GPIO15 must be low at reset, which is why phase 2 kept it off | GPIO1 has no strap duty, so the MOSFET drive pin is a normal GPIO |
| H17 console | bit-banged `EspSoftwareSerial` on GPIO1/3 | native USB-Serial/JTAG, no pin used at all |
| H6 answer window, observed 250-689 us on the bus (freezer 250, box 424-430, node `01` 604-689); the implant targets 250-600 us | 160 MHz, single core, Wi-Fi co-processor on the same CPU | 160 MHz RISC-V, no UART swap overhead; to be measured under Wi-Fi load |
| UART count | 1 usable (the other is the boot log/bridge) | 2, so bus + a hardware log UART at the same time |

Sending a BREAK (H2, `uart_write_bytes_with_break()`) is **not** a win for the implant: node `02` never sends one. It
matters only for a freezer emulator on the C3.

**Losses / limits:**

| Req | Verdict |
| --- | --- |
| H11 antenna | **worst of the candidates.** A ceramic chip antenna sits too close to the ground plane without the clearance and tuning stripline its datasheet asks for; weak Wi-Fi range is this board's most common complaint. Fine on the bench, wrong inside a steel cabinet. Antenna mods exist (see H). **Seen on our unit [ok] 2026-11-08:** at default TX power Wi-Fi association failed repeatedly (`Auth Expired`, `Probe Request Unsuccessful`) at -65 dBm; with ESPHome `wifi: output_power: 8.5dB` it connects. Re-tested 2026-11-10: at `20dB` it never associates and draws ~70 mA (scan/retry loop), at `8.5dB` it connects first try at ~10 mA average (checklist item 11). **Finger test 2026-11-10 (n=1):** at `20dB`, ~20 failed attempts over 2 min (14:05:45-14:07:44, `Auth Expired` / `Probe Request Unsuccessful`, also towards an AP at -58 dBm); finger placed on the 40 MHz crystal at 14:07:47, `Connected` at 14:07:49, HA API 5 s later. Reception is fine throughout (APs seen at normal RSSI), only our transmissions fail. Points to **crystal pulling**: the TX field of the badly placed chip antenna couples into the adjacent 40 MHz crystal and shifts the carrier, the AP cannot decode the frames; the finger adds capacitance/damping; no reset was involved, the running retry loop connected right after the finger went on. After removing the finger: ~22 mA average (meter does not resolve TX peaks), board warm, and the link **does not stay up**. **[hyp, strong]** - a repeat would confirm. Conclusion: `20dB` is unusable on this board; keep `8.5dB`. Known fixes from the community: lower TX power (what we use), antenna mod / external antenna. Supply droop in the `73L33` is the alternative explanation, less likely given the finger effect. Amazon reviews of this listing report boards getting very hot and dropping Wi-Fi; the mismatched antenna (reflected RF power) and the linear `73L33` regulator both turn into heat, and lower TX power helps both [hyp] |
| H9 cold | dev-board parts (LDO, USB connector, LEDs, crystal) are unrated consumer parts; only the chip itself is -40..+85 C |
| H10 condensation | no coating, exposed USB-C; bench only |
| H12 supply | the `73L33` regulator's current rating is unknown vs the 250-350 mA Wi-Fi peak. There is a Schottky diode VBUS -> VSYS **[photo], [schematic]**, so the `5V` pad and USB are probably diode-OR'd, but check before connecting both [?] |
| H8 size | 22.52 x 18 mm: the smallest board we have, but the header height and the USB-C plug are the real constraint |

**Unaffected by the choice:** H1, H5, H7, H13, H16 (ESPHome supports the C3 with `board: esp32-c3-devkitm-1`).

## F. Pin allocation for this project

The bus runs on **UART1**; UART0 stays free on GPIO21/20 (ROM boot log and a spare hardware UART).

| Function | GPIO | Pad | Notes |
| --- | --- | --- | --- |
| Bus data (TX + RX on one pin) | **IO3** | L5 | UART1 TX and RX routed to GPIO3, `GPIO_MODE_INPUT_OUTPUT_OD`, **100 Ohm series resistor** to the bus (H15), **external pull-up** (the freezer's, or the emulator's 4.7 kOhm on the bench), optional pull-up footprint (H14). Logic-analyzer CH0 stays on the same net. Wiring: `src/esp32-c3-supermini--esphome/WIRING.md` |
| Console / log | native USB-Serial/JTAG | - | ESPHome `hardware_uart: USB_SERIAL_JTAG`; nothing to wire |
| UART0 (spare) | IO21 / IO20 | R8 / R7 | free; reserved so the ROM log does not meet other wiring |
| MOSFET stage drive, fallback | **IO1** | L7 | never the bus pad itself |
| Spare | IO0, IO10, IO4-IO7 | L8, R6, L4, R1-R3 | IO4-IO7 are JTAG, keep them free if we ever want OpenOCD; IO5 has no ADC with Wi-Fi |
| Never | IO8, IO9, IO18, IO19 | R4, R5, - | IO8: strap, blue LED. IO9: boot-mode select **and** the `BOOT` button; wiring the bus there would make every button press a BREAK. IO18/19: USB |

Both emulators (`esp-freezer-emulator-poc`, `esp-smartdevice-emulator-poc`) could run one board each on this pinout:
node `7F` on one, node `02` on the other, analyzer in between.

## G. Bench checklist

Order roughly by cost. Items 1-3 gate the rest.

- [ ] 1. `ls /dev/ttyACM*` / `esptool --chip esp32c3 flash_id` -> confirm native USB (B), real flash size and model
- [ ] 2. Continuity test every pad against its silkscreen; note any deviation from C
- [x] 3. Photograph chip, USB-C area and regulator; transcribe markings into A (done from the two `signal-*.jpg`; a close-up of the `73L33` is still wanted)
- [ ] 4. Smoke test: PlatformIO `board = esp32-c3-devkitm-1`, framework arduino, `USB CDC On Boot: Enabled`; blink the on-board LED (confirm GPIO8, active low), then GPIO3 high/low
- [x] 5. Console: confirm serial output over USB-Serial/JTAG at 115200 without any UART wiring (ESPHome log on `/dev/ttyACM0`, 2026-11-08)
- [ ] 6. BREAK TX (**freezer emulator only**, not needed for the implant): `uart_write_bytes_with_break()` at 57600 with 315 us -> measure the low pulse on the 24 MHz analyzer; find the achievable tolerance range (H2, `04-open-questions.md` #11)
- [ ] 7. BREAK RX: feed the freezer's or the emulator's real BREAK into IO3 and confirm the driver reports a break/framing event rather than garbage bytes (H3)
- [ ] 8. Open-drain: IO3 as `INPUT_OUTPUT_OD` with a 4.7 kOhm pull-up -> rise time at 57600, and read-back of our own bytes
- [ ] 9. Live bus: attach to the freezer with TX off, measure the rise time and the resistance to 3.3 V with the box removed -> does the freezer already pull up? (H14)
- [ ] 10. Reset/flash while attached and `tx on`: the analyzer must show no low pulse caused by the board (phase-2 spec, `esp-node02-responder`)
- [ ] 11. Supply: current from 5 V idle, during Wi-Fi association and during Wi-Fi TX peaks (H12); check the `73L33`'s temperature after 10 min - compare default TX power vs `output_power: 8.5dB`; a board that is too hot to touch at 8.5 dB is likely defective (reviews report such units). **Measured 2026-11-10 [ok]** (USB power meter, 5.2 V, averaged, ~10 mA resolution): `8.5dB` → associates on the first try, HA API connected, **~10 mA**; `20dB` → never associates (`Probe Request Unsuccessful` / `Auth Expired` loop, even at -65 dBm), **~70 mA** from the continuous scan/retry. Peaks not captured; back at `8.5dB` after reflashing → **~10 mA** again (reproducible)
- [ ] 12. Power isolation: does the `5V` pad feed VBUS through the diode, and what happens with USB and an external 5 V applied at once (the listing says never) -> decide whether the installed unit can be fed from the appliance
- [ ] 13. RSSI from the cavity where the box sits, board antenna as-is vs. a wire antenna on the modded pad (H11)
- [ ] 14. Port `lib/bus_uart_esp8266` to a `bus_uart_esp32c3` and add `[env:esp32c3]` to both emulator projects (separate change; the ESPHome implant in `src/esp32-c3-supermini--esphome/` covers node `02`)

## H. Sources

- Board page, pinout table and antenna write-up: https://www.espboards.dev/esp32/esp32-c3-super-mini/
  (pinout art and board drawings are CC BY-NC 4.0, credit "ESPboards" if published - relevant for the public-repo plan
  in `../../../README.md`)
- Board datasheet mirror: https://cdn.espboards.net/boards/esp32-c3-super-mini/docs/datasheet.pdf
- ESP32-C3 Series Datasheet: https://cdn.espboards.net/mcus/esp32c3/datasheet.pdf (original: espressif.com)
- Our listing: https://www.amazon.de/-/en/dp/B0F4MXZ7Q3. Its image `71RSgyqvxoL._AC_SL1500_.jpg` shows **two** boards:
  the top half is an RP2040-Zero pinout, not ours; only the bottom half is the SuperMini pinout. Its image
  `71YZVbTXyKL._AC_SL1500_.jpg` is the vendor schematic.
- Our photos: `photos/signal-*.jpg` (see `photos/README.md`)
- Antenna analysis and mods: https://hackaday.com/2025-04-07/simple-antenna-makes-for-better-esp32-c3-wifi/,
  https://peterneufeld.wordpress.com/2025-03-04/esp32-c3-supermini-antenna-modification/
- 3D model: https://grabcad.com/library/esp32-c3-supermini-1
- Arduino settings: board "ESP32C3 Dev Module", `USB CDC On Boot: Enabled`, flash 4 MB QIO/QIO, 921600 baud.
  ESPHome/PlatformIO: `board: esp32-c3-devkitm-1`, framework esp-idf.
