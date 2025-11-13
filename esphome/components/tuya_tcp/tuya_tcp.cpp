#include "tuya_tcp.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/components/network/util.h"

namespace esphome {
namespace tuya_tcp {

static const char *const TAG = "tuya_tcp";

void TuyaTCP::setup() { ESP_LOGCONFIG(TAG, "Setting up Tuya TCP..."); }

void TuyaTCP::loop() {
  switch (state_) {
    case State::DISCONNECTED:
      if (network::is_connected()) {
        uint32_t now = millis();
        if (now - last_connect_attempt_ >= 10000) {
          last_connect_attempt_ = now;
          connect_();
        }
      }
      break;

    case State::CONNECTING: {
      if (!socket_) {
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
          ESP_LOGI(TAG, "Connected to %s:%d", address_.c_str(), port_);
          state_ = State::CONNECTED;
        } else {
          ESP_LOGW(TAG, "Connection failed: %d", error);
          disconnect_();
        }
      } else if (ret < 0) {
        ESP_LOGE(TAG, "Select error: %d", errno);
        disconnect_();
      }
      break;
    }

    case State::CONNECTED:
      // Inject fake datapoints once on connection
      if (!socket_) {
        disconnect_();
        break;
      }

      static bool injected = false;
      if (!injected) {
        injected = true;
        ESP_LOGI(TAG, "Injecting fake datapoints");

        // Inject datapoints like: {"dps":{"1":false,"2":"3","6":false,"12":"0","15":243,"104":"Turn_off"}}
        // Simulate receiving some datapoints
        tuya::TuyaDatapoint dp;

        // DP 1: boolean false
        dp.id = 1;
        dp.type = tuya::TuyaDatapointType::BOOLEAN;
        dp.value_bool = false;
        handle_datapoint_(dp);

        // DP 2: integer 3
        dp.id = 2;
        dp.type = tuya::TuyaDatapointType::INTEGER;
        dp.value_int = 3;
        handle_datapoint_(dp);

        // DP 6: boolean false
        dp.id = 6;
        dp.type = tuya::TuyaDatapointType::BOOLEAN;
        dp.value_bool = false;
        handle_datapoint_(dp);

        // DP 12: integer 0
        dp.id = 12;
        dp.type = tuya::TuyaDatapointType::INTEGER;
        dp.value_int = 0;
        handle_datapoint_(dp);

        // DP 15: integer 243
        dp.id = 15;
        dp.type = tuya::TuyaDatapointType::INTEGER;
        dp.value_int = 243;
        handle_datapoint_(dp);
      }
      break;
  }
}

void TuyaTCP::dump_config() {
  ESP_LOGCONFIG(TAG, "Tuya TCP:");
  ESP_LOGCONFIG(TAG, "  Address: %s:%d", address_.c_str(), port_);
  ESP_LOGCONFIG(TAG, "  Device ID: %s", device_id_.c_str());
  ESP_LOGCONFIG(TAG, "  Version: %s", version_.c_str());
  ESP_LOGCONFIG(TAG, "  State: %d", (int) state_);
}

void TuyaTCP::connect_() {
  // Detect address family based on whether address contains ':'
  int family = (address_.find(':') != std::string::npos) ? AF_INET6 : AF_INET;
  socket_ = socket::socket(family, SOCK_STREAM, 0);
  if (!socket_) {
    ESP_LOGE(TAG, "Failed to create socket");
    return;
  }

  socket_->setblocking(false);

  struct sockaddr_storage addr;
  socklen_t addrlen = socket::set_sockaddr((struct sockaddr *) &addr, sizeof(addr), address_, port_);
  if (addrlen == 0) {
    ESP_LOGE(TAG, "Failed to resolve address %s", address_.c_str());
    socket_.reset();
    return;
  }

  // Format IPv6 addresses with brackets for clarity
  if (family == AF_INET6) {
    ESP_LOGD(TAG, "Connecting to [%s]:%d", address_.c_str(), port_);
  } else {
    ESP_LOGD(TAG, "Connecting to %s:%d", address_.c_str(), port_);
  }

  int err = socket_->connect((struct sockaddr *) &addr, addrlen);
  if (err == 0) {
    if (family == AF_INET6) {
      ESP_LOGI(TAG, "Connected immediately to [%s]:%d", address_.c_str(), port_);
    } else {
      ESP_LOGI(TAG, "Connected immediately to %s:%d", address_.c_str(), port_);
    }
    state_ = State::CONNECTED;
  } else if (errno == EINPROGRESS) {
    if (family == AF_INET6) {
      ESP_LOGD(TAG, "Connection in progress to [%s]:%d", address_.c_str(), port_);
    } else {
      ESP_LOGD(TAG, "Connection in progress to %s:%d", address_.c_str(), port_);
    }
    state_ = State::CONNECTING;
  } else {
    ESP_LOGW(TAG, "Connection failed: %s", strerror(errno));
    socket_.reset();
  }
}

void TuyaTCP::disconnect_() {
  socket_.reset();
  state_ = State::DISCONNECTED;
}

void TuyaTCP::send_datapoint_command(uint8_t datapoint_id, tuya::TuyaDatapointType datapoint_type,
                                     const std::vector<uint8_t> &data) {
  if (state_ != State::CONNECTED) {
    ESP_LOGW(TAG, "Cannot send command: not connected");
    return;
  }
  ESP_LOGD(TAG, "Would send datapoint %d command (not implemented yet)", datapoint_id);
}

}  // namespace tuya_tcp
}  // namespace esphome
