// Freezer emulator PoC: plays node 7F (scheduler + freezer replies) towards a real SmartDeviceBox. Not bench-tested.
// Every slot, ours included, comes back through the UART RX and is logged in the phase-1 format.
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <SoftwareSerial.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "bus_master.h"
#include "freezer_master.h"
#include "freezer_bus.h"

static constexpr uint32_t CONSOLE_BAUD = 115200;
static constexpr uint32_t STATS_MS = 10000;

// ---- console: UART1 TX on GPIO2 (jumpered to the TX header pin), log ring buffer ----
static constexpr size_t LOG_RING = 4096;
static char logbuf[LOG_RING];
static size_t log_head = 0, log_tail = 0;
static uint32_t log_drops = 0;

static size_t log_free() { return (log_tail + LOG_RING - log_head - 1) % LOG_RING; }

static void log_line(const char* s, size_t n) {
  if (n + 1 > log_free()) { log_drops++; return; }
  for (size_t i = 0; i < n; i++) { logbuf[log_head] = s[i]; log_head = (log_head + 1) % LOG_RING; }
  logbuf[log_head] = '\n';
  log_head = (log_head + 1) % LOG_RING;
}

static void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf(const char* fmt, ...) {
  char b[760];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  log_line(b, (size_t)n >= sizeof b ? sizeof b - 1 : (size_t)n);
}

static void console_pump() {
  while (log_tail != log_head) {
    if (Serial1.availableForWrite() <= 0) return;
    Serial1.write((uint8_t)logbuf[log_tail]);
    log_tail = (log_tail + 1) % LOG_RING;
  }
}

static SoftwareSerial con_rx;
static bool full_output = false;

// ---- master state ----
static fm::Master master;
static bool running = false;
static uint32_t due_us = 0;
static uint32_t uptime_us_base, last_stats_ms;

struct Expect { bool valid; uint8_t n; uint8_t data[1 + fm::MAX_BODY]; };
static Expect expect_;

struct Stats { uint32_t slots, box_answers, box_bytes_bad, echo_err, framing; };
static Stats st;
static bus_master::Counters prev;

static const char* hs_name(fm::Hs h) { return h == fm::Hs::WaitBox ? "wait" : h == fm::Hs::Hello ? "hello" : "running"; }

static void send_next_slot() {
  static fm::Slot s;
  master.next(millis(), s);
  expect_.valid = true;
  expect_.data[0] = s.slot;
  memcpy(expect_.data + 1, s.body, s.len);
  expect_.n = (uint8_t)(1 + s.len);
  bus_master::transmit(s.slot, s.body, s.len);
  due_us += s.ticks * fm::TICK_US;
  st.slots++;
}

static void handle_slot(const bus_master::Slot& s) {
  static lb::Msg m;
  static lb::Interp in;
  uint64_t t_us = s.t0_cyc / bus_master::CPU_MHZ;
  if (s.n == 0) return;
  uint8_t slot = s.data[0];

  if (expect_.valid) {  // our own bytes must come back unchanged, anything else means a second driver on the bus
    expect_.valid = false;
    if (running) {
      size_t k = s.n < expect_.n ? s.n : expect_.n;
      bool bad = s.data[0] != expect_.data[0] || s.n < expect_.n;
      for (size_t i = 0; i < k && !bad; i++) bad = s.data[i] != expect_.data[i];
      if (bad && slot != 0x02) {  // slot 02: only the slot byte is ours
        st.echo_err++;
        logf("E %llu echo mismatch slot=%02X sent_n=%u got_n=%u", (unsigned long long)t_us, slot, expect_.n, s.n);
      }
    }
  }

  if (s.flags & (bus_master::F_FRAMING | bus_master::F_OVERRUN | bus_master::F_TRUNC)) {
    st.framing += (s.flags & bus_master::F_FRAMING) ? 1 : 0;
    logf("E %llu damaged slot=%02X flags=%02X n=%u", (unsigned long long)t_us, slot, s.flags, s.n);
    return;
  }
  lb::Status rc = lb::decode(s.data, s.n, m);
  bool empty = s.n == 1;
  int32_t lat = -1;
  if (s.n >= 2) lat = (int32_t)((s.dt_cyc[1] - s.dt_cyc[0]) / bus_master::CPU_MHZ);

  if (running && slot == fm::NODE_BOX && s.n > 1) {  // the box answered in its slot
    st.box_answers++;
    master.on_box(s.data + 1, s.n - 1);
    if (rc != lb::Status::Ok || !m.crc_ok) { st.box_bytes_bad++; logf("E %llu box message bad crc/decode", (unsigned long long)t_us); }
  }
  if (!full_output && empty) return;

  char line[700];
  int n = snprintf(line, sizeof line, "M %llu %02X ", (unsigned long long)t_us, slot);
  if (lat >= 0) n += snprintf(line + n, sizeof line - n, "%ld ", (long)lat);
  else n += snprintf(line + n, sizeof line - n, "- ");
  n += snprintf(line + n, sizeof line - n, "%c", empty ? '-' : (m.crc_ok ? '1' : '0'));
  for (uint16_t i = 1; i < s.n && n < (int)sizeof line - 8; i++) n += snprintf(line + n, sizeof line - n, " %02X", s.data[i]);
  n += snprintf(line + n, sizeof line - n, " | ");
  if (empty) n += snprintf(line + n, sizeof line - n, "empty");
  else if (rc != lb::Status::Ok) n += snprintf(line + n, sizeof line - n, "decode error %u", (unsigned)rc);
  else {
    lb::interpret(m, in);
    if (in.n) n += lb::describe(in, line + n, sizeof line - n);
    else n += snprintf(line + n, sizeof line - n, "kind=%u svc=%02X", (unsigned)m.kind, m.svc);
  }
  log_line(line, n >= (int)sizeof line ? sizeof line - 1 : (size_t)n);
}

static void print_stats() {
  const bus_master::Counters& c = bus_master::counters();
  const fm::Stats& f = master.stats();
  char b[600];
  int n = snprintf(b, sizeof b, "S %lu master=%s hs=%s slots=%lu box_answers=%lu box_bad=%lu echo_err=%lu framing=%lu",
                   (unsigned long)(micros() - uptime_us_base), running ? "on" : "off", hs_name(master.handshake()),
                   (unsigned long)st.slots, (unsigned long)st.box_answers, (unsigned long)st.box_bytes_bad,
                   (unsigned long)st.echo_err, (unsigned long)st.framing);
  n += snprintf(b + n, sizeof b - n, " hellos=%lu replies=%lu events=%lu box_msgs=%lu core_bad_crc=%lu reads=%lu writes=%lu unknown=%lu trunc=%lu queued=%u",
                (unsigned long)f.hellos, (unsigned long)f.replies, (unsigned long)f.events, (unsigned long)f.box_msgs,
                (unsigned long)f.box_bad_crc, (unsigned long)f.reads, (unsigned long)f.writes, (unsigned long)f.unknown_params,
                (unsigned long)f.truncated_reads, (unsigned)master.queued());
  n += snprintf(b + n, sizeof b - n, " overruns=%lu ring_drop=%lu self_break=%lu log_drops=%lu",
                (unsigned long)(c.overrun - prev.overrun), (unsigned long)c.ring_drop, (unsigned long)c.self_break,
                (unsigned long)log_drops);
  log_line(b, n >= (int)sizeof b ? sizeof b - 1 : (size_t)n);
  prev.overrun = c.overrun;
  log_drops = 0;
  memset(&st, 0, sizeof st);
}

static void welcome() {
  logf("# ==============================================");
  logf("# Freezer freezer emulator (node 7F), NOT bench-tested");
  logf("# cpu %u MHz, reset: %s, uptime %lu s", ESP.getCpuFreqMHz(), ESP.getResetReason().c_str(), (unsigned long)(millis() / 1000));
  logf("# bus RX GPIO13/D7, TX stage GPIO15/D8 (MOSFET), console 115200 on GPIO2/D4");
  logf("# master: %s, output: %s", running ? "RUNNING" : "stopped (bus idle)", full_output ? "full" : "filtered (bare slots hidden)");
  logf("# keys: g = go (start master), s = stop, r = restart handshake, v = toggle full output, ? = this message");
  logf("# ==============================================");
}

static void console_keys() {
  while (con_rx.available()) {
    int ch = con_rx.read();
    if (ch == '?' || ch == 'h' || ch == '\r' || ch == '\n') welcome();
    else if (ch == 'v') { full_output = !full_output; logf("# full output %s", full_output ? "on" : "off"); }
    else if (ch == 'g' && !running) {
      running = true;
      bus_master::tx_enable(true);
      due_us = micros() + 50000;
      logf("# master GO");
    } else if (ch == 's' && running) {
      running = false;
      bus_master::tx_enable(false);
      logf("# master STOP");
    } else if (ch == 'r') { master.restart_handshake(); logf("# handshake restart"); }
  }
}

void setup() {
  WiFi.mode(WIFI_OFF);
  WiFi.forceSleepBegin();
  Serial1.begin(CONSOLE_BAUD);
  con_rx.begin(CONSOLE_BAUD, SWSERIAL_8N1, 3, -1);
  con_rx.enableTx(false);
  uptime_us_base = micros();
  welcome();
  console_pump();
  bus_master::begin();  // transmit stays disabled until `g`
  last_stats_ms = millis();
}

void loop() {
  static bus_master::Slot s;
  if (running && (int32_t)(micros() - due_us) >= 0) send_next_slot();
  uint32_t t0 = micros();
  while (bus_master::pop(s)) {
    handle_slot(s);
    if ((uint32_t)(micros() - t0) > 800) break;
  }
  console_keys();
  if (millis() - last_stats_ms >= STATS_MS) { last_stats_ms += STATS_MS; print_stats(); }
  console_pump();
  yield();
}
