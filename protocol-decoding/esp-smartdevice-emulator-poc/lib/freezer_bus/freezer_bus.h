// Chip-independent core of the Freezer freezer bus protocol: a port of ../../src/crc.py and ../../src/protocol.py.
// Pure C++17, no Arduino/SDK headers, fixed buffers, no heap. All buffers are caller-owned or inside the structs.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace lb {

constexpr size_t MAX_FRAME = 160;  // slot byte + body, the longest captured message is 95 bytes
constexpr size_t MAX_RECS = 54;    // (MAX_FRAME - 8) / 3 records without values
constexpr size_t MAX_ITEMS = 16;

enum class Kind : uint8_t { Empty, Ack, List, Values, Unknown };  // Python: 'bare', 'ack', 'list', 'values', 'unknown'
enum class Status : uint8_t { Ok, ErrEmpty, ErrTooLong, ErrTruncated, ErrFlag, ErrLength };

struct Rec {  // Python: Rec
  uint8_t grp, id, flag;
  uint8_t vlen;  // 0 in a descriptor list
  uint8_t value[4];
};

struct Msg {  // Python: Msg
  uint8_t slot = 0;
  uint8_t dst = 0;  // marker: 0x80|node, FF = freezer, 80 = broadcast
  uint8_t src = 0;  // second: source node
  uint8_t svc = 0, flag = 0;
  Kind kind = Kind::Empty;
  bool crc_ok = false;
  uint8_t nrecs = 0;
  Rec recs[MAX_RECS];
  uint8_t raw[MAX_FRAME];  // slot byte + body + CRC, as received
  uint8_t raw_len = 0;
};

// Python: crc.crc16_xmodem
uint16_t crc16_xmodem(const uint8_t* data, size_t n);

// Python: protocol.WIDTH. Returns -1 for an unknown flag.
int record_width(uint8_t flag);

// Python: protocol.decode. `frame` holds the bytes after a BREAK, slot byte first.
Status decode(const uint8_t* frame, size_t n, Msg& m);

// Python: protocol.encode. Returns the frame length, 0 if `cap` is too small or the message is not encodable.
size_t encode(const Msg& m, uint8_t* out, size_t cap);

// Node numbers: dst 0x80|n -> n, FF -> 7F (freezer). Python: slots.node
constexpr uint8_t NODE_FREEZER = 0x7F;
constexpr uint8_t DST_BROADCAST = 0x80;
constexpr bool is_broadcast(uint8_t dst) { return dst == DST_BROADCAST; }
constexpr uint8_t node_of(uint8_t dst) { return dst == 0xFF ? NODE_FREEZER : (dst & 0x7F); }

// ---- semantics -------------------------------------------------------------------------
struct Item {
  const char* name;  // mode, party, setpoint, current_temp, door_open, door_alarm, door_push, event
  int32_t num;       // numeric value (bool 0/1, deg C, raw mode); 0 for text-only items
  char text[32];     // canonical value text: mode name, number, "1"/"0", event name
};

struct Interp {
  Item items[MAX_ITEMS];
  uint8_t n = 0;
  bool is_event = false;
  uint32_t flags[4] = {0, 0, 0, 0};  // event block words 00/00..00/03
  bool has_counter = false;
  uint8_t counter = 0;  // event block counter 00/04
};

// Python: protocol.interpret (+ door_push for 14/04 and the event words/counter). Needs a valid CRC.
void interpret(const Msg& m, Interp& out);

// One-line text for the log, e.g. "mode = superfrost, setpoint = -18" or "event door alarm changed". Returns length.
size_t describe(const Interp& in, char* out, size_t cap);

// ---- node-02 builders (bodies after the slot byte, CRC appended). Return length, 0 on overflow -----------
size_t build_body(uint8_t dst, uint8_t src, uint8_t svc, uint8_t flag, const uint8_t* payload, size_t plen,
                  bool with_len, uint8_t* out, size_t cap);
size_t build_idle(uint8_t* out, size_t cap);                                   // FF 02 30 01 02 00 00 crc
size_t build_write_param(uint8_t grp, uint8_t id, uint8_t flag, uint32_t value, uint8_t* out, size_t cap);  // write_param
size_t build_read_request(const uint8_t (*params)[3], size_t n, uint8_t* out, size_t cap);  // read_request
size_t build_hello_reply(uint8_t* out, size_t cap);                            // FF 02 11 01 06 03 01 00 00 00 00 crc
size_t build_init_ack(uint8_t svc, uint8_t* out, size_t cap);                  // FF 02 30 01 02 <svc> 00 crc
size_t build_table_request(uint8_t* out, size_t cap);                          // FF 02 02 01 04 13 00 00 00 crc
size_t build_version_request(uint8_t dst, uint8_t* out, size_t cap);           // <dst> 02 06 01 00 crc

// ---- timing ------------------------------------------------------------------------------
// Extends the 32-bit CPU cycle counter to 64 bits. Call at least once per wrap period (26.8 s at 160 MHz).
class CycleClock {
 public:
  uint64_t extend(uint32_t cc) {
    if (started_ && cc < last_) hi_ += (uint64_t)1 << 32;
    started_ = true;
    last_ = cc;
    return hi_ | cc;
  }

 private:
  uint64_t hi_ = 0;
  uint32_t last_ = 0;
  bool started_ = false;
};

// Schedule and latency statistics: periods per slot byte and latency per source node.
class ScheduleTracker {
 public:
  static constexpr size_t WINDOW = 64;
  static constexpr size_t SOURCES = 8;

  struct Latency {
    uint8_t src;
    uint32_t n;       // samples in the window
    uint32_t median;  // us
    uint32_t spread;  // max - min, us
  };

  void slot(uint8_t slot_byte, uint64_t t_us);  // records the period since the previous slot with this byte
  void latency(uint8_t src, uint32_t lat_us);   // sample for a source node (start of slot byte to first body byte)
  bool last_period_us(uint8_t slot_byte, uint32_t& p) const;
  // periods of a slot byte, counted near 285 ms, near 570 ms (+-60 ms) and anywhere else
  void period_stats(uint8_t slot_byte, uint32_t& n285, uint32_t& n570, uint32_t& other) const;
  size_t latencies(Latency* out, size_t cap) const;  // one entry per source seen
  void clear_latencies();

 private:
  struct Src {
    bool used = false;
    uint8_t id = 0;
    uint8_t head = 0, count = 0;
    uint32_t v[WINDOW];
  };
  uint64_t last_t_[256] = {};
  bool seen_[256] = {};
  uint32_t period_[256] = {};
  uint32_t n285_[256] = {}, n570_[256] = {}, other_[256] = {};
  Src src_[SOURCES];
};

}  // namespace lb
