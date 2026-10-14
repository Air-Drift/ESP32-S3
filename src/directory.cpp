#include "directory.h"
#include "config.h"
#include "favourites.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

String               directory::source;
std::vector<Station> directory::stations;
String               directory::error;

// Radio Browser mirrors, tried in turn. The pool is volunteer-run.
static const char* MIRRORS[] = { "de1.api.radio-browser.info", "fi1.api.radio-browser.info", "de2.api.radio-browser.info" };

// JSON documents go to PSRAM; internal RAM is for WiFi, TLS and the decoder.
struct PsramAllocator : ArduinoJson::Allocator {
  void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
  void  deallocate(void* p) override { heap_caps_free(p); }
  void* reallocate(void* p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
};
static PsramAllocator psram;

// GET a URL into a String. The site's files are not chunked, but Radio
// Browser's answers can be, and getString() handles both.
static bool fetch(const String& url, String& body, int* status = nullptr) {
  WiFiClientSecure secure;
  WiFiClient plain;
  secure.setInsecure();  // the directory is public data; no CA bundle to keep current
  HTTPClient http;
  http.setUserAgent(AD_USER_AGENT);
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setReuse(false);
  bool ok = url.startsWith("https://") ? http.begin(secure, url) : http.begin(plain, url);
  if (!ok) { directory::error = "bad url"; return false; }
  int code = http.GET();
  if (status) *status = code;
  if (code != HTTP_CODE_OK) {
    directory::error = String("HTTP ") + code + " for " + url;
    http.end();
    return false;
  }
  body = http.getString();
  http.end();
  return body.length() > 0;
}

static void fieldsFilter(JsonObject f) {
  f["id"] = true; f["name"] = true; f["url"] = true; f["codec"] = true;
  f["bitrate"] = true; f["country"] = true; f["hls"] = true; f["icon"] = true;
}

static void take(JsonArray arr) {
  directory::stations.clear();
  for (JsonObject o : arr) {
    if (directory::stations.size() >= MAX_STATIONS) break;
    const char* url = o["url"];
    if (!url || !*url) continue;
    Station s;
    s.id      = (const char*)(o["id"] | "");
    s.name    = (const char*)(o["name"] | "");
    s.url     = url;
    s.codec   = (const char*)(o["codec"] | "");
    s.country = (const char*)(o["country"] | "");
    s.icon    = (const char*)(o["icon"] | "");
    s.bitrate = o["bitrate"] | 0;
    s.hls     = (o["hls"] | 0) == 1 || s.url.endsWith(".m3u8");
    directory::stations.push_back(s);
  }
}

// Site files: popular/trending are bare arrays; .head.json wraps {total, httpsTotal, stations:[...]}.
static bool loadSiteFile(const String& path, bool wrapped) {
  String body;
  if (!fetch(String(AD_SITE) + "/data/v1/" + path, body)) return false;
  JsonDocument filter(&psram);
  fieldsFilter(wrapped ? filter["stations"].add<JsonObject>() : filter.add<JsonObject>());
  JsonDocument doc(&psram);
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  body = String();
  if (err) { directory::error = String("JSON: ") + err.c_str(); return false; }
  take(wrapped ? doc["stations"].as<JsonArray>() : doc.as<JsonArray>());
  return true;
}

// Genre files carry a hash in their name ("tag/jazz-1abc2de"); index.json maps names to files.
static String tagFile(const String& tag) {
  String body;
  if (!fetch(String(AD_SITE) + "/data/v1/index.json", body)) return "";
  JsonDocument filter(&psram);
  JsonObject g = filter["genres"].add<JsonObject>();
  g["value"] = true; g["file"] = true;
  JsonDocument doc(&psram);
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) { directory::error = "JSON: index"; return ""; }
  String want = tag; want.toLowerCase();
  for (JsonObject o : doc["genres"].as<JsonArray>()) {
    String v = (const char*)(o["value"] | "");
    if (v == want) return (const char*)(o["file"] | "");
  }
  directory::error = "no genre called \"" + tag + "\" in the directory";
  return "";
}

bool directory::load(const String& src) {
  error = "";
  bool ok = false;
  if (src == "favourites") {
    stations = favourites::list();
    ok = true;
    if (stations.empty()) error = "no favourites yet";
  } else if (src == "popular" || src == "trending") {
    ok = loadSiteFile(src + ".json", false);
  } else if (src.startsWith("country:")) {
    String cc = src.substring(8); cc.toUpperCase();
    ok = loadSiteFile("country/" + cc + ".head.json", true);
    if (!ok && error.startsWith("HTTP 404")) error = "no country \"" + cc + "\" (use a two-letter code like US, DE, FR)";
  } else if (src.startsWith("tag:")) {
    String f = tagFile(src.substring(4));
    ok = f.length() && loadSiteFile(f + ".head.json", true);
  } else {
    error = "unknown list \"" + src + "\"";
  }
  if (ok) source = src;
  else if (error.isEmpty()) error = "could not load " + src;
  return ok;
}

// Radio Browser's search answer has different field names from the site's files.
static bool takeRadioBrowser(const String& body) {
  JsonDocument filter(&psram);
  JsonObject f = filter.add<JsonObject>();
  f["stationuuid"] = true; f["name"] = true; f["url_resolved"] = true; f["url"] = true;
  f["codec"] = true; f["bitrate"] = true; f["countrycode"] = true; f["favicon"] = true; f["hls"] = true;
  JsonDocument doc(&psram);
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) { directory::error = "JSON: search"; return false; }
  directory::stations.clear();
  for (JsonObject o : doc.as<JsonArray>()) {
    if (directory::stations.size() >= MAX_STATIONS) break;
    Station s;
    s.id      = (const char*)(o["stationuuid"] | "");
    s.name    = (const char*)(o["name"] | "");
    s.url     = (const char*)(o["url_resolved"] | (const char*)(o["url"] | ""));
    s.codec   = (const char*)(o["codec"] | "");
    s.country = (const char*)(o["countrycode"] | "");
    s.icon    = (const char*)(o["favicon"] | "");
    s.bitrate = o["bitrate"] | 0;
    s.hls     = (o["hls"] | 0) == 1 || s.url.endsWith(".m3u8");
    if (s.url.length()) directory::stations.push_back(s);
  }
  return true;
}

static String urlencode(const String& s) {
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') out += c;
    else { char b[4]; snprintf(b, sizeof b, "%%%02X", (unsigned char)c); out += b; }
  }
  return out;
}

static bool radioBrowser(const String& pathAndQuery, String& body) {
  for (const char* host : MIRRORS) {
    if (fetch(String("https://") + host + pathAndQuery, body)) return true;
  }
  return false;
}

bool directory::search(const String& term) {
  error = "";
  String body;
  if (!radioBrowser("/json/stations/search?name=" + urlencode(term) + "&limit=" + MAX_STATIONS + "&hidebroken=true&order=clickcount&reverse=true", body)) return false;
  if (!takeRadioBrowser(body)) return false;
  source = "search:" + term;
  if (stations.empty()) error = "nothing called \"" + term + "\"";
  return !stations.empty();
}

bool directory::byId(const String& id, Station& out) {
  error = "";
  String body;
  if (!radioBrowser("/json/stations/byuuid/" + urlencode(id), body)) return false;
  std::vector<Station> keep; keep.swap(stations);
  String keepSource = source;
  bool ok = takeRadioBrowser(body) && !stations.empty();
  if (ok) out = stations[0]; else error = "no station with id " + id;
  stations.swap(keep); source = keepSource;
  return ok;
}

bool directory::reportPlay(const String& id) {
  if (!REPORT_PLAYS || id.isEmpty()) return false;
  String body;
  return radioBrowser("/json/url/" + urlencode(id), body);
}

// "https://airdrift.stream/listen/<uuid>", ".../#/listen/<uuid>" or a bare uuid.
String directory::stationIdFromLink(const String& text) {
  String t = text; t.trim();
  int at = t.indexOf("listen/");
  if (at >= 0) t = t.substring(at + 7);
  int end = 0;
  while (end < (int)t.length() && (isxdigit((unsigned char)t[end]) || t[end] == '-')) end++;
  t = t.substring(0, end);
  return t.length() == 36 ? t : String();
}
