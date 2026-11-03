#pragma once
#include <stdint.h>
#include <math.h>
namespace ChimePcm {
constexpr int rate=16000, lead=2400, spacing=4800, tone=3840, tail=448;
constexpr int frames=lead+spacing+tone+tail;
constexpr int amplitude=12000;
inline int16_t sample(int i) {
  float value=0;
  for(int chime=0;chime<2;++chime) {
    float t=(i-lead-chime*spacing)/float(rate);
    if(t>=0 && t<0.24f) {
      float attack=fminf(t/0.006f,1.0f),fade=fminf((0.24f-t)/0.025f,1.0f);
      value+=amplitude*attack*fade*expf(-15*t)*
        (0.8f*sinf(2*3.14159265358979323846f*1318.51f*t)+0.2f*sinf(2*3.14159265358979323846f*2637.02f*t));
    }
  }
  return int16_t(fmaxf(-32768.0f,fminf(32767.0f,value)));
}
}
