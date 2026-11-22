#include "../components/charging_controller/charging_core.h"
#include <cassert>
#include <cmath>
#include <iostream>
using namespace charging;
int main() {
  Settings settings;Inputs in;Plan plan;Control c;
  in.now=1800000000;in.soc=70;in.clock_ok=in.bms_fresh=in.meter_fresh=true;
  plan.current=0;
  c.set_mode(Mode::ALWAYS,in.now,settings);
  assert(c.request_postpone(in,300,10,20));
  assert(c.mode==Mode::AUTO&&c.postpone_until==in.now+18000);
  c.on=true;c.last_switch=in.now;
  assert(!c.evaluate_optimized(settings,in,plan,nullptr,true,true,10,20));
  assert(c.reason==Reason::POSTPONED); // Immediate stop despite cooldown/cheap price.
  assert(c.postponing(in));
  auto end=c.postpone_until;
  in.now=end-1;
  assert(!c.evaluate_optimized(settings,in,plan,nullptr,true,true,10,20));
  in.now=end;
  assert(c.evaluate_optimized(settings,in,plan,nullptr,true,true,10,20));
  assert(!c.postpone_until); // Normal automatic request resumes on expiry.
  assert(c.request_postpone(in,60,10,20));
  in.now+=121;in.soc=10;
  assert(c.evaluate_optimized(settings,in,plan,nullptr,false,false,10,20));
  assert(c.reason==Reason::LOW_BATTERY&&c.reserve_active&&!c.postpone_until);
  in.soc=15;
  assert(!c.request_postpone(in,60,10,20)); // Recovery can't be interrupted.
  assert(c.evaluate_optimized(settings,in,plan,nullptr,false,false,10,20));
  in.soc=20;
  assert(!c.evaluate_optimized(settings,in,plan,nullptr,true,false,10,20));
  assert(!c.reserve_active&&!c.postpone_until);
  assert(c.request_postpone(in,60,10,20));
  in.fault=true;
  assert(!c.evaluate_optimized(settings,in,plan,nullptr,true,true,10,20));
  assert(c.reason==Reason::FAULT);
  assert(!c.request_postpone(in,60,10,20));
  in.fault=false;
  Control restored;restored.mode=Mode::AUTO;restored.postpone_until=c.postpone_until;
  assert(!restored.evaluate_optimized(settings,in,plan,nullptr,true,true,10,20));
  assert(restored.reason==Reason::POSTPONED);
  in.clock_ok=false;
  assert(!restored.evaluate_optimized(settings,in,plan,nullptr,true,true,10,20));
  assert(restored.postpone_until); // Startup before NTP must not discard saved deadline.
  in.soc=5;
  assert(restored.evaluate_optimized(settings,in,plan,nullptr,false,false,10,20));
  assert(!restored.postpone_until&&restored.reserve_active); // Recovery needs no clock.
  in.clock_ok=true;in.soc=70;
  assert(c.request_postpone(in,60,10,20));
  in.bms_fresh=false;
  assert(!c.request_postpone(in,60,10,20));
  assert(!c.evaluate_optimized(settings,in,plan,nullptr,true,true,10,20));
  assert(c.reason==Reason::BMS);
  in.bms_fresh=true;in.meter_fresh=false;
  assert(!c.request_postpone(in,60,10,20));
  in.meter_fresh=true;
  assert(c.request_postpone(in,0,10,20)&&!c.postpone_until);
  for(auto minutes:{1u,14u,1441u,UINT32_MAX})assert(!c.request_postpone(in,minutes,10,20));
  assert(c.request_postpone(in,1440,10,20));
  c.set_mode(Mode::ALWAYS,in.now,settings);assert(!c.postpone_until);
  assert(c.request_postpone(in,15,10,20));
  c.set_mode(Mode::AUTO,in.now,settings);assert(!c.postpone_until);
  in.soc=UNKNOWN;assert(!c.request_postpone(in,60,10,20));
  in.soc=70;
  assert(std::abs(battery_runtime_hours(settings,in,10,72,.9,10)-float(2.4/.09))<.001f);
  in.soc=10;assert(battery_runtime_hours(settings,in,10,72,.9,10)==0);
  in.soc=70;in.bms_fresh=false;
  assert(!std::isfinite(battery_runtime_hours(settings,in,10,72,.9,10)));
  in.bms_fresh=true;
  assert(!std::isfinite(battery_runtime_hours(settings,in,10,0,.9,0)));
  std::cout<<"PASS: timed postponement, immediate stop, expiry, recovery cancellation/priority, restart state, pre-NTP recovery, faults, mode/cancel controls, duration bounds and runtime estimate\n";
}
