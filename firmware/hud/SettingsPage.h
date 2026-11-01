#pragma once
#include <lvgl.h>
namespace SettingsPage {
void load();
int brightness();
uint32_t timingMs(unsigned field);
bool isDetail();
void begin(lv_obj_t *parent,bool (*canClick)(),void (*applyBrightness)(int));
}
