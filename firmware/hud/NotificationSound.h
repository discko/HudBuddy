#pragma once
#include <stdint.h>
namespace NotificationSound {
void begin();
void notify();
void stop();
void update(uint32_t now);
}
