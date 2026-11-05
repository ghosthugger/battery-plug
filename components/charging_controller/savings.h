#pragma once
#include "charging_core.h"

namespace charging {
// Independent of detailed history: 30 Stockholm calendar days, including today.
struct SavingsDay {
  uint32_t day{0}, known_seconds{0}, missing_seconds{0};
  float baseline_kr{0}, grid_kr{0}, inventory_kr{0}, load_kwh{0}, grid_kwh{0};
  float benefit() const { return baseline_kr - grid_kr + inventory_kr; }
};
struct Savings {
  uint32_t started_at{0}, last_at{0};
  float reference_kr{UNKNOWN}; // Fixed SEK per stored DC kWh; never revalue history.
  std::array<SavingsDay, 30> days{};

  SavingsDay& day(uint32_t key) {
    auto& d = days[key % days.size()];
    if (d.day != key) { d = SavingsDay{}; d.day = key; }
    return d;
  }
  void missing(uint32_t start, uint32_t end) {
    if (start >= end) return;
    // Bound work even after a long outage or a large clock correction.
    start = std::max(start, stockholm_wall_epoch(day_key(end) - 29, 0, 0));
    while (start < end) {
      auto key = day_key(start);
      auto stop = std::min(end, stockholm_wall_epoch(key + 1, 0, 0));
      day(key).missing_seconds += stop - start;
      start = stop;
    }
  }
  void sample(uint32_t now, bool continuous, bool valid, bool mains, float grid_w, float dc_w,
              const Price* prices, size_t count, float adder, float eta_c, float eta_d,
              float idle_w, float reference_spot) {
    if (!started_at) { started_at = last_at = now; return; }
    // A backward clock correction must not count the same seconds twice.
    if (now <= last_at) return;
    uint32_t start = last_at;
    last_at = now;
    valid = valid && continuous && now - start <= 20 && std::isfinite(grid_w) &&
            grid_w >= 0 && std::isfinite(dc_w) && std::abs(dc_w) <= 10000 &&
            eta_c >= .1f && eta_c <= 1 && eta_d >= .1f && eta_d <= 1;
    if (!valid) { missing(start, now); return; }
    // The plug meter sees load + charging. Subtract estimated charger input;
    // when running on battery, subtract inverter idle and conversion losses.
    float output_w = std::max(0.f, (-dc_w - idle_w) * eta_d);
    float load_w = mains ? std::max(0.f, grid_w - std::max(0.f, dc_w) / eta_c) + output_w
                        : output_w;
    grid_w = mains ? grid_w : 0;
    while (start < now) {
      auto key = day_key(start);
      auto end = std::min({now, start / 900 * 900 + 900,
                           stockholm_wall_epoch(key + 1, 0, 0)});
      float spot = UNKNOWN;
      for (size_t i = 0; i < count; ++i)
        if (prices[i].start <= start && start < prices[i].start + 900) { spot = prices[i].ore; break; }
      if (!std::isfinite(spot)) { missing(start, end); start = end; continue; }
      if (!std::isfinite(reference_kr))
        reference_kr = std::max(0.f, (std::isfinite(reference_spot) ? reference_spot : spot) + adder) / 100 / eta_c;
      auto& d = day(key);
      float kwh_per_w = (end - start) / 3600000.f;
      float price_kr = (spot + adder) / 100;
      d.known_seconds += end - start;
      d.load_kwh += load_w * kwh_per_w;
      d.grid_kwh += grid_w * kwh_per_w;
      d.baseline_kr += load_w * kwh_per_w * price_kr;
      d.grid_kr += grid_w * kwh_per_w * price_kr;
      d.inventory_kr += dc_w * kwh_per_w * reference_kr;
      start = end;
    }
  }
};
static_assert(sizeof(Savings) <= 1024, "Savings must fit in 1 KiB retained RAM");
} // namespace charging
