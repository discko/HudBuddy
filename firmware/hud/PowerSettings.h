#pragma once
#include <stdint.h>
namespace PowerSettings {
constexpr uint16_t seconds[] = {30,60,90,120};
constexpr uint16_t minutes[] = {1,5,10,30,60,0};
inline uint16_t validate(unsigned field, uint16_t value) {
  const auto *options=field==3?minutes:seconds;
  unsigned count=field==3?6:4;
  for(unsigned i=0;i<count;++i) if(options[i]==value)return value;
  return field==3?30:60;
}
}
