# Bootup sequence and what the SmartDevice box sends

Capture: `raw-captures/freezer-bootup-fullduration.sal`, export `csv/freezer_bootup_hex.csv` (2321 messages, 2.7 s to 58.2 s).
Parser support: `src/protocol.py` (`identity()`, `wifi()`, `text()`); replay test `test_bootup_identity_and_wifi_strings`.
The box is to be replaced by an open-source ESP32, so this is documentation of what the original box does, not a spec to copy blindly.
Confidence tags as in `01-protocol.md`: **[ok]** verified, **[hyp]** hypothesis, **[?]** unknown.

> **Correction (2026-10-30):** the freezer (node `7F`) schedules the bus, and three devices answer: the box (node `02`),
> a third device (nodes `01` + `0B`) and the freezer itself. Several statements below were re-attributed; see
> `05-bus-roles.md` for the evidence.

## Who sends what
- Group `48` (network) is **written by the box** (`02 FF 02 24 01 ...`). Only the box has WiFi, so this is certain. **[ok]**
- The box is node `02`. It is silent until 5.8 s; nodes `01`/`0B` answer from 3.2 s, the freezer (node `7F`) from 3.06 s. **[ok]**
- Group `3C` (identification) is **read by the box and answered by the freezer** (`82 82 7F 13 01 ...` reply to a `03 01` request list).
  It describes the freezer. Earlier in this session I wrongly called all of it "sent by the box". **[ok]**
- Text strings travel as consecutive flag-`80` records of 4 ASCII bytes (NUL padded), in id order. A string is just the records
  joined; a long block is paged over several replies (the identification block arrives in 3 to 4 replies). **[ok]**

## Freezer identification (group 3C, read-only)
| Records | Value (this unit) | Meaning |
|---------|-------------------|---------|
| `3C/01`-`03` | `FNc    5076` | model name (spaces are part of the string) |
| `3C/05`-`06` | `20A/001` | index / revision **[hyp]** |
| `3C/07`-`09` | `123456789` | 9-digit number, maybe the serial **[?]** |
| `3C/0A`-`0C` | `9935672-01` | part number **[hyp]** |
| `3C/0D`-`0F` | `993567201` | same, without the dash |
| `3C/10`-`12` | `987654321` | 9-digit number **[?]** |
| `3C/1B`-`1D` | `0000` | unused |

The IDs `<box-id-1>` and `<box-id-2>` that were searched for are **not** in this capture. They may belong to the box or the cloud only.

## Network record (group 48, written by the box)
| Record | Value seen | Meaning |
|--------|------------|---------|
| `48/00` | `03` at 9.2 s, `04` at 25.8 s | box connection state, `03` while starting, `04` later (cloud reached?) **[hyp]** |
| `48/03` | `00`, then `01` at 21.5 s | WiFi connected flag **[hyp]** |
| `48/04`-`0B` | `192.168.1.100@Example_Wifi` | text: `<ip>@<ssid>`; the 4-byte chunks split it as `192.` `168.` `1.10` `0@Ex` `ampl` `e_Wi` `fi` **[ok]** |
| `48/14` | `64 01 A8 C0` | the IP as a little-endian 32-bit value = 192.168.1.100, matches the text **[ok]** |
| `48/0C`-`0D` | `00 1D 63 12 34 56` | 6 bytes, looks like the box MAC address **[hyp]** |
| `48/10`, `48/12`, `48/1E`, `48/20`, `48/01`, `48/02` | small values / zeros | not understood **[?]** |

Note: the string carries `.100`, and the binary field agrees. The WiFi name is therefore `Example_Wifi`, the IP `192.168.1.100`.
The `4C` group (`4C/01` = 4 bytes that differ per write, `4C/02`, `4C/03` = 0) is written once or twice during startup; meaning unknown **[?]**.

## Software versions (tag `16 01`, ASCII)
`TOD 5-600|SW 1-86-7|BUS 1-4-0` is sent at 7.4 s and 10.95 s, `TOD 5-600|SW 6-2-0|BUS 1-4-0` at 12.8 s.
These are two different devices, not a loader and a main program **[ok]**:
- `SW 1-86-7` comes from the freezer (node `7F`): to node `01` (slot `81`, answer to `01 FF 01 06`) and to the box (slot `82`,
  answer to `02 FF 02 06`).
- `SW 6-2-0` comes from node `01`, answering the box directly: the box sends `02 81 02 06 01 00` (to node `01`) at 11.2 s,
  node `01` answers `01 82 01 16 ...` (to node `02`) at 12.8 s.
- The box's own version is never asked for. `BUS 1-4-0` is probably the bus protocol version shared by all nodes. **[hyp]**

## Timeline
| Time | Event |
|------|-------|
| 2.69 s | power-up glitch on all channels (decoded as two "breaks", not traffic); pad 3 rises 0 V -> 3.3 V at 2.705 s |
| 3.06 s | first real frame: the freezer's hello to node `0B`, then to `0C`, `7F`, `01`, `02` |
| 3.2 s | node `0B` answers (hello reply), node `01` at 3.4 s; slot `02` stays empty until 5.8 s (box still booting) |
| 3.1 to 7.8 s | handshake with message types that never occur in normal operation (below) |
| 7.4 s | first version string |
| 8.4 s | first identification read (3C) |
| 7.7 s on | the normal `03 01` read lists and `13 01` value replies start (75 each) |
| 9.2 s | box writes network state `03` |
| 12.8 s | second version string, `SW 6-2-0` |
| 21.5 s | box writes IP and SSID, connected flag `01` |
| 25.8 s | box writes `48/00 = 04` (three times) |
| ~34 s on | quiet steady state: the freezer's slot schedule (`01` every 285 ms, `02` every 570 ms, `0B`/`0C` every 142.5 ms), the 1 s event block, empty slots `0C`/`7E`/`7F` |

### Reproduced on the bench (2026-11-05)
The same start-up ran against the ESP8266 freezer emulator after a box power cycle (`esp-freezer-emulator-poc/logs/box_run6.txt`):
hello reply, `21`/`22`/`23` acks, table `13 00 00 00` → `13 00 00 04`, a `48` read, `48/00 = 03`, `4C/01..03`, `48/10`..,
version requests to `7F` (`FF 02 06`) and to node `01` (`81 02 06`), the `08` and `0C` lists, the `3C` identification,
then (~15 s later) the `<ip>@<ssid>` text and `48/00` = `04`, then `05`. The order and every request list match this capture.
- `48/00` has a third value, **`05`**, written ~5 s after `04` (twice), and the box then reads `48/00` back. Final
  state, probably "cloud connected" **[hyp]**.
- `4C/01` (`84 8C B3 A0 24`) was identical in all bench sessions of the same box. **[?]**
- If the freezer side restarts while the box keeps running, the box repeats only the handshake and the `48`/`4C` writes,
  not the reads. After an outage of minutes it stays degraded until power-cycled; see `01-protocol.md` §3.

## New message types seen only at startup
All frames follow `<slot> <dst> <src> <svc> <flag> <len> ...` (`protocol.py`: `<cmd> <marker> <second> <tag>`) with the CRC
from `02-commands.md`. Services already known: `03` read list, `13` values, `24` write/values, `30` ack. Replies are the
request + `0x10` (`05-bus-roles.md` section 6).

| Service | Count | Example | Reading |
|-----|-------|---------|---------|
| `01` | 5 | `8B 8B 7F 01 01 00` | **hello** from the freezer to nodes `0B`, `0C`, `7F`, `01`, `02` at 3.1 s; node `0C` never answers **[ok]** |
| `11` | 4 | `0B FF 0B 11 01 06 43 01 02 03 00 01` | **hello reply**: `43 01 02 03 00 01` from nodes `01`/`0B`, `03 01 00 00 00 00` from the box **[ok]** |
| `21`/`22`/`23` | 4 each | `8B 8B 7F 21 01 00` | init steps from the freezer to nodes `0B`, `01`, `02`; each acked with `30 01 02 <svc> 00` **[ok pairing, ? meaning]** |
| `02`/`12` | 2 each | `01 FF 01 02 01 38 00 00 00 00 01 00 00 00 02 ...` | table exchange: node `01` sends 14 4-byte entries, the freezer returns them with some fields filled (`FD`); the box sends `13 00 00 00`, gets `13 00 00 04` **[?]** |
| `06`/`16` | 3 each | `01 FF 01 06 01 00` | version request / version string, see above **[ok]** |

The parser treats all of these as `unknown` and passes them through byte for byte, so replay stays exact. They are handled in
`encode()` through the raw bytes.

## Parser changes this capture required
- Flags `82` and `84` carry 4 bytes (as `80`); seen on `4C/01..03`. **[ok]**
- A message whose tag is not known now round-trips unchanged instead of raising.
