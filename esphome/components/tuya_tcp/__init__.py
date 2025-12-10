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
    PLATFORM_BK72XX,
    PLATFORM_ESP32,
    PLATFORM_HOST,
    PLATFORM_RTL87XX,
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


CONFIG_SCHEMA = cv.All(
    cv.ensure_list(
        tuya.BASE_SCHEMA.extend(
            {
                cv.GenerateID(): cv.declare_id(TuyaTCP),
                cv.Required(CONF_ADDRESS): validate_address,
                cv.Optional(CONF_PORT, default=6668): cv.port,
                cv.Optional(CONF_DEVICE_ID): cv.string,
                cv.Optional(CONF_KEY): cv.string,
                cv.Optional(CONF_VERSION, default="3.3"): cv.enum(PROTOCOL_VERSIONS),
            }
        ).extend(cv.COMPONENT_SCHEMA)
    ),
)

# Support ESP32, LibreTiny (BK72XX, RTL87XX), and host platforms
# ESP8266 lwip_raw_tcp implementation doesn't support outbound connections (no connect/select)
PLATFORMS = [PLATFORM_ESP32, PLATFORM_BK72XX, PLATFORM_RTL87XX, PLATFORM_HOST]


def _final_validate(config):
    """Validate IPv6 address requires IPv6 to be enabled."""
    for conf in config:
        address = conf[CONF_ADDRESS]
        if ":" in str(address):
            # IPv6 address - check if IPv6 is enabled
            network_config = fv.full_config.get().get("network", {})
            if not network_config.get("enable_ipv6", False):
                raise cv.Invalid(
                    f"IPv6 address '{address}' requires 'enable_ipv6: true' in network configuration"
                )


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    import pathlib

    # Add tuyapp src directory to include path for crypt/crc32.hpp
    component_dir = pathlib.Path(__file__).parent
    tuyapp_src = component_dir / ".." / ".." / ".." / ".." / "tuyapp" / "src"
    if tuyapp_src.exists():
        cg.add_build_flag(f"-I{tuyapp_src.resolve()}")

    # Add crypto library dependencies
    if CORE.is_host:
        cg.add_build_flag("-lcrypto")
        cg.add_build_flag("-lz")
    elif CORE.is_libretiny:
        # Enable GCM support in LibreTiny's mbedtls
        cg.add_build_flag("-DMBEDTLS_GCM_C")

    for conf in config:
        var = cg.new_Pvariable(conf[CONF_ID])
        await cg.register_component(var, conf)

        cg.add(var.set_address(str(conf[CONF_ADDRESS])))
        cg.add(var.set_port(conf[CONF_PORT]))
        if CONF_DEVICE_ID in conf:
            cg.add(var.set_device_id(conf[CONF_DEVICE_ID]))
        if CONF_KEY in conf:
            cg.add(var.set_key(conf[CONF_KEY]))

        # Emit direct protocol class instantiation instead of using factory
        version_str = conf.get(CONF_VERSION, "3.3")
        if not isinstance(version_str, str):
            # It's a Protocol enum, find the string
            for ver_str, ver_enum in PROTOCOL_VERSIONS.items():
                if ver_enum == version_str:
                    version_str = ver_str
                    break

        protocol_class = f"tuyaAPI{version_str.replace('.', '')}"
        cg.add_global(
            cg.RawStatement(
                f'#include "esphome/components/tuya_tcp/{protocol_class}.hpp"'
            )
        )
        cg.add(var.set_tuya_api(cg.RawExpression(f"new {protocol_class}()")))

        await tuya.register_tuya(var, conf)


def FILTER_SOURCE_FILES() -> list[str]:
    # Determine which crypto implementation to use
    if CORE.is_host:
        crypto_impl = "tuyaAPI-crypto.cpp"
    elif CORE.is_esp8266:
        crypto_impl = "tuyaAPI-bearssl.cpp"
    else:  # ESP32 (Arduino and ESP-IDF) and LibreTiny (both have mbedtls)
        crypto_impl = "tuyaAPI-mbedtls.cpp"

    # Return list of implementations to exclude
    all_impls = [
        "tuyaAPI-crypto.cpp",
        "tuyaAPI-mbedtls.cpp",
        "tuyaAPI-bearssl.cpp",
        "tuyaAPI-arduino.cpp",
    ]
    all_impls.remove(crypto_impl)
    return all_impls
