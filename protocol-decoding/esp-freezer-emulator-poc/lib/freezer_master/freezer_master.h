// Chip-independent freezer (node 7F) emulation: slot schedule, start-up handshake with node 02 and replies to the box.
// Pure C++17, no Arduino/SDK headers, no heap. Builds on the lb core of ../esp-smartdevice-emulator-poc/lib/freezer_bus.
//
// Behaviour copied from the captures (../../05-bus-roles.md §5, ../../03-bootup.md):
//  - a 12-slot half cycle of 16 ticks (a tick is ~17.8 ms, 285 ms in total), repeated; slot `02` is the box's, in every
//    second half cycle, and the freezer's reply slot `82` sits at the same position of the other half cycle
//  - the freezer answers a box message one 285 ms step later, in slot `82`, with the matching reply service
//  - nodes 0B and 0C are not emulated: their slots stay bare. Node 01 only answers the box's version request (slot 01)
//  - read replies come from the captured freezer's own answers (captured_params.h, tools/gen_params.py)
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "freezer_bus.h"

namespace fm {

constexpr uint32_t TICK_US = 17800;       // grid of one slot
constexpr size_t MAX_BODY = 150;          // body after the slot byte, incl. CRC (the transmitter refills the 128 B FIFO)
constexpr size_t MAX_REPLY_PAYLOAD = 140; // longest captured freezer reply payload is 127 bytes (len 0x7F, 08 list)
constexpr size_t QUEUE = 8;
constexpr uint8_t NODE_BOX = 0x02;

struct Slot {
  uint8_t slot = 0;
  uint8_t body[MAX_BODY];
  uint8_t len = 0;  // 0 = bare slot, nobody is addressed
  uint8_t ticks = 1;  // duration of this slot in grid ticks (1 or 2)
};

struct Param {
  uint8_t grp, id, req_flag;  // key: the freezer answers the same grp/id differently per requested flag (0C/01 20 vs 21)
  uint8_t flag;               // flag in the reply; 00 = not available
  uint8_t vlen;
  uint32_t value;  // big endian numeric value; text chunks are 4 bytes packed the same way
};

enum class Hs : uint8_t { WaitBox, Hello, Running };  // box not heard yet / hello sent, waiting / box answers

// First sighting of something the emulator does not know (or every occurrence for an unknown service).
enum class NoteKind : uint8_t { UnknownRead, UnknownWrite, UnknownSvc };
struct Note {
  NoteKind kind;
  uint8_t grp, id;   // UnknownRead/Write: parameter; UnknownSvc: grp = service byte
  uint8_t len;
  uint8_t data[32];  // UnknownSvc: raw frame from the dst byte on (without slot byte), truncated; UnknownWrite: flag + value
};

// Everything the box asked for / wrote / sent, counted. kind: 0 = read, 1 = write, 2 = service.
struct Seen {
  uint8_t kind, a, b;  // read/write: grp, id; service: svc, 0
  bool known;          // parameter was in the table / service is handled
  uint32_t count;
};

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
  const Param* find(uint8_t grp, uint8_t id, uint8_t req_flag) const;  // exact flag first, then any flag

  // ---- exploration ----
  // Queue a message to the box in the next reply slot `82`: `svc` + payload (length byte and CRC are added), or a raw
  // body `dst src svc flag [len] payload...` (CRC is added). False if it does not fit / the queue is full.
  bool inject(uint8_t svc, const uint8_t* payload, size_t n);
  bool inject_raw(const uint8_t* body_wo_crc, size_t n);
  // Change a parameter and optionally raise the event flag words 00/00 and 00/01 in the next 1 s event block.
  void set_value(uint8_t grp, uint8_t id, uint8_t flag, uint8_t vlen, uint32_t value) { set_param(grp, id, flag, vlen, value); }
  void raise_flags(uint32_t w0, uint32_t w1) { flags_[0] |= w0; flags_[1] |= w1; }
  void door_alarm(bool on);       // 0C/31 door + 0C/09 alarm, flag words 00000100/08000000 (captured door-alarm change)
  void set_temperature(int8_t c); // 0C/14 current temperature + "any value change" flags (flags hypothetical)
  bool take_note(Note& n);
  size_t seen_count() const { return nseen_; }
  const Seen& seen_at(size_t i) const { return seen_[i]; }

  // Set-point (`0C/06`, `00 <int8 degC>`) written by the box. Returns true once per write (even if the value is
  // unchanged, the box re-sends it) and gives the temperature in degC.
  bool take_setpoint(int8_t& degc) {
    if (!setpoint_new_) return false;
    setpoint_new_ = false; degc = setpoint_c_; return true;
  }

  // Simulated cooling/warming: after a set-point write the current temperature (0C/14) moves 1 degC towards the set-point
  // every STEP_MS, each step with the "any value change" event flags. take_followed() returns true once per step and
  // gives the new temperature.
  static constexpr uint32_t STEP_MS = 15000;
  bool take_followed(int8_t& degc) {
    if (!followed_new_) return false;
    followed_new_ = false; degc = followed_c_; return true;
  }

 private:
  // Sets the entry requested with `flag` (adds one if missing); the reply flag becomes `flag`.
  void set_param(uint8_t grp, uint8_t id, uint8_t flag, uint8_t vlen, uint32_t value);
  bool push(const uint8_t* body, size_t n);
  void reply_read(const lb::Msg& m);
  void apply_write(const lb::Msg& m);
  bool record_seen(uint8_t kind, uint8_t a, uint8_t b, bool known);  // true on the first sighting
  void note(const Note& n);
  bool event_body(Slot& out);
  void tick_counters();  // once per event block (1 s)

  uint8_t pos_ = 0;         // position in the 12-slot half cycle
  uint8_t half_ = 0;        // 0: reply slot position carries `82`, 1: carries `02`
  Hs hs_ = Hs::WaitBox;
  bool hello_due_ = true;
  bool have_time_ = false;
  uint32_t t_hello_ = 0, t_event_ = 0;
  uint8_t counter_ = 0;
  static constexpr size_t NSEEN = 200, NNOTE = 8;  // the box touches ~150 distinct params during its start-up
  Seen seen_[NSEEN];
  size_t nseen_ = 0;
  Note notes_[NNOTE];
  size_t note_h_ = 0, note_n_ = 0;
  bool node01_version_due_ = false;  // box asked node 01 (`81 02 06`) for its version
  bool setpoint_new_ = false;
  bool follow_pending_ = false, follow_armed_ = false, followed_new_ = false;
  uint32_t follow_t0_ = 0;  // time of the write / of the last step
  int8_t followed_c_ = 0;
  int8_t current_temp() const;
  int8_t setpoint_c_ = 0;
  uint32_t flags_[2] = {0, 0};  // event words 00/00, 00/01 for the next event block
  static constexpr size_t NPARAM = 360;  // captured table (~285) + what the box writes (groups 48, 4C)
  Param params_[NPARAM];
  size_t np_ = 0;
  struct Q { uint8_t body[MAX_BODY]; uint8_t len; };
  Q q_[QUEUE];
  size_t qh_ = 0, qn_ = 0;
  Stats st_ = {};
};

}  // namespace fm
