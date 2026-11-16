// Node-02 protocol engine: the SmartDeviceBox's role on the freezer bus, as pure logic.
//
// Pure C++17. Includes only freezer_bus.h (the `lb` core) and std C headers: no ESPHome, no ESP-IDF, no heap, fixed
// buffers. Single threaded: ONE caller (the bus task) drives every method below. The component layer adds locking
// around it later. Spec: openspec/changes/esphome-c3-implant/specs/node02-engine/spec.md.
//
// How the driver uses it
//   - Every complete BREAK-delimited slot (slot byte first, CRC last; a bare slot byte is n == 1) goes into on_frame(),
//     with a monotonic microsecond timestamp. That includes slot 02: it is either empty (n == 1) or our own
//     transmission echoed back (the driver itself checks the echo against what it sent).
//   - Right after each received frame the driver calls next_answer() to pre-build the body for the NEXT slot 02. It is
//     cheap and has no side effects: calling it again returns the same answer until the state changes.
//   - After slot 02 has passed it calls on_slot02_result(true) if the body went out and the echo matched, or
//     on_slot02_result(false) if not (missed slot, TX disabled, echo mismatch). A not-sent message stays queued and is
//     offered again by the next next_answer().
//   - tick(now_us) at least every ~100 ms (timeouts, bus-alive, status period). Commands use the time of the last
//     on_frame/tick/next_answer call.
//   - The ESPHome glue reads state() / change_counter() to publish, and take_write_result() for write outcomes.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "freezer_bus.h"

namespace n02 {

// ---- timing (spec values; public so tests and glue can refer to them) ---------------------------------------
constexpr uint32_t NODE01_VERSION_WINDOW_MS = 2000;  // node 01's version answer is accepted for this long
constexpr uint32_t WRITE_CONFIRM_MS = 3000;          // read-back must match within this long after the write
constexpr uint32_t WRITE_FALLBACK_READ_MS = 1000;    // no event after a write: read back anyway after this long
constexpr uint32_t DEFAULT_STATUS_PERIOD_MS = 10000; // periodic status read
constexpr uint32_t BUS_ALIVE_MS = 2000;              // no slot 02 for this long = bus not alive
constexpr uint32_t NET_FINAL_DELAY_MS = 5000;        // 48/00 = 05 this long after 48/00 = 04
constexpr uint32_t REPLY_TIMEOUT_MS = 1500;          // a start-up/status request without a reply is given up after this
constexpr size_t MAX_ANSWER = 96;                    // longest body we ever build (box messages are <= ~88 bytes)
constexpr size_t SSID_MAX = 32;
constexpr size_t NET_TEXT_MAX = 32;                  // 48/04..0B hold 8 x 4 bytes; "<ip>@<ssid>" is cut to this

enum class Phase : uint8_t {
  Waiting,    // no hello from the freezer yet: idle acks only
  Handshake,  // hello received, handshake answers pending
  Startup,    // versions, 3C identity, first status read
  Running     // start-up reads done: commands accepted
};

enum class Result : uint8_t {
  Accepted,
  RefusedTxOff,
  RefusedNotRunning,
  RefusedBusDown,
  RefusedRange,
  RefusedQueueFull
};

enum class WriteParam : uint8_t { SuperFrost, Party, Setpoint };
// Confirmed: read-back equals the written value within 3 s. Unconfirmed: it did not (or no read-back arrived); the
// state shows what was read back and the write is NOT resent. NotSent: the write never reached the bus (TX switched
// off, bus died or the freezer restarted while it was still queued).
enum class WriteOutcome : uint8_t { Confirmed, Unconfirmed, NotSent };
struct WriteResult {
  WriteParam param;
  int value;  // SuperFrost/Party: 1 = on, 0 = off; Setpoint: degC
  WriteOutcome outcome;
};

// A value that may be unknown (never read, the freezer answered flag 00, or the freezer restarted).
struct Opt {
  bool known = false;
  int32_t v = 0;         // see State for the meaning
  uint64_t t_us = 0;     // engine time of the last update (a read that returns the same value also updates it)
};

struct State {
  Opt mode;               // 0C/00 raw: 1 normal, 4 SuperFrost
  Opt party;              // 08/02: 1 on, 0 off
  Opt setpoint;           // 0C/06 degC
  Opt temperature;        // 0C/14 degC
  Opt door_open;          // 0C/31 (and node 0B's push 14/04): 1 open
  Opt door_alarm;         // 0C/09: 1 active
  Opt power_fail_alarm;   // 0C/11: 1 active
  Opt power_fail_temp;    // 0C/12 degC
  // Strings; an empty string = unknown. Identification is group 3C text records, 4 bytes each, NUL dropped.
  char freezer_version[40] = {0};  // "TOD 5-600|SW 1-86-7|BUS 1-4-0"
  char node01_version[40] = {0};   // empty when node 01 did not answer within 2 s (or yet)
  char model[24] = {0};            // 3C/01-03, e.g. "FNc    5076"
  char revision[16] = {0};         // 3C/05-06 [hyp]
  char serial[16] = {0};           // 3C/07-09 [?]
  char part_number[20] = {0};      // 3C/0A-0C [hyp]
  bool is_superfrost() const { return mode.known && mode.v == 4; }
};

struct Counters {
  uint32_t frames = 0;             // on_frame() calls
  uint32_t crc_errors = 0;         // frames (other than slot 02) with a bad CRC
  uint32_t hellos = 0;             // hellos to node 02 (= start-ups)
  uint32_t tx_slots = 0;           // on_slot02_result(true) with a message pending
  uint32_t missed_slots = 0;       // on_slot02_result(false) with a message pending
  uint32_t writes_sent = 0;        // user writes on the bus
  uint32_t writes_confirmed = 0;
  uint32_t writes_unconfirmed = 0;
  uint32_t net_writes = 0;         // group 48 messages on the bus
};

class Engine {
 public:
  Engine();

  // ---- bus input / output -------------------------------------------------------------------------------
  // One complete slot: frame[0] = slot byte, the rest = body incl. CRC. Slot 02: bus-alive only.
  void on_frame(const uint8_t* frame, size_t n, uint64_t t_us);
  // Body (bytes after the slot byte 02, CRC included) for the next slot 02: pending handshake answer, user write,
  // start-up step, network write, status read, else the idle ack `FF 02 30 01 02 00 00 0B 3A`. Returns 0 when TX is
  // disabled (nothing may be sent) or `cap` is too small (MAX_ANSWER is always enough).
  size_t next_answer(uint8_t* out, size_t cap, uint64_t now_us);
  // Outcome of the slot 02 that followed the last next_answer(): sent == true commits the offered message.
  void on_slot02_result(bool sent);
  void tick(uint64_t now_us);

  // ---- commands (they only queue; the entity changes when the read-back arrives) -------------------------------
  Result request_superfrost(bool on);
  Result request_party(bool on);
  Result request_setpoint(int degc);  // -28..-15 inclusive, else RefusedRange
  Result set_tx_enabled(bool on);     // false: nothing is answered and queued writes are dropped (NotSent). Always Accepted.
  bool tx_enabled() const { return tx_enabled_; }

  // ---- network record (group 48) ---------------------------------------------------------------------------
  // ip in dotted order {192,168,1,100}; ssid is cut to 32 chars; mac = the board's Wi-Fi MAC.
  void set_network(bool connected, const uint8_t ip[4], const char* ssid, const uint8_t mac[6]);
  void set_network_writes_enabled(bool on);  // default on; off = no group 48 (or 4C) write at all
  void set_status_period_ms(uint32_t ms) { status_period_ms_ = ms; }

  // ---- state ---------------------------------------------------------------------------------------------
  const State& state() const { return st_; }
  Phase phase() const { return phase_; }
  bool bus_alive() const { return bus_alive(now_); }
  bool bus_alive(uint64_t now_us) const;
  const Counters& counters() const { return cnt_; }
  // Bumped whenever something the glue publishes changes (a value or string, phase, bus alive, a write result).
  uint32_t change_counter() const { return change_; }
  // Pops the oldest unread write result (ring of 4; the oldest is dropped if never read). False if none.
  bool take_write_result(WriteResult& out);
  uint64_t now_us() const { return now_; }

 private:
  enum class Inflight : uint8_t { None, Hello, Ack, Table, UserWrite, Version, Node01, Ident, Status, NetState, NetText, NetMac, Idle };
  enum class Await : uint8_t { None, Version, Ident, Status };
  enum class Hq : uint8_t { Hello, Ack, Table };
  enum : uint8_t { SU_VER, SU_N01, SU_ID0, SU_ID1, SU_ID2, SU_ID3, SU_STATUS, SU_END };
  enum : uint8_t { N_STATE3, N_TEXT, N_MAC, N_STATE4, N_WAIT5, N_STATE5, N_DONE };

  struct HqItem { Hq type; uint8_t svc; };
  struct UserWrite { WriteParam p; uint8_t grp, id, flag; uint32_t value; int arg; int32_t expect; };  // arg: reported value, expect: state value after a good read-back
  struct Confirm {
    bool active = false;
    UserWrite w{};
    uint64_t t_sent = 0;
    bool event_seen = false, fallback = false, read_sent = false;
  };
  struct NetInfo { bool connected = false; uint8_t ip[4] = {0}; char ssid[SSID_MAX + 1] = {0}; uint8_t mac[6] = {0}; };

  size_t build_answer();
  void commit();
  void handle_frame(const lb::Msg& m);
  void restart_handshake();
  bool apply_param(const lb::Rec& r);
  void apply_values(const lb::Msg& m);
  void on_event_block(const lb::Msg& m);
  void on_version(const lb::Msg& m);
  void check_confirm(const lb::Rec& r);
  void finish_confirm(WriteOutcome o);
  void complete_await(Await a);
  void su_advance();
  void drop_user_writes();
  void push_result(const WriteResult& r);
  void set_opt(Opt& o, bool known, int32_t v);
  void ident_changed();
  void change() { change_++; }
  void bump_time(uint64_t t) { if (t > now_) now_ = t; }
  Result queue_write(const UserWrite& w);
  bool net_dirty() const;
  size_t build_net_text(uint8_t* out, const NetInfo& n) const;

  State st_;
  Counters cnt_;
  Phase phase_ = Phase::Waiting;
  uint32_t change_ = 0;
  uint64_t now_ = 0;
  bool tx_enabled_ = true;
  uint32_t status_period_ms_ = DEFAULT_STATUS_PERIOD_MS;

  // bus alive
  bool seen02_ = false;
  uint64_t last_slot02_ = 0;
  bool last_alive_ = false;

  // handshake
  HqItem hq_[6];
  uint8_t hq_h_ = 0, hq_n_ = 0;

  // answer offered by build_answer() and what it is
  uint8_t ans_[MAX_ANSWER];
  size_t ans_len_ = 0;
  Inflight inflight_ = Inflight::None;
  uint8_t inflight_arg_ = 0;
  NetInfo net_snap_;

  // outstanding request
  Await await_ = Await::None;
  uint64_t await_deadline_ = 0;

  // start-up
  uint8_t su_step_ = SU_VER;
  bool su_sent_ = false;
  bool node01_open_ = false;
  uint64_t node01_t_ = 0;
  uint8_t ident_[13][4];
  uint16_t ident_mask_ = 0;

  // status reads
  bool read_due_ = false;
  uint64_t last_read_sent_ = 0;
  bool read_ever_ = false;

  // user writes
  UserWrite wq_[4];
  uint8_t wq_h_ = 0, wq_n_ = 0;
  Confirm confirm_;
  WriteResult wr_[4];
  uint8_t wr_h_ = 0, wr_n_ = 0;

  // network
  NetInfo net_;
  NetInfo net_written_;
  bool net_enabled_ = true;
  uint8_t net_step_ = N_STATE3;
  uint64_t net_t4_ = 0;

  lb::Msg msg_;  // decode scratch
};

}  // namespace n02
