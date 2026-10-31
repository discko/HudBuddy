#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <TouchDrvCSTXXX.hpp>
#include <HudBuddyPower.h>
#include "DogScene.h"

// Waveshare ESP32-S3-Touch-AMOLED-2.16; confirmed against original firmware.
constexpr int kButton = 18;
constexpr int kTouchInt = 11;
constexpr uint8_t kBrightness = 64;
Arduino_ESP32QSPI bus(12, 38, 4, 5, 6, 7);
Arduino_CO5300 display(&bus, 39, 0, 480, 480, 0, 0, 0, 0);
Arduino_Canvas frame(480, 480, &display);
uint32_t lastFrame = 0;
TouchDrvCST92xx touch;
HudBuddyPower power;
bool touchReady = false;
volatile bool touchPending = false;

void IRAM_ATTR onTouch() { touchPending = true; }
bool consumeTouch() {
  noInterrupts();
  bool pending = touchPending;
  touchPending = false;
  interrupts();
  if (!touchReady || !pending) return false;
  int16_t x[5], y[5];
  return touch.getPoint(x, y, 5) > 0;
}
void drawStandby() {
  lastFrame = millis();
  DogScene::render(frame, lastFrame);
  frame.flush();
}
void setup() {
  Serial.begin(115200);
  pinMode(kButton, INPUT_PULLUP);
  Wire.begin(15, 14);
  touch.setPins(40, kTouchInt);
  touchReady = touch.begin(Wire, CST92XX_SLAVE_ADDRESS, 15, 14);
  if (touchReady) {
    pinMode(kTouchInt, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(kTouchInt), onTouch, FALLING);
  }
  if (!display.begin()) {
    Serial.println("ERROR display initialization failed");
    while (true) delay(1000);
  }
  if (!frame.begin(GFX_SKIP_OUTPUT_BEGIN)) {
    Serial.println("ERROR animation framebuffer allocation failed");
    display.displayOff();
    while (true) delay(1000);
  }
  bus.writeC8D8(0x36, 0xA0);  // Manufacturer's orientation for this panel.
  display.setBrightness(kBrightness);
  drawStandby();
  power.begin(millis());
  Serial.printf("HudBuddy puppy standby ready; touch=%s; idle=60000; hold=3000; button=GPIO18\n",
                touchReady ? "online" : "offline");
}
void loop() {
  bool touched = consumeTouch();
  bool pressed = digitalRead(kButton) == LOW;
  auto action = power.update(millis(), pressed, touched);
  if (action == HudBuddyPower::Action::Sleep) {
    display.setBrightness(0);
    display.displayOff();
    Serial.printf("%lu SCREEN_OFF idle\n", millis());
  } else if (action == HudBuddyPower::Action::Wake) {
    display.displayOn();
    drawStandby();
    display.setBrightness(kBrightness);
    Serial.printf("%lu SCREEN_ON user_hold\n", millis());
  }
  if (touched && power.awake()) Serial.printf("%lu TOUCH activity\n", millis());
  if (power.awake() && uint32_t(millis() - lastFrame) >= 40) drawStandby();
  delay(5);
}
