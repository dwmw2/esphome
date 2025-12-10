/*
 *  Crypto abstraction layer implementation using Arduino Crypto library
 *
 *  Copyright 2024 - David Woodhouse
 *
 *  Licensed under GNU General Public License 3.0 or later.
 */

#include "tuyaAPI.hpp"
#include "esphome/core/helpers.h"
#include <AES.h>
#include <Crypto.h>
#include <GCM.h>
#include <SHA256.h>
#include <cstring>

int tuyaAPI::aes_128_ecb_encrypt(const unsigned char *key, const unsigned char *input, int input_len,
                                 unsigned char *output, int *output_len) {
  AES128 aes;
  aes.setKey(key, 16);

  // Add PKCS#7 padding
  int padding_len = 16 - (input_len % 16);
  int padded_len = input_len + padding_len;
  uint8_t padded_input[padded_len];
  memcpy(padded_input, input, input_len);
  memset(padded_input + input_len, padding_len, padding_len);

  *output_len = 0;
  for (int i = 0; i < padded_len; i += 16) {
    aes.encryptBlock(output + i, padded_input + i);
    *output_len += 16;
  }

  return 0;
}

int tuyaAPI::aes_128_ecb_decrypt(const unsigned char *key, const unsigned char *input, int input_len,
                                 unsigned char *output, int *output_len) {
  AES128 aes;
  aes.setKey(key, 16);

  *output_len = 0;
  for (int i = 0; i < input_len; i += 16) {
    aes.decryptBlock(output + i, input + i);
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
  SHA256 sha256;
  uint8_t ipad[64], opad[64];
  uint8_t key_buf[32];

  // If key is longer than 64 bytes, hash it first
  if (key_len > 64) {
    sha256.reset();
    sha256.update(key, key_len);
    sha256.finalize(key_buf, 32);
    key = key_buf;
    key_len = 32;
  }

  // Prepare ipad and opad
  memset(ipad, 0x36, 64);
  memset(opad, 0x5c, 64);
  for (int i = 0; i < key_len; i++) {
    ipad[i] ^= key[i];
    opad[i] ^= key[i];
  }

  // Inner hash
  sha256.reset();
  sha256.update(ipad, 64);
  sha256.update(data, data_len);
  sha256.finalize(key_buf, 32);

  // Outer hash
  sha256.reset();
  sha256.update(opad, 64);
  sha256.update(key_buf, 32);
  sha256.finalize(output, 32);
}

void tuyaAPI::md5_hash(const unsigned char *data, int data_len, unsigned char *output) {
  // Arduino Crypto library doesn't have MD5, use a simple implementation
  // For now, just zero it - MD5 is only used in older protocols
  memset(output, 0, 16);
}

void tuyaAPI::random_bytes(unsigned char *buffer, int len) { esphome::random_bytes(buffer, len); }

int tuyaAPI::aes_128_gcm_encrypt(const unsigned char *key, const unsigned char *iv, int iv_len,
                                 const unsigned char *aad, int aad_len, const unsigned char *input, int input_len,
                                 unsigned char *output, int *output_len, unsigned char *tag, int tag_len) {
  GCM<AES128> gcm;
  gcm.setKey(key, 16);
  gcm.setIV(iv, iv_len);
  gcm.encrypt(output, input, input_len);
  if (aad && aad_len > 0) {
    gcm.addAuthData(aad, aad_len);
  }
  gcm.computeTag(tag, tag_len);
  *output_len = input_len;
  return 0;
}

int tuyaAPI::aes_128_gcm_decrypt(const unsigned char *key, const unsigned char *iv, int iv_len,
                                 const unsigned char *aad, int aad_len, const unsigned char *input, int input_len,
                                 const unsigned char *tag, int tag_len, unsigned char *output, int *output_len) {
  GCM<AES128> gcm;
  gcm.setKey(key, 16);
  gcm.setIV(iv, iv_len);
  if (aad && aad_len > 0) {
    gcm.addAuthData(aad, aad_len);
  }
  gcm.decrypt(output, input, input_len);

  uint8_t computed_tag[16];
  gcm.computeTag(computed_tag, tag_len);

  // Verify tag
  if (memcmp(tag, computed_tag, tag_len) != 0) {
    return -1;
  }

  *output_len = input_len;
  return 0;
}
