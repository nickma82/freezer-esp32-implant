#include "freezer_bus_component.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#ifdef USE_WIFI
#include "esphome/components/network/ip_address.h"
#include "esphome/components/wifi/wifi_component.h"
#endif

namespace esphome {
namespace freezer_bus {

static const char *const TAG = "freezer";

static const char *phase_name(n02::Phase p) {
  switch (p) {
    case n02::Phase::Waiting: return "waiting";
    case n02::Phase::Handshake: return "handshake";
    case n02::Phase::Startup: return "startup";
    case n02::Phase::Running: return "running";
  }
  return "?";
}

static const char *refusal_text(uint8_t r) {
  switch ((n02::Result) r) {
    case n02::Result::Accepted: return "accepted";
    case n02::Result::RefusedTxOff: return "bus transmit is off";
    case n02::Result::RefusedNotRunning: return "start-up with the freezer is not complete";
    case n02::Result::RefusedBusDown: return "bus is not alive";
    case n02::Result::RefusedRange: return "value out of range";
    case n02::Result::RefusedQueueFull: return "command queue full";
  }
  return "?";
}

static const char *param_name(n02::WriteParam p) {
  switch (p) {
    case n02::WriteParam::SuperFrost: return "SuperFrost";
    case n02::WriteParam::Party: return "Party mode";
    case n02::WriteParam::Setpoint: return "Set-point";
  }
  return "?";
}

#ifdef USE_SWITCH
void FreezerSwitch::write_state(bool state) { this->parent_->command_switch(this->kind_, state); }
#endif
#ifdef USE_NUMBER
void FreezerNumber::control(float value) { this->parent_->command_setpoint((int) lroundf(value)); }
#endif

// ---------------------------------------------------------------------------------------------------------------

void FreezerBusComponent::setup() {
  this->bus_.configure(this->bus_pin_, this->reply_delay_us_, this->status_interval_ms_, this->network_writes_);
  if (!this->bus_.start()) {
    ESP_LOGE(TAG, "bus driver failed to start (GPIO%u)", this->bus_pin_);
    this->mark_failed();
    return;
  }
  ESP_LOGI(TAG, "bus driver started on GPIO%u, reply delay %u us", this->bus_pin_, (unsigned) this->reply_delay_us_);
}

void FreezerBusComponent::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Freezer bus (node 02):\n"
                "  Bus pin: GPIO%u (open-drain, UART1, 57600 8N1)\n"
                "  Reply delay: %u us (window 250..600)\n"
                "  Status interval: %u ms\n"
                "  Network writes (group 48): %s",
                this->bus_pin_, (unsigned) this->reply_delay_us_, (unsigned) this->status_interval_ms_,
                YESNO(this->network_writes_));
#ifdef USE_SWITCH
  LOG_SWITCH("  ", "SuperFrost", this->sw_superfrost_);
  LOG_SWITCH("  ", "Party", this->sw_party_);
  LOG_SWITCH("  ", "Bus transmit", this->sw_tx_);
#endif
#ifdef USE_NUMBER
  LOG_NUMBER("  ", "Set-point", this->num_setpoint_);
#endif
#ifdef USE_SENSOR
  LOG_SENSOR("  ", "Temperature", this->s_temperature_);
  LOG_SENSOR("  ", "Power-failure temperature", this->s_pf_temperature_);
  LOG_SENSOR("  ", "Latency median", this->s_lat_median_);
  LOG_SENSOR("  ", "Latency max", this->s_lat_max_);
  LOG_SENSOR("  ", "CRC errors", this->s_crc_);
  LOG_SENSOR("  ", "Echo mismatches", this->s_echo_);
  LOG_SENSOR("  ", "Missed slots", this->s_missed_);
#endif
#ifdef USE_BINARY_SENSOR
  LOG_BINARY_SENSOR("  ", "Door", this->b_door_);
  LOG_BINARY_SENSOR("  ", "Door alarm", this->b_door_alarm_);
  LOG_BINARY_SENSOR("  ", "Power-failure alarm", this->b_pf_alarm_);
  LOG_BINARY_SENSOR("  ", "Bus connected", this->b_connected_);
#endif
#ifdef USE_TEXT_SENSOR
  LOG_TEXT_SENSOR("  ", "Freezer version", this->t_freezer_ver_);
  LOG_TEXT_SENSOR("  ", "Node 01 version", this->t_node01_ver_);
  LOG_TEXT_SENSOR("  ", "Model", this->t_model_);
  LOG_TEXT_SENSOR("  ", "Phase", this->t_phase_);
#endif
}

// ---------------------------------------------------------------------------------------------------------------
// Commands from the entities
// ---------------------------------------------------------------------------------------------------------------

void FreezerBusComponent::submit_(const Cmd &c, const char *what) {
  if (!this->bus_.submit(c)) {
    ESP_LOGW(TAG, "%s refused: command queue full", what);
    this->request_republish_();
  }
}

void FreezerBusComponent::command_switch(SwitchKind kind, bool on) {
  Cmd c;
  c.value = on ? 1 : 0;
  const char *what = "";
  switch (kind) {
    case SwitchKind::SUPERFROST: c.type = CmdType::SuperFrost; what = "SuperFrost"; break;
    case SwitchKind::PARTY: c.type = CmdType::Party; what = "Party mode"; break;
    case SwitchKind::BUS_TRANSMIT: c.type = CmdType::TxEnable; what = "Bus transmit"; break;
  }
  ESP_LOGI(TAG, "%s -> %s requested", what, on ? "on" : "off");
  this->submit_(c, what);
}

void FreezerBusComponent::command_setpoint(int degc) {
  Cmd c;
  c.type = CmdType::Setpoint;
  c.value = degc;
  ESP_LOGI(TAG, "Set-point -> %d degC requested", degc);
  this->submit_(c, "Set-point");
}

// ---------------------------------------------------------------------------------------------------------------
// Network info -> bus task (group 48 writes)
// ---------------------------------------------------------------------------------------------------------------

void FreezerBusComponent::update_network_() {
#ifdef USE_WIFI
  const uint32_t now = millis();
  if (this->net_sent_ && now - this->net_check_ms_ < 500) return;
  this->net_check_ms_ = now;

  bool conn = false;
  uint8_t ip[4] = {0, 0, 0, 0};
  char ssid[wifi::SSID_BUFFER_SIZE] = {0};
  wifi::WiFiComponent *w = wifi::global_wifi_component;
  if (w != nullptr && w->is_connected()) {
    for (auto &a : w->wifi_sta_ip_addresses()) {
      if (!a.is_set()) continue;
      char buf[network::IP_ADDRESS_BUFFER_SIZE];
      a.str_to(buf);
      unsigned o[4];
      if (sscanf(buf, "%u.%u.%u.%u", &o[0], &o[1], &o[2], &o[3]) == 4) {  // IPv6 strings do not match
        for (int i = 0; i < 4; i++) ip[i] = (uint8_t) o[i];
        conn = true;
        break;
      }
    }
    if (conn) w->wifi_ssid_to(std::span<char, wifi::SSID_BUFFER_SIZE>(ssid));
  }
  uint8_t mac[6];
  get_mac_address_raw(mac);
  ssid[sizeof ssid - 1] = 0;

  if (this->net_sent_ && conn == this->net_conn_ && memcmp(ip, this->net_ip_, 4) == 0 &&
      memcmp(mac, this->net_mac_, 6) == 0 && strncmp(ssid, this->net_ssid_, sizeof ssid) == 0)
    return;

  Cmd c;
  c.type = CmdType::Network;
  c.connected = conn;
  memcpy(c.ip, ip, 4);
  memcpy(c.mac, mac, 6);
  strncpy(c.ssid, ssid, n02::SSID_MAX);
  if (!this->bus_.submit(c)) return;  // retried on the next check
  this->net_sent_ = true;
  this->net_conn_ = conn;
  memcpy(this->net_ip_, ip, 4);
  memcpy(this->net_mac_, mac, 6);
  strncpy(this->net_ssid_, ssid, sizeof this->net_ssid_ - 1);
  ESP_LOGI(TAG, "network info for the freezer: %s", conn ? "connected" : "not connected");
  if (conn) ESP_LOGD(TAG, "network info: ip %u.%u.%u.%u ssid '%s'", ip[0], ip[1], ip[2], ip[3], ssid);
#endif
}

// ---------------------------------------------------------------------------------------------------------------
// loop
// ---------------------------------------------------------------------------------------------------------------

void FreezerBusComponent::loop() {
  this->update_network_();
  this->handle_events_();
  this->log_frames_();

  BusSnapshot &cur = this->snap_;
  BusSnapshot fresh;
  this->bus_.snapshot(fresh);
  if (fresh.seq != this->seen_seq_ || this->force_) {
    this->prev_ = this->have_snap_ ? cur : fresh;
    cur = fresh;
    this->seen_seq_ = fresh.seq;
    this->have_snap_ = true;
    const bool ok = cur.alive && cur.phase == n02::Phase::Running;
    this->log_changes_(cur, ok);
    this->publish_(cur, this->force_);
    this->force_ = false;
  }
}

void FreezerBusComponent::handle_events_() {
  BusEvent e;
  for (int i = 0; i < 8 && this->bus_.pop_event(e); i++) {
    switch (e.kind) {
      case BusEvent::Kind::Refusal:
        ESP_LOGW(TAG, "%s %d refused: %s", param_name(e.param), e.value, refusal_text(e.code));
        this->request_republish_();  // HA snaps back to the freezer's actual value
        break;
      case BusEvent::Kind::WriteResult: {
        switch ((n02::WriteOutcome) e.code) {
          case n02::WriteOutcome::Confirmed:
            ESP_LOGI(TAG, "write confirmed: %s = %d", param_name(e.param), e.value);
            break;
          case n02::WriteOutcome::Unconfirmed:
            ESP_LOGW(TAG, "write NOT confirmed: %s = %d (the freezer's read-back differs; showing its value)",
                     param_name(e.param), e.value);
            break;
          case n02::WriteOutcome::NotSent:
            ESP_LOGW(TAG, "write not sent: %s = %d (transmit off, bus down or freezer restarted)", param_name(e.param),
                     e.value);
            break;
        }
        this->request_republish_();
        break;
      }
      case BusEvent::Kind::EchoMismatch: {
        static const char *const why[] = {"byte differs", "echo incomplete / missing", "extra bytes after the answer",
                                          "framing error"};
        if (e.expected != 0xFFFF)
          ESP_LOGE(TAG, "echo mismatch (%s) at answer byte %u: sent %02X, heard %02X; bus transmit disabled",
                   why[e.reason & 3], (unsigned) e.idx, (unsigned) e.expected, (unsigned) e.got);
        else
          ESP_LOGE(TAG, "echo mismatch (%s) at answer byte %u; bus transmit disabled", why[e.reason & 3],
                   (unsigned) e.idx);
        break;
      }
    }
  }
}

// DEBUG: one line per frame in the PoC format (tools/compare.py reads it):
//   M <t_us> <slot hex> <lat_us|-> <crc 1|0|-> <body hex> | <decoded>
void FreezerBusComponent::log_frames_() {
  for (int i = 0; i < 4 && this->bus_.pop_log(this->lf_); i++) {
    const LogFrame &f = this->lf_;
    char *p = this->line_;
    char *end = this->line_ + sizeof this->line_;
    p += snprintf(p, end - p, "M %llu %02X ", (unsigned long long) f.t_us, f.data[0]);
    if (f.lat_us >= 0) p += snprintf(p, end - p, "%ld ", (long) f.lat_us);
    else p += snprintf(p, end - p, "- ");
    lb::Status st = lb::decode(f.data, f.n, this->msg_);
    p += snprintf(p, end - p, "%c", this->msg_.crc_ok ? '1' : '0');
    for (uint8_t k = 1; k < f.n && p < end - 8; k++) p += snprintf(p, end - p, " %02X", f.data[k]);
    p += snprintf(p, end - p, " | ");
    if (st != lb::Status::Ok) {
      p += snprintf(p, end - p, "decode error %u", (unsigned) st);
    } else {
      this->interp_ = lb::Interp();
      lb::interpret(this->msg_, this->interp_);
      if (this->interp_.n) p += lb::describe(this->interp_, p, end - p);
      else p += snprintf(p, end - p, "kind=%u svc=%02X", (unsigned) this->msg_.kind, this->msg_.svc);
    }
    ESP_LOGD(TAG, "%s", this->line_);
  }
}

void FreezerBusComponent::log_changes_(const BusSnapshot &s, bool ok) {
  const BusSnapshot &o = this->prev_;
  if (!this->have_snap_) return;
  if (s.phase != o.phase) ESP_LOGI(TAG, "phase: %s -> %s", phase_name(o.phase), phase_name(s.phase));
  if (o.phase == n02::Phase::Waiting && s.phase == n02::Phase::Startup)
    ESP_LOGI(TAG, "no hello from the freezer for %u s: start-up reads without handshake (rejoin)",
             (unsigned) (n02::REJOIN_AFTER_MS / 1000));
  if (o.phase == n02::Phase::Startup && s.phase == n02::Phase::Waiting)
    ESP_LOGW(TAG, "rejoin failed: the freezer answered no start-up read, next try in %u s",
             (unsigned) (n02::REJOIN_AFTER_MS / 1000));
  if (s.alive != o.alive) ESP_LOGI(TAG, "bus %s", s.alive ? "alive" : "lost (no slot 02 for 2 s)");
  if (s.tx_enabled != o.tx_enabled)
    ESP_LOGI(TAG, "bus transmit %s%s%s", s.tx_enabled ? "enabled" : "disabled", s.tx_off_reason[0] ? ": " : "",
             s.tx_off_reason);
  if (!ok) return;
  struct Item {
    const char *name;
    const n02::Opt n02::State::*m;
  };
  static const Item items[] = {
      {"mode (1 normal, 4 SuperFrost)", &n02::State::mode}, {"party", &n02::State::party},
      {"set-point", &n02::State::setpoint},                 {"temperature", &n02::State::temperature},
      {"door open", &n02::State::door_open},                {"door alarm", &n02::State::door_alarm},
      {"power-failure alarm", &n02::State::power_fail_alarm}, {"power-failure temperature", &n02::State::power_fail_temp},
  };
  const bool was_ok = o.alive && o.phase == n02::Phase::Running;
  for (const Item &it : items) {
    const n02::Opt &a = s.st.*(it.m);
    const n02::Opt &b = o.st.*(it.m);
    if (a.known != b.known || (a.known && a.v != b.v) || !was_ok) {
      if (a.known) ESP_LOGI(TAG, "%s = %d", it.name, (int) a.v);
      else ESP_LOGI(TAG, "%s = unknown", it.name);
    }
  }
  if (strcmp(s.st.freezer_version, o.st.freezer_version) != 0 && s.st.freezer_version[0])
    ESP_LOGI(TAG, "freezer version: %s", s.st.freezer_version);
  if (strcmp(s.st.node01_version, o.st.node01_version) != 0 && s.st.node01_version[0])
    ESP_LOGI(TAG, "node 01 version: %s", s.st.node01_version);
  if (strcmp(s.st.model, o.st.model) != 0 && s.st.model[0]) ESP_LOGI(TAG, "model: %s", s.st.model);
}

// ---------------------------------------------------------------------------------------------------------------
// Entity publishing. Rule: freezer entities follow the freezer; while the bus is not alive or the start-up is not
// Running they are unknown (NAN / invalidated), the control entities keep their last state and refuse changes (engine).
// ---------------------------------------------------------------------------------------------------------------

#ifdef USE_SENSOR
static void put_sensor(sensor::Sensor *s, float v, bool force, bool &valid, float &last) {
  if (s == nullptr) return;
  if (!force && valid && (last == v || (isnan(last) && isnan(v)))) return;
  valid = true;
  last = v;
  s->publish_state(v);
}
#endif
#ifdef USE_BINARY_SENSOR
static void put_binary(binary_sensor::BinarySensor *b, int v, bool force, bool &valid, int8_t &last) {  // v: -1 unknown
  if (b == nullptr) return;
  if (!force && valid && last == v) return;
  valid = true;
  last = (int8_t) v;
  if (v < 0) b->invalidate_state();
  else b->publish_state(v != 0);
}
#endif
#ifdef USE_TEXT_SENSOR
static void put_text(text_sensor::TextSensor *t, const char *v, bool force, bool &valid, std::string &last) {
  if (t == nullptr) return;
  if (!force && valid && last == v) return;
  valid = true;
  last = v;
  t->publish_state(v);
}
#endif

void FreezerBusComponent::publish_(const BusSnapshot &s, bool force) {
  const bool ok = s.alive && s.phase == n02::Phase::Running;
  const n02::State &st = s.st;
  (void) st;
#ifdef USE_SENSOR
  auto opt = [&](const n02::Opt &o) { return (ok && o.known) ? (float) o.v : NAN; };
  put_sensor(this->s_temperature_, opt(st.temperature), force, this->l_temperature_.valid, this->l_temperature_.v);
  put_sensor(this->s_pf_temperature_, opt(st.power_fail_temp), force, this->l_pf_temperature_.valid,
             this->l_pf_temperature_.v);
  put_sensor(this->s_lat_median_, s.lat_n ? (float) s.lat_median_us : NAN, force, this->l_lat_median_.valid,
             this->l_lat_median_.v);
  put_sensor(this->s_lat_max_, s.lat_n ? (float) s.lat_max_us : NAN, force, this->l_lat_max_.valid, this->l_lat_max_.v);
  put_sensor(this->s_crc_, (float) s.eng.crc_errors, force, this->l_crc_.valid, this->l_crc_.v);
  put_sensor(this->s_echo_, (float) s.echo_mismatches, force, this->l_echo_.valid, this->l_echo_.v);
  put_sensor(this->s_missed_, (float) s.missed_slots, force, this->l_missed_.valid, this->l_missed_.v);
#endif
#ifdef USE_BINARY_SENSOR
  auto bopt = [&](const n02::Opt &o) { return (ok && o.known) ? (o.v != 0 ? 1 : 0) : -1; };
  put_binary(this->b_door_, bopt(st.door_open), force, this->l_door_.valid, this->l_door_.v);
  put_binary(this->b_door_alarm_, bopt(st.door_alarm), force, this->l_door_alarm_.valid, this->l_door_alarm_.v);
  put_binary(this->b_pf_alarm_, bopt(st.power_fail_alarm), force, this->l_pf_alarm_.valid, this->l_pf_alarm_.v);
  put_binary(this->b_connected_, s.alive ? 1 : 0, force, this->l_connected_.valid, this->l_connected_.v);
#endif
#ifdef USE_TEXT_SENSOR
  auto txt = [&](const char *v) { return (ok && v[0]) ? v : "unknown"; };
  put_text(this->t_freezer_ver_, txt(st.freezer_version), force, this->l_freezer_ver_.valid, this->l_freezer_ver_.v);
  put_text(this->t_node01_ver_, txt(st.node01_version), force, this->l_node01_ver_.valid, this->l_node01_ver_.v);
  put_text(this->t_model_, txt(st.model), force, this->l_model_.valid, this->l_model_.v);
  put_text(this->t_phase_, phase_name(s.phase), force, this->l_phase_.valid, this->l_phase_.v);
#endif
#ifdef USE_SWITCH
  // Control switches have no "unknown": while the freezer value is unknown they keep their last state.
  auto put_sw = [&](FreezerSwitch *sw, Last<bool> &l, bool known, bool v) {
    if (sw == nullptr) return;
    if (known) {
      if (!force && l.valid && l.v == v) return;
      l.valid = true;
      l.v = v;
      sw->publish_state(v);
    } else if (force && l.valid) {
      sw->publish_state(sw->state);  // snap HA back to what the entity shows
    }
  };
  put_sw(this->sw_superfrost_, this->l_superfrost_, ok && st.mode.known, st.mode.v == 4);
  put_sw(this->sw_party_, this->l_party_, ok && st.party.known, st.party.v != 0);
  put_sw(this->sw_tx_, this->l_tx_, true, s.tx_enabled);  // follows the engine, incl. the automatic echo-mismatch off
#endif
#ifdef USE_NUMBER
  if (this->num_setpoint_ != nullptr) {
    const float v = (ok && st.setpoint.known) ? (float) st.setpoint.v : NAN;
    const bool same = this->l_setpoint_.valid && (this->l_setpoint_.v == v || (isnan(this->l_setpoint_.v) && isnan(v)));
    if (force || !same) {
      this->l_setpoint_.valid = true;
      this->l_setpoint_.v = v;
      this->num_setpoint_->publish_state(v);
    }
  }
#endif
}

}  // namespace freezer_bus
}  // namespace esphome
