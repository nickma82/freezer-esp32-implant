## Purpose

What Home Assistant sees of the freezer through the ESPHome native API, and what it can control. It replaces the
Freezer app and cloud with local entities backed by the node-02 engine.

## ADDED Requirements

### Requirement: Control entities
The device SHALL expose:
- a switch **SuperFrost** (on = mode `04`, off = mode `01`);
- a switch **Party mode**;
- a number **Set-point** (°C, −28 to −15, step 1).

Changing one SHALL issue the matching write command. Each entity's displayed state SHALL follow the freezer's
read-back, not the requested value. A refused or unconfirmed write SHALL therefore leave the entity at the freezer's
actual value and log a warning.

#### Scenario: SuperFrost from Home Assistant
- **WHEN** the SuperFrost switch is turned on in Home Assistant
- **THEN** the write reaches the bus, and the switch shows on once the freezer's read-back shows mode `04`

#### Scenario: Write not confirmed
- **WHEN** the set-point is changed to −20 °C and the freezer's read-back still shows −18 °C after 3 s
- **THEN** the number entity returns to −18, and a warning is logged

### Requirement: Status entities
The device SHALL expose:
- sensors **Temperature** (°C) and **Power-failure temperature** (°C);
- binary sensors **Door** (door class), **Door alarm** (problem class), **Power-failure alarm** (problem class) and
  **Bus connected** (connectivity class);
- text sensors **Freezer version**, **Node 01 version** and **Model**.

Values SHALL be published when they change. Values the freezer reports as not available SHALL be published as unknown.

#### Scenario: Door alarm shown
- **WHEN** the freezer raises the door alarm
- **THEN** the Door and Door alarm binary sensors turn on in Home Assistant within 3 s

#### Scenario: Power failure shown
- **WHEN** the freezer reports `0C/11 = 01` and `0C/12 = 00 C9`
- **THEN** Power-failure alarm is on, and Power-failure temperature shows −55 °C

### Requirement: Availability while the bus is down
While the bus is not alive, or the start-up is not complete, the freezer entities SHALL report unavailable/unknown
rather than stale values. The control entities SHALL refuse changes. Bus connected and the diagnostics SHALL stay
available.

#### Scenario: Freezer side stops
- **WHEN** the freezer emulator stops sending for more than 3 s
- **THEN** Bus connected turns off, and the temperature, door and alarm entities become unknown

### Requirement: Transmit control and diagnostics
The device SHALL expose:
- a configuration switch **Bus transmit**: on after every boot; turned off by the operator or automatically on an echo
  mismatch;
- diagnostic entities for the start-up state (`waiting`, `handshake`, `startup`, `running`), the median and maximum
  answer latency (µs), and the counters for CRC errors, echo mismatches and missed slots.

#### Scenario: Auto-disable visible
- **WHEN** the board detects an echo mismatch
- **THEN** the Bus transmit switch turns off in Home Assistant, and the echo-mismatch counter goes up

#### Scenario: Boot
- **WHEN** the board boots
- **THEN** Bus transmit is on, without any action in Home Assistant

### Requirement: Local operation without the Freezer cloud
All control and status SHALL work over the local ESPHome API without any connection to Freezer services. Wi-Fi
credentials and the API encryption key SHALL come from a git-ignored secrets file. Logs SHALL NOT print the Wi-Fi
password or the API key. Logs MAY print the IP and SSID only at debug level.

#### Scenario: No internet
- **WHEN** the Home Assistant network has no internet access
- **THEN** every entity works, and the freezer can be controlled
