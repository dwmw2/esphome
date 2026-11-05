/*
 *  Crypto helper functions using mbedtls
 *
 *  Copyright 2022-2024 - gordonb3 https://github.com/gordonb3/tuyapp
 *
 *  Licensed under GNU General Public License 3.0 or later.
 *  Some rights reserved. See COPYING, AUTHORS.
 *
 *  @license GPL-3.0+ <https://github.com/gordonb3/tuyapp/blob/master/LICENSE>
 */

#include "tuyaAPI.hpp"
#include "esphome/core/helpers.h"
#ifdef USE_ESP32
#define MBEDTLS_AES_ALT
#include <aes_alt.h>
#include "mbedtls/esp_config.h"
// Map standard mbedtls constants to ESP32-specific ones
#define MBEDTLS_AES_ENCRYPT ESP_AES_ENCRYPT
#define MBEDTLS_AES_DECRYPT ESP_AES_DECRYPT
#else
#include <mbedtls/aes.h>
#endif
#include <mbedtls/md.h>
#include "mbedtls/gcm.h"
#include <cstring>

int tuyaAPI::aes_128_ecb_encrypt(const unsigned char *key, const unsigned char *input, int input_len,
                                 unsigned char *output, int *output_len) {
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, key, 128);

  // Add PKCS#7 padding
  int padding_len = 16 - (input_len % 16);
  int padded_len = input_len + padding_len;
  unsigned char padded_input[padded_len];
  memcpy(padded_input, input, input_len);
  memset(padded_input + input_len, padding_len, padding_len);

  *output_len = 0;
  // ECB mode processes 16-byte blocks
  for (int i = 0; i < padded_len; i += 16) {
    mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, padded_input + i, output + i);
    *output_len += 16;
  }

  mbedtls_aes_free(&ctx);
  return 0;
}

int tuyaAPI::aes_128_ecb_decrypt(const unsigned char *key, const unsigned char *input, int input_len,
                                 unsigned char *output, int *output_len) {
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_dec(&ctx, key, 128);

  *output_len = 0;
  // ECB mode processes 16-byte blocks
  for (int i = 0; i < input_len; i += 16) {
    mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_DECRYPT, input + i, output + i);
    *output_len += 16;
  }

  // Remove PKCS#7 padding
  if (*output_len > 0) {
    int padding_len = output[*output_len - 1];
    if (padding_len > 0 && padding_len <= 16) {
      *output_len -= padding_len;
    }
  }

  mbedtls_aes_free(&ctx);
  return 0;
}

void tuyaAPI::hmac_sha256(const unsigned char *key, int key_len, const unsigned char *data, int data_len,
                          unsigned char *output) {
  const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_hmac(md_info, key, key_len, data, data_len, output);
}

void tuyaAPI::md5_hash(const unsigned char *data, int data_len, unsigned char *output) {
  const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
  mbedtls_md(md_info, data, data_len, output);
}

void tuyaAPI::random_bytes(unsigned char *buffer, int len) { esphome::random_bytes(buffer, len); }

int tuyaAPI::aes_128_gcm_encrypt(const unsigned char *key, const unsigned char *iv, int iv_len,
                                 const unsigned char *aad, int aad_len, const unsigned char *input, int input_len,
                                 unsigned char *output, int *output_len, unsigned char *tag, int tag_len) {
  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);

  if (mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128) != 0) {
    mbedtls_gcm_free(&ctx);
    return -1;
  }

  if (mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, input_len, iv, iv_len, aad, aad_len, input, output, tag_len,
                                tag) != 0) {
    mbedtls_gcm_free(&ctx);
    return -1;
  }

  *output_len = input_len;
  mbedtls_gcm_free(&ctx);
  return 0;
}

int tuyaAPI::aes_128_gcm_decrypt(const unsigned char *key, const unsigned char *iv, int iv_len,
                                 const unsigned char *aad, int aad_len, const unsigned char *input, int input_len,
                                 const unsigned char *tag, int tag_len, unsigned char *output, int *output_len) {
  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);

  if (mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128) != 0) {
    mbedtls_gcm_free(&ctx);
    return -1;
  }

  if (mbedtls_gcm_auth_decrypt(&ctx, input_len, iv, iv_len, aad, aad_len, tag, tag_len, input, output) != 0) {
    mbedtls_gcm_free(&ctx);
    return -1;
  }

  *output_len = input_len;
  mbedtls_gcm_free(&ctx);
  return 0;
}
