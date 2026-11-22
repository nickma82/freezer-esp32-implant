#include "node02_engine.h"

#include <string.h>

namespace n02 {

namespace {

constexpr uint8_t SLOT_BOX = 0x02;    // the slot the freezer grants us
constexpr uint8_t SLOT_REPLY = 0x82;  // freezer -> node 02
constexpr uint8_t NODE_FREEZER = 0x7F;
constexpr uint8_t NODE_0B = 0x0B;
constexpr uint8_t NODE_01 = 0x01;
constexpr uint8_t DST_FREEZER = 0xFF;  // from us: to the freezer
constexpr uint8_t DST_NODE01 = 0x81;

constexpr uint64_t ms_to_us(uint32_t ms) { return (uint64_t)ms * 1000u; }

// Status read list with the request flags the box uses (captured_params.h): 20 = 1-byte, 40 = 2-byte params.
constexpr uint8_t STATUS_LIST[][3] = {
    {0x0C, 0x00, 0x20}, {0x08, 0x02, 0x20}, {0x0C, 0x06, 0x40}, {0x0C, 0x09, 0x20},
    {0x0C, 0x11, 0x20}, {0x0C, 0x12, 0x40}, {0x0C, 0x14, 0x40}, {0x0C, 0x31, 0x20},
};
constexpr size_t STATUS_COUNT = sizeof STATUS_LIST / sizeof STATUS_LIST[0];

// The box's paged 3C identity requests (03-bootup.md, captured at 8.1 s, 17.5 s, 18.6 s, 19.2 s). Flag 80 = text chunk.
// Page 0: 3C/00 (20) + 01..09 + 0D..12; page 1: 0A..0C; page 2: 13..24; page 3: 25..2A. Replies stay below 128 bytes.
constexpr uint8_t IDENT_PAGES = 4;
size_t ident_page(uint8_t page, uint8_t (*recs)[3]) {
  size_t n = 0;
  auto add = [&](uint8_t id, uint8_t flag) { recs[n][0] = 0x3C; recs[n][1] = id; recs[n][2] = flag; n++; };
  switch (page) {
    case 0:
      add(0x00, 0x20);
      for (uint8_t i = 0x01; i <= 0x09; i++) add(i, 0x80);
      for (uint8_t i = 0x0D; i <= 0x12; i++) add(i, 0x80);
      break;
    case 1:
      for (uint8_t i = 0x0A; i <= 0x0C; i++) add(i, 0x80);
      break;
    case 2:
      for (uint8_t i = 0x13; i <= 0x24; i++) add(i, 0x80);
      break;
    default:
      for (uint8_t i = 0x25; i <= 0x2A; i++) add(i, 0x80);
      break;
  }
  return n;
}

void copy_str(char* dst, size_t cap, const uint8_t* src, size_t n) {
  size_t i = 0;
  while (i < n && i + 1 < cap && src[i]) { dst[i] = (char)src[i]; i++; }
  dst[i] = 0;
}

size_t put_dec(char* out, unsigned v) {
  char t[4];
  size_t k = 0;
  do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
  for (size_t i = 0; i < k; i++) out[i] = t[k - 1 - i];
  return k;
}

}  // namespace

Engine::Engine() {
  memset(ident_, 0, sizeof ident_);
  memset(ans_, 0, sizeof ans_);
}

// ---- time / bus alive ---------------------------------------------------------------------------------------
bool Engine::bus_alive(uint64_t now_us) const {
  return seen02_ && now_us >= last_slot02_ && now_us - last_slot02_ <= ms_to_us(BUS_ALIVE_MS);
}

void Engine::tick(uint64_t now_us) {
  bump_time(now_us);
  bool alive = bus_alive(now_);
  if (alive != last_alive_) {
    last_alive_ = alive;
    change();
    if (!alive) drop_user_writes();  // a write must not go out long after the user asked
  }
  if (phase_ == Phase::Waiting) {
    const bool freezer_active = alive && seen_freezer_ && now_ - last_freezer_ <= ms_to_us(BUS_ALIVE_MS);
    if (!freezer_active || !tx_enabled_) waiting_since_ = now_;
    else if (now_ - waiting_since_ >= ms_to_us(REJOIN_AFTER_MS)) rejoin();
  }
  if (await_ != Await::None && now_ >= await_deadline_) {
    Await a = await_;
    complete_await(a);  // gives up: the step is skipped, a missing answer must not block start-up
  }
  if (confirm_.active) {
    uint64_t age = now_ - confirm_.t_sent;
    if (age >= ms_to_us(WRITE_CONFIRM_MS)) finish_confirm(WriteOutcome::Unconfirmed);
    else if (!confirm_.event_seen && !confirm_.fallback && age >= ms_to_us(WRITE_FALLBACK_READ_MS)) {
      confirm_.fallback = true;  // no event block came: read back anyway
      read_due_ = true;
    }
  }
}

// ---- input ----------------------------------------------------------------------------------------------------
void Engine::on_frame(const uint8_t* f, size_t n, uint64_t t) {
  bump_time(t);
  cnt_.frames++;
  if (!n) return;
  if (f[0] == SLOT_BOX) {  // empty slot or our own echo
    last_slot02_ = now_;
    seen02_ = true;
    if (!last_alive_) { last_alive_ = true; change(); }
    return;
  }
  if (n == 1) return;  // bare slot
  lb::Status s = lb::decode(f, n, msg_);
  if (!msg_.crc_ok) { cnt_.crc_errors++; return; }
  if (s != lb::Status::Ok) return;
  handle_frame(msg_);
}

void Engine::handle_frame(const lb::Msg& m) {
  if (m.src == NODE_FREEZER) { seen_freezer_ = true; last_freezer_ = now_; }
  if (m.slot == SLOT_REPLY && m.dst == SLOT_REPLY && m.src == NODE_FREEZER) {
    switch (m.svc) {
      case 0x01:  // hello: (re)start everything, whatever the phase
        restart_handshake();
        return;
      case 0x21: case 0x22: case 0x23:  // init steps: ack; after the ack of 22 the table request follows
        if ((size_t)hq_n_ + 2 <= sizeof hq_ / sizeof hq_[0]) {
          hq_[(hq_h_ + hq_n_++) % 6] = HqItem{Hq::Ack, m.svc};
          if (m.svc == 0x22) hq_[(hq_h_ + hq_n_++) % 6] = HqItem{Hq::Table, 0};
        }
        return;
      default:
        break;  // 12 (table answer) is accepted without further action; 13/16 fall through below
    }
  }
  if (m.src == NODE_FREEZER) {
    if (m.svc == 0x13 && m.kind == lb::Kind::Values) apply_values(m);
    else if (m.svc == 0x16) on_version(m);
    else if (m.svc == 0x24 && m.dst == lb::DST_BROADCAST && m.kind == lb::Kind::Values) on_event_block(m);
  } else if (m.src == NODE_0B && m.svc == 0x24 && m.kind == lb::Kind::Values) {
    for (uint8_t i = 0; i < m.nrecs; i++) {  // node 0B's door push 14/04
      const lb::Rec& r = m.recs[i];
      if (r.grp == 0x14 && r.id == 0x04 && r.vlen) set_opt(st_.door_open, true, r.value[0] != 0);
    }
  } else if (m.src == NODE_01 && m.svc == 0x16) {
    on_version(m);
  }
}

void Engine::on_version(const lb::Msg& m) {
  if (m.raw_len < 8) return;
  size_t len = m.raw[5], avail = (size_t)m.raw_len - 2 - 6;
  if (len > avail) len = avail;
  if (m.src == NODE_FREEZER) {
    char t[sizeof st_.freezer_version];
    copy_str(t, sizeof t, m.raw + 6, len);
    if (strcmp(t, st_.freezer_version)) { memcpy(st_.freezer_version, t, sizeof t); change(); }
    su_answered_ = true;
    complete_await(Await::Version);
  } else if (m.src == NODE_01 && node01_open_ && m.dst == SLOT_REPLY &&
             now_ - node01_t_ <= ms_to_us(NODE01_VERSION_WINDOW_MS)) {
    char t[sizeof st_.node01_version];
    copy_str(t, sizeof t, m.raw + 6, len);
    if (strcmp(t, st_.node01_version)) { memcpy(st_.node01_version, t, sizeof t); change(); }
  }
}

void Engine::on_event_block(const lb::Msg& m) {
  bool nz = false;
  for (uint8_t i = 0; i < m.nrecs; i++) {
    const lb::Rec& r = m.recs[i];
    if (r.grp == 0x00 && r.id <= 0x03)
      for (uint8_t b = 0; b < r.vlen; b++) if (r.value[b]) nz = true;
  }
  if (!nz) return;
  if (phase_ == Phase::Running) read_due_ = true;
  if (confirm_.active) confirm_.event_seen = true;
}

void Engine::set_opt(Opt& o, bool known, int32_t v) {
  if (known != o.known || (known && v != o.v)) change();
  o.known = known;
  o.v = known ? v : 0;
  o.t_us = now_;
}

// Returns true for the parameters of the status model.
bool Engine::apply_param(const lb::Rec& r) {
  Opt* o = nullptr;
  enum { Raw, Bool, Int8 } kind = Raw;
  if (r.grp == 0x0C) {
    switch (r.id) {
      case 0x00: o = &st_.mode; kind = Raw; break;
      case 0x06: o = &st_.setpoint; kind = Int8; break;
      case 0x09: o = &st_.door_alarm; kind = Bool; break;
      case 0x11: o = &st_.power_fail_alarm; kind = Bool; break;
      case 0x12: o = &st_.power_fail_temp; kind = Int8; break;
      case 0x14: o = &st_.temperature; kind = Int8; break;
      case 0x31: o = &st_.door_open; kind = Bool; break;
      default: break;
    }
  } else if (r.grp == 0x08 && r.id == 0x02) {
    o = &st_.party; kind = Bool;
  }
  if (!o) return false;
  if (r.flag == 0x00 || r.vlen == 0) { set_opt(*o, false, 0); return true; }  // "not available"
  int32_t v;
  switch (kind) {
    case Bool: v = r.value[r.vlen - 1] != 0 ? 1 : 0; break;
    case Int8: v = (int8_t)r.value[r.vlen - 1]; break;  // low byte of the big-endian value
    default: v = r.value[r.vlen - 1]; break;
  }
  set_opt(*o, true, v);
  return true;
}

void Engine::apply_values(const lb::Msg& m) {
  bool any_status = false, any_ident = false;
  for (uint8_t i = 0; i < m.nrecs; i++) {
    const lb::Rec& r = m.recs[i];
    if (apply_param(r)) {
      any_status = true;
      check_confirm(r);
    } else if (r.grp == 0x3C) {
      any_ident = true;
      if (r.id >= 0x01 && r.id <= 0x0C && r.vlen == 4 && r.flag != 0x00) {
        memcpy(ident_[r.id], r.value, 4);
        ident_mask_ |= (uint16_t)(1u << r.id);
      }
    }
  }
  if (any_ident || any_status) su_answered_ = true;
  if (any_ident) { ident_changed(); complete_await(Await::Ident); }
  if (any_status) complete_await(Await::Status);
}

void Engine::ident_changed() {
  struct Range { uint8_t lo, hi; char* dst; size_t cap; };
  const Range ranges[] = {{0x01, 0x03, st_.model, sizeof st_.model},
                          {0x05, 0x06, st_.revision, sizeof st_.revision},
                          {0x07, 0x09, st_.serial, sizeof st_.serial},
                          {0x0A, 0x0C, st_.part_number, sizeof st_.part_number}};
  for (const Range& r : ranges) {
    bool all = true;
    for (uint8_t i = r.lo; i <= r.hi; i++) if (!(ident_mask_ & (1u << i))) all = false;
    if (!all) continue;
    uint8_t flat[16];
    size_t n = 0;
    for (uint8_t i = r.lo; i <= r.hi; i++)
      for (int b = 0; b < 4; b++) if (ident_[i][b]) flat[n++] = ident_[i][b];  // NUL padding dropped (protocol.py text())
    char t[24];
    copy_str(t, sizeof t < r.cap ? sizeof t : r.cap, flat, n);
    if (strcmp(t, r.dst)) { strcpy(r.dst, t); change(); }
  }
}

void Engine::complete_await(Await a) {
  if (await_ != a) return;
  await_ = Await::None;
  if (phase_ == Phase::Startup && su_sent_) su_advance();
}

void Engine::su_advance() {
  su_step_++;
  su_sent_ = false;
  await_ = Await::None;
  if (su_step_ < SU_END) return;
  // A rejoin the freezer did not answer at all: it does not talk to us without the handshake (or is not there).
  // Back to Waiting, the next attempt follows REJOIN_AFTER_MS later.
  const bool failed_rejoin = rejoin_ && !su_answered_;
  phase_ = failed_rejoin ? Phase::Waiting : Phase::Running;
  waiting_since_ = now_;
  rejoin_ = false;
  change();
}

// ---- recovery -------------------------------------------------------------------------------------------------
void Engine::restart_handshake() {
  cnt_.hellos++;
  reset_session();
  hq_[hq_n_++] = HqItem{Hq::Hello, 0};
  phase_ = Phase::Handshake;
  change();
}

// The C3 restarted while the freezer kept running: no hello will come, so do the start-up reads without it.
void Engine::rejoin() {
  cnt_.rejoins++;
  reset_session();
  rejoin_ = true;
  phase_ = Phase::Startup;
  change();
}

// Everything a new start-up begins without: queues, outstanding requests, values and the start-up/network progress.
void Engine::reset_session() {
  drop_user_writes();
  if (confirm_.active) finish_confirm(WriteOutcome::Unconfirmed);
  hq_h_ = 0;
  hq_n_ = 0;
  rejoin_ = false;
  su_answered_ = false;
  inflight_ = Inflight::None;
  await_ = Await::None;
  su_step_ = SU_VER;
  su_sent_ = false;
  node01_open_ = false;
  if (st_.node01_version[0]) { st_.node01_version[0] = 0; change(); }
  net_step_ = N_STATE3;
  read_due_ = false;
  read_ever_ = false;
  Opt* all[] = {&st_.mode, &st_.party, &st_.setpoint, &st_.temperature,
                &st_.door_open, &st_.door_alarm, &st_.power_fail_alarm, &st_.power_fail_temp};
  for (Opt* o : all) set_opt(*o, false, 0);
}

// ---- commands -------------------------------------------------------------------------------------------------
Result Engine::queue_write(const UserWrite& w) {
  if (!tx_enabled_) return Result::RefusedTxOff;
  if (phase_ != Phase::Running) return Result::RefusedNotRunning;
  if (!bus_alive(now_)) return Result::RefusedBusDown;
  if (wq_n_ >= sizeof wq_ / sizeof wq_[0]) return Result::RefusedQueueFull;
  wq_[(wq_h_ + wq_n_++) % 4] = w;
  return Result::Accepted;
}

Result Engine::request_superfrost(bool on) {
  return queue_write(UserWrite{WriteParam::SuperFrost, 0x0C, 0x00, 0x20, on ? 0x04u : 0x01u, on ? 1 : 0, on ? 4 : 1});
}

Result Engine::request_party(bool on) {
  return queue_write(UserWrite{WriteParam::Party, 0x08, 0x02, 0x20, on ? 0x02u : 0x00u, on ? 1 : 0, on ? 1 : 0});
}

Result Engine::request_setpoint(int degc) {
  if (degc < setpoint_min_ || degc > setpoint_max_) return Result::RefusedRange;
  return queue_write(UserWrite{WriteParam::Setpoint, 0x0C, 0x06, 0x40, (uint32_t)(uint8_t)(int8_t)degc, degc, degc});
}

Result Engine::set_tx_enabled(bool on) {
  if (tx_enabled_ == on) return Result::Accepted;
  tx_enabled_ = on;
  if (!on) { drop_user_writes(); inflight_ = Inflight::None; }
  change();
  return Result::Accepted;
}

void Engine::drop_user_writes() {
  while (wq_n_) {
    const UserWrite& w = wq_[wq_h_];
    push_result(WriteResult{w.p, w.arg, WriteOutcome::NotSent});
    wq_h_ = (uint8_t)((wq_h_ + 1) % 4);
    wq_n_--;
  }
}

void Engine::push_result(const WriteResult& r) {
  if (wr_n_ >= sizeof wr_ / sizeof wr_[0]) { wr_h_ = (uint8_t)((wr_h_ + 1) % 4); wr_n_--; }
  wr_[(wr_h_ + wr_n_++) % 4] = r;
  change();
}

bool Engine::take_write_result(WriteResult& out) {
  if (!wr_n_) return false;
  out = wr_[wr_h_];
  wr_h_ = (uint8_t)((wr_h_ + 1) % 4);
  wr_n_--;
  return true;
}

void Engine::check_confirm(const lb::Rec& r) {
  if (!confirm_.active || !confirm_.read_sent) return;
  if (r.grp != confirm_.w.grp || r.id != confirm_.w.id) return;
  const Opt* o = nullptr;
  if (r.grp == 0x0C && r.id == 0x00) o = &st_.mode;
  else if (r.grp == 0x08 && r.id == 0x02) o = &st_.party;
  else if (r.grp == 0x0C && r.id == 0x06) o = &st_.setpoint;
  if (!o) return;
  // The freezer's answer to the read issued after the event: it equals what we wrote, or it does not.
  finish_confirm(o->known && o->v == confirm_.w.expect ? WriteOutcome::Confirmed : WriteOutcome::Unconfirmed);
}

void Engine::finish_confirm(WriteOutcome o) {
  if (!confirm_.active) return;
  confirm_.active = false;
  if (o == WriteOutcome::Confirmed) cnt_.writes_confirmed++;
  else cnt_.writes_unconfirmed++;
  push_result(WriteResult{confirm_.w.p, confirm_.w.arg, o});
}

// ---- network --------------------------------------------------------------------------------------------------
void Engine::set_network(bool connected, const uint8_t ip[4], const char* ssid, const uint8_t mac[6]) {
  net_.connected = connected;
  memcpy(net_.ip, ip, 4);
  size_t i = 0;
  for (; ssid && ssid[i] && i < SSID_MAX; i++) net_.ssid[i] = ssid[i];
  net_.ssid[i] = 0;
  memcpy(net_.mac, mac, 6);
}

void Engine::set_network_writes_enabled(bool on) { net_enabled_ = on; }

bool Engine::net_dirty() const {
  return net_step_ == N_DONE && net_.connected &&
         (memcmp(net_.ip, net_written_.ip, 4) != 0 || strcmp(net_.ssid, net_written_.ssid) != 0);
}

// "<ip>@<ssid>" cut to 32 chars, NUL padded, in 48/04..0B as flag-80 records, then 48/14 = IP little endian.
size_t Engine::build_net_text(uint8_t* p, const NetInfo& n) const {
  char t[NET_TEXT_MAX + 1];
  size_t k = 0;
  for (int i = 0; i < 4; i++) {
    k += put_dec(t + k, n.ip[i]);
    if (i < 3) t[k++] = '.';
  }
  t[k++] = '@';
  for (size_t i = 0; n.ssid[i] && k < NET_TEXT_MAX; i++) t[k++] = n.ssid[i];
  if (k > NET_TEXT_MAX) k = NET_TEXT_MAX;
  while (k < NET_TEXT_MAX) t[k++] = 0;
  size_t o = 0;
  for (uint8_t i = 0; i < 8; i++) {
    p[o++] = 0x48; p[o++] = (uint8_t)(0x04 + i); p[o++] = 0x80;
    memcpy(p + o, t + 4 * i, 4);
    o += 4;
  }
  p[o++] = 0x48; p[o++] = 0x14; p[o++] = 0x80;
  p[o++] = n.ip[3]; p[o++] = n.ip[2]; p[o++] = n.ip[1]; p[o++] = n.ip[0];
  return o;
}

// ---- answer selection -----------------------------------------------------------------------------------------
// Priority: handshake > user write > start-up step > read-back of a write > network write > status read > idle ack.
size_t Engine::build_answer() {
  inflight_ = Inflight::None;
  ans_len_ = 0;
  if (!tx_enabled_) return 0;
  uint8_t* o = ans_;
  const size_t cap = sizeof ans_;
  size_t n = 0;
  auto body = [&](uint8_t dst, uint8_t svc, const uint8_t* p, size_t plen) {
    return lb::build_body(dst, 0x02, svc, 0x01, p, plen, true, o, cap);
  };

  if (hq_n_) {
    const HqItem& h = hq_[hq_h_];
    if (h.type == Hq::Hello) { n = lb::build_hello_reply(o, cap); inflight_ = Inflight::Hello; }
    else if (h.type == Hq::Ack) { n = lb::build_init_ack(h.svc, o, cap); inflight_ = Inflight::Ack; inflight_arg_ = h.svc; }
    else { n = lb::build_table_request(o, cap); inflight_ = Inflight::Table; }
    return ans_len_ = n;
  }

  if (phase_ == Phase::Running && wq_n_ && !confirm_.active) {
    const UserWrite& w = wq_[wq_h_];
    n = lb::build_write_param(w.grp, w.id, w.flag, w.value, o, cap);
    if (n) { inflight_ = Inflight::UserWrite; return ans_len_ = n; }
  }

  if (phase_ == Phase::Startup && !su_sent_) {
    if (su_step_ == SU_VER) {
      n = lb::build_version_request(DST_FREEZER, o, cap);
      inflight_ = Inflight::Version;
    } else if (su_step_ == SU_N01) {
      n = lb::build_version_request(DST_NODE01, o, cap);
      inflight_ = Inflight::Node01;
    } else if (su_step_ >= SU_ID0 && su_step_ < SU_ID0 + IDENT_PAGES) {
      uint8_t recs[24][3];
      size_t rn = ident_page((uint8_t)(su_step_ - SU_ID0), recs);
      n = lb::build_read_request(recs, rn, o, cap);
      inflight_ = Inflight::Ident;
    } else if (su_step_ == SU_STATUS) {
      n = lb::build_read_request(STATUS_LIST, STATUS_COUNT, o, cap);
      inflight_ = Inflight::Status;
    }
    if (n) return ans_len_ = n;
  }

  auto status_read = [&]() {
    n = lb::build_read_request(STATUS_LIST, STATUS_COUNT, o, cap);
    if (n) inflight_ = Inflight::Status;
    return ans_len_ = n;
  };
  // The read-back of a write must not queue behind the network record (several messages right after start-up or an
  // IP change): the write's 3 s confirm window is already running.
  if (phase_ == Phase::Running && await_ == Await::None && confirm_.active && read_due_) {
    if (status_read()) return ans_len_;
  }

  if (phase_ == Phase::Running && net_enabled_ && net_.connected) {
    uint8_t p[64];
    if (net_step_ == N_STATE3 || net_step_ == N_STATE4 || net_step_ == N_STATE5 ||
        (net_step_ == N_WAIT5 && now_ - net_t4_ >= ms_to_us(NET_FINAL_DELAY_MS))) {
      uint8_t v = net_step_ == N_STATE3 ? 3 : net_step_ == N_STATE4 ? 4 : 5;
      n = lb::build_write_param(0x48, 0x00, 0x20, v, o, cap);
      inflight_ = Inflight::NetState;
      inflight_arg_ = v;
    } else if (net_step_ == N_TEXT || net_dirty()) {
      net_snap_ = net_;
      size_t pn = build_net_text(p, net_snap_);
      n = body(DST_FREEZER, 0x24, p, pn);
      inflight_ = Inflight::NetText;
    } else if (net_step_ == N_MAC) {
      size_t pn = 0;
      p[pn++] = 0x48; p[pn++] = 0x0C; p[pn++] = 0x80;
      memcpy(p + pn, net_.mac, 4); pn += 4;
      p[pn++] = 0x48; p[pn++] = 0x0D; p[pn++] = 0x80;
      memcpy(p + pn, net_.mac + 4, 2); pn += 2;
      p[pn++] = 0; p[pn++] = 0;
      p[pn++] = 0x48; p[pn++] = 0x03; p[pn++] = 0x20; p[pn++] = 0x01;  // Wi-Fi connected
      n = body(DST_FREEZER, 0x24, p, pn);
      inflight_ = Inflight::NetMac;
    }
    if (n) return ans_len_ = n;
  }

  if (phase_ == Phase::Running && await_ == Await::None) {
    bool periodic = !read_ever_ || now_ - last_read_sent_ >= ms_to_us(status_period_ms_);
    if ((read_due_ || periodic) && status_read()) return ans_len_;
  }

  n = lb::build_idle(o, cap);
  inflight_ = Inflight::Idle;
  return ans_len_ = n;
}

size_t Engine::next_answer(uint8_t* out, size_t cap, uint64_t now_us) {
  bump_time(now_us);
  size_t n = build_answer();
  if (!n || n > cap) { inflight_ = Inflight::None; return 0; }
  memcpy(out, ans_, n);
  return n;
}

void Engine::on_slot02_result(bool sent) {
  if (inflight_ == Inflight::None) return;
  if (!sent) {
    if (inflight_ != Inflight::Idle) cnt_.missed_slots++;
    return;  // stays queued: the next next_answer() offers it again
  }
  commit();
}

void Engine::commit() {
  Inflight k = inflight_;
  inflight_ = Inflight::None;
  if (k != Inflight::Idle) cnt_.tx_slots++;
  auto pop_hq = [&]() { if (hq_n_) { hq_h_ = (uint8_t)((hq_h_ + 1) % 6); hq_n_--; } };
  auto arm = [&](Await a) { await_ = a; await_deadline_ = now_ + ms_to_us(REPLY_TIMEOUT_MS); };
  switch (k) {
    case Inflight::Hello:
    case Inflight::Table:
      pop_hq();
      break;
    case Inflight::Ack:
      pop_hq();
      if (inflight_arg_ == 0x23 && phase_ != Phase::Running && phase_ != Phase::Startup) {
        phase_ = Phase::Startup;  // handshake complete
        su_step_ = SU_VER;
        su_sent_ = false;
        change();
      }
      break;
    case Inflight::UserWrite: {
      confirm_ = Confirm();
      confirm_.active = true;
      confirm_.w = wq_[wq_h_];
      confirm_.t_sent = now_;
      wq_h_ = (uint8_t)((wq_h_ + 1) % 4);
      wq_n_--;
      cnt_.writes_sent++;
      break;
    }
    case Inflight::Version:
      su_sent_ = true;
      arm(Await::Version);
      break;
    case Inflight::Node01:
      node01_open_ = true;  // the answer comes in slot 01; accepted for 2 s, never awaited
      node01_t_ = now_;
      su_advance();
      break;
    case Inflight::Ident:
      su_sent_ = true;
      arm(Await::Ident);
      break;
    case Inflight::Status:
      read_ever_ = true;
      read_due_ = false;
      last_read_sent_ = now_;
      arm(Await::Status);
      if (phase_ == Phase::Startup && su_step_ == SU_STATUS) su_sent_ = true;
      if (confirm_.active && (confirm_.event_seen || confirm_.fallback)) confirm_.read_sent = true;
      break;
    case Inflight::NetState:
      cnt_.net_writes++;
      if (net_step_ == N_STATE3) net_step_ = N_TEXT;
      else if (net_step_ == N_STATE4) { net_step_ = N_WAIT5; net_t4_ = now_; }
      else if (net_step_ == N_STATE5 || net_step_ == N_WAIT5) net_step_ = N_DONE;
      break;
    case Inflight::NetText:
      cnt_.net_writes++;
      net_written_ = net_snap_;
      if (net_step_ == N_TEXT) net_step_ = N_MAC;
      break;
    case Inflight::NetMac:
      cnt_.net_writes++;
      net_step_ = N_STATE4;
      break;
    default:
      break;
  }
}

}  // namespace n02
