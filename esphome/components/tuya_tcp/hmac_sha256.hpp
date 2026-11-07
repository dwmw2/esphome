/*
 *  Client interface for local Tuya device access
 *
 *  Message authentication code module
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

#ifdef USE_OPENSSL
// must be included in global namespace
#include <openssl/hmac.h>
#endif

#ifdef USE_MBEDTLS
// must be included in global namespace
#include <mbedtls/md.h>
#endif

#ifdef USE_BEARSSL
// must be included in global namespace
#include <bearssl/bearssl.h>
#endif

namespace Tuya {

#ifdef USE_OPENSSL

static void hmac_sha256(const unsigned char *cEncryptionKey, int keySize, const unsigned char *cInputBuffer,
                        int inputSize, unsigned char *cOutputBuffer) {
  unsigned int len;
  HMAC(EVP_sha256(), cEncryptionKey, keySize, cInputBuffer, inputSize, cOutputBuffer, &len);
}

#endif  // USE_OPENSSL

#ifdef USE_MBEDTLS

static void hmac_sha256(const unsigned char *cEncryptionKey, int keySize, const unsigned char *cInputBuffer,
                        int inputSize, unsigned char *cOutputBuffer) {
  const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_hmac(md_info, cEncryptionKey, keySize, cInputBuffer, inputSize, cOutputBuffer);
}

#endif  // USE_MBEDTLS

#ifdef USE_BEARSSL

static void hmac_sha256(const unsigned char *cEncryptionKey, int keySize, const unsigned char *cInputBuffer,
                        int inputSize, unsigned char *cOutputBuffer) {
  br_hmac_key_context kc;
  br_hmac_context ctx;

  br_hmac_key_init(&kc, &br_sha256_vtable, cEncryptionKey, keySize);
  br_hmac_init(&ctx, &kc, 0);
  br_hmac_update(&ctx, cInputBuffer, inputSize);
  br_hmac_out(&ctx, cOutputBuffer);
}

#endif  // USE_BEARSSL

};  // namespace Tuya
