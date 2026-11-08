#include "tuya_tcp.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/components/network/util.h"

namespace esphome::tuya_tcp {

ESPHOME_LOG_TAG(TAG, "tuya_tcp");

static constexpr uint32_t RECONNECT_INTERVAL_MS = 10000;
static constexpr uint32_t NEGOTIATION_TIMEOUT_MS = 5000;
static constexpr uint32_t HEARTBEAT_INTERVAL_MS = 10000;
static constexpr size_t TX_BUFFER_SIZE = 1024;

void TuyaTCP::setup() {
  // tuya_api_ is set via set_tuya_api() before setup() is called
  if (this->tuya_api_ == nullptr) {
    this->mark_failed();
    return;
  }
}

void TuyaTCP::loop() {
#if !defined(USE_ESP32) && !defined(USE_ESP8266) && !defined(USE_RP2) && !defined(USE_LIBRETINY)
  // The socket-based AsyncClient implementation needs an explicit loop() call
  if (this->client_)
    this->client_->loop();
#endif
  const uint32_t now = App.get_loop_component_start_time();

  switch (this->state_) {
    case State::DISCONNECTED:
      if (network::is_connected() && now - this->last_connect_attempt_ >= RECONNECT_INTERVAL_MS) {
        this->last_connect_attempt_ = now;
        this->connect_();
      }
      break;
    case State::NEGOTIATING:
      this->process_rx_();
      if (this->state_ == State::NEGOTIATING && now - this->negotiation_start_ > NEGOTIATION_TIMEOUT_MS) {
        ESP_LOGW(TAG, "Session negotiation timed out");
        this->disconnect_();
      }
      break;
    case State::CONNECTED:
      this->process_rx_();
      if (now - this->last_rx_time_ > HEARTBEAT_INTERVAL_MS) {
        this->last_rx_time_ = now;
        ESP_LOGV(TAG, "Sending heartbeat");
        this->send_message_(TUYA_HEART_BEAT, this->tuya_api_->GeneratePayload(TUYA_HEART_BEAT, this->device_id_, ""));
      }
      break;
    default:
      break;
  }
}

int TuyaTCP::transport_send(const unsigned char *buffer, int size) {
  if (!this->client_ || !this->client_->connected())
    return -1;
  return (int) this->client_->write((const char *) buffer, size);
}

void TuyaTCP::connect_() {
  this->client_ = std::make_unique<AsyncClient>();

  this->client_->onConnect([this](void *arg, AsyncClient *c) {
    ESP_LOGI(TAG, "Connected");
    {
      LockGuard guard(this->rx_mutex_);
      this->rx_len_ = 0;
    }
    this->state_ = State::NEGOTIATING;
    this->negotiation_start_ = App.get_loop_component_start_time();
    // For 3.1/3.3 the base class marks the session established immediately;
    // for 3.4/3.5 this builds and sends the first negotiation message through
    // the BridgedAPI send() override
    this->tuya_api_->NegotiateSessionStart(this->key_);
    if (this->tuya_api_->getSessionState() == ::Tuya::Session::ESTABLISHED) {
      this->session_established_();
    }
  });

  this->client_->onDisconnect([this](void *arg, AsyncClient *c) {
    ESP_LOGI(TAG, "Disconnected");
    this->disconnect_();
  });

  this->client_->onError([this](void *arg, AsyncClient *c, int8_t error) {
    ESP_LOGW(TAG, "Connection error: %d", error);
    this->disconnect_();
  });

  this->client_->onData([this](void *arg, AsyncClient *c, void *data, size_t len) {
    // May run in the TCP stack's task: only buffer here, loop() does the
    // protocol work in the main loop
    this->last_rx_time_ = App.get_loop_component_start_time();
    LockGuard guard(this->rx_mutex_);
    if (this->rx_len_ + len > RX_BUFFER_SIZE) {
      ESP_LOGW(TAG, "RX buffer overflow, dropping %zu buffered bytes", this->rx_len_);
      this->rx_len_ = 0;
      if (len > RX_BUFFER_SIZE)
        return;
    }
    memcpy(this->rx_buffer_ + this->rx_len_, data, len);
    this->rx_len_ += len;
    // The main loop may be sleeping; have it process the frame promptly
    App.wake_loop_threadsafe();
  });

  ESP_LOGI(TAG, "Connecting to %s...", this->address_.c_str());
  this->state_ = State::CONNECTING;
  if (!this->client_->connect(this->address_.c_str(), this->port_)) {
    ESP_LOGW(TAG, "Connect failed");
    this->disconnect_();
  }
}

void TuyaTCP::disconnect_() {
  if (this->client_) {
    this->client_->close();
    this->client_.reset();
  }
  this->state_ = State::DISCONNECTED;
  this->initial_query_sent_ = false;
}

size_t TuyaTCP::frame_size_() const {
  // Returns the total size of the frame at the head of rx_buffer_, or 0 if
  // the header is not complete yet. Caller holds rx_mutex_.
  if (this->version_ >= tuyaAPI::Protocol::v35) {
    // 00006699: 18-byte header with a 32-bit payload length, 4-byte trailer
    if (this->rx_len_ < 18)
      return 0;
    uint32_t payload_len =
        encode_uint32(this->rx_buffer_[14], this->rx_buffer_[15], this->rx_buffer_[16], this->rx_buffer_[17]);
    return 18 + payload_len + 4;
  }
  // 000055aa: 16-byte header; the length at offset 12 covers the rest
  if (this->rx_len_ < 16)
    return 0;
  uint32_t len = encode_uint32(this->rx_buffer_[12], this->rx_buffer_[13], this->rx_buffer_[14], this->rx_buffer_[15]);
  return 16 + len;
}

void TuyaTCP::process_rx_() {
  static constexpr uint8_t PREFIX_55AA[4] = {0x00, 0x00, 0x55, 0xaa};
  static constexpr uint8_t PREFIX_6699[4] = {0x00, 0x00, 0x66, 0x99};
  const uint8_t *prefix = this->version_ >= tuyaAPI::Protocol::v35 ? PREFIX_6699 : PREFIX_55AA;

  LockGuard guard(this->rx_mutex_);
  while (this->rx_len_ >= 4) {
    if (memcmp(this->rx_buffer_, prefix, 4) != 0) {
      // Desynchronised: find the next frame prefix and drop what precedes it
      size_t skip = 1;
      while (skip + 4 <= this->rx_len_ && memcmp(this->rx_buffer_ + skip, prefix, 4) != 0)
        skip++;
      if (skip + 4 > this->rx_len_)
        skip = this->rx_len_;
      ESP_LOGW(TAG, "Dropping %zu bytes before frame prefix", skip);
      memmove(this->rx_buffer_, this->rx_buffer_ + skip, this->rx_len_ - skip);
      this->rx_len_ -= skip;
      continue;
    }
    size_t total = this->frame_size_();
    if (total == 0)
      return;  // Header not complete yet
    if (total > RX_BUFFER_SIZE) {
      ESP_LOGW(TAG, "Frame of %zu bytes exceeds buffer, dropping", total);
      this->rx_len_ = 0;
      return;
    }
    if (this->rx_len_ < total)
      return;  // Frame not complete yet

    if (this->state_ == State::NEGOTIATING) {
      this->handle_negotiation_data_(this->rx_buffer_, total);
    } else if (this->state_ == State::CONNECTED) {
      this->handle_connected_data_(this->rx_buffer_, total);
    }
    memmove(this->rx_buffer_, this->rx_buffer_ + total, this->rx_len_ - total);
    this->rx_len_ -= total;
  }
}

void TuyaTCP::handle_negotiation_data_(const uint8_t *data, size_t len) {
  this->tuya_api_->NegotiateSessionFinalize(const_cast<uint8_t *>(data), (int) len, this->key_);
  if (this->tuya_api_->getSessionState() == ::Tuya::Session::ESTABLISHED) {
    this->session_established_();
  } else if (this->tuya_api_->getSessionState() == ::Tuya::Session::INVALID) {
    ESP_LOGW(TAG, "Session negotiation failed");
    this->disconnect_();
  }
}

void TuyaTCP::session_established_() {
  ESP_LOGI(TAG, "Session established");
  this->state_ = State::CONNECTED;
  this->last_rx_time_ = App.get_loop_component_start_time();
  this->mark_initialized_();
  this->send_initial_query_();
}

void TuyaTCP::handle_connected_data_(const uint8_t *data, size_t len) {
  std::string decoded = this->tuya_api_->DecodeTuyaMessage(const_cast<uint8_t *>(data), (int) len, this->key_);
  if (!decoded.empty()) {
    ESP_LOGD(TAG, "Received: %s", decoded.c_str());
    this->parse_json_message_(decoded);
  }
}

bool TuyaTCP::send_message_(uint8_t command, const std::string &payload) {
  unsigned char buffer[TX_BUFFER_SIZE];
  int len = this->tuya_api_->BuildTuyaMessage(buffer, command, payload, this->key_);
  if (len <= 0) {
    ESP_LOGW(TAG, "Failed to build message for command %u", command);
    return false;
  }
  if (this->transport_send(buffer, len) < 0) {
    ESP_LOGW(TAG, "Failed to send command %u", command);
    return false;
  }
  return true;
}

void TuyaTCP::send_initial_query_() {
  if (this->initial_query_sent_)
    return;
  uint8_t command = this->version_ >= tuyaAPI::Protocol::v35 ? TUYA_DP_QUERY_NEW : TUYA_DP_QUERY;
  if (this->send_message_(command, this->tuya_api_->GeneratePayload(command, this->device_id_, ""))) {
    ESP_LOGI(TAG, "Sent DP query");
    this->initial_query_sent_ = true;
  }
}

void TuyaTCP::send_datapoint_command(uint8_t datapoint_id, tuya::TuyaDatapointType datapoint_type, const uint8_t *data,
                                     size_t len) {
  if (this->state_ != State::CONNECTED) {
    ESP_LOGW(TAG, "Cannot send command: not connected");
    return;
  }

  std::string dps_value;
  switch (datapoint_type) {
    case tuya::TuyaDatapointType::BOOLEAN:
      dps_value = data[0] ? "true" : "false";
      break;
    case tuya::TuyaDatapointType::INTEGER: {
      uint32_t val = encode_uint32(data[0], data[1], data[2], data[3]);
      char buf[UINT32_MAX_STR_SIZE];
      uint32_to_str(buf, val);
      dps_value = buf;
      break;
    }
    case tuya::TuyaDatapointType::STRING:
      dps_value = "\"";
      dps_value.append(reinterpret_cast<const char *>(data), len);
      dps_value += "\"";
      break;
    case tuya::TuyaDatapointType::ENUM: {
      char buf[UINT32_MAX_STR_SIZE];
      uint32_to_str(buf, data[0]);
      dps_value = buf;
      break;
    }
    default:
      ESP_LOGW(TAG, "Unsupported datapoint type: %d", (int) datapoint_type);
      return;
  }

  // GeneratePayload() wraps the bare datapoint map in the per-command envelope
  char dps[128];
  snprintf(dps, sizeof(dps), R"({"%d":%s})", datapoint_id, dps_value.c_str());
  uint8_t command = this->version_ >= tuyaAPI::Protocol::v34 ? TUYA_CONTROL_NEW : TUYA_CONTROL;
  if (this->send_message_(command, this->tuya_api_->GeneratePayload(command, this->device_id_, dps))) {
    ESP_LOGD(TAG, "Sent control command for DP %d", datapoint_id);
  }
}

void TuyaTCP::dump_config() {
  ESP_LOGCONFIG(TAG, "Tuya TCP:");
  ESP_LOGCONFIG(TAG, "  Address: %s", this->address_.c_str());
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
  const char *version_str = "unknown";
  switch (this->version_) {
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
  this->dump_datapoints_();
}

void TuyaTCP::parse_json_message_(const std::string &json) {
  // The decoder appends the payloads of every frame in the TCP segment into
  // one string, so this may hold several concatenated JSON objects; process
  // the dps map of each of them
  size_t dps_pos = json.find("\"dps\":");
  if (dps_pos == std::string::npos) {
    ESP_LOGW(TAG, "No 'dps' field in JSON");
    return;
  }
  while (dps_pos != std::string::npos) {
    this->parse_dps_object_(json, dps_pos);
    dps_pos = json.find("\"dps\":", dps_pos + 6);
  }
}

void TuyaTCP::parse_dps_object_(const std::string &json, size_t dps_pos) {
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

}  // namespace esphome::tuya_tcp
