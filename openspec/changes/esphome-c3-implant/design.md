## Context

See `proposal.md` for the motivation and `specs/` for the requirements. The constraints that shape the approach:

- **Bus**: single-wire, open-drain, 57600 8N1. The freezer (node `7F`) sends every BREAK and slot byte and grants slot
  `02` every 570 ms. Observed answer pauses: freezer 250 µs, box 424–430 µs, node `01` 604–689 µs
  (`protocol-decoding/01-protocol.md` §1).
- **Existing code**:
  - `protocol-decoding/esp-smartdevice-emulator-poc/lib/freezer_bus/`: the `lb` core, pure C++17, no heap.
    It has CRC, `decode`/`encode`, `interpret`, and the node-02 builders `build_idle`, `build_hello_reply`,
    `build_init_ack`, `build_table_request`, `build_version_request`, `build_read_request`, `build_write_param`.
    Host tests exist in `test/test_core`.
  - `protocol-decoding/esp-freezer-emulator-poc/lib/freezer_master/`: `fm::Master`, a chip-independent freezer
    (schedule, handshake, captured answers in `captured_params.h`). Its host tests replay the captured handshake.
  - The ESP8266 drivers (`bus_uart_esp8266`, `bus_master_esp8266`) are chip-specific and are not reused.
- **Board** (corrected doc): ESP32-C3FH4, native USB-Serial/JTAG (console needs no pins), two UARTs, GPIO3 free of strap
  duties. ESPHome on ESP-IDF. Single core, so the bus task shares the CPU with Wi-Fi.
- **Tooling**: ESPHome is not installed on the dev machine. `just` is the entry point (user rule).

## Goals / Non-Goals

**Goals:**
- One protocol engine, testable on the host in a closed loop against `fm::Master`.
- Meet the 250–600 µs answer window on a single-core chip while Wi-Fi is active.
- Use ESPHome only as glue for Wi-Fi, API, OTA, logging and entities. Keep protocol logic out of ESPHome classes.

**Non-Goals:**
- Changing the `lb` core's behaviour. Additions are fine if the PoC's host tests still pass.
- An ESP8266 build of the implant.
- MQTT. The ESPHome native API is enough.
- Replaying the box's full steady-state read lists. The implant reads only what it exposes.

## Decisions

### D1: ESPHome external component, not a YAML lambda or the stock `uart:` component
Layout:

```
src/esp32-c3-supermini--esphome/
  freezer-implant.yaml        # board, wifi, api, ota, logger, freezer_bus + entities
  secrets.yaml.example        # secrets.yaml is git-ignored
  components/freezer_bus/    # __init__.py, switch.py, number.py, sensor.py, binary_sensor.py, text_sensor.py, C++
  test/                       # host tests (engine + closed loop)
  justfile  README.md  WIRING.md
```

The stock `uart:` component has no BREAK events, no open-drain single-pin mode and no µs-level reply path. A lambda
cannot hold a driver task. The component is loaded with `external_components: - source: { type: local, path: components }`.
- Alternative: an Arduino sketch with MQTT, which would mean writing Wi-Fi, OTA and HA discovery ourselves (rejected, H16).

### D2: Three layers
1. **`BusC3`** (ESP-IDF only). It owns UART1 and the pin, frames slots, sends the prepared answer, checks the echo and
   measures latency. It runs in its own FreeRTOS task.
2. **`Node02Engine`** (pure C++17, only `freezer_bus.h`). Input: decoded frames plus a µs clock. Output: the next
   answer body for slot `02`, state-change events and write results. It holds the start-up state machine, the queue
   (priority: handshake > write > read > idle), the state model, write confirmation and the restart on hello.
3. **`FreezerBusComponent`** (ESPHome). It configures `BusC3`, owns the engine and maps state to entities.

Threading: the bus task is the only caller of the engine. Commands from HA go into a small lock-free/mutex-protected
command queue that the engine drains on its next frame. State snapshots come back the same way, and the component's
`loop()` publishes them. This keeps all protocol timing out of ESPHome's main loop, which runs every ~16 ms.

### D3: Reuse the `lb` core by symlink
`components/freezer_bus/freezer_bus.{h,cpp}` are relative symlinks to
`protocol-decoding/esp-smartdevice-emulator-poc/lib/freezer_bus/`. ESPHome copies component sources into its build tree
and follows symlinks. `just check-core` builds once and verifies that the copied file in `.esphome/build/...` matches the
source.
- Alternative: move the core to a shared top-level `lib/` (cleaner, but it touches both PoC `platformio.ini` files; can
  be done later).
- Fallback: if a tool breaks on symlinks (Windows checkout, ESPHome dashboard add-on), use a plain copy with
  `just check-core` failing on drift.

### D4: Bus driver on ESP-IDF UART1, single open-drain pin
- `uart_driver_install(UART_NUM_1, …, event queue)`, `uart_param_config` 57600 8N1.
- `uart_set_pin(UART_NUM_1, pin, pin, …)`, then make the pad input + output open-drain:
  `gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT_OD)` and re-route with
  `esp_rom_gpio_connect_out_signal(pin, U1TXD_OUT_IDX, …)` / `esp_rom_gpio_connect_in_signal(pin, U1RXD_IN_IDX, …)`.
  This is the order the ESP8266 work found necessary: open-drain set **after** the routing (FREEZER-EMULATOR-FINDINGS §1).
- The internal pull-up stays off, because the pull-up is external (freezer, or the emulator's 4.7 kΩ).
- `uart_set_rx_full_threshold(UART_NUM_1, 1)` and `uart_set_rx_timeout(…, 1)`, so every byte wakes the task quickly.
  `UART_BREAK` / `UART_FRAME_ERR` events delimit slots, and a `00` with a framing error directly before the slot byte is dropped.
- Reply path: the engine keeps the answer for the *next* slot `02` pre-built in a buffer. When the task sees slot byte
  `02` right after a BREAK, it busy-waits on `esp_timer_get_time()` until the target delay (default 420 µs after the end of
  the slot byte, configurable 250–600 µs) and calls `uart_write_bytes`. Answers are ≤ 128 bytes (the box's longest is
  ~88), so they fit the TX FIFO in one go. If the deadline has already passed when the task wakes, it does not send and
  counts a missed slot (spec: never late).
- Task priority `configMAX_PRIORITIES - 2`, above the Wi-Fi/LwIP tasks. It runs only for a few µs per byte, so Wi-Fi is not
  starved.
- Echo: RX keeps receiving during TX. The bytes of our own slot are compared with the sent buffer. The first difference
  disables TX and reports it (spec `c3-bus-driver`).
- Alternative: a custom UART ISR via the HAL (`uart_ll_*`, `esp_intr_alloc`). It has lower and more deterministic
  latency, but ESP-IDF 5 dropped `uart_isr_register`, so it means owning the whole UART. This is the **fallback** if bench
  latency with the event path exceeds 600 µs under Wi-Fi load (task 4.6).

### D5: Boot-safe pin
GPIO3 resets to input (high-Z) and has no strap function. The component configures the pin only in `setup()`, and only as
open-drain, so no window exists in which it is push-pull. The ROM bootloader prints on UART0 (GPIO21/20) or USB, never on
GPIO3, unlike the ESP8266's boot text on GPIO2. The YAML schema rejects GPIO8/9/18/19 as `bus_pin`.

### D6: Start-up and steady state
State machine: `waiting` (no hello yet) → `handshake` (hello, `21`, `22`, table, `23`) → `startup` (versions, `3C`,
first status read, `48` writes) → `running`. When a hello to node `02` arrives in any state, the engine goes back to
`handshake` and clears the queue (spec: recovery). If no slot `02` arrives for 2 s, the bus is reported as not alive.
The state machine itself stays where it is until the next hello, because the freezer always restarts with a hello.

Reads use one `03` list with the eight status params and their captured request flags (`captured_params.h`: `0C/00 20`,
`08/02 20`, `0C/06 40`, `0C/09 20`, `0C/11 20`, `0C/12 40`, `0C/14 40`, `0C/31 20`). The freezer answers in slot `82` one
285 ms step later, and the engine matches the answer to the outstanding request. Period: 10 s, plus once after any event
block with a non-zero flag word, plus once ~1 s after each write for the confirmation.

Freezer replies to node `01` (`81 81 7F 13 …`) and node `0B`'s pushes are decoded passively too, because they contain
`0C/00`, `0C/31`, `0C/09` and `14/04`. This makes door changes appear faster than the 10 s period.

### D7: Group `48` writes
The text is chunked into 4-byte flag-`80` records exactly as `protocol.py` `wifi()` decodes it (NUL padding). The IP is
little-endian, as in `03-bootup.md`. The MAC comes from `esp_read_mac(…, ESP_MAC_WIFI_STA)`. Order and timing: copy of
FREEZER-EMULATOR-FINDINGS §3, with `05` written 5 s after `04`. Values come from ESPHome's `wifi` component (the
component waits for `wifi::global_wifi_component->is_connected()`). Option `network_writes: true|false`.

### D8: Entities follow the freezer, not the request
HA switch and number callbacks only enqueue a command. They do not `publish_state` optimistically. The entity changes
when the read-back arrives. A refused command (TX off, bus down, out of range) publishes the current freezer value again,
so HA snaps back. Set-point: an ESPHome `number` with min −28, max −15, step 1, plus an engine-side range check for
defence in depth.

### D9: Host tests in the closed loop
`just test` builds with the host `g++ -std=gnu++17` and Unity (or PlatformIO `native`, as the PoCs do; whichever needs no
new tooling beyond `pio`, which already exists for the PoCs):
- engine unit tests on captured frames (hello → `11`, `21` → ack, idle ack, write bodies byte-identical to `02-commands.md`);
- **closed loop**: `fm::Master` produces slots, the engine answers, and a fake clock steps 17.8 ms ticks. The test checks the
  handshake, `Hs::Running` in the master, the status values equal to `captured_params.h`, a write updating the master's
  table, an event-triggered re-read, and a restart after the master is reset.

### D10: Logging
ESPHome `logger` on `USB_SERIAL_JTAG`. Tag `freezer`: INFO for state changes and writes, DEBUG for every frame in the
PoC's `M <t_us> <slot> <lat> <crc> <hex> | <decoded>` format (reuse `lb::describe`), so that
`esp-smartdevice-emulator-poc/tools/compare.py` can still compare against Logic 2 exports.

## Risks / Trade-offs

- [The freezer's real answer tolerance is unknown; only 250–689 µs has been observed] → Default 420 µs (in the middle of
  what the box does), configurable. Bench check on the analyzer, then on the real freezer with the box unplugged.
- [Wi-Fi interrupts on the single core delay the bus task past 600 µs] → Measure latency under Wi-Fi load (OTA, API
  streaming). The missed-slot counter makes it visible, and missing an answer is harmless (an empty slot). Fallback: the
  HAL ISR in D4.
- [Set-point range −28…−15 is beyond the verified −18…−16] → It is the user's decision, recorded. The write frame format
  is verified, and the confirmation read-back shows what the freezer accepted.
- [Symlinked core breaks in some ESPHome setups] → `just check-core`. Fallback: a copy (D3).
- [The `73L33` regulator may not handle Wi-Fi peaks] → Bench only, USB powered. Measuring the current is a board-doc task,
  not a blocker for the emulator bench.
- [Sharing the bus with the emulator: both boards USB-powered from one laptop] → Common ground through USB. The
  emulator's WIRING safety notes apply. Never connect pads 1/3.
- [The handshake order follows the spec and the emulator bench (ack `21`, ack `22`, table request, ack `23`), but the
  real box in `freezer_bootup` sends the table request in place of the `22` ack, acks `22` after the `12` answer, and does
  not ack `23`] → Both orders work against `fm::Master`. If the real freezer is strict, switch to the captured order;
  that is a few lines in the engine's frame handling.
- [The engine sends the `48` writes after the start-up reads and only once Wi-Fi is up; the box wrote `48/00 = 03`
  before its reads] → No effect on the emulator. Check on the real freezer.
- [The freezer emulator does not emulate nodes `01`/`0B`] → Passive door updates and node-`01` traffic cannot be tested
  on the bench. They are covered by host tests with captured frames (`protocol-decoding/csv/*_hex.csv` vectors).

## Migration Plan

New firmware; nothing to migrate. Rollback on the bench is unplugging the board. Before the real freezer (a follow-up
change): the emulator bench is complete (tasks section 7), and the board doc's checklist items 1–10 are ticked.

## Open Questions

- Exact ESPHome version to pin. Pick the newest release that is at least two weeks old when task 2.1 runs.
- Whether the freezer itself needs the `48` writes at all (only observable on the real freezer; the option exists).
