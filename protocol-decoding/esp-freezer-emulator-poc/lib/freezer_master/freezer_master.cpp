#include "freezer_master.h"

#include <string.h>

#include "captured_params.h"

namespace fm {

namespace {
struct Pos { uint8_t slot; uint8_t ticks; };
// One half cycle (16 ticks = 285 ms), from the steady-state captures. 0xFF in the table = the reply/box slot position.
constexpr Pos HALF[12] = {{0x7E, 2}, {0x01, 2}, {0x7F, 1}, {0x0B, 1}, {0x0C, 1}, {0x7F, 1},
                          {0xFF, 2}, {0x7E, 2}, {0x7F, 1}, {0x0B, 1}, {0x0C, 1}, {0x7F, 1}};
constexpr uint8_t POS_X = 6;      // reply slot `82` / box slot `02`
constexpr uint8_t POS_EVENT = 8;  // a `7F` slot that carries the 1 s event block when due
constexpr uint32_t EVENT_MS = 1000;
constexpr uint32_t HELLO_RETRY_MS = 5000;
constexpr uint8_t NODE_FREEZER = 0x7F;
constexpr uint8_t DST_BOX = 0x82;  // dst byte for messages to the box, also the reply slot byte
}  // namespace

Master::Master() {
  // Everything the captured freezer answered (../../03-bootup.md, ../../01-protocol.md §3), then the state we start in.
  for (unsigned i = 0; i < N_CAPTURED && np_ < NPARAM; i++) {
    const CapturedParam& c = CAPTURED[i];
    params_[np_++] = Param{c.grp, c.id, c.req_flag, c.flag, c.vlen, c.value};
  }
  set_param(0x0C, 0x00, 0x20, 1, 0x01);    // mode: normal
  set_param(0x0C, 0x06, 0x40, 2, 0x00EE);  // set-point -18 degC
  set_param(0x0C, 0x14, 0x40, 2, 0x00EF);  // current temperature -17 degC
  set_param(0x0C, 0x31, 0x20, 1, 0);       // door closed
  set_param(0x0C, 0x09, 0x20, 1, 0);       // door alarm inactive
  set_param(0x08, 0x02, 0x20, 1, 0);       // party mode off
}

const Param* Master::find(uint8_t grp, uint8_t id, uint8_t req_flag) const {
  const Param* any = nullptr;
  for (size_t i = 0; i < np_; i++) {
    const Param& p = params_[i];
    if (p.grp != grp || p.id != id) continue;
    if (p.req_flag == req_flag) return &p;
    if (!any) any = &p;
  }
  return any;
}

void Master::set_param(uint8_t grp, uint8_t id, uint8_t flag, uint8_t vlen, uint32_t value) {
  for (size_t i = 0; i < np_; i++) {
    Param& p = params_[i];
    if (p.grp == grp && p.id == id && p.req_flag == flag) { p.flag = flag; p.vlen = vlen; p.value = value; return; }
  }
  if (np_ >= NPARAM) return;
  params_[np_++] = Param{grp, id, flag, flag, vlen, value};
}

bool Master::push(const uint8_t* body, size_t n) {
  if (n == 0 || n > MAX_BODY) return false;
  if (qn_ >= QUEUE) return false;
  Q& q = q_[(qh_ + qn_) % QUEUE];
  memcpy(q.body, body, n);
  q.len = (uint8_t)n;
  qn_++;
  return true;
}

static size_t to_box(uint8_t svc, const uint8_t* p, size_t plen, uint8_t* out) {
  return lb::build_body(DST_BOX, NODE_FREEZER, svc, 0x01, p, plen, true, out, MAX_BODY);
}

// 08/04 and 08/2A are little-endian 32-bit second counters in the captured freezer (+1 per second in every capture;
// e.g. 08/04 = 76 9C 6A 05 at 13.8 s, 8A 9C 6A 05 at 33.7 s). Params store the bytes big endian, so swap, add, swap back.
void Master::tick_counters() {
  static constexpr uint8_t IDS[] = {0x04, 0x2A};
  for (uint8_t id : IDS) {
    for (size_t i = 0; i < np_; i++) {
      Param& p = params_[i];
      if (p.grp != 0x08 || p.id != id || p.vlen != 4) continue;
      uint32_t le = __builtin_bswap32(p.value) + 1;
      p.value = __builtin_bswap32(le);
    }
  }
}

bool Master::event_body(Slot& out) {
  uint8_t p[32];
  size_t n = 0;
  for (uint8_t w = 0; w < 4; w++) {
    uint32_t v = w < 2 ? flags_[w] : 0;
    p[n++] = 0x00; p[n++] = w; p[n++] = 0x80;
    p[n++] = (uint8_t)(v >> 24); p[n++] = (uint8_t)(v >> 16); p[n++] = (uint8_t)(v >> 8); p[n++] = (uint8_t)v;
  }
  p[n++] = 0x00; p[n++] = 0x04; p[n++] = 0x20; p[n++] = counter_;
  out.len = (uint8_t)lb::build_body(lb::DST_BROADCAST, NODE_FREEZER, 0x24, 0x01, p, n, true, out.body, MAX_BODY);
  return out.len != 0;
}

void Master::next(uint32_t now_ms, Slot& out) {
  if (!have_time_) { have_time_ = true; t_hello_ = now_ms; t_event_ = now_ms; }
  if (follow_pending_) {
    if (!follow_armed_) { follow_armed_ = true; follow_t0_ = now_ms; }
    else if ((uint32_t)(now_ms - follow_t0_) >= STEP_MS) {
      int8_t t = current_temp();
      if (t != setpoint_c_) {
        t = (int8_t)(t < setpoint_c_ ? t + 1 : t - 1);
        set_temperature(t);  // 0C/14 + event flags in the next 1 s block
        followed_c_ = t;
        followed_new_ = true;
      }
      follow_t0_ = now_ms;
      if (t == setpoint_c_) follow_pending_ = follow_armed_ = false;
    }
  }
  const Pos& p = HALF[pos_];
  out.slot = p.slot;
  out.ticks = p.ticks;
  out.len = 0;
  st_.slots++;

  if (pos_ == POS_X) {
    if (half_ == 1) {
      out.slot = NODE_BOX;  // the box's slot: nothing from us
    } else {
      out.slot = 0x7E;  // idle unless something is pending
      if (hs_ == Hs::Hello && (uint32_t)(now_ms - t_hello_) >= HELLO_RETRY_MS) { hs_ = Hs::WaitBox; hello_due_ = true; }
      if (hs_ == Hs::WaitBox && hello_due_) {
        out.slot = DST_BOX;
        out.len = (uint8_t)lb::build_body(DST_BOX, NODE_FREEZER, 0x01, 0x01, nullptr, 0, true, out.body, MAX_BODY);
        hello_due_ = false;
        hs_ = Hs::Hello;
        t_hello_ = now_ms;
        st_.hellos++;
      } else if (qn_) {
        out.slot = DST_BOX;
        Q& q = q_[qh_];
        memcpy(out.body, q.body, q.len);
        out.len = q.len;
        qh_ = (qh_ + 1) % QUEUE;
        qn_--;
        st_.replies++;
      }
    }
  } else if (p.slot == 0x01 && node01_version_due_) {
    // Node 01 (display board) is not on our bus; the box asks it for its version and waits. Answer as node 01 did in
    // the boot-up capture: `01 82 01 16 01 1D <text>` in node 01's own slot.
    static const uint8_t v[] = "TOD 5-600|SW 6-2-0|BUS 1-4-0";
    out.len = (uint8_t)lb::build_body(DST_BOX, 0x01, 0x16, 0x01, v, sizeof v, true, out.body, MAX_BODY);
    node01_version_due_ = false;
  } else if (pos_ == POS_EVENT && (uint32_t)(now_ms - t_event_) >= EVENT_MS) {
    out.slot = 0xFF;
    t_event_ = now_ms;
    tick_counters();
    if (event_body(out)) {
      st_.events++;
      counter_ = (uint8_t)(counter_ + 0x10);
      flags_[0] = flags_[1] = 0;  // flags are set for one block only
    } else out.slot = 0x7F;
  }

  if (++pos_ >= 12) { pos_ = 0; half_ ^= 1; }
}

void Master::on_box(const uint8_t* body, size_t n) {
  static lb::Msg m;  // large: keep it off the 4 KB stack of the ESP8266
  uint8_t frame[lb::MAX_FRAME];
  if (n == 0 || n + 1 > sizeof frame) return;
  frame[0] = NODE_BOX;
  memcpy(frame + 1, body, n);
  lb::Status rc = lb::decode(frame, n + 1, m);
  if (rc != lb::Status::Ok || !m.crc_ok) { st_.box_bad_crc++; return; }
  if (m.src != NODE_BOX) return;
  if (m.dst == 0x81 && m.svc == 0x06) {  // version request to node 01, answered in slot 01 (see next())
    st_.box_msgs++;
    node01_version_due_ = true;
    return;
  }
  if (m.dst != 0xFF) return;  // not a message from the box to the freezer
  st_.box_msgs++;
  if (hs_ != Hs::Running) hs_ = Hs::Running;

  uint8_t out[MAX_BODY];
  size_t k = 0;
  switch (m.svc) {
    case 0x11:  // hello reply -> init step 21
      k = to_box(0x21, nullptr, 0, out);
      break;
    case 0x30:  // ack `02 <svc> 00`: 21 -> 22, 22 -> 23; 00 = nothing to send
      if (m.raw_len >= 9 && m.raw[5] == 0x02) {
        if (m.raw[6] == 0x21) k = to_box(0x22, nullptr, 0, out);
        else if (m.raw[6] == 0x22) k = to_box(0x23, nullptr, 0, out);
      }
      break;
    case 0x02: {  // table request: the freezer returns the entry with the last byte filled in (captured: 13 00 00 04)
      static const uint8_t t[] = {0x13, 0x00, 0x00, 0x04};
      k = to_box(0x12, t, sizeof t, out);
      break;
    }
    case 0x06: {  // version string, as captured from node 7F
      static const uint8_t v[] = "TOD 5-600|SW 1-86-7|BUS 1-4-0";
      k = to_box(0x16, v, sizeof v, out);  // incl. the terminating NUL
      break;
    }
    case 0x03:
      reply_read(m);
      return;
    case 0x24:
      apply_write(m);
      return;
    default: {
      record_seen(2, m.svc, 0, false);
      Note nt = {};
      nt.kind = NoteKind::UnknownSvc; nt.grp = m.svc;
      size_t take = m.raw_len > 1 ? m.raw_len - 1u : 0;  // dst.. (CRC included), slot byte skipped
      if (take > sizeof nt.data) take = sizeof nt.data;
      memcpy(nt.data, m.raw + 1, take); nt.len = (uint8_t)take;
      note(nt);
      break;
    }
  }
  if (k) push(out, k);
}

void Master::reply_read(const lb::Msg& m) {
  if (m.kind != lb::Kind::List) return;
  st_.reads++;
  uint8_t p[MAX_REPLY_PAYLOAD];
  size_t n = 0;
  for (uint8_t i = 0; i < m.nrecs; i++) {
    const lb::Rec& r = m.recs[i];
    const Param* q = find(r.grp, r.id, r.flag);
    if (record_seen(0, r.grp, r.id, q != nullptr) && !q) {
      Note nt = {}; nt.kind = NoteKind::UnknownRead; nt.grp = r.grp; nt.id = r.id; note(nt);
    }
    size_t need = q ? 3 + q->vlen : 3;
    if (n + need > sizeof p) { st_.truncated_reads++; break; }
    p[n++] = r.grp; p[n++] = r.id;
    if (!q) { p[n++] = 0x00; st_.unknown_params++; continue; }  // flag 00 = parameter not available
    if (q->flag == 0x00) { p[n++] = 0x00; continue; }           // the captured freezer said "not available" itself
    p[n++] = q->flag;
    for (int b = q->vlen - 1; b >= 0; b--) p[n++] = (uint8_t)(q->value >> (8 * b));
  }
  uint8_t out[MAX_BODY];
  size_t k = to_box(0x13, p, n, out);
  if (k) push(out, k);
}

void Master::apply_write(const lb::Msg& m) {
  if (m.kind != lb::Kind::Values) return;
  st_.writes++;
  for (uint8_t i = 0; i < m.nrecs; i++) {
    const lb::Rec& r = m.recs[i];
    uint32_t v = 0;
    for (uint8_t b = 0; b < r.vlen; b++) v = (v << 8) | r.value[b];
    bool known = find(r.grp, r.id, r.flag) != nullptr;
    if (record_seen(1, r.grp, r.id, known) && !known) {
      Note nt = {}; nt.kind = NoteKind::UnknownWrite; nt.grp = r.grp; nt.id = r.id;
      nt.data[0] = r.flag; memcpy(nt.data + 1, r.value, r.vlen); nt.len = (uint8_t)(1 + r.vlen); note(nt);
    }
    set_param(r.grp, r.id, r.flag, r.vlen, v);
    if (r.grp == 0x0C && r.id == 0x06 && r.vlen >= 1) {
      setpoint_c_ = (int8_t)(v & 0xFF);
      setpoint_new_ = true;
      follow_pending_ = true;  // the clock starts at the next next() call (no time here); a new write restarts it
      follow_armed_ = false;
    }
  }
  flags_[0] = 0x01000000;  // "any value change", captured with SuperFrost, party and set-point writes
  flags_[1] = 0x08000000;
}

bool Master::record_seen(uint8_t kind, uint8_t a, uint8_t b, bool known) {
  for (size_t i = 0; i < nseen_; i++)
    if (seen_[i].kind == kind && seen_[i].a == a && seen_[i].b == b) { seen_[i].count++; return false; }
  if (nseen_ >= NSEEN) return false;
  seen_[nseen_++] = Seen{kind, a, b, known, 1};
  return true;
}

void Master::note(const Note& n) {
  if (note_n_ >= NNOTE) return;
  notes_[(note_h_ + note_n_) % NNOTE] = n;
  note_n_++;
}

bool Master::take_note(Note& n) {
  if (!note_n_) return false;
  n = notes_[note_h_];
  note_h_ = (note_h_ + 1) % NNOTE;
  note_n_--;
  return true;
}

bool Master::inject(uint8_t svc, const uint8_t* payload, size_t n) {
  uint8_t out[MAX_BODY];
  size_t k = to_box(svc, payload, n, out);
  return k && push(out, k);
}

bool Master::inject_raw(const uint8_t* body, size_t n) {
  if (n < 4 || n + 2 > MAX_BODY) return false;
  uint8_t out[MAX_BODY];
  memcpy(out, body, n);
  uint16_t c = lb::crc16_xmodem(out, n);
  out[n] = (uint8_t)c; out[n + 1] = (uint8_t)(c >> 8);
  return push(out, n + 2);
}

void Master::door_alarm(bool on) {
  set_param(0x0C, 0x31, 0x20, 1, on ? 1 : 0);
  set_param(0x0C, 0x09, 0x20, 1, on ? 1 : 0);
  flags_[0] |= 0x00000100;
  flags_[1] |= 0x08000000;
}

int8_t Master::current_temp() const {
  const Param* p = find(0x0C, 0x14, 0x40);
  return p ? (int8_t)(p->value & 0xFF) : setpoint_c_;
}

void Master::set_temperature(int8_t c) {
  set_param(0x0C, 0x14, 0x40, 2, (uint8_t)c);
  flags_[0] |= 0x01000000;
  flags_[1] |= 0x08000000;
}

}  // namespace fm
