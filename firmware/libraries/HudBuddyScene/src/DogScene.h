#pragma once
#include <math.h>
#include <stdint.h>
#include "DogFrames.h"

// Approved six-frame puppy art; motion and text avoidance remain procedural.
namespace DogScene {
constexpr uint16_t kFur = 0xE651;     // warm biscuit
constexpr uint16_t kFar = 0x8BCB;     // legs behind the body
constexpr uint16_t kCollar = 0xDB69;  // coral red
constexpr uint16_t kText = 0x8C71;
struct Pose { float x, y, phase; bool right; };
inline Pose pose(uint32_t ms) {
  const uint32_t segment = ms / 5000;
  const float t = (ms % 5000) / 5000.0f;
  const int row = segment % 6;
  const bool right = (segment % 2) == 0;
  const float travel = fminf(t / .88f, 1.0f);
  float y = -12.0f + row * 101.0f;
  if (t > .88f) {
    float blend = (t - .88f) / .12f;
    blend = blend * blend * (3 - 2 * blend);
    y += (((row + 1) % 6) - row) * 101.0f * blend;
  }
  return {right ? -105 + 690 * travel : 585 - 690 * travel,
          y + 4 * sinf(ms * .019f), ms * .014f, right};
}

struct TextPosition { int x,y,corner; };
inline bool overlaps(Pose p,int x,int y) {
  return p.x+104>x-10 && p.x-104<x+192 && p.y+84>y-10 && p.y-78<y+80;
}
inline TextPosition textPosition(Pose p,uint32_t ms) {
  int preferred=(ms/4000)%4;
  int jitter=(ms/4000)%3*5;
  const int xs[4]={32,260,260,32},ys[4]={40,40,356,356};
  // Retain position until the next 4 s slot, unless the puppy approaches it.
  for(int i=0;i<4;++i) {
    int c=(preferred+i)%4;
    if(!overlaps(p,xs[c]+jitter,ys[c]+jitter))
      return {xs[c]+jitter,ys[c]+jitter,c};
  }
  return {32,40,0}; // A single puppy cannot overlap all four corners.
}

template <class G> void render(G &g,uint32_t ms,const char *subtitle="Ready for development",const char *wakeHint="Hold USER 3s to wake") {
  g.fillScreen(0);
  Pose p=pose(ms);
  const auto &frame=DogFrames::pixels[p.right?0:1][(ms/125)%DogFrames::count];
  // Arduino_Canvas takes a mutable pointer but only reads the bitmap.
  g.draw16bitRGBBitmap(int(p.x)-100,int(p.y)-85,const_cast<uint16_t*>(frame),DogFrames::width,DogFrames::height);
  auto text=textPosition(p,ms);
  g.setTextWrap(false);
  g.setTextColor(kFur);g.setTextSize(2);g.setCursor(text.x,text.y);g.print("HudBuddy");
  g.setTextColor(kText);g.setTextSize(1);
  g.setCursor(text.x,text.y+25);g.print(subtitle);
  g.setCursor(text.x,text.y+42);g.print("Screen off after 1 min");
  g.setCursor(text.x,text.y+56);g.print(wakeHint);
}
} // namespace DogScene
