#include "../components/charging_controller/compact_optimizer.h"
#include <vector>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <cassert>
struct Row {uint32_t at;double price,load,pred,tail,terminal;int known,end;};
struct Observed { uint32_t hour=0;double energy=0,hours=0; };
template<int N,typename Real> int run(int argc,char** argv) {
 std::ifstream f(argv[1]);std::vector<Row> r;Row x;while(f>>x.at>>x.price>>x.load>>x.pred>>x.tail>>x.terminal>>x.known>>x.end)r.push_back(x);
 int first=std::stoi(argv[2]),last=std::stoi(argv[3]);double settle=std::stod(argv[4]);
 double scale=std::stod(argv[5]);bool observed=std::stoi(argv[6]);
 compact::Config p;p.idle=std::stod(argv[7]);
 int model=argc==12?std::stoi(argv[11]):2;
 p.full_pause=model!=0;auto prediction=p;prediction.full_pause=model==2;
 compact::Solver<N,Real> solver;std::array<Observed,168> observations{};
 double e=2,bill=0,grid=0,charged=0,thru=0,idle=0,batteryload=0,mainsload=0;
 double min_soc=50,max_soc=50,pred_error=0,solve_s=0,max_solve=0;
 uint64_t changes=0,internals=0,recovery_slots=0;int prev_request=0;bool latch=false,relay=false,full_paused=false;
 std::ofstream trace(argv[10]);trace<<"time,energy_kwh,relay,request,latch,grid_kwh,cost_sek,predicted_load_kw,battery_load_kwh,mains_load_kwh,idle_kwh\n";
 for(int t=first;t<last;++t) {
   double sum=0,hours=0;uint32_t h=r[t].at/3600;
   for(auto& b:observations) if(b.hours && b.hour+168>h) {sum+=b.energy;hours+=b.hours;}
   double pred=observed?(hours>1e-9?sum/hours:.06):r[t].pred*scale;
   int stop=std::min(last,r[t].end),known=std::min(last,r[t].known),count=stop-t;
   assert(count>0 && count<=compact::MAX_HORIZON);
   for(int j=t;j<stop;++j) solver.prices[j-t]=Real(j<known?r[j].price:r[t].tail);
   auto before=std::chrono::steady_clock::now();
   int a=solver.choose(prediction,count,pred,e,latch,prev_request,r[t].terminal,-1,nullptr,full_paused);
   double sec=std::chrono::duration<double>(std::chrono::steady_clock::now()-before).count();
   solve_s+=sec;max_solve=std::max(max_solve,sec);
   auto o=compact::advance(e,latch,a,r[t].load*scale,p,full_paused);
   changes+=(o.first_on!=relay)+o.internal_transfers;internals+=o.internal_transfers;
   relay=o.last_on;prev_request=o.last_on?1:0;latch=o.latched;full_paused=o.full_paused;e=o.energy;
   if(latch)++recovery_slots;
   min_soc=std::min(min_soc,e*25);max_soc=std::max(max_soc,e*25);
   bill+=o.grid*r[t].price;grid+=o.grid;charged+=o.charged;thru+=o.throughput;idle+=o.idle;
   batteryload+=o.battery_load;mainsload+=o.mains_load;pred_error+=std::abs(pred-r[t].load*scale)*.25;
   auto& b=observations[h%168];if(b.hour!=h) b={h,0,0};
   b.energy+=o.battery_load;b.hours+=p.dt-o.on_hours;
   trace<<std::setprecision(14)<<r[t].at<<','<<e<<','<<relay<<','<<a<<','<<latch<<','<<o.grid<<','<<o.grid*r[t].price<<','<<pred<<','<<o.battery_load<<','<<o.mains_load<<','<<o.idle<<'\n';
 }
 double baseline=0,total=0;for(int t=first;t<last;++t) {baseline+=r[t].price*r[t].load*scale*.25;total+=r[t].load*scale*.25;}
 assert(std::abs(total-batteryload-mainsload)<1e-8);
 assert(std::abs(e-2-p.eta_c*charged+batteryload/p.eta_d+idle)<1e-8);
 assert(std::abs(grid-charged-mainsload)<1e-8);
 double adjusted=bill-settle*(e-2);
 std::cout<<std::setprecision(14)<<"{\"nodes\":"<<N<<",\"float_bytes\":"<<sizeof(Real)<<",\"workspace_bytes\":"<<sizeof(solver)
 <<",\"observed_load_only\":"<<observed<<",\"load_scale\":"<<scale<<",\"idle_w\":"<<p.idle*1000
 <<",\"baseline_sek\":"<<baseline<<",\"grid_bill_sek\":"<<bill<<",\"adjusted_cost_sek\":"<<adjusted<<",\"saving_sek\":"<<baseline-adjusted
 <<",\"grid_kwh\":"<<grid<<",\"charge_ac_kwh\":"<<charged<<",\"throughput_dc_kwh\":"<<thru
 <<",\"end_soc\":"<<e*25<<",\"minimum_end_slot_soc\":"<<min_soc<<",\"maximum_end_slot_soc\":"<<max_soc
 <<",\"source_transfers\":"<<changes<<",\"internal_transfers\":"<<internals<<",\"recovery_latched_slot_ends\":"<<recovery_slots
 <<",\"load_forecast_mae_kw\":"<<pred_error/(.25*(last-first))<<",\"solve_total_seconds\":"<<solve_s<<",\"solve_max_seconds\":"<<max_solve<<"}\n";
 return 0;
}
int main(int argc,char**argv) {
 if(argc!=11 && argc!=12) return 2;
 int n=std::stoi(argv[8]);bool dbl=std::stoi(argv[9]);
 #define CASE(N) case N:return dbl?run<N,double>(argc,argv):run<N,float>(argc,argv)
 switch(n) {CASE(101);CASE(201);CASE(401);CASE(601);CASE(1001);default:return 3;}
}
