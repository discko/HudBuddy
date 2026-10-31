#pragma once
#include <stdint.h>

// A drag remains a drag even when the scroll container has no overflow,
// or when the finger returns to its starting position before release.
class TapGuard {
 public:
  void update(bool pressed, int x, int y) {
    if(pressed) {
      if(!pressed_) { startX_=x; startY_=y; moved_=false; }
      int32_t dx=x-startX_, dy=y-startY_;
      if(dx*dx+dy*dy >= 12*12) moved_=true;
    }
    pressed_=pressed;
  }
  bool allowsClick() const { return !moved_; }
 private:
  bool pressed_=false, moved_=false;
  int startX_=0, startY_=0;
};
