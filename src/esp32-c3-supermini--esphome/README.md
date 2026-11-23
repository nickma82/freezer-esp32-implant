# Freezer implant (ESP32-C3 SuperMini or ESP32-WROOM-32 + ESPHome)

An ESP32-C3 SuperMini replaces the original SmartDeviceBox as bus node `02` and exposes the freezer to Home Assistant
through ESPHome. A bare ESP32-WROOM-32 module (classic ESP32) works as well, see
[ESP32-WROOM-32](#esp32-wroom-32-classic-esp32). The protocol core is shared with the emulator PoCs
(`protocol-decoding/esp-smartdevice-emulator-poc/lib/freezer_bus`, symlinked into `components/freezer_bus/`).

## Quick start

```sh
just secrets   # creates secrets.yaml from the example
$EDITOR secrets.yaml   # api_key: openssl rand -base64 32
just build
just flash     # PORT=/dev/ttyACM0 to pick a port
just logs
```

`just` lists all recipes (`test` runs the host unit tests, `check-core` verifies the built core matches the shared source).
ESPHome is run through `uvx` with the version pinned in the justfile.

## Flashing

The board has native USB-Serial/JTAG (no UART bridge), so it shows up as `/dev/ttyACM*`. `just flash` (or
`PORT=/dev/ttyACM0 just flash`) compiles and uploads; later updates also work over Wi-Fi (OTA, `just flash` picks it
when no USB port is given). If `/dev/ttyACM*` does not appear (blank or crashed firmware), hold BOOT while pressing RST,
then release BOOT: the ROM bootloader enumerates and `esptool` can flash. `just logs` follows the log (tag `freezer`).
Do not flash while the board is wired to a running freezer without reading [WIRING.md](WIRING.md): the pin is
high-impedance during reset and flashing, so the bus is not disturbed.

## Wi-Fi on the SuperMini

`freezer-implant.yaml` sets `wifi: output_power: 8.5dB`. At the default power our board failed to associate
(`Auth Expired`, `Probe Request Unsuccessful` at -65 dBm); the chip antenna is badly matched, and the reflected power also
heats the board. Measured (5.2 V, averaged): `8.5dB` connects first try at ~10 mA; `20dB` never connects
and draws ~70 mA in its scan/retry loop. If it still drops or runs too hot to touch, see `docs/dev-boards/esp32-c3-supermini/README.md` (H11,
checklist item 11). The installed unit needs a board with a proper/external antenna anyway (H11).

## ESP32-WROOM-32 (classic ESP32)

`freezer-implant-wroom.yaml` runs the same component and entities on a bare ESP32-WROOM-32 module. Select it with
`CONFIG=freezer-implant-wroom.yaml` in front of any `just` recipe (`CONFIG=freezer-implant-wroom.yaml just build`); the
default stays the C3 config. Status: **running on a Liebherr `RBsdc 525i` fridge (2026-11-22): rejoin without a hello, all reads with CRC ok, set-point write confirmed, door contact**; reply latency median 490-502 us, max 546-557 us; no echo mismatch or missed slot. The pin and set-point settings in the file are that installation's (bus GPIO16, onboard Wi-Fi EN GPIO17, set-point 3..9 °C).

| Item | C3 SuperMini (`freezer-implant.yaml`) | ESP32-WROOM-32 (`freezer-implant-wroom.yaml`) |
|---|---|---|
| Bus pin | `GPIO3` | `GPIO16` (substitution `bus_pin`; `esphome -s bus_pin GPIO4 ...` overrides it; not on WROVER modules, which use GPIO16/17 for PSRAM) |
| Freezer's onboard Wi-Fi | - | `GPIO17` (substitution `factory_wifi_en_pin`) held low, open-drain: newer freezers have Wi-Fi on their own board, and its EN pin wired to this pad keeps it off. Unconnected, it does nothing. It only acts from `setup()` on (not during reset, flashing, safe mode or while the module is unpowered), so the EN line also needs a pull-down on the board. Why: with the freezer's own Wi-Fi online, anyone with access to the vendor cloud account can change the freezer's settings (e.g. switch it to holiday mode); the implant is meant to replace the cloud, not to run beside it |
| Console | native USB-Serial/JTAG (`/dev/ttyACM*`) | UART0 (GPIO1/3) through an external 3.3 V USB-UART adapter (`/dev/ttyUSB*`) |
| Wi-Fi | `output_power: 8.5dB` (antenna workaround) | default power (PCB antenna) |
| Bus task | single core | pinned to core 1 (with ESPHome's loop and the UART interrupt; Wi-Fi on core 0) |
| Abort on echo mismatch | TX FIFO reset | pad cut off from UART1 TX in the GPIO matrix (the classic ESP32 cannot reset UART1's TX FIFO); reconnected once the UART is idle, slot `02` stays empty until then |
| Device name | `freezer-implant` | `freezer-implant-wroom` (testbench: `C3_HOST=freezer-implant-wroom.local`) |

First flash: the module has no USB, so wire the adapter's TX/RX/GND to the module's RXD0 (GPIO3) / TXD0 (GPIO1) / GND,
hold IO0 low while pulsing EN low, release IO0, then `CONFIG=freezer-implant-wroom.yaml PORT=/dev/ttyUSB0 just flash`.
Later updates go over Wi-Fi (OTA). Supply, straps and pins: [WIRING.md](WIRING.md#esp32-wroom-32) and
`docs/dev-boards/esp32-wroom-32/README.md`.

To check on the bench before the freezer (same as for the C3, plus): the BREAK / `00` byte order on the classic ESP32's
UART (the frame splitting handles both orders), the reply latency (`latency_median/max`) and that an echo mismatch frees
the bus at once (analyzer).

## Configuration

```yaml
freezer_bus:
  id: freezer_bus
  bus_pin: GPIO3          # required; pins with a boot, console or flash duty are rejected (see below)
  reply_delay_us: 420     # 250..600, default 420
  status_interval: 10s    # periodic read of the freezer state, default 10 s
  network_writes: true    # write IP/SSID/MAC to the freezer like the original box (group 48), default true
  setpoint_min: -28       # set-point range in degC, default -28..-15 (a freezer); a fridge reports e.g. +4
  setpoint_max: -15
```

| Option | Default | Meaning |
|---|---|---|
| `bus_pin` | (required) | one GPIO for TX and RX, open-drain on UART1, 57600 8N1. Rejected on the C3: 8, 9 (straps, BOOT), 18, 19 (USB). Rejected on the classic ESP32: 0, 2, 5, 12, 15 (straps), 1, 3 (UART0), 6-11 (flash), 34-39 (input-only). Other ESP32 variants are refused |
| `reply_delay_us` | 420 | first answer byte this long after the slot byte `02` was read (spec window 250..600 us) |
| `status_interval` | 10s | period of the full status read |
| `network_writes` | true | `false` = never write group `48`/`4C` |
| `setpoint_min`, `setpoint_max` | -28, -15 | set-point range in whole degC (-40..+20, min <= max): the options of the `setpoint` select, the limits of the `setpoint` number, and the engine refuses requests outside it. The default fits a freezer; a fridge reports positive values (seen: +4 on an `RBsdc 525i`), which show as unknown until the range covers them |

The entities are platforms of `freezer_bus` (`switch`, `number`, `sensor`, `binary_sensor`, `text_sensor`); each is
optional. See `freezer-implant.yaml` for the full set.

## Entities

Freezer entities follow the freezer's read-back (never the request). While the bus is not alive (no slot `02` for 2 s)
or the start-up is not `running`, sensors are `NaN`, binary sensors and text sensors are unknown, and writes are
refused. A refused or unconfirmed write logs a warning and re-publishes the freezer's actual value.

| Entity | Type | Source parameter | Notes |
|---|---|---|---|
| `superfrost` | switch | `0C/00` mode (4 = on, 1 = off) | write `0C/00 = 04` / `01` |
| `party` | switch | `08/02` (2 = on, 0 = off) | write `08/02 = 02` / `00` |
| `bus_transmit` | switch (config) | engine `tx_enabled` | on at every boot; turns off by itself on an echo mismatch; not restored |
| `setpoint` | select (dropdown, one option per degC from `setpoint_min` to `setpoint_max`, default `-28 °C` .. `-15 °C`) | `0C/06` low byte | write `0C/06`; unknown if the appliance reports a value outside the options; alternative: `number:` platform `setpoint` (box, same range, step 1); range beyond the verified -18..-16 is the owner's choice |
| `temperature` | sensor, degC | `0C/14` | |
| `power_fail_temperature` | sensor, degC | `0C/12` | unknown if the freezer answers flag `00` |
| `door` | binary sensor (door) | `0C/31` and node `0B`'s push `14/04` | |
| `door_alarm` | binary sensor (problem) | `0C/09` | |
| `power_fail_alarm` | binary sensor (problem) | `0C/11` | |
| `bus_connected` | binary sensor (connectivity) | engine bus-alive | stays live while the bus is down |
| `freezer_version` | text sensor (diag) | service `16` answer | e.g. `TOD 5-600\|SW 1-86-7\|BUS 1-4-0` |
| `node01_version` | text sensor (diag) | node `01` version answer | unknown if node `01` does not answer |
| `model` | text sensor (diag) | group `3C` records `01`-`03` | |
| `phase` | text sensor (diag) | engine phase | `waiting`, `handshake`, `startup`, `running`; after 30 s in `waiting` with the freezer active the start-up reads begin without a hello (rejoin after a board restart) |
| `restart` | button (diag) | ESPHome `restart` | reboots only the board; it rejoins about 30 s later (the freezer sends no new hello) |
| `latency_median`, `latency_max` | sensor (diag), us | driver | over the last 64 answers; slot byte read -> TX FIFO filled |
| `crc_errors` | sensor (diag) | engine | frames with a bad CRC |
| `echo_mismatches` | sensor (diag) | driver | answers whose echo differed (transmit is switched off) |
| `missed_slots` | sensor (diag) | driver | slot-02 windows not answered because the 600 us limit was reached |

## Timing and its limits

The freezer sends every BREAK and slot byte; the board answers slot `02` only. The reply is timed from the moment the
bus task *read* the slot byte `02` from the UART event queue, not from its true end on the wire. The real end is earlier
by the UART interrupt, event queue and task wake-up latency (tens of us, more under Wi-Fi load; not measured yet, bench
task 4.6) minus about half a bit time. So the real delay is `reply_delay_us` plus that latency: with the default 420 us
it is expected to land in the 250..600 us window, but only a logic-analyzer capture proves it. The recorded latency
(`latency_median/max`) is measured from the same timestamp, so it has the same offset. A slot where the task wakes more
than 600 us after the byte is not answered (`missed_slots`). If the capture shows too much latency, lower
`reply_delay_us` or fall back to a HAL ISR (design D4).

The UART interrupt is not placed in IRAM, so flash writes (OTA, NVS) can delay it; a missed or late slot `02` is
harmless (the freezer sees an empty slot).

## Logging

Tag `freezer`: INFO for state changes, writes, refusals, write results and phase changes; ERROR for an echo mismatch;
DEBUG one line per frame in the emulator PoC format
(`M <t_us> <slot> <lat_us|-> <crc 1|0|-> <body hex> | <decoded>`, readable by
`protocol-decoding/esp-smartdevice-emulator-poc/tools/compare.py`). `lat_us` is only set for the board's own slot-02
answers. Frames are logged from the main loop through a small ring (overflow drops are counted), never from the bus
task. The Wi-Fi password and API key are never logged; the IP and SSID appear only at DEBUG.

## `just` recipes

`just` lists them: `secrets` (create `secrets.yaml`), `config` (validate the YAML), `build`, `flash`, `logs`,
`test` (host unit tests of the engine, 46 tests), `check-core` (after a build: the copied `lb` core equals the shared
source), `clean`.

## See also

- [WIRING.md](WIRING.md) for the connection to the freezer bus
- `openspec/changes/esphome-c3-implant/` for the proposal, design, specs and tasks
