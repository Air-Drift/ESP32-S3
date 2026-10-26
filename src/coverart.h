#pragma once
#include <Arduino.h>
// The cover of the song on air, found the way the site finds it
// (js/trackinfo.js): MusicBrainz says which recording "Artist - Title" is and
// its original album, the Cover Art Archive has that album's front. Anything
// short of a clear match gives no cover: a wrong album is worse than none.
// Without one, the station's own logo (its directory icon) stands in.
namespace coverart {
  void loop();                                   // notices a new song and looks it up, one at a time
  // The current song's cover, or else the station's logo: RGB565, side x
  // side, rows 128 apart; null for neither. Held until release(), so a new cover cannot land mid-draw.
  const uint16_t* image(int& side);
  void release();
}
