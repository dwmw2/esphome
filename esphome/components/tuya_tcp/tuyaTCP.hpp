#pragma once

#include <cstdint>

// Empty base class for tuyaAPI to inherit from.
// The send() and receive() methods are required because tuyaAPI::NegotiateSession()
// calls them, even though we don't use NegotiateSession() in ESPHome (we use the
// async BuildSessionMessage/DecodeSessionMessage methods instead).
//
// In an ideal world, this would not exist. The tuyapp tuyaTCP class should be a
// parent object which has a tuyaAPI member, not a base class. Then NegotiateSession()
// could live in tuyaTCP where it belongs, with send/receive implemented there.
class tuyaTCP {
 public:
  int send(const uint8_t *data, int len) { return -1; }
  int receive(uint8_t *data, int len, int timeout_ms) { return -1; }
};
