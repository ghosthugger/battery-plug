#include "../components/charging_controller/bms_handoff.h"
#include "../components/charging_controller/charging_core.h"
#include <cassert>
#include <cstdio>
using charging::BmsHandoff;
int main() {
  BmsHandoff h;
  assert(!h.start(100,false,true));
  assert(!h.start(100,true,false));
  assert(h.start(100,true,true));
  assert(!h.start(200,true,true));
  assert(h.remaining_seconds(100)==300);
  h.update(60100,false,false);
  assert(h.paused()); // Stale readings alone must not cut short phone access.
  h.update(300100,false,false);
  assert(h.state==BmsHandoff::State::RECONNECTING);
  assert(h.remaining_seconds(300100)==45);
  h.reconnect(310100);
  assert(h.remaining_seconds(310100)==35);
  h.update(345100,false,false);
  assert(!h.active()); // No phone release: bounded grace, then normal checks.
  assert(h.start(UINT32_MAX-1000,true,true));
  h.update(299000,false,false);
  assert(h.state==BmsHandoff::State::RECONNECTING); // Timer wraps safely.
  h.update(299001,true,false);
  assert(!h.active());
  assert(h.start(100,true,true));
  h.update(101,true,true);
  assert(!h.active()); // Fault wins even during handoff.
  assert(h.start(100,true,true));
  h.reconnect(200);
  h.update(201,true,false);
  assert(!h.active());
  // Exercise the real charging decision: stale data holds mains only during the
  // bounded window, without fabricating physical relay transitions/cooldowns.
  charging::Control c;
  c.mode=charging::Mode::AUTO;c.on=true;c.last_switch=100;
  charging::Settings s;
  charging::Inputs in;
  in.now=500;in.clock_ok=true;in.bms_fresh=false;in.meter_fresh=true;in.soc=80;
  charging::Plan p;p.current=0;
  assert(c.evaluate_optimized(s,in,p,nullptr,true,false,5,10,true));
  assert(c.reason==charging::Reason::BMS_HANDOFF && c.last_switch==100);
  in.now=800;
  assert(c.evaluate_optimized(s,in,p,nullptr,true,false,5,10,true));
  assert(c.last_switch==100);
  in.bms_fresh=true;in.now=801;
  assert(!c.evaluate_optimized(s,in,p,nullptr,true,false,5,10,false));
  assert(c.reason==charging::Reason::WAITING && c.last_switch==801);
  c.on=true;in.fault=true;
  assert(!c.evaluate_optimized(s,in,p,nullptr,true,false,5,10,true));
  c.on=true;in.fault=false;in.meter_fresh=false;
  assert(!c.evaluate_optimized(s,in,p,nullptr,true,false,5,10,true));
  c.on=true;in.meter_fresh=true;in.bms_fresh=false;
  assert(!c.evaluate_optimized(s,in,p,nullptr,true,false,5,10,false));
  puts("PASS: bounded Bluetooth handoff, reconnection, faults and timer wrap");
}
