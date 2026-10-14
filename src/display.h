#pragma once
#include <Arduino.h>
// The touch screen, on boards that have one (HAS_DISPLAY in config.h):
// station, title and volume on the screen; tap and swipe to drive the radio.
// On the custom build these are empty.
namespace display {
  void begin();
  void loop();
  void busy(const String& text);   // a full-screen notice that stays until cleared with ""
  const uint16_t* frame();         // the screen's pixels, RGB565, side() square; null without a screen
  int  side();                     // the screen's width and height in pixels, 0 without a screen
  void preview(const String& name); // show a screen by name for screenshots ("off" to stop)
}
