#include <cassert>
#include "../firmware/hud/ShortPressButton.h"
int main(){
 ShortPressButton b;b.begin(0,false);
 assert(!b.update(100,true));assert(b.settling());
 assert(!b.update(130,true));assert(!b.settling());
 assert(!b.update(200,false));assert(b.update(230,false));
 assert(!b.update(260,false));
 // Bounce never becomes a press.
 b.begin(0,false);b.update(10,true);b.update(20,false);
 assert(!b.update(60,false));
 // Long hold and a button held at boot are not clicks.
 b.begin(0,false);b.update(100,true);b.update(130,true);
 b.update(2000,false);assert(!b.update(2030,false));
 b.begin(0,true);b.update(100,false);assert(!b.update(130,false));
 // Rollover.
 b.begin(0xffffff00u,false);b.update(0xfffffff0u,true);b.update(20,true);
 b.update(100,false);assert(b.update(130,false));
}
