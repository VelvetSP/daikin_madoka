#include "madoka.h"

#include "esphome/core/log.h"
#include <utility>

#ifdef USE_ESP32

namespace esphome {
namespace madoka {

static const char *const TAG = "madoka";

using namespace esphome::climate;

static const uint16_t CMD_GET_SETTING_STATUS = 0x0020;
static const uint16_t CMD_SET_SETTING_STATUS = 0x4020;
static const uint16_t CMD_GET_OPERATION_MODE = 0x0030;
static const uint16_t CMD_SET_OPERATION_MODE = 0x4030;
static const uint16_t CMD_GET_SETPOINT = 0x0040;
static const uint16_t CMD_SET_SETPOINT = 0x4040;
static const uint16_t CMD_GET_FAN_SPEED = 0x0050;
static const uint16_t CMD_SET_FAN_SPEED = 0x4050;
static const uint16_t CMD_GET_SENSOR_INFORMATION = 0x0110;
static const uint16_t CMD_GET_CLEAN_FILTER = 0x0100;
static const uint16_t CMD_GET_VERSION = 0x0130;
static const uint16_t CMD_GET_EYE_BRIGHTNESS = 0x0302;
static const uint16_t CMD_RESET_FILTER = 0x4220;
static const uint16_t CMD_SET_EYE_BRIGHTNESS = 0x4302;

void Madoka::dump_config() { LOG_CLIMATE(TAG, "Daikin Madoka Climate Controller", this); }

void MadokaEyeBrightnessNumber::control(float value) {
  int level = static_cast<int>(value + 0.5f);
  if (level < 0) {
    level = 0;
  }
  if (level > 19) {
    level = 19;
  }
  this->parent_->set_eye_brightness(level);
}

void MadokaResetFilterButton::press_action() { this->parent_->reset_filter(); }

void Madoka::setup() { this->receive_semaphore_ = xSemaphoreCreateMutex(); }

void Madoka::loop() {
  std::vector<uint8_t> chk = {};
  if (xSemaphoreTake(this->receive_semaphore_, 0L)) {
    if (!this->received_chunks_.empty()) {
      chk = this->received_chunks_.front();
      this->received_chunks_.pop();
    }
    xSemaphoreGive(this->receive_semaphore_);
    if (!chk.empty()) {
      this->process_incoming_chunk_(chk);
    }
  }
  if (this->should_update_) {
    this->should_update_ = false;
    this->update();
  }
}

void Madoka::control(const ClimateCall &call) {
  if (this->node_state != espbt::ClientState::ESTABLISHED)
    return;
  if (call.get_mode().has_value()) {
    ClimateMode mode = *call.get_mode();
    uint8_t mode_out = 255, status_out = 0;
    switch (mode) {
      case climate::CLIMATE_MODE_OFF:
        status_out = 0;
        break;
      case climate::CLIMATE_MODE_HEAT_COOL:
        status_out = 1;
        mode_out = 2;
        break;
      case climate::CLIMATE_MODE_COOL:
        status_out = 1;
        mode_out = 3;
        break;
      case climate::CLIMATE_MODE_HEAT:
        status_out = 1;
        mode_out = 4;
        break;
      case climate::CLIMATE_MODE_FAN_ONLY:
        status_out = 1;
        mode_out = 0;
        break;
      case climate::CLIMATE_MODE_DRY:
        status_out = 1;
        mode_out = 1;
        break;
      default:
        ESP_LOGW(TAG, "Unsupported mode: %d", mode);
        break;
    }
    ESP_LOGD(TAG, "status: %d, mode: %d", status_out, mode_out);
    if (mode_out != 255) {
      this->query_(CMD_SET_OPERATION_MODE, std::vector<uint8_t>{0x20, 0x01, (uint8_t) mode_out}, 600);
    }
    this->query_(CMD_SET_SETTING_STATUS, std::vector<uint8_t>{0x20, 0x01, (uint8_t) status_out}, 200);
  }
  if (call.get_target_temperature().has_value()) {
    // Single target temperature: write the same value to both the cooling
    // (0x20) and heating (0x21) set points so the requested temperature is
    // honoured regardless of the active mode.
    uint16_t target = *call.get_target_temperature() * 128;
    this->query_(CMD_SET_SETPOINT,
                 std::vector<uint8_t>{0x20, 0x02, (uint8_t) ((target >> 8) & 0xFF), (uint8_t) (target & 0xFF),
                                      0x21, 0x02, (uint8_t) ((target >> 8) & 0xFF), (uint8_t) (target & 0xFF)},
                 400);
  }
  if (call.get_fan_mode().has_value()) {
    uint8_t fan_mode = call.get_fan_mode().value();
    uint8_t fan_mode_out = 255;
    switch (fan_mode) {
      case climate::CLIMATE_FAN_AUTO:
        fan_mode_out = 0;
        break;
      case climate::CLIMATE_FAN_LOW:
        fan_mode_out = 1;
        break;
      case climate::CLIMATE_FAN_MEDIUM:
        fan_mode_out = 3;
        break;
      case climate::CLIMATE_FAN_HIGH:
        fan_mode_out = 5;
        break;
      default:
        ESP_LOGW(TAG, "Unsupported fan mode: %d", fan_mode);
        break;
    }
    if (fan_mode_out != 255) {
      this->query_(CMD_SET_FAN_SPEED,
                   std::vector<uint8_t>{0x20, 0x01, (uint8_t) fan_mode_out, 0x21, 0x01, (uint8_t) fan_mode_out}, 200);
    }
  }
  this->should_update_ = true;
}

void Madoka::gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  switch (event) {
    case ESP_GAP_BLE_SEC_REQ_EVT:
      esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
      break;
    case ESP_GAP_BLE_NC_REQ_EVT:
      esp_ble_confirm_reply(param->ble_security.ble_req.bd_addr, true);
      ESP_LOGI(TAG, "ESP_GAP_BLE_NC_REQ_EVT, the passkey Notify number:%d", param->ble_security.key_notif.passkey);
      break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT: {
      if (!param->ble_security.auth_cmpl.success) {
        ESP_LOGE(TAG, "Authentication failed, status: 0x%x", param->ble_security.auth_cmpl.fail_reason);
        break;
      }
      auto *nfy = this->parent_->get_characteristic(MADOKA_SERVICE_UUID, NOTIFY_CHARACTERISTIC_UUID);
      auto *wwr = this->parent_->get_characteristic(MADOKA_SERVICE_UUID, WWR_CHARACTERISTIC_UUID);
      if (nfy == nullptr || wwr == nullptr) {
        ESP_LOGW(TAG, "[%s] No control service found at device, not a Daikin Madoka..?", this->get_name().c_str());
        break;
      }
      this->notify_handle_ = nfy->handle;
      this->wwr_handle_ = wwr->handle;

      auto status = esp_ble_gattc_register_for_notify(this->parent_->get_gattc_if(), this->parent_->get_remote_bda(),
                                                      nfy->handle);
      if (status) {
        ESP_LOGW(TAG, "[%s] esp_ble_gattc_register_for_notify failed, status=%d", this->get_name().c_str(), status);
      }
      break;
    }
    default:
      break;
  }
}

void Madoka::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_DISCONNECT_EVT: {
      this->node_state = espbt::ClientState::IDLE;  // ??
      this->current_temperature = NAN;
      this->target_temperature = NAN;
      this->publish_state();
      break;
    }
    case ESP_GATTC_WRITE_DESCR_EVT:
      if (param->write.status != ESP_GATT_OK) {
        if (param->write.status == ESP_GATT_INSUF_AUTHENTICATION) {
          ESP_LOGE(TAG, "Insufficient authentication");
        } else {
          ESP_LOGE(TAG, "Failed writing characteristic descriptor, status = 0x%x", param->write.status);
        }
      }
      break;
    case ESP_GATTC_SEARCH_CMPL_EVT: {
      esp_ble_set_encryption(this->parent_->get_remote_bda(), ESP_BLE_SEC_ENCRYPT_MITM);
      break;
    }
    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      this->node_state = espbt::ClientState::ESTABLISHED;  // ??
      break;
    }
    case ESP_GATTC_NOTIFY_EVT: {
      if (param->notify.handle != this->notify_handle_) {
        ESP_LOGW(TAG, "Different notify handle");
        break;
      }
      std::vector<uint8_t> chk =
          std::vector<uint8_t>{param->notify.value, param->notify.value + param->notify.value_len};
      xSemaphoreTake(this->receive_semaphore_, portMAX_DELAY);
      this->received_chunks_.push(chk);
      xSemaphoreGive(this->receive_semaphore_);
      break;
    }
    default:
      break;
  }
}

void Madoka::update() {
  ESP_LOGD(TAG, "Got update request...");
  if (this->node_state != espbt::ClientState::ESTABLISHED) {
    ESP_LOGD(TAG, "...but device is disconnected");
    return;
  }

  this->query_(CMD_GET_SETTING_STATUS, std::vector<uint8_t>{0x00, 0x00}, 50);
  this->query_(CMD_GET_OPERATION_MODE, std::vector<uint8_t>{0x00, 0x00}, 50);
  this->query_(CMD_GET_SETPOINT, std::vector<uint8_t>{0x00, 0x00}, 50);
  this->query_(CMD_GET_FAN_SPEED, std::vector<uint8_t>{0x00, 0x00}, 50);
  this->query_(CMD_GET_SENSOR_INFORMATION, std::vector<uint8_t>{0x00, 0x00}, 50);
  // Only poll the optional auxiliary entities when they are actually
  // configured; each query is a BLE round-trip plus a blocking delay, so
  // skipping the unused ones keeps every poll shorter.
  if (this->clean_filter_binary_sensor_ != nullptr) {
    // Empty-args request {0x00,0x00}: fn 0x0100 returns the 0x62 object regardless of args
    // (matches openHAB GetCleanFilterIndicatorCommand, which sends no args). Response arg 0x62 parsed below.
    this->query_(CMD_GET_CLEAN_FILTER, std::vector<uint8_t>{0x00, 0x00}, 50);
  }
  if (this->firmware_version_text_sensor_ != nullptr) {
    this->query_(CMD_GET_VERSION, std::vector<uint8_t>{0x00, 0x00}, 50);
  }
  if (this->eye_brightness_number_ != nullptr) {
    this->query_(CMD_GET_EYE_BRIGHTNESS, std::vector<uint8_t>{0x33, 0x01, 0x00}, 50);
  }
}

void Madoka::set_eye_brightness(uint8_t level) {
  if (this->node_state != espbt::ClientState::ESTABLISHED) {
    return;
  }
  this->query_(CMD_SET_EYE_BRIGHTNESS, std::vector<uint8_t>{0x33, 0x01, level}, 200);
  if (this->eye_brightness_number_ != nullptr) {
    this->eye_brightness_number_->publish_state(level);
  }
  this->should_update_ = true;
}

void Madoka::reset_filter() {
  if (this->node_state != espbt::ClientState::ESTABLISHED) {
    return;
  }
  // A proper "filter cleaned" reset needs TWO actions; openHAB DaikinMadokaHandler.resetCleanFilterIndicator()
  // issues them as two distinct command/response cycles, not one frame:
  //   DisableCleanFilterIndicator (0x51) then ResetCleanFilterTimer (0xFE).
  // We mirror that validated sequence. The combined {0x51,..,0xFE,..} frame this replaced may also work
  // (SetSetpoint above sends a multi-TLV frame fine), but no validated implementation sends a combined 0x4220.
  this->query_(CMD_RESET_FILTER, std::vector<uint8_t>{0x51, 0x01, 0x01}, 200);  // DisableCleanFilterIndicator
  this->query_(CMD_RESET_FILTER, std::vector<uint8_t>{0xFE, 0x01, 0x01}, 200);  // ResetCleanFilterTimer
  if (this->clean_filter_binary_sensor_ != nullptr) {
    this->clean_filter_binary_sensor_->publish_state(false);
  }
  this->should_update_ = true;
}

bool validate_buffer(std::vector<uint8_t> buffer) { return buffer[0] == buffer.size(); }

void Madoka::process_incoming_chunk_(std::vector<uint8_t> chk) {
  if (chk.size() < 2) {
    ESP_LOGI(TAG, "Chunk discarded: invalid length.");
    return;
  }
  uint8_t chunk_id = chk[0];
  std::vector<uint8_t> stripped{chk.begin() + 1, chk.end()};
  if (chunk_id == 0 && validate_buffer(stripped)) {
    this->parse_cb_(stripped);
    return;
  }
  if (this->pending_chunks_.count(chunk_id)) {
    if (chunk_id == 0) {
      // New incoming message: the previous buffer is stale, so clear it.
      ESP_LOGW(TAG, "New message detected, clearing incomplete buffer (chunk_id=0).");
      this->pending_chunks_.clear();
    } else {
      ESP_LOGE(TAG, "Another packet with the same chunk ID is already in the buffer.");
      ESP_LOGD(TAG, "Chunk ID: %d.", chunk_id);
      return;
    }
  }
  this->pending_chunks_[chunk_id] = chk;

  if (this->pending_chunks_.size() != this->pending_chunks_.rbegin()->first + 1) {
    ESP_LOGW(TAG, "Buffer is missing packets");
    return;
  }

  std::vector<uint8_t> msg;
  int lim = this->pending_chunks_.size();
  for (int i = 0; i < lim; i++) {
    msg.insert(msg.end(), this->pending_chunks_[i].begin() + 1, this->pending_chunks_[i].end());
  }
  if (validate_buffer(msg)) {
    this->pending_chunks_.clear();
    this->parse_cb_(msg);
  }
}

std::vector<std::vector<uint8_t>> Madoka::split_payload_(std::vector<uint8_t> msg) {
  std::vector<std::vector<uint8_t>> result;
  size_t len = msg.size();

  // Add leading length byte
  std::vector<uint8_t> buf{(uint8_t) (len + 1)};
  buf.insert(buf.end(), msg.begin(), msg.end());

  for (size_t i = 0; i <= len / (MAX_CHUNK_SIZE - 1); i++) {
    std::vector<uint8_t> chunk{(uint8_t) i};
    chunk.insert(chunk.end(), buf.begin() + (i * (MAX_CHUNK_SIZE - 1)),
                 std::min(buf.end(), buf.begin() + ((i + 1) * (MAX_CHUNK_SIZE - 1))));

    result.push_back(chunk);
  }

  return result;
}

std::vector<uint8_t> Madoka::prepare_message_(uint16_t cmd, std::vector<uint8_t> args) {
  std::vector<uint8_t> result({0x00, (uint8_t) ((cmd >> 8) & 0xFF), (uint8_t) (cmd & 0xFF)});
  result.insert(result.end(), args.begin(), args.end());
  return result;
}

void Madoka::query_(uint16_t cmd, std::vector<uint8_t> args, int t_d) {
  std::vector<uint8_t> payload = this->prepare_message_(cmd, std::move(args));

  if (this->node_state != espbt::ClientState::ESTABLISHED) {
    return;
  }
  std::vector<std::vector<uint8_t>> chunks = this->split_payload_(payload);

  for (auto chk : chunks) {
    esp_err_t status;
    for (int j = 0; j < BLE_SEND_MAX_RETRIES; j++) {
      status = esp_ble_gattc_write_char(this->parent_->get_gattc_if(), this->parent_->get_conn_id(), this->wwr_handle_,
                                        chk.size(), chk.data(), ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
      if (!status) {
        break;
      }
      ESP_LOGD(TAG, "[%s] esp_ble_gattc_write_char failed (%d of %d), status=%d", this->parent_->address_str(),
               j + 1, BLE_SEND_MAX_RETRIES, status);
    }
    if (status) {
      ESP_LOGE(TAG, "[%s] Command could not be sent, last status=%d", this->parent_->address_str(), status);
      return;
    }
  }
  esphome::delay(t_d);
}

void Madoka::parse_cb_(std::vector<uint8_t> msg) {
  // A well-formed frame is at least 4 bytes: the function id lives at msg[2..3].
  // Bail on anything shorter so the header reads below can't run off the end.
  if (msg.size() < 4) {
    ESP_LOGW(TAG, "Discarding short frame (%u bytes)", (unsigned) msg.size());
    return;
  }
  uint16_t function_id = msg[2] << 8 | msg[3];
  // size_t (not uint8_t) so `i + len` cannot wrap and re-enter the loop.
  size_t i = 4;
  size_t message_size = msg.size();

  // Each TLV is [argument_id][len][len payload bytes]. Every loop below:
  //   - requires 2 bytes for argument_id+len  -> `while (i + 1 < message_size)`
  //   - requires the declared payload to fit  -> `if (i + len > message_size) break;`
  //   - checks `len >= N` before reading N payload bytes.
  // These guards are inert for well-formed frames and only reject malformed
  // ones (a malfunctioning/spoofed controller can otherwise drive an OOB read).
  switch (function_id) {
    case CMD_GET_SETTING_STATUS:
      while (i + 1 < message_size) {
        uint8_t argument_id = msg[i++];
        uint8_t len = msg[i++];
        if (i + len > message_size)
          break;
        if (argument_id == 0x20 && len >= 1) {
          this->cur_status_.status = msg[i];
        }
        i += len;
      }
      break;
    case CMD_GET_OPERATION_MODE:
      while (i + 1 < message_size) {
        uint8_t argument_id = msg[i++];
        uint8_t len = msg[i++];
        if (i + len > message_size)
          break;
        if (argument_id == 0x20 && len >= 1) {
          this->cur_status_.mode = msg[i];
          this->op_mode_known_ = true;
        }
        i += len;
      }
      break;
    default:
      break;
  }
  switch (function_id) {
    case CMD_GET_SETTING_STATUS:
    case CMD_GET_OPERATION_MODE:
      // ESP_LOGI(TAG, "status: %d, mode: %d", this->cur_status_.status, this->cur_status_.mode);
      if (this->cur_status_.status) {
        // PER-85: only derive the running mode from cur_status_.mode once we have actually
        // received an operation-mode (CMD_GET_OPERATION_MODE) reply. On a degraded BLE link
        // (e.g. a controller that drops the 0x0030 reply) cur_status_.mode would otherwise be
        // its uninitialised 0 and we'd publish a confidently-wrong FAN_ONLY. Until the first
        // real reply we hold the existing mode (default / restored) instead of fabricating one.
        if (this->op_mode_known_) {
          switch (this->cur_status_.mode) {
            case 0:
              this->mode = climate::CLIMATE_MODE_FAN_ONLY;
              break;
            case 1:
              this->mode = climate::CLIMATE_MODE_DRY;
              break;
            case 2:
              this->mode = climate::CLIMATE_MODE_HEAT_COOL;
              break;
            case 3:
              this->mode = climate::CLIMATE_MODE_COOL;
              break;
            case 4:
              this->mode = climate::CLIMATE_MODE_HEAT;
              break;
          }
        }
      } else {
        this->mode = climate::CLIMATE_MODE_OFF;
      }
      break;
    case CMD_GET_SETPOINT: {
      // The device reports separate cooling (0x20) and heating (0x21) set
      // points; collapse them into the single target_temperature, showing the
      // one relevant to the active mode.
      float cooling_set_point = NAN, heating_set_point = NAN;
      while (i + 1 < message_size) {
        uint8_t argument_id = msg[i++];
        uint8_t len = msg[i++];
        if (i + len > message_size)
          break;
        switch (argument_id) {
          case 0x20:
            if (len >= 2)
              cooling_set_point = (float) (msg[i] << 8 | msg[i + 1]) / 128;
            break;
          case 0x21:
            if (len >= 2)
              heating_set_point = (float) (msg[i] << 8 | msg[i + 1]) / 128;
            break;
        }
        i += len;
      }
      this->target_temperature =
          (this->mode == climate::CLIMATE_MODE_HEAT) ? heating_set_point : cooling_set_point;
      break;
    }
    case CMD_GET_FAN_SPEED: {
      uint8_t fan_mode = 255;
      while (i + 1 < message_size) {
        uint8_t argument_id = msg[i++];
        uint8_t len = msg[i++];
        if (i + len > message_size)
          break;
        if (this->cur_status_.mode == 1) {
        } else if ((argument_id == 0x21 && len == 1 && this->cur_status_.mode == 4) ||
                   (argument_id == 0x20 && len == 1 && this->cur_status_.mode != 4)) {
          fan_mode = msg[i];
        }
        i += len;
      }
      switch (fan_mode) {
        case 0:
          this->fan_mode = climate::CLIMATE_FAN_AUTO;
          break;
        case 1:
          this->fan_mode = climate::CLIMATE_FAN_LOW;
          break;
        case 2:
        case 3:
        case 4:
          this->fan_mode = climate::CLIMATE_FAN_MEDIUM;
          break;
        case 5:
          this->fan_mode = climate::CLIMATE_FAN_HIGH;
          break;
        default:
          break;
      }
      break;
    }
    case CMD_GET_SENSOR_INFORMATION:
      while (i + 1 < message_size) {
        uint8_t argument_id = msg[i++];
        uint8_t len = msg[i++];
        if (i + len > message_size)
          break;
        if (argument_id == 0x40 && len >= 1) {
          this->current_temperature = msg[i];
        } else if (argument_id == 0x41 && this->outdoor_temperature_sensor_ != nullptr && len >= 1) {
          uint8_t value = msg[i];
          if (value != 0xFF) {
            this->outdoor_temperature_sensor_->publish_state(value);
          }
        }
        i += len;
      }
      break;
    case CMD_GET_CLEAN_FILTER:
      while (i + 1 < message_size) {
        uint8_t argument_id = msg[i++];
        uint8_t len = msg[i++];
        if (i + len > message_size)
          break;
        if (argument_id == 0x62 && this->clean_filter_binary_sensor_ != nullptr && len >= 1) {
          this->clean_filter_binary_sensor_->publish_state((msg[i] & 0x01) == 0x01);
        }
        i += len;
      }
      break;
    case CMD_GET_VERSION: {
      std::string rc_version;
      std::string ble_version;
      while (i + 1 < message_size) {
        uint8_t argument_id = msg[i++];
        uint8_t len = msg[i++];
        if (i + len > message_size)
          break;
        if (argument_id == 0x45 && len >= 3) {
          rc_version = std::to_string(msg[i]) + "." + std::to_string(msg[i + 1]) + "." + std::to_string(msg[i + 2]);
        } else if (argument_id == 0x46 && len >= 2) {
          ble_version = std::to_string(msg[i]) + "." + std::to_string(msg[i + 1]);
        }
        i += len;
      }
      if (this->firmware_version_text_sensor_ != nullptr) {
        if (!rc_version.empty() && !ble_version.empty()) {
          this->firmware_version_text_sensor_->publish_state("RC " + rc_version + " / BLE " + ble_version);
        } else if (!rc_version.empty()) {
          this->firmware_version_text_sensor_->publish_state(rc_version);
        } else if (!ble_version.empty()) {
          this->firmware_version_text_sensor_->publish_state("BLE " + ble_version);
        }
      }
      break;
    }
    case CMD_GET_EYE_BRIGHTNESS:
      while (i + 1 < message_size) {
        uint8_t argument_id = msg[i++];
        uint8_t len = msg[i++];
        if (i + len > message_size)
          break;
        if (argument_id == 0x33 && this->eye_brightness_number_ != nullptr && len >= 1) {
          this->eye_brightness_number_->publish_state(msg[i]);
        }
        i += len;
      }
      break;
    default:
      break;
  }

  this->publish_state();
}

}  // namespace madoka
}  // namespace esphome

#endif
