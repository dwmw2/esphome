import esphome.codegen as cg
from esphome.components import tuya
import esphome.config_validation as cv
from esphome.const import (
    CONF_ADDRESS,
    CONF_DEVICE_ID,
    CONF_ID,
    CONF_KEY,
    CONF_PORT,
    CONF_VERSION,
    PLATFORM_ESP32,
    PLATFORM_HOST,
)
from esphome.core import CORE
import esphome.final_validate as fv

CODEOWNERS = ["@dwmw2"]

DEPENDENCIES = ["network"]
AUTO_LOAD = ["async_tcp", "tuya"]

tuya_ns = cg.esphome_ns.namespace("tuya")
Tuya = tuya_ns.class_("Tuya", cg.Component)

tuya_tcp_ns = cg.esphome_ns.namespace("tuya_tcp")
TuyaTCP = tuya_tcp_ns.class_("TuyaTCP", Tuya)

tuyaAPI_ns = cg.global_ns.class_("tuyaAPI")
Protocol = tuyaAPI_ns.enum("Protocol", is_class=True)
PROTOCOL_VERSIONS = {
    "3.1": Protocol.v31,
    "3.3": Protocol.v33,
    "3.4": Protocol.v34,
    "3.5": Protocol.v35,
}


def validate_address(value):
    """Validate address is IPv4 or IPv6."""
    # Check if it's an IPv6 address (contains ':')
    if ":" in value:
        return cv.ipv6address(value)
    return cv.ipv4address(value)


CONFIG_SCHEMA = tuya.BASE_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(TuyaTCP),
        cv.Required(CONF_ADDRESS): validate_address,
        cv.Optional(CONF_PORT, default=6668): cv.port,
        cv.Optional(CONF_DEVICE_ID): cv.string,
        cv.Optional(CONF_KEY): cv.string,
        cv.Optional(CONF_VERSION, default="3.3"): cv.enum(PROTOCOL_VERSIONS),
    }
).extend(cv.COMPONENT_SCHEMA)

# Support ESP32 and host platforms
# ESP8266 lwip_raw_tcp implementation doesn't support outbound connections (no connect/select)
PLATFORMS = [PLATFORM_ESP32, PLATFORM_HOST]


def _final_validate(config):
    """Validate IPv6 address requires IPv6 to be enabled."""
    address = config[CONF_ADDRESS]
    if ":" in str(address):
        # IPv6 address - check if IPv6 is enabled
        network_config = fv.full_config.get().get("network", {})
        if not network_config.get("enable_ipv6", False):
            raise cv.Invalid(
                f"IPv6 address '{address}' requires 'enable_ipv6: true' in network configuration"
            )


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_address(str(config[CONF_ADDRESS])))
    cg.add(var.set_port(config[CONF_PORT]))
    if CONF_DEVICE_ID in config:
        cg.add(var.set_device_id(config[CONF_DEVICE_ID]))
    if CONF_KEY in config:
        cg.add(var.set_key(config[CONF_KEY]))
    cg.add(var.set_version(config[CONF_VERSION]))

    await tuya.register_tuya(var, config)

    # Add crypto library for host builds
    if CORE.is_host:
        cg.add_build_flag("-lcrypto")
        cg.add_build_flag("-lz")


def FILTER_SOURCE_FILES() -> list[str]:
    # Determine which crypto implementation to use
    if CORE.is_host:
        crypto_impl = "tuyaAPI-crypto.cpp"
    elif CORE.is_esp8266:
        crypto_impl = "tuyaAPI-bearssl.cpp"
    else:  # ESP32 (Arduino and ESP-IDF)
        crypto_impl = "tuyaAPI-mbedtls.cpp"

    # Return list of implementations to exclude
    all_impls = ["tuyaAPI-crypto.cpp", "tuyaAPI-mbedtls.cpp", "tuyaAPI-bearssl.cpp"]
    all_impls.remove(crypto_impl)
    return all_impls
