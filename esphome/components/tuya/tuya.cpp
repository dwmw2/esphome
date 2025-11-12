#include "tuya.h"
#include "esphome/core/log.h"

namespace esphome {
namespace tuya {

static const char *const TAG = "tuya";

void Tuya::dump_config() { ESP_LOGCONFIG(TAG, "Tuya:"); }

void Tuya::handle_datapoints_(const uint8_t *buffer, size_t len) {
  while (len >= 4) {
    TuyaDatapoint datapoint{};
    datapoint.id = buffer[0];
    datapoint.type = (TuyaDatapointType) buffer[1];
    datapoint.len = (buffer[2] << 8) + buffer[3];

    if (len < datapoint.len + 4) {
      ESP_LOGW(TAG, "Datapoint %u length mismatch: expected %zu, got %zu", datapoint.id, datapoint.len + 4, len);
      return;
    }

    const uint8_t *data = buffer + 4;

    switch (datapoint.type) {
      case TuyaDatapointType::BOOLEAN:
        if (datapoint.len != 1) {
          ESP_LOGW(TAG, "Datapoint %u has invalid length %zu for BOOLEAN", datapoint.id, datapoint.len);
          break;
        }
        datapoint.value_bool = data[0];
        break;
      case TuyaDatapointType::INTEGER:
        if (datapoint.len != 4) {
          ESP_LOGW(TAG, "Datapoint %u has invalid length %zu for INTEGER", datapoint.id, datapoint.len);
          break;
        }
        datapoint.value_int = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
        datapoint.value_uint = datapoint.value_int;
        break;
      case TuyaDatapointType::STRING:
        datapoint.value_string = std::string(reinterpret_cast<const char *>(data), datapoint.len);
        break;
      case TuyaDatapointType::ENUM:
        if (datapoint.len != 1) {
          ESP_LOGW(TAG, "Datapoint %u has invalid length %zu for ENUM", datapoint.id, datapoint.len);
          break;
        }
        datapoint.value_enum = data[0];
        break;
      case TuyaDatapointType::BITMASK:
        switch (datapoint.len) {
          case 1:
            datapoint.value_bitmask = data[0];
            break;
          case 2:
            datapoint.value_bitmask = (data[0] << 8) | data[1];
            break;
          case 4:
            datapoint.value_bitmask = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
            break;
          default:
            ESP_LOGW(TAG, "Datapoint %u has invalid length %zu for BITMASK", datapoint.id, datapoint.len);
            break;
        }
        break;
      case TuyaDatapointType::RAW:
        datapoint.value_raw = std::vector<uint8_t>(data, data + datapoint.len);
        break;
      default:
        ESP_LOGW(TAG, "Datapoint %u has unknown type 0x%02X", datapoint.id, static_cast<uint8_t>(datapoint.type));
        break;
    }

    ESP_LOGD(TAG, "Datapoint %u update to %s", datapoint.id, format_hex_pretty(data, datapoint.len).c_str());

    // Update internal datapoints
    bool found = false;
    for (auto &dp : this->datapoints_) {
      if (dp.id == datapoint.id) {
        dp = datapoint;
        found = true;
        break;
      }
    }
    if (!found) {
      this->datapoints_.push_back(datapoint);
    }

    // Notify listeners
    for (auto &listener : this->listeners_) {
      if (listener.datapoint_id == datapoint.id) {
        listener.on_datapoint(datapoint);
      }
    }

    buffer += datapoint.len + 4;
    len -= datapoint.len + 4;
  }
}

optional<TuyaDatapoint> Tuya::get_datapoint_(uint8_t datapoint_id) {
  for (auto &datapoint : this->datapoints_) {
    if (datapoint.id == datapoint_id)
      return datapoint;
  }
  return {};
}

void Tuya::set_numeric_datapoint_value_(uint8_t datapoint_id, TuyaDatapointType datapoint_type, const uint32_t value,
                                        uint8_t length, bool forced) {
  ESP_LOGD(TAG, "Setting datapoint %u to %u", datapoint_id, value);
  optional<TuyaDatapoint> datapoint = this->get_datapoint_(datapoint_id);
  if (!datapoint.has_value() || datapoint->type != datapoint_type) {
    ESP_LOGW(TAG, "Datapoint %u not found or has incorrect type", datapoint_id);
  } else if (!forced &&
             std::find(this->ignore_mcu_update_on_datapoints_.begin(), this->ignore_mcu_update_on_datapoints_.end(),
                       datapoint_id) != this->ignore_mcu_update_on_datapoints_.end()) {
    ESP_LOGV(TAG, "Ignoring MCU update for datapoint %u", datapoint_id);
    return;
  } else {
    bool should_send = forced;
    switch (datapoint_type) {
      case TuyaDatapointType::BOOLEAN:
        should_send = should_send || datapoint->value_bool != value;
        break;
      case TuyaDatapointType::INTEGER:
        should_send = should_send || datapoint->value_int != static_cast<int>(value);
        break;
      case TuyaDatapointType::ENUM:
        should_send = should_send || datapoint->value_enum != value;
        break;
      case TuyaDatapointType::BITMASK:
        should_send = should_send || datapoint->value_bitmask != value;
        break;
      default:
        break;
    }
    if (!should_send) {
      ESP_LOGV(TAG, "Not sending unchanged value");
      return;
    }
  }

  std::vector<uint8_t> data;
  for (int i = length - 1; i >= 0; i--) {
    data.push_back((value >> (i * 8)) & 0xFF);
  }

  this->send_datapoint_command_(datapoint_id, datapoint_type, data);
}

void Tuya::set_raw_datapoint_value_(uint8_t datapoint_id, const std::vector<uint8_t> &value, bool forced) {
  ESP_LOGD(TAG, "Setting datapoint %u to %s", datapoint_id, format_hex_pretty(value).c_str());
  optional<TuyaDatapoint> datapoint = this->get_datapoint_(datapoint_id);
  if (!datapoint.has_value() || datapoint->type != TuyaDatapointType::RAW) {
    ESP_LOGW(TAG, "Datapoint %u not found or has incorrect type", datapoint_id);
  } else if (!forced && datapoint->value_raw == value) {
    ESP_LOGV(TAG, "Not sending unchanged value");
    return;
  }

  this->send_datapoint_command_(datapoint_id, TuyaDatapointType::RAW, value);
}

void Tuya::set_string_datapoint_value_(uint8_t datapoint_id, const std::string &value, bool forced) {
  ESP_LOGD(TAG, "Setting datapoint %u to %s", datapoint_id, value.c_str());
  optional<TuyaDatapoint> datapoint = this->get_datapoint_(datapoint_id);
  if (!datapoint.has_value() || datapoint->type != TuyaDatapointType::STRING) {
    ESP_LOGW(TAG, "Datapoint %u not found or has incorrect type", datapoint_id);
  } else if (!forced &&
             std::find(this->ignore_mcu_update_on_datapoints_.begin(), this->ignore_mcu_update_on_datapoints_.end(),
                       datapoint_id) != this->ignore_mcu_update_on_datapoints_.end()) {
    ESP_LOGV(TAG, "Ignoring MCU update for datapoint %u", datapoint_id);
    return;
  } else if (!forced && datapoint->value_string == value) {
    ESP_LOGV(TAG, "Not sending unchanged value");
    return;
  }

  std::vector<uint8_t> data;
  for (char const &c : value) {
    data.push_back(c);
  }

  this->send_datapoint_command_(datapoint_id, TuyaDatapointType::STRING, data);
}

void Tuya::set_raw_datapoint_value(uint8_t datapoint_id, const std::vector<uint8_t> &value) {
  this->set_raw_datapoint_value_(datapoint_id, value, false);
}

void Tuya::set_boolean_datapoint_value(uint8_t datapoint_id, bool value) {
  this->set_numeric_datapoint_value_(datapoint_id, TuyaDatapointType::BOOLEAN, value, 1, false);
}

void Tuya::set_integer_datapoint_value(uint8_t datapoint_id, uint32_t value) {
  this->set_numeric_datapoint_value_(datapoint_id, TuyaDatapointType::INTEGER, value, 4, false);
}

void Tuya::set_string_datapoint_value(uint8_t datapoint_id, const std::string &value) {
  this->set_string_datapoint_value_(datapoint_id, value, false);
}

void Tuya::set_enum_datapoint_value(uint8_t datapoint_id, uint8_t value) {
  this->set_numeric_datapoint_value_(datapoint_id, TuyaDatapointType::ENUM, value, 1, false);
}

void Tuya::set_bitmask_datapoint_value(uint8_t datapoint_id, uint32_t value, uint8_t length) {
  this->set_numeric_datapoint_value_(datapoint_id, TuyaDatapointType::BITMASK, value, length, false);
}

void Tuya::force_set_raw_datapoint_value(uint8_t datapoint_id, const std::vector<uint8_t> &value) {
  this->set_raw_datapoint_value_(datapoint_id, value, true);
}

void Tuya::force_set_boolean_datapoint_value(uint8_t datapoint_id, bool value) {
  this->set_numeric_datapoint_value_(datapoint_id, TuyaDatapointType::BOOLEAN, value, 1, true);
}

void Tuya::force_set_integer_datapoint_value(uint8_t datapoint_id, uint32_t value) {
  this->set_numeric_datapoint_value_(datapoint_id, TuyaDatapointType::INTEGER, value, 4, true);
}

void Tuya::force_set_string_datapoint_value(uint8_t datapoint_id, const std::string &value) {
  this->set_string_datapoint_value_(datapoint_id, value, true);
}

void Tuya::force_set_enum_datapoint_value(uint8_t datapoint_id, uint8_t value) {
  this->set_numeric_datapoint_value_(datapoint_id, TuyaDatapointType::ENUM, value, 1, true);
}

void Tuya::force_set_bitmask_datapoint_value(uint8_t datapoint_id, uint32_t value, uint8_t length) {
  this->set_numeric_datapoint_value_(datapoint_id, TuyaDatapointType::BITMASK, value, length, true);
}

void Tuya::register_listener(uint8_t datapoint_id, const std::function<void(TuyaDatapoint)> &func) {
  auto listener = TuyaDatapointListener{
      .datapoint_id = datapoint_id,
      .on_datapoint = func,
  };
  this->listeners_.push_back(listener);

  // Run through existing datapoints
  for (auto &datapoint : this->datapoints_) {
    if (datapoint.id == datapoint_id)
      func(datapoint);
  }
}

}  // namespace tuya
}  // namespace esphome
