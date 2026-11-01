#include <cassert>
#include <cstdlib>
#include <iostream>

#include "../components/charging_controller/charging_core.h"
using namespace charging;
static uint32_t stamp(const char* s) {
  tm t{};
  assert(strptime(s, "%Y-%m-%d %H:%M:%S", &t));
  t.tm_isdst = -1;
  return mktime(&t);
}
static void planner() {
  Settings s;
  Inputs in;
  in.now = stamp("2026-11-01 01:00:00");
  in.clock_ok = in.bms_fresh = in.meter_fresh = true;
  in.soc = 89;
  in.effective_charge_w = 340;
  std::array<Price, 24> prices;
  for (size_t i = 0; i < prices.size(); ++i) prices[i] = {in.now + uint32_t(i) * 900, 80};
  prices[0].ore = 99;
  prices[1].ore = 20;
  prices[2].ore = -10;
  auto p = make_plan(s, in, prices.data(), prices.size());
  assert(p.seconds[2] > 0 && p.seconds[2] < 900);
  assert(!p.seconds[0]);
  assert(!p.unlikely && !p.provisional);
  Control c;
  c.set_mode(Mode::AUTO, in.now, s);
  assert(!c.evaluate(s, in, p, prices.data()));
  assert(c.reason == Reason::WAITING);
  in.now = prices[2].start;
  p = make_plan(s, in, prices.data(), prices.size());
  assert(c.evaluate(s, in, p, prices.data()));
  in.now = p.at + p.seconds[2];
  assert(!c.evaluate(s, in, p, prices.data()));
  in.now = prices[0].start;
  prices[0].ore = 101;
  p = make_plan(s, in, prices.data(), prices.size());
  c.on = true;
  assert(!c.evaluate(s, in, p, prices.data()) && c.reason == Reason::PRICE_HIGH);
  p.current = -1;
  c.on = true;
  assert(!c.evaluate(s, in, p, prices.data()) && c.reason == Reason::PRICE_UNKNOWN);
  c = Control{};
  c.set_mode(Mode::NOW, in.now, s);
  p = make_plan(s, in, prices.data(), prices.size());
  assert(c.evaluate(s, in, p, prices.data()));
  in.bms_fresh = false;
  assert(!c.evaluate(s, in, p, prices.data()));
  in.bms_fresh = true;
  in.meter_fresh = false;
  assert(!c.evaluate(s, in, p, prices.data()));
  in.meter_fresh = true;
  in.fault = true;
  assert(!c.evaluate(s, in, p, prices.data()));
  in.fault = false;
  in.soc = 90;
  assert(!c.evaluate(s, in, p, prices.data()) && c.mode == Mode::OFF);
  c.set_mode(Mode::AUTO, in.now, s);
  c.evaluate(s, in, p, prices.data());
  in.soc = 89;
  assert(!c.evaluate(s, in, p, prices.data()) && c.reason == Reason::TARGET);
  in.soc = 88;
  prices[0].ore = -20;
  p = make_plan(s, in, prices.data(), prices.size());
  in.now += 121;
  assert(c.evaluate(s, in, p, prices.data()));
  for (auto& x : prices) x.ore = 1000;
  in.soc = 10;
  p = make_plan(s, in, prices.data(), prices.size());
  assert(p.unlikely);
  for (auto seconds : p.seconds) assert(!seconds);
  for (auto& x : prices) x.ore = 50;
  in.load_w = 100;
  in.effective_charge_w = 240;
  auto loaded = make_plan(s, in, prices.data(), prices.size());
  in.load_w = 0;
  in.effective_charge_w = 340;
  assert(loaded.need_wh > make_plan(s, in, prices.data(), prices.size()).need_wh);
  in.effective_charge_w = 0;
  assert(make_plan(s, in, prices.data(), prices.size()).unlikely);
  in.effective_charge_w = 340;
  prices[2].ore = UNKNOWN;
  assert(make_plan(s, in, prices.data(), prices.size()).provisional);
  in.soc = UNKNOWN;
  assert(std::isfinite(make_plan(s, in, prices.data(), prices.size()).current_price));
  assert(std::abs(converted_price(100, 11) - 137.5f) < 0.001f);
}
static void low_battery_recovery() {
  Settings s;
  Inputs in;
  in.now = stamp("2026-11-01 12:00:00");
  in.bms_fresh = in.meter_fresh = in.clock_ok = true;
  in.soc = 5;
  in.effective_charge_w = 0; // The reserve must not depend on a load estimate.
  Plan p;
  p.current = -1; // Neither prices nor a schedule are required.
  Control c;
  c.set_mode(Mode::AUTO, in.now, s);
  c.last_switch = in.now; // Immediate low-battery start bypasses cooldown.
  assert(c.evaluate(s, in, p, nullptr, 5));
  assert(c.reserve_active && c.reason == Reason::LOW_BATTERY);
  in.soc = 7;
  assert(c.evaluate(s, in, p, nullptr, 5));
  in.clock_ok = false;
  assert(c.evaluate(s, in, p, nullptr, 5));
  in.bms_fresh = false;
  assert(!c.evaluate(s, in, p, nullptr, 5) && c.reason == Reason::BMS);
  in.bms_fresh = true;
  in.meter_fresh = false;
  assert(!c.evaluate(s, in, p, nullptr, 5) && c.reason == Reason::METER);
  in.meter_fresh = true;
  in.fault = true;
  assert(!c.evaluate(s, in, p, nullptr, 5) && c.reason == Reason::FAULT);
  in.fault = false;
  in.soc = UNKNOWN;
  assert(!c.evaluate(s, in, p, nullptr, 5));
  in.soc = -1;
  assert(!c.evaluate(s, in, p, nullptr, 5));
  in.soc = 101;
  assert(!c.evaluate(s, in, p, nullptr, 5));
  in.soc = 9.9;
  assert(c.evaluate(s, in, p, nullptr, 5));
  in.soc = 10;
  in.clock_ok = true;
  assert(!c.evaluate(s, in, p, nullptr, 5) && !c.reserve_active);
  in.soc = 7;
  assert(!c.evaluate(s, in, p, nullptr, 5)); // No retrigger until threshold.
  c.reserve_active = true; // Persisted latch restored after restart at 7%.
  assert(c.evaluate(s, in, p, nullptr, 5));
  c = Control{};
  c.set_mode(Mode::AUTO, in.now, s);
  in.soc = 5;
  assert(c.evaluate(s, in, p, nullptr, 5, 30));
  in.soc = 10;
  assert(c.evaluate(s, in, p, nullptr, 5, 30));
  in.soc = 29.9;
  assert(c.evaluate(s, in, p, nullptr, 5, 30));
  in.soc = 30;
  assert(!c.evaluate(s, in, p, nullptr, 5, 30) && !c.reserve_active);
  in.soc = 20;
  assert(!c.evaluate(s, in, p, nullptr, 5, 30));
  in.soc = 5;
  assert(c.evaluate(s, in, p, nullptr, 5, 100));
  in.soc = 99.9;
  assert(c.evaluate(s, in, p, nullptr, 5, 100));
  in.soc = 100;
  assert(!c.evaluate(s, in, p, nullptr, 5, 100));
  assert(valid_reserve_settings(5, 10) && valid_reserve_settings(0, 1));
  for (float stop : {0.0f, 4.0f, 5.0f, 101.0f, UNKNOWN})
    assert(!valid_reserve_settings(5, stop));
  assert(!valid_reserve_settings(UNKNOWN, 10) && !valid_reserve_settings(-1, 10));
  c.set_mode(Mode::OFF, in.now, s);
  in.soc = 2;
  assert(!c.evaluate(s, in, p, nullptr, 5) && !c.reserve_active);
  c.set_mode(Mode::AUTO, in.now, s);
  assert(!c.evaluate(s, in, p, nullptr, 0)); // Explicitly disabled.
  c.set_mode(Mode::NOW, in.now, s);
  in.now = c.manual_deadline + 1;
  s.target = 1; // Recovery overrides both an expired deadline and a lower target.
  assert(c.evaluate(s, in, p, nullptr, 5) && c.mode == Mode::NOW);
  in.soc = 10;
  assert(!c.evaluate(s, in, p, nullptr, 5) && c.mode == Mode::OFF);
}
static void json_array() {
  int count = 0;
  assert(price_objects(" [ {\"value\":1}, {\"value\":2} ] ", [&](std::string_view) {
    ++count;
    return true;
  }));
  assert(count == 2);
  for (auto bad : {"{}", "[{]", "[{},]", "[,{}]", "[{}]junk", "[{} {}]", "["})
    assert(!price_objects(bad, [](std::string_view) { return true; }));
  assert(!price_objects("[{}]", [](std::string_view) { return false; }));
  assert(price_objects(R"([{"text":"}\"{"}])", [](std::string_view) { return true; }));
}
static void daly() {
  std::array<uint8_t, 129> b{};
  b[0] = 0xd2;
  b[1] = 3;
  b[2] = 124;
  auto set = [&](int i, unsigned v) {
    b[i] = v >> 8;
    b[i + 1] = v;
  };
  auto crc = [&] {
    auto c = crc16(b.data(), 127);
    b[127] = c;
    b[128] = c >> 8;
  };
  for (int i = 0; i < 4; ++i) set(3 + 2 * i, 3300 + i);
  set(83, 132);
  set(85, 30010);
  set(87, 500);
  set(101, 4);
  set(103, 1);
  set(67, 65);
  set(109, 1);
  crc();
  Battery out;
  assert(decode_daly(b.data(), b.size(), out) && out.charge_allowed);
  assert(out.soc == 50 && std::abs(out.current - 1) < .001 && out.temp_min == 25);
  DalyFrames frames;
  for (size_t i = 0; i < b.size(); ++i) {
    bool done = frames.push(&b[i], 1, out);
    assert(done == (i + 1 == b.size()));
  }
  set(119, 0xcc00);
  crc();
  assert(decode_daly(b.data(), b.size(), out) && out.charge_allowed);
  set(119, 0x0200);
  crc();
  assert(decode_daly(b.data(), b.size(), out) && !out.charge_allowed);
  set(119, 0);
  set(121, 0x0200);
  crc();
  assert(decode_daly(b.data(), b.size(), out) && !out.charge_allowed);
  set(121, 0);
  set(109, 0);
  crc();
  assert(decode_daly(b.data(), b.size(), out) && !out.charge_allowed);
  b[10] ^= 1;
  assert(!decode_daly(b.data(), b.size(), out));
  assert(!decode_daly(b.data(), 128, out));
}
static void history() {
  History h;
  auto start = stamp("2026-11-01 12:00:00");
  assert(start % 300 == 0);
  h.add(start, 0, 400, 50, true, 100, 100, true);
  for (unsigned t = 10; t <= 3600; t += 10) h.add(start + t, 10, 400, 50, true, 100, 100, true);
  uint32_t energy = 0, coverage = 0;
  int32_t cost = 0;
  for (auto& b : h.buckets) {
    energy += b.energy_mwh;
    cost += b.cost_milliore;
    coverage += b.coverage;
  }
  assert(std::abs(int(energy) - 400000) <= 12);
  assert(std::abs(cost - 40000) <= 12);
  assert(coverage == 3600);
  if constexpr (RETAINED_DAYS > 0) assert(h.days[day_key(start) % DAY_COUNT].energy_mwh == energy);
  h.add(start + 3610, 10, 400, 50, true, UNKNOWN, 100, true);
  assert(h.current().unknown_mwh > 0);
  auto before = h.current().energy_mwh;
  h.add(start + 3620, 200, 400, 50, true, 100, 100, true);
  assert(h.current().energy_mwh == before);
  for (unsigned i = 1; i <= 300; ++i)
    h.add(start + 3600 + i * 300, 0, 0, UNKNOWN, false, UNKNOWN, 100, false);
  uint32_t oldest = UINT32_MAX, newest = 0;
  for (auto& b : h.buckets) {
    oldest = std::min(oldest, b.time);
    newest = std::max(newest, b.time);
  }
  assert(newest - oldest <= 287 * 300);
  for (unsigned i = 0; i < 200; ++i) h.event(start + i, 2, Reason::CHARGING, 50);
  assert(h.event_seq == 200);
  for (unsigned i = 0; i < 220; ++i) h.deadline(start + i * 86400, i % 2 == 0);
  assert(sizeof(h) + sizeof(Plan) + sizeof(Price) * PRICE_COUNT < 24576);
}
static void timestamps_and_large_costs() {
  assert(parse_utc_timestamp("2026-11-01T00:00:00Z") == 1793491200U);
  assert(parse_utc_timestamp("2028-02-29T12:00:00Z") != 0);
  for (auto bad : {"2026-02-29T00:00:00Z", "2026-11-31T00:00:00Z", "2026-11-01T24:00:00Z",
                   "2026-11-01T00:00:00+00:00", "999999999999-11-01T00:00:00Z"})
    assert(parse_utc_timestamp(bad) == 0);
  History h;
  Bucket b;
  b.time = 1793491200U;
  b.cost_milliore = 2000000000;
  h.add_to_day(b);
  h.add_to_day(b);
  if constexpr (RETAINED_DAYS > 0) assert(h.days[day_key(b.time) % DAY_COUNT].cost_milliore == 4000000000LL);
}
int main() {
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();
  timestamps_and_large_costs();
  planner();
  low_battery_recovery();
  json_array();
  daly();
  history();
  auto t = stamp("2026-03-28 07:00:00");
  assert(next_deadline(t, 7, 0) - t == 23 * 3600);
  t = stamp("2026-10-24 07:00:00");
  assert(next_deadline(t, 7, 0) - t == 25 * 3600);
  t = stamp("2026-11-01 23:50:00");
  assert(next_deadline(t, 7, 0) - t == 7 * 3600 + 600);
  t = stamp("2026-11-01 07:00:00");
  assert(next_deadline(t, 7, 0) - t == 86400);
  t = stamp("2026-03-29 00:00:00");
  auto spring = next_deadline(t, 2, 30);
  time_t z = spring;
  tm wall{};
  localtime_r(&z, &wall);
  assert(wall.tm_hour == 3 && wall.tm_min == 30);
  tm repeated{};
  strptime("2026-10-25 02:45:00", "%Y-%m-%d %H:%M:%S", &repeated);
  repeated.tm_isdst = 1;
  t = mktime(&repeated);
  auto autumn = next_deadline(t, 2, 30);
  assert(autumn - t == 45 * 60);
  std::cout << "PASS: planner, manual/auto fault stops, Daly CRC/alarms, energy/cost, history rings and "
               "Stockholm DST\n";
}
