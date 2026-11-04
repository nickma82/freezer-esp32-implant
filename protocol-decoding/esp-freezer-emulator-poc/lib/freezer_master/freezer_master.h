// Chip-independent freezer (node 7F) emulation: slot schedule, start-up handshake with node 02 and replies to the box.
// Pure C++17, no Arduino/SDK headers, no heap. Builds on the lb core of ../esp-smartdevice-emulator-poc/lib/freezer_bus.
//
// Behaviour copied from the captures (../../05-bus-roles.md §5, ../../03-bootup.md):
//  - a 12-slot half cycle of 16 ticks (a tick is ~17.8 ms, 285 ms in total), repeated; slot `02` is the box's, in every
//    second half cycle, and the freezer's reply slot `82` sits at the same position of the other half cycle
//  - the freezer answers a box message one 285 ms step later, in slot `82`, with the matching reply service
//  - nodes 01, 0B and 0C are not emulated: their slots stay bare
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "freezer_bus.h"

namespace fm {

constexpr uint32_t TICK_US = 17800;       // grid of one slot
constexpr size_t MAX_BODY = 120;          // body after the slot byte, incl. CRC; slot byte + body must fit the 128 B TX FIFO
constexpr size_t MAX_REPLY_PAYLOAD = 109; // longest captured freezer reply payload (3C identification, len 0x6D)
constexpr size_t QUEUE = 8;
constexpr uint8_t NODE_BOX = 0x02;

struct Slot {
  uint8_t slot = 0;
  uint8_t body[MAX_BODY];
  uint8_t len = 0;  // 0 = bare slot, nobody is addressed
  uint8_t ticks = 1;  // duration of this slot in grid ticks (1 or 2)
};

struct Param {
  uint8_t grp, id, flag;
  uint8_t vlen;
  uint32_t value;  // big endian numeric value; text chunks are 4 bytes packed the same way
};

enum class Hs : uint8_t { WaitBox, Hello, Running };  // box not heard yet / hello sent, waiting / box answers

struct Stats {
  uint32_t slots, replies, events, box_msgs, box_bad_crc, writes, reads, unknown_params, truncated_reads, hellos;
};

class Master {
 public:
  Master();

  // Next slot of the schedule. `now_ms` is a monotonic millisecond clock (wraps are fine, only differences are used).
  void next(uint32_t now_ms, Slot& out);

  // The box's message in the slot just sent: `body` = bytes after the slot byte `02`, including CRC. Call only for
  // slot `02`; an unanswered slot is simply not reported.
  void on_box(const uint8_t* body, size_t n);

  // Re-send the hello and start the handshake again (e.g. after the box was power cycled).
  void restart_handshake() { hs_ = Hs::WaitBox; hello_due_ = true; }

  Hs handshake() const { return hs_; }
  const Stats& stats() const { return st_; }
  size_t queued() const { return qn_; }
  const Param* find(uint8_t grp, uint8_t id) const;

 private:
  void set_param(uint8_t grp, uint8_t id, uint8_t flag, uint8_t vlen, uint32_t value);
  void set_text(uint8_t grp, uint8_t first_id, const char* s);
  bool push(const uint8_t* body, size_t n);
  void reply_read(const lb::Msg& m);
  void apply_write(const lb::Msg& m);
  bool event_body(Slot& out);

  uint8_t pos_ = 0;         // position in the 12-slot half cycle
  uint8_t half_ = 0;        // 0: reply slot position carries `82`, 1: carries `02`
  Hs hs_ = Hs::WaitBox;
  bool hello_due_ = true;
  bool have_time_ = false;
  uint32_t t_hello_ = 0, t_event_ = 0;
  uint8_t counter_ = 0;
  uint32_t flags_[2] = {0, 0};  // event words 00/00, 00/01 for the next event block
  static constexpr size_t NPARAM = 56;
  Param params_[NPARAM];
  size_t np_ = 0;
  struct Q { uint8_t body[MAX_BODY]; uint8_t len; };
  Q q_[QUEUE];
  size_t qh_ = 0, qn_ = 0;
  Stats st_ = {};
};

}  // namespace fm
