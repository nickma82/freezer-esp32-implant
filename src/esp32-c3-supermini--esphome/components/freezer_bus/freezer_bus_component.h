#pragma once
#include "esphome/core/component.h"

namespace esphome {
namespace freezer_bus {

class FreezerBusComponent : public Component {
 public:
  void set_bus_pin(uint8_t pin) { this->bus_pin_ = pin; }
  void setup() override;
  void loop() override;
  void dump_config() override;

 protected:
  uint8_t bus_pin_{3};
};

}  // namespace freezer_bus
}  // namespace esphome
