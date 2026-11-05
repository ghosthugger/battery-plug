#include "charging_controller.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/wifi/wifi_component.h"
#include "esphome/core/log.h"

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic error "-Wstack-usage=2048"
#endif

namespace esphome::charging_controller {
using namespace charging;
static const char* const TAG = "charging";
static constexpr uint32_t STORAGE_VERSION = 1;
template <class T>
struct Record {
  uint32_t version{STORAGE_VERSION};
  T data{};
  uint32_t checksum{0};
};
template <class T>
bool load_record(nvs_handle_t nvs, const char* key, T& data) {
  if (!nvs) return false;
  auto r = std::make_unique<Record<T>>();
  size_t n = sizeof(*r);
  if (nvs_get_blob(nvs, key, r.get(), &n) != ESP_OK || n != sizeof(*r) || r->version != STORAGE_VERSION ||
      r->checksum != crc32(r.get(), sizeof(*r) - 4))
    return false;
  data = r->data;
  return true;
}
template <class T>
bool save_record(nvs_handle_t nvs, const char* key, const T& data) {
  if (!nvs) return false;
  auto r = std::make_unique<Record<T>>();
  r->data = data;
  r->checksum = crc32(r.get(), sizeof(*r) - 4);
  return nvs_set_blob(nvs, key, r.get(), sizeof(*r)) == ESP_OK;
}
struct StoredSettings {
  Settings settings{};
  uint32_t automatic{0};
};
struct PriceMetadata {
  uint32_t count{0}, rate_time{0};
  float rate{0};
};
using Hour = std::array<Bucket, 12>;
using Month = std::array<Day, 30>;
using Events = std::array<Event, 16>;
static_assert((sizeof(Record<Hour>) + 95) / 32 * 32 * 24 + (sizeof(Record<Month>) + 95) / 32 * 32 * 6 +
                      (sizeof(Record<Events>) + 95) / 32 * 32 * 8 + 1024 <
                  48 * 1024,
              "Persistent history exceeds budget");
static std::string iso(uint32_t value) {
  auto t = utc_parts(value);
  char b[32];
  snprintf(b, sizeof(b), "%04d-%02d-%02dT%02d:%02d:%02dZ", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
           t.tm_hour, t.tm_min, t.tm_sec);
  return b;
}
static std::string get_https(const std::string& url) {
  esp_http_client_config_t cfg{};
  cfg.url = url.c_str();
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.timeout_ms = 8000;
  cfg.buffer_size = 1024;
  cfg.buffer_size_tx = 1024;
  cfg.disable_auto_redirect = true;
  auto client = esp_http_client_init(&cfg);
  if (!client) return {};
  std::string result;
  if (esp_http_client_open(client, 0) == ESP_OK) {
    int length = esp_http_client_fetch_headers(client);
    if (esp_http_client_get_status_code(client) == 200 && length <= 24576) {
      char buf[768];
      int n;
      while ((n = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
        if (result.size() + n > 24576) {
          result.clear();
          break;
        }
        result.append(buf, n);
      }
      if (n < 0 || !esp_http_client_is_complete_data_received(client)) result.clear();
    }
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return result;
}
void Controller::restore_() {
  storage_ok_ = nvs_open("charging_v1", NVS_READWRITE, &nvs_) == ESP_OK;
  if (!storage_ok_) return;
  StoredSettings s;
  if (load_record(nvs_, "settings", s) && valid_settings(s.settings)) {
    settings_ = s.settings;
    control_.mode = s.automatic ? Mode::AUTO : Mode::OFF;
  }
  float reserve = 5;
  if (load_record(nvs_, "reserve", reserve) && std::isfinite(reserve) && reserve >= 0 && reserve <= 95)
    reserve_threshold_ = reserve;
  reserve_recovery_ = reserve_threshold_ + 5; // Preserve the earlier release level on upgrade.
  float recovery;
  if (load_record(nvs_, "reserve_end", recovery) && valid_reserve_settings(reserve_threshold_, recovery))
    reserve_recovery_ = recovery;
  load_record(nvs_, "reserve_active", control_.reserve_active);
  OptimizerSettings optimizer;
  if (load_record(nvs_, "optimizer_cfg", optimizer) && valid_optimizer_settings(optimizer))
    optimizer_settings_ = optimizer;
  load_record(nvs_, "optimizer_hist", optimizer_history_);
  if (control_.mode == Mode::OFF || reserve_threshold_ == 0) control_.reserve_active = false;
  char key[12];
  for (size_t i = 0; i < 24; ++i) {
    Hour h;
    snprintf(key, sizeof(key), "hour%02u", unsigned(i));
    if (load_record(nvs_, key, h)) std::copy(h.begin(), h.end(), history_.buckets.begin() + i * 12);
  }
  if constexpr (RETAINED_DAYS > 0) {
    for (size_t i = 0; i < 6; ++i) {
      Month m;
      snprintf(key, sizeof(key), "month%u", unsigned(i));
      if (load_record(nvs_, key, m)) std::copy(m.begin(), m.end(), history_.days.begin() + i * 30);
    }
    // Recover partial daily totals from retained buckets if a daily block is
    // absent, corrupt, or from the earlier development record layout.
    std::array<bool, DAY_COUNT> rebuild{};
    for (auto& b : history_.buckets)
      if (b.time) {
        auto k = day_key(b.time);
        if (history_.days[k % DAY_COUNT].day != k) rebuild[k % DAY_COUNT] = true;
      }
    for (auto& b : history_.buckets)
      if (b.time && rebuild[day_key(b.time) % DAY_COUNT]) history_.add_to_day(b);
  }
  for (size_t i = 0; i < 8; ++i) {
    Events e;
    snprintf(key, sizeof(key), "events%u", unsigned(i));
    if (load_record(nvs_, key, e)) std::copy(e.begin(), e.end(), history_.events.begin() + i * 16);
  }
  load_record(nvs_, "event_seq", history_.event_seq);
  PriceMetadata meta;
  if (load_record(nvs_, "price_meta", meta) && meta.count <= PRICE_COUNT && meta.rate > 0) {
    if (load_record(nvs_, "prices", prices_)) {
      price_count_ = meta.count;
      fx_ = meta.rate;
      fx_time_ = meta.rate_time;
    }
  }
}
void Controller::setup() {
  ESP_LOGI(TAG, "Starting local charging controller");
  relay_->turn_off();
  node_state = esp32_ble_tracker::ClientState::IDLE;
  restore_();
  ESP_LOGI(TAG, "Persistent records loaded");
  gross_charge_w_ = settings_.charger_w * settings_.efficiency;
  power_->add_on_state_callback([this](float value) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    meter_w_ = value;
    if (std::isfinite(value) && value >= 0 && value <= 4000) last_power_ms_ = millis();
  });
  auto* base = web_server_base::global_web_server_base;
  base->init();
  base->add_handler(this);
  ready_ = true;
  ESP_LOGI(TAG, "Controller ready; %u bytes history/price buffers",
           unsigned(sizeof(history_) + sizeof(plan_) + sizeof(prices_)));
}
void Controller::dump_config() {
  ESP_LOGI(TAG, "Controller setup=%s, tick=%u, BMS=%s, prices=%u", ready_ ? "ready" : "pending",
           unsigned(tick_ms_), connected_ ? "connected" : "disconnected", unsigned(price_count_));
}
void Controller::on_shutdown() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  relay_->turn_off();
  persist_optimizer_();
  if (nvs_) nvs_commit(nvs_);
}
void Controller::persist_optimizer_() {
  storage_ok_ &= save_record(nvs_, "optimizer_cfg", optimizer_settings_);
  storage_ok_ &= save_record(nvs_, "optimizer_hist", optimizer_history_);
  if (nvs_) nvs_commit(nvs_);
}
void Controller::update_optimizer_(const Inputs& in) {
  uint32_t started=millis();
  plan_=Plan{};plan_.at=in.now;optimizer_ready_=false;
  optimizer_until_=in.now/900*900+900;
  uint32_t contiguous=in.now;
  for(size_t i=0;i<price_count_;++i) {
    const auto& x=prices_[i];
    if(!std::isfinite(x.ore)) continue;
    if(x.start<=in.now && in.now<x.start+900) {plan_.current=i;plan_.current_price=x.ore;}
    if(x.start<=contiguous && x.start+900>contiguous) contiguous=x.start+900;
  }
  plan_.horizon=contiguous;
  if(!in.clock_ok || !in.bms_fresh || !std::isfinite(in.soc) || in.soc<0 || in.soc>100 ||
     plan_.current<0 || reserve_threshold_<=0 || !valid_reserve_settings(reserve_threshold_,reserve_recovery_)) return;
  optimizer_history_.ingest_prices(in.now,prices_.data(),price_count_);
  optimizer_history_.price_estimates(in.now,prices_.data(),price_count_,forecast_ore_,continuation_ore_);
  if(!std::isfinite(forecast_ore_) || !std::isfinite(continuation_ore_)) return;
  predicted_load_w_=optimizer_history_.load_w(in.now,optimizer_settings_.fallback_load_w);
  // Workspace exists only during a solve; do not retain a full policy table.
  auto solver=std::unique_ptr<compact::Solver<101>>(new(std::nothrow) compact::Solver<101>);
  if(!solver) return;
  compact::Config p;
  p.capacity=settings_.capacity_kwh;p.reserve=p.capacity*reserve_threshold_/100;
  p.recovery=p.capacity*reserve_recovery_/100;p.charge=settings_.charger_w/1000;
  p.eta_c=optimizer_settings_.charge_efficiency;p.eta_d=optimizer_settings_.discharge_efficiency;
  p.idle=optimizer_settings_.idle_w/1000;
  int count=0;
  for(size_t i=plan_.current;i<price_count_ && prices_[i].start<contiguous;++i)
    solver->prices[count++]=(prices_[i].ore+optimizer_settings_.marginal_adder)/100;
  int published=count;
  for(int j=0;j<96 && count<compact::MAX_HORIZON;++j)
    solver->prices[count++]=(forecast_ore_+optimizer_settings_.marginal_adder)/100;
  plan_.deadline=in.now/900*900+count*900;plan_.provisional=count>published;
  double terminal=std::max(0.f,continuation_ore_+optimizer_settings_.marginal_adder)/100/p.eta_c;
  optimizer_request_=solver->choose(p,count,predicted_load_w_/1000,in.soc*p.capacity/100,
      control_.reserve_active,relay_->state?1:0,terminal,(optimizer_until_-in.now)/3600.)==1;
  optimizer_ready_=true;
  if(optimizer_request_) plan_.seconds[plan_.current]=optimizer_until_-in.now;
  optimizer_ms_=millis()-started;
  min_heap_=std::min(min_heap_,uint32_t(heap_caps_get_free_size(MALLOC_CAP_8BIT)));
  min_block_=std::min(min_block_,uint32_t(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}
void Controller::sync_settings_() {
  auto t = finish_->state_as_esptime();
  Settings s = settings_;
  if (std::isfinite(target_->state)) s.target = target_->state;
  s.hour = t.hour;
  s.minute = t.minute;
  if (valid_settings(s) &&
      (s.target != settings_.target || s.hour != settings_.hour || s.minute != settings_.minute)) {
    settings_ = s;
    plan_dirty_ = true;
    control_.target_latched = false;
    persist_settings_();
    cycle_deadline_ = 0;
    if (control_.mode == Mode::NOW && clock_->now().is_valid())
      control_.manual_deadline = next_deadline(clock_->now().timestamp, s.hour, s.minute);
  }
}
void Controller::persist_settings_() {
  storage_ok_ &= save_record(nvs_, "reserve", reserve_threshold_);
  storage_ok_ &= save_record(nvs_, "reserve_end", reserve_recovery_);
  storage_ok_ &= save_record(nvs_, "reserve_active", control_.reserve_active);
  storage_ok_ &=
      save_record(nvs_, "settings", StoredSettings{settings_, control_.mode == Mode::AUTO ? 1U : 0U});
  if (nvs_) storage_ok_ &= nvs_commit(nvs_) == ESP_OK;
}
void Controller::request_mode(int value) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (value < 0 || value > 2) return;
  sync_settings_();
  auto now = clock_->now();
  if (value == 2 && !now.is_valid()) return;
  if (value != 0) overcurrent_ = false;
  control_.set_mode(Mode(value), now.timestamp, settings_);
  if (value == 0) {
    if (relay_->state) control_.last_switch = now.timestamp;
    relay_->turn_off();
    cycle_deadline_ = 0;
  }
  plan_dirty_ = true;
  persist_settings_();
  if (now.is_valid()) log_event_(1, Reason::OFF, float(value));
}
void Controller::log_event_(unsigned kind, Reason why, float value) {
  auto now = clock_->now();
  if (!now.is_valid()) return;
  history_.event(now.timestamp, kind, why, battery_.soc, value);
  unsigned block = ((history_.event_seq - 1) % EVENT_COUNT) / 16;
  Events e;
  std::copy_n(history_.events.begin() + block * 16, 16, e.begin());
  char key[12];
  snprintf(key, sizeof(key), "events%u", block);
  storage_ok_ &= save_record(nvs_, key, e);
  storage_ok_ &= save_record(nvs_, "event_seq", history_.event_seq);
  if (nvs_) storage_ok_ &= nvs_commit(nvs_) == ESP_OK;
}
void Controller::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t, esp_ble_gattc_cb_param_t* p) {
  if (event == ESP_GATTC_SEARCH_CMPL_EVT) {
    auto *notify = parent_->get_characteristic(0xfff0, 0xfff1),
         *write = parent_->get_characteristic(0xfff0, 0xfff2);
    if (!notify || !write) return;
    notify_handle_ = notify->handle;
    write_handle_ = write->handle;
    parent_->register_for_notify(notify_handle_);
  } else if (event == ESP_GATTC_REG_FOR_NOTIFY_EVT && p->reg_for_notify.status == ESP_GATT_OK) {
    node_state = esp32_ble_tracker::ClientState::ESTABLISHED;
    connected_ = true;
    frames_.reset();
    poll_ms_ = millis() - 10000;
  } else if (event == ESP_GATTC_DISCONNECT_EVT) {
    connected_ = false;
    frames_.reset();
    notify_handle_ = write_handle_ = 0;
    node_state = esp32_ble_tracker::ClientState::IDLE;
  } else if (event == ESP_GATTC_NOTIFY_EVT && p->notify.handle == notify_handle_) {
    const auto* b = p->notify.value;
    size_t n = p->notify.value_len;
    Battery decoded;
    if (frames_.push(b, n, decoded) && !battery_ready_.load()) {
      pending_battery_ = decoded;
      battery_ready_.store(true, std::memory_order_release);
    }
  }
}
void Controller::poll_bms_() {
  uint8_t request[] = {0xd2, 3, 0, 0, 0, 0x3e, 0, 0};
  auto c = crc16(request, 6);
  request[6] = c;
  request[7] = c >> 8;
  frames_.reset();
  esp_ble_gattc_write_char(parent_->get_gattc_if(), parent_->get_conn_id(), write_handle_, sizeof(request),
                           request, ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
}
Inputs Controller::inputs_() const {
  Inputs in;
  auto t = clock_->now();
  in.now = t.timestamp;
  in.clock_ok = t.is_valid();
  uint32_t ms = millis();
  in.bms_fresh = last_bms_ms_ && ms - last_bms_ms_ < 60000;
  in.meter_fresh = last_power_ms_ && ms - last_power_ms_ < 30000 && std::isfinite(meter_w_) &&
                   meter_w_ >= 0 && meter_w_ <= 4000;
  in.soc = battery_.soc;
  in.fault = !battery_.charge_allowed || overcurrent_ || ota_;
  in.load_w = load_w_;
  in.effective_charge_w = std::max(0.0f, gross_charge_w_ - load_w_);
  return in;
}
void Controller::persist_history_(uint32_t timestamp) {
  char key[12];
  unsigned block = ((timestamp / 300) % BUCKET_COUNT) / 12;
  Hour h;
  std::copy_n(history_.buckets.begin() + block * 12, 12, h.begin());
  snprintf(key, sizeof(key), "hour%02u", block);
  storage_ok_ &= save_record(nvs_, key, h);
  if constexpr (RETAINED_DAYS > 0) {
    block = (day_key(timestamp) % DAY_COUNT) / 30;
    Month m;
    std::copy_n(history_.days.begin() + block * 30, 30, m.begin());
    snprintf(key, sizeof(key), "month%u", block);
    storage_ok_ &= save_record(nvs_, key, m);
  }
  if (nvs_) storage_ok_ &= nvs_commit(nvs_) == ESP_OK;
}
void Controller::record_(const Inputs& in, float seconds) {
  if (!in.clock_ok) return;
  uint32_t previous = history_.active.time;
  bool complete = history_.add(in.now, seconds, meter_w_, in.bms_fresh ? battery_.soc : UNKNOWN,
                               relay_->state, plan_.current_price, settings_.ceiling, in.meter_fresh);
  if (complete) persist_history_(previous);
}
void Controller::tick_() {
  sync_settings_();
  auto in = inputs_();
  uint32_t ms = millis();
  if (in.clock_ok && !network_enabled_) {
    network_enabled_ = true;
    log_event_(0, Reason::OFF);
  }
  if (control_.mode != Mode::AUTO && cycle_deadline_ && in.clock_ok && in.now >= cycle_deadline_) {
    bool met = in.bms_fresh && in.soc >= settings_.target;
    history_.deadline(cycle_deadline_ - 1, met);
    persist_history_(cycle_deadline_ - 1);
    log_event_(4, Reason::DEADLINE, met ? 1 : 0);
    control_.cycle_met = false;
    cycle_deadline_ = 0;
    plan_dirty_ = true;
  }
  bool automatic=control_.mode==Mode::AUTO;
  bool replan=automatic ? (plan_dirty_ || in.now>=optimizer_until_) :
      (plan_dirty_ || ms-last_plan_ms_>=60000 ||
       (plan_.current>=0 && in.now>=prices_[plan_.current].start+900));
  if (in.clock_ok && replan && (!automatic || !fetching_.load())) {
    if(automatic) update_optimizer_(in);
    else {
      plan_ = make_plan(settings_, in, prices_.data(), price_count_);
      // There is no committed automatic schedule in Off/Charge now modes.
      plan_.seconds.fill(0);
      if(control_.mode==Mode::NOW && plan_.current>=0)
        plan_.seconds[plan_.current]=prices_[plan_.current].start+900-in.now;
    }
    last_plan_ms_ = ms;
    plan_dirty_ = false;
    if (!automatic && !cycle_deadline_) cycle_deadline_ = plan_.deadline;
  }
  if(automatic) {
    // A fetch may still be running when a slot ends. A short hold of the last
    // economic choice must never turn a missing/expired price into a valid one.
    plan_.current=-1;plan_.current_price=UNKNOWN;
    for(size_t i=0;i<price_count_;++i)
      if(prices_[i].start<=in.now && in.now<prices_[i].start+900 && std::isfinite(prices_[i].ore)) {
        plan_.current=i;plan_.current_price=prices_[i].ore;break;
      }
  }
  bool was_reserve = control_.reserve_active;
  bool desired = control_.evaluate_optimized(settings_, in, plan_, prices_.data(),
      optimizer_ready_ && in.now<optimizer_until_+30,optimizer_request_,reserve_threshold_,reserve_recovery_);
  if (was_reserve != control_.reserve_active) {
    storage_ok_ &= save_record(nvs_, "reserve_active", control_.reserve_active);
    if (nvs_) storage_ok_ &= nvs_commit(nvs_) == ESP_OK;
  }
  if (ota_) {
    desired = false;
    control_.reason = Reason::OTA;
  }
  if (overcurrent_) {
    desired = false;
    control_.reason = Reason::OVERCURRENT;
  }
  if (desired != relay_->state) {
    if (desired)
      relay_->turn_on();
    else
      relay_->turn_off();
  }
  if (last_output_ != relay_->state || last_reason_ != int(control_.reason)) {
    log_event_(last_output_ != relay_->state ? 2 : 3, control_.reason, relay_->state ? 1 : 0);
    last_output_ = relay_->state;
    last_reason_ = int(control_.reason);
    status_->publish_state(reason_text(control_.reason));
  }
  if (ms - last_record_ms_ >= 10000) {
    float dt = last_record_ms_ ? (ms - last_record_ms_) / 1000.0f : 0;
    record_(in, dt);
    last_record_ms_ = ms;
  }
  min_heap_ = std::min(min_heap_, uint32_t(heap_caps_get_free_size(MALLOC_CAP_8BIT)));
  min_block_ = std::min(min_block_, uint32_t(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}
void Controller::loop() {
  // BLEClient invokes every registered node's loop during its own loop, which
  // can run while App is still waiting for Wi-Fi and before our setup priority.
  if (!ready_) return;
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  uint32_t ms = millis();
  if (battery_ready_.load(std::memory_order_acquire)) {
    float old = battery_.soc;
    battery_ = pending_battery_;
    battery_ready_.store(false);
    last_bms_ms_ = ms;
    if (!had_bms_) {
      had_bms_ = true;
      plan_dirty_ = true;
      log_event_(5, Reason::BMS, 1);
    }
    if (!std::isfinite(old) || std::abs(old - battery_.soc) >= 0.5f) plan_dirty_ = true;
    float net = battery_.voltage * battery_.current;
    if (!relay_->state)
      load_w_ = 0.9f * load_w_ + 0.1f * std::max(0.0f, -net);
    else if (std::isfinite(meter_w_) && meter_w_ > 20) {
      float observed = std::clamp(net + load_w_, 0.0f, settings_.charger_w);
      gross_charge_w_ = 0.9f * gross_charge_w_ + 0.1f * observed;
    }
    uint32_t now=clock_->now().timestamp;
    if(clock_->now().is_valid()) {
      if(!relay_->state && !ota_ && std::isfinite(net))
        optimizer_history_.observe_load(now,std::max(0.f,(-net-optimizer_settings_.idle_w)*
            optimizer_settings_.discharge_efficiency),observation_ms_?(ms-observation_ms_)/1000.f:0);
      if(observation_hour_ && observation_hour_!=now/3600) persist_optimizer_();
      observation_hour_=now/3600;
    }
    observation_ms_=ms;
    backoff_ms_ = 5000;
  }
  if (connected_ && ms - poll_ms_ >= 10000) {
    poll_ms_ = ms;
    poll_bms_();
  }
  if (!ota_ && (!last_bms_ms_ || ms - last_bms_ms_ > 45000) && ms - reconnect_ms_ > backoff_ms_) {
    reconnect_ms_ = ms;
    parent_->set_enabled(false);
    connected_ = false;
    set_timeout("reconnect", 1000, [this] {
      if (!ota_) parent_->set_enabled(true);
    });
    backoff_ms_ = std::min<uint32_t>(60000U, backoff_ms_ * 2);
  }
  if (had_bms_ && last_bms_ms_ && ms - last_bms_ms_ >= 60000) {
    had_bms_ = false;
    log_event_(5, Reason::BMS, 0);
  }
  if (fetch_ready_.load(std::memory_order_acquire)) {
    if (fetch_result_.count) {
      prices_ = fetch_result_.prices;
      price_count_ = fetch_result_.count;
      fx_ = fetch_result_.rate;
      fx_time_ = fetch_result_.rate_time;
      fetch_error_[0] = 0;
      fetch_failures_ = 0;
      plan_dirty_ = true;
      save_prices_();
    } else {
      snprintf(fetch_error_, sizeof(fetch_error_), "%s", fetch_result_.error);
      ++fetch_failures_;
    }
    fetch_ready_.store(false);
    fetching_.store(false);
  }
  if (ms - tick_ms_ >= 1000) {
    tick_ms_ = ms;
    tick_();
  }
  uint32_t interval =
      fetch_failures_ ? std::min(900000U, 30000U << std::min(fetch_failures_ - 1, 5U)) : 900000U;
  if (!ota_ && network_enabled_ && wifi::global_wifi_component->is_connected() &&
      (!last_fetch_ms_ || ms - last_fetch_ms_ >= interval) && !fetching_.load())
    start_fetch_();
}
void Controller::save_prices_() {
  storage_ok_ &= save_record(nvs_, "prices", prices_);
  storage_ok_ &= save_record(nvs_, "price_meta", PriceMetadata{uint32_t(price_count_), fx_time_, fx_});
  if (nvs_) storage_ok_ &= nvs_commit(nvs_) == ESP_OK;
}
void Controller::start_fetch_() {
  // TLS and JSON parsing run on a separate low-priority task so relay fault
  // evaluation and BLE processing continue during slow network requests.
  fetching_.store(true);
  last_fetch_ms_ = millis();
  fetch_result_ = FetchResult{};
  fetch_now_ = clock_->now().timestamp;
  fetch_result_.rate = fx_;
  fetch_result_.rate_time = fx_time_;
  if (xTaskCreate(fetch_task_, "prices", 8192, this, 1, nullptr) != pdPASS) {
    fetching_.store(false);
    ++fetch_failures_;
  }
}
void Controller::fetch_task_(void* arg) {
  auto* self = static_cast<Controller*>(arg);
  {
    auto& out = self->fetch_result_;
    uint32_t now = self->fetch_now_;
    const std::string base = "https://private.gridio.energy/api/v1/";
    const char* error = "Price request failed";
    if (!out.rate_time || now - out.rate_time >= 86400 || !std::isfinite(out.rate) || out.rate <= 0) {
      auto body = get_https(base + "currencies/0191c705-6efb-74f7-925e-faefbe529629/rate");
      bool ok = json::parse_json(body, [&](JsonObject root) {
        if (!root["rate"].is<float>()) return false;
        out.rate = root["rate"].as<float>();
        return std::isfinite(out.rate) && out.rate > 0 && out.rate < 100;
      });
      if (ok)
        out.rate_time = now;
      else {
        out.rate = 0;
        error = "SEK conversion request failed";
      }
    }
    if (out.rate > 0) {
      uint32_t day = day_key(now), start = stockholm_wall_epoch(day, 0, 0),
               end = stockholm_wall_epoch(day + 2, 0, 0);
      std::string url = base +
                        "service_providers/0191e65f-72a3-72ee-8441-92abaf2d6d8a/prices?start=" + iso(start) +
                        "&end=" + iso(end) + "&resolution=PT15M";
      auto body = get_https(url);
      bool ok = price_objects(body, [&](std::string_view object) {
        return json::parse_json(
            reinterpret_cast<const uint8_t*>(object.data()), object.size(), [&](JsonObject x) {
              if (out.count >= PRICE_COUNT) return false;
              const char* timestamp = x["timestamp"].as<const char*>();
              uint32_t ts = timestamp ? parse_utc_timestamp(timestamp) : 0;
              if (!x["value"].is<float>()) return false;
              float raw = x["value"].as<float>();
              if (!ts || ts < start || ts >= end || ts % 900 || !std::isfinite(raw) || std::abs(raw) > 100000)
                return false;
              if (out.count && ts <= out.prices[out.count - 1].start) return false;
              out.prices[out.count++] = {ts, converted_price(raw, out.rate)};
              return true;
            });
      });
      if (!ok || !out.count) out.count = 0;
    }
    if (!out.count) snprintf(out.error, sizeof(out.error), "%s", error);
  }  // Destroy request strings before deleting this FreeRTOS task.
  self->fetch_ready_.store(true, std::memory_order_release);
  vTaskDelete(nullptr);
}
// ESPHome's IDF response stream buffers its full response. Use IDF chunk sends
// directly for bounded history/schedule output instead.
static void begin_json(AsyncWebServerRequest* r) {
  r->beginResponse(200, "application/json");
  httpd_resp_set_hdr(*r, "Cache-Control", "no-store");
}
static bool chunk(AsyncWebServerRequest* r, const char* s) {
  return httpd_resp_send_chunk(*r, s, strlen(s)) == ESP_OK;
}
static void end_json(AsyncWebServerRequest* r) { httpd_resp_send_chunk(*r, nullptr, 0); }
static const char* num(float n, char* buf, size_t size) {
  if (std::isfinite(n))
    snprintf(buf, size, "%.3f", double(n));
  else
    snprintf(buf, size, "null");
  return buf;
}
bool Controller::canHandle(AsyncWebServerRequest* r) const {
  char path[AsyncWebServerRequest::URL_BUF_SIZE];
  auto u = r->url_to(path);
  return u == "/charging" || u == "/charging/" || u.starts_with("/charging/api/");
}
void Controller::handleRequest(AsyncWebServerRequest* r) {
  char path[AsyncWebServerRequest::URL_BUF_SIZE];
  auto u = r->url_to(path);
  if ((u == "/charging" || u == "/charging/") && r->method() == HTTP_GET) {
    auto* response = r->beginResponse(200, "text/html", DASHBOARD, DASHBOARD_SIZE);
    response->addHeader("Content-Encoding", "gzip");
    r->send(response);
    return;
  }
  if (r->method() == HTTP_GET) {
    if (u == "/charging/api/status") {
      status_json_(r);
      return;
    }
    if (u == "/charging/api/history") {
      history_json_(r, false);
      return;
    }
    if (u == "/charging/api/days") {
      history_json_(r, true);
      return;
    }
    if (u == "/charging/api/schedule") {
      schedule_json_(r);
      return;
    }
    if (u == "/charging/api/events") {
      events_json_(r);
      return;
    }
  }
  if (r->method() == HTTP_POST && u == "/charging/api/settings") {
    Settings s;
    OptimizerSettings optimizer;
    float reserve, recovery;
    {
      std::lock_guard<std::recursive_mutex> lock(mutex_);
      s = settings_;
      optimizer = optimizer_settings_;
      reserve = reserve_threshold_;
      recovery = reserve_recovery_;
    }
    auto read = [&](const char* key, float& dest) {
      if (!r->hasArg(key)) return true;
      auto value = r->arg(key);
      char* end;
      float f = strtof(value.c_str(), &end);
      if (value.empty() || *end || !std::isfinite(f)) return false;
      dest = f;
      return true;
    };
    bool valid = read("reserve_threshold", reserve) && read("reserve_recovery", recovery) &&
                 valid_reserve_settings(reserve, recovery) &&
                 read("target", s.target) && read("ceiling", s.ceiling) &&
                 read("capacity_kwh", s.capacity_kwh) && read("charger_w", s.charger_w) &&
                 read("efficiency", s.efficiency) &&
                 read("marginal_adder",optimizer.marginal_adder) &&
                 read("charge_efficiency",optimizer.charge_efficiency) &&
                 read("discharge_efficiency",optimizer.discharge_efficiency) &&
                 read("idle_w",optimizer.idle_w) && read("fallback_load_w",optimizer.fallback_load_w) &&
                 valid_optimizer_settings(optimizer);
    if (r->hasArg("finish")) {
      auto v = r->arg("finish");
      int h, m;
      char extra;
      if (sscanf(v.c_str(), "%d:%d%c", &h, &m, &extra) != 2 || h < 0 || h > 23 || m < 0 || m > 59)
        valid = false;
      else {
        s.hour = h;
        s.minute = m;
      }
    }
    if (!valid || !valid_settings(s)) {
      r->send(400, "application/json", "{\"error\":\"Invalid settings\"}");
      return;
    }
    defer([this, s, optimizer, reserve, recovery] {
      std::lock_guard<std::recursive_mutex> lock(mutex_);
      if(optimizer.discharge_efficiency!=optimizer_settings_.discharge_efficiency ||
          optimizer.idle_w!=optimizer_settings_.idle_w)
        for(auto& h:optimizer_history_.hours) {h.load_wh=0;h.off_seconds=0;}
      settings_ = s;
      optimizer_settings_ = optimizer;
      reserve_threshold_ = reserve;
      reserve_recovery_ = recovery;
      if (reserve == 0) control_.reserve_active = false;
      auto c = target_->make_call();
      c.set_value(s.target);
      c.perform();
      auto t = finish_->make_call();
      t.set_time(s.hour, s.minute, 0);
      t.perform();
      gross_charge_w_ = settings_.charger_w * settings_.efficiency;
      cycle_deadline_ = 0;
      if (control_.mode == Mode::NOW)
        control_.manual_deadline = next_deadline(clock_->now().timestamp, s.hour, s.minute);
      control_.target_latched = false;
      plan_dirty_ = true;
      persist_settings_();
      persist_optimizer_();
      log_event_(6, Reason::OFF);
    });
    r->send(200, "application/json", "{\"ok\":true}");
    return;
  }
  if (r->method() == HTTP_POST && u == "/charging/api/mode") {
    auto mode = r->arg("mode");
    int value = mode == "off" ? 0 : mode == "auto" ? 1 : mode == "now" ? 2 : -1;
    if (value < 0) {
      r->send(400, "application/json", "{\"error\":\"Invalid mode\"}");
      return;
    }
    if (value == 2 && !clock_->now().is_valid()) {
      r->send(409, "application/json", "{\"error\":\"Waiting for clock\"}");
      return;
    }
    defer([this, value] { request_mode(value); });
    r->send(200, "application/json", "{\"ok\":true}");
    return;
  }
  r->send(404, "application/json", "{\"error\":\"Not found\"}");
}
void Controller::status_json_(AsyncWebServerRequest* r) {
  std::unique_lock<std::recursive_mutex> lock(mutex_);
  auto in = inputs_();
  auto buffer = std::make_unique<std::array<char, 2700>>();
  auto* b = buffer->data();
  char soc[24], v[24], a[24], price[24], lo[24], hi[24], minc[24], maxc[24], power[24];
  snprintf(b, buffer->size(),
           "{\"now\":%u,\"mode\":\"%s\",\"reason\":\"%s\",\"relay\":%s,\"soc\":%s,"
           "\"voltage\":%s,\"current\":%s,\"power\":%s,\"price\":%s,\"temp_min\":%s,\"temp_max\":%s,"
           "\"cell_min\":%s,\"cell_max\":%s,\"alarms\":\"%016llx\",\"bms_fresh\":%s,\"bms_age\":%u,"
           "\"meter_fresh\":%s,\"clock_ok\":%s,\"target\":%.1f,\"ceiling\":%.3f,\"finish\":\"%02u:%02u\","
           "\"capacity_kwh\":%.2f,\"charger_w\":%.0f,\"efficiency\":%.3f,\"deadline\":%u,\"horizon\":%u,"
           "\"estimated_finish\":%u,\"achievable\":%.1f,\"unlikely\":%s,\"provisional\":%s,\"load_w\":%.1f,"
           "\"fetching\":%s,\"price_error\":\"%s\",\"storage_ok\":%s,\"free_heap\":%u,\"largest_block\":%u,"
           "\"min_heap\":%u,\"min_block\":%u,\"uptime\":%u,\"history_days\":%u,"
           "\"reserve_threshold\":%.1f,\"reserve_recovery\":%.1f,\"reserve_active\":%s,"
           "\"algorithm\":\"compact-mpc-v1\",\"optimizer_ready\":%s,\"decision_until\":%u,"
           "\"forecast_price\":%.3f,\"continuation_price\":%.3f,\"predicted_load_w\":%.2f,"
           "\"marginal_adder\":%.2f,\"charge_efficiency\":%.3f,\"discharge_efficiency\":%.3f,"
           "\"idle_w\":%.2f,\"fallback_load_w\":%.2f,\"optimizer_ms\":%u,\"optimizer_bytes\":%u}",
           in.now,
           control_.mode == Mode::AUTO  ? "auto"
           : control_.mode == Mode::NOW ? "now"
                                        : "off",
           reason_text(control_.reason), relay_->state ? "true" : "false",
           num(battery_.soc, soc, sizeof(soc)), num(battery_.voltage, v, sizeof(v)),
           num(battery_.current, a, sizeof(a)), num(meter_w_, power, sizeof(power)),
           num(plan_.current_price, price, sizeof(price)), num(battery_.temp_min, lo, sizeof(lo)),
           num(battery_.temp_max, hi, sizeof(hi)), num(battery_.cell_min, minc, sizeof(minc)),
           num(battery_.cell_max, maxc, sizeof(maxc)), static_cast<unsigned long long>(battery_.alarms),
           in.bms_fresh ? "true" : "false", last_bms_ms_ ? (millis() - last_bms_ms_) / 1000 : UINT32_MAX,
           in.meter_fresh ? "true" : "false", in.clock_ok ? "true" : "false", double(settings_.target),
           double(settings_.ceiling), settings_.hour, settings_.minute, double(settings_.capacity_kwh),
           double(settings_.charger_w), double(settings_.efficiency), plan_.deadline, plan_.horizon,
           plan_.estimated_finish, double(plan_.achievable), plan_.unlikely ? "true" : "false",
           plan_.provisional ? "true" : "false", double(load_w_), fetching_.load() ? "true" : "false",
           fetch_error_, storage_ok_ ? "true" : "false", unsigned(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
           unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)), min_heap_, min_block_,
           millis() / 1000, unsigned(RETAINED_DAYS), double(reserve_threshold_),
           double(reserve_recovery_), control_.reserve_active ? "true" : "false",
           optimizer_ready_?"true":"false",optimizer_until_,
           double(std::isfinite(forecast_ore_)?forecast_ore_:0),
           double(std::isfinite(continuation_ore_)?continuation_ore_:0),double(predicted_load_w_),
           double(optimizer_settings_.marginal_adder),double(optimizer_settings_.charge_efficiency),
           double(optimizer_settings_.discharge_efficiency),double(optimizer_settings_.idle_w),
           double(optimizer_settings_.fallback_load_w),optimizer_ms_,unsigned(sizeof(compact::Solver<101>)));
  lock.unlock();  // Never let a slow HTTP client block relay supervision.
  r->send(200, "application/json", b);
}
void Controller::history_json_(AsyncWebServerRequest* r, bool daily) {
  begin_json(r);
  if (!chunk(r, "[")) return;
  bool comma = false;
  uint32_t now = clock_->now().timestamp;
  char b[400], price[24];
  if (!clock_->now().is_valid() || (daily && RETAINED_DAYS == 0)) {
    chunk(r, "]");
    end_json(r);
    return;
  }
  if (daily) {
    auto today = day_key(now);
    for (size_t k = DAY_COUNT; k > 0; --k) {
      auto key = today - k + 1;
      Day d;
      {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        d = history_.days[key % DAY_COUNT];
      }
      if (d.day != key) continue;
      snprintf(
          b, sizeof(b),
          "%s{\"day\":%u,\"energy_kwh\":%.6f,\"unknown_kwh\":%.6f,\"cost_kr\":%.5f,"
          "\"on_seconds\":%u,\"coverage\":%u,\"soc_min\":%.1f,\"soc_max\":%.1f,\"met\":%u,\"missed\":%u}",
          comma ? "," : "", d.day, double(d.energy_mwh) / 1e6, double(d.unknown_mwh) / 1e6,
          double(d.cost_milliore) / 1e5, d.on_seconds, d.coverage,
          d.soc_min == 65535 ? -1.0 : d.soc_min / 10.0, d.soc_max / 10.0, d.met, d.missed);
      if (!chunk(r, b)) return;
      comma = true;
    }
  } else {
    uint32_t start = now / 300 * 300;
    for (size_t k = BUCKET_COUNT; k > 0; --k) {
      uint32_t ts = start - (k - 1) * 300;
      Bucket x;
      {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (ts == history_.active.time)
          x = history_.current();
        else
          x = history_.buckets[(ts / 300) % BUCKET_COUNT];
      }
      if (x.time != ts) continue;
      snprintf(b, sizeof(b),
               "%s{\"time\":%u,\"energy_kwh\":%.6f,\"unknown_kwh\":%.6f,\"cost_kr\":%.5f,"
               "\"soc\":%.1f,\"soc_min\":%.1f,\"soc_max\":%.1f,\"power\":%u,\"on_seconds\":%u,"
               "\"coverage\":%u,\"price\":%s,\"ceiling\":%.3f,\"flags\":%u}",
               comma ? "," : "", x.time, double(x.energy_mwh) / 1e6, double(x.unknown_mwh) / 1e6,
               double(x.cost_milliore) / 1e5, x.soc_last == 65535 ? -1.0 : x.soc_last / 10.0,
               x.soc_min == 65535 ? -1.0 : x.soc_min / 10.0, x.soc_max / 10.0, x.average_w, x.on_seconds,
               x.coverage, num(x.price, price, sizeof(price)), double(x.ceiling), x.flags);
      if (!chunk(r, b)) return;
      comma = true;
    }
  }
  chunk(r, "]");
  end_json(r);
}
void Controller::schedule_json_(AsyncWebServerRequest* r) {
  begin_json(r);
  if (!chunk(r, "[")) return;
  bool comma = false;
  char b[180];
  auto now = clock_->now().timestamp;
  size_t count;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    count = price_count_;
  }
  for (size_t i = 0; i < count; ++i) {
    Price x;
    uint16_t seconds;
    uint32_t at;
    {
      std::lock_guard<std::recursive_mutex> lock(mutex_);
      if (i >= price_count_) break;
      x = prices_[i];
      seconds = plan_.seconds[i];
      at = plan_.at;
    }
    if (x.start + 900 <= now) continue;
    snprintf(b, sizeof(b), "%s{\"time\":%u,\"price\":%.4f,\"start\":%u,\"seconds\":%u}", comma ? "," : "",
             x.start, double(x.ore), std::max(at, x.start), seconds);
    if (!chunk(r, b)) return;
    comma = true;
  }
  chunk(r, "]");
  end_json(r);
}
void Controller::events_json_(AsyncWebServerRequest* r) {
  begin_json(r);
  if (!chunk(r, "[")) return;
  bool comma = false;
  char b[250];
  uint32_t last;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    last = history_.event_seq;
  }
  uint32_t first = last > EVENT_COUNT ? last - EVENT_COUNT : 0;
  for (uint32_t i = first; i < last; ++i) {
    Event e;
    {
      std::lock_guard<std::recursive_mutex> lock(mutex_);
      e = history_.events[i % EVENT_COUNT];
    }
    snprintf(b, sizeof(b), "%s{\"time\":%u,\"kind\":%u,\"reason\":\"%s\",\"value\":%.2f}", comma ? "," : "",
             e.time, e.kind, reason_text(Reason(e.reason)), double(e.value));
    if (!chunk(r, b)) return;
    comma = true;
  }
  chunk(r, "]");
  end_json(r);
}
}  // namespace esphome::charging_controller
