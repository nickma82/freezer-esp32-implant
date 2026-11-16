# analysis/

SmartDevice Box traffic

Traffic between SmartDevice box and the freezers MCO.
Tested with appliance type: gss2766


Reverse-engineering notes and tooling for the SmartDevice <-> freezer MCU serial bus.

| Path | Content |
|------|---------|
| `01`-`05*.md` | findings: protocol spec, commands, bootup, open questions, bus roles (who schedules, who answers) |
| `potential-next-steps.md` | cheap tests still to do, ESP32 plan |
| `src/` | `crc.py` CRC-16/XMODEM, `frames.py` export splitter, `protocol.py` parser + generator, `timeline.py`, `demo.py`, `crcsearch.py`, `slots.py` schedule/latency/addressing, `clocks.py` per-sender clock fingerprint |
| `tests/` | pytest suite (CRC, parser, generator, replay of the real captures, bus roles) |
| `csv/` | Logic 2 data-table exports (Async Serial, hex radix) |
| `raw-captures/` | Logic 2 captures (`.sal`) and `digital.csv` raw edges |
| `justfile` | entrypoint: `just test`, `just demo`, `just timeline <csv>`, `just decode <csv>`, `just slots`, `just clocks` |


# Pinning Debug cables

See as well `../docs/smart-box-images/`
 
|| DevicePIN || Cable Col || Assumed functionality || 
| GND | Blue  | GND assumed        |
| 1   | Green+White | 0V default   |
| 2   | Brown       | 3.2V default (Data) |
| 3   | Orange+White | 3.3V default |

Protocol Analysis:
57600 bauds // Bits/s
1 Stop bit (or an inverse stop bit, somethings wrong with that one)

0x00+Command Request
0xff+Commond + data  response

(First notes, superseded: the `0x00` is a BREAK and the "command" is a slot byte, both sent by the freezer; the `0xff`
is the destination byte "to the freezer" in the slot owner's answer. The "inverse stop bit" is the BREAK.
See `01-protocol.md` and `05-bus-roles.md`.)


## Other Project

- Vendor cloud <-> HASS bridge https://github.com/ripleyXLR8/liebherr2mqtt
- Hackaday.io project debugging a smart freezer https://hackaday.io/project/175029-hack-a-fridge
