// Phase 1 listener: receive-only. Slots from the UART0 driver are decoded with the lb core and logged (design D6).
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <SoftwareSerial.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "bus_uart.h"
#include "freezer_bus.h"

static constexpr uint32_t CONSOLE_BAUD = 115200;
static constexpr uint32_t STATS_MS = 10000;

// ---- console: UART1 TX on GPIO2 (jumpered to the TX header pin), log ring buffer ----
static constexpr size_t LOG_RING = 4096;
static char logbuf[LOG_RING];
static size_t log_head = 0, log_tail = 0;
static uint32_t log_drops = 0, log_drops_total = 0;

static size_t log_free() { return (log_tail + LOG_RING - log_head - 1) % LOG_RING; }

static void log_line(const char* s, size_t n) {  // whole lines only: a line that does not fit is dropped and counted
  if (n + 1 > log_free()) { log_drops++; log_drops_total++; return; }
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

static void console_pump() {  // never blocks: writes only what the UART1 FIFO accepts
  while (log_tail != log_head) {
    int room = Serial1.availableForWrite();
    if (room <= 0) return;
    Serial1.write((uint8_t)logbuf[log_tail]);
    log_tail = (log_tail + 1) % LOG_RING;
  }
}

static SoftwareSerial con_rx;  // RX only on GPIO3 (CP2102 TX)
static bool full_output = false;

static uint32_t now_us() { return (uint32_t)(micros()); }

// ---- statistics ----
struct Stats {
  uint32_t slot_count[256];
  uint32_t empty, crc_ok, crc_bad, decode_err, framing, overruns;
};
static Stats st;
static lb::ScheduleTracker tracker;
static uint32_t uptime_us_base;
static uint32_t last_stats_ms;
static bus_uart::Counters prev;  // driver counters at the last S line

static bool is_idle(const lb::Msg& m) {  // 30 00 / 30 01 ack with payload 02 00 00 ("nothing to send")
  return m.kind == lb::Kind::Ack && m.raw_len == 10 && m.raw[5] == 0x02 && m.raw[6] == 0x00 && m.raw[7] == 0x00;
}

static void handle_slot(const bus_uart::Slot& s) {
  static lb::Msg m;
  static lb::Interp in;
  uint64_t t_us = s.t0_cyc / bus_uart::CPU_MHZ;
  if (s.n == 0) return;
  uint8_t slot = s.data[0];
  st.slot_count[slot]++;
  tracker.slot(slot, t_us);

  if (s.flags & (bus_uart::F_FRAMING | bus_uart::F_OVERRUN | bus_uart::F_TRUNC)) {
    st.framing += (s.flags & bus_uart::F_FRAMING) ? 1 : 0;
    logf("E %llu damaged slot=%02X flags=%02X n=%u", (unsigned long long)t_us, slot, s.flags, s.n);
    return;
  }
  lb::Status rc = lb::decode(s.data, s.n, m);
  bool empty = s.n == 1;
  int32_t lat = -1;
  if (s.n >= 2) {
    lat = (int32_t)((s.dt_cyc[1] - s.dt_cyc[0]) / bus_uart::CPU_MHZ);
    tracker.latency(m.src, (uint32_t)lat);
  }
  if (empty) st.empty++;
  else if (m.crc_ok) st.crc_ok++;
  else st.crc_bad++;
  if (rc != lb::Status::Ok) {
    st.decode_err++;
    logf("E %llu decode slot=%02X err=%u", (unsigned long long)t_us, slot, (unsigned)rc);
  }
  if (!full_output && (empty || is_idle(m))) return;

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
  const bus_uart::Counters& c = bus_uart::counters();
  char b[700];
  int n = snprintf(b, sizeof b, "S %lu", (unsigned long)((micros() - uptime_us_base)));
  for (int i = 0; i < 256 && n < (int)sizeof b - 40; i++)
    if (st.slot_count[i]) n += snprintf(b + n, sizeof b - n, " s%02X=%lu", i, (unsigned long)st.slot_count[i]);
  n += snprintf(b + n, sizeof b - n, " empty=%lu crc_ok=%lu crc_bad=%lu decode_err=%lu framing=%lu overruns=%lu overruns_total=%lu",
                (unsigned long)st.empty, (unsigned long)st.crc_ok, (unsigned long)st.crc_bad, (unsigned long)st.decode_err,
                (unsigned long)st.framing, (unsigned long)(c.overrun - prev.overrun), (unsigned long)c.overrun);
  n += snprintf(b + n, sizeof b - n, " ring_drop=%lu log_drops=%lu", (unsigned long)c.ring_drop, (unsigned long)log_drops);
  uint32_t a, b570, o;
  tracker.period_stats(0x02, a, b570, o);
  n += snprintf(b + n, sizeof b - n, " p02_285=%lu p02_570=%lu p02_other=%lu", (unsigned long)a, (unsigned long)b570, (unsigned long)o);
  lb::ScheduleTracker::Latency lat[lb::ScheduleTracker::SOURCES];
  size_t k = tracker.latencies(lat, lb::ScheduleTracker::SOURCES);
  for (size_t i = 0; i < k && n < (int)sizeof b - 40; i++)
    n += snprintf(b + n, sizeof b - n, " lat%02X=%lu/%lu/%lu", lat[i].src, (unsigned long)lat[i].median,
                  (unsigned long)lat[i].spread, (unsigned long)lat[i].n);
  n += snprintf(b + n, sizeof b - n, " ubd_irq=%lu brk_zero=%lu brk_nozero=%lu", (unsigned long)(c.ubd_irq - prev.ubd_irq),
                (unsigned long)(c.brk_with_zero - prev.brk_with_zero), (unsigned long)(c.brk_no_zero - prev.brk_no_zero));
  log_line(b, n >= (int)sizeof b ? sizeof b - 1 : (size_t)n);
  memset(&st, 0, sizeof st);
  log_drops = 0;
  prev.overrun = c.overrun; prev.ubd_irq = c.ubd_irq; prev.brk_with_zero = c.brk_with_zero; prev.brk_no_zero = c.brk_no_zero;
  tracker.clear_latencies();
}

static void welcome() {
  logf("# ==============================================");
  logf("# Freezer bus listener, phase 1 (receive only)");
  logf("# cpu %u MHz, reset: %s, uptime %lu s", ESP.getCpuFreqMHz(), ESP.getResetReason().c_str(), (unsigned long)(millis() / 1000));
  logf("# bus RX GPIO13/D7 (never transmits), console 115200 on GPIO2/D4");
  logf("# output: %s", full_output ? "full" : "filtered (empty slots, idle replies hidden)");
  logf("# keys: v = toggle full output, ? / h / Enter = this message");
  logf("# ==============================================");
}

void setup() {
  WiFi.mode(WIFI_OFF);
  WiFi.forceSleepBegin();
  Serial1.begin(CONSOLE_BAUD);  // UART1 TX = GPIO2
  con_rx.begin(CONSOLE_BAUD, SWSERIAL_8N1, 3, -1);
  con_rx.enableTx(false);
  uptime_us_base = micros();
  welcome();
  console_pump();
  bus_uart::begin();
  last_stats_ms = millis();
}

void loop() {
  static bus_uart::Slot s;
  uint32_t t0 = now_us();
  while (bus_uart::pop(s)) {
    handle_slot(s);
    if ((uint32_t)(now_us() - t0) > 800) break;  // keep loop() iterations short
  }
  while (con_rx.available()) {
    int ch = con_rx.read();
    if (ch == '?' || ch == 'h' || ch == '\r' || ch == '\n') welcome();
    else if (ch == 'v') { full_output = !full_output; logf("# full output %s", full_output ? "on" : "off"); }
  }
  if (millis() - last_stats_ms >= STATS_MS) { last_stats_ms += STATS_MS; print_stats(); }
  console_pump();
  yield();
}
