#pragma once
#include <Arduino.h>
#include "station.h"

// The player: one station at a time, decoded on its own task, out over I2S.
namespace player {
  void    begin();
  bool    play(const Station& s, int index = -1);  // index: position in directory::stations, or -1
  bool    playIndex(int i);
  void    stop();
  void    togglePause();
  void    next();
  void    prev();
  void    setVolume(int v);       // 0..21, clamped
  int     volume();
  bool    running();              // a stream is open (paused counts)
  bool    paused();
  bool    connecting();           // a stream was asked for and has not started yet
  const Station& current();
  int     currentIndex();
  String  title();                // the stream's own "now playing", or ""
  String  codec();                // "MP3 128 kbps"
  String  lastError();
  uint32_t buffered();            // bytes of stream waiting to be decoded
  uint32_t bufferSize();
  uint32_t sampleRate();          // the stream's own rate
  void    setResample(bool on);   // resample the output to 48 kHz, or not
  void    setDecodeCore(int core);
  int     decodeCore();
  void    setDebug(bool on);       // print the decoder's info lines on the console // which CPU core decodes (0 shares with WiFi, 1 with the UI)
  void    resumeLast();           // after a reboot, when AUTO_RESUME
  void    forgetState();          // drop the saved station and volume
  void    loop();                 // reconnects a dropped stream
}
