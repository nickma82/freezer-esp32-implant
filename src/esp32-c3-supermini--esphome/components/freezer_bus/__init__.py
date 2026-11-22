import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components.esp32 import (
    VARIANT_ESP32,
    VARIANT_ESP32C3,
    get_esp32_variant,
    only_on_variant,
)
from esphome.const import CONF_ID
from esphome.core import CORE

CODEOWNERS = []
DEPENDENCIES = ["wifi"]

freezer_bus_ns = cg.esphome_ns.namespace("freezer_bus")
FreezerBusComponent = freezer_bus_ns.class_("FreezerBusComponent", cg.Component)

CONF_FREEZER_BUS_ID = "freezer_bus_id"
CONF_BUS_PIN = "bus_pin"
CONF_REPLY_DELAY_US = "reply_delay_us"
CONF_STATUS_INTERVAL = "status_interval"
CONF_NETWORK_WRITES = "network_writes"
CONF_SETPOINT_MIN = "setpoint_min"
CONF_SETPOINT_MAX = "setpoint_max"

# ESP32-C3 pins that must not carry the bus
FORBIDDEN_PINS_C3 = {
    8: "GPIO8 is a strapping pin (and drives the on-board LED)",
    9: "GPIO9 is a strapping pin and the BOOT button",
    18: "GPIO18 is USB D- (USB-Serial/JTAG console)",
    19: "GPIO19 is USB D+ (USB-Serial/JTAG console)",
}

# Classic ESP32 (e.g. ESP32-WROOM-32) pins that must not carry the bus. A strapping pin on the bus would read the bus
# level at reset: idle high (GPIO12 high = 1.8 V flash), low during a BREAK (GPIO0 low = download mode).
_STRAP = "is a strapping pin and would read the bus level at reset"
FORBIDDEN_PINS_ESP32 = {
    0: f"GPIO0 {_STRAP} (low = download mode)",
    1: "GPIO1 is U0TXD (console, flashing)",
    2: f"GPIO2 {_STRAP} (boot mode)",
    3: "GPIO3 is U0RXD (console, flashing)",
    5: f"GPIO5 {_STRAP} (SDIO timing)",
    12: f"GPIO12 {_STRAP} (high = 1.8 V flash, the module does not boot)",
    15: f"GPIO15 {_STRAP} (boot log, SDIO timing)",
    **{p: f"GPIO{p} is wired to the module's SPI flash" for p in range(6, 12)},
    **{p: f"GPIO{p} does not exist on the ESP32" for p in (20, 24, 28, 29, 30, 31)},
    **{p: f"GPIO{p} is input-only and cannot drive the bus" for p in range(34, 40)},
}

# variant -> (highest GPIO number, forbidden pins)
PIN_RULES = {
    VARIANT_ESP32C3: (21, FORBIDDEN_PINS_C3),
    VARIANT_ESP32: (39, FORBIDDEN_PINS_ESP32),
}


def validate_bus_pin(value):
    max_pin, forbidden = PIN_RULES[get_esp32_variant()]
    pin = cv.int_range(min=0, max=max_pin)(_pin_number(value))
    if pin in forbidden:
        raise cv.Invalid(
            f"{forbidden[pin]}: choose another pin (straps/boot/console/flash pins are not usable for the bus)"
        )
    return pin


def _pin_number(value):
    if isinstance(value, str) and value.upper().startswith("GPIO"):
        value = value[4:]
    return cv.int_(value)


def _validate_setpoint_range(config):
    if config[CONF_SETPOINT_MIN] > config[CONF_SETPOINT_MAX]:
        raise cv.Invalid(
            f"{CONF_SETPOINT_MIN} ({config[CONF_SETPOINT_MIN]}) must not be above "
            f"{CONF_SETPOINT_MAX} ({config[CONF_SETPOINT_MAX]})"
        )
    return config


def setpoint_range():
    """(min, max) degC from the freezer_bus config, for the select and number platforms."""
    conf = CORE.config["freezer_bus"]
    return conf[CONF_SETPOINT_MIN], conf[CONF_SETPOINT_MAX]


CONFIG_SCHEMA = cv.All(
    cv.only_on_esp32,
    # before the schema, so validate_bus_pin() only ever sees a variant it has rules for
    only_on_variant(
        supported=list(PIN_RULES), msg_prefix="freezer_bus (ESP32-C3 and classic ESP32 only)"
    ),
    cv.only_with_framework("esp-idf"),
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
            # accepted set-point range (0C/06, whole degC): the default is a freezer's; a fridge reports e.g. +4
            cv.Optional(CONF_SETPOINT_MIN, default=-28): cv.int_range(min=-40, max=20),
            cv.Optional(CONF_SETPOINT_MAX, default=-15): cv.int_range(min=-40, max=20),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_setpoint_range,
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
    cg.add(var.set_setpoint_range(config[CONF_SETPOINT_MIN], config[CONF_SETPOINT_MAX]))
