// Shared helpers for the engine tests: hex parsing and a stand-in for the C3 bus driver.
#pragma once
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "freezer_bus.h"
#include "node02_engine.h"

static inline size_t hex(const char* s, uint8_t* out) {
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

static inline void expect_hex(const char* want, const uint8_t* got, size_t n) {
  uint8_t w[lb::MAX_FRAME];
  size_t wn = hex(want, w);
  TEST_ASSERT_EQUAL_UINT32((uint32_t)wn, (uint32_t)n);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(w, got, wn);
}

// Acts like BusC3: feeds frames, asks for the answer, "sends" it into slot 02, echoes it back, reports the result.
struct Bus {
  n02::Engine e;
  uint64_t t = 0;
  uint8_t last[n02::MAX_ANSWER];
  size_t last_n = 0;

  void frame(const char* h) { uint8_t b[lb::MAX_FRAME]; size_t n = hex(h, b); e.on_frame(b, n, t); }
  void frame(const uint8_t* b, size_t n) { e.on_frame(b, n, t); }
  void advance(uint32_t ms) { t += (uint64_t)ms * 1000; e.tick(t); }
  // One slot 02. send == false models a missed slot / TX problem (nothing on the wire, result false).
  size_t slot02(bool send = true) {
    last_n = e.next_answer(last, sizeof last, t);
    if (send && last_n) {
      uint8_t f[lb::MAX_FRAME];
      f[0] = 0x02;
      memcpy(f + 1, last, last_n);
      e.on_frame(f, last_n + 1, t);
      e.on_slot02_result(true);
    } else {
      uint8_t f[1] = {0x02};
      e.on_frame(f, 1, t);
      e.on_slot02_result(false);
    }
    return last_n;
  }
  void expect_sent(const char* want) { TEST_ASSERT_TRUE(last_n > 0); expect_hex(want, last, last_n); }
  // Slot 02 every 570 ms with timeouts running, until the phase is reached (max 60 slots).
  bool run_until(n02::Phase p) {
    for (int i = 0; i < 60 && e.phase() != p; i++) { advance(570); slot02(); }
    return e.phase() == p;
  }
  void handshake() {
    frame("82 82 7F 01 01 00 C9 F4");  slot02();
    frame("82 82 7F 21 01 00 0F 72");  slot02();
    frame("82 82 7F 22 01 00 5F 2B");  slot02();  slot02();
    frame("82 82 7F 12 01 04 13 00 00 04 DA 4B");
    frame("82 82 7F 23 01 00 6F 1C");  slot02();
  }
  // Handshake, then the start-up steps time out (no freezer behind this Bus), then Running.
  void to_running() {
    handshake();
    TEST_ASSERT_TRUE(run_until(n02::Phase::Running));
  }
};

// Build a freezer -> `dst` reply `13` frame (slot byte = dst) from a payload.
static inline size_t reply13(uint8_t dst, const uint8_t* payload, size_t n, uint8_t* out) {
  out[0] = dst;
  return 1 + lb::build_body(dst, 0x7F, 0x13, 0x01, payload, n, true, out + 1, lb::MAX_FRAME - 1);
}
