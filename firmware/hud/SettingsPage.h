#pragma once
#include <lvgl.h>
namespace SettingsPage {
void load();
int brightness();
bool isDetail();
void begin(lv_obj_t *parent,bool (*canClick)(),void (*applyBrightness)(int));
}
