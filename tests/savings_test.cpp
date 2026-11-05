#include "../components/charging_controller/savings.h"
#include <cassert>
#include <cmath>
#include <cstdio>
using namespace charging;

static void close(float actual, float expected) { assert(std::abs(actual - expected) < .0001f); }
int main() {
  uint32_t t = stockholm_wall_epoch(civil_days(2026, 11, 5), 12, 0);
  Price prices[2]{{t / 900 * 900, 100}, {t / 900 * 900 + 900, 300}};
  Savings s;
  auto sample = [&](uint32_t at, bool continuous, bool valid, bool mains, float grid, float dc) {
    s.sample(at, continuous, valid, mains, grid, dc, prices, 2, 0, .9, .9, 0, 100);
  };
  sample(t, false, true, false, 0, 0);
  sample(t+10, true, true, true, 400, 270); // 300 W charger + 100 W appliances
  auto& day = s.days[day_key(t) % 30];
  close(day.baseline_kr, 100.f*10/3600000);
  close(day.grid_kr, 400.f*10/3600000);
  close(day.inventory_kr, 300.f*10/3600000);
  close(day.benefit(), 0);
  sample(t+20, true, true, false, 0, -270); // 243 W output: equal stored energy used
  close(day.inventory_kr, 0);
  close(day.benefit(), (100+243-400.f)*10/3600000); // Conversion loss costs money.
  float before = day.benefit();
  sample(t+30, true, false, false, 0, -270);
  sample(t+40, false, true, false, 0, -270); // Restart: do not extrapolate last reading.
  sample(t+100, true, true, false, 0, -270); // Long gap also excluded.
  close(day.benefit(), before);
  assert(day.known_seconds==20 && day.missing_seconds==80);
  auto copy = s; // Persisted financial state; future reference changes do not revalue it.
  s.sample(t+110,true,true,false,0,-270,prices,2,0,.9,.9,0,900);
  close(s.reference_kr, copy.reference_kr);
  assert(day.benefit()<before); // Initially stored energy is not free.
  s.sample(t+90,true,true,false,0,-270,prices,2,0,.9,.9,0,100);
  assert(s.last_at==t+110); // No double accounting on clock rollback.
  auto restored = s;
  restored.sample(t+130,false,true,false,0,-270,prices,2,0,.9,.9,0,100);
  close(restored.days[day_key(t)%30].benefit(), day.benefit());
  assert(restored.days[day_key(t)%30].missing_seconds==100);
  restored.sample(t+140,true,true,false,0,-270,nullptr,0,0,.9,.9,0,100);
  assert(restored.days[day_key(t)%30].missing_seconds==110); // Fresh, but no published tariff.

  Savings boundary;
  boundary.sample(t+890,false,true,false,0,0,prices,2,0,1,1,0,100);
  boundary.sample(t+910,true,true,true,100,0,prices,2,0,1,1,0,100);
  close(boundary.days[day_key(t)%30].grid_kr, 100.f*10/3600000*4); // Both actual tariffs.
  boundary.sample(t+2000,true,true,true,100,0,prices,2,0,1,1,0,100);
  assert(boundary.days[day_key(t)%30].missing_seconds==1090);
  Savings negative;
  Price p{t,-100};
  negative.sample(t,false,true,false,0,0,&p,1,0,1,1,0,-100);
  negative.sample(t+10,true,true,false,0,-100,&p,1,0,1,1,0,-100);
  assert(negative.days[day_key(t)%30].benefit()<0); // Do not clamp negative savings/prices.

  for (auto date : {civil_days(2026,3,29),civil_days(2026,10,25)}) {
    Savings dst;
    uint32_t start=stockholm_wall_epoch(date,0,0), end=stockholm_wall_epoch(date+1,0,0);
    dst.started_at=dst.last_at=start;
    dst.sample(end,false,false,false,0,0,nullptr,0,0,1,1,0,100);
    assert(dst.days[date%30].missing_seconds==end-start); // 23/25-hour local day.
    dst.sample(end+10,false,false,false,0,0,nullptr,0,0,1,1,0,100);
    assert(dst.days[(date+1)%30].missing_seconds==10);
    // An arbitrarily long outage retains only the 30 recent calendar days.
    uint32_t later=stockholm_wall_epoch(date+100,12,0);
    dst.sample(later,false,false,false,0,0,nullptr,0,0,1,1,0,100);
    unsigned entries=0;
    for(auto& d:dst.days) { assert(d.day>=uint32_t(date+71)); ++entries; }
    assert(entries==30);
  }
  uint32_t midnight=stockholm_wall_epoch(day_key(t)+1,0,0);
  Price midnight_prices[2]{{midnight-900,100},{midnight,300}};
  Savings split;
  split.sample(midnight-10,false,true,true,100,0,midnight_prices,2,0,1,1,0,100);
  split.sample(midnight+10,true,true,true,100,0,midnight_prices,2,0,1,1,0,100);
  assert(split.days[day_key(t)%30].known_seconds==10);
  assert(split.days[(day_key(t)+1)%30].known_seconds==10);
  close(split.days[(day_key(t)+1)%30].grid_kr,300.f*10/3600000);
  std::puts("PASS: savings accounting, stored-energy valuation, losses, negative prices, gaps, restart, clock rollback, tariff boundaries, DST and 30-day retention");
}
