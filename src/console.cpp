#include "console.h"
#include "config.h"
#include "net.h"
#include "directory.h"
#include "player.h"
#include "favourites.h"
#include <Arduino.h>
#include <WiFi.h>
#include "display.h"
#include "screenshot.h"

// The screen as a BMP in base64 between two marker lines, for a script on
// the other end to decode: the only way to catch the setup screen, which
// shows when there is no WiFi to fetch /screen.bmp over.
static void dumpScreen() {
  const uint16_t* fb = display::frame();
  if (!fb) { Serial.println("this board has no screen"); return; }
  static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const int N = display::side();
  uint8_t hdr[BMP_HEADER]; bmpHeader(hdr, N, N);
  const size_t total = BMP_HEADER + (size_t)N * N * 2;
  auto byteAt = [&](size_t i) -> uint8_t {
    if (i < BMP_HEADER) return hdr[i];
    size_t p = i - BMP_HEADER, px = p / 2;
    uint16_t v = fb[(size_t)(N - 1 - px / N) * N + px % N];   // bottom-up
    return (p & 1) ? v >> 8 : v & 0xFF;
  };
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(2000);   // never drop a byte of this
#endif
  Serial.println("-----BEGIN SCREEN-----");
  char line[77]; int n = 0;
  for (size_t i = 0; i < total; i += 3) {
    uint32_t v = (uint32_t)byteAt(i) << 16 | (i + 1 < total ? byteAt(i + 1) : 0) << 8 | (i + 2 < total ? byteAt(i + 2) : 0);
    line[n++] = T[v >> 18]; line[n++] = T[(v >> 12) & 63];
    line[n++] = i + 1 < total ? T[(v >> 6) & 63] : '=';
    line[n++] = i + 2 < total ? T[v & 63] : '=';
    if (n == 76) { line[n] = 0; Serial.println(line); n = 0; }
  }
  if (n) { line[n] = 0; Serial.println(line); }
  Serial.println("-----END SCREEN-----");
  Serial.flush();
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(5);
#endif
}

static String line;

static void printList() {
  Serial.printf("--- %s: %d stations ---\n", directory::source.c_str(), (int)directory::stations.size());
  int i = 0;
  for (const Station& s : directory::stations) {
    Serial.printf("%3d  %-40.40s  %s %s %u\n", i++, s.name.c_str(), s.country.c_str(), s.codec.c_str(), s.bitrate);
  }
}

static void status() {
  Serial.printf("Air Drift ESP32 %s\n", AD_VERSION);
  Serial.printf("wifi:    %s", net::connected() ? net::ip().c_str() : (net::portalOpen() ? "setup portal open" : "not connected"));
  if (net::connected()) Serial.printf("  (%d dBm)", WiFi.RSSI());
  Serial.println();
  Serial.printf("list:    %s (%d)\n", directory::source.c_str(), (int)directory::stations.size());
  const Station& c = player::current();
  Serial.printf("station: %s\n", c.name.isEmpty() ? "-" : c.name.c_str());
  Serial.printf("state:   %s, volume %d\n", player::connecting() ? "connecting" : player::paused() ? "paused" : player::running() ? "playing" : "stopped", player::volume());
  Serial.printf("stream:  %s\n", player::codec().c_str());
  Serial.printf("title:   %s\n", player::title().c_str());
  Serial.printf("buffer:  %u of %u bytes, stream at %u Hz\n", player::buffered(), player::bufferSize(), player::sampleRate());
  Serial.printf("heap:    %u free, %u PSRAM free\n", ESP.getFreeHeap(), ESP.getFreePsram());
}

static void help() {
  Serial.println(
    "commands:\n"
    "  wifi <ssid> <password>   join a network (quote a name with spaces)\n"
    "  forget                   erase WiFi and restart into the setup portal\n"
"  factory                  erase WiFi, favourites and the last station, then restart\n"
"  screen                   the screen as a BMP, base64, between marker lines\n"
"  ui <name>                show a screen for a screenshot: home empty list about phone setup busy; ui off\n"
    "  popular | trending       load that list from airdrift.stream\n"
    "  favourites               load the saved stations\n"
    "  fav                      save or unsave the playing station\n"
    "  country <CC>             load a country, e.g. country GB\n"
    "  genre <name>             load a genre, e.g. genre jazz\n"
    "  search <words>           search stations by name\n"
    "  list                     show the loaded list\n"
    "  play <n>                 play station n from the list\n"
    "  play <air drift link>    play a shared https://airdrift.stream/listen/... link\n"
    "  play <stream url>        play any stream\n"
    "  next | prev | pause | stop\n"
    "  vol <0-21>\n"
    "  resample on|off          48 kHz output (boards with a codec), or the stream's rate\n"
    "  core 0|1                 which CPU core decodes\n"
    "  debug on|off             show the decoder's own messages\n"
    "  status | help | reboot");
}

// Split into words, honouring "quoted names".
static std::vector<String> words(const String& s) {
  std::vector<String> out; String w; bool q = false;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"') q = !q;
    else if (c == ' ' && !q) { if (w.length()) out.push_back(w); w = ""; }
    else w += c;
  }
  if (w.length()) out.push_back(w);
  return out;
}

static void loadAndList(const String& src) {
  if (!net::connected()) { Serial.println("not connected to WiFi"); return; }
  Serial.printf("loading %s ...\n", src.c_str());
  if (directory::load(src)) printList(); else Serial.println("failed: " + directory::error);
}

static void run(const String& cmd) {
  std::vector<String> w = words(cmd);
  if (w.empty()) return;
  String op = w[0]; op.toLowerCase();
  String rest = cmd.substring(op.length()); rest.trim();

  if (op == "help" || op == "?") help();
  else if (op == "status") status();
  else if (op == "wifi") { if (w.size() >= 2) net::setCredentials(w[1], w.size() > 2 ? w[2] : ""); else Serial.println("wifi <ssid> <password>"); }
  else if (op == "screen") dumpScreen();
  else if (op == "ui") { if (w.size() > 1) display::preview(w[1]); else Serial.println("ui home|empty|list|about|phone|setup|busy|off"); }
  else if (op == "forget") net::forget();
  else if (op == "factory") net::factoryReset();
  else if (op == "popular" || op == "trending" || op == "favourites") loadAndList(op);
  else if (op == "fav") {
    const Station& c = player::current();
    if (c.url.isEmpty()) Serial.println("nothing playing");
    else Serial.println(favourites::toggle(c) ? "saved to favourites" : "removed from favourites");
  }
  else if (op == "country") { if (w.size() > 1) loadAndList("country:" + w[1]); else Serial.println("country <CC>"); }
  else if (op == "genre" || op == "tag") { if (rest.length()) loadAndList("tag:" + rest); else Serial.println("genre <name>"); }
  else if (op == "search") {
    if (!net::connected()) Serial.println("not connected to WiFi");
    else if (rest.isEmpty()) Serial.println("search <words>");
    else if (directory::search(rest)) printList(); else Serial.println("failed: " + directory::error);
  }
  else if (op == "list") printList();
  else if (op == "play") {
    if (rest.isEmpty()) player::togglePause();
    else if (rest.indexOf("://") < 0 && rest.toInt() >= 0 && isdigit(rest[0])) { if (!player::playIndex(rest.toInt())) Serial.println(player::lastError()); }
    else {
      String id = directory::stationIdFromLink(rest);
      Station s;
      if (id.length()) {
        if (!directory::byId(id, s)) { Serial.println("failed: " + directory::error); return; }
      } else { s.url = rest; s.name = rest; }
      if (!player::play(s)) Serial.println(player::lastError());
    }
  }
  else if (op == "next") player::next();
  else if (op == "prev") player::prev();
  else if (op == "pause") player::togglePause();
  else if (op == "stop") player::stop();
  else if (op == "vol" || op == "volume") { if (w.size() > 1) { player::setVolume(w[1].toInt()); Serial.printf("volume %d\n", player::volume()); } else Serial.printf("volume %d\n", player::volume()); }
  else if (op == "debug") { player::setDebug(w.size() > 1 && w[1] == "on"); }
  else if (op == "core") { if (w.size() > 1) player::setDecodeCore(w[1].toInt()); else Serial.println("core 0|1"); }
  else if (op == "resample") { if (w.size() > 1) player::setResample(w[1] == "on"); else Serial.println("resample on|off"); }
  else if (op == "reboot") ESP.restart();
  else Serial.println("unknown command; try help");
}

void console::begin() {
  Serial.println("type help for commands");
}

void console::loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') { String cmd = line; line = ""; cmd.trim(); if (cmd.length()) run(cmd); }
    else if (line.length() < 512) line += c;
  }
}
