#include <cassert>
#include <cstring>
#include "../firmware/hud/QuotaPolicy.h"
using namespace QuotaPolicy;
int main() {
 const int64_t now=1793436858000LL;
 assert(urgency(now+12*hour,now)==Urgency::Critical);
 assert(urgency(now+12*hour+1,now)==Urgency::Urgent);
 assert(urgency(now+24*hour,now)==Urgency::Urgent);
 assert(urgency(now+72*hour,now)==Urgency::Soon);
 assert(urgency(now+72*hour+1,now)==Urgency::Normal);
 assert(urgency(now,now)==Urgency::Expired);
 assert(urgency(-1,now)==Urgency::Unknown);
 char s[100];deadline(s,sizeof(s),now+48*hour-1000,now,480);
 assert(strcmp(s,"还剩 1天 23:59:59")==0);
 deadline(s,sizeof(s),now+48*hour,now,480);assert(strstr(s,"还剩")==nullptr);
 deadline(s,sizeof(s),now+12*hour,now,480);assert(strcmp(s,"还剩 12:00:00")==0);
 deadline(s,sizeof(s),now,now,480);assert(strcmp(s,"已到期")==0);
 deadline(s,sizeof(s),0,1,480,true);assert(strcmp(s,"1970-01-01 08:00:00")==0);
 assert(!offlineWarning(now,now-24*hour,-1));
 assert(offlineWarning(now,now-24*hour-1,-1));
 assert(offlineWarning(now,-1,now-25*hour));
 assert(!offlineWarning(now,-1,-1));
 assert(!offlineWarning(now,now+1000,-1));
}
