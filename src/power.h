#pragma once
#include <Arduino.h>
// The battery, on boards that have one (PIN_BAT_ADC in config.h), and how the
// radio came on. Plugging in a cable powers the board up as surely as its
// switch does; when that is what woke it, it stands by on a charge screen
// instead of starting the radio, until someone asks for the radio.
namespace power {
  void begin();         // first thing in setup()
  void loop();
  int  millivolts();    // the battery, smoothed; 0 without one
  int  percent();       // 0..100, or -1 without a battery
  bool usb();           // a computer is on the USB port
  bool standby();       // on the charge screen: no WiFi, no radio yet
  void wake();          // leave the charge screen and start the radio
  // What it has learned about this battery (see power.cpp).
  int  lift();          // mV charging adds to the reading
  bool learned();       // a full run has given this battery's own curve
  int  runMinutes();    // of the run being recorded now, 0 for none
  void forget();        // back to the generic Li-ion table
}
