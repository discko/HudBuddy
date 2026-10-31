#include "SettingsPage.h"
#include "HudFonts.h"
#include <Arduino.h>
#include <Preferences.h>
namespace SettingsPage {
namespace {
Preferences prefs;
bool storage=false,detail=false;
int level=25;
lv_obj_t *home,*panel,*body,*value;
bool (*clickAllowed)();
void (*apply)(int);
lv_obj_t *box(lv_obj_t *p,int x,int y,int w,int h) {
 auto o=lv_obj_create(p);lv_obj_remove_style_all(o);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);
 lv_obj_clear_flag(o,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);return o;
}
lv_obj_t *label(lv_obj_t *p,const char *s,int x,int y,int w,bool title=false) {
 auto o=lv_label_create(p);lv_label_set_text(o,s);lv_obj_set_pos(o,x,y);lv_obj_set_width(o,w);
 lv_obj_set_style_text_font(o,title?&hud_font_28:&hud_font_24,0);
 lv_obj_set_style_text_color(o,lv_color_hex(0xe8eaed),0);lv_obj_set_style_text_line_space(o,6,0);return o;
}
void back(lv_event_t *) {
 if(!clickAllowed())return;detail=false;
 lv_obj_add_flag(panel,LV_OBJ_FLAG_HIDDEN);lv_obj_clear_flag(home,LV_OBJ_FLAG_HIDDEN);
}
void sliderEvent(lv_event_t *e) {
 if(lv_event_get_code(e)==LV_EVENT_VALUE_CHANGED) {
  level=lv_slider_get_value(lv_event_get_target(e));lv_label_set_text_fmt(value,"%d%%",level);apply(level);
 } else if(storage) prefs.putUChar("brightness",level);
}
void open(lv_event_t *e) {
 if(!clickAllowed())return;
 int page=int(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
 detail=true;lv_obj_add_flag(home,LV_OBJ_FLAG_HIDDEN);lv_obj_clear_flag(panel,LV_OBJ_FLAG_HIDDEN);
 lv_obj_clean(body);lv_obj_scroll_to_y(body,0,LV_ANIM_OFF);
 if(page==0) {
  label(body,"屏幕亮度",8,8,390,true);
  value=label(body,"",8,66,390,true);lv_label_set_text_fmt(value,"%d%%",level);
  auto slider=lv_slider_create(body);lv_obj_set_pos(slider,28,140);lv_obj_set_size(slider,350,16);
  lv_slider_set_range(slider,5,50);lv_slider_set_value(slider,level,LV_ANIM_OFF);
  lv_obj_clear_flag(slider,LV_OBJ_FLAG_SCROLL_CHAIN_HOR|LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_set_ext_click_area(slider,20);
  lv_obj_set_style_bg_color(slider,lv_color_hex(0x8bb8e9),LV_PART_INDICATOR);
  lv_obj_add_event_cb(slider,sliderEvent,LV_EVENT_VALUE_CHANGED,nullptr);
  lv_obj_add_event_cb(slider,sliderEvent,LV_EVENT_RELEASED,nullptr);
  label(body,"5%",8,182,100);label(body,"50%",330,182,80);
  label(body,"拖动调节，松手保存\n亮度上限为 50%，减少长时间高亮",8,230,390);
 } else if(page==1) {
  label(body,"电源与屏保",8,8,390,true);
  label(body,"无操作 1 分钟\n进入小狗屏保\n\n再过 1 分钟\n自动关闭屏幕\n\n屏保中触摸或按 USER 返回\n息屏后短按 BOOT 唤醒\n\nBOOT 短按：工作页进入屏保\n屏保中再按：关闭屏幕\n再按一次：返回工作页\nPWR 长按关机保持原设置\n\n数据同步不会延长亮屏时间",8,66,390);
 } else {
  label(body,"关于 HudBuddy",8,8,390,true);
  label(body,"Waveshare ESP32-S3\nTouch AMOLED 2.16\n480 × 480\n\nUSB 连接电脑同步数据\nHUD 任务 / Codex 额度\n\n16 MB Flash · 8 MB PSRAM\nLVGL 8.4",8,66,390);
 }
}
}
void load() {storage=prefs.begin("hudbuddy",false);level=storage?prefs.getUChar("brightness",25):25;if(level<5||level>50)level=25;}
int brightness() {return level;}
bool isDetail() {return detail;}
void begin(lv_obj_t *parent,bool (*canClick)(),void (*applyBrightness)(int)) {
 clickAllowed=canClick;apply=applyBrightness;
 home=box(parent,26,0,428,390);label(home,"设置",8,10,390,true);
 const char *names[]={"屏幕亮度","电源与屏保","设备信息"};
 const char *hints[]={"调节显示亮度","屏保、息屏与唤醒","关于 HudBuddy"};
 for(int i=0;i<3;++i) {
  auto row=box(home,0,62+i*102,428,94);lv_obj_add_flag(row,LV_OBJ_FLAG_CLICKABLE);
  label(row,names[i],8,4,330,true);auto hint=label(row,hints[i],8,44,350);
  lv_obj_set_style_text_color(hint,lv_color_hex(0x949eac),0);label(row,"›",385,20,30);
  lv_obj_add_event_cb(row,open,LV_EVENT_CLICKED,reinterpret_cast<void*>(uintptr_t(i)));
 }
 panel=box(parent,0,0,480,390);auto b=box(panel,26,0,124,46);lv_obj_add_flag(b,LV_OBJ_FLAG_CLICKABLE);
 label(b,"‹ 返回",8,4,116);lv_obj_add_event_cb(b,back,LV_EVENT_CLICKED,nullptr);
 body=box(panel,26,52,428,338);lv_obj_add_flag(body,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
 lv_obj_set_scroll_dir(body,LV_DIR_VER);lv_obj_clear_flag(body,LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
 lv_obj_set_scrollbar_mode(body,LV_SCROLLBAR_MODE_AUTO);lv_obj_add_flag(panel,LV_OBJ_FLAG_HIDDEN);
}
}
