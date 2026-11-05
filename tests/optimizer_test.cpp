#include <cassert>
#include <cmath>
#include <iostream>
#include "../components/charging_controller/charging_core.h"
#include "../components/charging_controller/compact_optimizer.h"
#include "../components/charging_controller/optimizer_history.h"
using namespace charging;
int main() {
 Settings s;Inputs in;in.now=1800000000;in.clock_ok=in.bms_fresh=in.meter_fresh=true;in.soc=90;
 Plan p;p.current=0;p.current_price=1000;Price price{in.now,1000};
 Control c;c.set_mode(Mode::AUTO,in.now,s);
 // Automatic can use mains at the old target and above the old price ceiling.
 assert(c.evaluate_optimized(s,in,p,&price,true,true,10,20));
 assert(c.reason==Reason::CHARGING);
 in.now+=900;assert(!c.evaluate_optimized(s,in,p,&price,true,false,10,20));
 // Recovery overrides absent prices/clock/plan, but not fault/data protection.
 in.soc=10;p.current=-1;in.clock_ok=false;
 assert(c.evaluate_optimized(s,in,p,&price,false,false,10,20)&&c.reserve_active);
 in.fault=true;assert(!c.evaluate_optimized(s,in,p,&price,false,true,10,20));
 in.fault=false;in.bms_fresh=false;assert(!c.evaluate_optimized(s,in,p,&price,true,true,10,20));
 in.bms_fresh=true;in.meter_fresh=false;assert(!c.evaluate_optimized(s,in,p,&price,true,true,10,20));
 in.meter_fresh=true;assert(c.evaluate_optimized(s,in,p,&price,false,false,10,20));
 in.soc=19.9;assert(c.evaluate_optimized(s,in,p,&price,false,false,10,20));
 in.soc=20;assert(!c.evaluate_optimized(s,in,p,&price,false,false,10,20)&&!c.reserve_active);
 in.clock_ok=true;p.current=0;in.now+=900;
 assert(!c.evaluate_optimized(s,in,p,&price,false,true,10,20)&&c.reason==Reason::PLANNER);
 assert(c.evaluate_optimized(s,in,p,&price,true,true,10,20));
 // Only non-expiring operating modes are accepted; legacy records migrate safely.
 assert(!valid_operating_mode(0)&&!valid_operating_mode(2));
 assert(valid_operating_mode(1)&&valid_operating_mode(3));
 assert(restored_operating_mode(0)==Mode::AUTO&&restored_operating_mode(2)==Mode::AUTO);
 assert(restored_operating_mode(3)==Mode::ALWAYS);
 assert(!valid_operating_reserve(0,10)&&valid_operating_reserve(10,20));
 c.set_mode(Mode::ALWAYS,in.now,s);in.soc=100;in.clock_ok=false;p.current=-1;
 assert(c.evaluate_optimized(s,in,p,&price,false,false,10,20));
 assert(c.reason==Reason::ALWAYS&&c.mode==Mode::ALWAYS);
 in.now+=3*86400;
 assert(c.evaluate_optimized(s,in,p,&price,false,false,10,20)&&c.mode==Mode::ALWAYS);
 in.soc=5;assert(c.evaluate_optimized(s,in,p,&price,false,false,10,20));
 in.fault=true;assert(!c.evaluate_optimized(s,in,p,&price,false,false,10,20));
 assert(c.mode==Mode::ALWAYS);
 in.fault=false;assert(c.evaluate_optimized(s,in,p,&price,false,false,10,20));
 in.bms_fresh=false;assert(!c.evaluate_optimized(s,in,p,&price,false,false,10,20));
 in.bms_fresh=true;in.meter_fresh=false;assert(!c.evaluate_optimized(s,in,p,&price,false,false,10,20));
 in.meter_fresh=true;in.soc=100;
 assert(c.evaluate_optimized(s,in,p,&price,false,false,10,20));
 OptimizerHistory history;OptimizerSettings settings;
 assert(valid_optimizer_settings(settings));settings.idle_w=NAN;assert(!valid_optimizer_settings(settings));
 uint32_t hour=in.now/3600*3600;
 std::array<Price,8> prices;for(int i=0;i<8;++i)prices[i]={hour+unsigned(i)*900,float(i<4?40:900)};
 // Completed historical hours only; tomorrow/high prices cannot leak into history.
 history.ingest_prices(hour+3600,prices.data(),prices.size());
 float median,lower;history.price_estimates(hour+3600,prices.data(),prices.size(),median,lower);
 assert(median==40&&lower==40);
 history.observe_load(hour+3600,100,10);history.observe_load(hour+3610,50,20);
 assert(std::abs(history.load_w(hour+3610,60)-200.f/3)<1e-4);
 assert(history.load_w(hour+169*3600,60)==60);
 OptimizerHistory week;
 for(unsigned i=0;i<168;++i) {
   std::array<Price,4> one;
   for(unsigned j=0;j<4;++j) one[j]={hour+i*3600+j*900,float(i)};
   week.ingest_prices(hour+(i+1)*3600,one.data(),one.size());
 }
 // Recording the partial current hour must retain all 168 completed prices.
 week.observe_load(hour+168*3600,60,10);
 week.price_estimates(hour+168*3600,prices.data(),prices.size(),median,lower);
 assert(median==83.5f&&lower==41.75f);
 compact::Config cfg;compact::Solver<101> solver;
 assert(sizeof(solver)<8*1024);
 solver.prices[0]=10;solver.prices[1]=.1;
 // Short first slot is handled explicitly, without pretending 15 min remains.
 auto a=compact::advance(.41,false,0,.1,cfg);
 assert(a.latched&&a.energy>=cfg.reserve&&a.grid>0);
 cfg.dt=1./3600;auto short_slot=compact::advance(.41,false,0,.1,cfg);
 assert(!short_slot.latched&&short_slot.grid==0&&short_slot.energy>.409);
 cfg.dt=.25;
 int decision=solver.choose(cfg,2,.1,2,false,0,.1,1./3600);
 assert(decision==0);
 std::cout<<"PASS: non-expiring Automatic/Always charging modes, fault precedence, recovery latch, bounded histories, partial slots; workspace "<<sizeof(solver)<<" bytes\n";
}
