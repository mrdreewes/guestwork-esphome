"""GuestWork mini-split on Wi-Fi for the ESP8266 (docs/direct-minisplits.md).

ESPHome's own `mqtt:` has no TLS on the ESP8266 (its async client needs axTLS, gone from the Arduino core), so this
component makes the board's one connection itself: MQTT over BearSSL, checked against GuestWork's device CA, with
the same topics and payloads ESPHome's mqtt climate uses. GuestWork's broker can't tell the two apart.
"""

import time

import esphome.codegen as cg
from esphome.components import climate, switch
import esphome.config_validation as cv
from esphome.const import (
    CONF_BROKER,
    CONF_CERTIFICATE_AUTHORITY,
    CONF_ID,
    CONF_PASSWORD,
    CONF_PORT,
    CONF_TOPIC_PREFIX,
    CONF_USERNAME,
)

DEPENDENCIES = ["wifi"]
CODEOWNERS = []

CONF_CLIMATE_ID = "climate_id"
CONF_BEEPER_ID = "beeper_id"

gw_direct_ns = cg.esphome_ns.namespace("gw_direct")
GwDirect = gw_direct_ns.class_("GwDirect", cg.Component)


def _pem(value):
    value = cv.string_strict(value)
    if "-----BEGIN CERTIFICATE-----" not in value:
        raise cv.Invalid("certificate_authority must be a PEM certificate")
    return value


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(GwDirect),
            cv.Required(CONF_BROKER): cv.domain,
            cv.Required(CONF_PORT): cv.port,
            cv.Required(CONF_USERNAME): cv.string_strict,
            cv.Required(CONF_PASSWORD): cv.string_strict,
            cv.Required(CONF_TOPIC_PREFIX): cv.publish_topic,
            cv.Required(CONF_CERTIFICATE_AUTHORITY): _pem,
            cv.Required(CONF_CLIMATE_ID): cv.use_id(climate.Climate),
            cv.Optional(CONF_BEEPER_ID): cv.use_id(switch.Switch),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp8266,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_broker(config[CONF_BROKER], config[CONF_PORT]))
    # The client id IS the username: GuestWork's broker refuses any other.
    cg.add(var.set_login(config[CONF_USERNAME], config[CONF_PASSWORD]))
    cg.add(var.set_topic_prefix(config[CONF_TOPIC_PREFIX]))
    cg.add(var.set_ca(config[CONF_CERTIFICATE_AUTHORITY]))
    # The earliest moment the board may believe it is (until the network's clock answers): certificates are checked
    # against the time, and an ESP8266 boots in 1970.
    cg.add(var.set_build_time(int(time.time())))
    cg.add(var.set_climate(await cg.get_variable(config[CONF_CLIMATE_ID])))
    if CONF_BEEPER_ID in config:
        cg.add(var.set_beeper(await cg.get_variable(config[CONF_BEEPER_ID])))
    cg.add_library("knolleary/PubSubClient", "2.8")
