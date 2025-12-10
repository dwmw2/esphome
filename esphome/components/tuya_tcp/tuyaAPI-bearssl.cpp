/*
 *  Crypto abstraction layer implementation using BearSSL
 *
 *  Copyright 2024 - David Woodhouse
 *
 *  Licensed under GNU General Public License 3.0 or later.
 */

#include "tuyaAPI.hpp"
#include "esphome/core/helpers.h"
#include <bearssl/bearssl.h>
#include <cstring>

int tuyaAPI::aes_128_ecb_encrypt(const unsigned char *key, const unsigned char *input, int input_len,
                                 unsigned char *output, int *output_len) {
  br_aes_ct_ctr_keys ctx;
  br_aes_ct_ctr_init(&ctx, key, 16);

  // Add PKCS#7 padding
  int padding_len = 16 - (input_len % 16);
  int padded_len = input_len + padding_len;
  uint8_t padded_input[padded_len];
  memcpy(padded_input, input, input_len);
  memset(padded_input + input_len, padding_len, padding_len);

  *output_len = 0;
  // ECB mode - encrypt each 16-byte block
  for (int i = 0; i < padded_len; i += 16) {
    uint8_t iv[16] = {0};
    br_aes_ct_ctr_run(&ctx, iv, 0, output + i, 16);
    for (int j = 0; j < 16; j++) {
      output[i + j] ^= padded_input[i + j];
    }
    *output_len += 16;
  }

  return 0;
}

int tuyaAPI::aes_128_ecb_decrypt(const unsigned char *key, const unsigned char *input, int input_len,
                                 unsigned char *output, int *output_len) {
  // BearSSL doesn't have direct ECB mode, use CTR mode with zero IV
  br_aes_ct_ctr_keys ctx;
  br_aes_ct_ctr_init(&ctx, key, 16);

  *output_len = 0;
  for (int i = 0; i < input_len; i += 16) {
    uint8_t iv[16] = {0};
    br_aes_ct_ctr_run(&ctx, iv, 0, output + i, 16);
    for (int j = 0; j < 16; j++) {
      output[i + j] ^= input[i + j];
    }
    *output_len += 16;
  }

  // Remove PKCS#7 padding
  if (*output_len > 0) {
    int padding_len = output[*output_len - 1];
    if (padding_len > 0 && padding_len <= 16) {
      *output_len -= padding_len;
    }
  }

  return 0;
}

void tuyaAPI::hmac_sha256(const unsigned char *key, int key_len, const unsigned char *data, int data_len,
                          unsigned char *output) {
  br_hmac_key_context kc;
  br_hmac_context ctx;

  br_hmac_key_init(&kc, &br_sha256_vtable, key, key_len);
  br_hmac_init(&ctx, &kc, 0);
  br_hmac_update(&ctx, data, data_len);
  br_hmac_out(&ctx, output);
}

void tuyaAPI::md5_hash(const unsigned char *data, int data_len, unsigned char *output) {
  br_md5_context ctx;
  br_md5_init(&ctx);
  br_md5_update(&ctx, data, data_len);
  br_md5_out(&ctx, output);
}

void tuyaAPI::random_bytes(unsigned char *buffer, int len) { esphome::random_bytes(buffer, len); }

int tuyaAPI::aes_128_gcm_encrypt(const unsigned char *key, const unsigned char *iv, int iv_len,
                                 const unsigned char *aad, int aad_len, const unsigned char *input, int input_len,
                                 unsigned char *output, int *output_len, unsigned char *tag, int tag_len) {
  br_aes_ct_ctr_keys aes_ctx;
  br_gcm_context gcm_ctx;

  br_aes_ct_ctr_init(&aes_ctx, key, 16);
  br_gcm_init(&gcm_ctx, &aes_ctx.vtable, br_ghash_ctmul32);

  br_gcm_reset(&gcm_ctx, iv, iv_len);
  br_gcm_aad_inject(&gcm_ctx, aad, aad_len);
  br_gcm_flip(&gcm_ctx);
  memcpy(output, input, input_len);
  br_gcm_run(&gcm_ctx, 1, output, input_len);
  br_gcm_get_tag(&gcm_ctx, tag);

  *output_len = input_len;
  return 0;
}

int tuyaAPI::aes_128_gcm_decrypt(const unsigned char *key, const unsigned char *iv, int iv_len,
                                 const unsigned char *aad, int aad_len, const unsigned char *input, int input_len,
                                 const unsigned char *tag, int tag_len, unsigned char *output, int *output_len) {
  br_aes_ct_ctr_keys aes_ctx;
  br_gcm_context gcm_ctx;

  br_aes_ct_ctr_init(&aes_ctx, key, 16);
  br_gcm_init(&gcm_ctx, &aes_ctx.vtable, br_ghash_ctmul32);

  br_gcm_reset(&gcm_ctx, iv, iv_len);
  br_gcm_aad_inject(&gcm_ctx, aad, aad_len);
  br_gcm_flip(&gcm_ctx);
  memcpy(output, input, input_len);
  br_gcm_run(&gcm_ctx, 0, output, input_len);

  uint8_t computed_tag[16];
  br_gcm_get_tag(&gcm_ctx, computed_tag);

  // Verify tag
  if (memcmp(tag, computed_tag, tag_len) != 0) {
    return -1;
  }

  *output_len = input_len;
  return 0;
}
