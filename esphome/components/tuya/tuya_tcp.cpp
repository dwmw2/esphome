#include "tuya_tcp.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/components/network/util.h"

namespace esphome {
namespace tuya {

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
        std::vector<uint8_t> dp_data;

        // DP 1: boolean false
        dp_data = {0};
        handle_datapoints_((const uint8_t[]){1, 1, 0, 1, 0}, 5);

        // DP 2: string "3"
        handle_datapoints_((const uint8_t[]){2, 3, 0, 1, '3'}, 5);

        // DP 6: boolean false
        handle_datapoints_((const uint8_t[]){6, 1, 0, 1, 0}, 5);

        // DP 12: string "0"
        handle_datapoints_((const uint8_t[]){12, 3, 0, 1, '0'}, 5);

        // DP 15: integer 243
        handle_datapoints_((const uint8_t[]){15, 2, 0, 4, 0, 0, 0, 243}, 8);

        // DP 104: string "Turn_off"
        handle_datapoints_((const uint8_t[]){104, 3, 0, 8, 'T', 'u', 'r', 'n', '_', 'o', 'f', 'f'}, 12);
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
  socket_ = socket::socket_ip(SOCK_STREAM, 0);
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

  int err = socket_->connect((struct sockaddr *) &addr, addrlen);
  if (err == 0) {
    ESP_LOGI(TAG, "Connected immediately to %s:%d", address_.c_str(), port_);
    state_ = State::CONNECTED;
  } else if (errno == EINPROGRESS) {
    ESP_LOGD(TAG, "Connection in progress to %s:%d", address_.c_str(), port_);
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

void TuyaTCP::send_datapoint_command_(uint8_t datapoint_id, TuyaDatapointType datapoint_type,
                                      std::vector<uint8_t> data) {
  if (state_ != State::CONNECTED) {
    ESP_LOGW(TAG, "Cannot send command: not connected");
    return;
  }
  ESP_LOGD(TAG, "Would send datapoint %d command (not implemented yet)", datapoint_id);
}

}  // namespace tuya
}  // namespace esphome
