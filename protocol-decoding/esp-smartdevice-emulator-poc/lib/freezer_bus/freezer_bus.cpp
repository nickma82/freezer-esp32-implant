#include "freezer_bus.h"

#include <stdio.h>
#include <string.h>

namespace lb {

uint16_t crc16_xmodem(const uint8_t* data, size_t n) {
  uint16_t c = 0;
  for (size_t i = 0; i < n; i++) {
    c ^= (uint16_t)data[i] << 8;
    for (int k = 0; k < 8; k++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
  }
  return c;
}

int record_width(uint8_t flag) {
  switch (flag) {
    case 0x00: return 0;
    case 0x20: case 0x21: return 1;
    case 0x40: case 0x41: return 2;
    case 0x80: case 0x82: case 0x84: return 4;
    default: return -1;
  }
}

// Python: protocol.parse_records
static Status parse_records(const uint8_t* b, size_t n, bool with_values, Msg& m) {
  size_t i = 0;
  m.nrecs = 0;
  while (i < n) {
    if (i + 3 > n) return Status::ErrTruncated;
    int w = record_width(b[i + 2]);
    if (w < 0) return Status::ErrFlag;
    size_t vn = with_values ? (size_t)w : 0;
    if (i + 3 + vn > n) return Status::ErrTruncated;
    if (m.nrecs >= MAX_RECS) return Status::ErrTooLong;
    Rec& r = m.recs[m.nrecs++];
    r.grp = b[i]; r.id = b[i + 1]; r.flag = b[i + 2]; r.vlen = (uint8_t)vn;
    memset(r.value, 0, sizeof r.value);
    memcpy(r.value, b + i + 3, vn);
    i += 3 + vn;
  }
  return Status::Ok;
}

Status decode(const uint8_t* f, size_t n, Msg& m) {
  m = Msg();
  if (n == 0) return Status::ErrEmpty;
  if (n > MAX_FRAME) return Status::ErrTooLong;
  memcpy(m.raw, f, n);
  m.raw_len = (uint8_t)n;
  m.slot = f[0];
  if (n == 1) return Status::Ok;  // bare 0C / 7E / 7F
  m.kind = Kind::Unknown;
  size_t body_n = n >= 3 ? n - 3 : 0;
  m.crc_ok = n >= 4 && crc16_xmodem(f + 1, n - 3) == (uint16_t)(f[n - 2] | (f[n - 1] << 8));
  if (body_n < 4) return Status::ErrTruncated;
  const uint8_t* body = f + 1;
  m.dst = body[0]; m.src = body[1]; m.svc = body[2]; m.flag = body[3];
  bool ack = m.svc == 0x30 && (m.flag == 0x00 || m.flag == 0x01);
  bool list = m.svc == 0x03 && m.flag == 0x01;
  bool values = (m.svc == 0x24 || m.svc == 0x13) && m.flag == 0x01;
  if (ack) { m.kind = Kind::Ack; return Status::Ok; }
  if (!list && !values) return Status::Ok;  // tag not understood: kept raw
  if (body_n < 5) return Status::ErrTruncated;
  size_t plen = body_n - 5;
  if (body[4] != plen) return Status::ErrLength;
  Status s = parse_records(body + 5, plen, values, m);
  if (s != Status::Ok) return s;
  m.kind = list ? Kind::List : Kind::Values;
  return Status::Ok;
}

size_t encode(const Msg& m, uint8_t* out, size_t cap) {
  if (m.kind == Kind::Empty) { if (cap < 1) return 0; out[0] = m.slot; return 1; }
  if (m.kind == Kind::Unknown) { if (cap < m.raw_len) return 0; memcpy(out, m.raw, m.raw_len); return m.raw_len; }
  uint8_t body[MAX_FRAME];
  size_t n = 0;
  body[n++] = m.dst; body[n++] = m.src; body[n++] = m.svc; body[n++] = m.flag;
  if (m.kind == Kind::Ack) {
    if (m.raw_len < 7) return 0;
    size_t tail = m.raw_len - 2 - 5;  // raw[1+4:-2]
    memcpy(body + n, m.raw + 5, tail);
    n += tail;
  } else {
    size_t lenpos = n++;
    for (uint8_t i = 0; i < m.nrecs; i++) {
      const Rec& r = m.recs[i];
      if (n + 3 + r.vlen > sizeof body) return 0;
      body[n++] = r.grp; body[n++] = r.id; body[n++] = r.flag;
      memcpy(body + n, r.value, r.vlen);
      n += r.vlen;
    }
    body[lenpos] = (uint8_t)(n - lenpos - 1);
  }
  if (1 + n + 2 > cap) return 0;
  uint16_t c = crc16_xmodem(body, n);
  out[0] = m.slot;
  memcpy(out + 1, body, n);
  out[1 + n] = (uint8_t)c; out[2 + n] = (uint8_t)(c >> 8);
  return 3 + n;
}

// ---- semantics ----------------------------------------------------------------------
static uint32_t be_u(const uint8_t* v, size_t n) { uint32_t x = 0; for (size_t i = 0; i < n; i++) x = (x << 8) | v[i]; return x; }
static int32_t be_s(const uint8_t* v, size_t n) {  // int.from_bytes(v, 'big', signed=True)
  if (n == 0) return 0;
  uint32_t x = be_u(v, n);
  if (n < 4 && (v[0] & 0x80)) x |= ~0u << (8 * n);
  return (int32_t)x;
}
static const Rec* find_last(const Msg& m, uint8_t g, uint8_t id) {
  for (int i = (int)m.nrecs - 1; i >= 0; i--) if (m.recs[i].grp == g && m.recs[i].id == id) return &m.recs[i];
  return nullptr;
}
static void add(Interp& o, const char* name, int32_t num, const char* text) {
  if (o.n >= MAX_ITEMS) return;
  Item& it = o.items[o.n++];
  it.name = name; it.num = num;
  snprintf(it.text, sizeof it.text, "%s", text);
}
static void add_int(Interp& o, const char* name, int32_t v) { char t[16]; snprintf(t, sizeof t, "%ld", (long)v); add(o, name, v, t); }
static void add_bool(Interp& o, const char* name, bool v) { add(o, name, v, v ? "1" : "0"); }

struct EventName { uint32_t a, b; const char* name; };
static const EventName EVENT_FLAGS[] = {  // Python: protocol.EVENT_FLAGS
    {0x01000000, 0x08000000, "value_changed"},
    {0x00800000, 0x04000000, "party_changed"},
    {0x00000100, 0x08000000, "door_alarm_changed"},
};

void interpret(const Msg& m, Interp& o) {
  o = Interp();
  if (m.kind != Kind::Values || !m.crc_ok) return;
  if (m.dst == 0x80 && m.src == 0x7F && find_last(m, 0, 0)) {  // FF 80 7F 24 01 20 ... event block
    for (uint8_t w = 0; w < 4; w++) if (const Rec* r = find_last(m, 0, w)) o.flags[w] = be_u(r->value, r->vlen);
    if (const Rec* r = find_last(m, 0, 4)) { o.has_counter = true; o.counter = r->vlen ? r->value[r->vlen - 1] : 0; }
    if (!o.flags[0] && !o.flags[1]) return;
    o.is_event = true;
    char t[32] = {0};
    snprintf(t, sizeof t, "unknown_%08lX_%08lX", (unsigned long)o.flags[0], (unsigned long)o.flags[1]);
    for (const EventName& e : EVENT_FLAGS) if (e.a == o.flags[0] && e.b == o.flags[1]) snprintf(t, sizeof t, "%s", e.name);
    add(o, "event", 0, t);
    return;
  }
  for (uint8_t i = 0; i < m.nrecs; i++) {  // first appearance order, last value, like a Python dict
    uint8_t g = m.recs[i].grp, id = m.recs[i].id;
    bool dup = false;
    for (uint8_t j = 0; j < i; j++) if (m.recs[j].grp == g && m.recs[j].id == id) dup = true;
    if (dup) continue;
    const Rec* r = find_last(m, g, id);
    if (!r->vlen) continue;  // not available
    const uint8_t* v = r->value;
    if (g == 0x0C && id == 0x00) {
      if (v[0] == 1) add(o, "mode", 1, "normal");
      else if (v[0] == 4) add(o, "mode", 4, "superfrost");
      else add_int(o, "mode", v[0]);
    } else if (g == 0x08 && id == 0x02) add_bool(o, "party", v[0] != 0);
    else if (g == 0x0C && id == 0x06) add_int(o, "setpoint", be_s(v + 1, r->vlen - 1));
    else if (g == 0x0C && id == 0x14) add_int(o, "current_temp", be_s(v + 1, r->vlen - 1));
    else if (g == 0x0C && id == 0x31) add_bool(o, "door_open", v[0] != 0);
    else if (g == 0x0C && id == 0x09) add_bool(o, "door_alarm", v[0] != 0);
    else if (g == 0x14 && id == 0x04) add_bool(o, "door_push", v[0] != 0);  // pushed by node 0B
  }
}

size_t describe(const Interp& in, char* out, size_t cap) {
  size_t n = 0;
  out[0] = 0;
  for (uint8_t i = 0; i < in.n && n + 1 < cap; i++) {
    const Item& it = in.items[i];
    const char* sep = i ? ", " : "";
    int w;
    bool b = it.num != 0;
    if (!strcmp(it.name, "event")) {
      char t[32];
      snprintf(t, sizeof t, "%s", it.text);
      if (strncmp(t, "unknown_", 8)) for (char* c = t; *c; c++) if (*c == '_') *c = ' ';
      w = snprintf(out + n, cap - n, "%sevent %s", sep, t);
    } else if (!strcmp(it.name, "door_open") || !strcmp(it.name, "door_push")) {
      w = snprintf(out + n, cap - n, "%sdoor %s", sep, b ? "open" : "closed");
    } else if (!strcmp(it.name, "door_alarm")) {
      w = snprintf(out + n, cap - n, "%sdoor alarm %s", sep, b ? "on" : "off");
    } else if (!strcmp(it.name, "party")) {
      w = snprintf(out + n, cap - n, "%sparty = %s", sep, b ? "on" : "off");
    } else {
      w = snprintf(out + n, cap - n, "%s%s = %s", sep, it.name, it.text);
    }
    if (w < 0) break;
    n = (size_t)w + n < cap ? n + (size_t)w : cap - 1;
  }
  return n;
}

// ---- builders ----------------------------------------------------------------------
size_t build_body(uint8_t dst, uint8_t src, uint8_t svc, uint8_t flag, const uint8_t* payload, size_t plen,
                  bool with_len, uint8_t* out, size_t cap) {
  size_t n = 4 + (with_len ? 1 : 0) + plen;
  if (n + 2 > cap || n > MAX_FRAME) return 0;
  out[0] = dst; out[1] = src; out[2] = svc; out[3] = flag;
  size_t i = 4;
  if (with_len) out[i++] = (uint8_t)plen;
  if (plen) memcpy(out + i, payload, plen);
  uint16_t c = crc16_xmodem(out, n);
  out[n] = (uint8_t)c; out[n + 1] = (uint8_t)(c >> 8);
  return n + 2;
}

size_t build_idle(uint8_t* out, size_t cap) {
  static const uint8_t p[] = {0x02, 0x00, 0x00};
  return build_body(0xFF, 0x02, 0x30, 0x01, p, sizeof p, false, out, cap);
}

size_t build_write_param(uint8_t grp, uint8_t id, uint8_t flag, uint32_t value, uint8_t* out, size_t cap) {
  int w = record_width(flag);
  if (w < 0) return 0;
  if (w < 4 && (value >> (8 * w)) != 0) return 0;  // Python: OverflowError
  uint8_t p[7] = {grp, id, flag};
  for (int i = 0; i < w; i++) p[3 + i] = (uint8_t)(value >> (8 * (w - 1 - i)));
  return build_body(0xFF, 0x02, 0x24, 0x01, p, 3 + (size_t)w, true, out, cap);
}

size_t build_read_request(const uint8_t (*params)[3], size_t n, uint8_t* out, size_t cap) {
  uint8_t p[MAX_FRAME];
  if (n * 3 > sizeof p - 8) return 0;
  for (size_t i = 0; i < n; i++) memcpy(p + 3 * i, params[i], 3);
  return build_body(0xFF, 0x02, 0x03, 0x01, p, 3 * n, true, out, cap);
}

size_t build_hello_reply(uint8_t* out, size_t cap) {
  static const uint8_t p[] = {0x03, 0x01, 0x00, 0x00, 0x00, 0x00};
  return build_body(0xFF, 0x02, 0x11, 0x01, p, sizeof p, true, out, cap);
}

size_t build_init_ack(uint8_t svc, uint8_t* out, size_t cap) {
  const uint8_t p[] = {0x02, svc, 0x00};
  return build_body(0xFF, 0x02, 0x30, 0x01, p, sizeof p, false, out, cap);
}

size_t build_table_request(uint8_t* out, size_t cap) {
  static const uint8_t p[] = {0x13, 0x00, 0x00, 0x00};
  return build_body(0xFF, 0x02, 0x02, 0x01, p, sizeof p, true, out, cap);
}

size_t build_version_request(uint8_t dst, uint8_t* out, size_t cap) {
  return build_body(dst, 0x02, 0x06, 0x01, nullptr, 0, true, out, cap);
}

// ---- schedule / latency tracker ---------------------------------------------------------
void ScheduleTracker::slot(uint8_t s, uint64_t t_us) {
  if (seen_[s] && t_us > last_t_[s]) {
    uint64_t d = t_us - last_t_[s];
    uint32_t p = d > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)d;
    period_[s] = p;
    uint32_t ms = p / 1000;
    if (ms >= 225 && ms <= 345) n285_[s]++;
    else if (ms >= 510 && ms <= 630) n570_[s]++;
    else other_[s]++;
  }
  seen_[s] = true;
  last_t_[s] = t_us;
}

bool ScheduleTracker::last_period_us(uint8_t s, uint32_t& p) const {
  if (!period_[s]) return false;
  p = period_[s];
  return true;
}

void ScheduleTracker::period_stats(uint8_t s, uint32_t& a, uint32_t& b, uint32_t& o) const {
  a = n285_[s]; b = n570_[s]; o = other_[s];
}

void ScheduleTracker::latency(uint8_t src, uint32_t lat_us) {
  Src* s = nullptr;
  for (Src& x : src_) if (x.used && x.id == src) { s = &x; break; }
  if (!s) for (Src& x : src_) if (!x.used) { x.used = true; x.id = src; s = &x; break; }
  if (!s) return;
  s->v[s->head] = lat_us;
  s->head = (uint8_t)((s->head + 1) % WINDOW);
  if (s->count < WINDOW) s->count++;
}

size_t ScheduleTracker::latencies(Latency* out, size_t cap) const {
  size_t k = 0;
  for (const Src& s : src_) {
    if (!s.used || !s.count || k >= cap) continue;
    uint32_t a[WINDOW];
    for (uint8_t i = 0; i < s.count; i++) a[i] = s.v[i];
    for (uint8_t i = 1; i < s.count; i++) {  // insertion sort, n <= 64
      uint32_t x = a[i]; int j = i - 1;
      while (j >= 0 && a[j] > x) { a[j + 1] = a[j]; j--; }
      a[j + 1] = x;
    }
    uint32_t med = s.count % 2 ? a[s.count / 2] : (a[s.count / 2 - 1] + a[s.count / 2]) / 2;
    out[k++] = Latency{s.id, s.count, med, a[s.count - 1] - a[0]};
  }
  return k;
}

void ScheduleTracker::clear_latencies() { for (Src& s : src_) { s.head = 0; s.count = 0; } }

}  // namespace lb
