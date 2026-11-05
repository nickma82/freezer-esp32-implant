// Freezer emulator PoC: plays node 7F (scheduler + freezer replies) towards a real SmartDeviceBox. Not bench-tested.
// Every slot, ours included, comes back through the UART RX and is logged in the phase-1 format.
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <SoftwareSerial.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bus_master.h"
#include "freezer_master.h"
#include "freezer_bus.h"

static constexpr uint32_t CONSOLE_BAUD = 57600;
static constexpr uint32_t STATS_MS = 10000;

// ---- console: SoftwareSerial TX on GPIO1 (USB chip), RX on GPIO3; log ring buffer. UART1/GPIO2 is the bus TX. ----
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

static SoftwareSerial con;  // bit-banged: blocks ~87 us per byte, so only pump while no slot is due soon
static bool running = false;
static uint32_t due_us = 0;

// The bit-banged console blocks interrupts for every byte it sends (~87 us), and a key arriving then is lost. So print
// little (see Output) and hold all output for a moment after a key, so the rest of a typed line gets through.
static constexpr uint32_t RX_HOLD_MS = 300;
static uint32_t rx_hold_from = 0;
static bool rx_hold = false;

static void console_pump() {
  if (rx_hold && millis() - rx_hold_from < RX_HOLD_MS) return;
  rx_hold = false;
  while (log_tail != log_head) {
    if (running && (int32_t)(due_us - micros()) < 600) return;
    con.write((uint8_t)logbuf[log_tail]);
    log_tail = (log_tail + 1) % LOG_RING;
  }
}
// quiet: only what the box changes or asks beyond its routine (writes, services, flagged event blocks), errors, notes,
// stats. normal: every slot with a body. full: also the bare slots.
enum class Output : uint8_t { Quiet, Normal, Full };
static Output output = Output::Quiet;
static const char* output_name() {
  return output == Output::Quiet ? "quiet (routine reads, replies and idle acks hidden)"
       : output == Output::Normal ? "normal (bare slots hidden)" : "full";
}

// Routine traffic that quiet mode hides: the box's read lists and our replies, its idle ack, event blocks without flags.
static bool routine(const bus_master::Slot& s, const lb::Msg& m) {
  uint8_t slot = s.data[0];
  if (slot == fm::NODE_BOX && m.svc == 0x03) return true;
  if (slot == 0x82 && m.svc == 0x13) return true;
  if (slot == fm::NODE_BOX && m.svc == 0x30 && s.n >= 8 && s.data[6] == 0x00) return true;  // "02 00 00": nothing to send
  if (slot == 0xFF && s.n >= 20) {  // event block: words 00/00 at data[9..12], 00/01 at data[16..19]
    for (int i : {9, 10, 11, 12, 16, 17, 18, 19}) if (s.data[i]) return false;
    return true;
  }
  return false;
}

// ---- master state ----
static fm::Master master;
static uint32_t uptime_us_base, last_stats_ms;

// What we sent, oldest first. A slot only reaches the RX ring when the NEXT break closes it, i.e. after we already sent the
// following slot, so the echo of a slot is compared against the oldest outstanding entry, not the latest.
struct Expect { uint8_t n; uint8_t data[1 + fm::MAX_BODY]; };
static constexpr uint8_t EXPECT_Q = 4;
static Expect expect_q[EXPECT_Q];
static uint8_t expect_head = 0, expect_len = 0;

static Expect& expect_push() {
  if (expect_len == EXPECT_Q) { expect_head = (uint8_t)((expect_head + 1) % EXPECT_Q); expect_len--; }  // drop the oldest
  Expect& e = expect_q[(expect_head + expect_len) % EXPECT_Q];
  expect_len++;
  return e;
}

static bool expect_pop(Expect& out) {
  if (!expect_len) return false;
  out = expect_q[expect_head];
  expect_head = (uint8_t)((expect_head + 1) % EXPECT_Q);
  expect_len--;
  return true;
}

struct Stats { uint32_t slots, box_answers, box_bytes_bad, echo_err, framing; };
static Stats st;
static bus_master::Counters prev;

static const char* hs_name(fm::Hs h) { return h == fm::Hs::WaitBox ? "wait" : h == fm::Hs::Hello ? "hello" : "running"; }

static void send_next_slot() {
  static fm::Slot s;
  master.next(millis(), s);
  Expect& e = expect_push();
  e.data[0] = s.slot;
  memcpy(e.data + 1, s.body, s.len);
  e.n = (uint8_t)(1 + s.len);
  bus_master::transmit(s.slot, s.body, s.len);
  due_us += s.ticks * fm::TICK_US;
  st.slots++;
}

static void hexcat(char* b, size_t cap, int& n, const uint8_t* d, size_t len) {
  for (size_t i = 0; i < len && n < (int)cap - 4; i++) n += snprintf(b + n, cap - n, " %02X", d[i]);
}

static void drain_notes(uint64_t t_us) {
  fm::Note nt;
  char b[160];
  while (master.take_note(nt)) {
    int n = 0;
    switch (nt.kind) {
      case fm::NoteKind::UnknownRead:
        n = snprintf(b, sizeof b, "# UNKNOWN %llu box READ %02X/%02X (answered flag 00 = not available)", (unsigned long long)t_us, nt.grp, nt.id);
        break;
      case fm::NoteKind::UnknownWrite:
        n = snprintf(b, sizeof b, "# UNKNOWN %llu box WRITE %02X/%02X flag+value:", (unsigned long long)t_us, nt.grp, nt.id);
        hexcat(b, sizeof b, n, nt.data, nt.len);
        break;
      case fm::NoteKind::UnknownSvc:
        n = snprintf(b, sizeof b, "# UNKNOWN %llu box SERVICE %02X (no reply) frame:", (unsigned long long)t_us, nt.grp);
        hexcat(b, sizeof b, n, nt.data, nt.len);
        break;
    }
    log_line(b, n >= (int)sizeof b ? sizeof b - 1 : (size_t)n);
  }
}

static void handle_slot(const bus_master::Slot& s) {
  static lb::Msg m;
  static lb::Interp in;
  uint64_t t_us = s.t0_cyc / bus_master::CPU_MHZ;
  if (s.n == 0) return;
  uint8_t slot = s.data[0];

  // Our own bytes must come back unchanged, anything else means a second driver on the bus. A slot lost on the wire
  // (e.g. a glitch while the box is plugged) would shift the queue for good, so re-sync on the slot byte.
  Expect ex;
  bool have = false;
  while (!have && expect_pop(ex)) have = ex.data[0] == slot;
  if (have) {
    size_t k = s.n < ex.n ? s.n : ex.n;
    bool bad = s.data[0] != ex.data[0] || s.n < ex.n;
    for (size_t i = 0; i < k && !bad; i++) bad = s.data[i] != ex.data[i];
    if (bad && slot != 0x02) {  // slot 02: only the slot byte is ours
      st.echo_err++;
      logf("E %llu echo mismatch slot=%02X sent=%02X sent_n=%u got_n=%u", (unsigned long long)t_us, slot, ex.data[0], ex.n, s.n);
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
    int8_t sp;
    drain_notes(t_us);
    if (master.take_setpoint(sp))
      logf("# SETPOINT %llu box wrote target temperature %d degC (current temperature follows, 1 degC every %lu s)",
           (unsigned long long)t_us, (int)sp, (unsigned long)(fm::Master::STEP_MS / 1000));
    if (rc != lb::Status::Ok || !m.crc_ok) { st.box_bytes_bad++; logf("E %llu box message bad crc/decode", (unsigned long long)t_us); }
  }
  if (output != Output::Full && empty) return;
  if (output == Output::Quiet && rc == lb::Status::Ok && routine(s, m)) return;

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
  const rst_info* ri = ESP.getResetInfoPtr();  // the core's crash dump goes to U0TXD (swapped to the unused GPIO15): keep it here
  if (ri->reason == REASON_EXCEPTION_RST || ri->reason == REASON_SOFT_WDT_RST || ri->reason == REASON_WDT_RST)
    logf("# CRASH reason=%u exccause=%u epc1=0x%08x epc2=0x%08x epc3=0x%08x excvaddr=0x%08x depc=0x%08x (decode: just crash <epc1>)",
         (unsigned)ri->reason, (unsigned)ri->exccause, (unsigned)ri->epc1, (unsigned)ri->epc2, (unsigned)ri->epc3,
         (unsigned)ri->excvaddr, (unsigned)ri->depc);
  logf("# bus RX GPIO13/D7, TX GPIO2/D4 (UART1, open-drain, 47 Ohm), console 57600 on the USB TX/RX pins");
  logf("# master: %s, output: %s", running ? "RUNNING" : "stopped (bus idle)", output_name());
  logf("# keys: g = go (start master), s = stop, r = restart handshake, v = output quiet/normal/full, k = list what the box asked, ? = this message");
  logf("# lines (end with Enter): x <svc> [payload]  send service to box | X <dst src svc flag [len] payload>  raw body, CRC added");
  logf("#   a 1|0  door alarm on/off | t <degC>  current temperature | e <w0> <w1>  raise event flags | p <grp id flag value>  set parameter | L <ms>  hold bus low (freezer off)");
  logf("# ==============================================");
}

static void dump_seen() {
  logf("# seen by the box (R = read, W = write, S = service); '*' = not in our table / not handled:");
  for (size_t i = 0; i < master.seen_count(); i++) {
    const fm::Seen& e = master.seen_at(i);
    if (e.kind == 2) logf("#   S %02X        x%lu%s", e.a, (unsigned long)e.count, e.known ? "" : " *");
    else logf("#   %c %02X/%02X    x%lu%s", e.kind ? 'W' : 'R', e.a, e.b, (unsigned long)e.count, e.known ? "" : " *");
  }
  logf("# %u entries", (unsigned)master.seen_count());
}

// hex bytes from `s` (spaces allowed) into out; returns count, -1 on a bad character
static int parse_hex(const char* s, uint8_t* out, size_t cap) {
  size_t n = 0;
  int hi = -1;
  for (; *s; s++) {
    if (*s == ' ' || *s == ',') { if (hi >= 0) return -1; continue; }
    int v = (*s >= '0' && *s <= '9') ? *s - '0' : (*s >= 'a' && *s <= 'f') ? *s - 'a' + 10 : (*s >= 'A' && *s <= 'F') ? *s - 'A' + 10 : -1;
    if (v < 0) return -1;
    if (hi < 0) hi = v;
    else { if (n >= cap) return -1; out[n++] = (uint8_t)(hi << 4 | v); hi = -1; }
  }
  return hi >= 0 ? -1 : (int)n;
}

static bool hold_low_on = false;
static uint32_t hold_low_until = 0;

static void line_command(char* l) {
  uint8_t b[lb::MAX_FRAME];
  char c = l[0];
  char* a = l + 1;
  while (*a == ' ') a++;
  if (c == 'x' || c == 'X') {  // x <svc> [payload hex] | X <dst src svc flag [len] payload...> (CRC added)
    int n = parse_hex(a, b, sizeof b);
    bool ok = n >= (c == 'x' ? 1 : 4);
    if (ok) ok = c == 'x' ? master.inject(b[0], b + 1, (size_t)n - 1) : master.inject_raw(b, (size_t)n);
    logf(ok ? "# queued %s for slot 82 (sent at the next free reply slot)" : "# x/X: bad hex, too long or queue full", c == 'x' ? "service message" : "raw body");
  } else if (c == 'a') {
    bool on = *a != '0';
    master.door_alarm(on);
    logf("# door alarm %s (0C/31 + 0C/09, event flags in the next 1 s block)", on ? "ON" : "OFF");
  } else if (c == 't') {
    int v = atoi(a);
    master.set_temperature((int8_t)v);
    logf("# current temperature %d degC (0C/14, event flags raised)", v);
  } else if (c == 'e') {  // e <w0 hex8> <w1 hex8>
    uint8_t w[8];
    int n = parse_hex(a, w, sizeof w);
    if (n != 8) { logf("# e: need 8 hex bytes = w0(4) w1(4), e.g. e 00000100 08000000"); return; }
    uint32_t w0 = (uint32_t)w[0] << 24 | w[1] << 16 | w[2] << 8 | w[3], w1 = (uint32_t)w[4] << 24 | w[5] << 16 | w[6] << 8 | w[7];
    master.raise_flags(w0, w1);
    logf("# event flags %08lX %08lX raised", (unsigned long)w0, (unsigned long)w1);
  } else if (c == 'p') {  // p <grp> <id> <flag> <value hex, 1-4 bytes>
    uint8_t w[8];
    int n = parse_hex(a, w, sizeof w);
    if (n < 4 || n > 7) { logf("# p: p <grp> <id> <flag> <value 1-4 bytes>, e.g. p 0C 06 40 00 EF"); return; }
    uint32_t v = 0;
    for (int i = 3; i < n; i++) v = v << 8 | w[i];
    master.set_value(w[0], w[1], w[2], (uint8_t)(n - 3), v);
    logf("# param %02X/%02X set (no event flag; use `e` to announce it)", w[0], w[1]);
  } else if (c == 'L') {  // L <ms>: stop the master and hold the bus low, like a freezer that is switched off
    long ms = atol(a);
    if (ms <= 0 || ms > 600000) { logf("# L: L <ms>, 1..600000"); return; }
    running = false;
    bus_master::tx_enable(false);
    bus_master::hold_low(true);
    hold_low_until = millis() + (uint32_t)ms;
    hold_low_on = true;
    logf("# bus held LOW for %ld ms (master stopped; press g afterwards)", ms);
  } else logf("# unknown command '%c', ? for help", c);
}

static void console_keys() {
  static char line[96];
  static size_t len = 0;
  while (con.available()) {
    int ch = con.read();
    rx_hold = true;
    rx_hold_from = millis();
    if (len == 0 && strchr("?hgsrvk", ch)) {  // single-key commands, no Enter needed
      if (ch == '?' || ch == 'h') welcome();
      else if (ch == 'v') {
        output = output == Output::Quiet ? Output::Normal : output == Output::Normal ? Output::Full : Output::Quiet;
        logf("# output %s", output_name());
      }
      else if (ch == 'k') dump_seen();
      else if (ch == 'g' && !running) {
        running = true;
        expect_len = 0;  // the last slot before a stop never got closed by a break
        bus_master::tx_enable(true);
        due_us = micros() + 50000;
        logf("# master GO");
      } else if (ch == 's' && running) {
        running = false;
        bus_master::tx_enable(false);
        logf("# master STOP");
      } else if (ch == 'r') { master.restart_handshake(); logf("# handshake restart"); }
    } else if (ch == '\r' || ch == '\n') {
      if (len) { line[len] = 0; line_command(line); len = 0; }
    } else if (ch == 0x1B || ch == 0x03) {  // Esc / Ctrl-C: drop a half-typed line
      if (len) { len = 0; logf("# line cleared"); }
    } else if (ch >= 0x20 && ch < 0x7F && len < sizeof line - 1) {
      line[len++] = (char)ch;  // noise bytes (port open/close, ROM garbage) must not block the single-key commands
    }
  }
}

void setup() {
  WiFi.mode(WIFI_OFF);
  WiFi.forceSleepBegin();
  bus_master::begin();  // must run before the console claims GPIO1
  con.begin(CONSOLE_BAUD, SWSERIAL_8N1, 3, 1);
  con.enableIntTx(false);  // each TX byte atomic; with interrupts on, the UART0 ISR garbles the bit-banged output
  uptime_us_base = micros();
  // The master starts at boot: a box that loses its freezer for minutes stays degraded until it is power-cycled
  // (../01-protocol.md §3), so after a crash or reset the bus must come back within a second. `s` stops it.
  running = true;
  bus_master::tx_enable(true);
  due_us = micros() + 50000;
  welcome();
  logf("# master GO (automatic at boot)");
  console_pump();
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
  if (hold_low_on && (int32_t)(millis() - hold_low_until) >= 0) {
    hold_low_on = false;
    bus_master::hold_low(false);
    logf("# bus released");
  }
  int8_t ft;
  if (master.take_followed(ft)) logf("# current temperature now %d degC (following the set-point, 0C/14, event flags raised)", (int)ft);
  if (millis() - last_stats_ms >= STATS_MS) { last_stats_ms += STATS_MS; print_stats(); }
  console_pump();
  yield();
}
