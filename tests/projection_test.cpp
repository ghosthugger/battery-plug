#include "../components/charging_controller/compact_optimizer.h"
#include <cassert>
#include <cstdio>
#include <memory>
using namespace compact;
int main() {
  Config p;
  Solver<101> plain, traced, suffix;
  auto policy=std::make_unique<Policy<101>>();
  // Exhaust the packed-table addressing, including both latches and last bit.
  for(int t=0;t<MAX_HORIZON;++t) for(int l=0;l<4;++l) for(int i=0;i<101;++i)
    policy->set(t,l,i,(t+l+i)%2);
  for(int t=0;t<MAX_HORIZON;++t) for(int l=0;l<4;++l) for(int i=0;i<101;++i)
    assert(policy->action(t,l&1,EnergyGrid<101>::at(i,p),p,(l&2)!=0)==(t+l+i)%2);
  // Merely requesting display data must never change the live decision or Qs.
  for(int scenario=0;scenario<100;++scenario) {
    int count=1+scenario%99;
    for(int i=0;i<count;++i) plain.prices[i]=traced.prices[i]=float(((i*37+scenario*13)%140)-20)/20;
    double energy=p.reserve+(p.capacity-p.reserve)*(scenario%101)/100;
    double hours=scenario%2?.25:1./3600;
    int a=plain.choose(p,count,.1,energy,scenario%3==0,scenario%2,.7,hours);
    int b=traced.choose(p,count,.1,energy,scenario%3==0,scenario%2,.7,hours,policy.get());
    assert(a==b && plain.last_q[0]==traced.last_q[0] && plain.last_q[1]==traced.last_q[1]);
  }
  constexpr int count=36;
  for(int i=0;i<count;++i) traced.prices[i]=(i>=8&&i<16)?.1f:10.f;
  double energy=.9; bool latch=false,full_paused=false;
  int first=traced.choose(p,count,.1,energy,latch,0,.1,1./3600,policy.get());
  assert(first==0);
  unsigned cheap_on=0,expensive_on=0,differences=0;
  int previous=0;
  for(int t=0;t<count;++t) {
    int action=t?policy->action(t,latch,energy,p,full_paused):first;
    for(int i=t;i<count;++i) suffix.prices[i-t]=traced.prices[i];
    int oracle=suffix.choose(p,count-t,.1,energy,latch,previous,.1,t?.25:1./3600,nullptr,full_paused);
    differences+=action!=oracle;
    Config step=p;if(!t)step.dt=1./3600;
    auto o=advance(energy,latch,action,.1,step,full_paused);
    if(o.on_hours>0) (t>=8&&t<16?cheap_on:expensive_on)++;
    assert(o.energy>=p.reserve-1e-8 && o.energy<=p.capacity+1e-8);
    energy=o.energy;latch=o.latched;full_paused=o.full_paused;previous=o.last_on;
  }
  assert(cheap_on>0 && expensive_on==0);
  // An off request still predicts mandatory low-battery recovery physically.
  auto o=advance(p.reserve+.001,false,0,.1,p);
  assert(o.on_hours>0 && o.on_hours<p.dt && o.energy>=p.reserve);
  // Recording can be omitted entirely on allocation failure (old API remains).
  assert(plain.choose(p,1,.1,1,false,0,.1,.25,nullptr)==
         plain.choose(p,1,.1,1,false,0,.1,.25));
  std::printf("PASS: unchanged live decisions/Qs, packed policy bounds, partial first slot, cheap-window projection, recovery; %u/%d future actions differ from continuous suffix oracle; temporary policy %zu bytes\n",differences,count,sizeof(*policy));
}
