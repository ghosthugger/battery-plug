#include "../components/charging_controller/compact_optimizer.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <random>
using namespace compact;
// Independently sampled supervisor: no event-duration code or solver geometry.
static Outcome sampled(double energy,bool reserve,bool paused,int request,double load,const Config& p) {
  Outcome o; o.energy=energy;
  const int ticks=36000;const double dt=p.dt/ticks;
  for(int i=0;i<ticks;++i) {
    if(o.energy<=p.reserve+1e-12) reserve=true;
    if(o.energy>=p.recovery-1e-12) reserve=false;
    if(o.energy<=.95*p.capacity+1e-12) paused=false;
    else if(o.energy>=p.capacity-1e-12) paused=true;
    if(reserve || (request&&!paused)) {
      double change=std::min(p.capacity-o.energy,p.charge*p.eta_c*dt);
      o.energy+=change;o.charged+=change/p.eta_c;
      o.mains_load+=load*dt;o.on_hours+=dt;
    } else {
      o.energy=std::max(p.reserve,o.energy-(load/p.eta_d+p.idle)*dt);
      o.battery_load+=load*dt;o.idle+=p.idle*dt;
    }
  }
  o.grid=o.charged+o.mains_load;return o;
}
int main() {
  Config p;
  // A request at full cannot buy mains-only appliance energy.
  auto o=advance(4,false,1,.1,p);
  assert(o.grid==0&&o.on_hours==0&&o.full_paused&&o.energy<4);
  // A partial slot charges only until full, then discharges for its remainder.
  o=advance(3.95,false,1,.1,p);
  const double on=.05/(p.eta_c*p.charge);
  assert(std::abs(o.on_hours-on)<1e-10);
  assert(std::abs(o.energy-(4-(p.dt-on)*(.1/p.eta_d+p.idle)))<1e-10);
  assert(o.full_paused&&o.internal_transfers==1);
  // The same SOC has different available actions depending on the pause latch.
  auto blocked=advance(3.9,false,1,.1,p,true);
  auto allowed=advance(3.9,false,1,.1,p,false);
  assert(blocked.grid==0&&allowed.grid>0);
  o=advance(3.8,false,1,.1,p,true);
  assert(!o.full_paused&&o.grid>0);
  o=advance(4,false,1,0,Config{4,.4,.8,.4,.9,.9,0,.25});
  assert(o.grid==0&&o.energy==4&&o.full_paused);
  // A long requested-on interval cycles rather than assuming perpetual bypass.
  Config long_slot=p;long_slot.dt=12;
  o=advance(4,false,1,.1,long_slot);
  assert(o.on_hours<6&&o.internal_transfers>2);
  // Recovery remains authoritative even with overlapping high thresholds.
  Config high=p;high.reserve=3.8;high.recovery=4;
  o=advance(3.9,true,0,.1,high,true);
  assert(o.on_hours>0&&o.energy>=high.reserve);
  std::mt19937 gen(20261108);
  std::uniform_real_distribution<double> u(0,1);
  for(int i=0;i<400;++i) {
    Config c=p;c.dt=.05+u(gen);c.charge=.1+.8*u(gen);
    c.idle=.02*u(gen);double load=.3*u(gen);
    double energy=c.reserve+(c.capacity-c.reserve)*u(gen);
    if(i%3==0)energy=3.8+.2*u(gen);
    bool reserve=i%7==0,paused=i%2;int request=i%3!=0;
    auto exact=advance(energy,reserve,request,load,c,paused);
    auto ref=sampled(energy,reserve,paused,request,load,c);
    assert(std::abs(exact.energy-ref.energy)<.0002);
    assert(std::abs(exact.grid-ref.grid)<.0002);
    assert(std::abs(exact.energy-energy-c.eta_c*exact.charged+
                    exact.battery_load/c.eta_d+exact.idle)<1e-9);
    assert(std::abs(exact.battery_load+exact.mains_load-load*c.dt)<1e-9);
    assert(exact.energy>=c.reserve-1e-9&&exact.energy<=c.capacity+1e-9);
  }
  // Both actual starting pause and future full cutoffs are part of the DP.
  Solver<101> solver;
  for(int i=0;i<12;++i)solver.prices[i]=i<4?3.f:.1f;
  assert(solver.choose(p,12,.1,3.9,false,0,.1,-1,nullptr,true)==0);
  assert(solver.last_q[0]==solver.last_q[1]); // Cannot charge before 95%.
  assert(solver.choose(p,12,.1,3.8,false,0,.1)==0); // Wait for cheaper slots.
  assert(sizeof(solver)<16*1024&&sizeof(Policy<101>)<16*1024);
  std::cout<<"PASS: full cutoff, pause/release, partial-slot accounting, zero-load full, long cycles, reserve priority, 400 independent sampled comparisons, energy conservation and DP pause state\n";
}
