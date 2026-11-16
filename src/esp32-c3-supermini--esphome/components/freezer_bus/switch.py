import esphome.codegen as cg
from esphome.components import switch
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_CONFIG

from . import (
    CONF_FREEZER_BUS_ID,
    FREEZER_BUS_CHILD_SCHEMA,
    FreezerBusComponent,
    freezer_bus_ns,
)

CONF_SUPERFROST = "superfrost"
CONF_PARTY = "party"
CONF_BUS_TRANSMIT = "bus_transmit"

FreezerSwitch = freezer_bus_ns.class_("FreezerSwitch", switch.Switch)
SwitchKind = freezer_bus_ns.enum("SwitchKind", is_class=True)

KINDS = {
    CONF_SUPERFROST: "SUPERFROST",
    CONF_PARTY: "PARTY",
    CONF_BUS_TRANSMIT: "BUS_TRANSMIT",
}


def _schema(**kwargs):
    # no restore: the state always follows the freezer (or the engine's tx_enabled)
    return switch.switch_schema(
        FreezerSwitch, block_inverted=True, **kwargs
    )


CONFIG_SCHEMA = FREEZER_BUS_CHILD_SCHEMA.extend(
    {
        cv.Optional(CONF_SUPERFROST): _schema(icon="mdi:snowflake-alert"),
        cv.Optional(CONF_PARTY): _schema(icon="mdi:party-popper"),
        cv.Optional(CONF_BUS_TRANSMIT): _schema(
            icon="mdi:transmission-tower", entity_category=ENTITY_CATEGORY_CONFIG
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_FREEZER_BUS_ID])
    for key, kind in KINDS.items():
        if key in config:
            var = await switch.new_switch(config[key])
            cg.add(var.set_parent(parent))
            cg.add(var.set_kind(getattr(SwitchKind, kind)))
            cg.add(getattr(parent, f"set_{key}_switch")(var))
