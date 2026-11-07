#include "tuyapp_glue.h"
#include "tuyaTCP.hpp"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "esphome/components/crc32/crc32.h"

// Declarations of the primitives the tuyapp library expects the application
// to provide under its USE_EXTERNAL_* defines
#include "crypt/rand.hpp"
#include "crypt/crc32.hpp"
#ifdef USE_RP2
#include "crypt/aes_128_ecb.hpp"
#include "crypt/aes_128_gcm.hpp"
#include "crypt/hmac_sha256.hpp"
#include "crypt/md5.hpp"

#include <bearssl/bearssl.h>
#include <cstring>
#endif

namespace esphome::tuya_tcp {

void tuyapp_glue_anchor() {}

}  // namespace esphome::tuya_tcp

namespace Tuya {

// The platform RNG: embedded targets have no /dev/urandom for the library's
// own rand module to read
void random_bytes(unsigned char *buffer, int len) { esphome::random_bytes(buffer, (size_t) len); }

// The shared crc32 component: the mask ROM on ESP32 instead of a 1 KiB table
unsigned long crc32(unsigned long crc, uint8_t const data[], uint32_t const len) {
  return esphome::crc32::crc32((uint32_t) crc, data, len);
}

#ifdef USE_RP2
// The RP2040 Arduino core ships BearSSL rather than mbedtls or OpenSSL, so
// provide the ciphers and hashes from it too

bool aes_128_ecb_encrypt(const unsigned char *cEncryptionKey, const unsigned char *cInputBuffer, int inputSize,
                         unsigned char *cOutputBuffer, int *outputSize) {
  uint8_t padding = 16 - (inputSize % 16);
  int paddedInputSize = inputSize + padding;

  unsigned char cPaddedInput[paddedInputSize];
  memcpy(cPaddedInput, cInputBuffer, inputSize);
  memset(cPaddedInput + inputSize, padding, padding);

  // BearSSL has no ECB interface; CBC over a single block with a zero IV is
  // ECB of that block
  br_aes_ct_cbcenc_keys ctx;
  br_aes_ct_cbcenc_init(&ctx, cEncryptionKey, 16);
  *outputSize = 0;
  for (int i = 0; i < paddedInputSize; i += 16) {
    unsigned char iv[16] = {0};
    memcpy(cOutputBuffer + i, cPaddedInput + i, 16);
    br_aes_ct_cbcenc_run(&ctx, iv, cOutputBuffer + i, 16);
    *outputSize += 16;
  }
  return true;
}

bool aes_128_ecb_decrypt(const unsigned char *cEncryptionKey, const unsigned char *cInputBuffer, int inputSize,
                         unsigned char *cOutputBuffer, int *outputSize) {
  br_aes_ct_cbcdec_keys ctx;
  br_aes_ct_cbcdec_init(&ctx, cEncryptionKey, 16);
  *outputSize = 0;
  for (int i = 0; i < inputSize; i += 16) {
    unsigned char iv[16] = {0};
    memcpy(cOutputBuffer + i, cInputBuffer + i, 16);
    br_aes_ct_cbcdec_run(&ctx, iv, cOutputBuffer + i, 16);
    *outputSize += 16;
  }

  //  trim padding chars from decrypted payload
  uint8_t padding = cOutputBuffer[inputSize - 1];
  if (padding <= 16) {
    cOutputBuffer[inputSize - padding] = 0;
    *outputSize -= padding;
  }
  return true;
}

bool aes_128_gcm_encrypt(const unsigned char *cEncryptionKey, const unsigned char *iv, int iv_len,
                         const unsigned char *aad, int aad_len, const unsigned char *cInputBuffer, int inputSize,
                         unsigned char *cOutputBuffer, int *outputSize, unsigned char *tag, int tag_len) {
  br_aes_ct_ctr_keys aes_ctx;
  br_gcm_context gcm_ctx;

  br_aes_ct_ctr_init(&aes_ctx, cEncryptionKey, 16);
  br_gcm_init(&gcm_ctx, &aes_ctx.vtable, br_ghash_ctmul32);

  br_gcm_reset(&gcm_ctx, iv, iv_len);
  br_gcm_aad_inject(&gcm_ctx, aad, aad_len);
  br_gcm_flip(&gcm_ctx);
  memcpy(cOutputBuffer, cInputBuffer, inputSize);
  br_gcm_run(&gcm_ctx, 1, cOutputBuffer, inputSize);
  br_gcm_get_tag(&gcm_ctx, tag);

  *outputSize = inputSize;
  return true;
}

bool aes_128_gcm_decrypt(const unsigned char *cEncryptionKey, const unsigned char *iv, int iv_len,
                         const unsigned char *aad, int aad_len, const unsigned char *cInputBuffer, int inputSize,
                         const unsigned char *tag, int tag_len, unsigned char *cOutputBuffer, int *outputSize) {
  br_aes_ct_ctr_keys aes_ctx;
  br_gcm_context gcm_ctx;

  br_aes_ct_ctr_init(&aes_ctx, cEncryptionKey, 16);
  br_gcm_init(&gcm_ctx, &aes_ctx.vtable, br_ghash_ctmul32);

  br_gcm_reset(&gcm_ctx, iv, iv_len);
  br_gcm_aad_inject(&gcm_ctx, aad, aad_len);
  br_gcm_flip(&gcm_ctx);
  memcpy(cOutputBuffer, cInputBuffer, inputSize);
  br_gcm_run(&gcm_ctx, 0, cOutputBuffer, inputSize);

  unsigned char computed_tag[16];
  br_gcm_get_tag(&gcm_ctx, computed_tag);
  if (memcmp(tag, computed_tag, tag_len) != 0)
    return false;

  *outputSize = inputSize;
  return true;
}

void hmac_sha256(const unsigned char *cEncryptionKey, int keySize, const unsigned char *cInputBuffer, int inputSize,
                 unsigned char *cOutputBuffer) {
  br_hmac_key_context kc;
  br_hmac_context ctx;

  br_hmac_key_init(&kc, &br_sha256_vtable, cEncryptionKey, keySize);
  br_hmac_init(&ctx, &kc, 0);
  br_hmac_update(&ctx, cInputBuffer, inputSize);
  br_hmac_out(&ctx, cOutputBuffer);
}

void md5_hash(const unsigned char *cInputBuffer, int inputSize, unsigned char *cOutputBuffer) {
  br_md5_context ctx;
  br_md5_init(&ctx);
  br_md5_update(&ctx, cInputBuffer, inputSize);
  br_md5_out(&ctx, cOutputBuffer);
}

#endif  // USE_RP2

}  // namespace Tuya

// With USE_EXTERNAL_TCP the library compiles none of its socket transport and
// the application defines the tuyaTCP members the link needs. The component
// talks to the device through AsyncClient: sends route through the virtual
// send() override in BridgedAPI, so these base definitions only exist to
// satisfy the vtable, the constructor, and the deprecated blocking
// NegotiateSession() path, and none of them is ever meant to run.

tuyaTCP::tuyaTCP() {
  this->m_sockfd = -1;
  this->m_lasterror = 0;
  this->m_socketState = ::Tuya::TCP::Socket::DISCONNECTED;
  this->m_asyncMode = false;
}

tuyaTCP::~tuyaTCP() {}

bool tuyaTCP::ConnectToDevice(const std::string &hostname) {
  this->m_socketState = ::Tuya::TCP::Socket::NO_SOCK_AVAIL;
  return false;
}

int tuyaTCP::send(unsigned char *buffer, const int size) { return -1; }

int tuyaTCP::receive(unsigned char *buffer, const int maxsize, const int minsize) { return -1; }
