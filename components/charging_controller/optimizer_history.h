#pragma once
#include "charging_core.h"
namespace charging {
struct OptimizerSettings {
  float marginal_adder{70},charge_efficiency{.9f},discharge_efficiency{.9f},idle_w{10},fallback_load_w{60};
};
inline bool valid_optimizer_settings(const OptimizerSettings& s) {
  return std::isfinite(s.marginal_adder)&&s.marginal_adder>=0&&s.marginal_adder<=1000&&
    std::isfinite(s.charge_efficiency)&&s.charge_efficiency>=.1f&&s.charge_efficiency<=1&&
    std::isfinite(s.discharge_efficiency)&&s.discharge_efficiency>=.1f&&s.discharge_efficiency<=1&&
    std::isfinite(s.idle_w)&&s.idle_w>=0&&s.idle_w<=200&&
    std::isfinite(s.fallback_load_w)&&s.fallback_load_w>=0&&s.fallback_load_w<=2500;
}
struct OptimizerHour {
  uint32_t hour{0};
  float price{UNKNOWN},load_wh{0},off_seconds{0};
};
struct OptimizerHistory {
  // Current partial hour plus 168 completed hours for price quantiles.
  std::array<OptimizerHour,169> hours{};
  OptimizerHour& at(uint32_t hour) {
    auto& b=hours[hour%hours.size()];
    if(b.hour!=hour) {b=OptimizerHour{};b.hour=hour;}
    return b;
  }
  void observe_load(uint32_t now,float ac_w,float seconds) {
    if(!now||!std::isfinite(ac_w)||ac_w<0||seconds<=0||seconds>30) return;
    auto& b=at(now/3600);b.load_wh+=ac_w*seconds/3600;b.off_seconds+=seconds;
  }
  void ingest_prices(uint32_t now,const Price* prices,size_t count) {
    for(size_t i=0;i+3<count;++i) {
      uint32_t t=prices[i].start;
      if(t%3600 || t+3600>now || t/3600+168<now/3600) continue;
      float sum=0;bool valid=true;
      for(size_t j=0;j<4;++j) {
        valid&=prices[i+j].start==t+j*900&&std::isfinite(prices[i+j].ore);
        sum+=prices[i+j].ore;
      }
      if(valid) at(t/3600).price=sum/4;
    }
  }
  float load_w(uint32_t now,float fallback) const {
    double wh=0,seconds=0;
    for(const auto& b:hours) if(b.hour<=now/3600&&b.hour+168>now/3600) {
      wh+=b.load_wh;seconds+=b.off_seconds;
    }
    return seconds>0?float(wh*3600/seconds):fallback;
  }
  void price_estimates(uint32_t now,const Price* prices,size_t count,float& median,float& lower) const {
    std::array<float,PRICE_COUNT> samples{};size_t n=0;
    for(const auto& b:hours) if(b.hour<now/3600&&b.hour+168>=now/3600&&std::isfinite(b.price)) samples[n++]=b.price;
    if(!n) for(size_t i=0;i<count&&n<samples.size();++i)
      if(prices[i].start+900>now&&std::isfinite(prices[i].ore)) samples[n++]=prices[i].ore;
    if(!n) {median=lower=UNKNOWN;return;}
    std::sort(samples.begin(),samples.begin()+n);
    auto quantile=[&](float q) {float k=(n-1)*q;size_t a=size_t(k),z=std::min(n-1,a+1);return samples[a]+(k-a)*(samples[z]-samples[a]);};
    median=quantile(.5f);lower=quantile(.25f);
  }
};
static_assert(sizeof(OptimizerHistory)<=2720,"Bounded observation history exceeds budget");
}
