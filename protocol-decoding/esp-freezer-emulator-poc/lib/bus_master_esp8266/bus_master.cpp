// RX ISR and ring are copied from ../../../esp-smartdevice-emulator-poc/lib/bus_uart_esp8266/bus_uart.cpp (phase 1, bench
// verified there). Differences: the shared baud divisor is 1401 (~57100 baud, the freezer's clock), the TX side is added,
// and the ISR also accepts our own break (see self_break_) as a BREAK.
#include "bus_master.h"

#include <Arduino.h>
#include <ets_sys.h>

namespace bus_master {

static constexpr uint32_t DIV_BIT = 1401;    // 80 MHz / 1401 = 57102 baud (freezer measured 57090)
static constexpr uint32_t DIV_BREAK = 2804;  // 9 bit times (start + 8 zero bits) = 315 us of low
static constexpr uint32_t BREAK_DONE_US = 360;   // break byte incl. stop bit has left the shifter, divisor may change
static constexpr uint32_t SLOT_AT_US = 405;      // slot byte starts this long after the break start (315 + ~90)
static constexpr uint32_t BODY_AFTER_US = 250;   // freezer answer pause: body starts 250 us after the slot byte start

static Slot ring_[RING];
static volatile uint8_t head_ = 0, tail_ = 0;
static Slot cur_;
static bool open_ = false;
static bool discard_ = false;
static Counters cnt_;
static uint64_t cyc_hi_ = 0;
static uint32_t cyc_last_ = 0;
static volatile bool tx_on_ = false;
// TX is UART1 (own divisor), RX is UART0 at DIV_BIT: our BREAK reaches the receiver through the bus as a real break
// (0x00 with framing error / UIBD). transmit() sets this flag as well, so the ISR treats the first 0x00 as the BREAK of a
// new slot either way.
static volatile bool self_break_ = false;

static inline uint32_t IRAM_ATTR ccount() {
  uint32_t c;
  asm volatile("rsr %0, ccount" : "=a"(c));
  return c;
}

static inline void IRAM_ATTR finish_slot() {
  if (!open_) return;
  open_ = false;
  if (discard_) { discard_ = false; return; }
  uint8_t h = head_, nx = (uint8_t)((h + 1) % RING);
  if (nx == tail_) { cnt_.ring_drop++; return; }
  ring_[h] = cur_;
  cnt_.slots++;
  head_ = nx;
}

static inline void IRAM_ATTR start_slot() { open_ = true; cur_.n = 0; cur_.flags = 0; }

static void IRAM_ATTR isr(void*, void*) {
  uint32_t now = ccount();
  uint32_t st = USIS(0);
  USIC(0) = st;
  if (now < cyc_last_) cyc_hi_ += (uint64_t)1 << 32;
  cyc_last_ = now;
  uint64_t t = cyc_hi_ | now;

  bool brk = st & (1 << UIBD);
  bool fr = st & (1 << UIFR);
  bool first = true;
  bool zero_seen = false;
  while ((USS(0) >> USRXC) & 0xFF) {
    uint8_t b = (uint8_t)USF(0);
    if (first && b == 0x00 && (brk || fr || self_break_)) {  // the BREAK itself (ours, or someone else's)
      if (self_break_) { self_break_ = false; cnt_.self_break++; }
      first = false;
      zero_seen = true;
      finish_slot();
      start_slot();
      brk = false; fr = false;
      continue;
    }
    first = false;
    if (brk) { finish_slot(); start_slot(); brk = false; }
    if (!open_) continue;
    if (cur_.n == 0) cur_.t0_cyc = t;
    if (cur_.n < MAX_SLOT) {
      cur_.data[cur_.n] = b;
      cur_.dt_cyc[cur_.n] = (uint32_t)(t - cur_.t0_cyc);
      cur_.n++;
    } else cur_.flags |= F_TRUNC;
  }
  if (brk) { finish_slot(); start_slot(); }
  if (fr && !zero_seen && open_) { cur_.flags |= F_FRAMING; cnt_.frame_err++; }
  if (st & (1 << UIOF)) { cnt_.overrun++; if (open_) cur_.flags |= F_OVERRUN; }
}

void begin() {
  ETS_UART_INTR_DISABLE();
  pinMode(1, INPUT);   // GPIO1: U0TXD is not used (UART0 is swapped), the console re-claims it after begin()
  pinMode(3, INPUT);
  // TX: UART1 TXD on GPIO2, open-drain, wired to the bus through 47 Ohm (no MOSFET). Output 0 pulls the bus low, output 1
  // releases it, so the box can pull the bus low in its own slot. GPIO2 is a strap pin (must be high at reset): the bus
  // pull-up keeps it high.
  pinMode(2, OUTPUT);
  digitalWrite(2, HIGH);
  GPC(2) |= (1 << GPCD);   // open-drain
  GPF(2) |= (1 << GPFPU);  // internal pull-up (weak, ~30-100 kOhm), in addition to the external 4.7 kOhm bus pull-up
  Serial1.begin(57102, SERIAL_8N1);  // stock UART1 init (TX on GPIO2); divisor is 80 MHz / 57102 = 1401 = DIV_BIT
  // RX: UART0 swapped to GPIO13 (also hears our own TX through the bus)
  pinMode(13, FUNCTION_4);
  GPF(13) &= ~((1 << GPFPU) | (1 << GPFPD));
  IOSWAP |= (1 << IOSWAPU0);  // U0RXD on GPIO13, U0TXD on GPIO15 (unused)
  USD(0) = DIV_BIT;
  USC0(0) = (3 << UCBN) | (1 << UCSBN);  // 8N1
  USC0(0) |= (1 << UCRXRST) | (1 << UCTXRST);
  USC0(0) &= ~((1 << UCRXRST) | (1 << UCTXRST));
  USC1(0) = (1 << UCFFT);
  USIC(0) = 0xFFFF;
  USIE(0) = (1 << UIFF) | (1 << UIBD) | (1 << UIFR) | (1 << UIOF);
  ETS_UART_INTR_ATTACH(isr, nullptr);
  ETS_UART_INTR_ENABLE();
  noInterrupts();
  pinMode(2, SPECIAL);      // route U1TXD to GPIO2; pinMode() resets pull-up and open-drain bits ...
  GPF(2) |= (1 << GPFPU);
  GPC(2) |= (1 << GPCD);    // ... so restore open-drain after routing
  interrupts();
}

bool pop(Slot& out) {
  uint8_t t = tail_;
  if (t == head_) return false;
  out = ring_[t];
  tail_ = (uint8_t)((t + 1) % RING);
  return true;
}

const Counters& counters() { return cnt_; }

void tx_enable(bool on) { tx_on_ = on; }

void hold_low(bool on) {
  if (on) USC0(1) |= (1 << UCBRK);
  else USC0(1) &= ~(1 << UCBRK);
}
bool tx_enabled() { return tx_on_; }

static inline void spin_until(uint32_t target) {
  while ((int32_t)(ccount() - target) < 0) {}
}

bool transmit(uint8_t slot, const uint8_t* body, size_t len) {
  if (!tx_on_ || len > MAX_TX) return false;
  uint32_t t0 = ccount();
  self_break_ = true;
  USD(1) = DIV_BREAK;
  USF(1) = 0x00;  // start bit + 8 zero bits = 315 us of bus low, then the stop bit releases the bus
  spin_until(t0 + BREAK_DONE_US * CPU_MHZ);
  USD(1) = DIV_BIT;
  spin_until(t0 + SLOT_AT_US * CPU_MHZ);
  uint32_t ts = ccount();
  USF(1) = slot;
  if (len) {
    spin_until(ts + BODY_AFTER_US * CPU_MHZ);
    for (size_t i = 0; i < len; i++) {
      while (((USS(1) >> USTXC) & 0xFF) >= 127) {}  // FIFO full: wait one byte time (only for bodies > 127 bytes)
      USF(1) = body[i];
    }
  }
  return true;
}

}  // namespace bus_master
