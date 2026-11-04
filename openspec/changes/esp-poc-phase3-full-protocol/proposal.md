## Why

After phase 2 (`esp-poc-phase2-node02`) the NodeMCU is a working node `02`: it answers in time, survives the handshake,
and can read. What is still missing is the functionality the SmartDeviceBox offers the user:
- switching SuperFrost and party mode;
- setting the target temperature;
- knowing the freezer's state (temperatures, door, door alarm).

Everything needed is already decoded (`protocol-decoding/02-commands.md`, `01-protocol.md` §3). The step from phase 2 is
small, and it completes the PoC as a usable box replacement on the serial console.

## What Changes

- Console commands for the known functions:
  - `superfrost on|off`, `party on|off`, `setpoint <°C>`;
  - `status`, `version`, `identity`;
  - a guarded `raw` debug command.
- Writes are sent byte-identical to the box's captured frames, one per slot `02`, and confirmed through the event block
  plus a re-read.
- A freezer state model, kept current by:
  - periodic status reads;
  - reads triggered by event-block flags;
  - passively observed broadcasts (door switch from node `0B`).
- Stable, machine-readable state lines on the console, the basis for a later Wi-Fi/Home Assistant bridge, which is a separate change.
- A safe set-point range: a constant limited to the captured values (−18…−16 °C) until the app's real range is checked.

## Capabilities

### New Capabilities
- `esp-freezer-control`: user-facing control and status of the freezer over the serial console:
  - the write commands with confirmation;
  - the state model and how it is kept current;
  - version and identity queries;
  - the debug raw command and its guards.

### Modified Capabilities
None. It builds on `esp-bus-core`, `esp-bus-listener` (phase 1) and `esp-node02-responder` (phase 2) without changing
their requirements.

## Impact

- **Firmware** in `protocol-decoding/esp-smartdevice-emulator-poc/`: command handlers, the state model and event reaction (pure C++, host-tested), and CLI extensions.
- **No new hardware.** Same bench setup as phase 2: box unplugged, logic analyzer attached.
- **Docs:** results go into `02-commands.md` (confirmation behaviour, observed ranges), `05-bus-roles.md`, `potential-next-steps.md` and the firmware README (command reference).
- **Out of scope:**
  - Wi-Fi, MQTT, Home Assistant;
  - BottleTimer (no bus command exists, `02-commands.md`);
  - writing the network records `48`/`4C`;
  - ESP32 port.
- **Risk to the appliance:** writes change real freezer settings. This is mitigated by: only captured, verified frames;
  a set-point range limit; one write per slot; and no automatic retries that could toggle repeatedly.
