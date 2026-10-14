#pragma once
#include <Arduino.h>
#include <vector>
#include "station.h"

// The station directory, read from the same static files the site serves:
//   popular, trending            data/v1/popular.json, data/v1/trending.json
//   country:US                   data/v1/country/US.head.json
//   tag:jazz                     data/v1/tag/<file>.head.json, via index.json
// Search and link resolution go to Radio Browser, as the site's search does.
namespace directory {
  extern String               source;    // what `stations` holds, e.g. "popular", "country:US", "search:jazz fm"
  extern std::vector<Station> stations;  // at most MAX_STATIONS
  extern String               error;     // why the last load failed, or ""

  bool load(const String& src);                  // "favourites" | "popular" | "trending" | "country:XX" | "tag:name"
  bool search(const String& term);               // live search by name
  bool byId(const String& id, Station& out);     // one station by uuid (an Air Drift /listen/<id> link)
  bool reportPlay(const String& id);             // the play count that ranks stations
  String stationIdFromLink(const String& text);  // uuid out of an airdrift.stream link, or ""
}
