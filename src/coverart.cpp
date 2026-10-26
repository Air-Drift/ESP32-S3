#include "config.h"
#if HAS_DISPLAY   // only the round screen shows covers (the custom build has no decoders)
#include "coverart.h"
#include "player.h"
#include "net.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <JPEGDEC.h>
#include <PNGdec.h>
#include <esp_heap_caps.h>
#include <new>
#include <vector>

// The cover is fetched at 250 px and decoded at half that: a disc of 125 px
// in the middle of the visualiser. Without a cover the station's own logo
// shows there, fitted inside the disc on its own background.
static const int MAX_SIDE = 128;
static const int ICON_FIT = 88;       // the logo's box: about the square inside the disc

static SemaphoreHandle_t lock;        // guards the fields below between the task and the loop
static uint16_t* art = nullptr;       // the shown cover, MAX_SIDE square
static int artSide = 0;
static String artFor;                 // the title the cover belongs to ("" for none yet)
static volatile bool busy = false;    // a lookup task is running
static TaskHandle_t task = nullptr;   // the last one, finished or not
static String asked;                  // the title last handed to a task
static uint16_t* icon = nullptr;      // the station's logo, MAX_SIDE square, composed
static uint16_t* iconNext = nullptr;  // the task composes here, then swaps it with icon
static String iconFor;                // the logo URL that icon shows ("" for none)
static String iconAsked;              // the logo URL last handed to a task
static unsigned long nextAsk = 0;     // MusicBrainz asks for one request a second at most

// ---- Text, as js/trackinfo.js compares it ----------------------------------
// Case, punctuation and a leading "The" gone. Letters beyond ASCII are kept as
// they come.
static String norm(const String& in) {
  String s = in; s.toLowerCase(); s.replace("&", " and ");
  String out; bool space = false;
  for (size_t i = 0; i < s.length(); i++) {
    unsigned char c = s[i];
    if (isalnum(c) || c >= 0x80) { if (space && out.length()) out += ' '; out += (char)c; space = false; }
    else space = true;
  }
  if (out.startsWith("the ")) out.remove(0, 4);
  return out;
}

static int findWord(const String& lower, const char* w) {   // " w " or " w." in a lowercased string
  String a = String(" ") + w + " ", b = String(" ") + w + ".";
  int i = lower.indexOf(a), j = lower.indexOf(b);
  return i < 0 ? j : j < 0 ? i : min(i, j);
}

// Without "(Radio Edit)", "[Explicit]", "- 2011 Remaster", "feat. Someone".
static String tidyTitle(String t) {
  for (int open; (open = max(t.indexOf('('), t.indexOf('['))) >= 0;) {
    int close = t.indexOf(t[open] == '(' ? ')' : ']', open);
    t.remove(open, close < 0 ? t.length() - open : close - open + 1);
  }
  String l = t; l.toLowerCase();
  int dash = l.indexOf(" - ");
  if (dash >= 0) {
    String tail = l.substring(dash);
    for (const char* w : {"remaster", "edit", "version", "mix", "mono", "stereo"})
      if (tail.indexOf(w) >= 0) { t.remove(dash); l.remove(dash); break; }
  }
  for (const char* w : {"feat", "ft", "featuring"}) { int i = findWord(l, w); if (i >= 0) { t.remove(i); l.remove(i); } }
  t.trim();
  return t;
}

// "Calvin Harris feat. Dua Lipa" -> "Calvin Harris".
static String leadArtist(String a) {
  String l = a; l.toLowerCase();
  for (const char* w : {"feat", "ft", "featuring", "with", "x", "vs"}) { int i = findWord(l, w); if (i >= 0) { a.remove(i); l.remove(i); } }
  for (char c : {',', '&', '/', ';'}) { int i = a.indexOf(c); if (i >= 0) a.remove(i); }
  a.trim();
  return a;
}

static String urlEncode(const String& s) {
  String out; char hex[4];
  for (size_t i = 0; i < s.length(); i++) {
    unsigned char c = s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
    else { snprintf(hex, sizeof hex, "%%%02X", c); out += hex; }
  }
  return out;
}

static String quoted(String s) { s.replace("\\", "\\\\"); s.replace("\"", "\\\""); return "\"" + s + "\""; }

// ---- Choosing, as js/trackinfo.js does -------------------------------------
struct PsramAllocator : ArduinoJson::Allocator {
  void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
  void  deallocate(void* p) override { heap_caps_free(p); }
  void* reallocate(void* p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
};
static PsramAllocator psram;

static String credit(JsonArrayConst c) {
  String s;
  for (JsonObjectConst o : c) { s += (const char*)(o["name"] | ""); s += (const char*)(o["joinphrase"] | ""); }
  return s;
}

static bool variant(String d) {
  d.toLowerCase();
  for (const char* w : {"live", "remix", "demo", "instrumental", "karaoke", "acoustic", "cover", "session", "rehearsal"})
    if (d.indexOf(w) >= 0) return true;
  return false;
}

// Lower is better: an album and nothing else, then a single, EP or soundtrack.
// An album from years after the song first came out ranks behind the single.
static int releaseRank(JsonObjectConst r, int firstYear) {
  if (String((const char*)(r["status"] | "")) != "Official") return 9;
  JsonObjectConst g = r["release-group"];
  JsonArrayConst extra = g["secondary-types"];
  String type = (const char*)(g["primary-type"] | "");
  if (type == "Album" && extra.size() == 0) {
    int year = String((const char*)(r["date"] | "")).substring(0, 4).toInt();
    return year && firstYear && year > firstYear + 1 ? 2 : 0;
  }
  if ((type == "Single" || type == "EP") && extra.size() == 0) return 1;
  if (extra.size() == 1 && String((const char*)extra[0]) == "Soundtrack") return 1;
  return 9;
}

// The release group of the song's original album, or "". Every recording that
// is clearly this song counts: search lists only a few releases per recording,
// so the site's "the recording on the most releases" can land on one that is
// only on compilations. Among all their releases: an album and nothing else,
// then a single, EP or soundtrack, the earliest first; never a compilation or
// a live album.
static String pick(JsonArrayConst recs, const String& artist, const String& title) {
  String nt = norm(title), ntt = norm(tidyTitle(title)), na = norm(artist), nl = norm(leadArtist(artist));
  std::vector<JsonObjectConst> fits;
  String first = "9999";
  for (JsonObjectConst r : recs) {
    if ((r["score"] | 0) < 60 || (r["video"] | false) || variant((const char*)(r["disambiguation"] | ""))) continue;
    String rt = (const char*)(r["title"] | "");
    if (norm(rt) != nt && norm(tidyTitle(rt)) != ntt) continue;
    JsonArrayConst c = r["artist-credit"];
    String full = norm(credit(c)), lead = norm((const char*)(c[0]["name"] | ""));
    if (full != na && lead != na && lead != nl) continue;
    fits.push_back(r);
    String f = (const char*)(r["first-release-date"] | "9999");
    if (f < first) first = f;
  }
  int firstYear = first.substring(0, 4).toInt();
  JsonObjectConst album; int rank = 9; String date;
  for (JsonObjectConst r : fits)
    for (JsonObjectConst rel : r["releases"].as<JsonArrayConst>()) {
      int k = releaseRank(rel, firstYear);
      String d = (const char*)(rel["date"] | "9999");
      if (k < rank || (k == rank && k < 9 && d < date)) { album = rel; rank = k; date = d; }
    }
  if (rank >= 9) return "";
  return (const char*)(album["release-group"]["id"] | "");
}

// ---- Fetching --------------------------------------------------------------
// Plain HTTP wherever the server allows it. This core's TLS takes its buffers
// from internal RAM only, and with an HTTPS station already holding one
// session there is room for one more handshake at most: kept for the last
// hop of the cover, which archive.org serves over HTTPS only. MusicBrainz and
// the Cover Art Archive's redirects answer over HTTP; all of it is public data.
static void setup(HTTPClient& http) {
  http.setUserAgent(AD_USER_AGENT);
  http.setTimeout(15000);
  http.useHTTP10(true);   // no chunking, so the body streams straight into the parser
}

static bool beginGet(HTTPClient& http, NetworkClient& client, const String& url, bool follow = false) {
  setup(http);
  http.setFollowRedirects(follow ? HTTPC_FORCE_FOLLOW_REDIRECTS : HTTPC_DISABLE_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code == HTTP_CODE_OK) return true;
  Serial.printf("[cover] HTTP %d (heap %u, largest %u) for %s\n", code, ESP.getFreeHeap(), ESP.getMaxAllocHeap(), url.c_str());
  http.end();
  return false;
}

// Follows redirects over plain HTTP until one leads to HTTPS (or the file).
static String resolve(String url) {
  static const char* keep[] = { "Location" };
  for (int hop = 0; hop < 5 && url.startsWith("http://"); hop++) {
    NetworkClient plain; HTTPClient http;
    setup(http);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.collectHeaders(keep, 1);
    if (!http.begin(plain, url)) return "";
    int code = http.sendRequest("HEAD");
    String next = http.header("Location");
    http.end();
    if (code == HTTP_CODE_OK) return url;
    if (code == 404 || code == 410) { Serial.printf("[cover] HTTP %d for %s\n", code, url.c_str()); return ""; }
    if (code < 300 || code >= 400 || next.isEmpty()) return url;   // some servers refuse HEAD: let the GET try
    url = next;
  }
  return url;
}

static String findGroup(const String& artist, const String& title) {
  String q = "recording:" + quoted(tidyTitle(title)) + " AND artist:" + quoted(leadArtist(artist)) +
             " AND status:official AND NOT comment:live";
  NetworkClient plain; HTTPClient http;
  String url = "http://musicbrainz.org/ws/2/recording?fmt=json&limit=25&query=" + urlEncode(q);
  // Busy (503, which is also how MusicBrainz says "slower"): wait 2, 4, 8
  // seconds and ask again, as the site does.
  for (int attempt = 0; !beginGet(http, plain, url); attempt++) {
    if (attempt >= 3) return "";
    delay(2000 << attempt);
  }
  JsonDocument filter(&psram);
  JsonObject f = filter["recordings"].add<JsonObject>();
  for (const char* k : {"score", "video", "disambiguation", "title", "first-release-date"}) f[k] = true;
  JsonObject ac = f["artist-credit"].add<JsonObject>(); ac["name"] = true; ac["joinphrase"] = true;
  JsonObject rel = f["releases"].add<JsonObject>(); rel["status"] = true; rel["date"] = true;
  JsonObject g = rel["release-group"].to<JsonObject>(); g["id"] = true; g["primary-type"] = true; g["secondary-types"] = true;
  JsonDocument doc(&psram);
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) { Serial.printf("[cover] MusicBrainz answer unreadable: %s\n", err.c_str()); return ""; }
  return pick(doc["recordings"].as<JsonArrayConst>(), artist, title);
}

static uint16_t* decoded = nullptr;   // the task decodes here, then swaps it with art
static int decodedSide = 0;

// A body into PSRAM (the caller frees it), over HTTP where it can be. Its
// length may be unknown: then it is read until the server closes, up to cap.
static uint8_t* fetchBytes(const String& from, int& got, int cap) {
  got = 0;
  String url = resolve(from);
  if (url.isEmpty()) return nullptr;
  NetworkClient plain; WiFiClientSecure tls; HTTPClient http;
  tls.setInsecure();   // public images, as the directory's data
  bool secure = url.startsWith("https://");
  if (!beginGet(http, secure ? (NetworkClient&)tls : plain, url, secure)) {
    char why[100] = ""; if (secure) tls.lastError(why, sizeof why);
    if (*why) Serial.printf("[cover] TLS: %s\n", why);
    return nullptr;
  }
  int len = http.getSize();
  if (len > cap) { http.end(); return nullptr; }
  int size = len > 0 ? len : cap;
  uint8_t* buf = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  if (!buf) { http.end(); return nullptr; }
  NetworkClient* s = http.getStreamPtr();
  unsigned long t0 = millis();
  while (got < size && millis() - t0 < 15000) {
    int n = s->available() ? s->read(buf + got, size - got) : 0;
    if (n > 0) got += n;
    else if (len <= 0 && !s->connected()) break;
    else delay(5);
  }
  http.end();
  if (!got || (len > 0 && got != len)) { heap_caps_free(buf); return nullptr; }
  return buf;
}

// ---- Decoding: JPEG or PNG, whole, into PSRAM ------------------------------
struct Image { uint16_t* px = nullptr; int w = 0, h = 0; };
static Image* into = nullptr;          // where the decoders' callbacks write

static int onJpeg(JPEGDRAW* d) {
  for (int y = 0; y < d->iHeight && d->y + y < into->h; y++)
    for (int x = 0; x < d->iWidth && d->x + x < into->w; x++)
      into->px[(d->y + y) * into->w + d->x + x] = d->pPixels[y * d->iWidth + x];
  return 1;
}

static int onPng(PNGDRAW* d) {
  if (d->y < into->h) ((PNG*)d->pUser)->getLineAsRGB565(d, into->px + d->y * into->w, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);   // see-through onto white
  return 1;
}

// JPEG is decoded at 1/2, 1/4 or 1/8 while that still leaves `least` pixels
// on its longer side; PNG at full size. The decoders are large: in PSRAM, and
// only while they work. Internal RAM is what TLS needs.
static bool decodeImage(uint8_t* data, int len, int least, Image& out) {
  bool jpg = len > 3 && data[0] == 0xFF && data[1] == 0xD8, png = len > 8 && data[0] == 0x89 && data[1] == 'P';
  if (!jpg && !png) return false;   // ICO, SVG, WebP, GIF: the logo stays ours
  void* mem = heap_caps_malloc(jpg ? sizeof(JPEGDEC) : sizeof(PNG), MALLOC_CAP_SPIRAM);
  if (!mem) return false;
  bool ok = false;
  into = &out;
  if (jpg) {
    JPEGDEC& jpeg = *new (mem) JPEGDEC();
    if (jpeg.openRAM(data, len, onJpeg)) {
      int w = jpeg.getWidth(), h = jpeg.getHeight(), div = 1;
      while (div < 8 && max(w, h) / (div * 2) >= least) div *= 2;
      out.w = (w + div - 1) / div; out.h = (h + div - 1) / div;
      out.px = (uint16_t*)heap_caps_calloc(out.w * out.h, 2, MALLOC_CAP_SPIRAM);
      if (out.px) {
        jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
        ok = jpeg.decode(0, 0, div == 8 ? JPEG_SCALE_EIGHTH : div == 4 ? JPEG_SCALE_QUARTER : div == 2 ? JPEG_SCALE_HALF : 0) == 1;
        if (!ok) Serial.printf("[cover] could not decode (%d)\n", jpeg.getLastError());
      }
      jpeg.close();
    }
    jpeg.~JPEGDEC();
  } else {
    PNG& p = *new (mem) PNG();
    if (p.openRAM(data, len, onPng) == PNG_SUCCESS && p.getWidth() * p.getHeight() <= 1600 * 1600) {
      out.w = p.getWidth(); out.h = p.getHeight();
      out.px = (uint16_t*)heap_caps_calloc(out.w * out.h, 2, MALLOC_CAP_SPIRAM);
      if (out.px) ok = p.decode(&p, 0) == PNG_SUCCESS;
      p.close();
    }
    p.~PNG();
  }
  heap_caps_free(mem);
  into = nullptr;
  if (!ok && out.px) { heap_caps_free(out.px); out.px = nullptr; }
  return ok;
}

// RGB565 in this framebuffer's byte order is plain little-endian: average it
// channel by channel.
static uint16_t average(const Image& im, int x0, int y0, int x1, int y1) {
  uint32_t r = 0, g = 0, b = 0, n = 0;
  for (int y = y0; y < y1; y++)
    for (int x = x0; x < x1; x++) { uint16_t c = im.px[y * im.w + x]; r += c >> 11; g += (c >> 5) & 63; b += c & 31; n++; }
  if (!n) return 0;
  return (uint16_t)((r / n) << 11 | (g / n) << 5 | (b / n));
}

static bool fetchCover(const String& group) {
  int len = 0;
  uint8_t* data = fetchBytes("http://coverartarchive.org/release-group/" + group + "/front-250", len, 400000);
  if (!data) return false;
  Image im;
  bool ok = decodeImage(data, len, MAX_SIDE, im);
  heap_caps_free(data);
  if (!ok) return false;
  // The middle square of it, MAX_SIDE at most.
  decodedSide = min(MAX_SIDE, min(im.w, im.h));
  int ox = (im.w - decodedSide) / 2, oy = (im.h - decodedSide) / 2;
  for (int y = 0; y < decodedSide; y++)
    memcpy(decoded + y * MAX_SIDE, im.px + (oy + y) * im.w + ox, decodedSide * 2);
  heap_caps_free(im.px);
  return true;
}

// The station's logo, whole, inside ICON_FIT, on the colour of its own corner
// (white where it is see-through), so a wide logo is not cut by the disc.
// It comes through wsrv.nl, an open image proxy: over plain HTTP (a TLS
// handshake rarely fits beside an HTTPS stream), as a JPEG whatever the
// station serves (PNG, ICO, SVG, WebP), already the size of the disc's box.
// Straight from the station if the proxy has nothing.
static bool fetchIcon(const String& url) {
  int len = 0;
  uint8_t* data = fetchBytes("http://wsrv.nl/?url=" + urlEncode(url) + "&w=" + ICON_FIT + "&h=" + ICON_FIT + "&fit=inside&bg=white&output=jpg", len, 200000);
  if (!data) data = fetchBytes(url, len, 1500000);
  if (!data) return false;
  Image im;
  bool ok = decodeImage(data, len, ICON_FIT, im);
  heap_caps_free(data);
  if (!ok) return false;
  uint16_t bg = im.px[0];
  for (int i = 0; i < MAX_SIDE * MAX_SIDE; i++) iconNext[i] = bg;
  float k = min((float)ICON_FIT / im.w, (float)ICON_FIT / im.h);
  int tw = max(1, (int)lroundf(im.w * k)), th = max(1, (int)lroundf(im.h * k));
  int ox = (MAX_SIDE - tw) / 2, oy = (MAX_SIDE - th) / 2;
  for (int y = 0; y < th; y++) {
    int y0 = (int)(y / k), y1 = max(y0 + 1, min(im.h, (int)((y + 1) / k)));
    for (int x = 0; x < tw; x++) {
      int x0 = (int)(x / k), x1 = max(x0 + 1, min(im.w, (int)((x + 1) / k)));
      iconNext[(oy + y) * MAX_SIDE + ox + x] = average(im, min(x0, im.w - 1), min(y0, im.h - 1), x1, y1);
    }
  }
  heap_caps_free(im.px);
  return true;
}

// ---- The song in a stream title, as js/nowplaying.js reads it --------------
// Stations tag their own titles after a pipe or a dash ("Daisy by André
// Previn - Classic Vinyl on walmradio.com"): a segment that is a bare domain
// or names the station goes. Then "Artist - Title", or failing that "Title by
// Artist" (a lowercase "by", the last one).
static const char* GENERIC[] = { "radio", "fm", "am", "hd", "the", "live", "stream", "streaming", "online",
                                 "music", "hits", "net", "com", "digital", "channel", "station" };
static bool generic(const String& w) { for (const char* g : GENERIC) if (w == g) return true; return false; }

static std::vector<String> wordsOf(const String& s) {
  std::vector<String> out; String w;
  for (size_t i = 0; i <= s.length(); i++) {
    unsigned char c = i < s.length() ? s[i] : ' ';
    if (isalnum(c) || c >= 0x80) w += (char)tolower(c);
    else if (w.length()) { out.push_back(w); w = ""; }
  }
  return out;
}

static bool namesStation(const String& word, const std::vector<String>& tokens) {
  for (const String& t : tokens)
    if (word == t || (word.startsWith(t) && generic(word.substring(t.length())))) return true;
  return false;
}

// A bare domain in a short segment: the station signing its work.
static bool signature(const String& part) {
  static const char* TLD[] = { "com", "net", "org", "fm", "am", "io", "co", "tv", "radio", "live", "app", "de", "ru", "it", "es",
                               "fr", "nl", "pl", "uk", "us", "ca", "au", "hu", "gr", "se", "no", "fi", "dk", "cz", "ro", "tr", "br", "mx", "in", "jp" };
  int count = 0; bool domain = false;
  String p = part + " ";
  for (int i = 0, j; (j = p.indexOf(' ', i)) >= 0; i = j + 1) {
    String w = p.substring(i, j); if (w.isEmpty()) continue;
    count++;
    w.toLowerCase();
    int dot = w.lastIndexOf('.');
    if (dot > 0) { String tld = w.substring(dot + 1); for (const char* t : TLD) if (tld == t) domain = true; }
  }
  return domain && count <= 4;
}

static bool songOf(String text, const String& station, String& artist, String& title) {
  text.replace(" \xE2\x80\x93 ", " - "); text.replace(" \xE2\x80\x94 ", " - ");   // en and em dashes
  text.replace("|", " - "); text.replace("\xC2\xB7", " - ");
  while (text.indexOf("  ") >= 0) text.replace("  ", " ");
  text.replace("- -", "-");
  std::vector<String> tokens;
  for (const String& w : wordsOf(station)) if (w.length() >= 4 && !generic(w)) tokens.push_back(w);
  std::vector<String> parts, kept;
  String t = text + " - ";
  for (int i = 0, j; (j = t.indexOf(" - ", i)) >= 0; i = j + 3) { String p = t.substring(i, j); p.trim(); parts.push_back(p); }
  for (const String& p : parts) {
    bool named = false;
    for (const String& w : wordsOf(p)) if (namesStation(w, tokens)) named = true;
    if (p.length() && !signature(p) && !named) kept.push_back(p);
  }
  if (parts.size() > 1 && kept.size() && kept.size() < parts.size()) {
    text = ""; for (size_t i = 0; i < kept.size(); i++) text += (i ? " - " : "") + kept[i];
  }
  int dash = text.indexOf(" - ");
  if (dash > 0) { artist = text.substring(0, dash); title = text.substring(dash + 3); }
  else {
    int by = text.lastIndexOf(" by ");
    if (by <= 0) return false;
    title = text.substring(0, by); artist = text.substring(by + 4);
  }
  artist.trim(); title.trim();
  return artist.length() && title.length();
}

static void iconTask(void* arg) {
  String* given = (String*)arg;              // the logo's URL
  String url = *given; delete given;
  bool found = fetchIcon(url);
  Serial.printf("[cover] station logo %s: %s\n", url.c_str(), found ? "shown" : "not usable");
  xSemaphoreTake(lock, portMAX_DELAY);
  if (found) { std::swap(icon, iconNext); iconFor = url; } else iconFor = "";
  xSemaphoreGive(lock);
  busy = false;
  vTaskSuspend(nullptr);
}

static void lookupTask(void* arg) {
  String* given = (String*)arg;              // "station\ntitle"
  int nl = given->indexOf('\n');
  String station = given->substring(0, nl), song = given->substring(nl + 1);
  delete given;
  String artist, name;
  bool found = false;
  if (songOf(song, station, artist, name)) {
    String group = findGroup(artist, name);
    if (group.length()) found = fetchCover(group);
    Serial.printf("[cover] %s: %s\n", song.c_str(), found ? "cover found" : group.length() ? "no cover" : "no clear match");
  }
  xSemaphoreTake(lock, portMAX_DELAY);
  if (found) { std::swap(art, decoded); artSide = decodedSide; } else artSide = 0;
  artFor = song;
  xSemaphoreGive(lock);
  busy = false;
  vTaskSuspend(nullptr);   // the loop deletes it: a stack made WithCaps is freed from outside
}

void coverart::loop() {
  if (!lock) {
    lock = xSemaphoreCreateMutex();
    art = (uint16_t*)heap_caps_calloc(MAX_SIDE * MAX_SIDE, 2, MALLOC_CAP_SPIRAM);
    decoded = (uint16_t*)heap_caps_calloc(MAX_SIDE * MAX_SIDE, 2, MALLOC_CAP_SPIRAM);
    icon = (uint16_t*)heap_caps_calloc(MAX_SIDE * MAX_SIDE, 2, MALLOC_CAP_SPIRAM);
    iconNext = (uint16_t*)heap_caps_calloc(MAX_SIDE * MAX_SIDE, 2, MALLOC_CAP_SPIRAM);
  }
  if (!busy && task) { vTaskDeleteWithCaps(task); task = nullptr; }
  if (busy || !art || !decoded || !icon || !iconNext || !net::connected() || millis() < nextAsk) return;
  if (!player::running() || player::paused()) return;
  // A new station: its logo first, once.
  const String& logo = player::current().icon;
  if (logo != iconAsked) {
    iconAsked = logo;
    if (logo.startsWith("http")) {
      busy = true;
      if (xTaskCreatePinnedToCoreWithCaps(iconTask, "logo", 12288, new String(logo), 1, &task, tskNO_AFFINITY, MALLOC_CAP_SPIRAM) != pdPASS) { busy = false; task = nullptr; }
      return;
    }
  }
  String t = player::title();
  if (t.isEmpty() || t == asked) return;
  asked = t; nextAsk = millis() + 1100;
  busy = true;
  // Its stack in PSRAM: internal RAM is kept for the TLS handshake.
  if (xTaskCreatePinnedToCoreWithCaps(lookupTask, "cover", 12288, new String(player::current().name + "\n" + t), 1, &task, tskNO_AFFINITY, MALLOC_CAP_SPIRAM) != pdPASS) { busy = false; task = nullptr; }
}

const uint16_t* coverart::image(int& side) {
  if (!lock) return nullptr;
  xSemaphoreTake(lock, portMAX_DELAY);
  // The song's cover, else the station's logo, else none (the display draws ours).
  if (artSide && artFor == player::title()) { side = artSide; return art; }
  if (iconFor.length() && iconFor == player::current().icon) { side = MAX_SIDE; return icon; }
  xSemaphoreGive(lock);
  return nullptr;
}

void coverart::release() { xSemaphoreGive(lock); }
#endif
