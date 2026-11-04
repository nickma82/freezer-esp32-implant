#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "freezer_master.h"

using namespace fm;

void setUp() {}
void tearDown() {}

struct Sim {
  Master m;
  uint32_t t_us = 0;
  uint32_t now_ms() const { return t_us / 1000; }
  Slot step() { Slot s; m.next(now_ms(), s); t_us += s.ticks * TICK_US; return s; }
  // run until a slot with the given byte carrying a body appears (max 40 slots)
  bool until_body(uint8_t slot, Slot& out) {
    for (int i = 0; i < 40; i++) { out = step(); if (out.slot == slot && out.len) return true; }
    return false;
  }
};

static size_t hex(const char* s, uint8_t* out) {
  size_t n = 0;
  while (*s) { if (*s == ' ') { s++; continue; } unsigned v; sscanf(s, "%2x", &v); out[n++] = (uint8_t)v; s += 2; }
  return n;
}
static void expect(const char* want, const Slot& s) {
  uint8_t w[160];
  size_t n = hex(want, w);
  TEST_ASSERT_EQUAL_UINT32(n, (uint32_t)s.len + 1);
  TEST_ASSERT_EQUAL_HEX8(w[0], s.slot);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(w + 1, s.body, s.len);
}
static void box(Sim& s, const char* body) { uint8_t b[160]; size_t n = hex(body, b); s.m.on_box(b, n); }

void test_schedule_shape() {
  Sim s;
  uint32_t ticks = 0;
  int n02 = 0, n82 = 0;
  static const uint8_t want[12] = {0x7E, 0x01, 0x7F, 0x0B, 0x0C, 0x7F, 0x7E, 0x7E, 0x7F, 0x0B, 0x0C, 0x7F};
  for (int i = 0; i < 24; i++) {
    Slot x = s.step();
    ticks += x.ticks;
    if (i % 12 == 6 && (i / 12) == 1) { TEST_ASSERT_EQUAL_HEX8(0x02, x.slot); n02++; }
    else if (i == 6) { TEST_ASSERT_TRUE(x.slot == 0x82); n82++; }  // hello in the first half
    else if (i % 12 != 8) TEST_ASSERT_EQUAL_HEX8(want[i % 12], x.slot);
    if (x.slot == 0x02) TEST_ASSERT_EQUAL_UINT8(0, x.len);
  }
  TEST_ASSERT_EQUAL_UINT32(32, ticks);  // two half cycles of 16 ticks
  TEST_ASSERT_EQUAL(1, n02);
  TEST_ASSERT_EQUAL(1, n82);
}

void test_handshake_replay() {
  Sim s;
  Slot x;
  TEST_ASSERT_TRUE(s.until_body(0x82, x));
  expect("82 82 7F 01 01 00 C9 F4", x);
  box(s, "FF 02 11 01 06 03 01 00 00 00 00 1B 5D");
  TEST_ASSERT_TRUE(s.until_body(0x82, x));
  expect("82 82 7F 21 01 00 0F 72", x);
  box(s, "FF 02 30 01 02 21 00 DC 0F");
  box(s, "FF 02 02 01 04 13 00 00 00 A5 2F");  // box sends both before the freezer's reply slot comes round
  TEST_ASSERT_TRUE(s.until_body(0x82, x));
  expect("82 82 7F 22 01 00 5F 2B", x);
  TEST_ASSERT_TRUE(s.until_body(0x82, x));
  expect("82 82 7F 12 01 04 13 00 00 04 DA 4B", x);
  box(s, "FF 02 30 01 02 22 00 8F 5A");
  TEST_ASSERT_TRUE(s.until_body(0x82, x));
  expect("82 82 7F 23 01 00 6F 1C", x);
  TEST_ASSERT_EQUAL(1, (int)s.m.handshake() == (int)Hs::Running);
}

void test_read_identity_and_unknown() {
  Sim s;
  Slot x;
  box(s, "FF 02 03 01 30 3C 00 20 3C 01 80 3C 02 80 3C 03 80 3C 04 80 3C 05 80 3C 06 80 3C 07 80 3C 08 80 3C 09 80 3C 0D 80 3C 0E 80 3C 0F 80 3C 10 80 3C 11 80 3C 12 80 A2 0D");
  TEST_ASSERT_TRUE(s.until_body(0x82, x));
  uint8_t w[] = {0x82, 0x7F, 0x13, 0x01};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(w, x.body, 4);
  uint8_t head[] = {0x3C, 0x00, 0x20, 0x1D, 0x3C, 0x01, 0x80, 0x46, 0x4E, 0x63, 0x20};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(head, x.body + 5, sizeof head);
  TEST_ASSERT_TRUE(x.body[4] <= MAX_REPLY_PAYLOAD);
  uint8_t full[160];
  full[0] = 0x82; memcpy(full + 1, x.body, x.len);
  lb::Msg m;
  TEST_ASSERT_EQUAL(0, (int)lb::decode(full, x.len + 1, m));
  TEST_ASSERT_TRUE(m.crc_ok);

  box(s, "FF 02 03 01 03 0C F9 20 00 00");  // CRC wrong on purpose: must be dropped, not answered
  TEST_ASSERT_EQUAL(1, (int)s.m.stats().box_bad_crc);
  uint8_t p[] = {0x0C, 0xF9, 0x20};
  uint8_t b[32];
  size_t n = lb::build_body(0xFF, 0x02, 0x03, 0x01, p, 3, true, b, sizeof b);
  s.m.on_box(b, n);
  TEST_ASSERT_TRUE(s.until_body(0x82, x));
  uint8_t unk[] = {0x0C, 0xF9, 0x00};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(unk, x.body + 5, 3);
  TEST_ASSERT_EQUAL(1, (int)s.m.stats().unknown_params);
}

void test_write_and_event_block() {
  Sim s;
  Slot x;
  uint8_t b[32];
  size_t n = lb::build_write_param(0x0C, 0x00, 0x20, 0x04, b, sizeof b);
  s.m.on_box(b, n);
  TEST_ASSERT_EQUAL(1, (int)s.m.stats().writes);
  TEST_ASSERT_EQUAL_UINT32(4, s.m.find(0x0C, 0x00)->value);
  uint8_t e1[160], e2[160];
  int got = 0;
  uint8_t ctr[3] = {0, 0, 0};
  for (int i = 0; i < 200 && got < 3; i++) {
    x = s.step();
    if (x.slot != 0xFF) continue;
    uint8_t* e = got == 0 ? e1 : e2;
    memcpy(e, x.body, x.len);
    TEST_ASSERT_EQUAL_HEX8(0x80, x.body[0]);
    TEST_ASSERT_EQUAL_HEX8(0x24, x.body[2]);
    TEST_ASSERT_EQUAL_HEX8(0x20, x.body[4]);
    ctr[got] = x.body[36];
    if (got == 0) {  // flags after a write: 00/00 = 01000000, 00/01 = 08000000
      uint8_t f0[] = {0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0x00};
      uint8_t f1[] = {0x00, 0x01, 0x80, 0x08, 0x00, 0x00, 0x00};
      TEST_ASSERT_EQUAL_HEX8_ARRAY(f0, x.body + 5, 7);
      TEST_ASSERT_EQUAL_HEX8_ARRAY(f1, x.body + 12, 7);
    } else {
      uint8_t z[8] = {0};
      TEST_ASSERT_EQUAL_HEX8_ARRAY(z, x.body + 8, 4);  // flags cleared again
    }
    got++;
  }
  TEST_ASSERT_EQUAL(3, got);
  TEST_ASSERT_EQUAL_HEX8(0x00, ctr[0]);
  TEST_ASSERT_EQUAL_HEX8(0x10, ctr[1]);
  TEST_ASSERT_EQUAL_HEX8(0x20, ctr[2]);
  uint8_t full[160];
  full[0] = 0xFF; memcpy(full + 1, e1, 39);
  lb::Msg m;
  TEST_ASSERT_EQUAL(0, (int)lb::decode(full, 40, m));
  TEST_ASSERT_TRUE(m.crc_ok);
}

void test_hello_retry() {
  Sim s;
  Slot x;
  TEST_ASSERT_TRUE(s.until_body(0x82, x));
  int hellos = 0;
  for (int i = 0; i < 600; i++) { x = s.step(); if (x.slot == 0x82 && x.len && x.body[2] == 0x01) hellos++; }
  TEST_ASSERT_TRUE(hellos >= 1);  // silent box: hello is repeated about every 5 s
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_schedule_shape);
  RUN_TEST(test_handshake_replay);
  RUN_TEST(test_read_identity_and_unknown);
  RUN_TEST(test_write_and_event_block);
  RUN_TEST(test_hello_retry);
  return UNITY_END();
}
