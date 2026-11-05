# Wiring: freezer emulator (NOT bench-tested)

Amica NodeMCU v2 (ESP8266) plays the freezer (node `7F`) towards the **real SmartDeviceBox**. The freezer is **not attached**.
Based on `../esp-smartdevice-emulator-poc/WIRING.md` (receive side). No MOSFET: the ESP drives the bus directly from an
open-drain pin.

```
 SmartDeviceBox connector            Amica NodeMCU v2 (ESP8266)
 ────────────────────────            ──────────────────────────
                     3V3 ──[ 4.7 kΩ ]──┐     bus pull-up (the freezer normally provides it)
                                       │
  pad 2 (BUS) ──┬──────────────────────┤
                ├──[  47 Ω  ]──────────┼──── D4 (GPIO2)   bus TX: UART1, open-drain (0 = pull low, 1 = release)
                ├──[ 4.7 kΩ ]──────────┼──── D7 (GPIO13)  bus RX: UART0 (swapped), also hears our own TX
                └── Logic analyzer CH0 │
  GND ──────────┬── Logic analyzer GND
                └── NodeMCU GND
  pad 1, pad 3 ── NOT connected (see "Open problem")

  D8 (GPIO15) ── NOT connected (boot strap, must be low at reset)
  TX / RX pins ── nothing extra: the console runs over USB (SoftwareSerial on GPIO1/GPIO3, 57600)
```

| NodeMCU pin | Connect to | Why |
|---|---|---|
| **D4** (GPIO2) | pad 2 through **47 Ω** | bus transmit, open-drain UART1 TX. GPIO2 is a boot strap pin and must be high at reset: the bus pull-up keeps it high |
| **D7** (GPIO13) | pad 2 through **4.7 kΩ** | bus receive; also reads back what we send |
| **3V3** | pad 2 through **4.7 kΩ** | bus pull-up, the freezer normally provides it (hypothesis H14). The internal pull-up on D4 alone (~50 kΩ) is too weak for the box |
| **GND** | box GND | common reference |
| **D8** (GPIO15) | nothing | boot strap: with D8 on the bus the board does not boot or flash |
| **D4 ↔ TX** | **no jumper** | the old console jumper shorts the bus TX to the console TX |

## Open problem: how is the box powered without the freezer?
Not solved. Do not guess.
- Known: pad 3 rises 0 V → 3.3 V at power-up together with the bus; pad 1 stays 0 V (`../05-bus-roles.md` §7). Hypothesis H12: pad 3 is the box's 3.3 V supply, pad 1 an enable/strap line (`../../docs/hardware-requirements.md` H12, H13 "never drive pins 1 and 3 until understood").
- **Measure first** on the freezer's connector with the box unplugged (Phase 0 test 3 in `../potential-next-steps.md`): voltage on pads 1 and 3, current available, and whether the freezer has a pull-up on pad 2. Only then decide how to power the box.
- If pad 3 is a 3.3 V supply: the NodeMCU 3V3 pin (onboard AMS1117, USB powered) may carry the box only if the box's draw fits. The box has a Wi-Fi SoC (CC3200), TX peaks of 250-350 mA; USB 500 mA minus the ESP8266's own Wi-Fi-off draw is tight. A separate 3.3 V supply with a common GND is safer.
- If pad 1 is needed as enable: unknown polarity and level, measure it on the freezer first.
- Simplest safe option while unresolved: keep the box connected to the freezer's supply pads as today and only move pad 2 (data) to the NodeMCU; the freezer MCU must then be silent on the bus, which is not possible with it attached. So a real test needs the box powered separately. Nothing in this repo has done that yet.

## Safety
- Measure the voltage between box GND and laptop/USB GND before connecting; run the laptop on battery for first contact.
- Power the NodeMCU over USB before connecting D4/D7 to the bus.
- The firmware **starts the master at boot** (~50 ms after the console comes up): a box that loses its freezer for minutes stays
  degraded until power-cycled, so the bus must come back right after a crash or reset. For bench work without the box press
  `s` to stop it. Pads 1 and 3 are never driven by this firmware.
- GPIO2: `bus_master::begin()` sets it open-drain high, then routes U1TXD to it with interrupts off and sets open-drain again (`pinMode` clears it). It is never push-pull, so it cannot fight the box. The onboard blue LED is on GPIO2 and flickers with bus traffic.
- At every reset the ESP8266 boot ROM puts its boot text on GPIO2 too (measured 2026-11-05: ~30 ms of 74880-baud text,
  longest low 80 µs, so never a BREAK; then one 4.7 µs glitch when the app takes the pin, then idle high). Harmless with no box attached; with the box attached it sees a short burst of noise at reset.- Same D7 pitfall as phase 1: the internal pull-up on D7 is cleared in `begin()`.
