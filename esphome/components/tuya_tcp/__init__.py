import esphome.codegen as cg
from esphome.components import tuya
from esphome.config_helpers import filter_source_files_from_platform
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
    PLATFORM_LN882X,
    PLATFORM_RP2040,
    PLATFORM_RTL87XX,
    PlatformFramework,
)
from esphome.core import CORE
import esphome.final_validate as fv
from esphome.helpers import IS_MACOS

CODEOWNERS = ["@dwmw2"]
DOMAIN = "tuya_tcp"

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
        tuya.TUYA_SCHEMA.extend(
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

# ESP8266's lwip_raw_tcp cannot make outbound connections
PLATFORMS = [
    PLATFORM_ESP32,
    PLATFORM_BK72XX,
    PLATFORM_RTL87XX,
    PLATFORM_LN882X,
    PLATFORM_RP2040,
    PLATFORM_HOST,
]


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
    cg.add_library(
        "tuyapp",
        None,
        "https://github.com/dwmw2/tuyapp.git#0c28168db111fe2a6c0aa229bb2eaddd4871f76d",
    )

    # Crypto backend: OpenSSL on host (the library's default), BearSSL via
    # the USE_EXTERNAL_* primitives on RP2040, mbedtls everywhere else
    if CORE.is_host:
        if IS_MACOS:
            # macOS needs special handling for Homebrew OpenSSL
            cg.add_build_flag("-I/opt/homebrew/opt/openssl/include")
            cg.add_build_flag("-L/opt/homebrew/opt/openssl/lib")
        cg.add_build_flag("-lcrypto")
    elif CORE.is_rp2040:
        # The RP2040 Arduino core ships BearSSL; tuyapp_bearssl.cpp provides
        # the ciphers and hashes from it
        for primitive in ("AES_ECB", "AES_GCM", "HMAC_SHA256", "MD5"):
            cg.add_build_flag(f"-DUSE_EXTERNAL_{primitive}")
    else:
        cg.add_build_flag("-DUSE_MBEDTLS")
        # Enable GCM support in LibreTiny's mbedtls (except LN882X which has it)
        if CORE.is_libretiny and not CORE.is_ln882x:
            cg.add_build_flag("-DMBEDTLS_GCM_C")
    # tuyapp_glue.cpp provides the RNG (the library's rand module reads
    # /dev/urandom, which embedded targets do not have) and CRC-32 (the
    # shared core crc32 helper: mask ROM on ESP32 instead of a 1 KiB table)
    cg.add_build_flag("-DUSE_EXTERNAL_RAND")
    cg.add_build_flag("-DUSE_EXTERNAL_CRC32")
    # The transport is our AsyncClient, not the library's sockets (which do
    # not even compile on embedded SDKs); tuyapp_glue.cpp has the stubs
    cg.add_build_flag("-DUSE_EXTERNAL_TCP")

    for conf in config:
        var = cg.new_Pvariable(conf[CONF_ID])
        await cg.register_component(var, conf)

        cg.add(var.set_address(str(conf[CONF_ADDRESS])))
        cg.add(var.set_port(conf[CONF_PORT]))
        if CONF_DEVICE_ID in conf:
            cg.add(var.set_device_id(conf[CONF_DEVICE_ID]))
        if CONF_KEY in conf:
            cg.add(var.set_key(conf[CONF_KEY]))

        # Emit direct protocol class instantiation instead of using the factory
        version_str = conf.get(CONF_VERSION, "3.3")
        if not isinstance(version_str, str):
            # It's a Protocol enum, find the string
            for ver_str, ver_enum in PROTOCOL_VERSIONS.items():
                if ver_enum == version_str:
                    version_str = ver_str
                    break

        protocol_class = f"tuyaAPI{version_str.replace('.', '')}"
        cg.add_global(cg.RawStatement(f'#include "{protocol_class}.hpp"'))
        cg.add(
            var.set_tuya_api(
                cg.RawExpression(
                    f"new esphome::tuya_tcp::BridgedAPI<{protocol_class}>({conf[CONF_ID]})"
                )
            )
        )
        cg.add(var.set_version(PROTOCOL_VERSIONS[version_str]))

        await tuya.setup_tuya(var, conf)


FILTER_SOURCE_FILES = filter_source_files_from_platform(
    {"tuyapp_bearssl.cpp": {PlatformFramework.RP2_ARDUINO}}
)
