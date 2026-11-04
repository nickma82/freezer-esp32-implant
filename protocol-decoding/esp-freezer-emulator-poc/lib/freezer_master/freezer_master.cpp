#include "freezer_master.h"

#include <string.h>

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
  // Values as read from the captured freezer (../../03-bootup.md, ../../01-protocol.md §3).
  set_param(0x0C, 0x00, 0x20, 1, 0x01);    // mode: normal
  set_param(0x0C, 0x06, 0x40, 2, 0x00EE);  // set-point -18 degC
  set_param(0x0C, 0x14, 0x40, 2, 0x00EE);  // current temperature -18 degC
  set_param(0x0C, 0x31, 0x20, 1, 0);       // door closed
  set_param(0x0C, 0x09, 0x20, 1, 0);       // door alarm inactive
  set_param(0x08, 0x02, 0x20, 1, 0);       // party mode off
  set_param(0x3C, 0x00, 0x20, 1, 0x1D);
  set_text(0x3C, 0x01, "FNc    5076");
  set_param(0x3C, 0x04, 0x80, 4, 0);
  set_text(0x3C, 0x05, "20A/001");
  set_text(0x3C, 0x07, "123456789");
  set_text(0x3C, 0x0A, "9935672-01");
  set_text(0x3C, 0x0D, "993567201");
  set_text(0x3C, 0x10, "987654321");
  set_text(0x3C, 0x1B, "0000");
}

const Param* Master::find(uint8_t grp, uint8_t id) const {
  for (size_t i = 0; i < np_; i++)
    if (params_[i].grp == grp && params_[i].id == id) return &params_[i];
  return nullptr;
}

void Master::set_param(uint8_t grp, uint8_t id, uint8_t flag, uint8_t vlen, uint32_t value) {
  for (size_t i = 0; i < np_; i++)
    if (params_[i].grp == grp && params_[i].id == id) { params_[i].value = value; return; }
  if (np_ >= NPARAM) return;
  params_[np_++] = Param{grp, id, flag, vlen, value};
}

void Master::set_text(uint8_t grp, uint8_t first_id, const char* s) {  // 4-byte NUL padded chunks in id order
  size_t n = strlen(s);
  for (size_t off = 0; off < n; off += 4) {
    uint32_t v = 0;
    for (size_t k = 0; k < 4; k++) v = (v << 8) | (off + k < n ? (uint8_t)s[off + k] : 0);
    set_param(grp, (uint8_t)(first_id + off / 4), 0x80, 4, v);
  }
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
  } else if (pos_ == POS_EVENT && (uint32_t)(now_ms - t_event_) >= EVENT_MS) {
    out.slot = 0xFF;
    t_event_ = now_ms;
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
  if (m.src != NODE_BOX || m.dst != 0xFF) return;  // not a message from the box to the freezer
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
    default:
      break;
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
    const Param* q = find(r.grp, r.id);
    size_t need = q ? 3 + q->vlen : 3;
    if (n + need > sizeof p) { st_.truncated_reads++; break; }
    p[n++] = r.grp; p[n++] = r.id;
    if (!q) { p[n++] = 0x00; st_.unknown_params++; continue; }  // flag 00 = parameter not available
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
    set_param(r.grp, r.id, r.flag, r.vlen, v);
  }
  flags_[0] = 0x01000000;  // "any value change", captured with SuperFrost, party and set-point writes
  flags_[1] = 0x08000000;
}

}  // namespace fm
