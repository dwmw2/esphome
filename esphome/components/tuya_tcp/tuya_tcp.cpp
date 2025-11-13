#include "tuya_tcp.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/components/network/util.h"

namespace esphome {
namespace tuya_tcp {

static const char *const TAG = "tuya_tcp";

void TuyaTCP::setup() {
  tuya_api_ = tuyaAPI::create(version_);
  if (!tuya_api_) {
    ESP_LOGE(TAG, "Failed to create Tuya API");
    mark_failed();
  }
}

void TuyaTCP::loop() {
#if !defined(USE_ESP32) && !defined(USE_ARDUINO)
  // Only socket-based AsyncClient needs manual loop() polling
  if (client_)
    client_->loop();
#endif

  if (state_ == State::DISCONNECTED && network::is_connected()) {
    uint32_t now = millis();
    if (now - last_connect_attempt_ >= 10000) {
      last_connect_attempt_ = now;
      connect_();
    }
  }

  if (state_ == State::NEGOTIATING && millis() - negotiation_start_ > 5000) {
    ESP_LOGW(TAG, "Negotiation timeout");
    disconnect_();
  }

  if (state_ == State::CONNECTED && millis() - last_rx_time_ > 5000) {
    send_heartbeat_();
  }

  tuya::Tuya::loop();
}

void TuyaTCP::disconnect_() {
  if (client_) {
    client_->close();
    client_.reset();
  }
  state_ = State::DISCONNECTED;
  initial_query_sent_ = false;
}

void TuyaTCP::start_negotiation_() {
  if (tuya_api_) {
    delete tuya_api_;
  }
  tuya_api_ = tuyaAPI::create(version_);
  if (!tuya_api_) {
    ESP_LOGE(TAG, "Failed to recreate Tuya API");
    return;
  }
  tuya_api_->SetEncryptionKey(key_);
}

void TuyaTCP::handle_negotiation_data_(const uint8_t *data, size_t len) {
  tuya_api_->DecodeSessionMessage(const_cast<uint8_t *>(data), len);

  if (tuya_api_->isSessionEstablished()) {
    ESP_LOGI(TAG, "Negotiation complete");
    state_ = State::CONNECTED;
    initial_query_sent_ = false;
    this->initialized_callback_.call();
    send_initial_query_();
    return;
  }

  pending_send_.resize(1024);
  int packet_size = tuya_api_->BuildSessionMessage(pending_send_.data());
  if (packet_size < 0) {
    ESP_LOGE(TAG, "Negotiation failed");
    disconnect_();
  } else if (packet_size > 0) {
    pending_send_.resize(packet_size);
    client_->write((const char *) pending_send_.data(), pending_send_.size());
    pending_send_.clear();

    if (tuya_api_->isSessionEstablished()) {
      ESP_LOGI(TAG, "Negotiation complete");
      state_ = State::CONNECTED;
      initial_query_sent_ = false;
      this->initialized_callback_.call();
      send_initial_query_();
    }
  }
}

void TuyaTCP::handle_connected_data_(const uint8_t *data, size_t len) {
  std::string decoded = tuya_api_->DecodeTuyaMessage(const_cast<uint8_t *>(data), len);
  if (!decoded.empty()) {
    ESP_LOGI(TAG, "Received: %s", decoded.c_str());
    parse_json_message_(decoded);
  }
}

void TuyaTCP::send_initial_query_() {
  if (initial_query_sent_)
    return;

  char payload[256];
  uint32_t now = time(nullptr);
  snprintf(payload, sizeof(payload), "{\"gwId\":\"%s\",\"devId\":\"%s\",\"uid\":\"%s\",\"t\":\"%u\"}",
           device_id_.c_str(), device_id_.c_str(), device_id_.c_str(), now);

  std::vector<uint8_t> message(1024);
  uint8_t command = (tuya_api_->getProtocol() >= tuyaAPI::Protocol::v35) ? TUYA_DP_QUERY_NEW : TUYA_DP_QUERY;
  int len = tuya_api_->BuildTuyaMessage(message.data(), command, std::string(payload));
  if (len > 0) {
    client_->write((const char *) message.data(), len);
    ESP_LOGI(TAG, "Sent DP query");
    initial_query_sent_ = true;
  }
}

void TuyaTCP::send_heartbeat_() {
  if (state_ != State::CONNECTED || !client_)
    return;

  std::vector<uint8_t> message(1024);
  int len = tuya_api_->BuildTuyaMessage(message.data(), TUYA_HEART_BEAT, "");
  if (len > 0) {
    client_->write((const char *) message.data(), len);
    ESP_LOGV(TAG, "Sent heartbeat");
    last_rx_time_ = millis();
  }
}
void TuyaTCP::send_datapoint_command(uint8_t datapoint_id, tuya::TuyaDatapointType datapoint_type,
                                     const std::vector<uint8_t> &data) {
  if (!client_ || state_ != State::CONNECTED) {
    ESP_LOGW(TAG, "Cannot send command: not connected");
    return;
  }

  char payload[512];
  uint32_t now = time(nullptr);

  std::string dps_value;
  switch (datapoint_type) {
    case tuya::TuyaDatapointType::BOOLEAN:
      dps_value = data[0] ? "true" : "false";
      break;
    case tuya::TuyaDatapointType::INTEGER: {
      uint32_t val = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
      char buf[16];
      snprintf(buf, sizeof(buf), "%u", val);
      dps_value = buf;
      break;
    }
    case tuya::TuyaDatapointType::STRING:
      dps_value = "\"";
      for (auto c : data)
        dps_value += c;
      dps_value += "\"";
      break;
    case tuya::TuyaDatapointType::ENUM:
      dps_value = std::to_string(data[0]);
      break;
    default:
      ESP_LOGW(TAG, "Unsupported datapoint type: %d", (int) datapoint_type);
      return;
  }

  snprintf(payload, sizeof(payload), "{\"protocol\":5,\"t\":%u,\"data\":{\"dps\":{\"%d\":%s}}}", now, datapoint_id,
           dps_value.c_str());

  std::vector<uint8_t> message(1024);
  int len = tuya_api_->BuildTuyaMessage(message.data(), TUYA_CONTROL_NEW, std::string(payload));
  if (len > 0) {
    client_->write((const char *) message.data(), len);
    ESP_LOGD(TAG, "Sent control command for DP %d", datapoint_id);
  }
}

void TuyaTCP::connect_() {
  client_ = std::make_unique<AsyncClient>();

  client_->onConnect([this](void *arg, AsyncClient *c) {
    ESP_LOGI(TAG, "Connected!");
    start_negotiation_();

    pending_send_.resize(1024);
    int packet_size = tuya_api_->BuildSessionMessage(pending_send_.data());
    if (packet_size < 0) {
      ESP_LOGE(TAG, "Failed to build negotiation packet");
      disconnect_();
    } else if (packet_size == 0) {
      ESP_LOGI(TAG, "No negotiation needed");
      pending_send_.clear();
      state_ = State::CONNECTED;
      initial_query_sent_ = false;
      last_rx_time_ = millis();
      send_initial_query_();
    } else {
      pending_send_.resize(packet_size);
      negotiation_start_ = millis();
      state_ = State::NEGOTIATING;
      c->write((const char *) pending_send_.data(), pending_send_.size());
      pending_send_.clear();
    }
  });

  client_->onDisconnect([this](void *arg, AsyncClient *c) {
    ESP_LOGI(TAG, "Disconnected");
    disconnect_();
  });

  client_->onError([this](void *arg, AsyncClient *c, int8_t error) {
    ESP_LOGW(TAG, "Connection error: %d", error);
    disconnect_();
  });

  client_->onData([this](void *arg, AsyncClient *c, void *data, size_t len) {
    last_rx_time_ = millis();
    if (state_ == State::NEGOTIATING) {
      handle_negotiation_data_((const uint8_t *) data, len);
    } else if (state_ == State::CONNECTED) {
      handle_connected_data_((const uint8_t *) data, len);
    }
  });

  if (address_.find(':') != std::string::npos) {
    ESP_LOGI(TAG, "Connecting to [%s]:%d...", address_.c_str(), port_);
  } else {
    ESP_LOGI(TAG, "Connecting to %s:%d...", address_.c_str(), port_);
  }
  state_ = State::CONNECTING;
  if (!client_->connect(address_.c_str(), port_)) {
    ESP_LOGE(TAG, "Connect failed");
    disconnect_();
  }
}

void TuyaTCP::dump_config() {
  ESP_LOGCONFIG(TAG, "Tuya TCP:");
  ESP_LOGCONFIG(TAG, "  Address: %s", address_.c_str());
  ESP_LOGCONFIG(TAG, "  Port: %d", (int) port_);
  ESP_LOGCONFIG(TAG, "  Device ID: %s", device_id_.c_str());
  const char *version_str = "unknown";
  switch (version_) {
    case tuyaAPI::Protocol::v31:
      version_str = "3.1";
      break;
    case tuyaAPI::Protocol::v33:
      version_str = "3.3";
      break;
    case tuyaAPI::Protocol::v34:
      version_str = "3.4";
      break;
    case tuyaAPI::Protocol::v35:
      version_str = "3.5";
      break;
  }
  ESP_LOGCONFIG(TAG, "  Version: %s", version_str);
  tuya::Tuya::dump_config();
}

void TuyaTCP::parse_json_message_(const std::string &json) {
  size_t dps_pos = json.find("\"dps\":");
  if (dps_pos == std::string::npos) {
    ESP_LOGW(TAG, "No 'dps' field in JSON");
    return;
  }

  size_t start = json.find('{', dps_pos);
  if (start == std::string::npos)
    return;

  size_t pos = start + 1;
  while (pos < json.length() && json[pos] != '}') {
    size_t key_start = json.find('"', pos);
    if (key_start == std::string::npos || key_start >= json.length())
      break;
    size_t key_end = json.find('"', key_start + 1);
    if (key_end == std::string::npos)
      break;

    std::string key = json.substr(key_start + 1, key_end - key_start - 1);
    uint8_t dp_id = atoi(key.c_str());

    size_t colon = json.find(':', key_end);
    if (colon == std::string::npos)
      break;

    pos = colon + 1;
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t'))
      pos++;

    if (json[pos] == '"') {
      size_t val_end = json.find('"', pos + 1);
      std::string value = json.substr(pos + 1, val_end - pos - 1);
      inject_datapoint_(dp_id, tuya::TuyaDatapointType::STRING, value);
      pos = val_end + 1;
    } else if (json.substr(pos, 4) == "true" || json.substr(pos, 5) == "false") {
      bool value = json.substr(pos, 4) == "true";
      inject_datapoint_(dp_id, tuya::TuyaDatapointType::BOOLEAN, value ? "1" : "0");
      pos += value ? 4 : 5;
    } else {
      size_t val_end = pos;
      while (val_end < json.length() && (isdigit(json[val_end]) || json[val_end] == '-' || json[val_end] == '.')) {
        val_end++;
      }
      std::string value = json.substr(pos, val_end - pos);
      inject_datapoint_(dp_id, tuya::TuyaDatapointType::INTEGER, value);
      pos = val_end;
    }

    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t'))
      pos++;
    if (pos >= json.length() || json[pos] == '}')
      break;
    if (json[pos] == ',')
      pos++;
  }
}

void TuyaTCP::inject_datapoint_(uint8_t id, tuya::TuyaDatapointType type, const std::string &value) {
  tuya::TuyaDatapoint datapoint{};
  datapoint.id = id;
  datapoint.type = type;

  switch (type) {
    case tuya::TuyaDatapointType::BOOLEAN:
      datapoint.len = 1;
      datapoint.value_bool = (value == "1" || value == "true");
      break;
    case tuya::TuyaDatapointType::INTEGER: {
      datapoint.len = 4;
      int32_t val = atoi(value.c_str());
      datapoint.value_int = val;
      datapoint.value_uint = val;
      break;
    }
    case tuya::TuyaDatapointType::STRING:
      datapoint.len = value.length();
      datapoint.value_string = value;
      break;
    case tuya::TuyaDatapointType::ENUM:
      datapoint.len = 1;
      datapoint.value_enum = atoi(value.c_str());
      break;
    default:
      return;
  }

  this->handle_datapoint_(datapoint);
}

}  // namespace tuya_tcp
}  // namespace esphome
