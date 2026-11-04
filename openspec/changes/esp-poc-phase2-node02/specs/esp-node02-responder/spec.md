## Purpose

Lets the NodeMCU take the SmartDeviceBox's place on the freezer bus as node `02`. It answers the freezer's slot `02` in
time, with valid messages, and is guarded so that it can never take over or disturb the rest of the bus.

## ADDED Requirements

### Requirement: Open-drain, boot-safe transmit stage
The board SHALL only ever pull the bus line low and SHALL never drive it high. While the board is unpowered, resetting,
booting or being flashed, the bus line SHALL remain released (high impedance), so that the freezer's traffic with other
nodes continues undisturbed.

#### Scenario: Board reset while attached
- **WHEN** the board is reset or re-flashed while connected to the bus with the freezer running
- **THEN** the logic analyzer shows no low pulse caused by the board, and the slots of nodes `01`/`0B` continue normally

#### Scenario: Idle level
- **WHEN** the board is powered with transmit disabled
- **THEN** the bus idle level stays at the freezer's idle level (3.2-3.3 V), the same as without the board

### Requirement: Transmit disabled by default
Transmission SHALL be disabled after every power-up and reset. It SHALL be enabled only by an explicit console command.
The enabled state SHALL NOT persist across resets. While disabled, the board SHALL behave exactly like the phase-1
listener.

#### Scenario: Power-up
- **WHEN** the board boots
- **THEN** it does not transmit until the operator enters `tx on`, and slot `02` stays unanswered (empty) on the analyzer

### Requirement: Answer only in slot 02
The board SHALL transmit only as the answer to a BREAK followed by slot byte `02`. It SHALL never transmit a BREAK, a
slot byte, or anything in any other slot (including `82`, `FF`, `01`, `0B`, `0C`, `7E`, `7F`). Every answer SHALL be
exactly one message: destination `FF` or `81`, source `02`, valid CRC-16/XMODEM. A slot byte received with a framing
error SHALL NOT be answered.

#### Scenario: Other slots
- **WHEN** a capture of at least 60 s with transmit enabled is decoded
- **THEN** every byte sent by the board lies in a slot whose slot byte is `02`, and no slot has two answering devices

### Requirement: Answer timing
The first body byte SHALL start at a configurable delay after the start of slot byte `02`. The default is 426 µs, the
box's measured latency. The accuracy SHALL be ±10 µs. Body bytes SHALL follow back to back. The delay SHALL be adjustable
at runtime from the console, within 300-690 µs. The whole message SHALL be finished before the freezer's next BREAK.

#### Scenario: Default delay
- **WHEN** the board answers 100 consecutive slots `02` and the analyzer export is evaluated with `just slots`
- **THEN** the latency of source node `02` has a median of 426 µs ± 10 µs, and no single answer is outside 410-445 µs

#### Scenario: Longest message
- **WHEN** a 94-byte body (the longest observed from the box) is sent
- **THEN** it is complete before the next BREAK, and decodes with a valid CRC

### Requirement: Idle answer
When nothing is queued, the board SHALL answer slot `02` with `FF 02 30 01 02 00 00 0B 3A`.

#### Scenario: Steady state with nothing to send
- **WHEN** transmit is enabled and no request is queued
- **THEN** every slot `02` carries `FF 02 30 01 02 00 00 0B 3A`, and the freezer keeps its normal schedule (slot `02` every 285/570 ms, `0B`/`0C` every 142.5 ms, event block about every 1 s)

### Requirement: Collision and error auto-disable
The board SHALL read back every byte it transmits. If the read-back differs from what was sent, or a framing error
occurs during its own transmission, it SHALL stop that message, disable transmit, and log the event. It SHALL then stay
disabled until the operator enables it again.

#### Scenario: Box still connected
- **WHEN** transmit is enabled while the original box is still connected and also answers slot `02`
- **THEN** the board detects the echo mismatch in the first collided slot, disables transmit, and logs an error line naming the slot and the first differing byte

### Requirement: Start-up handshake responder
The board SHALL respond to the freezer's node-02 start-up messages in slot `82` the way the box did in the bootup capture
(`protocol-decoding/03-bootup.md`). Each response is queued and sent in a later slot `02`, in first-in, first-out order:

| Freezer sends (slot `82`) | Board queues |
|---|---|
| `82 7F 01 01 00` (hello) | hello reply `FF 02 11 01 06 03 01 00 00 00 00 1B 5D` |
| `82 7F 21 01 00` | ack `FF 02 30 01 02 21 00 DC 0F`, then table request `FF 02 02 01 04 13 00 00 00 A5 2F` |
| `82 7F 22 01 00` | ack `FF 02 30 01 02 22 00 8F 5A` |
| `82 7F 23 01 00` | nothing (the box sent no ack) |

The table reply (`82 7F 12 01 04 13 00 00 04`) and the handshake state SHALL be logged.

#### Scenario: Freezer power cycle with the board attached
- **WHEN** the freezer is powered up with the board attached and transmit enabled
- **THEN** the board's slot-`02` answers follow the order hello reply, ack `21`, table request, ack `22`, as in the bootup capture, and the freezer afterwards keeps scheduling slot `02` normally

#### Scenario: Board appears while the freezer runs (hot-plug)
- **WHEN** transmit is enabled while the freezer has been running without node `02`
- **THEN** the board answers with idle messages, and the log and capture record whether the freezer sends a hello to node `02` (input to `04-open-questions.md` #14)

### Requirement: Read request and reply
On a console command, the board SHALL send one read request (service `03`) for a list of parameters in its next slot
`02`. It SHALL take the values from the freezer's next service-`13` message to node `02` (slot `82`), and log them
decoded. At most one read SHALL be outstanding. If no reply arrives within 1 s, the read SHALL be logged as timed out.

#### Scenario: Status read
- **WHEN** the operator requests a read of `0C/00`, `0C/06`, `0C/14`, `0C/31`, `0C/09`, `08/02`
- **THEN** the board sends `FF 02 03 01 12 0C 00 20 0C 06 40 0C 14 40 0C 31 20 0C 09 20 08 02 20 9B 42` in slot `02`, receives the `13` reply in slot `82` about 285 ms later, and logs mode, set-point, current temperature, door, door alarm and party mode

### Requirement: Transmit logging
For every message sent, the board SHALL log the slot timestamp, the body bytes, and the measured start latency, taken
from the read-back of its first byte relative to the slot byte. The statistics lines SHALL include the count of answers,
the latency median and spread of the board's own answers, and the auto-disable events.

#### Scenario: Latency cross-check
- **WHEN** the board log and the analyzer export of the same run are compared with the phase-1 compare tool
- **THEN** the board-measured latency of its own answers matches the analyzer's within ±10 µs
