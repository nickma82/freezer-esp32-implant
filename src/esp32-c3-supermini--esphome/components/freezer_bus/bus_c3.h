// ESP32-C3 bus driver (ESP-IDF only): UART1 on one open-drain pin, BREAK framing, slot-02 reply path, echo check.
//
// Compiled only for ESP-IDF builds, so the host tests (which build node02_engine.cpp alone) are not affected.
// Spec: openspec/changes/esphome-c3-implant/specs/c3-bus-driver/spec.md, design D2/D4/D5.
//
// THREADING: the bus task is the ONLY caller of the n02::Engine (frames, tick, commands, state snapshots).
//   - HA commands (component loop) -> submit() -> FreeRTOS queue -> drained by the bus task.
//   - results the component must react to (refusals, write outcomes, echo mismatch) -> pop_event().
//   - snapshot() copies the state the bus task last published (spinlock-protected copy).
//   - frame copies for DEBUG logging -> single-producer/single-consumer ring, pop_log().
// No logging, no heap allocation and no blocking calls (except the planned reply busy-wait) in the byte path.
#pragma once
#if defined(ESP_PLATFORM)

#include <stddef.h>
#include <stdint.h>

#include <atomic>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freezer_bus.h"
#include "node02_engine.h"

namespace esphome {
namespace freezer_bus {

constexpr size_t LAT_WINDOW = 64;
constexpr uint32_t REPLY_DELAY_MIN_US = 250;
constexpr uint32_t REPLY_DELAY_MAX_US = 600;  // also the "never late" limit: no answer starts later than this

struct BusSnapshot {
  uint32_t seq = 0;  // bumped on every publish by the bus task
  n02::State st;
  n02::Phase phase = n02::Phase::Waiting;
  n02::Counters eng;
  bool alive = false;
  bool tx_enabled = true;
  uint32_t change = 0;  // engine change counter
  uint32_t echo_mismatches = 0;
  uint32_t missed_slots = 0;   // slot-02 windows we could not answer in time
  uint32_t uart_overflows = 0;
  uint32_t log_drops = 0;      // frame copies dropped because the log ring was full
  uint32_t lat_median_us = 0;  // over the last LAT_WINDOW answers (0 = none yet)
  uint32_t lat_max_us = 0;
  uint32_t lat_n = 0;
  char tx_off_reason[20] = {0};  // "", "operator", "echo mismatch"
};

enum class CmdType : uint8_t { SuperFrost, Party, Setpoint, TxEnable, Network };
struct Cmd {
  CmdType type = CmdType::TxEnable;
  int value = 0;  // bool for SuperFrost/Party/TxEnable, degC for Setpoint
  bool connected = false;
  uint8_t ip[4] = {0};
  uint8_t mac[6] = {0};
  char ssid[n02::SSID_MAX + 1] = {0};
};

struct BusEvent {
  enum class Kind : uint8_t { Refusal, WriteResult, EchoMismatch } kind = Kind::Refusal;
  n02::WriteParam param = n02::WriteParam::SuperFrost;
  int value = 0;
  uint8_t code = 0;  // Refusal: n02::Result, WriteResult: n02::WriteOutcome
  // EchoMismatch: index in the answer, expected and received byte (0xFFFF = none); reason 0 byte, 1 short, 2 extra, 3 frame error
  uint16_t idx = 0, expected = 0, got = 0;
  uint8_t reason = 0;
};

struct LogFrame {
  uint64_t t_us = 0;
  int32_t lat_us = -1;  // reply latency for our own slot-02 answer, else -1
  uint8_t n = 0;
  uint8_t data[lb::MAX_FRAME];  // slot byte first
};

class BusC3 {
 public:
  void configure(uint8_t pin, uint32_t reply_delay_us, uint32_t status_period_ms, bool network_writes) {
    this->pin_ = pin;
    this->reply_delay_us_ = reply_delay_us < REPLY_DELAY_MIN_US ? REPLY_DELAY_MIN_US
                            : reply_delay_us > REPLY_DELAY_MAX_US ? REPLY_DELAY_MAX_US
                                                                   : reply_delay_us;
    this->engine_.set_status_period_ms(status_period_ms);
    this->engine_.set_network_writes_enabled(network_writes);
  }
  // Configures UART1 + the pin (open-drain only, never push-pull) and starts the bus task. False on failure.
  bool start();

  bool submit(const Cmd &c);                // false if the command queue is full
  void snapshot(BusSnapshot &out);          // component loop
  bool pop_event(BusEvent &out);            // component loop
  bool pop_log(LogFrame &out);              // component loop (SPSC)

 protected:
  // ---- bus task ----
  static void task_trampoline(void *arg) { static_cast<BusC3 *>(arg)->task_main(); }
  void task_main();
  void handle_event(const uart_event_t &ev);
  void on_data(const uint8_t *buf, size_t n, uint64_t t);
  void on_byte(uint8_t b, uint64_t t);
  void on_break();
  void on_frame_error();
  void on_overflow();
  void finish_frame();
  void reply_to_slot02(uint64_t t_slot_byte);
  void echo_byte(uint8_t b);
  void echo_mismatch(uint8_t reason, uint16_t idx, uint16_t expected, uint16_t got);
  void abort_tx();
  void drain_commands();
  void refresh_answer(uint64_t now);
  void publish_snapshot(uint64_t now);
  void push_event(const BusEvent &e);
  void push_log(const uint8_t *f, size_t n, uint64_t t, int32_t lat);
  void drain_write_results();
  void record_latency(uint32_t us);
  bool route_pin();

  uint8_t pin_ = 3;
  uint32_t reply_delay_us_ = 420;

  n02::Engine engine_;  // touched by the bus task only
  QueueHandle_t uart_q_ = nullptr;
  QueueHandle_t cmd_q_ = nullptr;
  QueueHandle_t evt_q_ = nullptr;
  TaskHandle_t task_ = nullptr;

  // frame assembly (bus task)
  uint8_t cur_[lb::MAX_FRAME];
  size_t cur_n_ = 0;
  uint64_t cur_t_ = 0;
  bool cur_overflow_ = false;
  bool in_slot_ = false;        // a BREAK was seen: bytes belong to a slot
  bool fe_pending_ = false;     // a FRAME_ERR event since the last BREAK (the 00 byte of the BREAK is in the data)
  bool zero_pending_ = false;   // the BREAK's 00 byte has not been attributed yet: drop a leading 00
  int32_t cur_lat_ = -1;

  // reply / echo (bus task)
  uint8_t ans_[n02::MAX_ANSWER];
  size_t ans_n_ = 0;
  uint8_t tx_buf_[n02::MAX_ANSWER];
  size_t tx_len_ = 0;
  size_t tx_echoed_ = 0;
  bool tx_active_ = false;      // answer on the wire, echo not complete yet
  bool tx_aborted_ = false;     // this slot's transmission was aborted (mismatch)
  uint64_t tx_t_start_ = 0;
  bool need_refresh_ = true;
  uint64_t last_tick_ = 0;
  uint64_t last_pub_ = 0;
  uint32_t last_change_ = 0xFFFFFFFFu;

  // stats (bus task writes; copied into the snapshot)
  uint32_t echo_mismatches_ = 0, missed_slots_ = 0, uart_overflows_ = 0;
  char tx_off_reason_[20] = {0};
  uint16_t lat_win_[LAT_WINDOW];
  uint32_t lat_count_ = 0;  // total recorded; window index = count % LAT_WINDOW
  bool stats_dirty_ = true;

  portMUX_TYPE snap_mux_ = portMUX_INITIALIZER_UNLOCKED;
  BusSnapshot snap_;

  static constexpr size_t LOG_RING = 16;
  LogFrame log_ring_[LOG_RING];
  std::atomic<uint32_t> log_head_{0}, log_tail_{0}, log_drops_{0};
};

}  // namespace freezer_bus
}  // namespace esphome

#endif  // ESP_PLATFORM
