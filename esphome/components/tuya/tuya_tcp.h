#pragma once

#include "esphome/components/tuya/tuya.h"
#include "esphome/components/socket/socket.h"
#include "esphome/components/network/ip_address.h"
#include "esphome/core/component.h"
#include <string>

namespace esphome {
namespace tuya {

enum class State : uint8_t {
  DISCONNECTED = 0,
  CONNECTING,
  CONNECTED,
};

class TuyaTCP : public Tuya {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_address(const std::string &address) { address_ = address; }
  void set_port(uint16_t port) { port_ = port; }
  void set_device_id(const std::string &device_id) { device_id_ = device_id; }
  void set_key(const std::string &key) { key_ = key; }
  void set_version(const std::string &version) { version_ = version; }

 protected:
  void send_datapoint_command_(uint8_t datapoint_id, TuyaDatapointType datapoint_type,
                               std::vector<uint8_t> data) override;
  void connect_();
  void disconnect_();

  std::string address_;
  uint16_t port_{6668};
  std::string device_id_;
  std::string key_;
  std::string version_;
  std::unique_ptr<socket::Socket> socket_;
  State state_{State::DISCONNECTED};
  uint32_t last_connect_attempt_{0};
};

}  // namespace tuya
}  // namespace esphome
