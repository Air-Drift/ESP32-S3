#pragma once
#include <Arduino.h>
// Firmware from the project's GitHub releases: find out what the newest is,
// and fetch and install it on request. The install runs on the main loop,
// which it holds for the download; the screen shows the progress.
namespace updater {
  bool   latest(String& version, String& error);   // asks the release page; false with a reason
  bool   newer(const String& version);              // than the running firmware
  void   install();                                 // on the next loop: download, write, restart
  bool   installing();
  void   loop();
}
