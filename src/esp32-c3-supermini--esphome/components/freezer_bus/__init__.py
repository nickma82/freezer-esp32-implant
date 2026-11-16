import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = []
DEPENDENCIES = ["wifi"]

freezer_bus_ns = cg.esphome_ns.namespace("freezer_bus")
FreezerBusComponent = freezer_bus_ns.class_("FreezerBusComponent", cg.Component)

CONF_FREEZER_BUS_ID = "freezer_bus_id"
CONF_BUS_PIN = "bus_pin"
CONF_REPLY_DELAY_US = "reply_delay_us"
CONF_STATUS_INTERVAL = "status_interval"
CONF_NETWORK_WRITES = "network_writes"

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


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(FreezerBusComponent),
            cv.Required(CONF_BUS_PIN): validate_bus_pin,
            # first answer byte this long after the slot byte was read (spec window 250..600 us)
            cv.Optional(CONF_REPLY_DELAY_US, default=420): cv.int_range(
                min=250, max=600
            ),
            cv.Optional(
                CONF_STATUS_INTERVAL, default="10s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_NETWORK_WRITES, default=True): cv.boolean,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    cv.only_with_framework("esp-idf"),
)

# Parent reference used by the platform files (switch.py, number.py, ...)
FREEZER_BUS_CHILD_SCHEMA = cv.Schema(
    {cv.GenerateID(CONF_FREEZER_BUS_ID): cv.use_id(FreezerBusComponent)}
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_bus_pin(config[CONF_BUS_PIN]))
    cg.add(var.set_reply_delay_us(config[CONF_REPLY_DELAY_US]))
    cg.add(var.set_status_interval_ms(config[CONF_STATUS_INTERVAL]))
    cg.add(var.set_network_writes(config[CONF_NETWORK_WRITES]))
