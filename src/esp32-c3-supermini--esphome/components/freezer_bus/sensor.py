import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    DEVICE_CLASS_TEMPERATURE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_CELSIUS,
)

from . import CONF_FREEZER_BUS_ID, FREEZER_BUS_CHILD_SCHEMA

CONF_TEMPERATURE = "temperature"
CONF_POWER_FAIL_TEMPERATURE = "power_fail_temperature"
CONF_LATENCY_MEDIAN = "latency_median"
CONF_LATENCY_MAX = "latency_max"
CONF_CRC_ERRORS = "crc_errors"
CONF_ECHO_MISMATCHES = "echo_mismatches"
CONF_MISSED_SLOTS = "missed_slots"

UNIT_MICROSECOND = "µs"


def _temp(icon):
    return sensor.sensor_schema(
        unit_of_measurement=UNIT_CELSIUS,
        accuracy_decimals=0,
        device_class=DEVICE_CLASS_TEMPERATURE,
        state_class=STATE_CLASS_MEASUREMENT,
        icon=icon,
    )


def _latency():
    return sensor.sensor_schema(
        unit_of_measurement=UNIT_MICROSECOND,
        accuracy_decimals=0,
        state_class=STATE_CLASS_MEASUREMENT,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        icon="mdi:timer-outline",
    )


def _counter():
    return sensor.sensor_schema(
        accuracy_decimals=0,
        state_class=STATE_CLASS_TOTAL_INCREASING,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        icon="mdi:counter",
    )


SENSORS = {
    CONF_TEMPERATURE: _temp("mdi:thermometer"),
    CONF_POWER_FAIL_TEMPERATURE: _temp("mdi:thermometer-alert"),
    CONF_LATENCY_MEDIAN: _latency(),
    CONF_LATENCY_MAX: _latency(),
    CONF_CRC_ERRORS: _counter(),
    CONF_ECHO_MISMATCHES: _counter(),
    CONF_MISSED_SLOTS: _counter(),
}

CONFIG_SCHEMA = FREEZER_BUS_CHILD_SCHEMA.extend(
    {cv.Optional(key): schema for key, schema in SENSORS.items()}
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_FREEZER_BUS_ID])
    for key in SENSORS:
        if key in config:
            var = await sensor.new_sensor(config[key])
            cg.add(getattr(parent, f"set_{key}_sensor")(var))
