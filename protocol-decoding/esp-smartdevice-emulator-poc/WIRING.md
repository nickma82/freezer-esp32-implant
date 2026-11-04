# Wiring (phase 1, receive only)

Amica NodeMCU v2 (ESP8266) connected in parallel to the bus. Source: design D9 of `esp-poc-phase1-listener`.

```
 FREEZER (appliance)                          Amica NodeMCU v2 (ESP8266)
 ───────────────────                          ──────────────────────────
                                                   ┌──[ USB ]──┐   ◄── to laptop
  pad 1 ─ ✗ NEVER connect                          │           │
  pad 2 ─ BUS ──┬─── SmartDeviceBox (unchanged)    │  D4 ●─────┼──┐  GPIO2  = console out (UART1 TX)
                │                                  │           │  │  jumper wire
                ├─── Logic analyzer CH0            │  TX ●─────┼──┘  GPIO1  = CP2102 RX -> USB
                │                                  │           │
                └──[ 4.7 kΩ ]──────────────────────┼─● D7      │  GPIO13 = bus RX (input only)
  pad 3 ─ ✗ NEVER connect                          │           │
                                                   │  D8 ○     │  GPIO15 = LEFT OPEN (phase 1)
  GND pad ──┬─── Logic analyzer GND                │           │
            └──────────────────────────────────────┼─● GND     │
                                                   └───────────┘
```

| NodeMCU pin | Connect to | Why |
|---|---|---|
| **D7** (GPIO13) | pad 2 through **4.7 kΩ** | bus receive, input only |
| **GND** | appliance GND pad | common reference |
| **D4** (GPIO2) | **TX** pin (GPIO1) with a jumper | puts the UART1 console onto the USB serial chip |
| **D8** (GPIO15) | nothing | strap pin, phase 2 only |
| pads 1 and 3 | nothing, ever | |

## Console test only (no bus)
Only the **D4 → TX jumper** is needed. Flash, open a serial monitor at 115200 and press `?`: the welcome banner appears.

## Safety
- Measure the voltage between appliance GND and laptop/USB GND before connecting. For the first contact, run the laptop on battery.
- Power the NodeMCU over USB before plugging in D7. The 4.7 kΩ series resistor limits back-feed below 1 mA if it is not.
- The firmware never transmits in phase 1. Pads 1 and 3 are never connected.

## Pitfall: internal pull-up on D7
Arduino's `pinMode(13, FUNCTION_4)` enables the pad's internal pull-up. With the 4.7 kΩ series resistor that holds the bus node
flat and the UART receives nothing. `bus_uart::begin()` clears `GPFPU`/`GPFPD` afterwards. If the log shows no slots, probe D7
(on the resistor side) with the analyzer: it must follow the bus.
