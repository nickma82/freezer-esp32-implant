## Purpose

Chip-independent implementation of the Freezer freezer bus protocol: checksum, framing, parameter records, known
semantics and the node-02 message builders. It is verified on the host against the captured traffic so that every
firmware phase rests on the same proven core.

## ADDED Requirements

### Requirement: CRC-16/XMODEM checksum
The core SHALL compute CRC-16/XMODEM (poly 0x1021, init 0x0000, no reflection, xorout 0x0000) over the message body from
the destination byte up to the last payload byte. The slot byte SHALL be excluded. The CRC SHALL be stored and expected
low byte first.

#### Scenario: Known idle reply of the box
- **WHEN** the CRC is computed over `FF 02 30 01 02 00 00`
- **THEN** the result is `0x3A0B`, transmitted as `0B 3A`

#### Scenario: Agreement with the reference implementation
- **WHEN** the CRC is computed for every complete message in `protocol-decoding/csv/*_hex.csv`
- **THEN** it matches the trailing two bytes exactly where `protocol-decoding/src/crc.py` reports a match, and reports a mismatch only for the messages truncated by the end of a capture

### Requirement: Slot and message decoding
The core SHALL decode one slot, given the bytes received after a BREAK, into:
- the slot byte;
- for a non-empty slot: destination, source, service, flag, optional length plus payload, and CRC validity.

A slot with only the slot byte SHALL be reported as an empty slot. Destination `0x80 | n` SHALL be reported as node `n`,
`FF` as the freezer (node `7F`), and `80` as broadcast.

#### Scenario: Empty slot
- **WHEN** the bytes after a BREAK are only `7E`
- **THEN** the result is an empty slot with slot byte `7E` and no body

#### Scenario: Box idle reply
- **WHEN** the bytes are `02 FF 02 30 01 02 00 00 0B 3A`
- **THEN** the result is slot `02`, destination freezer, source node `02`, service `30`, flag `01`, payload `00 00`, CRC valid

#### Scenario: Corrupted message
- **WHEN** any single byte of a valid message body is altered
- **THEN** the message is reported with CRC invalid and is not interpreted further

### Requirement: Parameter records
The core SHALL parse and encode records `<grp> <id> <flag> [value]`. The flag determines the value width:
- `20`/`21`: 1 byte;
- `40`/`41`: 2 bytes, big endian;
- `80`/`82`/`84`: 4 bytes;
- `00`: no value.

Read-request lists (service `03`) SHALL be parsed as headers without values. Value messages (services `13` and `24`)
SHALL be parsed with values. An unknown flag or a length mismatch SHALL be reported as a decode error, not guessed.

#### Scenario: Value reply with a missing parameter
- **WHEN** a `13` reply contains the record `0C F9 00`
- **THEN** it is decoded as parameter `0C/F9` with no value (not available)

#### Scenario: Lossless round trip over all captures
- **WHEN** every non-empty message in `protocol-decoding/csv/*_hex.csv` is decoded and re-encoded
- **THEN** the re-encoded bytes are identical to the captured bytes, with 0 decode errors

### Requirement: Known parameter and event semantics
The core SHALL interpret the parameters documented as [ok] in `protocol-decoding/01-protocol.md` §3:
- `0C/00` mode: `01` normal, `04` SuperFrost;
- `08/02` party mode: `02` on, `00` off;
- `0C/06` set-point: signed °C in the low byte;
- `0C/14` current temperature: signed °C in the low byte;
- `0C/31` door open;
- `0C/09` door-open alarm;
- `14/04` door switch pushed by node `0B`.

It SHALL also interpret the event block (`FF 80 7F 24 01 20 ...`): the four flag words, the counter `00/04`, and the
named flag combinations (value changed, party changed, door alarm changed). Unknown flag combinations SHALL be reported
raw, not dropped.

#### Scenario: Set-point readback
- **WHEN** a value message carries `0C 06 40 00 EE`
- **THEN** the set-point is reported as -18 °C

#### Scenario: Door-alarm event
- **WHEN** an event block carries `00/00 = 00000100` and `00/01 = 08000000`
- **THEN** the event is reported as "door alarm changed"

### Requirement: Node-02 message builders
The core SHALL build the bodies node `02` sends after the slot byte (destination `FF`, source `02`, CRC appended). The
results SHALL be byte-identical to the frames in `protocol-decoding/02-commands.md`, without the leading slot byte:
- idle;
- write of one parameter;
- read request for a list of parameters;
- hello reply;
- init ack;
- table request;
- version request.

#### Scenario: SuperFrost on
- **WHEN** a write of `0C/00` = `04` is built
- **THEN** the body is `FF 02 24 01 04 0C 00 20 04 5D 69`

#### Scenario: Set-point -17 °C
- **WHEN** a write of `0C/06` = -17 °C is built
- **THEN** the body is `FF 02 24 01 05 0C 06 40 00 EF D8 99`

#### Scenario: Idle
- **WHEN** the idle message is built
- **THEN** the body is `FF 02 30 01 02 00 00 0B 3A`

### Requirement: Host-verifiable without hardware
The core SHALL build and run its tests on the development PC, without the microcontroller or its SDK. The test vectors
SHALL be generated from the captured exports. Messages that carry identification or network data (groups `3C` and `48`:
serial and part numbers, SSID, IP, MAC) SHALL be left out of the generated vectors, so that no device or network
identifiers end up in the firmware repository.

#### Scenario: Native test run
- **WHEN** the developer runs the firmware project's test recipe on a PC without a board attached
- **THEN** all core tests run and pass
