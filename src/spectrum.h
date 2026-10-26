#pragma once
#include <Arduino.h>
// The music's spectrum, for the visualiser: the decoder's samples go into a
// ring as they are decoded, and read() takes an FFT of the stretch that is
// coming out of the speaker about now. Bands are log-spaced like the site's
// (js/audiograph.js), 40 Hz to 16 kHz, each 0..1.
namespace spectrum {
  void enable(bool on);                       // collect samples (only while it is shown)
  bool read(float* bands, int n, uint32_t rate);  // false while there is nothing to read
  unsigned long heard();                      // millis() when the decoder last handed over sound
}
