#pragma once
#include <Arduino.h>

// One station, with the fields the site's directory files carry.
struct Station {
  String   id;       // Radio Browser uuid; the same id the site uses in /listen/<id>
  String   name;
  String   url;      // stream
  String   codec;    // MP3, AAC, AAC+, ...
  String   country;  // ISO code
  String   icon;     // logo, shown by the web page (the ESP never fetches it)
  uint16_t bitrate = 0;
  bool     hls = false;
};
