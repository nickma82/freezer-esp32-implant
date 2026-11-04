#include "bus_uart.h"

#include <Arduino.h>
#include <ets_sys.h>

namespace bus_uart {

static Slot ring_[RING];
static volatile uint8_t head_ = 0, tail_ = 0;  // head: next to write (ISR), tail: next to read (loop)
static Slot cur_;
static bool open_ = false;
static bool discard_ = false;  // ring full: the slot being received is dropped
static Counters cnt_;
static uint64_t cyc_hi_ = 0;
static uint32_t cyc_last_ = 0;

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
  if (now < cyc_last_) cyc_hi_ += (uint64_t)1 << 32;  // extend to 64 bit; the ISR runs far more often than every 26.8 s
  cyc_last_ = now;
  uint64_t t = cyc_hi_ | now;

  bool brk = st & (1 << UIBD);
  bool fr = st & (1 << UIFR);
  if (brk) cnt_.ubd_irq++;
  bool first = true;
  bool zero_seen = false;
  while ((USS(0) >> USRXC) & 0xFF) {
    uint8_t b = (uint8_t)USF(0);
    if (first && b == 0x00 && (brk || fr)) {  // the BREAK itself (framing error, all zero)
      first = false;
      zero_seen = true;
      finish_slot();
      start_slot();
      brk = false; fr = false;
      cnt_.brk_with_zero++;
      continue;
    }
    first = false;
    if (brk) { finish_slot(); start_slot(); brk = false; cnt_.brk_no_zero++; }  // BREAK before this byte, no 0x00 pushed
    if (!open_) continue;                                                       // bytes before the first BREAK
    if (cur_.n == 0) cur_.t0_cyc = t;
    if (cur_.n < MAX_SLOT) {
      cur_.data[cur_.n] = b;
      cur_.dt_cyc[cur_.n] = (uint32_t)(t - cur_.t0_cyc);
      cur_.n++;
    } else cur_.flags |= F_TRUNC;
  }
  if (brk) { finish_slot(); start_slot(); cnt_.brk_no_zero++; }  // UIBD with an empty FIFO
  if (fr && !zero_seen && open_) { cur_.flags |= F_FRAMING; cnt_.frame_err++; }
  if (st & (1 << UIOF)) { cnt_.overrun++; if (open_) cur_.flags |= F_OVERRUN; }
}

void begin() {
  ETS_UART_INTR_DISABLE();
  pinMode(1, INPUT);   // GPIO1: no TX function, high-Z (the console TX is UART1 on GPIO2)
  pinMode(3, INPUT);   // CP2102 side of the default RX pin
  pinMode(15, INPUT);  // strap pin, never driven in phase 1
  pinMode(13, FUNCTION_4);
  GPF(13) &= ~((1 << GPFPU) | (1 << GPFPD));  // pinMode(FUNCTION_x) enables the internal pull-up; the bus node must see a high-Z input
  IOSWAP |= (1 << IOSWAPU0);  // U0RXD on GPIO13
  USD(0) = ESP8266_CLOCK / 57600;  // UART clock is 80 MHz regardless of the CPU frequency
  USC0(0) = (3 << UCBN) | (1 << UCSBN);  // 8N1: 8 data bits (UCBN=3), 1 stop bit (UCSBN=1)
  USC0(0) |= (1 << UCRXRST) | (1 << UCTXRST);
  USC0(0) &= ~((1 << UCRXRST) | (1 << UCTXRST));
  USC1(0) = (1 << UCFFT);  // RX FIFO full threshold 1: interrupt per byte
  USIC(0) = 0xFFFF;
  USIE(0) = (1 << UIFF) | (1 << UIBD) | (1 << UIFR) | (1 << UIOF);
  ETS_UART_INTR_ATTACH(isr, nullptr);
  ETS_UART_INTR_ENABLE();
}

bool pop(Slot& out) {
  uint8_t t = tail_;
  if (t == head_) return false;
  out = ring_[t];
  tail_ = (uint8_t)((t + 1) % RING);
  return true;
}

const Counters& counters() { return cnt_; }

}  // namespace bus_uart
