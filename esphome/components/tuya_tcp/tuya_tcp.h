#pragma once

#include "esphome/components/tuya/tuya.h"
#include "esphome/components/socket/socket.h"
#include "esphome/components/network/ip_address.h"
#include "esphome/core/component.h"
#include "tuyaAPI.hpp"
#include <string>
#include <vector>

namespace esphome {
namespace tuya_tcp {

enum class State : uint8_t {
  DISCONNECTED = 0,
  CONNECTING,
  NEGOTIATING,
  CONNECTED,
};

class TuyaTCP : public tuya::Tuya {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_address(const std::string &address) { address_ = address; }
  void set_port(uint16_t port) { port_ = port; }
  void set_device_id(const std::string &device_id) { device_id_ = device_id; }
  void set_key(const std::string &key) { key_ = key; }
  void set_version(float version) { version_ = version; }

 protected:
  void send_datapoint_command(uint8_t datapoint_id, tuya::TuyaDatapointType datapoint_type,
                              const std::vector<uint8_t> &data) override;
  void connect_();
  void start_negotiation_();
  void disconnect_();
  void parse_json_message_(const std::string &json);
  void inject_datapoint_(uint8_t id, tuya::TuyaDatapointType type, const std::string &value);

  std::string address_;
  uint16_t port_{6668};
  std::string device_id_;
  std::string key_;
  float version_{3.3f};
  std::unique_ptr<socket::Socket> socket_{nullptr};
  std::string rx_buffer_;
  State state_{State::DISCONNECTED};
  uint32_t last_connect_attempt_{0};
  uint32_t negotiation_start_{0};
  uint32_t last_rx_time_{0};
  std::vector<uint8_t> pending_send_;
  bool initial_query_sent_{false};
  tuyaAPI *tuya_api_{nullptr};
};

}  // namespace tuya_tcp
}  // namespace esphome
