## Purpose

Passive reception of the freezer bus on the Amica NodeMCU, connected in parallel to the SmartDeviceBox and the logic
analyzer. It proves that the board receives and decodes the live traffic completely and with correct timing, without
influencing the bus.

## ADDED Requirements

### Requirement: Receive-only connection
In this phase the firmware SHALL never drive the bus line. The bus input SHALL be configured as a high-impedance input,
and the UART transmit function SHALL NOT be routed to any pin connected to the bus. The firmware SHALL NOT drive the
appliance pads 1 and 3 or connect to them.

#### Scenario: Bus unaffected by the listener
- **WHEN** the board is attached in parallel to the box and the logic analyzer, and the bus is captured with and without the board attached
- **THEN** the slot schedule, the BREAK length (315 µs ± 1 µs) and the box's answer latency (426 µs ± 5 µs) are the same in both captures, and the decoded export has no additional framing errors

#### Scenario: Board reset or unpowered
- **WHEN** the board resets, is being flashed, or is unpowered while attached
- **THEN** normal traffic between the freezer and the box continues uninterrupted

### Requirement: Break-delimited framing with timestamps
The listener SHALL detect every BREAK (a low period of at least 10 bit times, nominally 315 µs) as the start of a slot.
It SHALL collect the following bytes up to the next BREAK into one slot, and timestamp each byte with microsecond
resolution relative to a monotonic clock. It SHALL accept bit rates between 56.5 and 58.5 kbit/s (the freezer runs at
about 57.09 kbit/s, the box at about 57.13 kbit/s).

#### Scenario: Normal slot
- **WHEN** the freezer sends BREAK, slot byte `02`, and the box answers with a 9-byte body
- **THEN** one slot with 10 bytes is reported, together with the time of the slot byte and the latency from the start of the slot byte to the start of the first body byte

#### Scenario: Empty slot
- **WHEN** a BREAK is followed by a single slot byte (e.g. `0C`, `7E`, `7F`) and then the next BREAK
- **THEN** an empty slot is reported for that slot byte

#### Scenario: Missed or broken byte
- **WHEN** a byte has a framing error that is not a BREAK, or the receive buffer overruns
- **THEN** the slot is reported as damaged, and the error counters are incremented, instead of the slot being dropped silently

### Requirement: Machine-readable message log
For every slot the listener SHALL output one log line over the USB console, containing:
- the timestamp;
- the slot byte;
- the body bytes in hex;
- CRC validity;
- the latency from the slot byte to the first body byte in µs;
- the decoded meaning, using the semantics of `esp-bus-core`.

The line format SHALL be stable and parseable, so that a host tool can compare it with a Logic 2 export. A filter SHALL
allow suppressing empty slots and idle replies.

#### Scenario: Box write seen on the bus
- **WHEN** the original box sends the SuperFrost-on write in slot `02`
- **THEN** the log contains its bytes `02 FF 02 24 01 04 0C 00 20 04 5D 69`, CRC valid, and the decoded text "mode = superfrost"

#### Scenario: Door opened
- **WHEN** node `0B` broadcasts `0B 80 0B 24 01 04 14 04 20 01 DA 11`
- **THEN** the log line reports "door open"

### Requirement: Periodic statistics
At a fixed interval (default 10 s) the listener SHALL output a statistics line with:
- slots per slot byte;
- empty slots;
- messages with valid and invalid CRC;
- framing errors;
- overruns;
- the observed slot-`02` periods;
- median and spread of the answer latency per source node.

#### Scenario: Steady state
- **WHEN** the freezer and box run in steady state for 10 s
- **THEN** the statistics show 0 invalid CRCs, slot-`02` periods of about 285 or 570 ms, and median latencies of about 250 µs (node `7F`), 426 µs (node `02`) and 604 or 648-689 µs (node `01`/`0B`)

### Requirement: Evaluation against the logic analyzer
The project SHALL provide a host tool that compares a board log with a Logic 2 Async Serial export (hex radix,
`protocol-decoding/csv` format) recorded at the same time. The tool SHALL report:
- matched messages;
- messages missing from the board log;
- extra messages in the board log;
- byte mismatches;
- the latency difference per source node.

#### Scenario: Acceptance run
- **WHEN** board and analyzer record the same 60 s or more, including a door open/close and one command from the app
- **THEN** at least 99.9 % of the analyzer's messages appear byte-identical in the board log, there are no extra messages, and the median latency per source node differs by at most 10 µs

#### Scenario: Long run
- **WHEN** the listener runs attached for 1 hour
- **THEN** it does not reset, and its statistics report no overruns
