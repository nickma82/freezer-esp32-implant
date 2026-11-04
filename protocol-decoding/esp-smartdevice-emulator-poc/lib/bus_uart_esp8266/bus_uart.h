// UART0 receive-only driver for the Freezer bus on the ESP8266 (design D2).
// RX on GPIO13 (IO swap + FUNCTION_4). The TX function is never routed: GPIO15 stays an input, GPIO1 is set to a
// high-Z input. Every byte is timestamped in the ISR with the 64-bit extended CPU cycle counter. A BREAK opens a
// new slot; completed slots go through a single-producer / single-consumer ring to loop().
//
// Bench finding (task 3.4, 2026-10-31, live bus, 40 s, 4 x 10 s S lines):
//   - does a BREAK push a 0x00 into the FIFO:  yes. brk_zero == ubd_irq (421/421), brk_nozero == 0
//   - does UIBD fire exactly once per BREAK:   yes. ubd_irq matches the ~42 slots/s of the bus, no spurious slots
//   - result: crc_bad=0, overruns=0, ring_drop=0; slot 02 box answer 425 us (Logic: 426 us)
// Wiring pitfall: Arduino's pinMode(13, FUNCTION_4) enables the internal pull-up. Together with the 4.7k series
// resistor that held the bus node flat (no bytes at all). begin() clears GPFPU/GPFPD after the mux setup.
// The ISR copes with both: a 0x00 that arrives together with UIBD/UIFR as the first byte of an interrupt is the BREAK
// itself and is discarded. The counters below make the behaviour visible (`brk_*` in the S line).
#pragma once
#include <stdint.h>

namespace bus_uart {

constexpr uint32_t CPU_MHZ = 160;
constexpr unsigned MAX_SLOT = 160;
constexpr unsigned RING = 6;

enum : uint8_t { F_FRAMING = 1, F_OVERRUN = 2, F_TRUNC = 4 };

struct Slot {
  uint64_t t0_cyc;           // cycle counter at the slot byte (end of its stop bit)
  uint16_t n;                // bytes including the slot byte
  uint8_t flags;
  uint8_t data[MAX_SLOT];
  uint32_t dt_cyc[MAX_SLOT];  // cycles since t0 for each byte
};

struct Counters {
  volatile uint32_t slots, frame_err, overrun, ring_drop;
  volatile uint32_t ubd_irq;       // interrupts with UIBD set
  volatile uint32_t brk_with_zero;  // BREAK where a 0x00 sat in the FIFO
  volatile uint32_t brk_no_zero;    // BREAK without a 0x00 byte
};

void begin();
bool pop(Slot& out);  // loop() side
const Counters& counters();

}  // namespace bus_uart
