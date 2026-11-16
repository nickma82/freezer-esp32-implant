import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import (
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_DOOR,
    DEVICE_CLASS_PROBLEM,
)

from . import CONF_FREEZER_BUS_ID, FREEZER_BUS_CHILD_SCHEMA

CONF_DOOR = "door"
CONF_DOOR_ALARM = "door_alarm"
CONF_POWER_FAIL_ALARM = "power_fail_alarm"
CONF_BUS_CONNECTED = "bus_connected"

SENSORS = {
    CONF_DOOR: binary_sensor.binary_sensor_schema(device_class=DEVICE_CLASS_DOOR),
    CONF_DOOR_ALARM: binary_sensor.binary_sensor_schema(
        device_class=DEVICE_CLASS_PROBLEM, icon="mdi:door-open"
    ),
    CONF_POWER_FAIL_ALARM: binary_sensor.binary_sensor_schema(
        device_class=DEVICE_CLASS_PROBLEM, icon="mdi:flash-alert"
    ),
    CONF_BUS_CONNECTED: binary_sensor.binary_sensor_schema(
        device_class=DEVICE_CLASS_CONNECTIVITY
    ),
}

CONFIG_SCHEMA = FREEZER_BUS_CHILD_SCHEMA.extend(
    {cv.Optional(key): schema for key, schema in SENSORS.items()}
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_FREEZER_BUS_ID])
    for key in SENSORS:
        if key in config:
            var = await binary_sensor.new_binary_sensor(config[key])
            cg.add(getattr(parent, f"set_{key}_binary_sensor")(var))
