#pragma once
#include <Arduino.h>
#include <vector>
#include "station.h"
// Saved stations, kept in flash as /favourites.json.
namespace favourites {
  void begin();
  const std::vector<Station>& list();
  bool has(const String& id);
  bool add(const Station& s);        // false if already there or full
  bool remove(const String& id);
  bool toggle(const Station& s);     // returns true if it is now a favourite
  void clear();                      // forget them all
}
