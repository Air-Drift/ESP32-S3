#include "favourites.h"
#include <ArduinoJson.h>
#include <LittleFS.h>

static std::vector<Station> favs;
static const char* FILE_PATH = "/favourites.json";
static const size_t MAX_FAVS = 100;
static bool fsUp = false;

static void save() {
  if (!fsUp) return;
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (const Station& s : favs) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = s.id; o["name"] = s.name; o["url"] = s.url; o["codec"] = s.codec;
    o["country"] = s.country; o["icon"] = s.icon; o["bitrate"] = s.bitrate; o["hls"] = s.hls;
  }
  File f = LittleFS.open(FILE_PATH, "w");
  if (!f) { Serial.println("[favourites] could not write"); return; }
  serializeJson(doc, f);
  f.close();
}

void favourites::clear() {
  favs.clear();
  if (fsUp) LittleFS.remove(FILE_PATH);
}

void favourites::begin() {
  fsUp = LittleFS.begin(true);
  if (!fsUp) { Serial.println("[favourites] no filesystem; favourites will not persist"); return; }
  File f = LittleFS.open(FILE_PATH, "r");
  if (!f) return;
  JsonDocument doc;
  if (deserializeJson(doc, f) == DeserializationError::Ok) {
    for (JsonObject o : doc.as<JsonArray>()) {
      Station s;
      s.id = (const char*)(o["id"] | ""); s.name = (const char*)(o["name"] | ""); s.url = (const char*)(o["url"] | "");
      s.codec = (const char*)(o["codec"] | ""); s.country = (const char*)(o["country"] | ""); s.icon = (const char*)(o["icon"] | "");
      s.bitrate = o["bitrate"] | 0; s.hls = o["hls"] | false;
      if (s.url.length()) favs.push_back(s);
    }
  }
  f.close();
  Serial.printf("[favourites] %d saved\n", (int)favs.size());
}

const std::vector<Station>& favourites::list() { return favs; }

static int indexOf(const String& id) {
  for (size_t i = 0; i < favs.size(); i++) if (favs[i].id == id) return i;
  return -1;
}

bool favourites::has(const String& id) { return id.length() && indexOf(id) >= 0; }

bool favourites::add(const Station& s) {
  if (s.url.isEmpty() || favs.size() >= MAX_FAVS) return false;
  String key = s.id.length() ? s.id : s.url;
  if (indexOf(key) >= 0) return false;
  Station c = s;
  if (c.id.isEmpty()) c.id = c.url;   // a pasted stream URL has no id; the URL stands in
  favs.insert(favs.begin(), c);       // newest first
  save();
  return true;
}

bool favourites::remove(const String& id) {
  int i = indexOf(id);
  if (i < 0) return false;
  favs.erase(favs.begin() + i);
  save();
  return true;
}

bool favourites::toggle(const Station& s) {
  String key = s.id.length() ? s.id : s.url;
  if (has(key)) { remove(key); return false; }
  add(s);
  return true;
}
