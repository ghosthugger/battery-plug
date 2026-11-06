#pragma once
#include "charging_core.h"
namespace charging {
// Four-byte alignment keeps the existing checksummed NVS wrapper compatible.
struct BmsSnapshot {
  uint32_t sequence{0}, time{0}, uptime_ms{0};
  float soc{UNKNOWN}, voltage{UNKNOWN}, current{UNKNOWN}, temp_min{UNKNOWN}, temp_max{UNKNOWN},
      cell_min{UNKNOWN}, cell_max{UNKNOWN}, meter_w{UNKNOWN};
  uint16_t charge_mos{0}, discharge_mos{0};
  std::array<uint16_t,4> alarms{}; // Raw registers 0x3A..0x3D, including reserved bits.
  uint8_t charge_allowed{0}, relay{0}, trigger{0}, clock_ok{0};
};
static_assert(sizeof(BmsSnapshot)==60 && alignof(BmsSnapshot)==4);
struct BmsDiagnostics {
  static constexpr unsigned COUNT=32;
  std::array<BmsSnapshot,COUNT> records{};
  uint32_t next_sequence{1};
  bool observed{false}, last_allowed{false};
  uint16_t last_charge_mos{0}, last_discharge_mos{0};
  uint64_t last_alarms{0};
  bool observe(const Battery& b,uint32_t timestamp,bool clock_ok,uint32_t uptime_ms,
               bool relay,float meter_w,BmsSnapshot& result) {
    uint8_t trigger=!observed?0:b.charge_allowed!=last_allowed?1:2;
    bool changed=!observed || b.charge_allowed!=last_allowed ||
        (!b.charge_allowed && (b.charge_mos!=last_charge_mos ||
          b.discharge_mos!=last_discharge_mos || b.alarms!=last_alarms));
    observed=true;last_allowed=b.charge_allowed;last_charge_mos=b.charge_mos;
    last_discharge_mos=b.discharge_mos;last_alarms=b.alarms;
    if(!changed) return false;
    BmsSnapshot s;
    s.sequence=next_sequence++;if(!next_sequence) next_sequence=1;
    s.time=clock_ok?timestamp:0;s.clock_ok=clock_ok;s.uptime_ms=uptime_ms;
    s.soc=b.soc;s.voltage=b.voltage;s.current=b.current;s.temp_min=b.temp_min;s.temp_max=b.temp_max;
    s.cell_min=b.cell_min;s.cell_max=b.cell_max;s.meter_w=meter_w;
    s.charge_mos=b.charge_mos;s.discharge_mos=b.discharge_mos;
    for(unsigned i=0;i<4;++i)s.alarms[i]=uint16_t(b.alarms>>(48-16*i));
    s.charge_allowed=b.charge_allowed;s.relay=relay;s.trigger=trigger;
    records[s.sequence%COUNT]=s;result=s;
    return true;
  }
  void restore(const BmsSnapshot& s) {
    if(!s.sequence) return;
    records[s.sequence%COUNT]=s;
    if(int32_t(s.sequence-next_sequence)>=0) {
      next_sequence=s.sequence+1;if(!next_sequence)next_sequence=1;
    }
    // First live sample is always a baseline, never a fabricated transition.
  }
};
static_assert(sizeof(BmsDiagnostics)<=2048,"BMS diagnostics exceed the 2 KiB RAM budget");
} // namespace charging
