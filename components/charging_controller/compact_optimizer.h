#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
namespace compact {
constexpr int MAX_HORIZON=300;
struct Config {
  double capacity=4, reserve=.4, recovery=.8, charge=.4, eta_c=.9, eta_d=.9, idle=.01, dt=.25;
};
struct Outcome {
  double energy=0, grid=0, charged=0, battery_load=0, mains_load=0, idle=0, throughput=0, on_hours=0;
  bool latched=false, first_on=false, last_on=false;
  uint64_t internal_transfers=0;
};
// Event-driven continuous physical plant. No optional mid-interval decisions.
inline Outcome advance(double energy,bool latched,int requested,double load,const Config& p) {
  Outcome o; o.energy=energy;
  double remain=p.dt; bool first=true,previous=false;
  while(remain>1e-12) {
    if(o.energy<=p.reserve+1e-12) latched=true;
    if(o.energy>=p.recovery-1e-12) latched=false;
    // Very narrow recovery bands can contain many complete charge/discharge
    // cycles in one interval. Account for all of them without truncation or
    // an unbounded per-cycle loop.
    if(!requested && latched && std::abs(o.energy-p.reserve)<1e-12) {
      double delta=p.recovery-p.reserve,draw=load/p.eta_d+p.idle;
      if(delta>0 && draw>0 && p.charge>0) {
        double on_time=delta/(p.eta_c*p.charge),off_time=delta/draw;
        uint64_t cycles=uint64_t(std::floor(remain/(on_time+off_time)));
        if(cycles) {
          if(first) {o.first_on=true;first=false;}
          else o.internal_transfers+=!previous;
          o.internal_transfers+=2*cycles-1;
          previous=false;o.last_on=false;
          o.charged+=cycles*delta/p.eta_c;o.mains_load+=cycles*load*on_time;
          o.grid+=cycles*(delta/p.eta_c+load*on_time);
          o.battery_load+=cycles*load*off_time;o.idle+=cycles*p.idle*off_time;
          o.throughput+=cycles*2*delta;o.on_hours+=cycles*on_time;
          remain-=cycles*(on_time+off_time);
          continue;
        }
      }
    }
    bool on=requested==1 || latched;
    if(first) {o.first_on=on;first=false;}
    else o.internal_transfers+=on!=previous;
    previous=on;o.last_on=on;
    double duration=remain;
    if(on) {
      if(latched) duration=std::min(duration,(p.recovery-o.energy)/(p.eta_c*p.charge));
      duration=std::max(0.,duration);
      double ac=std::min(p.charge*duration,std::max(0.,p.capacity-o.energy)/p.eta_c);
      o.energy+=p.eta_c*ac;o.charged+=ac;o.grid+=load*duration+ac;
      o.mains_load+=load*duration;o.throughput+=p.eta_c*ac;o.on_hours+=duration;
      if(latched && o.energy>=p.recovery-1e-12) latched=false;
    } else {
      double draw=load/p.eta_d+p.idle;
      if(draw>0) duration=std::min(duration,std::max(0.,o.energy-p.reserve)/draw);
      o.energy-=draw*duration;o.battery_load+=load*duration;
      o.idle+=p.idle*duration;o.throughput+=draw*duration;
      if(o.energy<=p.reserve+1e-12) {o.energy=p.reserve;latched=true;}
    }
    remain-=duration;
  }
  o.latched=latched;
  return o;
}
// Display-only policy: one bit per time/latch/SOC node, allocated only while
// solving. The current action still uses continuous measured SOC and exact Qs.
template<int N> struct Policy {
  std::array<uint8_t,(MAX_HORIZON*2*N+7)/8> bits{};
  void set(int t,int latch,int i,bool on) {
    size_t bit=(t*2+latch)*N+i;
    uint8_t mask=uint8_t(1u<<(bit%8));
    if(on) bits[bit/8]|=mask; else bits[bit/8]&=uint8_t(~mask);
  }
  int action(int t,bool latch,double energy,const Config& p) const {
    int i=int(std::lround(std::clamp((energy-p.reserve)/(p.capacity-p.reserve),0.,1.)*(N-1)));
    size_t bit=(t*2+int(latch))*N+i;
    return (bits[bit/8]>>(bit%8))&1;
  }
};
static_assert(sizeof(Policy<101>)<8*1024,"Display policy exceeds temporary RAM budget");
template<int N,typename Real=float> class Solver {
 public:
  // Interpolation geometry reused for every stage of a constant-load forecast.
  struct Edge { Real grid, weight; uint16_t lower; uint8_t latch; };
  std::array<Real,MAX_HORIZON> prices{};
  std::array<std::array<std::array<Edge,N>,2>,2> edges{};
  std::array<std::array<std::array<Real,N>,2>,2> values{};
  double last_q[2]{};
  size_t workspace_bytes() const {return sizeof(*this);}
  Edge geometry(const Outcome& o,const Config& p) const {
    double f=std::clamp((o.energy-p.reserve)/(p.capacity-p.reserve)*(N-1),0.,double(N-1));
    int lo=std::min(N-2,int(f));
    return {Real(o.grid),Real(f-lo),uint16_t(lo),uint8_t(o.latched)};
  }
  Real continuation(const Edge& e,const std::array<std::array<Real,N>,2>& v) const {
    Real a=v[e.latch][e.lower],b=v[e.latch][e.lower+1];
    return a+e.weight*(b-a);
  }
  int choose(const Config& p,int count,double predicted_load,double energy,bool latch,int previous,double terminal,
             double first_hours=-1,Policy<N>* policy=nullptr) {
    int next=0,cur=1;
    for(int l=0;l<2;++l) for(int i=0;i<N;++i) {
      double e=p.reserve+(p.capacity-p.reserve)*i/(N-1);
      values[next][l][i]=Real(-terminal*(e-p.reserve));
      for(int a=0;a<2;++a) edges[l][a][i]=geometry(advance(e,l,a,predicted_load,p),p);
    }
    // Keep V at t=1; evaluate t=0 at the continuous actual measured SOC.
    for(int t=count-1;t>=1;--t) {
      for(int l=0;l<2;++l) for(int i=0;i<N;++i) {
        Real best=std::numeric_limits<Real>::infinity();
        Real off=0;
        for(int a=0;a<2;++a) {
          const auto& g=edges[l][a][i];
          Real q=prices[t]*g.grid+continuation(g,values[next]);
          if(a==0) off=q;
          else if(policy) policy->set(t,l,i,q<off);
          best=std::min(best,q);
        }
        values[cur][l][i]=best;
      }
      std::swap(next,cur);
    }
    for(int a=0;a<2;++a) {
      Config first=p;
      if(first_hours>0) first.dt=first_hours;
      auto g=geometry(advance(energy,latch,a,predicted_load,first),p);
      last_q[a]=double(prices[0]*g.grid+continuation(g,values[next]));
    }
    int best=last_q[1]<last_q[0]?1:0;
    if(last_q[previous]<=last_q[best]+1e-9) best=previous;
    return best;
  }
};
}
