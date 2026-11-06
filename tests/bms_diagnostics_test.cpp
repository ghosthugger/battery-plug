#include "../components/charging_controller/bms_diagnostics.h"
#include <cassert>
#include <cstddef>
#include <cstring>
#include <cstdio>
using namespace charging;
struct Stored {uint32_t version{1};BmsSnapshot data{};uint32_t checksum{0};};
static_assert(offsetof(Stored,checksum)==sizeof(Stored)-4 && sizeof(Stored)==68);
int main() {
  BmsDiagnostics log;BmsSnapshot s;Battery b;
  b.soc=100;b.voltage=14.4;b.current=20;b.cell_min=3.5;b.cell_max=3.65;
  b.temp_min=25;b.temp_max=26;b.charge_mos=1;b.discharge_mos=1;b.charge_allowed=true;
  assert(log.observe(b,1800000000,true,1000,true,400,s));
  assert(s.sequence==1 && s.trigger==0 && s.relay && s.charge_allowed);
  assert(s.cell_max==3.65f && s.charge_mos==1 && s.discharge_mos==1);
  assert(!log.observe(b,1800000010,true,11000,true,400,s));
  b.charge_allowed=false;b.charge_mos=0;b.alarms=0x123456789abcdef0ULL;
  assert(log.observe(b,1800000020,true,21000,true,400,s));
  assert(s.sequence==2 && s.trigger==1 && !s.charge_allowed && s.relay);
  assert(s.alarms[0]==0x1234 && s.alarms[1]==0x5678 && s.alarms[2]==0x9abc && s.alarms[3]==0xdef0);
  b.cell_max=3.60f;
  assert(!log.observe(b,1800000030,true,31000,false,0,s)); // No writes for ordinary readings.
  b.alarms=0x123456789abcdef1ULL;
  assert(log.observe(b,1800000040,true,41000,false,0,s) && s.trigger==2);
  b.charge_allowed=true;b.charge_mos=1;b.alarms=0;
  assert(log.observe(b,0,false,51000,false,NAN,s));
  assert(s.trigger==1 && !s.clock_ok && s.time==0 && s.uptime_ms==51000 && std::isnan(s.meter_w));
  // Persist exactly the snapshot, including raw/reserved bits, with wrapper CRC.
  Stored record;record.data=s;record.checksum=crc32(&record,sizeof(record)-4);
  assert(record.checksum==crc32(&record,sizeof(record)-4));
  record.data.charge_mos^=1;
  assert(record.checksum!=crc32(&record,sizeof(record)-4));
  for(unsigned i=0;i<100;++i) {
    b.charge_allowed=!b.charge_allowed;
    assert(log.observe(b,1800000050+i*10,true,61000+i*10000,false,0,s));
  }
  unsigned valid=0;for(auto& x:log.records){assert(x.sequence>=log.next_sequence-32);++valid;}
  assert(valid==32);
  // NVS slots are read in slot order, not sequence order. Restore all 32.
  BmsDiagnostics restored;
  for(auto& x:log.records)restored.restore(x);
  assert(restored.next_sequence==log.next_sequence);
  assert(!restored.observed);
  assert(restored.observe(b,1800002000,true,1000,false,0,s));
  assert(s.trigger==0 && s.sequence==log.next_sequence); // Restart baseline, not fake transition.
  std::puts("PASS: BMS snapshots, raw alarm/MOS capture, change-only writes, CRC layout, ring overwrite, out-of-order restore and restart baseline; retained RAM <2 KiB");
}
