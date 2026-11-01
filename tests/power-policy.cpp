#include <cassert>
#include "../firmware/libraries/HudBuddyPower/src/HudBuddyPower.h"
#include "../firmware/hud/PowerSettings.h"
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
  // All supported work/saver durations; BOOT off always gets its own grace.
  for(auto work:PowerSettings::seconds)for(auto saver:PowerSettings::seconds) {
    p.begin(100,(work+saver)*1000,false);
    assert(p.update(100+(work+saver)*1000-1,false,false)==A::None);
    uint32_t off=100+(work+saver)*1000;
    assert(p.update(off,false,false)==A::Sleep);
    for(auto grace:PowerSettings::seconds) {
      assert(!p.deepSleepDue(off+grace*1000-1,grace*1000,false));
      assert(p.deepSleepDue(off+grace*1000,grace*1000,false));
      assert(!p.deepSleepDue(off+grace*1000,grace*1000,true));
    }
    assert(p.cycleDisplay(off+500,work*1000)==A::Wake);
    assert(!p.deepSleepDue(off+999999,30000,false));
    assert(p.cycleDisplay(off+600,work*1000)==A::None);
    assert(p.idleFor(off+600)==work*1000);
    assert(p.cycleDisplay(off+700,work*1000)==A::Sleep);
    assert(!p.deepSleepDue(off+700+59999,60000,false));
    assert(p.deepSleepDue(off+700+60000,60000,false));
  }
  p.begin(0,120000,false);p.startOff(0xfffffff0u);
  assert(!p.deepSleepDue(uint32_t(0xfffffff0u+59999),60000,false));
  assert(p.deepSleepDue(uint32_t(0xfffffff0u+60000),60000,false));
  // USB/touch/USER cannot extend off grace or wake formal firmware.
  p.startOff(0);p.update(4000,true,true);p.update(9000,true,true);
  assert(p.deepSleepDue(60000,60000,false));
  assert(!p.deepSleepDue(60000,60000,true));
  assert(p.cycleDisplay(60001)==A::Wake);
  p.update(60002,false,false);p.update(60032,false,false);
  p.setIdleMs(60000);assert(p.update(120002,false,false)==A::Sleep);
  for(unsigned field=0;field<4;++field) {
    assert(PowerSettings::validate(field,17)==(field==3?30:60));
    for(auto v:PowerSettings::seconds)if(field<3)assert(PowerSettings::validate(field,v)==v);
  }
  for(auto v:PowerSettings::minutes)assert(PowerSettings::validate(3,v)==v);
  assert(PowerSettings::validate(0,0)==60);
}
