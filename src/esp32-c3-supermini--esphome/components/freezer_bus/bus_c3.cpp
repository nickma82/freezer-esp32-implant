#if defined(ESP_PLATFORM)
#include "bus_c3.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#include "esp_timer.h"
#include "hal/uart_ll.h"
#include "soc/gpio_sig_map.h"

// Everything in this file runs in the bus task (except start(), submit(), snapshot(), pop_*()). It never logs and never
// allocates: results go out through queues / the snapshot and the component logs them from its own loop().

namespace esphome {
namespace freezer_bus {

namespace {
constexpr uart_port_t BUS_UART = UART_NUM_1;
constexpr uint8_t SLOT_BOX = 0x02;
constexpr uint32_t BYTE_US = 174;  // 10 bit times at 57600
constexpr uint32_t TICK_US = 50000;
constexpr uint32_t PUBLISH_US = 100000;

inline uint64_t now_us() { return (uint64_t) esp_timer_get_time(); }
inline TickType_t ms_ticks(uint32_t ms) {
  TickType_t t = pdMS_TO_TICKS(ms);
  return t ? t : 1;
}
}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------------------------------------------

// The pad is routed through the GPIO matrix by hand instead of uart_set_pin(): uart_set_pin() would switch the pin to a
// push-pull output for a moment. Here the order is: level 1 (released in open-drain), input+output-open-drain, then
// connect UART1 TX/RX, then open-drain asserted once more. The pin is never a push-pull output. Pulls stay off: the
// pull-up is external (freezer / emulator).
// gpio_set_direction() must not come after the TX connection: it calls gpio_output_enable(), which routes the pad back
// to "simple GPIO output" and cuts UART TX off (bench 2026-11-17: C3 received fine but never drove the bus).
// esp_rom_gpio_connect_out_signal() enables the output itself and leaves the pad's open-drain bit (set above) alone.
bool BusC3::route_pin() {
  const gpio_num_t p = (gpio_num_t) this->pin_;
  esp_rom_gpio_pad_select_gpio(p);
  gpio_set_pull_mode(p, GPIO_FLOATING);
  gpio_set_level(p, 1);
  if (gpio_set_direction(p, GPIO_MODE_INPUT_OUTPUT_OD) != ESP_OK) return false;
  esp_rom_gpio_connect_out_signal(p, U1TXD_OUT_IDX, false, false);
  esp_rom_gpio_connect_in_signal(p, U1RXD_IN_IDX, false);
  return true;
}

bool BusC3::start() {
  this->cmd_q_ = xQueueCreate(8, sizeof(Cmd));
  this->evt_q_ = xQueueCreate(16, sizeof(BusEvent));
  if (!this->cmd_q_ || !this->evt_q_) return false;

  uart_config_t cfg = {};
  cfg.baud_rate = 57600;
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_DEFAULT;
  // RX ring 1 KiB, no TX ring (uart_write_bytes() fills the 128 byte FIFO directly), 32 queued events
  if (uart_driver_install(BUS_UART, 1024, 0, 32, &this->uart_q_, 0) != ESP_OK) return false;
  if (uart_param_config(BUS_UART, &cfg) != ESP_OK) return false;
  if (!this->route_pin()) return false;
  uart_set_rx_full_threshold(BUS_UART, 1);  // every byte raises an event
  uart_set_rx_timeout(BUS_UART, 1);
  uart_flush_input(BUS_UART);
  xQueueReset(this->uart_q_);

  this->snap_ = BusSnapshot();
  return xTaskCreate(&BusC3::task_trampoline, "freezer_bus", 6144, this, configMAX_PRIORITIES - 2, &this->task_) ==
         pdPASS;
}

// ---------------------------------------------------------------------------------------------------------------
// Component-side API
// ---------------------------------------------------------------------------------------------------------------

bool BusC3::submit(const Cmd &c) { return xQueueSend(this->cmd_q_, &c, 0) == pdTRUE; }

void BusC3::snapshot(BusSnapshot &out) {
  taskENTER_CRITICAL(&this->snap_mux_);
  out = this->snap_;
  taskEXIT_CRITICAL(&this->snap_mux_);
}

bool BusC3::pop_event(BusEvent &out) { return xQueueReceive(this->evt_q_, &out, 0) == pdTRUE; }

bool BusC3::pop_log(LogFrame &out) {
  uint32_t tail = this->log_tail_.load(std::memory_order_relaxed);
  if (tail == this->log_head_.load(std::memory_order_acquire)) return false;
  out = this->log_ring_[tail % LOG_RING];
  this->log_tail_.store(tail + 1, std::memory_order_release);
  return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Bus task
// ---------------------------------------------------------------------------------------------------------------

void BusC3::task_main() {
  this->last_tick_ = now_us();
  this->refresh_answer(this->last_tick_);
  this->publish_snapshot(this->last_tick_);
  uart_event_t ev;
  for (;;) {
    // while an answer is on the wire the echo must be judged soon: poll faster
    const TickType_t wait = this->tx_active_ ? ms_ticks(2) : ms_ticks(20);
    if (xQueueReceive(this->uart_q_, &ev, wait) == pdTRUE) {
      do {
        this->handle_event(ev);
      } while (xQueueReceive(this->uart_q_, &ev, 0) == pdTRUE);
    }
    uint64_t now = now_us();
    if (this->tx_active_) {
      // no (complete) echo within the time the answer needs on the wire: we cannot verify it, so stop transmitting
      if (now - this->tx_t_start_ > (uint64_t) this->tx_len_ * BYTE_US + 4000) this->echo_mismatch(1, (uint16_t) this->tx_echoed_, 0xFFFF, 0xFFFF);
      continue;  // commands and ticks wait: the engine must keep the offered message until the echo is judged
    }
    this->drain_commands();
    if (now - this->last_tick_ >= TICK_US) {
      this->engine_.tick(now);
      this->last_tick_ = now;
      this->need_refresh_ = true;
    }
    this->drain_write_results();
    if (this->need_refresh_) this->refresh_answer(now);
    this->publish_snapshot(now);
  }
}

void BusC3::handle_event(const uart_event_t &ev) {
  switch (ev.type) {
    case UART_DATA: {
      // exactly the bytes this event announced, in event order: BREAK / FRAME_ERR events interleave correctly
      uint8_t buf[UART_HW_FIFO_LEN(BUS_UART)];
      size_t want = ev.size < sizeof buf ? ev.size : sizeof buf;
      int n = uart_read_bytes(BUS_UART, buf, want, 0);
      uint64_t t = now_us();  // upper bound of the end of the last byte (see README: latency approximation)
      if (n > 0) this->on_data(buf, (size_t) n, t);
      break;
    }
    case UART_BREAK:
      this->on_break();
      break;
    case UART_FRAME_ERR:
      this->on_frame_error();
      break;
    case UART_FIFO_OVF:
    case UART_BUFFER_FULL:
      this->on_overflow();
      break;
    default:
      break;  // parity (not used), pattern detect (not enabled), ...
  }
}

void BusC3::on_data(const uint8_t *buf, size_t n, uint64_t t) {
  for (size_t i = 0; i < n; i++) this->on_byte(buf[i], t);
}

void BusC3::on_byte(uint8_t b, uint64_t t) {
  if (!this->in_slot_) return;  // before the first BREAK (or after an overflow): resync on the next BREAK
  if (this->cur_n_ == 0) {      // first byte after the BREAK = slot byte
    if (this->zero_pending_) {
      this->zero_pending_ = false;
      if (b == 0x00) return;  // the 00 the UART reports for the BREAK itself, arrived after the BREAK event
    }
    this->cur_t_ = t;
    this->cur_lat_ = -1;
    this->cur_[0] = b;
    this->cur_n_ = 1;
    if (b == SLOT_BOX) this->reply_to_slot02(t);
    return;
  }
  if (this->cur_n_ < sizeof this->cur_) this->cur_[this->cur_n_++] = b;
  else this->cur_overflow_ = true;
  if (this->cur_[0] == SLOT_BOX && this->tx_active_) this->echo_byte(b);
}

void BusC3::on_frame_error() {
  // a framing error while our own answer is on the wire = the line is not what we drive
  if (this->tx_active_) this->echo_mismatch(3, (uint16_t) this->tx_echoed_, 0xFFFF, 0xFFFF);
}

void BusC3::on_overflow() {
  this->uart_overflows_++;
  this->stats_dirty_ = true;
  uart_flush_input(BUS_UART);
  xQueueReset(this->uart_q_);
  this->cur_n_ = 0;
  this->in_slot_ = false;  // resync on the next BREAK
  this->zero_pending_ = false;
  if (this->tx_active_) this->echo_mismatch(1, (uint16_t) this->tx_echoed_, 0xFFFF, 0xFFFF);
}

// The completed slot: a BREAK ends it. The UART reports every BREAK as a 00 byte, and on the C3 that 00 arrives
// before the BREAK event, so it sits at the end of the previous slot's bytes and is removed here. A FRAME_ERR is not
// reliably reported before the BREAK (bench 2026-11-17: never), so it is not required. A legitimate trailing 00 of a
// frame survives, since only the one extra 00 is dropped. If the 00 arrives late instead, zero_pending_ drops it as the
// first byte.
void BusC3::finish_frame() {
  bool stripped = false;
  if (this->in_slot_ && this->cur_n_ >= 2 && this->cur_[this->cur_n_ - 1] == 0x00) {
    this->cur_n_--;
    stripped = true;
  }
  this->zero_pending_ = !stripped;
  if (this->in_slot_ && this->cur_n_ > 0) {
    // slot 02 after our answer: nothing may follow the complete echo
    if (this->cur_[0] == SLOT_BOX && this->tx_len_ && this->tx_echoed_ == this->tx_len_ && this->cur_n_ != 1 + this->tx_len_ &&
        this->cur_lat_ >= 0) {
      this->echo_mismatch(2, (uint16_t) this->tx_len_, 0xFFFF, 0xFFFF);
    }
    if (this->cur_overflow_) {
      this->uart_overflows_++;
      this->stats_dirty_ = true;
    } else {
      this->engine_.on_frame(this->cur_, this->cur_n_, this->cur_t_);
      this->push_log(this->cur_, this->cur_n_, this->cur_t_, this->cur_lat_);
    }
    this->need_refresh_ = true;
  }
}

void BusC3::on_break() {
  // BREAK while the echo of our answer is incomplete: the answer did not come back as sent
  if (this->tx_active_) this->echo_mismatch(1, (uint16_t) this->tx_echoed_, 0xFFFF, 0xFFFF);
  this->finish_frame();
  this->cur_n_ = 0;
  this->cur_overflow_ = false;
  this->in_slot_ = true;
  this->tx_len_ = 0;
  this->tx_echoed_ = 0;
  // pre-build the answer for the next slot 02 right away (the slot byte follows >= one byte time later)
  this->refresh_answer(now_us());
}

// ---------------------------------------------------------------------------------------------------------------
// Reply path
// ---------------------------------------------------------------------------------------------------------------

// Called with the time at which the task read the slot byte 02. That time is the best available estimate of "end of the
// slot byte": the true end is earlier by the interrupt/event-queue/task wake-up latency (unknown, tens of us, more under
// Wi-Fi load) and about half a bit time (the UART raises its interrupt when the stop bit is sampled).
void BusC3::reply_to_slot02(uint64_t t) {
  if (!this->engine_.tx_enabled() || this->ans_n_ == 0) return;  // slot 02 stays empty
  uint64_t now = now_us();
  if (now > t + REPLY_DELAY_MAX_US) {  // woke too late: never answer late
    this->missed_slots_++;
    this->stats_dirty_ = true;
    this->engine_.on_slot02_result(false);
    return;
  }
  const uint64_t target = t + this->reply_delay_us_;
  while ((now = now_us()) < target) {
  }
  if (now > t + REPLY_DELAY_MAX_US) {  // preempted during the wait
    this->missed_slots_++;
    this->stats_dirty_ = true;
    this->engine_.on_slot02_result(false);
    return;
  }
  memcpy(this->tx_buf_, this->ans_, this->ans_n_);
  this->tx_len_ = this->ans_n_;
  this->tx_echoed_ = 0;
  this->tx_active_ = true;
  this->tx_t_start_ = now;
  // <= 96 bytes: goes straight into the 128 byte TX FIFO and returns without waiting for the wire
  uart_write_bytes(BUS_UART, this->tx_buf_, this->tx_len_);
  const uint64_t t_done = now_us();
  const uint32_t lat = (uint32_t) (t_done - t);  // slot byte read -> FIFO filled (first start bit is within this)
  this->cur_lat_ = (int32_t) lat;
  this->record_latency(lat);
}

void BusC3::echo_byte(uint8_t b) {
  if (this->tx_echoed_ >= this->tx_len_) return;
  if (b != this->tx_buf_[this->tx_echoed_]) {
    this->echo_mismatch(0, (uint16_t) this->tx_echoed_, this->tx_buf_[this->tx_echoed_], b);
    return;
  }
  if (++this->tx_echoed_ == this->tx_len_) {  // whole answer heard back exactly as sent
    this->tx_active_ = false;
    this->engine_.on_slot02_result(true);
    this->need_refresh_ = true;
  }
}

void BusC3::abort_tx() {
  // drop what is still queued in the TX FIFO; a byte already shifting out finishes (<= 174 us)
  uart_ll_txfifo_rst(UART_LL_GET_HW(BUS_UART));
}

void BusC3::echo_mismatch(uint8_t reason, uint16_t idx, uint16_t expected, uint16_t got) {
  this->abort_tx();
  const bool was_active = this->tx_active_;
  this->tx_active_ = false;
  this->tx_echoed_ = 0;
  this->tx_len_ = 0;
  if (was_active) this->engine_.on_slot02_result(false);  // not committed: stays queued for a later re-enable
  this->engine_.set_tx_enabled(false);
  strncpy(this->tx_off_reason_, "echo mismatch", sizeof this->tx_off_reason_ - 1);
  this->echo_mismatches_++;
  this->stats_dirty_ = true;
  this->need_refresh_ = true;
  BusEvent e;
  e.kind = BusEvent::Kind::EchoMismatch;
  e.idx = idx;
  e.expected = expected;
  e.got = got;
  e.reason = reason;
  this->push_event(e);
}

// ---------------------------------------------------------------------------------------------------------------
// Commands, answer, snapshot
// ---------------------------------------------------------------------------------------------------------------

void BusC3::drain_commands() {
  Cmd c;
  while (xQueueReceive(this->cmd_q_, &c, 0) == pdTRUE) {
    n02::Result r = n02::Result::Accepted;
    BusEvent e;
    e.kind = BusEvent::Kind::Refusal;
    e.value = c.value;
    switch (c.type) {
      case CmdType::SuperFrost:
        e.param = n02::WriteParam::SuperFrost;
        r = this->engine_.request_superfrost(c.value != 0);
        break;
      case CmdType::Party:
        e.param = n02::WriteParam::Party;
        r = this->engine_.request_party(c.value != 0);
        break;
      case CmdType::Setpoint:
        e.param = n02::WriteParam::Setpoint;
        r = this->engine_.request_setpoint(c.value);
        break;
      case CmdType::TxEnable:
        this->engine_.set_tx_enabled(c.value != 0);
        if (c.value) this->tx_off_reason_[0] = 0;
        else strncpy(this->tx_off_reason_, "operator", sizeof this->tx_off_reason_ - 1);
        this->stats_dirty_ = true;
        break;
      case CmdType::Network:
        this->engine_.set_network(c.connected, c.ip, c.ssid, c.mac);
        break;
    }
    if (r != n02::Result::Accepted) {
      e.code = (uint8_t) r;
      this->push_event(e);
    }
    this->need_refresh_ = true;
  }
}

void BusC3::drain_write_results() {
  n02::WriteResult w;
  while (this->engine_.take_write_result(w)) {
    BusEvent e;
    e.kind = BusEvent::Kind::WriteResult;
    e.param = w.param;
    e.value = w.value;
    e.code = (uint8_t) w.outcome;
    this->push_event(e);
  }
}

// The engine's offered message always is the one of the LAST next_answer() call, so this must run after every engine
// change and before the slot byte 02 (it does: BREAK, commands, ticks). Never while an answer is awaiting its echo.
void BusC3::refresh_answer(uint64_t now) {
  if (this->tx_active_) return;
  this->ans_n_ = this->engine_.next_answer(this->ans_, sizeof this->ans_, now);
  this->need_refresh_ = false;
}

void BusC3::record_latency(uint32_t us) {
  this->lat_win_[this->lat_count_ % LAT_WINDOW] = us > 0xFFFF ? 0xFFFF : (uint16_t) us;
  this->lat_count_++;
  this->stats_dirty_ = true;
}

void BusC3::publish_snapshot(uint64_t now) {
  const uint32_t change = this->engine_.change_counter();
  if (change == this->last_change_ && !this->stats_dirty_ && now - this->last_pub_ < PUBLISH_US) return;
  this->last_change_ = change;
  this->last_pub_ = now;
  this->stats_dirty_ = false;

  BusSnapshot s;
  s.st = this->engine_.state();
  s.phase = this->engine_.phase();
  s.eng = this->engine_.counters();
  s.alive = this->engine_.bus_alive(now);
  s.tx_enabled = this->engine_.tx_enabled();
  s.change = change;
  s.echo_mismatches = this->echo_mismatches_;
  s.missed_slots = this->missed_slots_;
  s.uart_overflows = this->uart_overflows_;
  s.log_drops = this->log_drops_.load(std::memory_order_relaxed);
  memcpy(s.tx_off_reason, this->tx_off_reason_, sizeof s.tx_off_reason);
  const uint32_t n = this->lat_count_ < LAT_WINDOW ? this->lat_count_ : (uint32_t) LAT_WINDOW;
  if (n) {
    uint16_t v[LAT_WINDOW];
    memcpy(v, this->lat_win_, n * sizeof(uint16_t));
    for (uint32_t i = 1; i < n; i++) {  // insertion sort, n <= 64
      uint16_t x = v[i];
      uint32_t j = i;
      while (j > 0 && v[j - 1] > x) {
        v[j] = v[j - 1];
        j--;
      }
      v[j] = x;
    }
    s.lat_median_us = (n & 1) ? v[n / 2] : ((uint32_t) v[n / 2 - 1] + v[n / 2]) / 2;
    s.lat_max_us = v[n - 1];
  }
  s.lat_n = n;
  s.seq = this->snap_.seq + 1;  // only this task writes snap_
  taskENTER_CRITICAL(&this->snap_mux_);
  this->snap_ = s;
  taskEXIT_CRITICAL(&this->snap_mux_);
}

void BusC3::push_event(const BusEvent &e) { xQueueSend(this->evt_q_, &e, 0); }

void BusC3::push_log(const uint8_t *f, size_t n, uint64_t t, int32_t lat) {
  if (n < 2) return;  // bare slot bytes are not logged
  const uint32_t head = this->log_head_.load(std::memory_order_relaxed);
  if (head - this->log_tail_.load(std::memory_order_acquire) >= LOG_RING) {
    this->log_drops_.fetch_add(1, std::memory_order_relaxed);
    this->stats_dirty_ = true;
    return;
  }
  LogFrame &l = this->log_ring_[head % LOG_RING];
  l.t_us = t;
  l.lat_us = lat;
  l.n = (uint8_t) n;
  memcpy(l.data, f, n);
  this->log_head_.store(head + 1, std::memory_order_release);
}

}  // namespace freezer_bus
}  // namespace esphome

#endif  // ESP_PLATFORM
