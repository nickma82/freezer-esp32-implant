import esphome.codegen as cg
from esphome.components import select
import esphome.config_validation as cv

from . import (
    CONF_FREEZER_BUS_ID,
    FREEZER_BUS_CHILD_SCHEMA,
    freezer_bus_ns,
    setpoint_range,
)

CONF_SETPOINT = "setpoint"

FreezerSelect = freezer_bus_ns.class_("FreezerSelect", select.Select)

CONFIG_SCHEMA = FREEZER_BUS_CHILD_SCHEMA.extend(
    {
        cv.Optional(CONF_SETPOINT): select.select_schema(
            FreezerSelect,
            icon="mdi:thermometer-lines",
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_FREEZER_BUS_ID])
    if conf := config.get(CONF_SETPOINT):
        # one option per degC in freezer_bus: setpoint_min..setpoint_max (default -28..-15);
        # option index i = setpoint_min + i degC (FreezerSelect::control / publish)
        lo, hi = setpoint_range()
        var = await select.new_select(conf, options=[f"{c} °C" for c in range(lo, hi + 1)])
        cg.add(var.set_min_c(lo))
        cg.add(var.set_parent(parent))
        cg.add(parent.set_setpoint_select(var))
