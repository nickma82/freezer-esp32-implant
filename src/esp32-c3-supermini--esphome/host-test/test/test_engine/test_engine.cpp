// Unit tests of the node-02 engine on frames captured from the real freezer / box (protocol-decoding/csv).
#include "../test_support.h"

using namespace n02;

static Bus* B;
void setUp() { B = new Bus(); }
void tearDown() { delete B; }

static const char* IDLE = "FF 02 30 01 02 00 00 0B 3A";
static const char* HELLO_REPLY = "FF 02 11 01 06 03 01 00 00 00 00 1B 5D";

// ---- idle / handshake -------------------------------------------------------------------------------------
void test_idle_ack_when_nothing_queued() {
  B->slot02();
  B->expect_sent(IDLE);
  TEST_ASSERT_EQUAL((int)Phase::Waiting, (int)B->e.phase());
}

void test_hello_gets_reply_and_restarts() {
  B->frame("82 82 7F 01 01 00 C9 F4");
  TEST_ASSERT_EQUAL((int)Phase::Handshake, (int)B->e.phase());
  B->slot02();
  B->expect_sent(HELLO_REPLY);
  B->slot02();
  B->expect_sent(IDLE);  // popped after it went out
  TEST_ASSERT_EQUAL_UINT32(1, B->e.counters().hellos);
}

void test_not_sent_message_stays_queued() {
  B->frame("82 82 7F 01 01 00 C9 F4");
  B->slot02(false);  // missed slot
  B->slot02(false);
  B->slot02(true);
  B->expect_sent(HELLO_REPLY);
  TEST_ASSERT_EQUAL_UINT32(2, B->e.counters().missed_slots);
  B->slot02();
  B->expect_sent(IDLE);
}

void test_answer_is_idempotent_until_result() {
  B->frame("82 82 7F 01 01 00 C9 F4");
  uint8_t a[MAX_ANSWER], b[MAX_ANSWER];
  size_t na = B->e.next_answer(a, sizeof a, B->t);
  size_t nb = B->e.next_answer(b, sizeof b, B->t);
  TEST_ASSERT_EQUAL_UINT32((uint32_t)na, (uint32_t)nb);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(a, b, na);
}

void test_handshake_order_and_priority() {
  B->frame("82 82 7F 01 01 00 C9 F4");  B->slot02();  B->expect_sent(HELLO_REPLY);
  B->frame("82 82 7F 21 01 00 0F 72");  B->slot02();  B->expect_sent("FF 02 30 01 02 21 00 DC 0F");
  B->frame("82 82 7F 22 01 00 5F 2B");
  B->slot02();  B->expect_sent("FF 02 30 01 02 22 00 8F 5A");
  B->slot02();  B->expect_sent("FF 02 02 01 04 13 00 00 00 A5 2F");  // table request after the second ack
  B->frame("82 82 7F 12 01 04 13 00 00 04 DA 4B");                    // 12 accepted
  B->frame("82 82 7F 23 01 00 6F 1C");
  TEST_ASSERT_EQUAL((int)Phase::Handshake, (int)B->e.phase());
  B->slot02();  B->expect_sent("FF 02 30 01 02 23 00 BE 69");
  TEST_ASSERT_EQUAL((int)Phase::Startup, (int)B->e.phase());
}

void test_tx_disabled_sends_nothing() {
  B->e.set_tx_enabled(false);
  B->frame("82 82 7F 01 01 00 C9 F4");
  B->slot02();
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)B->last_n);
  B->e.set_tx_enabled(true);
  B->slot02();
  B->expect_sent(HELLO_REPLY);
}

// ---- start-up -------------------------------------------------------------------------------------------------
void test_startup_versions_ident_status_in_order() {
  B->handshake();
  B->advance(570);  B->slot02();  B->expect_sent("FF 02 06 01 00 56 36");   // freezer version
  B->frame("82 82 7F 16 01 1E 54 4F 44 20 35 2D 36 30 30 7C 53 57 20 31 2D 38 36 2D 37 7C 42 55 53 20 31 2D 34 2D 30 00 94 85");
  TEST_ASSERT_EQUAL_STRING("TOD 5-600|SW 1-86-7|BUS 1-4-0", B->e.state().freezer_version);
  B->advance(570);  B->slot02();  B->expect_sent("81 02 06 01 00 78 E4");   // node 01 version
  // node 01 answers in its own slot 01 within 2 s
  B->advance(285);
  B->frame("01 82 01 16 01 1D 54 4F 44 20 35 2D 36 30 30 7C 53 57 20 36 2D 32 2D 30 7C 42 55 53 20 31 2D 34 2D 30 00 EC AD");
  TEST_ASSERT_EQUAL_STRING("TOD 5-600|SW 6-2-0|BUS 1-4-0", B->e.state().node01_version);
  B->advance(285);  B->slot02();
  B->expect_sent("FF 02 03 01 30 3C 00 20 3C 01 80 3C 02 80 3C 03 80 3C 04 80 3C 05 80 3C 06 80 3C 07 80 3C 08 80 3C 09 80 3C 0D 80 3C 0E 80 3C 0F 80 3C 10 80 3C 11 80 3C 12 80 A2 0D");
  B->frame("82 82 7F 13 01 6D 3C 00 20 1D 3C 01 80 46 4E 63 20 3C 02 80 20 20 20 35 3C 03 80 30 37 36 00 3C 04 80 00 00 00 00 3C 05 80 32 30 41 2F 3C 06 80 30 30 31 00 3C 07 80 31 32 33 34 3C 08 80 35 36 37 38 3C 09 80 39 00 00 00 3C 0D 80 39 39 33 35 3C 0E 80 36 37 32 30 3C 0F 80 31 00 00 00 3C 10 80 39 38 37 36 3C 11 80 35 34 33 32 3C 12 80 31 00 00 00 DD F9");
  TEST_ASSERT_EQUAL_STRING("FNc    5076", B->e.state().model);
  TEST_ASSERT_EQUAL_STRING("20A/001", B->e.state().revision);
  TEST_ASSERT_EQUAL_STRING("123456789", B->e.state().serial);
  TEST_ASSERT_EQUAL((int)Phase::Startup, (int)B->e.phase());
  // pages 1..3 of the 3C identity, then the status read
  B->advance(570);  B->slot02();
  lb::Msg m;
  uint8_t f[lb::MAX_FRAME];
  f[0] = 2; memcpy(f + 1, B->last, B->last_n);
  TEST_ASSERT_EQUAL((int)lb::Status::Ok, (int)lb::decode(f, B->last_n + 1, m));
  TEST_ASSERT_TRUE(m.crc_ok);
  TEST_ASSERT_EQUAL(3, m.nrecs);  // 3C/0A..0C
  TEST_ASSERT_EQUAL_HEX8(0x0A, m.recs[0].id);
  TEST_ASSERT_EQUAL_HEX8(0x0C, m.recs[2].id);
}

void test_node01_absent_does_not_block_and_is_unknown() {
  B->handshake();
  TEST_ASSERT_TRUE(B->run_until(Phase::Running));
  TEST_ASSERT_EQUAL_STRING("", B->e.state().node01_version);
  // an answer after the 2 s window is ignored
  B->frame("01 82 01 16 01 1D 54 4F 44 20 35 2D 36 30 30 7C 53 57 20 36 2D 32 2D 30 7C 42 55 53 20 31 2D 34 2D 30 00 EC AD");
  TEST_ASSERT_EQUAL_STRING("", B->e.state().node01_version);
}

// ---- status model -----------------------------------------------------------------------------------------
static const uint8_t STATUS_LIST[8][3] = {{0x0C, 0x00, 0x20}, {0x08, 0x02, 0x20}, {0x0C, 0x06, 0x40}, {0x0C, 0x09, 0x20},
                                          {0x0C, 0x11, 0x20}, {0x0C, 0x12, 0x40}, {0x0C, 0x14, 0x40}, {0x0C, 0x31, 0x20}};

void test_status_request_uses_captured_flags() {
  B->to_running();
  // the status read goes out within one period; find it
  lb::Msg m;
  bool found = false;
  for (int i = 0; i < 40 && !found; i++) {
    B->advance(570);
    B->slot02();
    uint8_t f[lb::MAX_FRAME];
    f[0] = 2; memcpy(f + 1, B->last, B->last_n);
    if (lb::decode(f, B->last_n + 1, m) == lb::Status::Ok && m.svc == 0x03 && m.nrecs == 8) found = true;
  }
  TEST_ASSERT_TRUE(found);
  TEST_ASSERT_TRUE(m.crc_ok);
  for (int i = 0; i < 8; i++) {
    TEST_ASSERT_EQUAL_HEX8(STATUS_LIST[i][0], m.recs[i].grp);
    TEST_ASSERT_EQUAL_HEX8(STATUS_LIST[i][1], m.recs[i].id);
    TEST_ASSERT_EQUAL_HEX8(STATUS_LIST[i][2], m.recs[i].flag);
  }
}

static void feed_status(Bus& b, uint8_t mode, uint8_t party, int8_t sp, uint8_t da, uint8_t pf, uint8_t pft, int8_t temp,
                        uint8_t door, uint8_t dst = 0x82) {
  const uint8_t p[] = {0x0C, 0x00, 0x20, mode, 0x08, 0x02, 0x20, party, 0x0C, 0x06, 0x40, 0x00, (uint8_t)sp,
                       0x0C, 0x09, 0x20, da, 0x0C, 0x11, 0x20, pf, 0x0C, 0x12, 0x40, 0x00, pft,
                       0x0C, 0x14, 0x40, 0x00, (uint8_t)temp, 0x0C, 0x31, 0x20, door};
  uint8_t f[lb::MAX_FRAME];
  size_t n = reply13(dst, p, sizeof p, f);
  b.frame(f, n);
}

void test_value_decoding() {
  B->to_running();
  feed_status(*B, 4, 1, -17, 1, 1, (uint8_t)-55, -20, 1);
  const State& s = B->e.state();
  TEST_ASSERT_TRUE(s.is_superfrost());
  TEST_ASSERT_EQUAL(4, s.mode.v);
  TEST_ASSERT_EQUAL(1, s.party.v);
  TEST_ASSERT_EQUAL(-17, s.setpoint.v);
  TEST_ASSERT_EQUAL(-20, s.temperature.v);
  TEST_ASSERT_EQUAL(1, s.door_open.v);
  TEST_ASSERT_EQUAL(1, s.door_alarm.v);
  TEST_ASSERT_EQUAL(1, s.power_fail_alarm.v);
  TEST_ASSERT_EQUAL(-55, s.power_fail_temp.v);
  feed_status(*B, 1, 0, -18, 0, 0, (uint8_t)-55, -18, 0);
  TEST_ASSERT_FALSE(s.is_superfrost());
  TEST_ASSERT_EQUAL(0, s.party.v);
  TEST_ASSERT_EQUAL(0, s.door_open.v);
  TEST_ASSERT_EQUAL(0, s.power_fail_alarm.v);
}

void test_flag_00_marks_unknown_not_zero() {
  B->to_running();
  feed_status(*B, 1, 0, -18, 0, 0, (uint8_t)-55, -18, 0);
  TEST_ASSERT_TRUE(B->e.state().power_fail_temp.known);
  const uint8_t p[] = {0x0C, 0x12, 0x00, 0x0C, 0x11, 0x20, 0x01};  // 0C/12 flag 00 (not available), 0C/11 = 1
  uint8_t f[lb::MAX_FRAME];
  size_t n = reply13(0x82, p, sizeof p, f);
  uint32_t c0 = B->e.change_counter();
  B->frame(f, n);
  TEST_ASSERT_FALSE(B->e.state().power_fail_temp.known);
  TEST_ASSERT_EQUAL(0, B->e.state().power_fail_temp.v);
  TEST_ASSERT_TRUE(B->e.state().power_fail_alarm.known);
  TEST_ASSERT_EQUAL(1, B->e.state().power_fail_alarm.v);
  TEST_ASSERT_TRUE(B->e.change_counter() != c0);
}

void test_periodic_read_every_10s_and_event_reread() {
  B->to_running();
  auto reads = [&](int slots) {  // count status reads over `slots` slot-02 turns of 570 ms
    int k = 0;
    for (int i = 0; i < slots; i++) {
      B->advance(570);
      B->slot02();
      if (B->last_n > 9 && B->last[2] == 0x03) k++;
      feed_status(*B, 1, 0, -18, 0, 0, (uint8_t)-55, -18, 0);  // freezer answers
    }
    return k;
  };
  // drain whatever start-up left, then measure 30 s
  reads(4);
  int n = reads(53);  // ~30 s
  TEST_ASSERT_TRUE(n >= 2 && n <= 4);
  // an event block with a zero flag word does not trigger a read
  B->frame("FF 80 7F 24 01 20 00 00 80 00 00 00 00 00 01 80 00 00 00 00 00 02 80 00 00 00 00 00 03 80 00 00 00 00 00 04 20 20 18 7F");
  B->advance(570);
  B->slot02();
  TEST_ASSERT_TRUE(B->last_n == 9);  // idle
  // a non-zero one does, at the next slot 02
  B->frame("FF 80 7F 24 01 20 00 00 80 00 00 01 00 00 01 80 08 00 00 00 00 02 80 00 00 00 00 00 03 80 00 00 00 00 00 04 20 10 8E 02");
  B->advance(570);
  B->slot02();
  TEST_ASSERT_TRUE(B->last_n > 9 && B->last[2] == 0x03 && B->last[4] == 0x18 && B->last[5] == 0x0C);
}

void test_passive_81_reply_updates_door_alarm() {  // captured freezer reply to node 01 (door_alarm_on, 2.56 s)
  B->to_running();
  feed_status(*B, 1, 0, -18, 0, 0, (uint8_t)-55, -18, 1);
  TEST_ASSERT_EQUAL(0, B->e.state().door_alarm.v);
  B->frame("81 81 7F 13 01 4E 0C 4C 20 00 0C 47 20 00 0C 09 20 01 0C 11 20 00 0C 0D 20 00 0C 0F 20 00 0C 46 20 00 0C 4B 20 00 0C 12 40 00 C9 0C 2E 40 00 BD 0C 0C 40 00 C9 0C 2F 40 00 BD 0C 0E 40 00 00 0C 30 40 00 00 0C 45 20 00 0C 58 20 00 0C 49 20 00 0C 0B 20 00 3C 3B");
  TEST_ASSERT_EQUAL(1, B->e.state().door_alarm.v);
  TEST_ASSERT_EQUAL(0, B->e.state().power_fail_alarm.v);
  TEST_ASSERT_EQUAL(-55, B->e.state().power_fail_temp.v);
  // the freezer's reply to the box in slot 82 (2.95 s): door open, alarm on, mode normal, temperature -18
  B->frame("82 82 7F 13 01 59 0C 15 20 00 0C 80 20 01 0C 32 20 00 0C 00 20 01 0C 06 40 00 EE 0C 07 40 9A FF 0C 14 40 30 EE 0C 2B 40 00 00 0C 31 20 01 0C 0B 20 00 0C 09 20 01 0C 0D 20 00 0C 0C 40 00 C9 0C 2F 40 00 BD 0C AB 20 1E 0C A8 40 00 08 0C 11 20 00 0C 12 40 00 C9 0C 2E 40 00 BD 0C 34 20 00 63 6D");
  TEST_ASSERT_EQUAL(1, B->e.state().door_open.v);
  TEST_ASSERT_EQUAL(-18, B->e.state().setpoint.v);
  TEST_ASSERT_EQUAL(-18, B->e.state().temperature.v);
}

void test_node0b_door_push() {  // captured node 0B broadcasts (door_short_open / door_alarm_off)
  B->to_running();
  B->frame("0B 80 0B 24 01 04 14 04 20 01 DA 11");
  TEST_ASSERT_TRUE(B->e.state().door_open.known);
  TEST_ASSERT_EQUAL(1, B->e.state().door_open.v);
  B->frame("0B 80 0B 24 01 04 14 04 20 00 FB 01");
  TEST_ASSERT_EQUAL(0, B->e.state().door_open.v);
}

void test_crc_errors_counted_and_ignored() {
  B->to_running();
  B->frame("0B 80 0B 24 01 04 14 04 20 01 DA 12");  // bad CRC
  TEST_ASSERT_EQUAL_UINT32(1, B->e.counters().crc_errors);
  TEST_ASSERT_FALSE(B->e.state().door_open.known);
}

// ---- writes ---------------------------------------------------------------------------------------------------
static void send_and_expect(Bus& b, const char* want) {
  for (int i = 0; i < 6; i++) {
    b.advance(570);
    b.slot02();
    if (b.last_n != 9) { b.expect_sent(want); return; }
  }
  TEST_FAIL_MESSAGE("write did not go out");
}

// Answer the read-back (and everything) so the confirmation can finish before the next write.
static void settle(Bus& b, uint8_t mode, uint8_t party, int8_t sp) {
  for (int i = 0; i < 8; i++) {
    b.advance(570);
    b.slot02();
    feed_status(b, mode, party, sp, 0, 0, (uint8_t)-55, -18, 0);
  }
}

void test_write_bodies_byte_identical() {
  B->to_running();
  settle(*B, 1, 0, -18);
  TEST_ASSERT_EQUAL((int)Result::Accepted, (int)B->e.request_superfrost(true));
  send_and_expect(*B, "FF 02 24 01 04 0C 00 20 04 5D 69");
  settle(*B, 4, 0, -18);
  TEST_ASSERT_EQUAL((int)Result::Accepted, (int)B->e.request_superfrost(false));
  send_and_expect(*B, "FF 02 24 01 04 0C 00 20 01 F8 39");
  settle(*B, 1, 0, -18);
  TEST_ASSERT_EQUAL((int)Result::Accepted, (int)B->e.request_party(true));
  send_and_expect(*B, "FF 02 24 01 04 08 02 20 02 0A AD");
  settle(*B, 1, 1, -18);
  TEST_ASSERT_EQUAL((int)Result::Accepted, (int)B->e.request_party(false));
  send_and_expect(*B, "FF 02 24 01 04 08 02 20 00 48 8D");
  settle(*B, 1, 0, -18);
  TEST_ASSERT_EQUAL((int)Result::Accepted, (int)B->e.request_setpoint(-17));
  send_and_expect(*B, "FF 02 24 01 05 0C 06 40 00 EF D8 99");
  settle(*B, 1, 0, -17);
  TEST_ASSERT_EQUAL((int)Result::Accepted, (int)B->e.request_setpoint(-16));
  send_and_expect(*B, "FF 02 24 01 05 0C 06 40 00 F0 06 7A");
  settle(*B, 1, 0, -16);
  TEST_ASSERT_EQUAL((int)Result::Accepted, (int)B->e.request_setpoint(-28));
  send_and_expect(*B, "FF 02 24 01 05 0C 06 40 00 E4 B3 28");
  settle(*B, 1, 0, -28);
  TEST_ASSERT_EQUAL((int)Result::Accepted, (int)B->e.request_setpoint(-15));
  send_and_expect(*B, "FF 02 24 01 05 0C 06 40 00 F1 27 6A");
}

void test_refusals() {
  // not running (before / during the handshake)
  TEST_ASSERT_EQUAL((int)Result::RefusedNotRunning, (int)B->e.request_superfrost(true));
  B->slot02();
  TEST_ASSERT_EQUAL((int)Result::RefusedNotRunning, (int)B->e.request_setpoint(-17));
  B->handshake();
  TEST_ASSERT_EQUAL((int)Result::RefusedNotRunning, (int)B->e.request_party(true));
  TEST_ASSERT_TRUE(B->run_until(Phase::Running));
  // range, before any other reason
  TEST_ASSERT_EQUAL((int)Result::RefusedRange, (int)B->e.request_setpoint(-30));
  TEST_ASSERT_EQUAL((int)Result::RefusedRange, (int)B->e.request_setpoint(-29));
  TEST_ASSERT_EQUAL((int)Result::RefusedRange, (int)B->e.request_setpoint(-14));
  TEST_ASSERT_EQUAL((int)Result::RefusedRange, (int)B->e.request_setpoint(0));
  uint32_t sent = B->e.counters().writes_sent;
  for (int i = 0; i < 4; i++) { B->advance(570); B->slot02(); }
  TEST_ASSERT_EQUAL_UINT32(sent, B->e.counters().writes_sent);  // refused range: nothing went out
  // tx off
  B->e.set_tx_enabled(false);
  TEST_ASSERT_EQUAL((int)Result::RefusedTxOff, (int)B->e.request_superfrost(true));
  TEST_ASSERT_EQUAL((int)Result::RefusedTxOff, (int)B->e.request_setpoint(-30 + 13));
  B->e.set_tx_enabled(true);
  // bus down: no slot 02 for more than 2 s
  B->advance(2600);
  TEST_ASSERT_FALSE(B->e.bus_alive());
  TEST_ASSERT_EQUAL((int)Result::RefusedBusDown, (int)B->e.request_superfrost(true));
  B->slot02();
  TEST_ASSERT_TRUE(B->e.bus_alive());
  TEST_ASSERT_EQUAL((int)Result::Accepted, (int)B->e.request_superfrost(true));
}

void test_queue_full_and_order() {
  B->to_running();
  for (int i = 0; i < 4; i++) TEST_ASSERT_EQUAL((int)Result::Accepted, (int)B->e.request_setpoint(-20 + i));
  TEST_ASSERT_EQUAL((int)Result::RefusedQueueFull, (int)B->e.request_setpoint(-16));
}

void test_write_queues_behind_handshake_and_dropped_on_tx_off() {
  B->to_running();
  B->e.request_superfrost(true);
  B->e.set_tx_enabled(false);
  WriteResult r;
  TEST_ASSERT_TRUE(B->e.take_write_result(r));
  TEST_ASSERT_EQUAL((int)WriteOutcome::NotSent, (int)r.outcome);
  TEST_ASSERT_EQUAL((int)WriteParam::SuperFrost, (int)r.param);
  B->e.set_tx_enabled(true);
  B->advance(570); B->slot02();
  TEST_ASSERT_EQUAL_UINT32(0, B->e.counters().writes_sent);
}

void test_write_missed_slot_stays_queued() {
  B->to_running();
  settle(*B, 1, 0, -18);
  B->e.request_superfrost(true);
  B->advance(570); B->slot02(false);
  B->advance(570); B->slot02(false);
  B->advance(570); B->slot02(true);
  B->expect_sent("FF 02 24 01 04 0C 00 20 04 5D 69");
  TEST_ASSERT_EQUAL_UINT32(1, B->e.counters().writes_sent);
}

// ---- confirmation -----------------------------------------------------------------------------------------
// Event block with a non-zero flag word, as the freezer raises after a write.
static const char* EV_CHANGED = "FF 80 7F 24 01 20 00 00 80 01 00 00 00 00 01 80 08 00 00 00 00 02 80 00 00 00 00 00 03 80 00 00 00 00 00 04 20 10 0E E7";

void test_write_confirmed_after_event_and_readback() {
  B->to_running();
  settle(*B, 1, 0, -18);
  TEST_ASSERT_EQUAL((int)Result::Accepted, (int)B->e.request_setpoint(-17));
  send_and_expect(*B, "FF 02 24 01 05 0C 06 40 00 EF D8 99");
  B->advance(300);
  B->frame(EV_CHANGED);
  B->advance(270); B->slot02();  // read-back request
  TEST_ASSERT_TRUE(B->last_n > 9 && B->last[2] == 0x03);
  B->advance(285);
  feed_status(*B, 1, 0, -17, 0, 0, (uint8_t)-55, -18, 0);
  WriteResult r;
  TEST_ASSERT_TRUE(B->e.take_write_result(r));
  TEST_ASSERT_EQUAL((int)WriteOutcome::Confirmed, (int)r.outcome);
  TEST_ASSERT_EQUAL((int)WriteParam::Setpoint, (int)r.param);
  TEST_ASSERT_EQUAL(-17, r.value);
  TEST_ASSERT_EQUAL(-17, B->e.state().setpoint.v);
  TEST_ASSERT_EQUAL_UINT32(1, B->e.counters().writes_confirmed);
}

void test_write_unconfirmed_on_mismatch_and_not_resent() {
  B->to_running();
  settle(*B, 1, 0, -18);
  B->e.request_setpoint(-17);
  send_and_expect(*B, "FF 02 24 01 05 0C 06 40 00 EF D8 99");
  B->advance(300);
  B->frame(EV_CHANGED);
  B->advance(270); B->slot02();
  B->advance(285);
  feed_status(*B, 1, 0, -18, 0, 0, (uint8_t)-55, -18, 0);  // freezer kept -18
  WriteResult r;
  TEST_ASSERT_TRUE(B->e.take_write_result(r));
  TEST_ASSERT_EQUAL((int)WriteOutcome::Unconfirmed, (int)r.outcome);
  TEST_ASSERT_EQUAL(-18, B->e.state().setpoint.v);
  for (int i = 0; i < 12; i++) { B->advance(570); B->slot02(); TEST_ASSERT_TRUE(B->last_n == 9 || B->last[2] != 0x24 || B->last[4] != 0x01 || B->last[5] != 0x05); }
  TEST_ASSERT_EQUAL_UINT32(1, B->e.counters().writes_sent);  // never resent
}

void test_write_unconfirmed_without_readback_in_3s() {
  B->to_running();
  settle(*B, 1, 0, -18);
  B->e.request_party(true);
  send_and_expect(*B, "FF 02 24 01 04 08 02 20 02 0A AD");
  WriteResult r;
  B->advance(2900);
  TEST_ASSERT_FALSE(B->e.take_write_result(r));
  B->advance(200);
  TEST_ASSERT_TRUE(B->e.take_write_result(r));
  TEST_ASSERT_EQUAL((int)WriteOutcome::Unconfirmed, (int)r.outcome);
  TEST_ASSERT_EQUAL_UINT32(1, B->e.counters().writes_unconfirmed);
}

void test_hello_restart_clears_queue_and_values() {
  B->to_running();
  feed_status(*B, 4, 1, -17, 1, 1, (uint8_t)-55, -20, 1);
  B->e.request_superfrost(false);
  B->e.request_setpoint(-16);
  B->frame("82 82 7F 01 01 00 C9 F4");
  TEST_ASSERT_EQUAL((int)Phase::Handshake, (int)B->e.phase());
  WriteResult r;
  TEST_ASSERT_TRUE(B->e.take_write_result(r));
  TEST_ASSERT_EQUAL((int)WriteOutcome::NotSent, (int)r.outcome);
  TEST_ASSERT_TRUE(B->e.take_write_result(r));
  TEST_ASSERT_FALSE(B->e.state().temperature.known);
  TEST_ASSERT_FALSE(B->e.state().door_open.known);
  B->slot02();
  B->expect_sent(HELLO_REPLY);
  TEST_ASSERT_EQUAL_UINT32(0, B->e.counters().writes_sent);
  // and it runs the whole start-up again
  B->frame("82 82 7F 21 01 00 0F 72"); B->slot02();
  B->frame("82 82 7F 22 01 00 5F 2B"); B->slot02(); B->slot02();
  B->frame("82 82 7F 23 01 00 6F 1C"); B->slot02();
  TEST_ASSERT_TRUE(B->run_until(Phase::Running));
}

void test_bus_alive_tracking() {
  TEST_ASSERT_FALSE(B->e.bus_alive());
  B->slot02();
  TEST_ASSERT_TRUE(B->e.bus_alive());
  B->advance(1900);
  TEST_ASSERT_TRUE(B->e.bus_alive());
  B->advance(200);
  TEST_ASSERT_FALSE(B->e.bus_alive());
  B->frame("7F");  // other slots do not count
  TEST_ASSERT_FALSE(B->e.bus_alive());
}

// ---- rejoin without a hello (the C3 restarted while the freezer kept running) ---------------------------------
// The freezer's 1 s event block as the emulator sends it (taken from the C3's log on the testbench).
static const char* EVENT_BLOCK = "FF 80 7F 24 01 20 00 00 80 00 00 00 00 00 01 80 00 00 00 00 00 02 80 00 00 00 00 00 "
                                 "03 80 00 00 00 00 00 04 20 F0 65 B4";

// A running freezer that sends no hello: an event block and a slot 02 every 570 ms, for `ms`.
static void freezer_without_hello(Bus& b, uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += 570) {
    b.advance(570);
    b.frame(EVENT_BLOCK);
    b.slot02();
  }
}

void test_rejoin_after_30s_of_freezer_traffic_without_hello() {
  freezer_without_hello(*B, 29000);
  TEST_ASSERT_EQUAL((int)Phase::Waiting, (int)B->e.phase());
  B->expect_sent(IDLE);
  freezer_without_hello(*B, 1500);
  TEST_ASSERT_EQUAL((int)Phase::Startup, (int)B->e.phase());
  TEST_ASSERT_EQUAL_UINT32(1, B->e.counters().rejoins);
  TEST_ASSERT_EQUAL_UINT32(0, B->e.counters().hellos);
  B->expect_sent("FF 02 06 01 00 56 36");  // the first start-up read (freezer version), no hello reply
}

void test_no_rejoin_without_freezer_traffic_or_with_tx_off() {
  for (int i = 0; i < 120; i++) { B->advance(570); B->slot02(); }  // bus alive, but the freezer never speaks
  TEST_ASSERT_EQUAL((int)Phase::Waiting, (int)B->e.phase());
  B->e.set_tx_enabled(false);
  freezer_without_hello(*B, 60000);
  TEST_ASSERT_EQUAL((int)Phase::Waiting, (int)B->e.phase());
  TEST_ASSERT_EQUAL_UINT32(0, B->e.counters().rejoins);
}

void test_unanswered_rejoin_returns_to_waiting_and_retries() {
  freezer_without_hello(*B, 31000);
  TEST_ASSERT_EQUAL((int)Phase::Startup, (int)B->e.phase());
  freezer_without_hello(*B, 15000);  // every start-up read times out
  TEST_ASSERT_EQUAL((int)Phase::Waiting, (int)B->e.phase());
  TEST_ASSERT_FALSE(B->e.state().temperature.known);
  freezer_without_hello(*B, 20000);
  TEST_ASSERT_EQUAL((int)Phase::Waiting, (int)B->e.phase());
  TEST_ASSERT_EQUAL_UINT32(1, B->e.counters().rejoins);
  freezer_without_hello(*B, 15000);  // 30 s after the failure: the next try
  TEST_ASSERT_EQUAL_UINT32(2, B->e.counters().rejoins);
}

void test_hello_during_rejoin_runs_the_full_handshake() {
  freezer_without_hello(*B, 31000);
  TEST_ASSERT_EQUAL((int)Phase::Startup, (int)B->e.phase());
  B->frame("82 82 7F 01 01 00 C9 F4");
  TEST_ASSERT_EQUAL((int)Phase::Handshake, (int)B->e.phase());
  B->slot02();
  B->expect_sent(HELLO_REPLY);
}

// ---- group 48 ---------------------------------------------------------------------------------------------
static void set_net(Bus& b, bool conn, uint8_t d, const char* ssid) {
  const uint8_t ip[4] = {192, 168, 1, d};
  const uint8_t mac[6] = {0x00, 0x1D, 0x63, 0x12, 0x34, 0x56};
  b.e.set_network(conn, ip, ssid, mac);
}

struct Net { lb::Msg m[16]; int n = 0; };
// Run slots until Running + `slots` more; collect every group-48 write.
static void collect48(Bus& b, Net& net, int slots, uint32_t step_ms = 570) {
  for (int i = 0; i < slots; i++) {
    b.advance(step_ms);
    b.slot02();
    if (b.last_n > 9) {
      uint8_t f[lb::MAX_FRAME];
      f[0] = 2; memcpy(f + 1, b.last, b.last_n);
      lb::Msg m;
      if (lb::decode(f, b.last_n + 1, m) == lb::Status::Ok && m.svc == 0x24 && m.nrecs && m.recs[0].grp == 0x48 && net.n < 16) net.m[net.n++] = m;
    }
  }
}

static const lb::Rec* rec(const lb::Msg& m, uint8_t id) {
  for (int i = 0; i < m.nrecs; i++) if (m.recs[i].id == id) return &m.recs[i];
  return nullptr;
}

void test_network_writes_sequence() {
  set_net(*B, true, 100, "Example_Wifi");
  B->handshake();
  Net net;
  collect48(*B, net, 60);
  // 48/00=03, text+48/14, MAC+48/03, 48/00=04, (5 s) 48/00=05
  TEST_ASSERT_EQUAL(5, net.n);
  expect_hex("FF 02 24 01 04 48 00 20 03 D7 BD", net.m[0].raw + 1, net.m[0].raw_len - 1);
  char text[40] = {0};
  for (uint8_t id = 4; id <= 0x0B; id++) {
    const lb::Rec* r = rec(net.m[1], id);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_HEX8(0x80, r->flag);
    memcpy(text + 4 * (id - 4), r->value, 4);
  }
  TEST_ASSERT_EQUAL_STRING("192.168.1.100@Example_Wifi", text);
  const lb::Rec* ip = rec(net.m[1], 0x14);
  TEST_ASSERT_NOT_NULL(ip);
  const uint8_t ipw[4] = {0x64, 0x01, 0xA8, 0xC0};  // 192.168.1.100 little endian, as captured
  TEST_ASSERT_EQUAL_HEX8_ARRAY(ipw, ip->value, 4);
  // chunking exactly as the capture: "192." "168." "1.10" "0@Ex" "ampl" "e_Wi" "fi\0\0" "\0\0\0\0"
  TEST_ASSERT_EQUAL_HEX8_ARRAY("1.10", rec(net.m[1], 6)->value, 4);
  TEST_ASSERT_EQUAL_HEX8_ARRAY("fi\0\0", rec(net.m[1], 0x0A)->value, 4);
  // MAC split as the captured box: 48/0C = 00 1D 63 12, 48/0D = 34 56 00 00 (flag 80), then 48/03 = 01
  const uint8_t m0[4] = {0x00, 0x1D, 0x63, 0x12}, m1[4] = {0x34, 0x56, 0x00, 0x00};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(m0, rec(net.m[2], 0x0C)->value, 4);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(m1, rec(net.m[2], 0x0D)->value, 4);
  TEST_ASSERT_EQUAL_HEX8(0x01, rec(net.m[2], 0x03)->value[0]);
  expect_hex("FF 02 24 01 04 48 00 20 04 30 CD", net.m[3].raw + 1, net.m[3].raw_len - 1);
  expect_hex("FF 02 24 01 04 48 00 20 05 11 DD", net.m[4].raw + 1, net.m[4].raw_len - 1);
}

void test_network_05_is_5s_after_04() {
  set_net(*B, true, 100, "x");
  B->handshake();
  Net net;
  // find 04, then the time to 05
  uint64_t t4 = 0, t5 = 0;
  for (int i = 0; i < 400 && !t5; i++) {
    B->advance(100);
    B->slot02();
    if (B->last_n == 11 && B->last[2] == 0x24 && B->last[5] == 0x48) {
      if (B->last[8] == 0x04) t4 = B->t;
      if (B->last[8] == 0x05) t5 = B->t;
    }
  }
  TEST_ASSERT_TRUE(t4 && t5);
  TEST_ASSERT_TRUE(t5 - t4 >= 5000000u && t5 - t4 < 5300000u);
}

void test_network_waits_for_wifi_and_rewrites_on_ip_change() {
  set_net(*B, false, 100, "Example_Wifi");
  B->handshake();
  Net net;
  collect48(*B, net, 30);
  TEST_ASSERT_EQUAL(0, net.n);  // no Wi-Fi: no 48 write at all
  set_net(*B, true, 100, "Example_Wifi");
  collect48(*B, net, 40);
  TEST_ASSERT_EQUAL(5, net.n);
  set_net(*B, true, 101, "Example_Wifi");
  Net again;
  collect48(*B, again, 10);
  TEST_ASSERT_EQUAL(1, again.n);  // text + 48/14 only
  char text[40] = {0};
  for (uint8_t id = 4; id <= 0x0B; id++) memcpy(text + 4 * (id - 4), rec(again.m[0], id)->value, 4);
  TEST_ASSERT_EQUAL_STRING("192.168.1.101@Example_Wifi", text);
  const uint8_t ipw[4] = {0x65, 0x01, 0xA8, 0xC0};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(ipw, rec(again.m[0], 0x14)->value, 4);
  Net quiet;
  collect48(*B, quiet, 20);
  TEST_ASSERT_EQUAL(0, quiet.n);
}

void test_network_long_ssid_is_cut_to_32() {
  set_net(*B, true, 100, "A_very_long_network_name_of_32_chars_plus_more");
  B->handshake();
  Net net;
  collect48(*B, net, 30);
  char text[40] = {0};
  for (uint8_t id = 4; id <= 0x0B; id++) memcpy(text + 4 * (id - 4), rec(net.m[1], id)->value, 4);
  TEST_ASSERT_EQUAL_UINT32(32, (uint32_t)strlen(text));
}

void test_network_writes_disabled() {
  set_net(*B, true, 100, "Example_Wifi");
  B->e.set_network_writes_enabled(false);
  B->handshake();
  Net net;
  collect48(*B, net, 60);
  TEST_ASSERT_EQUAL(0, net.n);
  // and never a 4C write either
  TEST_ASSERT_EQUAL_UINT32(0, B->e.counters().net_writes);
}

void test_message_sizes_within_box_limit() {
  set_net(*B, true, 100, "A_very_long_network_name_of_32_chars_plus_more");
  B->handshake();
  for (int i = 0; i < 60; i++) {
    B->advance(570);
    B->slot02();
    TEST_ASSERT_TRUE(B->last_n <= 88);
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_idle_ack_when_nothing_queued);
  RUN_TEST(test_hello_gets_reply_and_restarts);
  RUN_TEST(test_not_sent_message_stays_queued);
  RUN_TEST(test_answer_is_idempotent_until_result);
  RUN_TEST(test_handshake_order_and_priority);
  RUN_TEST(test_tx_disabled_sends_nothing);
  RUN_TEST(test_startup_versions_ident_status_in_order);
  RUN_TEST(test_node01_absent_does_not_block_and_is_unknown);
  RUN_TEST(test_status_request_uses_captured_flags);
  RUN_TEST(test_value_decoding);
  RUN_TEST(test_flag_00_marks_unknown_not_zero);
  RUN_TEST(test_periodic_read_every_10s_and_event_reread);
  RUN_TEST(test_passive_81_reply_updates_door_alarm);
  RUN_TEST(test_node0b_door_push);
  RUN_TEST(test_crc_errors_counted_and_ignored);
  RUN_TEST(test_write_bodies_byte_identical);
  RUN_TEST(test_refusals);
  RUN_TEST(test_queue_full_and_order);
  RUN_TEST(test_write_queues_behind_handshake_and_dropped_on_tx_off);
  RUN_TEST(test_write_missed_slot_stays_queued);
  RUN_TEST(test_write_confirmed_after_event_and_readback);
  RUN_TEST(test_write_unconfirmed_on_mismatch_and_not_resent);
  RUN_TEST(test_write_unconfirmed_without_readback_in_3s);
  RUN_TEST(test_hello_restart_clears_queue_and_values);
  RUN_TEST(test_bus_alive_tracking);
  RUN_TEST(test_rejoin_after_30s_of_freezer_traffic_without_hello);
  RUN_TEST(test_no_rejoin_without_freezer_traffic_or_with_tx_off);
  RUN_TEST(test_unanswered_rejoin_returns_to_waiting_and_retries);
  RUN_TEST(test_hello_during_rejoin_runs_the_full_handshake);
  RUN_TEST(test_network_writes_sequence);
  RUN_TEST(test_network_05_is_5s_after_04);
  RUN_TEST(test_network_waits_for_wifi_and_rewrites_on_ip_change);
  RUN_TEST(test_network_long_ssid_is_cut_to_32);
  RUN_TEST(test_network_writes_disabled);
  RUN_TEST(test_message_sizes_within_box_limit);
  return UNITY_END();
}
