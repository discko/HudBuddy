#include <cassert>
#include <string>
#include "../firmware/hud/TaskNotifications.h"
#include "../firmware/libraries/HudBuddyPower/src/HudBuddyPower.h"
struct Task {std::string id;bool waitingForInput;};
int main() {
 TaskNotifications<std::string> n;
 std::vector<Task> tasks={{"a",false},{"b",true}};
 auto r=n.apply(tasks,{},100,true);assert(!r.wake&&!r.chime); // Cache is a baseline, not an event.
 r=n.apply(tasks,{},200);assert(!r.wake&&!r.chime);
 tasks[0].waitingForInput=true;
 r=n.apply(tasks,{"a"},300);assert(r.wake&&r.chime&&r.target=="a");
 assert(n.red("a",60299));assert(!n.red("a",60300));
 r=n.apply(tasks,{},400);assert(!r.wake&&!r.chime); // Repeated push doesn't extend red or ring.
 assert(n.red("a",60299));assert(n.expire(60300));assert(!n.expire(60301));
 r=n.apply(tasks,{"b"},61000);assert(r.wake&&!r.chime&&r.target=="b");
 r=n.apply(tasks,{"b"},62000);assert(n.red("b",121999));assert(!n.red("b",122000));
 tasks[0].waitingForInput=false;
 r=n.apply(tasks,{"a"},63000);assert(r.wake&&!r.chime);
 tasks[0].waitingForInput=true;
 r=n.apply(tasks,{},64000);assert(r.wake&&r.chime);
 tasks.clear();r=n.apply(tasks,{"a","b"},65000);assert(r.wake&&!r.chime);
 tasks={{"c",true},{"d",true}};
 r=n.apply(tasks,{},66000);assert(r.wake&&r.chime&&r.target=="c"); // One chime for whole batch.
 r=n.apply(tasks,{"d"},0xfffffff0u);assert(n.red("d",10));
 assert(!n.red("d",uint32_t(0xfffffff0u+60000u)));
 HudBuddyPower power;power.begin(0,120000,false);power.startOff(100);
 assert(power.notify(200)==HudBuddyPower::Action::Wake);
 assert(power.awake()&&power.idleFor(200)==0);
 assert(power.update(120199,false,false)==HudBuddyPower::Action::None);
 assert(power.update(120200,false,false)==HudBuddyPower::Action::Sleep);
 assert(!power.deepSleepDue(180199,60000,false));
 assert(power.deepSleepDue(180200,60000,false));
 power.notify(190000);power.cycleDisplay(190001); // Saver, then explicit task returns to work.
 assert(power.notify(190002)==HudBuddyPower::Action::None&&power.idleFor(190002)==0);
}
