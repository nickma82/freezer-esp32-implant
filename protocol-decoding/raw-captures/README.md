# raw-captures/

Logic 2 captures of the single-wire bus (CH0 = brown cable, device pin 2), 57600 8N1.
Open the `.sal` files in Logic 2. The decoded hex exports live in `../csv/`.

| File | Situation | Decoded export (`../csv/`) |
|------|-----------|----------------------------|
| `freezer-bootup-fullduration.sal` | power-up of the box, about 58 s | `freezer_bootup_hex.csv` |
| `enabled-superfrost.sal` | SuperFrost switched on | `superfrost_enable_hex.csv` |
| `deactivated-superfrost.sal` | SuperFrost switched off | `superfrost_disable_hex.csv` |
| `enable-partymode--auto-activates-superfrost.sal` | party mode on (the freezer starts SuperFrost itself) | `partymode_enable_hex.csv` |
| `disable-partymode--auto-disables-superfrost.sal` | party mode off (SuperFrost ends with it) | `partymode_disable_hex.csv` |
| `enable-Bottletimer-41min--auto-activates-superfrost.sal` | bottle timer on (41 min), SuperFrost starts | `bottletimer_enable_hex.csv` |
| `disable-bottletimer--auto-disables-superfrost.sal` | bottle timer off, SuperFrost ends | `bottletimer_disable_hex.csv` |
| `raised-setpoint-to-minus17--current-temp-minus18.sal` | set-point -18 to -17 °C, current temperature -18 °C | `setpoint_-18_to_-17_hex.csv` |
| `raised-setpoint-from-minus17-to-minus16--current-temp-minus18.sal` | set-point -17 to -16 °C | `setpoint_-17_to_-16_hex.csv` |
| `door-short-open.sal` | door opened briefly, still open at the end | `door_short_open_hex.csv` |
| `door-short-closed--no-alarm.sal` | door opened and closed again within a minute, no alarm | `door_short_closed_hex.csv` |
| `door-alarm-on.sal` | door left open, the alarm starts (the freezer beeps) | `door_alarm_on_hex.csv` |
| `door-alarm-off--plus-door-closed.sal` | the alarm ends, then the door is closed | `door_alarm_off_hex.csv` |
| `door-open-alarm--activate--and-door-close.sal` | a whole cycle: alarm starts, door closes | `door_open_alarm_hex.csv` |
| `door-open--2sek--door-close--NOP-30sek.sal` | door opened in the first seconds, closed 2-4 s later, then 30 s without input (2026-10-31) | `door_open_close_nop30_hex.csv` |
| `freezer-bootup-fullduration.csv` | raw edges of the bootup capture, GUI export; used by `just clocks`. **Not published** (the waveform contains the owner's WiFi name and device serials), so the tests that need it skip. | `freezer_bootup_hex.csv` |
| `digital.csv` | raw edges (all three channels) of the very first capture; used to measure the break length. Its `.sal` is not stored here. | `ch0.csv`, `ch0_hex.csv` |

The exact moment of the alarm in the door captures is only known roughly, "somewhere in the middle" (no extra cabling to the beeper).
