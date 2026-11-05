// Bus driver for the freezer emulator on the ESP8266: RX as in ../../../esp-smartdevice-emulator-poc/lib/bus_uart_esp8266
// (copied, see the note in bus_master.cpp) plus a transmitter that can send BREAK + slot byte + body.
// RX: UART0 RXD on GPIO13 (IO swap, FUNCTION_4). TX: UART1 TXD on GPIO2 (D4), open-drain, to the bus through 47 Ohm
// (see WIRING.md). Our own bytes come back on RX like on the real bus, so the ring also holds what we sent.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace bus_master {

constexpr uint32_t CPU_MHZ = 160;
constexpr unsigned MAX_SLOT = 160;
constexpr unsigned RING = 6;
constexpr size_t MAX_TX = 150;  // body bytes after the slot byte; beyond the 128 B TX FIFO transmit() waits for room

enum : uint8_t { F_FRAMING = 1, F_OVERRUN = 2, F_TRUNC = 4 };

struct Slot {
  uint64_t t0_cyc;  // cycle counter at the slot byte (end of its stop bit)
  uint16_t n;       // bytes including the slot byte
  uint8_t flags;
  uint8_t data[MAX_SLOT];
  uint32_t dt_cyc[MAX_SLOT];  // cycles since t0 for each byte
};

struct Counters {
  volatile uint32_t slots, frame_err, overrun, ring_drop, self_break;
};

void begin();
bool pop(Slot& out);  // loop() side
const Counters& counters();

// Transmit is off after begin(): the TX pin stays released (bus high) and transmit() does nothing.
void tx_enable(bool on);
// Hold the bus low (UART1 TX break) or release it: what the box sees when the freezer is switched off and the pull-up
// with it. Only for experiments; the master must be stopped.
void hold_low(bool on);
bool tx_enabled();

// Blocking (about 0.8 ms plus the body duration is NOT waited for): BREAK, slot byte, then the body 250 us after the
// slot byte start. len may be 0 (bare slot). Returns false if disabled or the body is too long.
bool transmit(uint8_t slot, const uint8_t* body, size_t len);

}  // namespace bus_master
