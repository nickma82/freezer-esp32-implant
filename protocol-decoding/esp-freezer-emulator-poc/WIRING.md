# Wiring: freezer emulator (NOT bench-tested)

Amica NodeMCU v2 (ESP8266) plays the freezer (node `7F`) towards the **real SmartDeviceBox**. The freezer is **not attached**.
Based on `../esp-smartdevice-emulator-poc/WIRING.md` (receive side) and the phase-2 transmit stage (design D1 of `esp-poc-phase2-node02`).

```
 SmartDeviceBox connector                       Amica NodeMCU v2 (ESP8266)
 ────────────────────────                       ──────────────────────────
  pad 2 (BUS) ──┬── Logic analyzer CH0
                │                  3V3 ──[ 4.7 kΩ ]──┐  REQUIRED: there is no freezer pull-up now
                ├────────────────────────────────────┘
                ├──[ 4.7 kΩ ]──────────────────────── D7 (GPIO13)  bus RX, also hears our own TX
                │
                └──[ 100 Ω ]── drain  N-MOSFET (2N7002 / BSS138 / AO3400, Vgs(th) < 2 V)
                                source ── GND
                                gate ──┬──[ 100 Ω ]── D8 (GPIO15)  inverted UART0 TX
                                       └──[ 100 kΩ ]── GND         keeps the stage off at boot/reset/flash
  GND ──────────┬─── Logic analyzer GND
                └─── NodeMCU GND
  pad 1, pad 3 ── NOT connected (see "Open problem")
  NodeMCU D4 (GPIO2) ── jumper ── TX pin (GPIO1)    console to USB, 115200
```

| NodeMCU pin | Connect to | Why |
|---|---|---|
| **D7** (GPIO13) | pad 2 through **4.7 kΩ** | bus receive; also reads back what we send |
| **D8** (GPIO15) | MOSFET gate through **100 Ω**, **100 kΩ** to GND | bus transmit, open-drain stage; GPIO15 is a boot strap pin and must be low at reset |
| **3V3** | pad 2 through **4.7 kΩ** | bus pull-up, the freezer normally provides it (hypothesis H14) |
| **GND** | box GND | common reference |
| **D4** → **TX** | jumper | console |

## Open problem: how is the box powered without the freezer?
Not solved. Do not guess.
- Known: pad 3 rises 0 V → 3.3 V at power-up together with the bus; pad 1 stays 0 V (`../05-bus-roles.md` §7). Hypothesis H12: pad 3 is the box's 3.3 V supply, pad 1 an enable/strap line (`../../docs/hardware-requirements.md` H12, H13 "never drive pins 1 and 3 until understood").
- **Measure first** on the freezer's connector with the box unplugged (Phase 0 test 3 in `../potential-next-steps.md`): voltage on pads 1 and 3, current available, and whether the freezer has a pull-up on pad 2. Only then decide how to power the box.
- If pad 3 is a 3.3 V supply: the NodeMCU 3V3 pin (onboard AMS1117, USB powered) may carry the box only if the box's draw fits. The box has a Wi-Fi SoC (CC3200), TX peaks of 250-350 mA; USB 500 mA minus the ESP8266's own Wi-Fi-off draw is tight. A separate 3.3 V supply with a common GND is safer.
- If pad 1 is needed as enable: unknown polarity and level, measure it on the freezer first.
- Simplest safe option while unresolved: keep the box connected to the freezer's supply pads as today and only move pad 2 (data) to the NodeMCU; the freezer MCU must then be silent on the bus, which is not possible with it attached. So a real test needs the box powered separately. Nothing in this repo has done that yet.

## Safety
- Measure the voltage between box GND and laptop/USB GND before connecting; run the laptop on battery for first contact.
- Power the NodeMCU over USB before connecting D7/D8 stage.
- The firmware boots with **master stopped**: the bus stays idle until you press `g`. Pads 1 and 3 are never driven by this firmware.
- GPIO15: `bus_master::begin()` sets inversion first, routes U0TXD to GPIO15 with interrupts off, then clears the internal pull-up (`GPFPU`/`GPFPD`). Verify with the analyzer that the bus stays high after boot with the master stopped.
- Same D7 pitfall as phase 1: the internal pull-up on D7 is cleared in `begin()`.
