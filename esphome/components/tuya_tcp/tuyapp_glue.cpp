#include "tuyaTCP.hpp"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "esphome/components/crc32/crc32.h"

// Declarations of the primitives the tuyapp library expects the application
// to provide under its USE_EXTERNAL_* defines
#include "crypt/rand.hpp"
#include "crypt/crc32.hpp"

namespace Tuya {

// The platform RNG: embedded targets have no /dev/urandom for the library's
// own rand module to read
void random_bytes(unsigned char *buffer, int len) { esphome::random_bytes(buffer, (size_t) len); }

// The shared crc32 component: the mask ROM on ESP32 instead of a 1 KiB table
unsigned long crc32(unsigned long crc, uint8_t const data[], uint32_t const len) {
  return esphome::crc32::crc32((uint32_t) crc, data, len);
}

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
