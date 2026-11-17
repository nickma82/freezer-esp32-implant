// Closed loop: the simulated freezer (fm::Master, captured answers) on one side, the node-02 engine on the other.
// A fake clock steps the master's schedule in its 17.8 ms grid; every slot goes to the engine, and the engine's answer for
// slot 02 goes back into the master (which replies one 285 ms step later in slot 82).
#include "../test_support.h"
#include "freezer_master.h"

using namespace n02;

struct Loop {
  fm::Master m;
  Engine e;
  uint64_t t_us = 0;
  uint32_t slots02_sent = 0;
  bool silent = false;  // true: nothing reaches the engine (bus cable pulled)

  void step() {
    fm::Slot s;
    m.next((uint32_t)(t_us / 1000), s);
    uint8_t f[lb::MAX_FRAME];
    if (s.slot == 0x02) {
      uint8_t a[MAX_ANSWER];
      size_t n = silent ? 0 : e.next_answer(a, sizeof a, t_us);
      f[0] = 0x02;
      if (n) memcpy(f + 1, a, n);
      if (!silent) {
        e.on_frame(f, n + 1, t_us);  // our own echo, or the empty slot
        e.on_slot02_result(n > 0);
      }
      if (n) { m.on_box(a, n); slots02_sent++; }
    } else if (!silent) {
      f[0] = s.slot;
      if (s.len) memcpy(f + 1, s.body, s.len);
      e.on_frame(f, (size_t)s.len + 1, t_us);
    }
    t_us += (uint64_t)s.ticks * fm::TICK_US;
    e.tick(t_us);
  }
  void run_ms(uint32_t ms) { uint64_t end = t_us + (uint64_t)ms * 1000; while (t_us < end) step(); }
  template <class P> bool run_until(P pred, uint32_t max_ms) {
    uint64_t end = t_us + (uint64_t)max_ms * 1000;
    while (t_us < end) { if (pred()) return true; step(); }
    return pred();
  }
  // fm::Master::restart_handshake() models a silent box: any box message flips it back to Running before the hello
  // goes out. A real freezer sends the hello regardless, so restart right after our slot 02, i.e. just before the
  // next reply slot 82 that carries the hello.
  void restart_master() {
    uint32_t n = slots02_sent;
    while (slots02_sent == n) step();
    m.restart_handshake();
  }
  bool running() { return e.phase() == Phase::Running; }
  // The C3 reboots: a fresh engine (Wi-Fi up again), the master keeps running.
  void restart_c3() { e = Engine(); net(); }
  void net(bool conn = true) {
    const uint8_t ip[4] = {192, 168, 1, 100}, mac[6] = {0x00, 0x1D, 0x63, 0x12, 0x34, 0x56};
    e.set_network(conn, ip, "Example_Wifi", mac);
  }
  // Wait for the next write result; returns its outcome through `out`.
  bool result(WriteResult& out, uint32_t max_ms = 6000) {
    return run_until([&] { return e.take_write_result(out); }, max_ms);
  }
};

static Loop* L;
void setUp() { L = new Loop(); }
void tearDown() { delete L; }

static int32_t param_int8(const fm::Master& m, uint8_t g, uint8_t id, uint8_t f) {
  const fm::Param* p = m.find(g, id, f);
  return p ? (int8_t)(p->value & 0xFF) : 9999;
}

void test_handshake_and_startup_reach_running() {
  L->net();
  TEST_ASSERT_TRUE(L->run_until([] { return L->running(); }, 60000));
  TEST_ASSERT_TRUE(L->m.handshake() == fm::Hs::Running);
  TEST_ASSERT_TRUE(L->e.bus_alive());
  const State& s = L->e.state();
  TEST_ASSERT_EQUAL_STRING("TOD 5-600|SW 1-86-7|BUS 1-4-0", s.freezer_version);
  TEST_ASSERT_EQUAL_STRING("TOD 5-600|SW 6-2-0|BUS 1-4-0", s.node01_version);
  TEST_ASSERT_EQUAL_STRING("FNc    5076", s.model);
  TEST_ASSERT_EQUAL_STRING("20A/001", s.revision);
  TEST_ASSERT_EQUAL_STRING("9935672-01", s.part_number);
  TEST_ASSERT_EQUAL_UINT32(0, L->e.counters().crc_errors);
  TEST_ASSERT_EQUAL_UINT32(1, L->m.stats().hellos);
}

void test_status_equals_captured_params() {
  TEST_ASSERT_TRUE(L->run_until([] { return L->running(); }, 60000));
  const State& s = L->e.state();
  // every value of the model must equal what the master (= captured freezer) answers
  TEST_ASSERT_TRUE(s.mode.known && s.party.known && s.setpoint.known && s.temperature.known && s.door_open.known &&
                   s.door_alarm.known && s.power_fail_alarm.known && s.power_fail_temp.known);
  TEST_ASSERT_EQUAL(L->m.find(0x0C, 0x00, 0x20)->value, s.mode.v);
  TEST_ASSERT_EQUAL(L->m.find(0x08, 0x02, 0x20)->value != 0, s.party.v);
  TEST_ASSERT_EQUAL(param_int8(L->m, 0x0C, 0x06, 0x40), s.setpoint.v);
  TEST_ASSERT_EQUAL(param_int8(L->m, 0x0C, 0x14, 0x40), s.temperature.v);
  TEST_ASSERT_EQUAL(L->m.find(0x0C, 0x31, 0x20)->value != 0, s.door_open.v);
  TEST_ASSERT_EQUAL(L->m.find(0x0C, 0x09, 0x20)->value != 0, s.door_alarm.v);
  TEST_ASSERT_EQUAL(L->m.find(0x0C, 0x11, 0x20)->value != 0, s.power_fail_alarm.v);
  TEST_ASSERT_EQUAL(param_int8(L->m, 0x0C, 0x12, 0x40), s.power_fail_temp.v);
  // the concrete captured numbers
  TEST_ASSERT_EQUAL(1, s.mode.v);
  TEST_ASSERT_EQUAL(-18, s.setpoint.v);
  TEST_ASSERT_EQUAL(-17, s.temperature.v);
  TEST_ASSERT_EQUAL(-55, s.power_fail_temp.v);
}

void test_network_record_reaches_the_master() {
  L->net();
  // 48/00 starts at 05 in the captured table; watch it go 03 -> 04 -> 05 as the box does
  uint32_t seq[8], ns = 0, last = 99;
  for (int i = 0; i < 6000; i++) {  // ~100 s of simulated time
    L->step();
    const fm::Param* p = L->m.find(0x48, 0x00, 0x20);
    if (p && p->value != last && ns < 8) { last = p->value; seq[ns++] = p->value; }
    if (L->e.counters().net_writes >= 5) break;
  }
  TEST_ASSERT_EQUAL_UINT32(4, ns);
  TEST_ASSERT_EQUAL_UINT32(5, seq[0]);
  TEST_ASSERT_EQUAL_UINT32(3, seq[1]);
  TEST_ASSERT_EQUAL_UINT32(4, seq[2]);
  TEST_ASSERT_EQUAL_UINT32(5, seq[3]);
  TEST_ASSERT_TRUE(L->running());
  TEST_ASSERT_EQUAL_HEX32(0x3139322E, L->m.find(0x48, 0x04, 0x80)->value);  // "192."
  TEST_ASSERT_EQUAL_HEX32(0x6401A8C0, L->m.find(0x48, 0x14, 0x80)->value);  // 192.168.1.100 little endian
  TEST_ASSERT_EQUAL_HEX32(0x001D6312, L->m.find(0x48, 0x0C, 0x80)->value);
  TEST_ASSERT_EQUAL_HEX32(0x34560000, L->m.find(0x48, 0x0D, 0x80)->value);
  TEST_ASSERT_EQUAL(1, L->m.find(0x48, 0x03, 0x20)->value);
  // no 4C write ever
  for (size_t i = 0; i < L->m.seen_count(); i++) TEST_ASSERT_TRUE(!(L->m.seen_at(i).kind == 1 && L->m.seen_at(i).a == 0x4C));
}

void test_network_writes_off_writes_nothing() {
  L->net();
  L->e.set_network_writes_enabled(false);
  TEST_ASSERT_TRUE(L->run_until([] { return L->running(); }, 60000));
  L->run_ms(20000);
  for (size_t i = 0; i < L->m.seen_count(); i++) TEST_ASSERT_TRUE(!(L->m.seen_at(i).kind == 1));
}

void test_setpoint_write_changes_master_and_is_confirmed() {
  L->net();
  TEST_ASSERT_TRUE(L->run_until([] { return L->running(); }, 60000));
  TEST_ASSERT_EQUAL((int)Result::Accepted, (int)L->e.request_setpoint(-17));
  WriteResult r;
  TEST_ASSERT_TRUE(L->result(r));
  TEST_ASSERT_EQUAL((int)WriteOutcome::Confirmed, (int)r.outcome);
  TEST_ASSERT_EQUAL(-17, r.value);
  int8_t sp;
  TEST_ASSERT_TRUE(L->m.take_setpoint(sp));
  TEST_ASSERT_EQUAL(-17, sp);
  TEST_ASSERT_EQUAL(-17, param_int8(L->m, 0x0C, 0x06, 0x40));
  TEST_ASSERT_EQUAL(-17, L->e.state().setpoint.v);
  TEST_ASSERT_EQUAL_UINT32(1, L->e.counters().writes_confirmed);
  TEST_ASSERT_EQUAL_UINT32(0, L->e.counters().writes_unconfirmed);
}

void test_out_of_range_setpoint_never_reaches_master() {
  TEST_ASSERT_TRUE(L->run_until([] { return L->running(); }, 60000));
  TEST_ASSERT_EQUAL((int)Result::RefusedRange, (int)L->e.request_setpoint(-30));
  TEST_ASSERT_EQUAL((int)Result::RefusedRange, (int)L->e.request_setpoint(-14));
  L->run_ms(5000);
  int8_t sp;
  TEST_ASSERT_FALSE(L->m.take_setpoint(sp));
  TEST_ASSERT_EQUAL(-18, param_int8(L->m, 0x0C, 0x06, 0x40));
}

void test_superfrost_and_party_roundtrip() {
  TEST_ASSERT_TRUE(L->run_until([] { return L->running(); }, 60000));
  WriteResult r;
  L->e.request_superfrost(true);
  TEST_ASSERT_TRUE(L->result(r));
  TEST_ASSERT_EQUAL((int)WriteOutcome::Confirmed, (int)r.outcome);
  TEST_ASSERT_EQUAL(4, L->m.find(0x0C, 0x00, 0x20)->value);
  TEST_ASSERT_TRUE(L->e.state().is_superfrost());
  L->e.request_superfrost(false);
  TEST_ASSERT_TRUE(L->result(r));
  TEST_ASSERT_EQUAL((int)WriteOutcome::Confirmed, (int)r.outcome);
  TEST_ASSERT_EQUAL(1, L->m.find(0x0C, 0x00, 0x20)->value);
  TEST_ASSERT_FALSE(L->e.state().is_superfrost());
  L->e.request_party(true);
  TEST_ASSERT_TRUE(L->result(r));
  TEST_ASSERT_EQUAL((int)WriteOutcome::Confirmed, (int)r.outcome);
  TEST_ASSERT_EQUAL(2, L->m.find(0x08, 0x02, 0x20)->value);
  TEST_ASSERT_EQUAL(1, L->e.state().party.v);
  L->e.request_party(false);
  TEST_ASSERT_TRUE(L->result(r));
  TEST_ASSERT_EQUAL((int)WriteOutcome::Confirmed, (int)r.outcome);
  TEST_ASSERT_EQUAL(0, L->e.state().party.v);
}

// With the periodic read pushed out of the way, only the event flag can make the engine see a change.
void test_event_triggers_reread_door_alarm() {
  L->e.set_status_period_ms(3600000);
  TEST_ASSERT_TRUE(L->run_until([] { return L->running(); }, 60000));
  L->run_ms(3000);
  L->m.door_alarm(true);
  uint64_t t0 = L->t_us;
  TEST_ASSERT_TRUE(L->run_until([] { return L->e.state().door_alarm.v == 1 && L->e.state().door_open.v == 1; }, 4000));
  TEST_ASSERT_TRUE(L->t_us - t0 < 2500000u);  // spec: within 2 s (+ one slot of simulation granularity)
  L->m.door_alarm(false);
  t0 = L->t_us;
  TEST_ASSERT_TRUE(L->run_until([] { return L->e.state().door_alarm.v == 0 && L->e.state().door_open.v == 0; }, 4000));
  TEST_ASSERT_TRUE(L->t_us - t0 < 2500000u);
}

void test_event_triggers_reread_temperature() {
  L->e.set_status_period_ms(3600000);
  TEST_ASSERT_TRUE(L->run_until([] { return L->running(); }, 60000));
  L->run_ms(3000);
  L->m.set_temperature(-25);
  uint64_t t0 = L->t_us;
  TEST_ASSERT_TRUE(L->run_until([] { return L->e.state().temperature.v == -25; }, 4000));
  TEST_ASSERT_TRUE(L->t_us - t0 < 2500000u);
}

void test_power_failure_alarm() {  // 7.5 of tasks.md: p 0C 11 20 01, p 0C 12 40 00 C9, e 00000100 08000000
  L->e.set_status_period_ms(3600000);
  TEST_ASSERT_TRUE(L->run_until([] { return L->running(); }, 60000));
  L->run_ms(3000);
  L->m.set_value(0x0C, 0x11, 0x20, 1, 1);
  L->m.set_value(0x0C, 0x12, 0x40, 2, 0x00C9);
  L->m.raise_flags(0x00000100, 0x08000000);
  TEST_ASSERT_TRUE(L->run_until([] { return L->e.state().power_fail_alarm.v == 1; }, 4000));
  TEST_ASSERT_EQUAL(-55, L->e.state().power_fail_temp.v);
}

void test_flag_00_from_the_freezer_means_unknown() {
  TEST_ASSERT_TRUE(L->run_until([] { return L->running(); }, 60000));
  TEST_ASSERT_TRUE(L->e.state().power_fail_temp.known);
  const uint8_t p[] = {0x0C, 0x12, 0x00};  // freezer answers 0C/12 with flag 00
  TEST_ASSERT_TRUE(L->m.inject(0x13, p, sizeof p));
  TEST_ASSERT_TRUE(L->run_until([] { return !L->e.state().power_fail_temp.known; }, 3000));
  TEST_ASSERT_EQUAL(0, L->e.state().power_fail_temp.v);
}

// The C3 restarts (OTA, watchdog, power) while the freezer keeps running: no hello comes. After 30 s the engine does
// the start-up reads without the handshake, and the freezer (emulator core) answers them.
void test_c3_restart_rejoins_without_hello() {
  L->net();
  TEST_ASSERT_TRUE(L->run_until([] { return L->running(); }, 60000));
  L->restart_c3();
  L->run_ms(25000);
  TEST_ASSERT_EQUAL((int)Phase::Waiting, (int)L->e.phase());
  TEST_ASSERT_FALSE(L->e.state().temperature.known);
  TEST_ASSERT_TRUE(L->run_until([] { return L->running(); }, 30000));
  TEST_ASSERT_EQUAL_UINT32(1, L->m.stats().hellos);  // the freezer never sent another hello
  TEST_ASSERT_EQUAL_UINT32(0, L->e.counters().hellos);
  TEST_ASSERT_EQUAL_UINT32(1, L->e.counters().rejoins);
  const State& s = L->e.state();
  TEST_ASSERT_EQUAL_STRING("TOD 5-600|SW 1-86-7|BUS 1-4-0", s.freezer_version);
  TEST_ASSERT_EQUAL_STRING("FNc    5076", s.model);
  TEST_ASSERT_TRUE(s.temperature.known && s.setpoint.known);
  TEST_ASSERT_EQUAL(-18, s.setpoint.v);
  WriteResult r;
  TEST_ASSERT_EQUAL((int)Result::Accepted, (int)L->e.request_setpoint(-20));
  TEST_ASSERT_TRUE(L->result(r));
  TEST_ASSERT_EQUAL((int)WriteOutcome::Confirmed, (int)r.outcome);
  TEST_ASSERT_EQUAL(-20, param_int8(L->m, 0x0C, 0x06, 0x40));
}

void test_master_restart_redoes_startup() {
  L->net();
  TEST_ASSERT_TRUE(L->run_until([] { return L->running(); }, 60000));
  TEST_ASSERT_EQUAL_UINT32(1, L->e.counters().hellos);
  uint32_t net0 = L->e.counters().net_writes;
  L->restart_master();
  TEST_ASSERT_TRUE(L->run_until([] { return L->e.counters().hellos == 2; }, 5000));
  TEST_ASSERT_TRUE(L->e.phase() != Phase::Running);
  TEST_ASSERT_FALSE(L->e.state().temperature.known);  // values go unknown until re-read
  TEST_ASSERT_TRUE(L->run_until([] { return L->running(); }, 60000));
  TEST_ASSERT_TRUE(L->m.handshake() == fm::Hs::Running);
  TEST_ASSERT_TRUE(L->e.state().temperature.known);
  TEST_ASSERT_EQUAL_STRING("FNc    5076", L->e.state().model);
  L->run_ms(20000);
  TEST_ASSERT_TRUE(L->e.counters().net_writes >= net0 + 5);  // network writes again
  // and commands work again
  WriteResult r;
  TEST_ASSERT_EQUAL((int)Result::Accepted, (int)L->e.request_superfrost(true));
  TEST_ASSERT_TRUE(L->result(r));
  TEST_ASSERT_EQUAL((int)WriteOutcome::Confirmed, (int)r.outcome);
}

void test_restart_during_queued_write_drops_it() {
  TEST_ASSERT_TRUE(L->run_until([] { return L->running(); }, 60000));
  L->restart_master();                       // the hello comes in the next reply slot, 285 ms from now
  TEST_ASSERT_EQUAL((int)Result::Accepted, (int)L->e.request_setpoint(-16));  // queued, no slot 02 before the hello
  L->run_ms(60000);
  TEST_ASSERT_EQUAL_UINT32(2, L->e.counters().hellos);
  TEST_ASSERT_TRUE(L->running());
  WriteResult r;
  TEST_ASSERT_TRUE(L->e.take_write_result(r));
  TEST_ASSERT_EQUAL((int)WriteOutcome::NotSent, (int)r.outcome);
  TEST_ASSERT_FALSE(L->e.take_write_result(r));
  TEST_ASSERT_EQUAL_UINT32(0, L->e.counters().writes_sent);
  int8_t sp;
  TEST_ASSERT_FALSE(L->m.take_setpoint(sp));  // it never reached the freezer
}

void test_long_outage_then_restart() {  // freezer gone for 5 minutes, then comes back with a hello
  TEST_ASSERT_TRUE(L->run_until([] { return L->running(); }, 60000));
  L->silent = true;
  L->run_ms(300000);
  TEST_ASSERT_FALSE(L->e.bus_alive());
  TEST_ASSERT_EQUAL((int)Result::RefusedBusDown, (int)L->e.request_superfrost(true));
  L->silent = false;
  L->m.restart_handshake();
  TEST_ASSERT_TRUE(L->run_until([] { return L->running() && L->e.bus_alive(); }, 90000));
  TEST_ASSERT_EQUAL_UINT32(2, L->e.counters().hellos);
  TEST_ASSERT_TRUE(L->e.state().temperature.known);
  TEST_ASSERT_EQUAL((int)Result::Accepted, (int)L->e.request_superfrost(true));
}

void test_answers_never_exceed_box_size_and_all_crc_good() {
  L->net();
  TEST_ASSERT_TRUE(L->run_until([] { return L->running(); }, 60000));
  L->run_ms(30000);
  TEST_ASSERT_EQUAL_UINT32(0, L->m.stats().box_bad_crc);
  TEST_ASSERT_EQUAL_UINT32(0, L->m.stats().unknown_params);  // every param we asked for was known to the freezer
  TEST_ASSERT_EQUAL_UINT32(0, L->e.counters().crc_errors);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_handshake_and_startup_reach_running);
  RUN_TEST(test_status_equals_captured_params);
  RUN_TEST(test_network_record_reaches_the_master);
  RUN_TEST(test_network_writes_off_writes_nothing);
  RUN_TEST(test_setpoint_write_changes_master_and_is_confirmed);
  RUN_TEST(test_out_of_range_setpoint_never_reaches_master);
  RUN_TEST(test_superfrost_and_party_roundtrip);
  RUN_TEST(test_event_triggers_reread_door_alarm);
  RUN_TEST(test_event_triggers_reread_temperature);
  RUN_TEST(test_power_failure_alarm);
  RUN_TEST(test_flag_00_from_the_freezer_means_unknown);
  RUN_TEST(test_c3_restart_rejoins_without_hello);
  RUN_TEST(test_master_restart_redoes_startup);
  RUN_TEST(test_restart_during_queued_write_drops_it);
  RUN_TEST(test_long_outage_then_restart);
  RUN_TEST(test_answers_never_exceed_box_size_and_all_crc_good);
  return UNITY_END();
}
