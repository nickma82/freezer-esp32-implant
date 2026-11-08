# Freezer freezer implant (ESP32-C3 SuperMini + ESPHome)

An ESP32-C3 SuperMini replaces the Freezer SmartDeviceBox as bus node `02` and exposes the freezer to Home Assistant
through ESPHome. The protocol core is shared with the emulator PoCs
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

The board has native USB-Serial/JTAG (no UART bridge), so it shows up as `/dev/ttyACM*`. If the port does not appear,
hold BOOT while pressing RST, then release BOOT.

## Entities

TODO (task 5.7): list the switches, numbers, sensors and binary sensors exposed to Home Assistant.

## See also

- [WIRING.md](WIRING.md) for the connection to the freezer bus
- `openspec/changes/esphome-c3-implant/` for the proposal, design, specs and tasks
