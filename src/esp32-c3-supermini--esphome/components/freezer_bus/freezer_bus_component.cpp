#include "freezer_bus_component.h"
#include "esphome/core/log.h"
#include "freezer_bus.h"

namespace esphome {
namespace freezer_bus {

static const char *const TAG = "freezer";

void FreezerBusComponent::setup() {}

void FreezerBusComponent::loop() {}

void FreezerBusComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Freezer bus:");
  ESP_LOGCONFIG(TAG, "  Bus pin: GPIO%u", this->bus_pin_);
  // proves the shared core links in
  static const uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  ESP_LOGCONFIG(TAG, "  Core CRC16/XMODEM('123456789') = 0x%04X (expect 0x31C3)", ::lb::crc16_xmodem(check, sizeof(check)));
}

}  // namespace freezer_bus
}  // namespace esphome
