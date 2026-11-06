#pragma once
#include "charging_core.h"
namespace charging {
// A read-only D2 holding-register block. Never used to change control limits.
struct DalySettings {
  std::array<uint16_t,41> registers{};
  uint16_t get(unsigned address) const { return registers[address-0x80]; }
};
class DalySettingsFrames {
 public:
  void reset() { size_=0; }
  bool push(const uint8_t* bytes,size_t length,DalySettings& result) {
    bool found=false;
    for(size_t i=0;i<length;++i) {
      uint8_t b=bytes[i];
      if(!size_ && b!=0xd2)continue;
      data_[size_++]=b;
      if((size_==2 && b!=3) || (size_==3 && b!=82)) {
        size_=0;if(b==0xd2)data_[size_++]=b;
        continue;
      }
      if(size_==data_.size()) {
        if(crc16(data_.data(),85)==(data_[85]|uint16_t(data_[86])<<8)) {
          for(unsigned j=0;j<41;++j)result.registers[j]=uint16_t(data_[3+2*j])<<8|data_[4+2*j];
          found=true;
        }
        size_=0;
      }
    }
    return found;
  }
 private:
  std::array<uint8_t,87> data_{};
  size_t size_{0};
};
static_assert(sizeof(DalySettings)*2+sizeof(DalySettingsFrames)<300);
} // namespace charging
