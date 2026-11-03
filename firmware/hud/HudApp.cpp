#include "HudApp.h"
#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#include <TouchDrvCSTXXX.hpp>
#include <XPowersLib.h>
#include <HudBuddyPower.h>
#include <cJSON.h>
#include <esp_timer.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <sys/time.h>
#include <LittleFS.h>
#include "FirmwareVersion.h"
#include <vector>
#include <time.h>
#include <cmath>
#include "HudFonts.h"
#include "QuotaPage.h"
#include "SettingsPage.h"
#include "TapGuard.h"
#include "ShortPressButton.h"
#include "TaskNotifications.h"
#include "NotificationSound.h"
#include <DogScene.h>

namespace {
constexpr size_t MAX_LINE = 102400;
constexpr uint32_t TEXT = 0xe8eaed, MUTED = 0x8d96a4;
Arduino_ESP32QSPI bus(12,38,4,5,6,7);
Arduino_CO5300 display(&bus,39,0,480,480,0,0,0,0);
Arduino_Canvas saverFrame(480,480,&display);
TouchDrvCST92xx touch;
XPowersAXP2101 pmu;
HudBuddyPower powerPolicy;
bool pmuReady=false, touchReady=false, received=false, dirty=true, hudOkay=true;
bool haveTasks=false;
bool pendingHello=true;
bool displayReady=false,cacheReady=false,restoringCache=false,cacheChanged=false;
cJSON *cachedSnapshot=nullptr;
RTC_DATA_ATTR uint32_t sleepMarker=0;
RTC_DATA_ATTR uint8_t savedPage=0;
constexpr uint32_t SLEEP_MARKER=0x48424431;
uint64_t lastCommunicationUnix=0;
bool bootWakeConsumed=false;
volatile bool touchPending=false,bootPending=false;
ShortPressButton bootButton;
bool touchPressed=false, touchActivity=false;
TapGuard tapGuard;
bool screensaver=false, suppressTouchUntilRelease=false;
uint32_t saverStarted=0, saverFrameAt=0;
int16_t touchX=0,touchY=0;
uint32_t lastTouch=0;
uint64_t clockBase=0, clockAt=0;
int timezoneMinutes=480;
char *lineBuffer=nullptr;
size_t lineLength=0;
bool droppingLine=false;
struct Task { String id,title,status,summary; int64_t updated; bool waitingForInput=false; };
TaskNotifications<String> notifications;
bool taskAlertPending=false,chimePending=false;
String alertTarget;
struct RowAlert {lv_obj_t *dot;String id;};
std::vector<RowAlert> rowAlerts;
std::vector<Task> tasks;
String selectedId;
int savedListY=0,requestedListY=-1;
lv_obj_t *pages, *hudTile, *quotaTile, *settingsTile, *listPanel, *detailPanel, *detailScroll;
lv_obj_t *timeLabel, *usbLabel, *batteryLabel, *freshDot, *pageDot1,*pageDot2,*pageDot3;
lv_disp_draw_buf_t drawBuffer;
lv_color_t *pixels;

uint64_t nowMs() { return esp_timer_get_time()/1000ULL; }
void IRAM_ATTR onBoot() {bootPending=true;}
void IRAM_ATTR onTouch() { touchPending=true; }
void report(const char *event) {
  if(Serial) Serial.printf("{\"type\":\"telemetry\",\"event\":\"%s\",\"awake\":%s,\"tasks\":%u,\"heap\":%u,\"uptime_ms\":%lu}\n",event,powerPolicy.awake()?"true":"false",unsigned(tasks.size()),ESP.getFreeHeap(),millis());
}
void hello() { if(Serial) Serial.println("{\"type\":\"hello\",\"protocol\":1,\"device\":\"HudBuddy\",\"firmware\":\"" HUDBUDDY_VERSION "\"}"); }
lv_obj_t *box(lv_obj_t *parent,int x,int y,int w,int h) {
  auto o=lv_obj_create(parent); lv_obj_remove_style_all(o);
  lv_obj_set_pos(o,x,y); lv_obj_set_size(o,w,h);
  lv_obj_clear_flag(o,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
  return o;
}
lv_obj_t *label(lv_obj_t *parent,const char *text,int x,int y,int w,const lv_font_t *font=&hud_font_24,uint32_t color=TEXT) {
  auto o=lv_label_create(parent); lv_label_set_text(o,text); lv_obj_set_pos(o,x,y);
  if(w>0) lv_obj_set_width(o,w);
  lv_obj_set_style_text_font(o,font,0); lv_obj_set_style_text_color(o,lv_color_hex(color),0);
  lv_obj_set_style_text_line_space(o,6,0);
  return o;
}
void badge(lv_obj_t *parent,const Task &task,int x,int y) {
  bool doing=task.status=="doing";
  auto b=box(parent,x,y,100,36);
  lv_obj_set_style_bg_color(b,lv_color_hex(doing?0x2c3c52:0x443a27),0);
  lv_obj_set_style_bg_opa(b,LV_OPA_COVER,0); lv_obj_set_style_radius(b,8,0);
  auto t=label(b,doing?"进行中":"等待中",0,0,0,&hud_font_24,doing?0xa5c4ee:0xe2c48b);
  lv_obj_center(t);
}
void renderTasks();
void openTask(lv_event_t *e) {
  if(!tapGuard.allowsClick()) return;
  size_t i=reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
  if(i>=tasks.size()) return;
  selectedId=tasks[i].id; renderTasks(); report("detail");
}
void back(lv_event_t *) { if(!tapGuard.allowsClick()) return; selectedId=""; renderTasks(); report("list"); }
void scrollStyle(lv_obj_t *o) {
  lv_obj_add_flag(o,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_scroll_dir(o,LV_DIR_VER);
  lv_obj_set_scrollbar_mode(o,LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_style_bg_color(o,lv_color_hex(0x39404c),LV_PART_SCROLLBAR);
  lv_obj_set_style_width(o,3,LV_PART_SCROLLBAR);
  // Allow a horizontal drag to propagate to the enclosing tileview.
  lv_obj_add_flag(o,LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
}
void renderTasks() {
  dirty=false;
  if(!lv_obj_has_flag(listPanel,LV_OBJ_FLAG_HIDDEN)) savedListY=lv_obj_get_scroll_y(listPanel);
  if(requestedListY>=0){savedListY=requestedListY;requestedListY=-1;}
  int detailY=detailScroll?lv_obj_get_scroll_y(detailScroll):0;
  rowAlerts.clear();
  lv_obj_clean(listPanel); lv_obj_clean(detailPanel); detailScroll=nullptr;
  const Task *selected=nullptr;
  for(const auto &t:tasks) if(t.id==selectedId) selected=&t;
  if(!selected) selectedId="";
  if(selectedId.length()) {
    lv_obj_add_flag(listPanel,LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(detailPanel,LV_OBJ_FLAG_HIDDEN);
    auto backButton=box(detailPanel,26,2,124,46);
    lv_obj_add_flag(backButton,LV_OBJ_FLAG_CLICKABLE);
    label(backButton,"‹ 返回",8,4,0,&hud_font_24,MUTED);
    lv_obj_add_event_cb(backButton,back,LV_EVENT_CLICKED,nullptr);
    detailScroll=box(detailPanel,26,52,428,332); scrollStyle(detailScroll);
    lv_obj_clear_flag(detailScroll,LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
    auto title=label(detailScroll,selected->title.c_str(),8,0,394,&hud_font_28);
    lv_obj_update_layout(title);
    int y=lv_obj_get_height(title)+14;
    badge(detailScroll,*selected,8,y); y+=54;
    auto summary=label(detailScroll,selected->summary.length()?selected->summary.c_str():"暂无任务说明",8,y,394,&hud_font_24);
    lv_obj_update_layout(summary); y+=lv_obj_get_height(summary)+24;
    label(detailScroll,"上次更新",8,y,394,&hud_font_24,MUTED); y+=38;
    time_t updatedSeconds=selected->updated/1000+timezoneMinutes*60;
    tm updatedTm; gmtime_r(&updatedSeconds,&updatedTm); char updated[32];
    strftime(updated,sizeof(updated),"%Y-%m-%d  %H:%M:%S",&updatedTm);
    label(detailScroll,updated,8,y,394,&lv_font_montserrat_20,MUTED);
    box(detailScroll,8,y+40,1,18);
    lv_obj_update_layout(detailScroll); lv_obj_scroll_to_y(detailScroll,detailY,LV_ANIM_OFF);
  } else {
    lv_obj_clear_flag(listPanel,LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(detailPanel,LV_OBJ_FLAG_HIDDEN);
    String count="HUD  /  "+String(tasks.size())+" 个任务";
    label(listPanel,count.c_str(),8,0,390,&hud_font_24,MUTED);
    if(tasks.empty()) {
      label(listPanel,!hudOkay?"HUD 暂不可用":haveTasks?"暂无进行中或等待中的任务":"等待电脑连接",8,118,390,&hud_font_28);
      label(listPanel,!hudOkay?"连接恢复后自动更新":haveTasks?"任务更新后会自动出现在这里":"请启动 HudBuddy Bridge",8,170,390,&hud_font_24,MUTED);
    }
    for(size_t i=0;i<tasks.size();++i) {
      auto row=box(listPanel,0,48+i*108,414,108);
      lv_obj_add_flag(row,LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(row,openTask,LV_EVENT_CLICKED,reinterpret_cast<void*>(i));
      auto t=label(row,tasks[i].title.c_str(),8,8,394,&hud_font_28);
      lv_label_set_long_mode(t,LV_LABEL_LONG_SCROLL_CIRCULAR);
      lv_obj_set_style_anim_speed(t,32,0);
      badge(row,tasks[i],8,53);
      auto red=box(row,118,66,10,10);lv_obj_set_style_radius(red,5,0);
      lv_obj_set_style_bg_color(red,lv_color_hex(0xef7379),0);lv_obj_set_style_bg_opa(red,LV_OPA_COVER,0);
      if(!notifications.red(tasks[i].id,millis()))lv_obj_add_flag(red,LV_OBJ_FLAG_HIDDEN);
      rowAlerts.push_back({red,tasks[i].id});
      if(tasks[i].waitingForInput) {
        lv_obj_set_style_border_color(row,lv_color_hex(0xe8c86a),0);lv_obj_set_style_border_width(row,2,0);lv_obj_set_style_radius(row,10,0);
        auto tag=box(row,306,53,100,36);lv_obj_set_style_bg_color(tag,lv_color_hex(0x443a27),0);lv_obj_set_style_bg_opa(tag,LV_OPA_COVER,0);lv_obj_set_style_radius(tag,8,0);
        auto caption=label(tag,"待输入",0,0,0,&hud_font_24,0xe8c86a);lv_obj_center(caption);
        lv_anim_t pulse;lv_anim_init(&pulse);lv_anim_set_var(&pulse,row);
        lv_anim_set_exec_cb(&pulse,[](void *o,int32_t opacity){lv_obj_set_style_border_opa((lv_obj_t*)o,opacity,0);});
        lv_anim_set_values(&pulse,100,255);lv_anim_set_time(&pulse,1100);lv_anim_set_playback_time(&pulse,1100);lv_anim_set_repeat_count(&pulse,LV_ANIM_REPEAT_INFINITE);lv_anim_start(&pulse);
      }
      auto line=box(row,8,106,394,1); lv_obj_set_style_bg_color(line,lv_color_hex(0x202329),0); lv_obj_set_style_bg_opa(line,LV_OPA_COVER,0);
    }
    lv_obj_update_layout(listPanel); lv_obj_scroll_to_y(listPanel,savedListY,LV_ANIM_OFF);
  }
}
void buildUi() {
  auto screen=lv_scr_act(); lv_obj_set_style_bg_color(screen,lv_color_black(),0);
  lv_obj_clear_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
  pages=lv_tileview_create(screen); lv_obj_remove_style_all(pages);
  lv_obj_set_pos(pages,0,60); lv_obj_set_size(pages,480,390);
  lv_obj_set_scrollbar_mode(pages,LV_SCROLLBAR_MODE_OFF);
  hudTile=lv_tileview_add_tile(pages,0,0,LV_DIR_RIGHT);
  quotaTile=lv_tileview_add_tile(pages,1,0,LV_DIR_HOR);
  settingsTile=lv_tileview_add_tile(pages,2,0,LV_DIR_LEFT);
  listPanel=box(hudTile,26,0,428,390); scrollStyle(listPanel);
  detailPanel=box(hudTile,0,0,480,390);
  QuotaPage::begin(quotaTile,[](){return tapGuard.allowsClick();});
  SettingsPage::begin(settingsTile,[](){return tapGuard.allowsClick();},[](int percent){display.setBrightness((percent*255+50)/100);});
  auto header=box(screen,0,0,480,60);
  timeLabel=label(header,"--:--",36,20,90,&lv_font_montserrat_24);
  usbLabel=label(header,"USB —",146,22,132,&lv_font_montserrat_20,MUTED);
  freshDot=box(header,294,27,10,10); lv_obj_set_style_radius(freshDot,5,0); lv_obj_set_style_bg_opa(freshDot,LV_OPA_COVER,0);
  batteryLabel=label(header,"--%",328,22,120,&lv_font_montserrat_20,MUTED);
  pageDot1=box(screen,214,462,10,5); pageDot2=box(screen,235,462,10,5); pageDot3=box(screen,256,462,10,5);
  for(auto dot:{pageDot1,pageDot2,pageDot3}) {lv_obj_set_style_radius(dot,3,0);lv_obj_set_style_bg_opa(dot,LV_OPA_COVER,0);}
  renderTasks();
}
const char *str(cJSON *o,const char *name) {
  cJSON *v=cJSON_GetObjectItemCaseSensitive(o,name); return cJSON_IsString(v)?v->valuestring:nullptr;
}
bool number(cJSON *o,const char *name,double &out) {
  cJSON *v=cJSON_GetObjectItemCaseSensitive(o,name); if(!cJSON_IsNumber(v)||!std::isfinite(v->valuedouble)) return false; out=v->valuedouble; return true;
}
bool optionalTime(cJSON *o,const char *name,int64_t &out) {
  auto v=cJSON_GetObjectItemCaseSensitive(o,name);
  if(!v||cJSON_IsNull(v)){out=-1;return true;}
  double n; if(!number(o,name,n)||n<0||n>253402300799999.0)return false;
  out=int64_t(n);return true;
}
void receiveLine() {
  lineBuffer[lineLength]=0;
  const char *end=nullptr;
  cJSON *root=cJSON_ParseWithLengthOpts(lineBuffer,lineLength+1,&end,true);
  if(!root) {report("invalid_json");return;}
  double protocol=0,unixMs=0,tz=0,seq=0;
  const char *type=str(root,"type");
  bool valid=type && number(root,"protocol",protocol) && protocol==1 && number(root,"unix_ms",unixMs) && unixMs>=0 && number(root,"tz_offset_min",tz) && tz>=-840 && tz<=840;
  bool snapshot=type && strcmp(type,"snapshot")==0;
  bool heartbeat=type && strcmp(type,"heartbeat")==0;
  valid=valid && (snapshot||heartbeat);
  std::vector<Task> next;
  std::vector<String> changedIds;
  QuotaPage::Data nextQuota;
  bool hasTasks=cJSON_HasObjectItem(root,"tasks"),hasQuota=cJSON_HasObjectItem(root,"quota");
  int64_t magpieSuccess=-1,magpieStarted=-1;
  if(valid && heartbeat)valid=optionalTime(root,"magpie_last_success_ms",magpieSuccess)&&optionalTime(root,"magpie_monitor_started_ms",magpieStarted);
  if(valid && snapshot) {
    auto array=cJSON_GetObjectItemCaseSensitive(root,"tasks");
    valid=number(root,"seq",seq) && seq>=0 && seq<=9007199254740991.0 && (hasTasks||hasQuota);
    if(valid && hasTasks)valid=cJSON_IsArray(array) && cJSON_GetArraySize(array)<=64;
    auto changed=cJSON_GetObjectItemCaseSensitive(root,"changed_task_ids");
    if(valid && changed) {
      valid=hasTasks && cJSON_IsArray(changed) && cJSON_GetArraySize(changed)<=128;
      if(valid) {cJSON *id;cJSON_ArrayForEach(id,changed){
        if(!cJSON_IsString(id)||!id->valuestring[0]){valid=false;break;}
        changedIds.push_back(id->valuestring);
      }}
    }
    if(valid && hasTasks) {
      next.reserve(cJSON_GetArraySize(array));
      cJSON *item;
      cJSON_ArrayForEach(item,array) {
        const char *id=str(item,"id"),*title=str(item,"title"),*status=str(item,"status"),*summary=str(item,"summary");
        double updated=0;
        if(!id||!title||!status||!summary||!number(item,"updated_at",updated)||updated<0||!id[0]||(strcmp(status,"doing") && strcmp(status,"waiting"))) {valid=false;break;}
        for(const auto &t:next) if(t.id==id) valid=false;
        if(!valid) break;
        auto wfi=cJSON_GetObjectItemCaseSensitive(item,"waiting_for_input");
        if(wfi && !cJSON_IsBool(wfi)){valid=false;break;}
        next.push_back({id,title,status,summary,int64_t(updated),cJSON_IsTrue(wfi)});
      }
    }
    if(valid && hasQuota)valid=QuotaPage::parse(cJSON_GetObjectItemCaseSensitive(root,"quota"),nextQuota);
  }
  if(valid) {
    received=true; clockAt=nowMs();
    clockBase=uint64_t(unixMs); timezoneMinutes=int(tz);
    if(!restoringCache) {
      lastCommunicationUnix=uint64_t(unixMs);
      timeval tv={time_t(unixMs/1000),suseconds_t(uint64_t(unixMs)%1000*1000)};settimeofday(&tv,nullptr);
      if(!cachedSnapshot)cachedSnapshot=cJSON_CreateObject();
      for(const char *key:{"tasks","quota","hud_ok","magpie_last_success_ms","magpie_monitor_started_ms"}) {
        auto v=cJSON_GetObjectItemCaseSensitive(root,key);
        if(v) {cJSON_DeleteItemFromObjectCaseSensitive(cachedSnapshot,key);cJSON_AddItemToObject(cachedSnapshot,key,cJSON_Duplicate(v,true));}
      }
      if(hasQuota) {
        cJSON_DeleteItemFromObjectCaseSensitive(cachedSnapshot,"magpie_last_success_ms");
        cJSON_DeleteItemFromObjectCaseSensitive(cachedSnapshot,"magpie_monitor_started_ms");
      }
      for(const char *key:{"protocol","seq","unix_ms","tz_offset_min"})cJSON_DeleteItemFromObjectCaseSensitive(cachedSnapshot,key);
      cJSON_DeleteItemFromObjectCaseSensitive(cachedSnapshot,"type");
      cJSON_AddStringToObject(cachedSnapshot,"type","snapshot");
      cJSON_AddNumberToObject(cachedSnapshot,"protocol",1);cJSON_AddNumberToObject(cachedSnapshot,"seq",0);
      cJSON_AddNumberToObject(cachedSnapshot,"unix_ms",unixMs);cJSON_AddNumberToObject(cachedSnapshot,"tz_offset_min",tz);
      cacheChanged=true;
    }
    auto health=cJSON_GetObjectItemCaseSensitive(root,"hud_ok");
    if(cJSON_IsBool(health)) {bool ok=cJSON_IsTrue(health);if(ok!=hudOkay)dirty=true;hudOkay=ok;}
    if(heartbeat)QuotaPage::connectionTime(magpieSuccess,magpieStarted);
    if(snapshot) {
      // A host push completes the boot handshake even before CDC is ready.
      pendingHello=false;
      if(hasTasks){
        auto alert=notifications.apply(next,changedIds,millis(),restoringCache);
        if(alert.wake){taskAlertPending=true;if(alert.target.length())alertTarget=alert.target;}
        if(alert.chime)chimePending=true;
        tasks.swap(next);haveTasks=true;dirty=true;
      }
      if(hasQuota){if(restoringCache)nextQuota.stale=true;QuotaPage::set(std::move(nextQuota));}
      if(!restoringCache) {Serial.printf("{\"type\":\"ack\",\"seq\":%.0f}\n",seq);report("snapshot");}
    }
  }
  cJSON_Delete(root);
}
void readUsb() {
  unsigned budget=8192;
  while(budget-- && Serial.available()) {
    char c=Serial.read();
    if(c=='\n') {
      if(!droppingLine && lineLength) receiveLine();
      lineLength=0; droppingLine=false;
    } else if(!droppingLine) {
      if(lineLength>=MAX_LINE-1) {droppingLine=true;lineLength=0;}
      else lineBuffer[lineLength++]=c;
    }
  }
}
void updateHeader() {
  if(received) {
    time_t seconds=(clockBase+nowMs()-clockAt)/1000+timezoneMinutes*60;
    tm t; gmtime_r(&seconds,&t); char text[8]; strftime(text,sizeof(text),"%H:%M",&t); lv_label_set_text(timeLabel,text);
  }
  bool usb=Serial.isPlugged();
  lv_label_set_text(usbLabel,usb?"USB  ON":"USB  OFF");
  lv_obj_set_style_text_color(usbLabel,lv_color_hex(usb?TEXT:MUTED),0);
  uint64_t currentUnix=clockBase+nowMs()-clockAt;
  uint64_t age=received && currentUnix>=lastCommunicationUnix?currentUnix-lastCommunicationUnix:UINT64_MAX;
  uint32_t color=!received?0x636b78:age<60000?0x5ba7ff:age<600000?0x63c98d:age<3600000?0xe8c86a:0xef7379;
  lv_obj_set_style_bg_color(freshDot,lv_color_hex(color),0);
  if(pmuReady && pmu.isBatteryConnect()) {
    int percent=pmu.getBatteryPercent();
    if(percent>=0 && percent<=100) lv_label_set_text_fmt(batteryLabel,"%s %d%%",pmu.isCharging()?LV_SYMBOL_CHARGE:LV_SYMBOL_BATTERY_FULL,percent);
    else lv_label_set_text(batteryLabel,"--%");
  } else lv_label_set_text(batteryLabel,"--%");
  auto active=lv_tileview_get_tile_act(pages);
  lv_obj_set_style_bg_color(pageDot1,lv_color_hex(active==hudTile?0xc4ccd8:0x3b414b),0);
  lv_obj_set_style_bg_color(pageDot2,lv_color_hex(active==quotaTile?0xc4ccd8:0x3b414b),0);
  lv_obj_set_style_bg_color(pageDot3,lv_color_hex(active==settingsTile?0xc4ccd8:0x3b414b),0);
}
void updateNavigation() {
  // Lock the outer pager itself: even gestures starting in blank detail space
  // must stay on this tile, while child vertical scrollers and sliders work.
  bool locked=selectedId.length()||QuotaPage::isDetail()||SettingsPage::isDetail();
  if(locked) lv_obj_clear_flag(pages,LV_OBJ_FLAG_SCROLLABLE);
  else lv_obj_add_flag(pages,LV_OBJ_FLAG_SCROLLABLE);
}
bool readBootShortPress() {
  noInterrupts();bool pending=bootPending;bootPending=false;interrupts();
  if(bootWakeConsumed) {
    if(digitalRead(0)==HIGH) {bootWakeConsumed=false;bootButton.begin(millis(),false);}
    return false;
  }
  if(!pending && !bootButton.settling())return false;
  return bootButton.update(millis(),digitalRead(0)==LOW);
}
void pollTouch() {
  if(!touchReady) return;
  noInterrupts(); bool pending=touchPending; touchPending=false; interrupts();
  if(pending) {
    int16_t x[5],y[5];
    touchPressed=touch.getPoint(x,y,5)>0;
    if(touchPressed) {touchX=constrain(x[0],0,479);touchY=constrain(y[0],0,479);lastTouch=millis();touchActivity=true;}
  }
  // Recover from a missed release IRQ; a stationary held touch emits frames.
  if(touchPressed && uint32_t(millis()-lastTouch)>250) touchPressed=false;
  tapGuard.update(touchPressed,touchX,touchY);
}
void input(lv_indev_drv_t *,lv_indev_data_t *data) {
  data->state=powerPolicy.awake() && !screensaver && !suppressTouchUntilRelease && touchPressed?LV_INDEV_STATE_PR:LV_INDEV_STATE_REL;
  data->point.x=touchX;data->point.y=touchY;
}
void flush(lv_disp_drv_t *driver,const lv_area_t *a,lv_color_t *p) {
  if(powerPolicy.awake() && !screensaver) display.draw16bitRGBBitmap(a->x1,a->y1,(uint16_t*)p,a->x2-a->x1+1,a->y2-a->y1+1);
  lv_disp_flush_ready(driver);
}
void roundArea(lv_disp_drv_t *,lv_area_t *a) {a->x1&=~1;a->y1&=~1;a->x2|=1;a->y2|=1;}
void restoreCache() {
  cacheReady=LittleFS.begin(true);
  if(!cacheReady)return;
  auto file=LittleFS.open("/snapshot.json","r");
  if(!file || file.size()>=MAX_LINE*2)return;
  lineLength=file.readBytes(lineBuffer,file.size());file.close();lineBuffer[lineLength]=0;
  cachedSnapshot=cJSON_Parse(lineBuffer);
  restoringCache=true;receiveLine();restoringCache=false;lineLength=0;
  if(received) {
    lastCommunicationUnix=clockBase;
    // ESP RTC system time survives deep sleep; a cold boot has no valid time.
    timeval tv;gettimeofday(&tv,nullptr);
    uint64_t wall=uint64_t(tv.tv_sec)*1000+tv.tv_usec/1000;
    if(sleepMarker==SLEEP_MARKER && wall>=clockBase)clockBase=wall;
    else received=false; // keep cached content, but don't invent current time/freshness
    int64_t success=-1,started=-1;
    optionalTime(cachedSnapshot,"magpie_last_success_ms",success);
    optionalTime(cachedSnapshot,"magpie_monitor_started_ms",started);
    QuotaPage::connectionTime(success,started);
  }
  if(!haveTasks && !cJSON_HasObjectItem(cachedSnapshot,"quota")) {cJSON_Delete(cachedSnapshot);cachedSnapshot=nullptr;}
  pendingHello=true;
}
void saveCache() {
  if(!cacheReady||!cacheChanged||!cachedSnapshot)return;
  char *json=cJSON_PrintUnformatted(cachedSnapshot);if(!json)return;
  auto file=LittleFS.open("/snapshot.tmp","w");size_t length=strlen(json);
  bool ok=file && file.write((const uint8_t*)json,length)==length;
  file.close();free(json);
  if(ok && LittleFS.rename("/snapshot.tmp","/snapshot.json"))cacheChanged=false;
}
void enterDeepSleep(bool fromTimer=false) {
  // Never enter with BOOT already held: ext0 is level triggered.
  if(digitalRead(0)==LOW)return;
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  if(esp_sleep_enable_ext0_wakeup(GPIO_NUM_0,0)!=ESP_OK)return;
  rtc_gpio_pullup_en(GPIO_NUM_0);rtc_gpio_pulldown_dis(GPIO_NUM_0);
  uint32_t interval=SettingsPage::timingMs(3);
  if(interval && esp_sleep_enable_timer_wakeup(uint64_t(interval)*1000)!=ESP_OK)return;
  if(!fromTimer) {
    NotificationSound::stop();
    saveCache();
    auto active=lv_tileview_get_tile_act(pages);savedPage=active==quotaTile?1:active==settingsTile?2:0;
    if(touchReady)touch.sleep();
    report("deep_sleep");
  }
  // Keep PMU charging and hardware PWR shutdown configuration unchanged.
  sleepMarker=SLEEP_MARKER;
  Wire.end();
  esp_deep_sleep_start();
}
void beginDisplay() {
  if(displayReady)return;
  display.begin();bus.writeC8D8(0x36,0xA0);display.setBrightness(0);display.fillScreen(0);
  displayReady=true;
}

}

void hudSetup() {
  NotificationSound::begin();
  auto cause=esp_sleep_get_wakeup_cause();
  bool timerWake=cause==ESP_SLEEP_WAKEUP_TIMER;
  bool buttonWake=cause==ESP_SLEEP_WAKEUP_EXT0;
  rtc_gpio_deinit(GPIO_NUM_0);pinMode(0,INPUT_PULLUP);
  SettingsPage::load();
  Wire.begin(15,14);
  pmuReady=pmu.begin(Wire,AXP2101_SLAVE_ADDRESS,15,14);
  // Minimal timer path: no panel, LVGL, cache or Serial initialization.
  if(timerWake && pmuReady && !pmu.isVbusGood() && digitalRead(0)==HIGH)enterDeepSleep(true);
  bool startDark=timerWake && digitalRead(0)==HIGH;
  Serial.setRxBufferSize(128*1024);Serial.begin(115200);
  pinMode(18,INPUT_PULLUP);
  powerPolicy.begin(millis(),SettingsPage::timingMs(0)+SettingsPage::timingMs(1),false);
  if(startDark)powerPolicy.startOff(millis());
  bootWakeConsumed=buttonWake || digitalRead(0)==LOW;
  bootButton.begin(millis(),digitalRead(0)==LOW);
  attachInterrupt(digitalPinToInterrupt(0),onBoot,CHANGE);
  if(pmuReady) {pmu.enableBattDetection();pmu.enableBattVoltageMeasure();pmu.enableVbusVoltageMeasure();
    pmu.disableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);pmu.clearIrqStatus();
  }
  touch.setPins(40,11);touchReady=touch.begin(Wire,CST92XX_SLAVE_ADDRESS,15,14);
  if(touchReady) {touch.setMaxCoordinates(480,480);touch.setSwapXY(true);touch.setMirrorXY(true,false);pinMode(11,INPUT_PULLUP);attachInterrupt(digitalPinToInterrupt(11),onTouch,FALLING);}
  if(!startDark)beginDisplay();
  lineBuffer=(char*)ps_malloc(MAX_LINE*2);
  pixels=(lv_color_t*)heap_caps_malloc(480*40*sizeof(lv_color_t),MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
  if(!lineBuffer||!pixels||!saverFrame.begin(GFX_SKIP_OUTPUT_BEGIN)) {display.displayOff();while(true) delay(1000);}
  lv_init();hud_fonts_init();
  lv_disp_draw_buf_init(&drawBuffer,pixels,nullptr,480*40);
  static lv_disp_drv_t driver;lv_disp_drv_init(&driver);driver.hor_res=480;driver.ver_res=480;driver.draw_buf=&drawBuffer;driver.flush_cb=flush;driver.rounder_cb=roundArea;lv_disp_drv_register(&driver);
  static lv_indev_drv_t indev;lv_indev_drv_init(&indev);indev.type=LV_INDEV_TYPE_POINTER;indev.read_cb=input;lv_indev_drv_register(&indev);
  restoreCache();buildUi();
  if(sleepMarker==SLEEP_MARKER && savedPage<3)lv_obj_set_tile_id(pages,savedPage,0,LV_ANIM_OFF);
  updateHeader();
  if(!startDark)display.setBrightness((SettingsPage::brightness()*255+50)/100);
  report(startDark?"timer_usb_wake":"boot");
}
void hudLoop() {
  static uint32_t previous=millis(),headerAt=0;
  static bool connected=false;
  uint32_t now=millis();lv_tick_inc(now-previous);previous=now;
  // IOKit on the host pushes on replug. Device hello is boot-only: both CDC
  // readiness and USB SOF detection can settle after a snapshot already arrived.
  bool usb=Serial.isPlugged();
  if(usb!=connected) {lineLength=0;droppingLine=false;}
  connected=usb;
  if(pendingHello && Serial) {hello();pendingHello=false;}
  readUsb();pollTouch();
  // USB parsing timestamps new notifications; expiry/power checks must not use
  // the earlier loop-start tick (unsigned subtraction would expire them).
  now=millis();
  if(!touchPressed) suppressTouchUntilRelease=false;
  powerPolicy.setIdleMs(SettingsPage::timingMs(0)+SettingsPage::timingMs(1));
  auto action=powerPolicy.update(now,digitalRead(18)==LOW,touchActivity);touchActivity=false;
  if(taskAlertPending) {
    taskAlertPending=false;action=powerPolicy.notify(now);
    selectedId="";screensaver=false;SettingsPage::closeDetail();QuotaPage::closeDetail();
    lv_obj_set_tile_id(pages,0,0,LV_ANIM_OFF);requestedListY=0;
    for(size_t i=0;i<tasks.size();++i)if(tasks[i].id==alertTarget){requestedListY=i*108;break;}
    alertTarget="";dirty=true;touchPressed=false;suppressTouchUntilRelease=true;
    lv_obj_invalidate(lv_scr_act());report("task_alert");
    if(chimePending){chimePending=false;NotificationSound::notify();}
  }
  if(readBootShortPress()) {
    auto bootAction=powerPolicy.cycleDisplay(now,SettingsPage::timingMs(0));
    if(bootAction!=HudBuddyPower::Action::None) action=bootAction;
    report("boot_short");
  }
  if(action==HudBuddyPower::Action::Sleep) {display.setBrightness(0);display.displayOff();touchPressed=false;report("sleep");}
  if(action==HudBuddyPower::Action::Wake) {beginDisplay();display.displayOn();lv_obj_invalidate(lv_scr_act());display.setBrightness((SettingsPage::brightness()*255+50)/100);touchPressed=false;report("wake");}
  bool nextSaver=powerPolicy.awake() && powerPolicy.idleFor(now)>=SettingsPage::timingMs(0);
  if(nextSaver!=screensaver) {
    screensaver=nextSaver;
    if(screensaver) {saverStarted=now;saverFrameAt=now-40;report("screensaver");}
    else {suppressTouchUntilRelease=touchPressed;lv_obj_invalidate(lv_scr_act());if(powerPolicy.awake()) report("resume_ui");}
  }
  if(powerPolicy.awake()) {
    if(screensaver) {
      if(uint32_t(now-saverFrameAt)>=40) {
        saverFrameAt=now;DogScene::render(saverFrame,now-saverStarted,"Touch to return to HUD","Press BOOT to wake");saverFrame.flush();
      }
    } else {
      if(dirty) renderTasks();
      if(notifications.expire(now))for(const auto &row:rowAlerts)
        if(!notifications.red(row.id,now))lv_obj_add_flag(row.dot,LV_OBJ_FLAG_HIDDEN);

      QuotaPage::update(received?int64_t(clockBase+nowMs()-clockAt):0,timezoneMinutes);
      if(uint32_t(now-headerAt)>=500) {headerAt=now;updateHeader();}
      updateNavigation();
      lv_timer_handler();
      updateNavigation();
    }
  }
  NotificationSound::update(millis());
  static uint32_t supplyChecked=0;
  if(!powerPolicy.awake() && pmuReady && uint32_t(now-supplyChecked)>=1000) {
    supplyChecked=now;
    if(powerPolicy.deepSleepDue(now,SettingsPage::timingMs(2),pmu.isVbusGood()))enterDeepSleep();
  }
  delay(3);
}
