## Purpose

User-facing control and status of the freezer from the NodeMCU's serial console, acting as node `02`. It covers
the functions the SmartDeviceBox offers (SuperFrost, party mode, set-point, state and alarms) using only frames verified
against the captures.

## ADDED Requirements

### Requirement: Write commands
The console SHALL accept `superfrost on|off`, `party on|off` and `setpoint <°C>`. Each SHALL queue exactly one write
message for the next free slot `02`. The body (after the slot byte) SHALL be byte-identical to `protocol-decoding/02-commands.md`:

| Command | Body |
|---|---|
| `superfrost on` | `FF 02 24 01 04 0C 00 20 04 5D 69` |
| `superfrost off` | `FF 02 24 01 04 0C 00 20 01 F8 39` |
| `party on` | `FF 02 24 01 04 08 02 20 02 0A AD` |
| `party off` | `FF 02 24 01 04 08 02 20 00 48 8D` |
| `setpoint -17` | `FF 02 24 01 05 0C 06 40 00 EF D8 99` |
| `setpoint -16` | `FF 02 24 01 05 0C 06 40 00 F0 06 7A` |

Commands SHALL be refused, with a message, while transmit is disabled or the start-up handshake is in progress.

#### Scenario: SuperFrost on
- **WHEN** the operator enters `superfrost on` with transmit enabled
- **THEN** the logic analyzer shows `02 FF 02 24 01 04 0C 00 20 04 5D 69` in the next slot `02` that is not taken by a queued handshake message, and the freezer display shows SuperFrost active

#### Scenario: Transmit disabled
- **WHEN** a write command is entered while transmit is off
- **THEN** nothing is queued, and the console answers that transmit is disabled

### Requirement: Set-point range limit
`setpoint` SHALL accept only integer °C values within a firmware-configured range. The default range SHALL be −18…−16 °C
(the values verified in the captures). Values outside the range SHALL be refused without sending anything.

#### Scenario: Out of range
- **WHEN** the operator enters `setpoint -30`
- **THEN** the command is refused with the allowed range, and nothing is sent on the bus

### Requirement: Write confirmation without automatic retry
After each write, the board SHALL watch for the freezer's "value changed" event (flag words `00/00 = 01000000`,
`00/01 = 08000000` in the event block). It SHALL then read back the written parameter. It SHALL report the write as
confirmed if the read-back equals the written value within 3 s, otherwise as unconfirmed. It SHALL NOT resend a write
automatically.

#### Scenario: Confirmed write
- **WHEN** `setpoint -17` is sent and the freezer accepts it
- **THEN** the console reports the write as confirmed, with the read-back value −17 °C

#### Scenario: Unconfirmed write
- **WHEN** no matching read-back arrives within 3 s
- **THEN** the console reports the write as unconfirmed, and no further write is sent

### Requirement: Freezer state model
The board SHALL maintain a state of mode (normal/SuperFrost), party mode, set-point, current temperature, door open,
and door-open alarm. Each value SHALL carry the age of its last update. The state SHALL be kept current by:
- a periodic status read (default every 10 s);
- an immediate status read when an event block carries any non-zero flag word;
- passive updates from node `0B`'s door broadcast (`14/04`) and from any observed `13` reply containing these parameters.

#### Scenario: Door opened
- **WHEN** the door is opened
- **THEN** the state shows door open within 1 s, from node `0B`'s broadcast `0B 80 0B 24 01 04 14 04 20 01 DA 11`

#### Scenario: Door-open alarm
- **WHEN** the door stays open for more than one minute and the freezer raises the door alarm
- **THEN** after the event block with `00/00 = 00000100`, `00/01 = 08000000`, the state shows door alarm active within 2 s; when the door is closed and the alarm ends, it shows inactive again

#### Scenario: Value changed elsewhere
- **WHEN** SuperFrost is switched on at the freezer's own panel
- **THEN** the event flag triggers a read, and the state shows mode SuperFrost within 2 s

### Requirement: Machine-readable state output
The `status` command and every state change SHALL print one line in a stable key=value format, containing all state
fields and their ages, so that a later bridge can parse it, for example:
`STATE mode=superfrost party=off setpoint=-18 temp=-18 door=closed alarm=off age_ms=...`.

#### Scenario: Status command
- **WHEN** the operator enters `status`
- **THEN** one `STATE` line is printed with every field, and unknown fields are shown as `?` instead of being omitted

### Requirement: Version and identity queries
`version` SHALL request the version string from the freezer (body `FF 02 06 01 00 56 36`, reply service `16` in slot `82`)
and from node `01` (body `81 02 06 01 00 78 E4`, reply from node `01` with destination `82`, which can take up to 2 s).
It SHALL print both strings. `identity` SHALL read group `3C` with the paged request lists the box used
(`03-bootup.md`) and print the assembled strings. Identification strings SHALL only be printed to the console and SHALL
NOT be stored in flash.

#### Scenario: Version
- **WHEN** the operator enters `version`
- **THEN** the console prints the freezer's string (e.g. `TOD 5-600|SW 1-86-7|BUS 1-4-0`) and node `01`'s string (e.g. `... SW 6-2-0 ...`)

### Requirement: Guarded raw debug command
A `raw <hex>` command SHALL queue an arbitrary body for slot `02` with the CRC appended automatically. It SHALL be
available only after `debug on` in the same session. It SHALL be refused during the handshake, and SHALL log a warning
for every use. The destination SHALL be restricted to `FF` or `0x81`.

#### Scenario: Raw without debug mode
- **WHEN** `raw FF 02 30 01 02 00 00` is entered without `debug on`
- **THEN** the command is refused and nothing is sent
