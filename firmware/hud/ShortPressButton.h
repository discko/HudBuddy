#pragma once
#include <stdint.h>
// GPIO CHANGE IRQ starts processing; only debounce settling needs follow-up.
class ShortPressButton {
 public:
  void begin(uint32_t now,bool pressed) {
    raw_=stable_=pressed;changed_=pressedAt_=now;armed_=false;
  }
  bool settling() const {return raw_!=stable_;}
  bool update(uint32_t now,bool pressed) {
    if(pressed!=raw_) {raw_=pressed;changed_=now;}
    if(raw_==stable_ || uint32_t(now-changed_)<30) return false;
    stable_=raw_;
    if(stable_) {pressedAt_=changed_;armed_=true;return false;}
    bool shortPress=armed_ && uint32_t(changed_-pressedAt_)<=800;
    armed_=false;return shortPress;
  }
 private:
  bool raw_=false,stable_=false,armed_=false;
  uint32_t changed_=0,pressedAt_=0;
};
