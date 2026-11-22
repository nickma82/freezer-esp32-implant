# ESP32-WROOM-32 (bare module): spec sheet

Status: from the Espressif datasheet and hardware design guidelines; the firmware runs on a WROOM-32 (ESP32 rev v3.1, dual core) in a
Liebherr `RBsdc 525i` fridge since 2026-11-22. Bench checklist items not ticked below are still unmeasured.
**[src]** = datasheet or Espressif documentation, not verified on the physical module, **[?]** = must be checked on our
unit before it is used in a build. Requirement IDs (`H1`-`H17`) refer to `../../hardware-requirements.md`.

Firmware: `src/esp32-c3-supermini--esphome/freezer-implant-wroom.yaml`. Wiring: `WIRING.md` in the same directory,
section "ESP32-WROOM-32".

## A. The module

| Item | Value | Status |
| --- | --- | --- |
| Chip | ESP32-D0WD(-V3) (WROOM-32E) or ESP32-D0WDQ6 (WROOM-32): classic ESP32, dual-core Xtensa LX6, up to 240 MHz | [src], which one [?] (marking on the can) |
| Memory | 520 KB SRAM, no PSRAM (the WROVER modules have PSRAM on GPIO16/17) | [src] |
| Flash | 4 MB on the module (WROOM-32E also 8/16 MB), 3.3 V | [src], size [?] (`esptool flash_id`) |
| Antenna | PCB antenna (the `-32UE` variant has an IPEX connector instead, see H11) | [src] |
| Size | 18 x 25.5 x 3.1 mm, 38 castellated pins | [src] |
| Supply | 3.0-3.6 V; Espressif asks for a supply of at least 500 mA | [src] |
| Temperature | -40..+85 °C (all parts on the module rated) | [src] |
| USB | none: flashing and console through UART0 and an external USB-UART adapter | [src] |
| ESPHome | `board: esp32dev`, `variant: esp32`, framework esp-idf | running (2026-11-22): bus, set-point write, door contact; OTA updates |

## B. Pins for this project

Module pin numbers per the WROOM-32 datasheet **[src]**, check against your module **[?]**.

| Function | GPIO | Module pin | Notes |
| --- | --- | --- | --- |
| Bus data (TX + RX on one pin) | **IO16** (default) | 27 | open-drain, UART1 through the GPIO matrix, 100 Ω series resistor, pull-up from the bus side |
| Freezer's onboard Wi-Fi EN | **IO17** (default) | 28 | held low (open-drain) from `setup()` on, so the freezer's own Wi-Fi stays off; needs a pull-down on the board for reset, flashing and safe mode. Unconnected on freezers without onboard Wi-Fi |
| Console / flashing | IO1 (TXD0), IO3 (RXD0) | 35, 34 | USB-UART adapter; the ROM log appears on TXD0 at every boot |
| Reset | EN | 3 | 10 kΩ to 3V3 + 1 µF to GND (Espressif's RC delay) |
| Boot mode | IO0 | 25 | 10 kΩ to 3V3; low at reset = download mode |
| Other good bus pins | IO4, IO13, IO14, IO18, IO19, IO21-IO23, IO25-IO27, IO32, IO33 | | no strap, console or flash duty. IO13/IO14 are JTAG (MTCK/MTMS): keep them free if JTAG is wanted |
| Never the bus (schema rejects) | IO0, IO2, IO5, IO12, IO15 | | straps: they read the bus level at reset. IO12 high = 1.8 V flash, the module does not boot |
| Never the bus (schema rejects) | IO1, IO3 | | UART0 |
| Never the bus (schema rejects) | IO6-IO11 | 17-22 | the module's SPI flash |
| Never the bus (schema rejects) | IO34-IO39 | | input-only, no pull-ups |

## C. Differences from the C3 that the firmware handles

- UART1's TX FIFO cannot be reset on the classic ESP32 (ESP-IDF's `uart_ll_txfifo_rst()` only acts on UART0). On an echo
  mismatch the driver therefore cuts the pad off from UART1 TX in the GPIO matrix, which frees the bus at once, and
  reconnects it once the UART is idle.
- Two cores: the bus task is pinned to core 1 (ESPHome's loop task and so the UART interrupt are there), Wi-Fi runs on
  core 0.
- No USB-Serial/JTAG: the logger uses UART0 (default), the C3 config's `hardware_uart: USB_SERIAL_JTAG` does not apply.
- No reduced Wi-Fi power: the `8.5dB` in the C3 config is a SuperMini antenna workaround.

## D. Bench checklist

- [ ] 1. `esptool flash_id` through the adapter: chip revision, flash size (the installed module reports ESP32 rev3.1 in its
       log; a first module from another reel read ESP32-D0WD-V3 rev v3.1, 4 MB, but never joined Wi-Fi: `Auth Expired`)
- [ ] 2. Boot with the bus pin wired to the bench bus (pull-up on): normal boot, no low pulse on the bus during reset,
       ROM log and download mode (analyzer)
- [ ] 3. BREAK RX: the driver reports BREAK events; note in which order the BREAK's `00` byte and the event arrive (the
       C3 sends the `00` first; the frame splitting handles both)
- [ ] 4. Reply latency under Wi-Fi load (`latency_median/max`, analyzer on the bus): inside 250-600 us
- [ ] 5. Forced echo mismatch (e.g. a second driver on the bench bus): the module stops driving the bus at once, `Bus
       transmit` turns off, after re-enabling it answers again
- [ ] 6. Freezer with onboard Wi-Fi: its EN stays low through power-up, reset, flashing and normal operation (the
       pull-down alone, then IO17 from `setup()` on)
- [ ] 7. The testbench suites (`tools/testbench`, `C3_HOST=freezer-implant-wroom.local`, `EMU_PORT` set explicitly)
- [ ] 8. Supply current at idle and during Wi-Fi TX (H12); RSSI from the install location (H11)

## E. Sources

- ESP32-WROOM-32E / WROOM-32UE datasheet and ESP32-WROOM-32 datasheet (espressif.com): pin layout, strapping pins,
  supply, dimensions
- ESP32 Hardware Design Guidelines (espressif.com): EN RC delay, strapping pins, decoupling
- ESP-IDF 5.5 `components/hal/esp32/include/hal/uart_ll.h`: `uart_ll_txfifo_rst()` resets only UART0's FIFO on the ESP32
