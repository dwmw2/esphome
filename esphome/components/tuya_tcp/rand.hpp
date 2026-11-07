/*
 *  Client interface for local Tuya device access
 *
 *  Random bytes sequence generator
 *
 *
 *  Copyright 2022-2026 - gordonb3 https://github.com/gordonb3/tuyapp
 *
 *  Licensed under GNU General Public License 3.0 or later.
 *  Some rights reserved. See COPYING, AUTHORS.
 *
 *  @license GPL-3.0+ <https://github.com/gordonb3/tuyapp/blob/master/LICENSE>
 */

#if !defined(USE_MBEDTLS) && !defined(USE_BEARSSL)

// select default encryption routines
#define USE_OPENSSL

#endif

#if defined(USE_OPENSSL) && !defined(USE_ESPHOME)

#include <openssl/rand.h>

namespace Tuya {
static void random_bytes(unsigned char *buffer, int len) { RAND_bytes(buffer, len); }
};  // namespace Tuya

#endif  // USE_OPENSSL

#if defined(USE_MBEDTLS) && !defined(USE_ESPHOME)

#include <fstream>  // must be included in global namespace

namespace Tuya {
static void random_bytes(unsigned char *buffer, int len) {
  std::fstream fr;
  fr.open("/dev/urandom", std::ios::in | std::ios::binary);
  fr.read((char *) buffer, len);
  fr.close();
}
};  // namespace Tuya

#endif  // USE_MBEDTLS

#ifdef USE_ESPHOME

// Vendored-copy deviation: embedded targets have no /dev/urandom (the mbedtls
// section would silently leave the buffer uninitialised) and BearSSL has no
// system RNG, so use the ESPHome platform RNG
#include "esphome/core/helpers.h"

namespace Tuya {
static void random_bytes(unsigned char *buffer, int len) { esphome::random_bytes(buffer, (size_t) len); }
};  // namespace Tuya

#endif  // USE_ESPHOME
