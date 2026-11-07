#pragma once

#include "esphome/components/tuya/tuya.h"
#include "esphome/core/component.h"
#include "esphome/components/async_tcp/async_tcp.h"
#include "tuyaAPI.hpp"

namespace esphome::tuya_tcp {

enum class State : uint8_t {
  DISCONNECTED = 0,
  CONNECTING,
  NEGOTIATING,
  CONNECTED,
};

class TuyaTCP;

/// Routes the sends the tuyaAPI session negotiation makes internally into the
/// component's AsyncClient instead of the unused built-in tuyaTCP socket.
template<typename API> class BridgedAPI : public API {
 public:
  explicit BridgedAPI(TuyaTCP *component) : component_(component) {}
  int send(unsigned char *buffer, const int size) override;

 protected:
  TuyaTCP *component_;
};

class TuyaTCP : public tuya::Tuya {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_address(const std::string &address) { address_ = address; }
  void set_device_id(const std::string &device_id) { device_id_ = device_id; }
  void set_key(const std::string &key) { key_ = key; }
  void set_version(tuyaAPI::Protocol version) { version_ = version; }
  void set_tuya_api(tuyaAPI *api) { tuya_api_ = api; }

  /// Transport hook for BridgedAPI
  int transport_send(const unsigned char *buffer, int size);

 protected:
  void send_datapoint_command(uint8_t datapoint_id, tuya::TuyaDatapointType datapoint_type,
                              std::vector<uint8_t> data) override;

  void connect_();
  void disconnect_();
  void handle_negotiation_data_(const uint8_t *data, size_t len);
  void handle_connected_data_(const uint8_t *data, size_t len);
  void session_established_();
  bool send_message_(uint8_t command, const std::string &payload);
  void send_initial_query_();
  void parse_json_message_(const std::string &json);
  void inject_datapoint_(uint8_t id, tuya::TuyaDatapointType type, const std::string &value);

  std::string address_;
  std::string device_id_;
  std::string key_;
  tuyaAPI::Protocol version_{tuyaAPI::Protocol::v33};
  tuyaAPI *tuya_api_{nullptr};
  std::unique_ptr<AsyncClient> client_;
  State state_{State::DISCONNECTED};
  uint32_t last_connect_attempt_{0};
  uint32_t negotiation_start_{0};
  uint32_t last_rx_time_{0};
  bool initial_query_sent_{false};
};

template<typename API> int BridgedAPI<API>::send(unsigned char *buffer, const int size) {
  return this->component_->transport_send(buffer, size);
}

}  // namespace esphome::tuya_tcp
