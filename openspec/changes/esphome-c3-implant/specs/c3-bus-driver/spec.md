## Purpose

The ESP32-C3 SuperMini's electrical and timing layer on the freezer bus. It receives every slot, answers the freezer's
slot `02` as node `02` within the observed reply window, and can never drive the shared open-drain line high or take the
freezer's role as scheduler.

## ADDED Requirements

### Requirement: One open-drain pin for transmit and receive
The board SHALL use one GPIO (default GPIO3, configurable) for both bus receive and bus transmit. It SHALL configure the
transmit side as open-drain: it may pull the line low but SHALL never drive it high. The line SHALL be read back on the
same pin, so the board hears its own transmission. The UART SHALL run at 57600 bit/s, 8N1, LSB first, idle high. GPIO8,
GPIO9, GPIO18 and GPIO19 SHALL NOT be accepted as the bus pin.

#### Scenario: Idle level unchanged
- **WHEN** the board is attached to the bus and powered, with transmit disabled
- **THEN** the bus idle level and the BREAK/slot timing on the logic analyzer are the same as without the board

#### Scenario: Strap pin refused
- **WHEN** the YAML configures GPIO9 as the bus pin
- **THEN** the configuration is rejected at compile time with a message naming the strap/BOOT conflict

### Requirement: Released line during boot, reset and flashing
While the board is unpowered, resetting, booting, crashing or being flashed, the bus pin SHALL be high impedance. The
board SHALL NOT pull the line low for longer than one byte time (174 µs) outside its own reply in slot `02`. A low that
long or longer would look like a BREAK to the freezer.

#### Scenario: Reset while attached
- **WHEN** the board is reset or flashed over USB while attached to a running bus
- **THEN** the logic analyzer shows no low pulse caused by the board, and the other slots continue normally

### Requirement: BREAK-delimited framing
The board SHALL treat a BREAK (low for ≥ 10 bit times) as the start of a new slot. The first byte after the BREAK is
the slot byte. Everything up to the next BREAK is that slot's message. The `00` byte that a UART may report for the BREAK
itself SHALL NOT be part of the frame. Frames with a CRC-16/XMODEM mismatch SHALL be counted and SHALL NOT change any
state.

#### Scenario: Freezer reply framed
- **WHEN** the freezer sends BREAK, slot byte `82` and a `13` value reply
- **THEN** the board hands on exactly `82` + the reply body, with a valid CRC

#### Scenario: Corrupt frame
- **WHEN** a frame arrives whose CRC does not match
- **THEN** the CRC error counter goes up and no entity value changes

### Requirement: Answer only in slot 02, inside the reply window
The board SHALL transmit only as the answer to a BREAK followed by slot byte `02`. It SHALL never send a BREAK, a slot
byte, or anything in any other slot. Every answer SHALL be exactly one complete message with destination `FF` or `81`,
source `02` and a valid CRC. The first byte of the answer SHALL start between **250 µs and 600 µs** after the end of the
slot byte. That range lies inside the window observed on the bus (freezer 250 µs, box 426 µs, node `01` up to 689 µs).
If the window cannot be met, the board SHALL stay silent in that slot and count a missed slot rather than answer late.

#### Scenario: Reply latency
- **WHEN** a capture of at least 60 s with transmit enabled is decoded
- **THEN** every byte the board sent lies in a slot `02`, and every answer starts 250–600 µs after the end of the slot byte

#### Scenario: Missed window
- **WHEN** the board is too busy to start the answer within 600 µs
- **THEN** it sends nothing in that slot, and the missed-slot counter goes up

### Requirement: Echo check and transmit auto-disable
The board SHALL compare every byte it hears during its own answer with the byte it sent. On the first mismatch it SHALL
stop the answer, disable transmit, log an error and report the reason. It SHALL NOT re-enable transmit by itself.
Transmit SHALL be enabled at boot, can be switched off and on by the operator, and SHALL NOT be enabled while a
mismatch is being reported until the operator re-enables it.

#### Scenario: Collision
- **WHEN** another device drives the line during the board's answer
- **THEN** the board stops sending, transmit is reported as disabled with reason "echo mismatch", and no further slot is answered until the operator enables transmit

#### Scenario: Transmit disabled by the operator
- **WHEN** the operator switches transmit off
- **THEN** slot `02` stays empty on the analyzer, and the board keeps receiving and decoding all slots

### Requirement: Bus health reporting
The board SHALL report:
- whether the bus is alive, meaning a BREAK and slot byte were received within the last 2 s;
- the median and maximum answer latency over a sliding window;
- counters for CRC errors, echo mismatches, missed slots and received frames.

#### Scenario: Bus lost
- **WHEN** the freezer side stops sending (bus idle high, or held low) for more than 2 s
- **THEN** the board reports the bus as not alive within 3 s, and as alive again within 1 s after the slots resume
