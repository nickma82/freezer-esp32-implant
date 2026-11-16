import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_FREEZER_BUS_ID, FREEZER_BUS_CHILD_SCHEMA

CONF_FREEZER_VERSION = "freezer_version"
CONF_NODE01_VERSION = "node01_version"
CONF_MODEL = "model"
CONF_PHASE = "phase"

SENSORS = {
    CONF_FREEZER_VERSION: text_sensor.text_sensor_schema(
        icon="mdi:chip", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
    ),
    CONF_NODE01_VERSION: text_sensor.text_sensor_schema(
        icon="mdi:chip", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
    ),
    CONF_MODEL: text_sensor.text_sensor_schema(
        icon="mdi:fridge", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
    ),
    CONF_PHASE: text_sensor.text_sensor_schema(
        icon="mdi:state-machine", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
    ),
}

CONFIG_SCHEMA = FREEZER_BUS_CHILD_SCHEMA.extend(
    {cv.Optional(key): schema for key, schema in SENSORS.items()}
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_FREEZER_BUS_ID])
    for key in SENSORS:
        if key in config:
            var = await text_sensor.new_text_sensor(config[key])
            cg.add(getattr(parent, f"set_{key}_text_sensor")(var))
