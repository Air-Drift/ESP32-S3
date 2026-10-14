#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
// What the radio knows about itself: board, chip, memory, WiFi, CPU load,
// the audio pipeline and the task list. Feeds the web page's Device tab.
namespace devinfo {
  void begin();
  void loop();                    // samples CPU load every couple of seconds
  void fill(JsonDocument& doc);   // the whole picture, as JSON
}
