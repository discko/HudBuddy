#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <algorithm>
#include "../firmware/libraries/HudBuddyScene/src/DogScene.h"
struct Raster {
 uint16_t pixels[480*480]; int tx=0,ty=0,size=1;uint16_t color=0;
 void fillScreen(uint16_t c){std::fill(pixels,pixels+480*480,c);}
 void fillCircle(int x,int y,int r,uint16_t c){for(int yy=y-r;yy<=y+r;++yy)for(int xx=x-r;xx<=x+r;++xx)if(xx>=0&&xx<480&&yy>=0&&yy<480&&(xx-x)*(xx-x)+(yy-y)*(yy-y)<=r*r)pixels[yy*480+xx]=c;}
 void draw16bitRGBBitmap(int x,int y,const uint16_t *data,int w,int h){for(int yy=0;yy<h;++yy)for(int xx=0;xx<w;++xx)if(x+xx>=0&&x+xx<480&&y+yy>=0&&y+yy<480)pixels[(y+yy)*480+x+xx]=data[yy*w+xx];}
 void setTextWrap(bool){} void setTextColor(uint16_t c){color=c;} void setTextSize(int s){size=s;}void setCursor(int x,int y){tx=x;ty=y;}
 void print(const char *s){fprintf(stderr,"%d|%d|%d|%u|%s\n",tx,ty,size,color,s);}
 void save(const char *p){FILE *f=fopen(p,"wb");fprintf(f,"P6\n480 480\n255\n");for(auto c:pixels){unsigned char b[3]={(unsigned char)(((c>>11)&31)*255/31),(unsigned char)(((c>>5)&63)*255/63),(unsigned char)((c&31)*255/31)};fwrite(b,1,3,f);}fclose(f);}
};
int main(int argc,char **argv){
 for(uint32_t t=0;t<60000;t+=20){auto p=DogScene::pose(t);auto a=DogScene::textPosition(p,t);assert(!DogScene::overlaps(p,a.x,a.y));assert(a.x>=0&&a.x+182<=480&&a.y+70<=480);}
 Raster r;DogScene::render(r,argc>2?atoi(argv[2]):11500);r.save(argv[1]);
}
