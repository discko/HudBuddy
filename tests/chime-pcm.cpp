#include <cassert>
#include <cstdlib>
#include "../firmware/hud/ChimePcm.h"
int main() {
 using namespace ChimePcm;
 assert(amplitude==12000 && lead==rate*150/1000 && spacing==rate*300/1000);
 assert(frames==11488);
 int peak=0;
 for(int i=0;i<frames;++i){int v=sample(i);assert(abs(v)<=amplitude);if(abs(v)>peak)peak=abs(v);}
 assert(peak>0);
 for(int i=0;i<lead;++i)assert(sample(i)==0);
 for(int i=0;i<tone;++i)assert(sample(lead+i)==sample(lead+spacing+i));
 for(int i=lead+tone;i<lead+spacing;++i)assert(sample(i)==0);
 for(int i=lead+spacing+tone;i<frames;++i)assert(sample(i)==0);
}
