#pragma once
#include <Arduino.h>
// Drift, as the site's landing does it (js/home.js): a station somewhere on
// Earth, or in a mood. Anywhere picks a country weighted by how many stations
// it has, then one of its 40 most played; a mood picks among the genre's most
// played. Recent picks are left out, and a station that does not answer is
// drifted on from, three times at most. The fetching happens on a task of its
// own, so the screen keeps moving while it tunes.
namespace drift {
  enum State { IDLE, TUNING, LANDED, FAILED };
  void   start(const String& mood);   // "" for anywhere
  void   loop();                      // the main loop: plays what the task found, watches it answer
  State  state();
  unsigned long since();              // when the state last changed
  String mood();
  String place();                     // the country it landed in
  String message();                   // what went wrong, or that it is drifting on
  String countryName(const String& code);   // "DE" -> "Germany"
}
