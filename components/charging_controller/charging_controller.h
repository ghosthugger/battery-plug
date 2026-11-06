#pragma once
#include <atomic>
#include <mutex>

#include "esphome/core/defines.h"
#include "charging_core.h"
#include "compact_optimizer.h"
#include "optimizer_history.h"
#include "savings.h"
#include "bms_diagnostics.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/core/component.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

namespace esphome::charging_controller {
extern const uint8_t DASHBOARD[];
extern const size_t DASHBOARD_SIZE;
class Controller : public Component, public ble_client::BLEClientNode, public AsyncWebHandler {
 public:
  Controller() { node_state = esp32_ble_tracker::ClientState::IDLE; }
  void setup() override;
  void dump_config() override;
  void loop() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }
  void set_relay(switch_::Switch* v) { relay_ = v; }
  void set_power(sensor::Sensor* v) { power_ = v; }
  void set_clock(time::RealTimeClock* v) { clock_ = v; }
  void set_status(text_sensor::TextSensor* v) { status_ = v; }
  void request_mode(int mode);
  bool always_charging() const { return control_.mode==charging::Mode::ALWAYS; }
  void button_toggle() { request_mode(always_charging()?1:3); }
  void overcurrent() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    overcurrent_ = true;
    relay_->turn_off();
    persist_settings_();
    control_.reason = charging::Reason::OVERCURRENT;
  }
  void prepare_ota() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    ota_ = true;
    relay_->turn_off();
    parent_->set_enabled(false);
  }
  void gattc_event_handler(esp_gattc_cb_event_t, esp_gatt_if_t, esp_ble_gattc_cb_param_t*) override;
  bool canHandle(AsyncWebServerRequest*) const override;
  void handleRequest(AsyncWebServerRequest*) override;

 protected:
  switch_::Switch* relay_{};
  sensor::Sensor* power_{};
  time::RealTimeClock* clock_{};
  text_sensor::TextSensor* status_{};
  charging::Settings settings_{};
  charging::Control control_{};
  charging::Plan plan_{};
  charging::Battery battery_{};
  charging::History history_{};
  charging::OptimizerSettings optimizer_settings_{};
  charging::OptimizerHistory optimizer_history_{};
  charging::Savings savings_{};
  charging::BmsDiagnostics bms_diagnostics_{};
  bool savings_continuous_{false};
  uint32_t savings_saved_ms_{0}, output_changed_ms_{0};
  bool optimizer_ready_{false}, optimizer_request_{false};
  // Display-only, indexed by published prices; never drives relay control.
  std::array<uint16_t,charging::PRICE_COUNT> projected_mains_{};
  bool projection_ready_{false};
  float forecast_ore_{charging::UNKNOWN}, continuation_ore_{charging::UNKNOWN}, predicted_load_w_{60};
  uint32_t optimizer_until_{0}, optimizer_ms_{0}, observation_ms_{0}, observation_hour_{0};
  std::array<charging::Price, charging::PRICE_COUNT> prices_{};
  size_t price_count_{0};
  nvs_handle_t nvs_{0};
  uint32_t last_bms_ms_{0}, last_power_ms_{0}, tick_ms_{0}, poll_ms_{0}, last_plan_ms_{0}, last_fetch_ms_{0},
      last_record_ms_{0}, reconnect_ms_{0}, backoff_ms_{5000},
      min_heap_{UINT32_MAX}, min_block_{UINT32_MAX};
  uint16_t notify_handle_{0}, write_handle_{0};
  charging::DalyFrames frames_{};
  bool ready_{false}, connected_{false}, ota_{false}, overcurrent_{false}, plan_dirty_{true},
      storage_ok_{true}, network_enabled_{false}, had_bms_{false}, last_output_{false};
  float reserve_threshold_{5}, reserve_recovery_{10};
  float load_w_{0}, gross_charge_w_{0}, fx_{0}, meter_w_{charging::UNKNOWN};
  uint32_t fx_time_{0}, fetch_now_{0};
  uint16_t history_written_{0};
  int last_reason_{-1};
  struct FetchResult {
    std::array<charging::Price, charging::PRICE_COUNT> prices{};
    size_t count{0};
    float rate{0};
    uint32_t rate_time{0};
    char error[96]{};
  };
  static_assert(sizeof(charging::History) + sizeof(charging::Plan) + sizeof(prices_) + sizeof(FetchResult) +
                    sizeof(charging::OptimizerHistory) <
                    24 * 1024,
                "Retained history/prices, including fetch staging, exceed the RAM budget");
  FetchResult fetch_result_{};
  std::atomic<bool> fetching_{false}, fetch_ready_{false};
  std::atomic<bool> battery_ready_{false};
  charging::Battery pending_battery_{};
  char fetch_error_[96]{"Not fetched yet"};
  unsigned fetch_failures_{0};
  std::recursive_mutex mutex_;
  void poll_bms_();
  void tick_();
  void record_(const charging::Inputs&, float);
  charging::Inputs inputs_() const;
  void persist_settings_();
  void log_event_(unsigned, charging::Reason, float = 0);
  void restore_();
  void persist_history_(uint32_t);
  void save_prices_();
  void update_optimizer_(const charging::Inputs&);
  void persist_optimizer_();
  void start_fetch_();
  static void fetch_task_(void*);
  void status_json_(AsyncWebServerRequest*);
  void history_json_(AsyncWebServerRequest*, bool);
  void schedule_json_(AsyncWebServerRequest*);
  void events_json_(AsyncWebServerRequest*);
  void savings_json_(AsyncWebServerRequest*);
  void bms_diagnostics_json_(AsyncWebServerRequest*);
};
}  // namespace esphome::charging_controller
