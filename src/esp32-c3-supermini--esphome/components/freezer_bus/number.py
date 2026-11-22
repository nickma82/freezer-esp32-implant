import esphome.codegen as cg
from esphome.components import number
import esphome.config_validation as cv
from esphome.const import CONF_MODE, UNIT_CELSIUS

from . import (
    CONF_FREEZER_BUS_ID,
    FREEZER_BUS_CHILD_SCHEMA,
    freezer_bus_ns,
    setpoint_range,
)

CONF_SETPOINT = "setpoint"

FreezerNumber = freezer_bus_ns.class_("FreezerNumber", number.Number)

CONFIG_SCHEMA = FREEZER_BUS_CHILD_SCHEMA.extend(
    {
        cv.Optional(CONF_SETPOINT): number.number_schema(
            FreezerNumber,
            unit_of_measurement=UNIT_CELSIUS,
            icon="mdi:thermometer-lines",
        ).extend({cv.Optional(CONF_MODE, default="BOX"): cv.enum(number.NUMBER_MODES, upper=True)}),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_FREEZER_BUS_ID])
    if conf := config.get(CONF_SETPOINT):
        # freezer_bus: setpoint_min..setpoint_max (default -28..-15), 1 degC steps
        lo, hi = setpoint_range()
        var = await number.new_number(conf, min_value=lo, max_value=hi, step=1)
        cg.add(var.set_parent(parent))
        cg.add(parent.set_setpoint_number(var))
