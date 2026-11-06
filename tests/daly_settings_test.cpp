#include "../components/charging_controller/daly_settings.h"
#include <cassert>
#include <cstdio>
using namespace charging;
int main() {
  DalySettingsFrames frames;DalySettings result;
  std::array<uint8_t,87> reply{};
  reply[0]=0xd2;reply[1]=3;reply[2]=82;
  auto set=[&](unsigned reg,unsigned value){unsigned i=3+2*(reg-0x80);reply[i]=value>>8;reply[i+1]=value;};
  set(0x80,2800);set(0x83,4);set(0x89,0);set(0x8b,3550);set(0x8c,3650);set(0xa3,3400);set(0xa4,20);
  auto crc=crc16(reply.data(),85);reply[85]=crc;reply[86]=crc>>8;
  for(unsigned i=0;i<86;++i)assert(!frames.push(&reply[i],1,result));
  assert(frames.push(&reply[86],1,result));
  assert(result.get(0x80)==2800 && result.get(0x83)==4 && result.get(0x8c)==3650);
  assert(result.get(0xa3)==3400 && result.get(0xa4)==20 && result.get(0x89)==0);
  reply[85]^=1;assert(!frames.push(reply.data(),reply.size(),result));reply[85]^=1;
  // Normal status replies and unsupported Modbus exceptions cannot be settings.
  std::array<uint8_t,129> status{};status[0]=0xd2;status[1]=3;status[2]=124;
  assert(!frames.push(status.data(),status.size(),result));
  uint8_t exception[]={0xd2,0x83,2,0,0};assert(!frames.push(exception,sizeof(exception),result));
  assert(frames.push(reply.data(),reply.size(),result));
  // Standard battery decoder remains isolated from the shorter settings frame.
  DalyFrames standard;Battery battery;
  assert(!standard.push(reply.data(),reply.size(),battery));
  // Truncation followed by timeout/reset must recover on the next complete read.
  frames.push(reply.data(),20,result);frames.reset();
  assert(frames.push(reply.data(),reply.size(),result));
  uint8_t request[]={0xd2,3,0,0x80,0,0x29};
  assert(crc16(request,sizeof(request))==0x5f96); // Read function only.
  std::puts("PASS: isolated read-only settings frames, fragmentation/CRC, status/exception rejection, timeout reset, mapping and request CRC");
}
