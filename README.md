# freezer-esp32

<p align="center">
  <img src="docs/project-logo/Smart%20Freezer%20Home%20Assistant%20Badge2.png" width="300"
       alt="Smart Freezer badge: a chest freezer reading -18 °C, an ESP32 SuperMini board and the Home Assistant logo">
</p>

Reverse engineering the bus between a smart appliance and its **SmartDeviceBox** (the Wi-Fi module), so that an
ESP32 can replace the box entirely and talk to Home Assistant locally, without the vendor's cloud.

Private while captures still carry device IDs; made public once they are anonymized (see *Publishing* below).

## Status

- Captures of every app function in progress (Nick): SuperFrost, target temperature, BottleTimer, PartyMode, door alarm.
- Protocol: first commands identified, not yet written up here.

## Hardware

The SmartDeviceBox PCB is made by E.G.O. Four gold spring-contact pads on the edge are the box ↔ appliance connector,
silkscreened `1`, `2`, `3` and `GND` (the lone pad, top right in the photo).

See docs/smartdevicebox-pcb.jpg

Debug wires in the photo: blue → GND, brown → pad 2, orange → pad 3, green+white → a test point next to pad 1 (a trace
runs from it to pad 1). Label and QR code pixelated.

| Pad | Idle level | Assumed function |
| --- | ---------- | ---------------- |
| GND | 0 V        | ground           |
| 1   | 0 V        | ?                |
| 2   | 3.2 V      | ? (UART line?)   |
| 3   | 3.3 V      | ? (UART line / supply?) |

An unpopulated 4-pad footprint (`S0`) and 4 holes at the board edge look like programming/debug headers.

## Protocol (what we know)

- UART, **57600 baud**, 3.3 V logic.
- Requests start with `0x00` + command, responses with `0xff` + command + data.
- The stop bit looks odd ("inverse stop bit"). Candidates: a parity bit (decode as 8E1 / 8O1), 9-bit framing
  (address/data bit), or an inverted line. To settle on a capture.

## Layout

| Path             | What                                                                 |
| ---------------- | -------------------------------------------------------------------- |
| `captures/raw/`  | Raw Logic 2 captures (`.sal`) + decoded CSVs. **May contain device IDs: never published.** |
| `captures/anon/` | The same captures with device IDs blanked; the publishable set.     |
| `docs/`          | Protocol write-up: framing, command table, state machine; photos.   |
| `tools/`         | Decoders, the anonymizer, test senders.                             |
| `src/esp32-c3-supermini--esphome/` | ESPHome firmware: ESP32-C3 SuperMini as node `02` (box replacement), Home Assistant. |

Name captures `YYYY-MM-DD-<appliance-model>-<action>.sal`, with a matching `.md` noting what was done when (box
plugged in, door opened at ~12 s, SuperFrost on in the app at ~30 s, ...) so messages can be matched to actions.
Power-up of the box is worth capturing on its own, since any handshake happens there.

## Privacy for publishing logs

Remove metrics out of your raw captures

1. Run the device-ID check (`tools/`, to be written once we know where the ID sits in a frame). It must pass on the
   copy: every `.sal` unpacked, every file, every blob in the history and every commit message searched for the ID as
   ASCII, hex in both byte orders and BCD. It also checks that no checksum in the anonymized captures still covers the
   original ID.
2. Push the copy to a new public repo.

Keep the ID out of everything except `captures/raw/`: no IDs in `docs/`, test files, code comments, commit messages,
file or branch names, issues or PRs.

## Credits

- nickma82: captures, pinout, first protocol analysis.
- Enduriel: motivation, architecture
