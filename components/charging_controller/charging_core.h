#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <limits>
#include <string_view>

#ifndef CHARGING_DAILY_HISTORY_DAYS
#define CHARGING_DAILY_HISTORY_DAYS 180
#endif
namespace charging {
constexpr size_t RETAINED_DAYS = CHARGING_DAILY_HISTORY_DAYS;
constexpr size_t PRICE_COUNT = 200, BUCKET_COUNT = 288, DAY_COUNT = RETAINED_DAYS ? RETAINED_DAYS : 1,
                 EVENT_COUNT = 128;
constexpr float UNKNOWN = std::numeric_limits<float>::quiet_NaN();
enum class Mode : uint8_t { OFF, AUTO, NOW, ALWAYS };
// Values0/2 remain only for decoding legacy records/tests; never accepted as operating modes.
inline Mode restored_operating_mode(uint32_t value) { return value==3?Mode::ALWAYS:Mode::AUTO; }
inline bool valid_operating_mode(int value) { return value==1 || value==3; }
enum class Reason : uint8_t {
  OFF,
  CLOCK,
  BMS,
  METER,
  FAULT,
  TARGET,
  PRICE_UNKNOWN,
  PRICE_HIGH,
  WAITING,
  CHARGING,
  NO_GAIN,
  DEADLINE,
  OVERCURRENT,
  OTA,
  COOLDOWN,
  LOW_BATTERY,
  PLANNER,
  ALWAYS,
  BMS_HANDOFF
};
inline const char* reason_text(Reason r) {
  static const char* names[] = {"Off",
                                "Waiting for clock",
                                "Battery disconnected or stale",
                                "Power meter stale",
                                "BMS prohibits charging",
                                "Target reached",
                                "Current price unavailable",
                                "Price above limit",
                                "Waiting for cheaper electricity",
                                "Charging",
                                "Loads exceed charging capacity",
                                "Deadline reached",
                                "Overcurrent protection",
                                "Firmware update in progress",
                                "Waiting for switch cooldown",
                                "Low battery: restoring mains",
                                "Waiting for optimiser settings or price data",
                                "Always charging: mains enabled",
                                "Phone Bluetooth access: mains held on, live BMS checks paused"};
  return names[static_cast<unsigned>(r)];
}
struct Settings {
  float target{90}, ceiling{100}, capacity_kwh{4}, charger_w{400}, efficiency{0.85f};
  uint8_t hour{7}, minute{0};
};
inline bool valid_settings(const Settings& s) {
  return std::isfinite(s.target) && s.target >= 0 && s.target <= 100 && std::isfinite(s.ceiling) &&
         s.ceiling >= -10000 && s.ceiling <= 100000 && std::isfinite(s.capacity_kwh) &&
         s.capacity_kwh >= 0.1f && s.capacity_kwh <= 100 && std::isfinite(s.charger_w) && s.charger_w >= 10 &&
         s.charger_w <= 2500 && std::isfinite(s.efficiency) && s.efficiency >= 0.1f && s.efficiency <= 1 &&
         s.hour < 24 && s.minute < 60;
}
inline bool valid_reserve_settings(float start, float stop) {
  return std::isfinite(start) && std::isfinite(stop) && start >= 0 && start <= 95 &&
         stop > start && stop <= 100;
}
inline bool valid_operating_reserve(float start,float stop) {
  return start>=1 && valid_reserve_settings(start,stop);
}
struct Price {
  uint32_t start{0};
  float ore{UNKNOWN};
};
inline float converted_price(float eur_mwh, float sek_per_eur) { return eur_mwh * sek_per_eur * 1.25f / 10; }
// Visit one JSON object at a time. Each object is parsed/validated by the caller;
// no full-array JSON tree or duplicate response string is needed on the device.
template <class Visitor>
bool price_objects(std::string_view text, Visitor visit) {
  size_t i = 0;
  auto ws = [&] {
    while (i < text.size() && (text[i] == ' ' || text[i] == '\n' || text[i] == '\r' || text[i] == '\t')) ++i;
  };
  ws();
  if (i == text.size() || text[i++] != '[') return false;
  ws();
  bool first = true;
  while (i < text.size()) {
    if (text[i] == ']') {
      ++i;
      ws();
      return i == text.size();
    }
    if (!first) {
      if (text[i++] != ',') return false;
      ws();
    }
    if (i == text.size() || text[i] != '{') return false;
    size_t start = i;
    int depth = 0;
    bool quoted = false, escape = false, ended = false;
    for (; i < text.size() && i - start < 1024; ++i) {
      char c = text[i];
      if (quoted) {
        if (escape)
          escape = false;
        else if (c == '\\')
          escape = true;
        else if (c == '"')
          quoted = false;
      } else if (c == '"')
        quoted = true;
      else if (c == '{')
        ++depth;
      else if (c == '}' && --depth == 0) {
        ++i;
        ended = true;
        break;
      }
    }
    if (!ended || !visit(text.substr(start, i - start))) return false;
    first = false;
    ws();
  }
  return false;
}
// Civil-date arithmetic avoids nonportable timegm() and never changes global TZ.
inline int32_t civil_days(int year, int month, int day) {
  year -= month <= 2;
  int era = (year >= 0 ? year : year - 399) / 400;
  unsigned y = year - era * 400;
  unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  return era * 146097 + int(y * 365 + y / 4 - y / 100 + doy) - 719468;
}
inline uint32_t utc_timestamp(const tm& t) {
  return uint32_t(int64_t(civil_days(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday)) * 86400 + t.tm_hour * 3600 +
                  t.tm_min * 60 + t.tm_sec);
}
inline uint32_t parse_utc_timestamp(std::string_view s) {
  if (s.size() != 20 || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':' ||
      s[19] != 'Z')
    return 0;
  auto number = [&](size_t i, size_t n) {
    int value = 0;
    for (size_t k = 0; k < n; ++k) {
      char c = s[i + k];
      if (c < '0' || c > '9') return -1;
      value = value * 10 + c - '0';
    }
    return value;
  };
  int year = number(0, 4), month = number(5, 2), day = number(8, 2), hour = number(11, 2),
      minute = number(14, 2), second = number(17, 2);
  if (year < 2025 || year > 2100 || month < 1 || month > 12 || hour < 0 || hour > 23 || minute < 0 ||
      minute > 59 || second < 0 || second > 59)
    return 0;
  const int lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int days = lengths[month - 1] + (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
  if (day < 1 || day > days) return 0;
  return uint32_t(civil_days(year, month, day)) * 86400 + hour * 3600 + minute * 60 + second;
}
inline tm utc_parts(uint32_t stamp) {
  int z = int(stamp / 86400) + 719468;
  int era = z / 146097;
  unsigned doe = z - era * 146097;
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int y = yoe + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
  unsigned d = doy - (153 * mp + 2) / 5 + 1;
  int m = int(mp) + (mp < 10 ? 3 : -9);
  y += m <= 2;
  tm t{};
  t.tm_year = y - 1900;
  t.tm_mon = m - 1;
  t.tm_mday = d;
  t.tm_wday = (stamp / 86400 + 4) % 7;
  t.tm_yday = int(stamp / 86400) - civil_days(y, 1, 1);
  t.tm_hour = stamp % 86400 / 3600;
  t.tm_min = stamp % 3600 / 60;
  t.tm_sec = stamp % 60;
  return t;
}
inline uint32_t stockholm_offset(uint32_t stamp) {
  int y = utc_parts(stamp).tm_year + 1900;
  int march = 31 - (civil_days(y, 3, 31) + 4) % 7, october = 31 - (civil_days(y, 10, 31) + 4) % 7;
  uint32_t begin = uint32_t(civil_days(y, 3, march)) * 86400 + 3600,
           end = uint32_t(civil_days(y, 10, october)) * 86400 + 3600;
  return stamp >= begin && stamp < end ? 7200 : 3600;
}
inline tm stockholm_parts(uint32_t stamp) {
  auto t = utc_parts(stamp + stockholm_offset(stamp));
  t.tm_isdst = stockholm_offset(stamp) == 7200;
  return t;
}
inline uint32_t stockholm_wall_epoch(uint32_t day, unsigned hour, unsigned minute) {
  uint32_t wall = day * 86400 + hour * 3600 + minute * 60, best = UINT32_MAX;
  for (uint32_t offset : {3600U, 7200U}) {
    uint32_t candidate = wall - offset;
    if (candidate + stockholm_offset(candidate) == wall) best = std::min(best, candidate);
  }
  return best == UINT32_MAX ? wall - 3600 : best;
}
inline uint32_t next_deadline(uint32_t now, unsigned hour, unsigned minute) {
  auto today = stockholm_parts(now);
  uint32_t day_id = civil_days(today.tm_year + 1900, today.tm_mon + 1, today.tm_mday);
  for (int day = 0; day < 2; ++day) {
    uint32_t best = UINT32_MAX, wall = (day_id + day) * 86400 + hour * 3600 + minute * 60;
    for (uint32_t offset : {3600U, 7200U}) {
      uint32_t candidate = wall - offset;
      if (candidate > now && candidate + stockholm_offset(candidate) == wall)
        best = std::min(best, candidate);
    }
    if (best != UINT32_MAX) return best;
    uint32_t fallback = stockholm_wall_epoch(day_id + day, hour, minute);
    if (fallback > now) return fallback;
  }
  return now + 86400;
}
inline uint32_t day_key(uint32_t now) {
  auto local = stockholm_parts(now);
  return civil_days(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
}
inline uint16_t crc16(const uint8_t* p, size_t n) {
  uint16_t c = 0xffff;
  for (size_t i = 0; i < n; ++i) {
    c ^= p[i];
    for (int j = 0; j < 8; ++j) c = (c & 1) ? (c >> 1) ^ 0xa001 : c >> 1;
  }
  return c;
}
inline uint32_t crc32(const void* data, size_t n) {
  auto* p = static_cast<const uint8_t*>(data);
  uint32_t c = ~0U;
  for (size_t i = 0; i < n; ++i) {
    c ^= p[i];
    for (int j = 0; j < 8; ++j) c = (c >> 1) ^ ((0U - (c & 1)) & 0xedb88320U);
  }
  return ~c;
}
struct Battery {
  float soc{UNKNOWN}, voltage{UNKNOWN}, current{UNKNOWN}, temp_min{UNKNOWN}, temp_max{UNKNOWN},
      cell_min{UNKNOWN}, cell_max{UNKNOWN};
  uint64_t alarms{0};
  uint16_t charge_mos{0}, discharge_mos{0}; // Raw D2 registers 0x35 / 0x36.
  bool charge_allowed{false};
  uint16_t balancing_status{0}; // Live active/inactive readback, not the enable setting.
};
inline bool decode_daly(const uint8_t* b, size_t n, Battery& out) {
  if (n != 129 || b[0] != 0xd2 || b[1] != 3 || b[2] != 124 ||
      crc16(b, 127) != (b[127] | uint16_t(b[128]) << 8))
    return false;
  auto u16 = [&](size_t i) { return uint16_t(uint16_t(b[i]) << 8 | b[i + 1]); };
  Battery v;
  v.voltage = u16(83) * 0.1f;
  v.current = (int(u16(85)) - 30000) * 0.1f;
  v.soc = u16(87) * 0.1f;
  const auto cells = u16(101), temps = u16(103);
  if (v.soc > 100 || v.voltage <= 0 || cells == 0 || cells > 32 || temps > 8) return false;
  v.cell_min = 100;
  v.cell_max = 0;
  for (unsigned i = 0; i < cells; ++i) {
    float x = u16(3 + 2 * i) * 0.001f;
    if (x <= 0 || x > 6) return false;
    v.cell_min = std::min(v.cell_min, x);
    v.cell_max = std::max(v.cell_max, x);
  }
  if (temps) {
    v.temp_min = 200;
    v.temp_max = -100;
    for (unsigned i = 0; i < temps; ++i) {
      float x = int(u16(67 + 2 * i)) - 40;
      if (x < -40 || x > 150) return false;
      v.temp_min = std::min(v.temp_min, x);
      v.temp_max = std::max(v.temp_max, x);
    }
  }
  for (int i = 0; i < 8; ++i) v.alarms = (v.alarms << 8) | b[119 + i];
  v.charge_mos=u16(109);v.discharge_mos=u16(111);
  v.balancing_status=u16(107);
  // Registers 0x3A..0x3D: high voltage, charge temperature/current,
  // cell/temperature imbalance and acquisition/MOS faults prohibit a start.
  // Undervoltage and low-SOC flags must not prevent recovery charging.
  v.charge_allowed = v.charge_mos == 1 && !(u16(119) & 0x330f) && !(u16(121) & 0x330f) && u16(123) == 0;
  out = v;
  return true;
}
class DalyFrames {
 public:
  void reset() { size_ = 0; }
  bool push(const uint8_t* bytes, size_t length, Battery& result) {
    bool found = false;
    for (size_t i = 0; i < length; ++i) {
      auto b = bytes[i];
      if (!size_ && b != 0xd2) continue;
      data_[size_++] = b;
      if ((size_ == 2 && b != 3) || (size_ == 3 && b != 124)) {
        size_ = 0;
        if (b == 0xd2) data_[size_++] = b;
        continue;
      }
      if (size_ == data_.size()) {
        found = decode_daly(data_.data(), size_, result) || found;
        size_ = 0;
      }
    }
    return found;
  }

 private:
  std::array<uint8_t, 129> data_{};
  size_t size_{0};
};
struct Inputs {
  uint32_t now{0};
  bool clock_ok{false}, bms_fresh{false}, meter_fresh{false}, fault{false};
  float soc{UNKNOWN}, load_w{0}, effective_charge_w{0};
};
struct Plan {
  std::array<uint16_t, PRICE_COUNT> seconds{};
  uint32_t at{0}, deadline{0}, horizon{0}, estimated_finish{0};
  float achievable{0}, need_wh{0}, current_price{UNKNOWN};
  bool provisional{true}, unlikely{false};
  int current{-1};
};
inline Plan make_plan(const Settings& s, const Inputs& in, const Price* prices, size_t count) {
  Plan p;
  if (!in.clock_ok) return p;
  count = std::min(count, PRICE_COUNT);
  p.at = in.now;
  p.deadline = next_deadline(in.now, s.hour, s.minute);
  float load = std::max(0.0f, in.load_w), gain = std::max(0.0f, in.effective_charge_w);
  const bool have_soc = std::isfinite(in.soc);
  float deficit = have_soc ? std::max(0.0f, s.target - in.soc) * s.capacity_kwh * 10 : 0;
  // Add forecast discharge over the entire deadline horizon, then replace that
  // energy during the cheapest eligible charging windows.
  p.need_wh = deficit + load * (p.deadline - in.now) / 3600.0f;
  std::array<size_t, PRICE_COUNT> order{};
  size_t eligible = 0;
  uint32_t contiguous = in.now;
  for (size_t i = 0; i < count; ++i) {
    const auto& x = prices[i];
    if (!std::isfinite(x.ore)) continue;
    if (x.start <= in.now && in.now < x.start + 900) {
      p.current = i;
      p.current_price = x.ore;
    }
    if (x.start <= contiguous && x.start + 900 > contiguous) contiguous = x.start + 900;
    if (x.start + 900 > in.now && x.start < p.deadline && x.ore <= s.ceiling) order[eligible++] = i;
  }
  p.horizon = contiguous;
  p.provisional = contiguous < p.deadline;
  if (!have_soc) return p;
  std::sort(order.begin(), order.begin() + eligible, [&](size_t a, size_t b) {
    return prices[a].ore == prices[b].ore ? prices[a].start < prices[b].start : prices[a].ore < prices[b].ore;
  });
  float gross = gain + load, remaining = p.need_wh, supplied = 0;
  for (size_t j = 0; j < eligible && remaining > 0 && gross > 0; ++j) {
    size_t i = order[j];
    uint32_t a = std::max(in.now, prices[i].start), z = std::min(p.deadline, prices[i].start + 900);
    uint16_t seconds = std::min<uint32_t>(z - a, static_cast<uint32_t>(std::ceil(remaining * 3600 / gross)));
    p.seconds[i] = seconds;
    float wh = seconds * gross / 3600;
    remaining -= wh;
    supplied += wh;
    p.estimated_finish = std::max(p.estimated_finish, a + seconds);
  }
  p.achievable = std::clamp(in.soc + (supplied - load * (p.deadline - in.now) / 3600) / (s.capacity_kwh * 10),
                            0.0f, s.target);
  p.unlikely = remaining > 1 || gain <= 0;
  return p;
}
class Control {
 public:
  Mode mode{Mode::OFF};
  Reason reason{Reason::OFF};
  bool on{false}, target_latched{false}, reserve_active{false};
  uint32_t last_switch{0}, manual_deadline{0}, cycle_deadline{0};
  bool cycle_met{false};
  void set_mode(Mode value, uint32_t now, const Settings& s) {
    mode = value;
    target_latched = false;
    manual_deadline = value == Mode::NOW ? next_deadline(now, s.hour, s.minute) : 0;
    if (value == Mode::OFF) { on = false; reserve_active = false; }
  }
  bool evaluate(const Settings& s, const Inputs& in, const Plan& p, const Price* prices,
                float reserve_threshold = 0, float reserve_recovery = 10) {
    if (mode == Mode::OFF || reserve_threshold <= 0 ||
        !valid_reserve_settings(reserve_threshold, reserve_recovery)) reserve_active = false;
    else if (in.bms_fresh && std::isfinite(in.soc) && in.soc >= 0 && in.soc <= 100) {
      if (in.soc <= reserve_threshold) reserve_active = true;
      else if (in.soc >= reserve_recovery) reserve_active = false;
    }
    bool want = false, immediate = true;
    if (mode == Mode::OFF)
      reason = Reason::OFF;
    else if (!in.clock_ok && !reserve_active)
      reason = Reason::CLOCK;
    else if (!in.bms_fresh || !std::isfinite(in.soc) || in.soc < 0 || in.soc > 100)
      reason = Reason::BMS;
    else if (!in.meter_fresh)
      reason = Reason::METER;
    else if (in.fault)
      reason = Reason::FAULT;
    else if (reserve_active) {
      want = true;
      reason = Reason::LOW_BATTERY;
    } else if (mode == Mode::NOW && in.now >= manual_deadline) {
      reason = Reason::DEADLINE;
      mode = Mode::OFF;
    } else {
      if (in.soc >= s.target) {
        target_latched = true;
        cycle_met = true;
        if (mode == Mode::NOW) mode = Mode::OFF;
      }
      if (in.soc <= s.target - 2) target_latched = false;
      if (target_latched || in.soc >= s.target)
        reason = Reason::TARGET;
      else if (mode == Mode::NOW) {
        want = true;
        reason = Reason::CHARGING;
        immediate = false;
      } else if (p.current < 0)
        reason = Reason::PRICE_UNKNOWN;
      else if (p.current_price > s.ceiling)
        reason = Reason::PRICE_HIGH;
      else if (in.effective_charge_w <= 0)
        reason = Reason::NO_GAIN;
      else {
        const auto i = static_cast<size_t>(p.current);
        want = p.seconds[i] > 0 && in.now < std::max(p.at, prices[i].start) + p.seconds[i];
        // Replanning always starts a partial current slot at now. Stop exactly
        // at the selected end via the component's retained schedule anchor.
        reason = want ? Reason::CHARGING : Reason::WAITING;
        immediate = false;
      }
    }
    if (want != on && (!immediate && in.now - last_switch < 120)) {
      reason = Reason::COOLDOWN;
      return on;
    }
    if (want != on) {
      on = want;
      last_switch = in.now;
    }
    return on;
  }
  // Automatic cost control has no daily target/deadline/ceiling. All existing
  // data/fault checks and the persistent low-battery recovery remain authoritative.
  bool evaluate_optimized(const Settings& s, const Inputs& in, const Plan& p, const Price* prices,
                          bool ready, bool request, float start, float stop,
                          bool diagnostic_mains_hold = false) {
    (void)s; (void)prices;
    if (mode!=Mode::AUTO && mode!=Mode::ALWAYS) mode=Mode::AUTO;
    if (start<=0 || !valid_reserve_settings(start,stop)) reserve_active=false;
    else if (in.bms_fresh && std::isfinite(in.soc) && in.soc>=0 && in.soc<=100) {
      if (in.soc<=start) reserve_active=true;
      else if (in.soc>=stop) reserve_active=false;
    }
    bool want=false,immediate=true;
    // Explicit bounded phone-access exception; the component admits it only
    // from healthy readings with mains already on. No phantom switch/cooldown.
    if (diagnostic_mains_hold && in.meter_fresh && !in.fault) {want=true;reason=Reason::BMS_HANDOFF;}
    else if (mode==Mode::AUTO && !in.clock_ok && !reserve_active) reason=Reason::CLOCK;
    else if (!in.bms_fresh || !std::isfinite(in.soc) || in.soc<0 || in.soc>100) reason=Reason::BMS;
    else if (!in.meter_fresh) reason=Reason::METER;
    else if (in.fault) reason=Reason::FAULT;
    else if (mode==Mode::ALWAYS) {want=true;reason=Reason::ALWAYS;}
    else if (reserve_active) {want=true;reason=Reason::LOW_BATTERY;}
    else if (p.current<0) reason=Reason::PRICE_UNKNOWN;
    else if (!ready) reason=Reason::PLANNER;
    else {want=request;reason=want?Reason::CHARGING:Reason::WAITING;immediate=false;}
    if (want!=on && !immediate && in.now-last_switch<120) {reason=Reason::COOLDOWN;return on;}
    if (want!=on) {on=want;last_switch=in.now;}
    return on;
  }
};
struct Bucket {
  uint32_t time{0}, energy_mwh{0}, unknown_mwh{0};
  int32_t cost_milliore{0};
  float price{UNKNOWN}, ceiling{0};
  uint16_t soc_min{65535}, soc_max{0}, soc_last{65535}, average_w{0}, on_seconds{0}, coverage{0}, flags{0},
      reserved{0};
};
struct Day {
  uint32_t day{0}, energy_mwh{0}, unknown_mwh{0};
  int64_t cost_milliore{0};
  uint32_t on_seconds{0}, coverage{0};
  uint16_t soc_min{65535}, soc_max{0}, met{0}, missed{0};
} __attribute__((packed));
struct Event {
  uint32_t time{0};
  uint16_t kind{0}, reason{0};
  float soc{UNKNOWN}, value{0};
};
static_assert(sizeof(Bucket) == 40 && sizeof(Day) == 36 && sizeof(Event) == 16);
class History {
 public:
  std::array<Bucket, BUCKET_COUNT> buckets{};
  std::array<Day, DAY_COUNT> days{};
  std::array<Event, EVENT_COUNT> events{};
  uint32_t event_seq{0};
  Bucket active{};
  float energy{0}, unknown{0}, cost{0}, power_seconds{0}, price_seconds{0}, priced_seconds{0};
  bool add(uint32_t now, float seconds, float watts, float soc, bool on, float price, float ceiling,
           bool valid) {
    uint32_t start = now / 300 * 300;
    bool completed = active.time && active.time != start;
    bool usable = valid && seconds > 0 && seconds <= 30 && std::isfinite(watts) && watts >= 0;
    if (completed) {
      if (usable && start == active.time + 300) {
        // Avoid converting the epoch itself to float (single precision loses
        // minute resolution at current Unix timestamps).
        float old_seconds = std::max(0.0f, seconds - float(now - start));
        accumulate(old_seconds, watts, soc, on, price, ceiling);
        seconds -= old_seconds;
      }
      finish();
    }
    if (active.time != start) {
      active = Bucket{};
      active.time = start;
      energy = unknown = cost = power_seconds = price_seconds = priced_seconds = 0;
    }
    active.ceiling = ceiling;
    if (!usable) {
      active.flags |= 1;
      return completed;
    }
    seconds = std::min(seconds, float(now - start));
    accumulate(seconds, watts, soc, on, price, ceiling);
    return completed;
  }
  void accumulate(float seconds, float watts, float soc, bool on, float price, float ceiling) {
    active.ceiling = ceiling;
    float mwh = std::max(0.0f, watts) * seconds / 3.6f;
    energy += mwh;
    if (std::isfinite(price)) {
      cost += mwh * price / 1000;
      price_seconds += price * seconds;
      priced_seconds += seconds;
    } else {
      unknown += mwh;
      active.flags |= 2;
    }
    power_seconds += watts * seconds;
    active.coverage += uint16_t(seconds);
    if (on) active.on_seconds += uint16_t(seconds);
    if (std::isfinite(soc)) {
      auto x = uint16_t(std::lround(soc * 10));
      active.soc_last = x;
      active.soc_min = std::min(active.soc_min, x);
      active.soc_max = std::max(active.soc_max, x);
    }
  }
  Bucket current() const {
    Bucket b = active;
    b.energy_mwh = std::lround(energy);
    b.unknown_mwh = std::lround(unknown);
    b.cost_milliore = std::lround(cost);
    b.average_w = b.coverage ? std::lround(power_seconds / b.coverage) : 0;
    b.price = priced_seconds ? price_seconds / priced_seconds : UNKNOWN;
    return b;
  }
  void finish() {
    Bucket b = current();
    buckets[(b.time / 300) % BUCKET_COUNT] = b;
    add_to_day(b);
  }
  void add_to_day(const Bucket& b) {
    if constexpr (RETAINED_DAYS == 0) return;
    auto key = day_key(b.time);
    auto& d = days[key % DAY_COUNT];
    if (d.day != key) {
      d = Day{};
      d.day = key;
    }
    d.energy_mwh += b.energy_mwh;
    d.unknown_mwh += b.unknown_mwh;
    d.cost_milliore += b.cost_milliore;
    d.on_seconds += b.on_seconds;
    d.coverage += b.coverage;
    d.soc_min = std::min(uint16_t(d.soc_min), b.soc_min);
    d.soc_max = std::max(uint16_t(d.soc_max), b.soc_max);
  }
  void event(uint32_t now, uint16_t kind, Reason why, float soc, float value = 0) {
    events[event_seq % EVENT_COUNT] = {now, kind, uint16_t(why), soc, value};
    ++event_seq;
  }
  void deadline(uint32_t now, bool met) {
    if constexpr (RETAINED_DAYS == 0) return;
    auto key = day_key(now);
    auto& d = days[key % DAY_COUNT];
    if (d.day != key) {
      d = Day{};
      d.day = key;
    }
    if (met)
      ++d.met;
    else
      ++d.missed;
  }
};
static_assert(sizeof(History) + sizeof(Plan) + sizeof(Price) * PRICE_COUNT < 24 * 1024,
              "History, events and price buffers exceed the agreed RAM budget");
}  // namespace charging
