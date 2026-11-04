## Context

See `proposal.md` (Why). Shared base: `openspec/changes/esp-poc-phase1-listener/design.md` (references, protocol card,
board, RX driver, core, log format) and `openspec/changes/esp-poc-phase2-node02/design.md` (TX stage, timed reply,
auto-disable, responder queue, CLI). After archiving, the same content is in `openspec/specs/esp-*`. Phase 3 adds only
application logic on top of the node-02 responder. It is pure C++ in the core and testable on the host.

Facts used (paths relative to `protocol-decoding/`):
- **Write frames and the meaning of the values:** `02-commands.md` (commands table), `01-protocol.md` §3 (parameters).
- **Event block layout and flag combinations:** `01-protocol.md` §2 "Event-flag block". "Value changed" = `01000000`/`08000000`, party = `00800000`/`04000000`, door alarm = `00000100`/`08000000`, counter `00/04`.
- **A write gets no reply slot:** the only confirmation is the event flag plus a later read (`05-bus-roles.md` §5, `01-protocol.md` §5).
- **Version paths and timing:** `03-bootup.md` "Software versions". Box → freezer `06`, reply in `82` 285 ms later. Box → node `01` `02 81 02 06`, node `01` answers `01 82 01 16 …` after 1.6 s.
- **Identity:** the box's paged `3C` read lists in `csv/freezer_bootup_hex.csv` (8.1 s, 17.5-19.2 s), parsed by `identity()`/`text()` in `src/protocol.py`.
- **Door:** node `0B` broadcasts `14/04`, and pushes `14/00` and `58/07` (`02-commands.md` door table). The alarm is `0C/09`.

## Goals / Non-Goals

**Goals:**
- The box's user functions on the console, sent byte-identical to the captures.
- A state model that stays correct without the operator polling: event-driven plus periodic.
- A stable `STATE` line format that a later bridge (Wi-Fi/MQTT) can consume without changing the firmware's bus logic.

**Non-Goals:**
- Wi-Fi, MQTT, Home Assistant. This is a separate change that consumes the `STATE` lines or the core directly.
- Parameters marked `[?]` in `01-protocol.md` (`0C/07`, `0C/2B`, group `24`/`58` meanings, the high byte of `0C/14`). They are logged raw and not interpreted.
- BottleTimer and network records `48`/`4C`.

## Decisions

### D1 Command layer on top of the phase-2 queue
- Each command maps to a core builder (`write_param`, `read_request`, version request) and queues one body.
- The queue order is handshake first, then user writes, then reads. That gives priority to the handshake and keeps user writes ahead of periodic reads.
- "One message per slot `02`" stays the rule, so a write plus its read-back takes at least two slot-`02` turns.
- Refuse while `tx` is off or the handshake is not finished. "Finished" means: no handshake message queued, and at least one idle or read answer since the last `21`/`22`/`23`.

### D2 Status read list
- **Standard list:** `0C/00` mode, `0C/06` set-point, `0C/14` current temperature, `0C/31` door, `0C/09` alarm, `08/02` party. It is the same as the phase-2 `read`.
- **Body:** `FF 02 03 01 12 0C 00 20 0C 06 40 0C 14 40 0C 31 20 0C 09 20 08 02 20 9B 42`.
- **Periodic every 10 s** by default (`interval <s>` on the console, 0 = off).
- **Event-triggered:** if any of the four flag words in an event block is non-zero, queue the standard read. Duplicates are coalesced: at most one read is queued or pending at a time.
- **Alternative rejected:** copying the box's long rotating read lists (`0C 15 …`, `08 15 …`, `0C 45 …`). Most of their parameters are not understood yet. The standard list is enough for every state field, and the box's lists stay available via `raw` for experiments.

### D3 Write confirmation state machine
- After a write is sent (its `T` line), start a 3 s window.
- If a "value changed" or "party changed" event block arrives, queue a read of the written parameter (or the standard list).
- **Confirmed** when the read-back value equals the written one. **Unconfirmed** when the window expires.
- Never resend automatically. A resend could toggle a function twice if the first write was applied but its confirmation got lost.
- Host test: replay the event block and `13` reply frames from `csv/superfrost_enable_hex.csv` and `csv/partymode_enable_hex.csv`, plus synthetic slot-`82` replies, through the state machine.

### D4 State model
- One struct of optional values, each with the timestamp of its last update.
- **Update sources:**
  - `13` replies to node `02`;
  - any `13` reply to node `01` that the listener sees (slot `81`, e.g. `0C/00`, `0C/31`), decoded by the same core;
  - node `0B`'s `14/04` broadcast for the door.
- Emit a `STATE …` line on every change and on `status`. Unknown values print as `?`.
- **Temperatures:** the signed low byte (int8 °C). The high byte of `0C/14` is logged raw (`temp_raw=`) because its meaning is `[?]`.

### D5 Version and identity
- **`version`:** queue `FF 02 06 01 00` to the freezer (reply svc `16` in slot `82`) and `81 02 06 01 00` to node `01` (reply in slot `01` with dst `82`). Correlate by service `16` and source, with timeouts of 1 s for `7F` and 3 s for `01`. Print the ASCII strings.
- **`identity`:** queue the paged `3C` lists from the bootup capture, rebuilt with the core's `read_request`. Group the replies and assemble the strings with the `text()` logic. Print them only; never write them to flash or to repository files.

### D6 Debug `raw`
- Available only after `debug on`. The destination must be `FF` or `81`, and the source byte is forced to `02`. The CRC is appended by the core, and the message is refused during the handshake.
- Logs a `W` warning line.
- Meant for experimenting with not-yet-understood reads, never for writes outside the verified table.

### D7 Console reference (added to the phase-2 commands)
| Command | Effect |
|---|---|
| `superfrost on\|off`, `party on\|off`, `setpoint <°C>` | writes (spec table), with confirmation |
| `status` | print the `STATE` line |
| `interval <s>` | periodic status read interval, 0 = off (default 10) |
| `version`, `identity` | queries (D5) |
| `debug on\|off`, `raw <hex>` | guarded raw body (D6) |

New log lines: `STATE …`, `W …` (warnings), `C <t> write=<param> result=confirmed|unconfirmed value=<v>`.

## Risks / Trade-offs

- **[Set-point range of the appliance unknown]** → the default is limited to the captured −18…−16 °C. Widening it means changing a constant after checking the app's range, which is deliberately not a console command.
- **[Event flag missed (event blocks come only about every 1 s)]** → the periodic read catches up within 10 s, and the confirmation window of 3 s covers about 3 event blocks.
- **[Node 01's reply to the version request is slow or missing]** → a 3 s timeout, reported as "no answer", which is not an error.
- **[Reads and writes compete for the single slot every 570 ms]** → the priority order in D1 and coalesced reads. A user write waits at most one or two slot-`02` turns, about 0.6-1.2 s.
- **Trade-off:** confirmation relies on an event flag whose meaning is `[hyp]`. Mitigated by the read-back, which is authoritative, and by reporting "unconfirmed" instead of guessing.

## Migration Plan

Builds on the phase-2 firmware, so phase 2 must have passed its bench verification first. No data migration. Rollback:
don't use the new commands, or flash the phase-2 build. Re-plugging the original box restores the factory setup.

## Open Questions

- The allowed set-point range in the Freezer app. Deferrable: it only changes the range constant.
- Whether node `01`/`0B`'s low-power mode delays its answers to the version request. Deferrable: it only affects the timeout value.
