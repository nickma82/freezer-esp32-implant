import esphome.codegen as cg
from esphome.components import select
import esphome.config_validation as cv

from . import (
    CONF_FREEZER_BUS_ID,
    FREEZER_BUS_CHILD_SCHEMA,
    freezer_bus_ns,
)

CONF_SETPOINT = "setpoint"

# the freezer accepts -28..-15 degC in 1 degC steps (spec ha-freezer-entities); keep in sync with SETPOINT_MIN_C/MAX_C
SETPOINT_MIN_C = -28
SETPOINT_MAX_C = -15
SETPOINT_OPTIONS = [f"{c} °C" for c in range(SETPOINT_MIN_C, SETPOINT_MAX_C + 1)]

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
        # option index i = SETPOINT_MIN_C + i degC (FreezerSelect::control / publish)
        var = await select.new_select(conf, options=SETPOINT_OPTIONS)
        cg.add(var.set_parent(parent))
        cg.add(parent.set_setpoint_select(var))
