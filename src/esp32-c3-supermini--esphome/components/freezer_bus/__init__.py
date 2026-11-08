import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = []

freezer_bus_ns = cg.esphome_ns.namespace("freezer_bus")
FreezerBusComponent = freezer_bus_ns.class_("FreezerBusComponent", cg.Component)

CONF_BUS_PIN = "bus_pin"

# ESP32-C3 pins that must not carry the bus
FORBIDDEN_PINS = {
    8: "GPIO8 is a strapping pin (and drives the on-board LED)",
    9: "GPIO9 is a strapping pin and the BOOT button",
    18: "GPIO18 is USB D- (USB-Serial/JTAG console)",
    19: "GPIO19 is USB D+ (USB-Serial/JTAG console)",
}


def validate_bus_pin(value):
    pin = cv.int_range(min=0, max=21)(_pin_number(value))
    if pin in FORBIDDEN_PINS:
        raise cv.Invalid(
            f"{FORBIDDEN_PINS[pin]}: choose another pin (straps/BOOT/USB are not usable for the bus)"
        )
    return pin


def _pin_number(value):
    if isinstance(value, str) and value.upper().startswith("GPIO"):
        value = value[4:]
    return cv.int_(value)


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(FreezerBusComponent),
        cv.Required(CONF_BUS_PIN): validate_bus_pin,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_bus_pin(config[CONF_BUS_PIN]))
