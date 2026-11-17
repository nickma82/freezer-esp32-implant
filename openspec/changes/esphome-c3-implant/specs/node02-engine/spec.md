## Purpose

The protocol behaviour of the ESP32 implant as bus node `02`, in place of the SmartDeviceBox: start-up with the freezer,
keeping a model of the freezer's state current, and sending the verified write commands. Sources:
`protocol-decoding/01-protocol.md`, `02-commands.md`, `03-bootup.md` and
`esp-smartdevice-emulator-poc/FREEZER-EMULATOR-FINDINGS.md`.

## ADDED Requirements

### Requirement: Idle answer
When the board has nothing queued, it SHALL answer slot `02` with the idle ack `FF 02 30 01 02 00 00 0B 3A`.

#### Scenario: Nothing to send
- **WHEN** slot `02` arrives with an empty queue and transmit enabled
- **THEN** the board sends `FF 02 30 01 02 00 00 0B 3A`

### Requirement: Start-up handshake
The board SHALL answer the freezer's handshake messages to node `02` (in slot `82`), each in the next slot `02`:
- hello (service `01`) → `FF 02 11 01 06 03 01 00 00 00 00` + CRC;
- init steps `21`, `22`, `23` → ack `FF 02 30 01 02 <svc> 00` + CRC.

After the second init ack it SHALL send the table request `FF 02 02 01 04 13 00 00 00` + CRC, and it SHALL accept the
freezer's `12` answer. Handshake answers SHALL take precedence over every other queued message.

#### Scenario: Handshake against the freezer emulator
- **WHEN** the freezer emulator sends hello, `21`, `22`, table answer and `23` to node `02`
- **THEN** the board answers each with the frames above in the order captured in `03-bootup.md`, and the emulator reports the box as running

### Requirement: Start-up reads and identification
After the handshake the board SHALL, in this order:
- request the freezer's version (`FF 02 06 01 00`) and node `01`'s version (`81 02 06 01 00`), accepting node `01`'s
  answer for up to 2 s;
- read the freezer identification (group `3C`, the box's paged request lists), and assemble the model and number
  strings from the flag-`80` text records;
- read every parameter of the status model once.

A missing node-`01` answer SHALL NOT block the start-up.

#### Scenario: Versions received
- **WHEN** start-up completes against the freezer emulator
- **THEN** the freezer version `TOD 5-600|SW 1-86-7|BUS 1-4-0` and the model string from `3C/01`–`03` are available

#### Scenario: Node 01 absent
- **WHEN** no node-`01` version answer arrives within 2 s
- **THEN** start-up continues, and node `01`'s version is reported as unknown

### Requirement: Network record writes
Unless disabled in the configuration, the board SHALL write group `48` during start-up in the box's order:
- `48/00 = 03`, then the `<ip>@<ssid>` text in `48/04`–`0B` as 4-byte flag-`80` records;
- `48/14` = the IP as little-endian 32 bit;
- `48/0C`–`0D` = its own Wi-Fi MAC;
- `48/03` = `01` once Wi-Fi is connected;
- then `48/00 = 04`, and about 5 s later `48/00 = 05`.

The values SHALL come from the board's real network state. When the IP changes, the text and `48/14` SHALL be written
again. The board SHALL NOT write group `4C`.

#### Scenario: Wi-Fi up
- **WHEN** the board has an IP address and finishes the handshake
- **THEN** the bus shows `48/00 = 03`, the text record with the board's real IP and SSID, `48/14` matching that IP, `48/00 = 04` and then `05`

#### Scenario: Disabled
- **WHEN** network writes are disabled in the configuration
- **THEN** no group-`48` or `4C` record is written

### Requirement: Freezer state model
The board SHALL keep a state of:
- mode (`0C/00`: `01` normal, `04` SuperFrost);
- party mode (`08/02`: `02` on, `00` off);
- set-point (`0C/06`, int8 °C in the low byte);
- current temperature (`0C/14`, int8 °C in the low byte);
- door open (`0C/31`);
- door alarm (`0C/09`);
- power-failure alarm (`0C/11`) and its temperature (`0C/12`, int8 °C in the low byte).

Each value SHALL be read with the request flag the box uses (`20` for 1-byte, `40` for 2-byte params). A reply with flag
`00` SHALL mark that value as not available. The state SHALL be kept current by:
- a periodic read of all of them, by default every 10 s;
- an immediate read after an event block (slot `FF`) in which any flag word `00/00`–`00/03` is non-zero;
- passive updates from node `0B`'s door push `14/04`, and from any freezer reply (to any node) that contains these
  parameters.

#### Scenario: Door alarm
- **WHEN** the freezer emulator raises the door alarm (`a 1`: `0C/31 = 01`, `0C/09 = 01`, event flags `00000100/08000000`)
- **THEN** the state shows door open and door alarm active within 2 s; after `a 0` both return to inactive within 2 s

#### Scenario: Temperature changes
- **WHEN** the freezer emulator's temperature is set with `t -20`
- **THEN** the state shows −20 °C within 12 s, or within 2 s if an event flag is raised with it

#### Scenario: Parameter not available
- **WHEN** the freezer answers `0C/12` with flag `00`
- **THEN** the power-failure temperature is reported as unknown, not as 0 °C

### Requirement: Verified write commands
The board SHALL send exactly these bodies (after the slot byte, CRC included) for the user commands, one message per slot
`02`, byte-identical to `02-commands.md`:

| Command | Body |
|---|---|
| SuperFrost on | `FF 02 24 01 04 0C 00 20 04 5D 69` |
| SuperFrost off | `FF 02 24 01 04 0C 00 20 01 F8 39` |
| Party on | `FF 02 24 01 04 08 02 20 02 0A AD` |
| Party off | `FF 02 24 01 04 08 02 20 00 48 8D` |
| Set-point *n* °C | `FF 02 24 01 05 0C 06 40 00 <int8 n> <crc>` (e.g. −17 → `... 00 EF D8 99`) |

Writes SHALL be refused while transmit is disabled, before the handshake is complete, or while the bus is not alive.
Writes SHALL queue behind handshake answers.

#### Scenario: SuperFrost on
- **WHEN** SuperFrost on is requested with the board running
- **THEN** the next free slot `02` carries `FF 02 24 01 04 0C 00 20 04 5D 69`

#### Scenario: Refused before start-up
- **WHEN** a write is requested before the handshake is complete
- **THEN** nothing is sent, and the request is reported as refused

### Requirement: Set-point range
The set-point SHALL accept only integer °C values from −28 to −15 inclusive. Values outside this range SHALL be refused
without sending anything.

#### Scenario: Out of range
- **WHEN** a set-point of −30 °C is requested
- **THEN** nothing is sent on the bus, and the request is reported as refused

### Requirement: Write confirmation without automatic resend
After a write, the board SHALL wait for an event block with a non-zero flag word and then read back the written
parameter. If the read-back equals the written value within 3 s, the write SHALL be reported as confirmed. Otherwise it
SHALL be reported as unconfirmed, and the state SHALL show the value that was read back. The board SHALL NOT resend a
write by itself.

#### Scenario: Confirmed
- **WHEN** set-point −17 °C is written and the freezer emulator accepts it
- **THEN** the write is reported as confirmed, and the state shows −17 °C

#### Scenario: Unconfirmed
- **WHEN** no matching read-back arrives within 3 s
- **THEN** the write is reported as unconfirmed, the state keeps the freezer's value, and no second write is sent

### Requirement: Recovery after a freezer restart
Whenever the freezer sends a new hello to node `02`, the board SHALL drop its queue and run the full start-up again
(handshake, start-up reads, network writes). This holds regardless of how long the freezer was gone. The original
box's "degraded" behaviour after outages of minutes SHALL NOT be copied.

#### Scenario: Long outage
- **WHEN** the freezer emulator is stopped for 5 minutes and then restarted
- **THEN** the board completes the handshake, re-reads the status model, and accepts writes again without a reset

### Requirement: Rejoin after a board restart
The freezer sends its hello only at its own power-up, so a board that restarts while the freezer keeps running (OTA
update, watchdog, brownout, replugged) gets no hello. When the board has been in phase `waiting` for 30 s while the bus
is alive, frames from the freezer arrive and transmit is enabled, it SHALL run the start-up reads without the handshake
(a rejoin) and then continue as after a normal start-up. If the freezer answers none of the start-up reads, the board
SHALL return to `waiting` and try again 30 s later. A hello SHALL still restart the full start-up at any time, a rejoin
in progress included.

Whether the real freezer accepts reads and writes from a node `02` that skipped the handshake is open
(`protocol-decoding/04-open-questions.md` question 14); the first rejoin at the freezer answers it.

#### Scenario: Board restarted, freezer kept running
- **WHEN** the board restarts while the freezer emulator keeps running and sends no hello
- **THEN** about 30 s after booting the board reads versions, identification and status, reaches `running`, and accepts writes

#### Scenario: Freezer ignores the rejoin
- **WHEN** the freezer answers none of the start-up reads of a rejoin
- **THEN** the board returns to `waiting`, its values stay unknown, and the next rejoin follows 30 s later
