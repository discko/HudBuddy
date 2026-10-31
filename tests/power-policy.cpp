#include <cassert>
#include "../firmware/libraries/HudBuddyPower/src/HudBuddyPower.h"
using A = HudBuddyPower::Action;
int main() {
  HudBuddyPower p; p.begin(0);
  assert(p.update(59999,false,false)==A::None);
  assert(p.update(60000,false,false)==A::Sleep);
  assert(p.update(61000,false,true)==A::None); // touch must not wake
  assert(p.update(62000,true,false)==A::None);
  assert(p.update(62030,true,false)==A::None);
  assert(p.update(65029,true,false)==A::None);
  assert(p.update(65030,true,false)==A::Wake);
  p.update(65040,false,false); p.update(65070,false,false);
  assert(p.update(125039,false,false)==A::None);
  assert(p.update(125040,false,false)==A::Sleep);
  // Brief press / bounce does not wake; a fresh long press does.
  p.update(126000,true,false); p.update(126010,false,false);
  assert(p.update(130000,false,false)==A::None);
  p.begin(0); p.update(59000,false,true);
  assert(p.update(60000,false,false)==A::None);
  assert(p.update(119000,false,false)==A::Sleep);
  // Pressing and holding while awake is ongoing activity.
  p.begin(0); p.update(50000,true,false); p.update(50030,true,false);
  assert(p.update(200000,true,false)==A::None);
  // millis rollover must not skip the idle deadline.
  p.begin(0xfffffff0u);
  assert(p.update(uint32_t(0xfffffff0u+59999u),false,false)==A::None);
  assert(p.update(uint32_t(0xfffffff0u+60000u),false,false)==A::Sleep);
  // Formal HUD: 60 s UI + 60 s screensaver. Animation/USB supply no activity.
  p.begin(0,120000);
  assert(p.update(60000,false,false)==A::None);
  assert(p.idleFor(60000)==60000 && p.awake());
  assert(p.update(119999,false,false)==A::None);
  assert(p.update(120000,false,false)==A::Sleep);
  assert(p.update(121000,false,true)==A::None);
  p.update(122000,true,false);p.update(122030,true,false);
  assert(p.update(125030,true,false)==A::Wake);
  assert(p.idleFor(125030)==0);
  p.begin(0,120000);
  p.update(90000,false,true); // Touch during saver restores a full UI interval.
  assert(p.idleFor(90000)==0);
  assert(p.update(150000,false,false)==A::None);
  assert(p.update(210000,false,false)==A::Sleep);
  // Formal BOOT cycle; touch and USER cannot wake the display.
  p.begin(0,120000,false);
  assert(p.cycleDisplay(100)==A::None && p.awake());
  assert(p.idleFor(100)==60000);
  assert(p.cycleDisplay(200)==A::Sleep && !p.awake());
  p.update(300,true,true);p.update(330,true,true);
  assert(p.update(4000,true,true)==A::None && !p.awake());
  assert(p.cycleDisplay(4100)==A::Wake && p.awake());
  assert(p.idleFor(4100)==0);
  p.begin(0,120000,false);p.cycleDisplay(1000);
  assert(p.update(60999,false,false)==A::None);
  assert(p.update(61000,false,false)==A::Sleep);
  assert(p.cycleDisplay(62000)==A::Wake);
  p.begin(0,120000,false);p.cycleDisplay(1000);
  p.update(2000,false,true);assert(p.idleFor(2000)==0);
  assert(p.cycleDisplay(3000)==A::None && p.awake());
  p.begin(0xfffffff0u,120000,false);p.cycleDisplay(0xfffffff8u);
  assert(p.idleFor(0xfffffff8u)==60000);
  assert(p.cycleDisplay(10)==A::Sleep);
  assert(p.cycleDisplay(100)==A::Wake);
}
