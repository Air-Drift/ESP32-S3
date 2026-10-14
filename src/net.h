#pragma once
#include <Arduino.h>

// WiFi: stored credentials first, otherwise a setup access point with a
// captive portal, and the serial console can set credentials at any time.
namespace net {
  void   begin();
  void   loop();
  bool   connected();
  bool   portalOpen();
  String ip();
  void   setCredentials(const String& ssid, const String& pass);
  String joining();       // the network the setup portal is trying right now, or ""
  void   forget();        // erase credentials and restart into the setup portal
  void   factoryReset();  // erase credentials, favourites, last station and volume, then restart
}
