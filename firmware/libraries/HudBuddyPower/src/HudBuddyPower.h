#pragma once
#include <stdint.h>

// Hardware-independent policy shared by all HudBuddy firmware.
class HudBuddyPower {
 public:
  enum class Action { None, Sleep, Wake };
  static constexpr uint32_t kIdleMs = 60000;
  static constexpr uint32_t kHoldMs = 3000;
  static constexpr uint32_t kDebounceMs = 30;

  void begin(uint32_t now, uint32_t idleMs = kIdleMs, bool userHoldWake = true) {
    idleMs_ = idleMs;
    userHoldWake_ = userHoldWake;
    awake_ = true;
    rawPressed_ = pressed_ = false;
    lastActivity_ = rawChanged_ = holdStarted_ = now;
  }
  Action update(uint32_t now, bool rawPressed, bool touch) {
    if (rawPressed != rawPressed_) {
      rawPressed_ = rawPressed;
      rawChanged_ = now;
    }
    if (pressed_ != rawPressed_ && uint32_t(now - rawChanged_) >= kDebounceMs) {
      pressed_ = rawPressed_;
      if (pressed_) holdStarted_ = now;
    }
    if (awake_) {
      if (pressed_ || touch) lastActivity_ = now;
      if (uint32_t(now - lastActivity_) >= idleMs_) {
        awake_ = false;
        offAt_ = now;
        // A press that started before sleep must not count towards wake.
        holdStarted_ = now;
        return Action::Sleep;
      }
    } else if (userHoldWake_ && pressed_ && uint32_t(now - holdStarted_) >= kHoldMs) {
      awake_ = true;
      lastActivity_ = now;
      return Action::Wake;
    }
    return Action::None;
  }
  void setIdleMs(uint32_t ms) { idleMs_ = ms; }
  void startOff(uint32_t now) { awake_ = false; offAt_ = now; }
  bool deepSleepDue(uint32_t now, uint32_t delayMs, bool usbPowered) const {
    return !awake_ && !usbPowered && uint32_t(now-offAt_) >= delayMs;
  }
  bool awake() const { return awake_; }
  // Formal UI: BOOT cycles work -> screensaver -> display off -> work.
  Action cycleDisplay(uint32_t now, uint32_t saverAfterMs = 60000) {
    if (!awake_) {
      awake_ = true;
      lastActivity_ = now;
      return Action::Wake;
    }
    if (idleFor(now) < saverAfterMs) {
      lastActivity_ = now - saverAfterMs;
      return Action::None;
    }
    awake_ = false;
    offAt_ = now;
    holdStarted_ = now;
    return Action::Sleep;
  }
  uint32_t idleFor(uint32_t now) const { return uint32_t(now-lastActivity_); }
 private:
  bool awake_ = true, rawPressed_ = false, pressed_ = false;
  bool userHoldWake_ = true;
  uint32_t lastActivity_ = 0, rawChanged_ = 0, holdStarted_ = 0;
  uint32_t offAt_ = 0;
  uint32_t idleMs_ = kIdleMs;
};
