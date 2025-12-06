#include "tuya_tcp.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/components/network/util.h"

namespace esphome {
namespace tuya_tcp {

static const char *const TAG = "tuya_tcp";

void TuyaTCP::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Tuya TCP...");

  tuya_api_ = tuyaAPI::create(version_);
  if (!tuya_api_) {
    ESP_LOGE(TAG, "Failed to create Tuya API");
    mark_failed();
    return;
  }

  ESP_LOGCONFIG(TAG, "Setting up Tuya TCP...");
}

void TuyaTCP::loop() {
  switch (state_) {
    case State::DISCONNECTED:
      if (network::is_connected()) {
        uint32_t now = millis();
        if (now - last_connect_attempt_ >= 10000) {
          last_connect_attempt_ = now;
          state_ = State::CONNECTING;
          connect_();
        }
      }
      break;

    case State::CONNECTING: {
      if (!socket_) {
        ESP_LOGE(TAG, "Invalid socket in CONNECTING state");
        disconnect_();
        break;
      }

      // Use select to check if socket is writable (connection complete)
      fd_set write_fds;
      FD_ZERO(&write_fds);
      int fd = socket_->get_fd();
      if (fd < 0) {
        ESP_LOGE(TAG, "Invalid socket fd");
        disconnect_();
        break;
      }
      FD_SET(fd, &write_fds);

      struct timeval tv = {0, 0};  // Non-blocking check
      int ret = select(fd + 1, nullptr, &write_fds, nullptr, &tv);

      if (ret > 0 && FD_ISSET(fd, &write_fds)) {
        // Socket is writable, check for errors
        int error = 0;
        socklen_t len = sizeof(error);
        if (socket_->getsockopt(SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0) {
          ESP_LOGI(TAG, "Connected!");

          // Initialize negotiation state
          start_negotiation_();

          // Try to get first negotiation packet
          pending_send_.resize(1024);
          int packet_size = tuya_api_->BuildSessionMessage(pending_send_.data());
          if (packet_size < 0) {
            ESP_LOGE(TAG, "Failed to build negotiation packet: %d", packet_size);
            disconnect_();
          } else if (packet_size == 0) {
            // No negotiation needed
            ESP_LOGI(TAG, "No negotiation needed");
            pending_send_.clear();
            state_ = State::CONNECTED;
          } else {
            // Prepare to send negotiation packet
            pending_send_.resize(packet_size);
            ESP_LOGD(TAG, "Prepared negotiation packet: %d bytes", packet_size);
            negotiation_start_ = millis();
            state_ = State::NEGOTIATING;
          }
        } else {
          ESP_LOGW(TAG, "Connection failed: %d", error);
          disconnect_();
        }
      } else if (ret < 0) {
        ESP_LOGE(TAG, "Select error: %d", errno);
        disconnect_();
      }
      // If ret == 0, socket not ready yet, try again next loop
      break;
    }

    case State::NEGOTIATING: {
      if (!socket_) {
        ESP_LOGW(TAG, "Disconnected during negotiation");
        disconnect_();
        break;
      }

      // Check for timeout (5 seconds)
      if (millis() - negotiation_start_ > 5000) {
        ESP_LOGW(TAG, "Negotiation timeout");
        disconnect_();
        break;
      }

      // Send pending data if any
      if (!pending_send_.empty()) {
        ssize_t sent = socket_->write(pending_send_.data(), pending_send_.size());
        if (sent > 0) {
          ESP_LOGD(TAG, "Sent negotiation data: %d bytes", (int) sent);
          pending_send_.clear();

          // Check if negotiation complete after sending
          if (tuya_api_->isSessionEstablished()) {
            ESP_LOGI(TAG, "Negotiation complete");
            state_ = State::CONNECTED;
            initial_query_sent_ = false;
            last_rx_time_ = millis();
            break;
          }
        } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
          ESP_LOGE(TAG, "Write error during negotiation: %d", errno);
          socket_->close();
          disconnect_();
          break;
        }
        // If EAGAIN, just try again next loop
      }

      // Always listening for data
      uint8_t buf[512];
      ssize_t len = socket_->read(buf, sizeof(buf));
      if (len > 0) {
        // Feed response to protocol handler
        std::string response = tuya_api_->DecodeSessionMessage(buf, len);

        // Check if negotiation complete
        if (tuya_api_->isSessionEstablished()) {
          ESP_LOGI(TAG, "Negotiation complete");
          state_ = State::CONNECTED;
          initial_query_sent_ = false;  // Reset flag for new connection
          this->initialized_callback_.call();
          break;  // Exit to CONNECTED state
        }

        // Get next packet to send (if any)
        pending_send_.resize(1024);
        int packet_size = tuya_api_->BuildSessionMessage(pending_send_.data());
        if (packet_size < 0) {
          ESP_LOGE(TAG, "Negotiation failed");
          disconnect_();
        } else if (packet_size > 0) {
          pending_send_.resize(packet_size);
          ESP_LOGD(TAG, "Prepared negotiation response: %d bytes", packet_size);
          // Will be sent on next loop iteration, then check if done
        } else {
          pending_send_.clear();
        }
      } else if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        ESP_LOGW(TAG, "Read error during negotiation: %d", errno);
        disconnect_();
      }
      break;
    }

    case State::CONNECTED: {
      if (!socket_) {
        ESP_LOGW(TAG, "Disconnected");
        disconnect_();
        initial_query_sent_ = false;
        break;
      }

      // Send initial DP query once after connection
      if (!initial_query_sent_) {
        char payload[256];
        uint32_t now = time(nullptr);
        snprintf(payload, sizeof(payload), "{\"gwId\":\"%s\",\"devId\":\"%s\",\"uid\":\"%s\",\"t\":\"%u\"}",
                 device_id_.c_str(), device_id_.c_str(), device_id_.c_str(), now);

        ESP_LOGD(TAG, "DP query payload: %s", payload);

        std::vector<uint8_t> message(1024);
        uint8_t command = (tuya_api_->getProtocol() >= tuyaAPI::Protocol::v35) ? TUYA_DP_QUERY_NEW : TUYA_DP_QUERY;
        int len = tuya_api_->BuildTuyaMessage(message.data(), command, std::string(payload));
        if (len > 0) {
          ssize_t sent = socket_->write(message.data(), len);
          if (sent == len) {
            ESP_LOGI(TAG, "Sent DP query");
            initial_query_sent_ = true;
          } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGE(TAG, "Failed to send DP query: %d", errno);
          }
        }
      }

      uint8_t buf[256];
      ssize_t len = socket_->read(buf, sizeof(buf));
      if (len > 0) {
        last_rx_time_ = millis();
        std::string decoded = tuya_api_->DecodeTuyaMessage(buf, len);
        if (!decoded.empty()) {
          ESP_LOGI(TAG, "Received: %s", decoded.c_str());
          parse_json_message_(decoded);
        }
      } else if (len == 0) {
        // Connection closed by peer
        ESP_LOGI(TAG, "Connection closed by peer");
        disconnect_();
      } else if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        ESP_LOGW(TAG, "Read error: %d", errno);
        disconnect_();
      }

      // Send heartbeat if no data received for 5 seconds
      if (millis() - last_rx_time_ > 5000) {
        std::vector<uint8_t> message(1024);
        int hb_len = tuya_api_->BuildTuyaMessage(message.data(), TUYA_HEART_BEAT, "");
        if (hb_len > 0) {
          ssize_t sent = socket_->write(message.data(), hb_len);
          if (sent == hb_len) {
            ESP_LOGV(TAG, "Sent heartbeat");
            last_rx_time_ = millis();  // Reset timer after sending
          } else if (sent < 0) {
            ESP_LOGW(TAG, "Failed to send heartbeat: %d", errno);
            disconnect_();
          }
        }
      }
      break;
    }
  }

  tuya::Tuya::loop();
}

void TuyaTCP::start_negotiation_() {
  // Recreate API object for fresh state on each connection
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

void TuyaTCP::disconnect_() {
  socket_.reset();
  state_ = State::DISCONNECTED;
}

void TuyaTCP::send_datapoint_command(uint8_t datapoint_id, tuya::TuyaDatapointType datapoint_type,
                                     const std::vector<uint8_t> &data) {
  if (!socket_ || state_ != State::CONNECTED) {
    ESP_LOGW(TAG, "Cannot send command: not connected");
    return;
  }

  // Build JSON payload for v3.4
  char payload[512];
  uint32_t now = time(nullptr);

  // Start building the dps object
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

  ESP_LOGD(TAG, "Control payload: %s", payload);

  std::vector<uint8_t> message(1024);
  int len = tuya_api_->BuildTuyaMessage(message.data(), TUYA_CONTROL_NEW, std::string(payload));
  if (len > 0) {
    ssize_t sent = socket_->write(message.data(), len);
    if (sent == len) {
      ESP_LOGD(TAG, "Sent control command for DP %d", datapoint_id);
    } else {
      ESP_LOGE(TAG, "Failed to send control command");
    }
  }
}

void TuyaTCP::connect_() {
  // Socket should already be cleaned up by disconnect_()
  // Just create a new one
  struct sockaddr_storage addr;
  socklen_t addrlen = socket::set_sockaddr((struct sockaddr *) &addr, sizeof(addr), address_, port_);
  if (addrlen == 0) {
    ESP_LOGE(TAG, "Invalid address: %s", address_.c_str());
    disconnect_();
    return;
  }

  int family = ((struct sockaddr *) &addr)->sa_family;
  socket_ = socket::socket(family, SOCK_STREAM, IPPROTO_TCP);
  if (!socket_) {
    ESP_LOGE(TAG, "Failed to create socket");
    disconnect_();
    return;
  }

  socket_->setblocking(false);

  // IPv6 literals should be printed in square brackets
  if (family == AF_INET) {
    ESP_LOGI(TAG, "Connecting to %s:%d...", address_.c_str(), port_);
  } else {
    ESP_LOGI(TAG, "Connecting to [%s]:%d...", address_.c_str(), port_);
  }
  int err = socket_->connect((struct sockaddr *) &addr, addrlen);
  if (err != 0 && errno != EINPROGRESS) {
    ESP_LOGE(TAG, "Connect failed: %d", errno);
    disconnect_();
  }
}

void TuyaTCP::dump_config() {
  ESP_LOGCONFIG(TAG, "Tuya TCP:");
  ESP_LOGCONFIG(TAG, "  Address: %s", address_.c_str());
  ESP_LOGCONFIG(TAG, "  Port: %d", (int) port_);
  ESP_LOGCONFIG(TAG, "  Device ID: %s", device_id_.c_str());
  ESP_LOGCONFIG(TAG, "  Version: %d", (int) version_);
  ESP_LOGCONFIG(TAG, "  State: %d", (int) state_);
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
  while (pos < json.length()) {
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

    size_t comma = json.find(',', pos);
    if (comma == std::string::npos)
      break;
    pos = comma + 1;
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
