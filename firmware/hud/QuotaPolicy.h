#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>

namespace QuotaPolicy {
constexpr int64_t hour=3600000LL;
enum class Urgency { Unknown, Normal, Soon, Urgent, Critical, Expired };
inline Urgency urgency(int64_t expiry,int64_t now) {
  if(expiry<0||now<=0) return Urgency::Unknown;
  int64_t left=expiry-now;
  if(left<=0) return Urgency::Expired;
  if(left<=12*hour) return Urgency::Critical;
  if(left<=24*hour) return Urgency::Urgent;
  if(left<=72*hour) return Urgency::Soon;
  return Urgency::Normal;
}
inline bool offlineWarning(int64_t now,int64_t lastSuccess,int64_t firstAttempt) {
  int64_t baseline=lastSuccess>=0?lastSuccess:firstAttempt;
  return baseline>=0 && now>baseline && now-baseline>24*hour;
}
inline void deadline(char *out,size_t size,int64_t expiry,int64_t now,int timezoneMinutes,bool fullDate=false) {
  if(expiry<0||now<=0) {snprintf(out,size,"时间未知");return;}
  int64_t left=expiry-now;
  if(!fullDate && left<=0) {snprintf(out,size,"已到期");return;}
  if(!fullDate && left<48*hour) {
    int64_t sec=left/1000;
    int days=sec/86400,h=sec%86400/3600,m=sec%3600/60,s=sec%60;
    if(days) snprintf(out,size,"还剩 %d天 %02d:%02d:%02d",days,h,m,s);
    else snprintf(out,size,"还剩 %02d:%02d:%02d",h,m,s);
    return;
  }
  time_t seconds=expiry/1000+timezoneMinutes*60;
  tm t;gmtime_r(&seconds,&t);
  strftime(out,size,fullDate?"%Y-%m-%d %H:%M:%S":"%m/%d %H:%M:%S",&t);
}
}
