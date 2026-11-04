#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "freezer_bus.h"
#include "vectors.h"

using namespace lb;

void setUp() {}
void tearDown() {}

static size_t hex(const char* s, uint8_t* out) {
  size_t n = 0;
  while (*s) {
    if (*s == ' ') { s++; continue; }
    unsigned v;
    sscanf(s, "%2x", &v);
    out[n++] = (uint8_t)v;
    s += 2;
  }
  return n;
}

static void expect_bytes(const char* want, const uint8_t* got, size_t n) {
  uint8_t w[MAX_FRAME];
  size_t wn = hex(want, w);
  TEST_ASSERT_EQUAL_UINT32(wn, n);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(w, got, wn);
}

static void render(const Interp& in, char* out, size_t cap) {  // same "name=text;..." format as make_vectors.py
  out[0] = 0;
  for (uint8_t i = 0; i < in.n; i++) {
    size_t n = strlen(out);
    snprintf(out + n, cap - n, "%s%s=%s", i ? ";" : "", in.items[i].name, in.items[i].text);
  }
}

static Status dec(const char* s, Msg& m) {
  uint8_t b[MAX_FRAME];
  size_t n = hex(s, b);
  return decode(b, n, m);
}

// ---- CRC ----
void test_crc_idle() {
  const uint8_t d[] = {0xFF, 0x02, 0x30, 0x01, 0x02, 0x00, 0x00};
  TEST_ASSERT_EQUAL_HEX16(0x3A0B, crc16_xmodem(d, sizeof d));
}

// ---- vectors ----
void test_vectors_decode() {
  for (unsigned i = 0; i < VECTOR_COUNT; i++) {
    const Vector& v = VECTORS[i];
    Msg m;
    char msg[64];
    snprintf(msg, sizeof msg, "vector %u", i);
    TEST_ASSERT_EQUAL_MESSAGE((int)Status::Ok, (int)decode(v.bytes, v.len, m), msg);
    TEST_ASSERT_EQUAL_MESSAGE((int)v.kind, (int)m.kind, msg);
    TEST_ASSERT_EQUAL_MESSAGE(v.crc_ok, m.crc_ok, msg);
    TEST_ASSERT_EQUAL_MESSAGE(v.nrecs, m.nrecs, msg);
    if (v.len > 1) {
      TEST_ASSERT_EQUAL_HEX8_MESSAGE(v.dst, m.dst, msg);
      TEST_ASSERT_EQUAL_HEX8_MESSAGE(v.src, m.src, msg);
      TEST_ASSERT_EQUAL_HEX8_MESSAGE(v.svc, m.svc, msg);
      TEST_ASSERT_EQUAL_HEX8_MESSAGE(v.flag, m.flag, msg);
    }
    Interp in;
    interpret(m, in);
    char got[256];
    render(in, got, sizeof got);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(v.interp, got, msg);
  }
}

void test_vectors_roundtrip() {
  unsigned crc_ok = 0;
  for (unsigned i = 0; i < VECTOR_COUNT; i++) {
    const Vector& v = VECTORS[i];
    Msg m;
    TEST_ASSERT_EQUAL((int)Status::Ok, (int)decode(v.bytes, v.len, m));
    if (!v.crc_ok && v.len > 1) continue;  // encode recomputes the CRC
    uint8_t out[MAX_FRAME];
    size_t n = encode(m, out, sizeof out);
    TEST_ASSERT_EQUAL_UINT32(v.len, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(v.bytes, out, n);
    crc_ok++;
  }
  TEST_ASSERT_GREATER_THAN_UINT32(200, crc_ok);
}

// ---- decode ----
void test_empty_slot() {
  Msg m;
  TEST_ASSERT_EQUAL((int)Status::Ok, (int)dec("7E", m));
  TEST_ASSERT_EQUAL((int)Kind::Empty, (int)m.kind);
  TEST_ASSERT_EQUAL_HEX8(0x7E, m.slot);
}

void test_box_idle() {
  Msg m;
  TEST_ASSERT_EQUAL((int)Status::Ok, (int)dec("02 FF 02 30 01 02 00 00 0B 3A", m));
  TEST_ASSERT_EQUAL_HEX8(0x02, m.slot);
  TEST_ASSERT_EQUAL_HEX8(NODE_FREEZER, node_of(m.dst));
  TEST_ASSERT_EQUAL_HEX8(0x02, m.src);
  TEST_ASSERT_EQUAL_HEX8(0x30, m.svc);
  TEST_ASSERT_EQUAL_HEX8(0x01, m.flag);
  TEST_ASSERT_EQUAL((int)Kind::Ack, (int)m.kind);
  TEST_ASSERT_TRUE(m.crc_ok);
}

void test_node_mapping() {
  TEST_ASSERT_EQUAL_HEX8(0x7F, node_of(0xFF));
  TEST_ASSERT_EQUAL_HEX8(0x02, node_of(0x82));
  TEST_ASSERT_EQUAL_HEX8(0x0B, node_of(0x8B));
  TEST_ASSERT_TRUE(is_broadcast(0x80));
  TEST_ASSERT_FALSE(is_broadcast(0x81));
}

void test_corrupted_message() {
  const char* good = "02 FF 02 24 01 04 0C 00 20 04 5D 69";
  uint8_t b[MAX_FRAME];
  size_t n = hex(good, b);
  for (size_t i = 1; i < n; i++) {  // every byte after the slot byte
    uint8_t c[MAX_FRAME];
    memcpy(c, b, n);
    c[i] ^= 0x10;
    Msg m;
    decode(c, n, m);
    TEST_ASSERT_FALSE(m.crc_ok);
    Interp in;
    interpret(m, in);
    TEST_ASSERT_EQUAL(0, in.n);
  }
}

// ---- records ----
void test_missing_param_record() {
  uint8_t body[16];
  const uint8_t p[] = {0x0C, 0xF9, 0x00};
  size_t n = build_body(0x82, 0x7F, 0x13, 0x01, p, sizeof p, true, body, sizeof body);
  uint8_t f[20];
  f[0] = 0x82;
  memcpy(f + 1, body, n);
  Msg m;
  TEST_ASSERT_EQUAL((int)Status::Ok, (int)decode(f, n + 1, m));
  TEST_ASSERT_EQUAL(1, m.nrecs);
  TEST_ASSERT_EQUAL_HEX8(0xF9, m.recs[0].id);
  TEST_ASSERT_EQUAL(0, m.recs[0].vlen);
}

void test_widths() {
  const uint8_t flags[] = {0x00, 0x20, 0x21, 0x40, 0x41, 0x80, 0x82, 0x84};
  const int w[] = {0, 1, 1, 2, 2, 4, 4, 4};
  for (size_t i = 0; i < sizeof flags; i++) TEST_ASSERT_EQUAL(w[i], record_width(flags[i]));
  TEST_ASSERT_EQUAL(-1, record_width(0x33));
}

void test_decode_errors() {
  Msg m;
  // unknown flag 33 in a values message
  uint8_t body[16], f[20];
  const uint8_t p[] = {0x0C, 0x00, 0x33, 0x01};
  size_t n = build_body(0x82, 0x7F, 0x13, 0x01, p, sizeof p, true, body, sizeof body);
  f[0] = 0x82; memcpy(f + 1, body, n);
  TEST_ASSERT_EQUAL((int)Status::ErrFlag, (int)decode(f, n + 1, m));
  // truncated record (header cut)
  const uint8_t q[] = {0x0C, 0x00};
  n = build_body(0x82, 0x7F, 0x13, 0x01, q, sizeof q, true, body, sizeof body);
  f[0] = 0x82; memcpy(f + 1, body, n);
  TEST_ASSERT_EQUAL((int)Status::ErrTruncated, (int)decode(f, n + 1, m));
  // truncated value: flag 40 with one value byte
  const uint8_t r[] = {0x0C, 0x06, 0x40, 0x00};
  n = build_body(0x82, 0x7F, 0x13, 0x01, r, sizeof r, true, body, sizeof body);
  f[0] = 0x82; memcpy(f + 1, body, n);
  TEST_ASSERT_EQUAL((int)Status::ErrTruncated, (int)decode(f, n + 1, m));
  // length byte mismatch
  const uint8_t s[] = {0x0C, 0x00, 0x20, 0x04};
  n = build_body(0x82, 0x7F, 0x13, 0x01, s, sizeof s, true, body, sizeof body);
  body[4] = 9;
  f[0] = 0x82; memcpy(f + 1, body, n);
  TEST_ASSERT_EQUAL((int)Status::ErrLength, (int)decode(f, n + 1, m));
}

void test_unknown_passthrough() {
  const char* s = "01 FF 01 06 01 00 5B 6A";  // version request, CRC irrelevant for pass-through
  Msg m;
  TEST_ASSERT_EQUAL((int)Status::Ok, (int)dec(s, m));
  TEST_ASSERT_EQUAL((int)Kind::Unknown, (int)m.kind);
  uint8_t out[MAX_FRAME];
  size_t n = encode(m, out, sizeof out);
  expect_bytes(s, out, n);
}

// ---- semantics ----
static void interp_of(const char* s, Interp& in, Msg& m) {
  TEST_ASSERT_EQUAL((int)Status::Ok, (int)dec(s, m));
  interpret(m, in);
}

// builds a full frame from slot + body parts and returns the interpretation
static void interp_body(uint8_t slot, uint8_t dst, uint8_t src, uint8_t svc, const uint8_t* p, size_t pl, Interp& in) {
  uint8_t body[MAX_FRAME], f[MAX_FRAME];
  size_t n = build_body(dst, src, svc, 0x01, p, pl, true, body, sizeof body);
  f[0] = slot; memcpy(f + 1, body, n);
  static Msg m;
  TEST_ASSERT_EQUAL((int)Status::Ok, (int)decode(f, n + 1, m));
  interpret(m, in);
}

void test_setpoint() {
  const uint8_t p[] = {0x0C, 0x06, 0x40, 0x00, 0xEE};
  Interp in;
  interp_body(0x82, 0x82, 0x7F, 0x13, p, sizeof p, in);
  TEST_ASSERT_EQUAL(1, in.n);
  TEST_ASSERT_EQUAL_STRING("setpoint", in.items[0].name);
  TEST_ASSERT_EQUAL_INT32(-18, in.items[0].num);
}

void test_mode_party_temp_door() {
  const uint8_t p[] = {0x0C, 0x00, 0x20, 0x04, 0x08, 0x02, 0x20, 0x02, 0x0C, 0x14, 0x40, 0x00, 0xF0,
                       0x0C, 0x31, 0x20, 0x01, 0x0C, 0x09, 0x20, 0x00};
  Interp in;
  interp_body(0x82, 0x82, 0x7F, 0x13, p, sizeof p, in);
  char got[128];
  render(in, got, sizeof got);
  TEST_ASSERT_EQUAL_STRING("mode=superfrost;party=1;current_temp=-16;door_open=1;door_alarm=0", got);
  describe(in, got, sizeof got);
  TEST_ASSERT_EQUAL_STRING("mode = superfrost, party = on, current_temp = -16, door open, door alarm off", got);
}

void test_door_push() {
  Msg m; Interp in;
  interp_of("0B 80 0B 24 01 04 14 04 20 01 DA 11", in, m);
  TEST_ASSERT_TRUE(m.crc_ok);
  char got[64];
  describe(in, got, sizeof got);
  TEST_ASSERT_EQUAL_STRING("door open", got);
  interp_of("0B 80 0B 24 01 04 14 04 20 00 FB 01", in, m);
  describe(in, got, sizeof got);
  TEST_ASSERT_EQUAL_STRING("door closed", got);
}

static void event_frame(uint32_t w0, uint32_t w1, Interp& in) {
  uint8_t p[40];
  size_t n = 0;
  const uint32_t w[4] = {w0, w1, 0, 0};
  for (uint8_t i = 0; i < 4; i++) {
    p[n++] = 0x00; p[n++] = i; p[n++] = 0x80;
    for (int k = 3; k >= 0; k--) p[n++] = (uint8_t)(w[i] >> (8 * k));
  }
  p[n++] = 0x00; p[n++] = 0x04; p[n++] = 0x20; p[n++] = 0xD0;
  interp_body(0xFF, 0x80, 0x7F, 0x24, p, n, in);
}

void test_event_blocks() {
  Interp in;
  event_frame(0x00000100, 0x08000000, in);
  TEST_ASSERT_TRUE(in.is_event);
  TEST_ASSERT_EQUAL_STRING("door_alarm_changed", in.items[0].text);
  TEST_ASSERT_EQUAL_HEX8(0xD0, in.counter);
  char got[64];
  describe(in, got, sizeof got);
  TEST_ASSERT_EQUAL_STRING("event door alarm changed", got);
  event_frame(0x01000000, 0x08000000, in);
  TEST_ASSERT_EQUAL_STRING("value_changed", in.items[0].text);
  event_frame(0x00800000, 0x04000000, in);
  TEST_ASSERT_EQUAL_STRING("party_changed", in.items[0].text);
  event_frame(0x12345678, 0x0000ABCD, in);
  TEST_ASSERT_EQUAL_STRING("unknown_12345678_0000ABCD", in.items[0].text);  // reported raw, not dropped
  describe(in, got, sizeof got);
  TEST_ASSERT_EQUAL_STRING("event unknown_12345678_0000ABCD", got);
  event_frame(0, 0, in);
  TEST_ASSERT_FALSE(in.is_event);
  TEST_ASSERT_EQUAL(0, in.n);
}

// ---- builders ----
static void expect_built(const char* want, size_t (*fn)(uint8_t*, size_t)) {
  uint8_t out[MAX_FRAME];
  expect_bytes(want, out, fn(out, sizeof out));
}

void test_builders_golden() {
  uint8_t out[MAX_FRAME];
  size_t n;
  n = build_write_param(0x0C, 0x00, 0x20, 0x04, out, sizeof out);
  expect_bytes("FF 02 24 01 04 0C 00 20 04 5D 69", out, n);
  n = build_write_param(0x0C, 0x00, 0x20, 0x01, out, sizeof out);
  expect_bytes("FF 02 24 01 04 0C 00 20 01 F8 39", out, n);
  n = build_write_param(0x08, 0x02, 0x20, 0x02, out, sizeof out);
  expect_bytes("FF 02 24 01 04 08 02 20 02 0A AD", out, n);
  n = build_write_param(0x08, 0x02, 0x20, 0x00, out, sizeof out);
  expect_bytes("FF 02 24 01 04 08 02 20 00 48 8D", out, n);
  n = build_write_param(0x0C, 0x06, 0x40, (uint8_t)-17, out, sizeof out);
  expect_bytes("FF 02 24 01 05 0C 06 40 00 EF D8 99", out, n);
  n = build_write_param(0x0C, 0x06, 0x40, (uint8_t)-16, out, sizeof out);
  expect_bytes("FF 02 24 01 05 0C 06 40 00 F0 06 7A", out, n);
  expect_built("FF 02 30 01 02 00 00 0B 3A", build_idle);
  expect_built("FF 02 11 01 06 03 01 00 00 00 00 1B 5D", build_hello_reply);
  expect_built("FF 02 02 01 04 13 00 00 00 A5 2F", build_table_request);
  n = build_init_ack(0x21, out, sizeof out);
  expect_bytes("FF 02 30 01 02 21 00 DC 0F", out, n);
  n = build_init_ack(0x22, out, sizeof out);
  expect_bytes("FF 02 30 01 02 22 00 8F 5A", out, n);
}

void test_builders_checks() {
  uint8_t out[MAX_FRAME];
  TEST_ASSERT_EQUAL(0, build_write_param(0x0C, 0x00, 0x20, 0x1FF, out, sizeof out));  // too large for the flag
  TEST_ASSERT_EQUAL(0, build_write_param(0x0C, 0x00, 0x33, 1, out, sizeof out));      // unknown flag
  TEST_ASSERT_EQUAL(0, build_idle(out, 4));                                           // buffer too small
  uint8_t d[] = {0xFF, 0x02, 0x06, 0x01, 0x00};
  size_t n = build_version_request(0xFF, out, sizeof out);
  uint16_t c = crc16_xmodem(d, sizeof d);
  TEST_ASSERT_EQUAL(7, n);
  TEST_ASSERT_EQUAL_HEX8((uint8_t)c, out[5]);
  TEST_ASSERT_EQUAL_HEX8((uint8_t)(c >> 8), out[6]);
  n = build_version_request(0x81, out, sizeof out);
  TEST_ASSERT_EQUAL_HEX8(0x81, out[0]);
  TEST_ASSERT_EQUAL_HEX8(0x02, out[1]);
}

void test_read_request_golden() {
  const uint8_t params[][3] = {{0x0C, 0x00, 0x20}, {0x0C, 0x06, 0x40}, {0x0C, 0x07, 0x40}, {0x0C, 0x10, 0x20},
                               {0x0C, 0x2B, 0x40}, {0x0C, 0x14, 0x40}, {0x0C, 0x08, 0x20}};
  uint8_t out[MAX_FRAME];
  size_t n = build_read_request(params, 7, out, sizeof out);
  // the Python helper sends it as node 01 ("01 FF 01 03 ..."); here the box (src 02) sends the same list
  uint8_t d[MAX_FRAME];
  d[0] = 0xFF; d[1] = 0x02; d[2] = 0x03; d[3] = 0x01; d[4] = 21;
  memcpy(d + 5, params, 21);
  uint16_t c = crc16_xmodem(d, 26);
  TEST_ASSERT_EQUAL(28, n);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(d, out, 26);
  TEST_ASSERT_EQUAL_HEX8((uint8_t)c, out[26]);
  TEST_ASSERT_EQUAL_HEX8((uint8_t)(c >> 8), out[27]);
  Msg m;
  uint8_t f[MAX_FRAME];
  f[0] = 0x02; memcpy(f + 1, out, n);
  TEST_ASSERT_EQUAL((int)Status::Ok, (int)decode(f, n + 1, m));
  TEST_ASSERT_EQUAL((int)Kind::List, (int)m.kind);
  TEST_ASSERT_EQUAL(7, m.nrecs);
  TEST_ASSERT_TRUE(m.crc_ok);
}

void test_builders_decode_back() {
  uint8_t b[MAX_FRAME], f[MAX_FRAME];
  size_t n = build_write_param(0x0C, 0x06, 0x40, (uint8_t)-17, b, sizeof b);
  f[0] = 0x02; memcpy(f + 1, b, n);
  Msg m;
  TEST_ASSERT_EQUAL((int)Status::Ok, (int)decode(f, n + 1, m));
  TEST_ASSERT_TRUE(m.crc_ok);
  Interp in;
  interpret(m, in);
  TEST_ASSERT_EQUAL_INT32(-17, in.items[0].num);
  n = build_write_param(0x0C, 0x3A, 0x80, 0xC0080000u, b, sizeof b);
  f[0] = 0x02; memcpy(f + 1, b, n);
  decode(f, n + 1, m);
  TEST_ASSERT_EQUAL_HEX8(0xC0, m.recs[0].value[0]);
  TEST_ASSERT_EQUAL_HEX8(0x00, m.recs[0].value[3]);
}

// ---- tracker (2.8) ----
void test_cycle_clock_wrap() {
  CycleClock c;
  TEST_ASSERT_TRUE(c.extend(100) == 100ull);
  TEST_ASSERT_TRUE(c.extend(0xFFFFFF00u) == 0xFFFFFF00ull);
  TEST_ASSERT_TRUE(c.extend(0x00000010u) == 0x100000010ull);  // wrapped
  TEST_ASSERT_TRUE(c.extend(0xFFFFFFF0u) == 0x1FFFFFFF0ull);
  TEST_ASSERT_TRUE(c.extend(0x00000020u) == 0x200000020ull);  // second wrap
}

void test_period_tracking() {
  ScheduleTracker t;
  uint64_t now = 1000000;
  for (int i = 0; i < 5; i++) { t.slot(0x02, now); now += (i % 2) ? 285000 : 570000; }
  uint32_t p, a, b, o;
  TEST_ASSERT_TRUE(t.last_period_us(0x02, p));
  TEST_ASSERT_EQUAL_UINT32(285000, p);
  t.period_stats(0x02, a, b, o);
  TEST_ASSERT_EQUAL_UINT32(2, a);
  TEST_ASSERT_EQUAL_UINT32(2, b);
  TEST_ASSERT_EQUAL_UINT32(0, o);
  TEST_ASSERT_FALSE(t.last_period_us(0x7E, p));
}

void test_latency_median_spread() {
  ScheduleTracker t;
  const uint32_t box[] = {426, 427, 425, 426, 428, 424, 426};
  for (uint32_t v : box) t.latency(0x02, v);
  t.latency(0x7F, 250);
  t.latency(0x7F, 252);
  ScheduleTracker::Latency l[ScheduleTracker::SOURCES];
  size_t n = t.latencies(l, ScheduleTracker::SOURCES);
  TEST_ASSERT_EQUAL(2, n);
  TEST_ASSERT_EQUAL_HEX8(0x02, l[0].src);
  TEST_ASSERT_EQUAL_UINT32(426, l[0].median);
  TEST_ASSERT_EQUAL_UINT32(4, l[0].spread);
  TEST_ASSERT_EQUAL_UINT32(251, l[1].median);
  t.clear_latencies();
  TEST_ASSERT_EQUAL(0, t.latencies(l, ScheduleTracker::SOURCES));
}

void test_latency_window() {
  ScheduleTracker t;
  for (int i = 0; i < 200; i++) t.latency(0x02, 1000);  // old samples
  for (size_t i = 0; i < ScheduleTracker::WINDOW; i++) t.latency(0x02, 426);
  ScheduleTracker::Latency l[2];
  t.latencies(l, 2);
  TEST_ASSERT_EQUAL_UINT32(426, l[0].median);
  TEST_ASSERT_EQUAL_UINT32(0, l[0].spread);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_crc_idle);
  RUN_TEST(test_vectors_decode);
  RUN_TEST(test_vectors_roundtrip);
  RUN_TEST(test_empty_slot);
  RUN_TEST(test_box_idle);
  RUN_TEST(test_node_mapping);
  RUN_TEST(test_corrupted_message);
  RUN_TEST(test_missing_param_record);
  RUN_TEST(test_widths);
  RUN_TEST(test_decode_errors);
  RUN_TEST(test_unknown_passthrough);
  RUN_TEST(test_setpoint);
  RUN_TEST(test_mode_party_temp_door);
  RUN_TEST(test_door_push);
  RUN_TEST(test_event_blocks);
  RUN_TEST(test_builders_golden);
  RUN_TEST(test_builders_checks);
  RUN_TEST(test_read_request_golden);
  RUN_TEST(test_builders_decode_back);
  RUN_TEST(test_cycle_clock_wrap);
  RUN_TEST(test_period_tracking);
  RUN_TEST(test_latency_median_spread);
  RUN_TEST(test_latency_window);
  return UNITY_END();
}
