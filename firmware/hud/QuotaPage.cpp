#include "QuotaPage.h"
#include "QuotaPolicy.h"
#include "HudFonts.h"
#include <algorithm>
#include <cmath>

namespace QuotaPage {
namespace {
constexpr uint32_t text=0xe8eaed,muted=0x949eac;
Data current;
bool received=false,dirty=true,detail=false;
bool (*canClick)()=nullptr;
int64_t clockNow=0,lastSecond=-1;
int tz=480;
lv_obj_t *mainPanel,*detailPanel,*detailList,*titleDot,*detailDot;
lv_obj_t *account,*plan,*cached,*numberLabel,*bar,*resetLabel,*extraLabel,*extraReset;
lv_obj_t *card,*cardTitle,*warning,*expiryLabel,*caption,*emptyLabel,*emptyHint;
lv_obj_t *detailTitle;
struct CardRow {lv_obj_t *time,*status,*absolute;int64_t expiry;};
std::vector<CardRow> rows;

lv_obj_t *box(lv_obj_t *p,int x,int y,int w,int h) {
 auto o=lv_obj_create(p);lv_obj_remove_style_all(o);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);
 lv_obj_clear_flag(o,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);return o;
}
lv_obj_t *label(lv_obj_t *p,const char *s,int x,int y,int w,const lv_font_t *f=&hud_font_24,uint32_t color=text) {
 auto o=lv_label_create(p);lv_label_set_text(o,s);lv_obj_set_pos(o,x,y);if(w)lv_obj_set_width(o,w);
 lv_obj_set_style_text_font(o,f,0);lv_obj_set_style_text_color(o,lv_color_hex(color),0);return o;
}
void scroll(lv_obj_t *o) {
 lv_obj_add_flag(o,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
 lv_obj_set_scroll_dir(o,LV_DIR_VER);lv_obj_set_scrollbar_mode(o,LV_SCROLLBAR_MODE_AUTO);
 lv_obj_set_style_width(o,3,LV_PART_SCROLLBAR);lv_obj_set_style_bg_color(o,lv_color_hex(0x39404c),LV_PART_SCROLLBAR);
}
void hidden(lv_obj_t *o,bool hide) {if(hide)lv_obj_add_flag(o,LV_OBJ_FLAG_HIDDEN);else lv_obj_clear_flag(o,LV_OBJ_FLAG_HIDDEN);}
void setLabel(lv_obj_t *o,const char *s) {if(strcmp(lv_label_get_text(o),s))lv_label_set_text(o,s);}
const char *status(QuotaPolicy::Urgency u) {
 using U=QuotaPolicy::Urgency;
 switch(u){case U::Expired:return "! 已到期";case U::Critical:return "!! 12小时内";case U::Urgent:return "! 24小时内";case U::Soon:return "! 即将到期";case U::Unknown:return "日期未知";default:return "可用";}
}
uint32_t accent(QuotaPolicy::Urgency u) {
 using U=QuotaPolicy::Urgency;
 if(u==U::Critical||u==U::Expired)return 0xff7b84;
 if(u==U::Urgent)return 0xff9090;
 if(u==U::Soon)return 0xe7c071;
 return 0xb9c6d6;
}
uint32_t background(QuotaPolicy::Urgency u) {
 using U=QuotaPolicy::Urgency;
 if(u==U::Critical||u==U::Expired)return 0x32141a;
 if(u==U::Urgent)return 0x281519;
 if(u==U::Soon)return 0x272114;
 return 0x151b24;
}
void switchDetail(bool show) {
 detail=show;hidden(mainPanel,show);hidden(detailPanel,!show);lastSecond=-1;
}
void openCards(lv_event_t *) {if(canClick&&canClick())switchDetail(true);}
void back(lv_event_t *) {if(canClick&&canClick())switchDetail(false);}
bool timeField(cJSON *o,const char *key,int64_t &out) {
 auto v=cJSON_GetObjectItemCaseSensitive(o,key);
 if(!v||cJSON_IsNull(v)){out=-1;return true;}
 if(!cJSON_IsNumber(v)||!std::isfinite(v->valuedouble)||v->valuedouble<0||v->valuedouble>253402300799999.0)return false;
 out=int64_t(v->valuedouble);return true;
}
bool parseWindow(cJSON *v,Window &w) {
 if(!v||cJSON_IsNull(v))return true;
 if(!cJSON_IsObject(v))return false;w.present=true;
 auto n=cJSON_GetObjectItemCaseSensitive(v,"remaining");
 if(n&&!cJSON_IsNull(n)) {
  if(!cJSON_IsNumber(n)||!std::isfinite(n->valuedouble)||n->valuedouble<0||n->valuedouble>100)return false;
  w.remaining=n->valuedouble;
 }
 return timeField(v,"resets_at_ms",w.resetsAt);
}
void rebuild() {
 dirty=false;
 lv_coord_t y=lv_obj_get_scroll_y(detailList);lv_obj_clean(detailList);rows.clear();
 lv_label_set_text(account,current.account.c_str());lv_label_set_text(plan,current.plan.c_str());
 hidden(cached,!current.stale);
 bool empty=!current.available;
 for(auto o:{numberLabel,bar,resetLabel,card,extraLabel,extraReset})hidden(o,empty);
 hidden(emptyLabel,!empty);hidden(emptyHint,!empty);
 if(empty) {
  setLabel(emptyLabel,!received?"等待 Magpie 数据":current.stale?"Magpie 暂不可用":"未找到 Codex 账号");
  setLabel(emptyHint,current.stale?"连接恢复后自动更新":"请在电脑端连接 Codex");
  setLabel(numberLabel,"--%");
 } else {
  if(current.weekly.remaining>=0)lv_label_set_text_fmt(numberLabel,"%d%%",int(std::round(current.weekly.remaining)));else setLabel(numberLabel,"--%");
  lv_bar_set_value(bar,current.weekly.remaining<0?0:int(std::round(current.weekly.remaining)),LV_ANIM_OFF);
  bool extra=current.fiveHour.present;
  hidden(extraLabel,!extra);hidden(extraReset,!extra);
  lv_obj_set_y(card,extra?301:231);
  if(extra) {
   if(current.fiveHour.remaining<0)setLabel(extraLabel,"5小时额度 · 余量未知");
   else lv_label_set_text_fmt(extraLabel,"5小时额度 · 剩余 %d%%",int(std::round(current.fiveHour.remaining)));
  }
 }
 if(current.cardsKnown) {
  lv_label_set_text_fmt(cardTitle,"重置卡 %u 张",current.cardCount);
  lv_label_set_text_fmt(detailTitle,"重置卡 · %u 张",current.cardCount);
 } else {setLabel(cardTitle,"重置卡");setLabel(detailTitle,"重置卡");}
 if(!current.cardsKnown||!current.cardCount)label(detailList,current.cardsKnown?"暂无重置卡":"重置卡信息暂不可用",8,16,396);
 for(size_t i=0;i<current.expiries.size();++i) {
  int top=i*114;
  auto line=box(detailList,8,top,396,1);lv_obj_set_style_bg_color(line,lv_color_hex(0x252a32),0);lv_obj_set_style_bg_opa(line,LV_OPA_COVER,0);
  auto rowTitle=label(detailList,"",8,top+12,160);lv_label_set_text_fmt(rowTitle,"第 %u 张",unsigned(i+1));
  auto state=label(detailList,"",190,top+12,214);lv_obj_set_style_text_align(state,LV_TEXT_ALIGN_RIGHT,0);
  auto remaining=label(detailList,"",8,top+45,396);
  auto absolute=label(detailList,"",8,top+80,396,&lv_font_montserrat_20,muted);
  rows.push_back({remaining,state,absolute,current.expiries[i]});
 }
 lv_obj_update_layout(detailList);lv_obj_scroll_to_y(detailList,y,LV_ANIM_OFF);
 lastSecond=-1;
}
}

bool parse(cJSON *raw,Data &out) {
 if(!cJSON_IsObject(raw))return false;
 auto a=cJSON_GetObjectItemCaseSensitive(raw,"available"),s=cJSON_GetObjectItemCaseSensitive(raw,"stale");
 if(!cJSON_IsBool(a)||!cJSON_IsBool(s))return false;
 out=Data{};out.available=cJSON_IsTrue(a);out.stale=cJSON_IsTrue(s);
 if(!timeField(raw,"last_success_ms",out.lastSuccess)||!timeField(raw,"monitor_started_ms",out.firstAttempt)||!timeField(raw,"read_at_ms",out.readAt))return false;
 if(!out.available)return true;
 auto account=cJSON_GetObjectItemCaseSensitive(raw,"account"),plan=cJSON_GetObjectItemCaseSensitive(raw,"plan");
 if(!cJSON_IsString(account)||!cJSON_IsString(plan)||strlen(account->valuestring)>512||strlen(plan->valuestring)>128)return false;
 out.account=account->valuestring;out.plan=plan->valuestring;
 if(!parseWindow(cJSON_GetObjectItemCaseSensitive(raw,"weekly"),out.weekly)||!parseWindow(cJSON_GetObjectItemCaseSensitive(raw,"five_hour"),out.fiveHour))return false;
 auto cards=cJSON_GetObjectItemCaseSensitive(raw,"cards");
 if(!cards||cJSON_IsNull(cards))return true;
 if(!cJSON_IsObject(cards))return false;
 auto count=cJSON_GetObjectItemCaseSensitive(cards,"count"),dates=cJSON_GetObjectItemCaseSensitive(cards,"expires_at_ms");
 if(!cJSON_IsNumber(count)||count->valuedouble<0||count->valuedouble>64||floor(count->valuedouble)!=count->valuedouble||!cJSON_IsArray(dates)||cJSON_GetArraySize(dates)>64)return false;
 out.cardsKnown=true;out.cardCount=count->valueint;
 cJSON *d; cJSON_ArrayForEach(d,dates) {
  if(cJSON_IsNull(d))out.expiries.push_back(-1);
  else if(cJSON_IsNumber(d)&&std::isfinite(d->valuedouble)&&d->valuedouble>=0&&d->valuedouble<=253402300799999.0)out.expiries.push_back(int64_t(d->valuedouble));
  else return false;
 }
 if(out.expiries.size()>out.cardCount)return false;
 while(out.expiries.size()<out.cardCount)out.expiries.push_back(-1);
 std::sort(out.expiries.begin(),out.expiries.end(),[](int64_t a,int64_t b){return (a<0?INT64_MAX:a)<(b<0?INT64_MAX:b);});
 return true;
}
bool isDetail() {return detail;}
void closeDetail(){switchDetail(false);}
void set(Data data) {current=std::move(data);received=true;dirty=true;}
void connectionTime(int64_t lastSuccess,int64_t firstAttempt) {
 if(lastSuccess>=0)current.lastSuccess=lastSuccess;
 if(firstAttempt>=0)current.firstAttempt=firstAttempt;
}
void begin(lv_obj_t *parent,bool (*allowsClick)()) {
 canClick=allowsClick;
 mainPanel=box(parent,0,0,480,390);scroll(mainPanel);
 label(mainPanel,"Codex",34,10,100,&hud_font_28);
 titleDot=box(mainPanel,133,22,8,8);lv_obj_set_style_radius(titleDot,4,0);lv_obj_set_style_bg_opa(titleDot,LV_OPA_COVER,0);
 plan=label(mainPanel,"",254,17,192,&lv_font_montserrat_20,0xa5c4ee);lv_obj_set_style_text_align(plan,LV_TEXT_ALIGN_RIGHT,0);
 account=label(mainPanel,"",34,50,300,&lv_font_montserrat_20,muted);lv_label_set_long_mode(account,LV_LABEL_LONG_SCROLL_CIRCULAR);
 cached=label(mainPanel,"缓存",370,46,76,&hud_font_24,0xe7c071);
 label(mainPanel,"7天额度",34,94,150);label(mainPanel,"剩余可用",34,128,150,&hud_font_24,muted);
 numberLabel=label(mainPanel,"--%",208,88,238,&lv_font_montserrat_48);lv_obj_set_style_text_align(numberLabel,LV_TEXT_ALIGN_RIGHT,0);
 bar=lv_bar_create(mainPanel);lv_obj_set_pos(bar,34,164);lv_obj_set_size(bar,412,10);lv_obj_set_style_bg_color(bar,lv_color_hex(0x1b2530),0);lv_obj_set_style_bg_color(bar,lv_color_hex(0x8bb8e9),LV_PART_INDICATOR);lv_obj_set_style_radius(bar,5,0);lv_obj_set_style_radius(bar,5,LV_PART_INDICATOR);
 label(mainPanel,"额度重置",34,187,108,&hud_font_24,muted);
 resetLabel=label(mainPanel,"时间未知",144,187,302,&hud_font_24,0xbfc8d5);lv_obj_set_style_text_align(resetLabel,LV_TEXT_ALIGN_RIGHT,0);
 extraLabel=label(mainPanel,"",34,229,412,&hud_font_24,muted);
 extraReset=label(mainPanel,"",34,260,412,&hud_font_24,muted);
 card=box(mainPanel,34,231,412,136);lv_obj_add_flag(card,LV_OBJ_FLAG_CLICKABLE);lv_obj_set_style_bg_opa(card,LV_OPA_COVER,0);lv_obj_set_style_radius(card,12,0);lv_obj_set_style_border_width(card,4,0);lv_obj_set_style_border_side(card,LV_BORDER_SIDE_LEFT,0);lv_obj_add_event_cb(card,openCards,LV_EVENT_CLICKED,nullptr);
 cardTitle=label(card,"重置卡",16,10,195);
 warning=label(card,"",208,10,188);lv_obj_set_style_text_align(warning,LV_TEXT_ALIGN_RIGHT,0);
 expiryLabel=label(card,"",16,49,380,&hud_font_28);
 caption=label(card,"",16,96,300,&hud_font_24,muted);label(card,"明细 ›",324,96,76,&hud_font_24,muted);
 emptyLabel=label(mainPanel,"等待 Magpie 数据",34,252,412,&hud_font_28);
 emptyHint=label(mainPanel,"请启动电脑端 Bridge",34,300,412,&hud_font_24,muted);
 detailPanel=box(parent,0,0,480,390);
 auto backButton=box(detailPanel,26,0,124,46);lv_obj_add_flag(backButton,LV_OBJ_FLAG_CLICKABLE);label(backButton,"‹ 返回",8,4,0,&hud_font_24,muted);lv_obj_add_event_cb(backButton,back,LV_EVENT_CLICKED,nullptr);
 detailDot=box(detailPanel,430,21,8,8);lv_obj_set_style_radius(detailDot,4,0);lv_obj_set_style_bg_opa(detailDot,LV_OPA_COVER,0);
 detailTitle=label(detailPanel,"重置卡",34,51,412,&hud_font_28);
 detailList=box(detailPanel,26,96,428,290);scroll(detailList);
 lv_obj_clear_flag(detailList,LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
 switchDetail(false);rebuild();
}
void update(int64_t now,int timezoneMinutes) {
 clockNow=now;tz=timezoneMinutes;
 if(dirty)rebuild();
 if(lastSecond==now/1000)return;lastSecond=now/1000;
 bool offline=QuotaPolicy::offlineWarning(now,current.lastSuccess,current.firstAttempt);
 for(auto dot:{titleDot,detailDot}) {hidden(dot,!offline);lv_obj_set_style_bg_color(dot,lv_color_hex(0xef7379),0);}
 char buf[96];
 auto resetText=[&](const Window &w,lv_obj_t *o){
  if(w.resetsAt>=0&&w.resetsAt<=now)snprintf(buf,sizeof(buf),"等待额度更新");
  else QuotaPolicy::deadline(buf,sizeof(buf),w.resetsAt,now,tz);
  setLabel(o,buf);
 };
 resetText(current.weekly,resetLabel);resetText(current.fiveHour,extraReset);
 int64_t nearest=current.expiries.empty()?-1:current.expiries.front();
 auto urgency=QuotaPolicy::urgency(nearest,now);
 if(!current.cardsKnown||current.cardCount==0)urgency=QuotaPolicy::Urgency::Unknown;
 lv_obj_set_style_bg_color(card,lv_color_hex(background(urgency)),0);lv_obj_set_style_border_color(card,lv_color_hex(accent(urgency)),0);
 for(auto o:{warning,expiryLabel})lv_obj_set_style_text_color(o,lv_color_hex(accent(urgency)),0);
 setLabel(warning,current.cardsKnown&&current.cardCount?status(urgency):"");
 if(!current.cardsKnown)snprintf(buf,sizeof(buf),"信息暂不可用");
 else if(!current.cardCount)snprintf(buf,sizeof(buf),"暂无重置卡");
 else QuotaPolicy::deadline(buf,sizeof(buf),nearest,now,tz);
 setLabel(expiryLabel,buf);
 setLabel(caption,urgency==QuotaPolicy::Urgency::Expired?"已到期 · 等待更新":urgency==QuotaPolicy::Urgency::Critical?"最近一张即将失效":urgency==QuotaPolicy::Urgency::Urgent||urgency==QuotaPolicy::Urgency::Soon?"最近一张即将过期":"最近到期");
 for(auto &r:rows) {
  auto u=QuotaPolicy::urgency(r.expiry,now);setLabel(r.status,status(u));lv_obj_set_style_text_color(r.status,lv_color_hex(accent(u)),0);
  QuotaPolicy::deadline(buf,sizeof(buf),r.expiry,now,tz);setLabel(r.time,buf);
  if(r.expiry<0)snprintf(buf,sizeof(buf),"--");
  else QuotaPolicy::deadline(buf,sizeof(buf),r.expiry,now,tz,true);
  setLabel(r.absolute,buf);
 }
}
}
