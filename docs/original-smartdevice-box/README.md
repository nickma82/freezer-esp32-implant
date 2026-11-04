# Original SmartDeviceBox: Wi-Fi module identification

Status: identified. **[ok]** = confirmed from a photo or an official filing, **[src]** = taken from a web source
I did not verify against the primary document, **[?]** = open.

## Summary

The Wi-Fi part of the original box is a **Miele EK037 module**. Under its shield sits a **TI CC3200R1**
(SimpleLink Wi-Fi SoC, ARM Cortex-M4 + network processor). It is **not an ESP**, so there is no
ESP firmware to dump or reuse. The ESP32 replacement only has to speak the bus protocol
(`protocol-decoding/`), not mimic the module.

## Evidence

| Finding | Source | Status |
| ------- | ------ | ------ |
| Label: `Miele EK037`, 14-pin castellated module, shielded can, PCB trace antenna | `../smart-box-images/2025-04-17-172558.jpeg` | [ok] |
| Label IDs: `IC: 5669C-EK037`, `R-CRI-ML6-EK037`, CE, KC, MIC, CNC, CMIIT, ANATEL marks, date `12.04.21` | same photo | [ok] |
| FCC ID `2ACUWEK037` (grantee `2ACUW` = Miele & Cie. KG), class "WLAN Module", filed 2016-09-22, test lab PHOENIX TESTLAB | https://fccid.io/2ACUWEK037 | [src] |
| Chip marking `CC3200R1`, TI logo, lot code `4B3 ZERX G3`, large QFN next to a small IC and a 40 MHz-style oscillator can, shield removed | `Screenshot_20261101_212319.png` (FCC internal photos) | [ok] |
| Module text: "based on the CC3200 WiFi SOC controller", 3.3 V, 40 MHz + 32.768 kHz crystals, PCB antenna, about 30.5 x 28 mm | Miele module manual (FCC exhibit `user-manual-3143131`, copy at `../user-manual-3143131.pdf`), mirrored at manuals.plus / electric.garden | [src] |
| Intended use: solder-on module for Miele domestic appliances (hoods, hobs, cooling, washers) | same manual | [src] |

Further links:

- FCC internal photos: https://fccid.io/2ACUWEK037/Internal-Photos/int-photos-3143130
- Miele manual mirror: https://manuals.plus/m/5b6be9a42e96065a4220b68c79bb5956327b7a561b19f435762a1f983b25757b
- The ISED (Canada) Radio Equipment List entry for `5669C-EK037` was **not** found by web search.
  The IC number only encodes company `5669C` (Miele) + product `EK037`.

## Board layout (from the box photo)

- The EK037 is soldered flat onto a carrier PCB. Its pads are numbered 1-14: pin 1 bottom right, pin 7 top right,
  pin 8 top left, pin 14 bottom left (as mounted in the photo).
- The carrier PCB (right half) holds only power and interface parts: SOT-23 regulators/transistors, a bulk
  capacitor, and the 4-pad connector `X2` towards the appliance. It is silkscreened `IC4`, `T0`, `C0 K0`.
- See `../smartdevicebox-pcb.jpg` and `../smart-box-images/` for other views.

## Consequences for the ESP32 replacement

- The module's own firmware and pinout are irrelevant. The box's behaviour towards the freezer is
  defined by the bus protocol, which is already captured and decoded.
- The CC3200 is a 3.3 V device. This agrees with the measured 3.3 V idle level on the bus
  (see `../hardware-requirements.md`, H5).
- The box's MAC `00:1D:63:xx:xx:xx` does not point to Espressif. I believe `00:1D:63` is Miele's own OUI **[?]**
  (from memory, check against the IEEE registry), so the MAC says nothing about the SoC.

## Open points

- [?] Which of the module's 14 pads go to the appliance connector `X2`. Only the carrier-board traces
  tell us; the module pinout is in the Miele manual PDF and is not yet transcribed here.
- [?] Whether the CC3200 drives the bus directly or through the carrier-board parts. Relevant for the
  open-drain / pull-up questions H4 and H14 in `../hardware-requirements.md`.
