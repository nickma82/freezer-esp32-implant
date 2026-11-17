// ESPHome glue: owns the bus driver, maps its snapshots to entities, forwards HA commands. No protocol logic here.
// The main loop only ever reads snapshots / pops events and submits commands; the bus task owns the engine.
#pragma once
#include <string>

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif
#ifdef USE_NUMBER
#include "esphome/components/number/number.h"
#endif
#ifdef USE_SELECT
#include "esphome/components/select/select.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif

#include "bus_c3.h"

namespace esphome {
namespace freezer_bus {

class FreezerBusComponent;
enum class SwitchKind : uint8_t { SUPERFROST, PARTY, BUS_TRANSMIT };

#ifdef USE_SWITCH
// write_state() only enqueues a command: the state changes when the freezer's read-back arrives (no optimistic state)
class FreezerSwitch : public switch_::Switch {
 public:
  void set_parent(FreezerBusComponent *p) { this->parent_ = p; }
  void set_kind(SwitchKind k) { this->kind_ = k; }
  SwitchKind kind() const { return this->kind_; }

 protected:
  void write_state(bool state) override;
  FreezerBusComponent *parent_{nullptr};
  SwitchKind kind_{SwitchKind::SUPERFROST};
};
#endif

#ifdef USE_NUMBER
class FreezerNumber : public number::Number {
 public:
  void set_parent(FreezerBusComponent *p) { this->parent_ = p; }

 protected:
  void control(float value) override;
  FreezerBusComponent *parent_{nullptr};
};
#endif

#ifdef USE_SELECT
// Set-point as a dropdown: option index i = SETPOINT_MIN_C + i degC (the options come from select.py).
// control() only enqueues the write; the selection follows the freezer's read-back.
class FreezerSelect : public select::Select {
 public:
  static constexpr int SETPOINT_MIN_C = -28;
  void set_parent(FreezerBusComponent *p) { this->parent_ = p; }
  void publish_degc(int degc) { this->publish_state((size_t) (degc - SETPOINT_MIN_C)); }
  // Select::publish_state() cannot express "unknown"; Home Assistant shows unknown for missing_state
  void publish_unknown();

 protected:
  void control(size_t index) override;
  FreezerBusComponent *parent_{nullptr};
};
#endif

class FreezerBusComponent : public Component {
 public:
  void set_bus_pin(uint8_t pin) { this->bus_pin_ = pin; }
  void set_reply_delay_us(uint32_t us) { this->reply_delay_us_ = us; }
  void set_status_interval_ms(uint32_t ms) { this->status_interval_ms_ = ms; }
  void set_network_writes(bool on) { this->network_writes_ = on; }

#ifdef USE_SWITCH
  void set_superfrost_switch(FreezerSwitch *s) { this->sw_superfrost_ = s; }
  void set_party_switch(FreezerSwitch *s) { this->sw_party_ = s; }
  void set_bus_transmit_switch(FreezerSwitch *s) { this->sw_tx_ = s; }
#endif
#ifdef USE_NUMBER
  void set_setpoint_number(FreezerNumber *n) { this->num_setpoint_ = n; }
#endif
#ifdef USE_SELECT
  void set_setpoint_select(FreezerSelect *s) { this->sel_setpoint_ = s; }
#endif
#ifdef USE_SENSOR
  void set_temperature_sensor(sensor::Sensor *s) { this->s_temperature_ = s; }
  void set_power_fail_temperature_sensor(sensor::Sensor *s) { this->s_pf_temperature_ = s; }
  void set_latency_median_sensor(sensor::Sensor *s) { this->s_lat_median_ = s; }
  void set_latency_max_sensor(sensor::Sensor *s) { this->s_lat_max_ = s; }
  void set_crc_errors_sensor(sensor::Sensor *s) { this->s_crc_ = s; }
  void set_echo_mismatches_sensor(sensor::Sensor *s) { this->s_echo_ = s; }
  void set_missed_slots_sensor(sensor::Sensor *s) { this->s_missed_ = s; }
#endif
#ifdef USE_BINARY_SENSOR
  void set_door_binary_sensor(binary_sensor::BinarySensor *s) { this->b_door_ = s; }
  void set_door_alarm_binary_sensor(binary_sensor::BinarySensor *s) { this->b_door_alarm_ = s; }
  void set_power_fail_alarm_binary_sensor(binary_sensor::BinarySensor *s) { this->b_pf_alarm_ = s; }
  void set_bus_connected_binary_sensor(binary_sensor::BinarySensor *s) { this->b_connected_ = s; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_freezer_version_text_sensor(text_sensor::TextSensor *s) { this->t_freezer_ver_ = s; }
  void set_node01_version_text_sensor(text_sensor::TextSensor *s) { this->t_node01_ver_ = s; }
  void set_model_text_sensor(text_sensor::TextSensor *s) { this->t_model_ = s; }
  void set_phase_text_sensor(text_sensor::TextSensor *s) { this->t_phase_ = s; }
#endif

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  // from the entities (main loop)
  void command_switch(SwitchKind kind, bool on);
  void command_setpoint(int degc);

 protected:
  template<typename T> struct Last {
    bool valid{false};
    T v{};
  };

  void update_network_();
  void handle_events_();
  void log_frames_();
  void publish_(const BusSnapshot &s, bool force);
  void log_changes_(const BusSnapshot &s, bool ok);
  void request_republish_() { this->force_ = true; }
  void submit_(const Cmd &c, const char *what);

  uint8_t bus_pin_{3};
  uint32_t reply_delay_us_{420};
  uint32_t status_interval_ms_{10000};
  bool network_writes_{true};

  BusC3 bus_;
  uint32_t seen_seq_{0};
  bool force_{true};
  bool have_snap_{false};
  BusSnapshot snap_;  // last snapshot copied by loop() (kept out of the stack)
  BusSnapshot prev_;  // the one before, for change logging

  // network, last value sent to the bus task
  bool net_sent_{false};
  bool net_conn_{false};
  uint8_t net_ip_[4]{};
  uint8_t net_mac_[6]{};
  char net_ssid_[n02::SSID_MAX + 1]{};
  uint32_t net_check_ms_{0};

  // log scratch (member, not stack)
  LogFrame lf_;
  lb::Msg msg_;
  lb::Interp interp_;
  char line_[640];

#ifdef USE_SWITCH
  FreezerSwitch *sw_superfrost_{nullptr}, *sw_party_{nullptr}, *sw_tx_{nullptr};
  Last<bool> l_superfrost_, l_party_, l_tx_;
#endif
#ifdef USE_NUMBER
  FreezerNumber *num_setpoint_{nullptr};
  Last<float> l_setpoint_;
#endif
#ifdef USE_SELECT
  FreezerSelect *sel_setpoint_{nullptr};
  Last<int> l_setpoint_sel_;  // degC, or SETPOINT_UNKNOWN
#endif
#ifdef USE_SENSOR
  sensor::Sensor *s_temperature_{nullptr}, *s_pf_temperature_{nullptr}, *s_lat_median_{nullptr}, *s_lat_max_{nullptr},
      *s_crc_{nullptr}, *s_echo_{nullptr}, *s_missed_{nullptr};
  Last<float> l_temperature_, l_pf_temperature_, l_lat_median_, l_lat_max_, l_crc_, l_echo_, l_missed_;
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *b_door_{nullptr}, *b_door_alarm_{nullptr}, *b_pf_alarm_{nullptr}, *b_connected_{nullptr};
  Last<int8_t> l_door_, l_door_alarm_, l_pf_alarm_, l_connected_;  // -1 = unknown
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *t_freezer_ver_{nullptr}, *t_node01_ver_{nullptr}, *t_model_{nullptr}, *t_phase_{nullptr};
  Last<std::string> l_freezer_ver_, l_node01_ver_, l_model_, l_phase_;
#endif
};

}  // namespace freezer_bus
}  // namespace esphome
