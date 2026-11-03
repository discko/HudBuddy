#pragma once
#include <Arduino.h>
#include <lvgl.h>
#include <cJSON.h>
#include <vector>

namespace QuotaPage {
struct Window {bool present=false;double remaining=-1;int64_t resetsAt=-1;};
struct Data {
  bool available=false,stale=false,cardsKnown=false;
  String account,plan;
  int64_t readAt=-1,lastSuccess=-1,firstAttempt=-1;
  Window weekly,fiveHour;
  unsigned cardCount=0;
  std::vector<int64_t> expiries;
};
bool parse(cJSON *raw,Data &out);
void set(Data data);
bool isDetail();
void closeDetail();
void connectionTime(int64_t lastSuccess,int64_t firstAttempt);
void begin(lv_obj_t *parent,bool (*allowsClick)());
void update(int64_t now,int timezoneMinutes);
}
