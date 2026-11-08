// Verifier-only bridge. No production entry points or device dependencies.
#include "../../components/charging_controller/compact_optimizer.h"
#include <iomanip>
#include <iostream>
#include <string>

static compact::Config config() {
  compact::Config p;
  std::cin >> p.capacity >> p.reserve >> p.recovery >> p.charge
           >> p.eta_c >> p.eta_d >> p.idle >> p.dt >> p.full_pause;
  return p;
}

template<int N, typename Real>
void solve(const compact::Config& p, int count, double load, double energy,
           bool latch, int previous, double terminal, double first_hours) {
  compact::Solver<N, Real> solver;
  for (int i=0; i<count; ++i) { double v; std::cin >> v; solver.prices[i]=Real(v); }
  int action=solver.choose(p,count,load,energy,latch,previous,terminal,first_hours);
  std::cout << "{\"action\":" << action << ",\"q\":[" << solver.last_q[0]
            << ',' << solver.last_q[1] << "],\"workspace_bytes\":"
            << solver.workspace_bytes() << "}\n";
}

int main() {
  std::cout << std::setprecision(17);
  std::string command;
  while (std::cin >> command) {
    if (command=="P") {
      double energy,load; int latch,requested;
      std::cin >> energy >> latch >> requested >> load;
      auto p=config();
      auto o=compact::advance(energy,latch,requested,load,p);
      std::cout << "{\"energy\":" << o.energy << ",\"grid\":" << o.grid
        << ",\"charged\":" << o.charged << ",\"battery_load\":" << o.battery_load
        << ",\"mains_load\":" << o.mains_load << ",\"idle\":" << o.idle
        << ",\"throughput\":" << o.throughput << ",\"on_hours\":" << o.on_hours
        << ",\"full_paused\":" << int(o.full_paused)
        << ",\"latched\":" << int(o.latched) << ",\"first_on\":" << int(o.first_on)
        << ",\"last_on\":" << int(o.last_on)
        << ",\"internal_transfers\":" << o.internal_transfers << "}\n";
    } else if (command=="S") {
      int nodes,count,latch,previous; std::string precision;
      double load,energy,terminal,first_hours;
      std::cin >> nodes >> precision >> count >> load >> energy >> latch >> previous >> terminal >> first_hours;
      auto p=config();
      if (count<1 || count>compact::MAX_HORIZON) return 3;
      if (nodes==101 && precision=="f") solve<101,float>(p,count,load,energy,latch,previous,terminal,first_hours);
      else if (nodes==101 && precision=="d") solve<101,double>(p,count,load,energy,latch,previous,terminal,first_hours);
      else if (nodes==1001 && precision=="d") solve<1001,double>(p,count,load,energy,latch,previous,terminal,first_hours);
      else if (nodes==10001 && precision=="d") solve<10001,double>(p,count,load,energy,latch,previous,terminal,first_hours);
      else return 4;
    } else return 2;
    std::cout.flush();
  }
}
