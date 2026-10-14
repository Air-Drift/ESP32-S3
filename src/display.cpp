#include "config.h"
#include "display.h"

#if HAS_DISPLAY

// The round screen on the Waveshare ESP32-S3-Touch-LCD-1.85 / 1.85C.
//
// The panel is driven the way Waveshare's own demo drives it: ESP-IDF's
// esp_lcd SPI panel IO in quad mode, commands as 0x02 00 cc 00, pixels as
// 0x32 00 2C 00 followed by the whole frame in one DMA transfer. Drawing
// happens in a framebuffer (Arduino_GFX's canvas, for its fonts and shapes),
// and the frame is pushed whenever something changed.
#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <U8g2lib.h>
#include <qrcode.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_heap_caps.h>
#include "boards/waveshare185_init.h"
#include "net.h"
#include "directory.h"
#include "player.h"
#include "favourites.h"
#include <WiFi.h>

static const int C = LCD_SIZE / 2;  // centre

static Arduino_Canvas* gfx = new Arduino_Canvas(LCD_SIZE, LCD_SIZE, nullptr);
static esp_lcd_panel_io_handle_t io = nullptr;
// The SPI driver DMAs from internal RAM only, so the frame goes out in row
// chunks through two small internal buffers, swapped to the panel's byte order.
static const int CHUNK_ROWS = 20;
static const size_t CHUNK_BYTES = LCD_SIZE * CHUNK_ROWS * 2;
static uint16_t* chunk[2] = {nullptr, nullptr};
static volatile int inFlight = 0;

// Air Drift's dark palette (css/app.css in the site), as RGB565.
static const uint16_t BG = 0x0841;       // surface-000 #08090a
static const uint16_t FG = 0xEF9E;       // ink-100     #eef2f5
static const uint16_t INK2 = 0xB5F8;     // ink-200     #b3bcc4
static const uint16_t DIM = 0x8CB4;      // ink-300     #8b97a2
static const uint16_t ACCENT = 0x46D1;   // green-500   #46d98a
static const uint16_t ON_GREEN = 0x00A1; // on-green    #04150c
static const uint16_t RING = 0x2145;     // line-100    #242a2f

// The logo: the green tile with five signal bars, drawn from the site's
// icon.svg (tile radius 28%, bars on a 24-unit grid across 72% of the tile).
static void logoTile(int x, int y, int size) {
  gfx->fillRoundRect(x, y, size, size, size * 28 / 100, ACCENT);
  float u = size * 0.72f / 24.0f, off = size * 0.14f;
  const int heights[5] = {4, 10, 18, 10, 4};
  for (int i = 0; i < 5; i++) {
    float cx = x + off + u * (4 + 4 * i), h = heights[i] * u, w = 2.8f * u;
    gfx->fillRoundRect((int)(cx - w / 2), (int)(y + off + u * 12 - h / 2 - w / 2), (int)w, (int)(h + w), (int)(w / 2), ON_GREEN);
  }
}

static void addHit(int x, int y, int w, int h, const String& id);

// The lockup: tile, then "airdrift" with "drift" in green, centred at y.
// Tapping it always leads home.
static void brand(int cy, int tile, int cx = C) {
  int16_t x1, y1; uint16_t w1, w2, h;
  gfx->setFont(u8g2_font_helvB14_tf);
  gfx->getTextBounds("air", 0, 0, &x1, &y1, &w1, &h);
  gfx->getTextBounds("drift", 0, 0, &x1, &y1, &w2, &h);
  int gap = tile / 3, total = tile + gap + w1 + w2;
  int x = cx - total / 2;
  addHit(x - 12, cy - tile / 2 - 12, total + 24, tile + 24, "home");   // the logo always leads home
  logoTile(x, cy - tile / 2, tile);
  int ty = cy + h / 2 - 1;
  gfx->setCursor(x + tile + gap, ty); gfx->setTextColor(FG); gfx->print("air");
  gfx->setCursor(x + tile + gap + w1, ty); gfx->setTextColor(ACCENT); gfx->print("drift");
}

// ---- TCA9554 expander: the panel's and touch's reset lines live on it. ----
static void tcaWrite(uint8_t reg, uint8_t v) {
  Wire.beginTransmission(TCA9554_ADDR);
  Wire.write(reg); Wire.write(v);
  Wire.endTransmission();
}

static void resetPanelAndTouch() {
  uint8_t bits = (1 << EXIO_TP_RST) | (1 << EXIO_LCD_RST);
  tcaWrite(0x03, (uint8_t)~bits);  // those two as outputs, the rest inputs
  tcaWrite(0x01, 0x00);
  delay(20);
  tcaWrite(0x01, bits);
  delay(120);
}

static void scanBus() {
  String found;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) found += String(" 0x") + String(a, HEX);
  }
  Serial.printf("[display] system I2C:%s\n", found.c_str());
}

// ---- Panel ------------------------------------------------------------------
static bool IRAM_ATTR onFrameDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void*) {
  inFlight--;
  return false;
}

static void panelCmd(uint8_t cmd, const uint8_t* data, size_t len) {
  esp_lcd_panel_io_tx_param(io, (0x02UL << 24) | ((uint32_t)cmd << 8), data, len);
}

static bool panelBegin() {
  spi_bus_config_t bus = {};
  bus.sclk_io_num = PIN_LCD_SCK;
  bus.data0_io_num = PIN_LCD_D0; bus.data1_io_num = PIN_LCD_D1;
  bus.data2_io_num = PIN_LCD_D2; bus.data3_io_num = PIN_LCD_D3;
  bus.max_transfer_sz = CHUNK_BYTES;
  bus.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD;
  if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) { Serial.println("[display] SPI bus failed"); return false; }

  esp_lcd_panel_io_spi_config_t cfg = {};
  cfg.cs_gpio_num = PIN_LCD_CS;
  cfg.dc_gpio_num = -1;
  cfg.spi_mode = 0;
  cfg.pclk_hz = 40 * 1000 * 1000;
  cfg.trans_queue_depth = 4;
  cfg.on_color_trans_done = onFrameDone;
  cfg.lcd_cmd_bits = 32;
  cfg.lcd_param_bits = 8;
  cfg.flags.quad_mode = 1;
  if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &cfg, &io) != ESP_OK) { Serial.println("[display] panel IO failed"); return false; }

  for (const PanelCmd& c : waveshare185_st77916_init) {
    panelCmd(c.cmd, c.len ? c.data : nullptr, c.len);
    if (c.delayMs) delay(c.delayMs);
  }
  return true;
}

static void waitFor(int pending) {
  unsigned long t0 = millis();
  while (inFlight > pending && millis() - t0 < 200) delay(1);
}

static void pushFrame() {
  const uint16_t* src = gfx->getFramebuffer();
  uint8_t col[4] = {0, 0, (uint8_t)((LCD_SIZE - 1) >> 8), (uint8_t)((LCD_SIZE - 1) & 0xFF)};
  waitFor(0);
  panelCmd(0x2A, col, 4);
  panelCmd(0x2B, col, 4);
  int b = 0;
  for (int row = 0; row < LCD_SIZE; row += CHUNK_ROWS, b ^= 1) {
    waitFor(1);  // this buffer's previous transfer must be done
    uint16_t* dst = chunk[b];
    const uint16_t* s = src + (size_t)row * LCD_SIZE;
    for (size_t i = 0; i < (size_t)LCD_SIZE * CHUNK_ROWS; i++) dst[i] = __builtin_bswap16(s[i]);
    inFlight++;
    // 2C starts a write at the window origin; 3C continues from where it left off.
    esp_lcd_panel_io_tx_color(io, (0x32UL << 24) | ((row ? 0x3CUL : 0x2CUL) << 8), dst, CHUNK_BYTES);
  }
}

// ---- CST816 touch -----------------------------------------------------
// On the 1.85C the touch shares the system bus; on the 1.85 it has its own
// (GPIO 1/3). It only answers reliably while its INT line reports activity,
// so reads are gated on that pin.
static TwoWire tpOwn(1);
static TwoWire* tp = nullptr;

static bool touchPresent(TwoWire& w) {
  w.beginTransmission(CST816_ADDR);
  return w.endTransmission() == 0;
}

static bool touchRead(uint8_t& fingers, int& x, int& y) {
  tp->beginTransmission(CST816_ADDR);
  tp->write(0x02);
  if (tp->endTransmission(true) != 0 || tp->requestFrom((uint8_t)CST816_ADDR, (uint8_t)5) != 5) return false;
  uint8_t r[5];
  for (int i = 0; i < 5; i++) r[i] = tp->read();
  fingers = r[0] & 0x0F;
  x = ((r[1] & 0x0F) << 8) | r[2];
  y = ((r[3] & 0x0F) << 8) | r[4];
  return true;
}

static void touchBegin() {
  pinMode(PIN_TP_INT, INPUT_PULLUP);
  Wire.setTimeOut(10);
  if (touchPresent(Wire)) tp = &Wire;
  else {
    tpOwn.begin(PIN_TP_SDA, PIN_TP_SCL, 400000);
    tpOwn.setTimeOut(10);
    if (touchPresent(tpOwn)) tp = &tpOwn;
  }
  if (tp) {
    // Keep INT pulsing for as long as a finger is down (EN_TOUCH | EN_CHANGE),
    // otherwise a still finger goes quiet and a hold looks like a release.
    const uint8_t cfg[][2] = { {0xFA, 0x60}, {0xFE, 0x01} };   // IrqCtl, DisAutoSleep
    for (auto& c : cfg) { tp->beginTransmission(CST816_ADDR); tp->write(c[0]); tp->write(c[1]); tp->endTransmission(true); }
  }
  Serial.printf("[display] touch %s\n", tp ? (tp == &Wire ? "on the system bus" : "on its own bus") : "not found");
}

// ---- The UI ---------------------------------------------------------------
// Two screens. Home: the station, its title, and a row of round buttons
// (volume down, previous, play/pause, next, volume up). List: the loaded
// station list, five rows at a time, tap a row to play. Everything you can
// tap is drawn, so nothing has to be guessed; swipes are shortcuts.
enum Screen { HOME, LIST, ABOUT, PHONE };
static Screen screen = HOME;
static int listTop = 0;
static const int ROWS = 5, ROW_H = 40, LIST_Y = 100;
static bool listLoading = false, wantLoad = false;
static String flash;                     // a short note shown for a moment
static unsigned long flashUntil = 0;
static unsigned long resetArmedUntil = 0;
static String busyText;                     // a full-screen notice, e.g. while firmware is written
static String previewName;                  // a screen forced by the console, for screenshots   // the Factory reset pill wants a second tap before this
static int uiEpoch = 0;                  // bumped whenever the UI itself changes

struct Hit { int x, y, w, h; String id; };
static std::vector<Hit> hits;            // tap targets of the last draw
static void addHit(int x, int y, int w, int h, const String& id) { hits.push_back({x, y, w, h, id}); }
static String hitAt(int x, int y) {
  for (int i = (int)hits.size() - 1; i >= 0; i--) {   // drawn last = on top
    const Hit& h = hits[i];
    if (x >= h.x && x < h.x + h.w && y >= h.y && y < h.y + h.h) return h.id;
  }
  return "";
}

static void note(const String& s) { flash = s; flashUntil = millis() + 1200; uiEpoch++; }

static void volumeBy(int d) {
  player::setVolume(player::volume() + d);
  note("Volume " + String(player::volume()));
}

static String lastSource = "popular";   // the list to go back to from favourites
static String wantSource;               // the list the next loop should load
static String phoneFrom;                // what was playing when the phone screen opened

static void star(int cx, int cy, int r, bool filled, uint16_t colour) {
  // A five-point star as a fan of triangles from the centre.
  float ox[5], oy[5], ix[5], iy[5];
  for (int i = 0; i < 5; i++) {
    float a = -PI / 2 + i * 2 * PI / 5, b = a + PI / 5;
    ox[i] = cx + r * cosf(a); oy[i] = cy + r * sinf(a);
    ix[i] = cx + r * 0.45f * cosf(b); iy[i] = cy + r * 0.45f * sinf(b);
  }
  for (int i = 0; i < 5; i++) {
    int j = (i + 1) % 5;
    if (filled) {
      gfx->fillTriangle(cx, cy, ox[i], oy[i], ix[i], iy[i], colour);
      gfx->fillTriangle(cx, cy, ix[i], iy[i], ox[j], oy[j], colour);
    } else {
      gfx->drawLine(ox[i], oy[i], ix[i], iy[i], colour);
      gfx->drawLine(ix[i], iy[i], ox[j], oy[j], colour);
    }
  }
}

static void openList() {
  screen = LIST;
  int cur = player::currentIndex();
  listTop = cur >= 0 ? (cur / ROWS) * ROWS : 0;
  if (directory::stations.empty()) { listLoading = true; wantLoad = true; }
  uiEpoch++;
}

static void act(const String& id) {
  if (id == "vol-") volumeBy(-1);
  else if (id == "vol+") volumeBy(+1);
  else if (id == "prev") { player::prev(); }
  else if (id == "next") { player::next(); }
  else if (id == "play") player::togglePause();
  else if (id == "list") openList();
  else if (id == "home") { screen = (screen == ABOUT || screen == LIST) ? HOME : HOME; uiEpoch++; }
  else if (id == "about") { screen = ABOUT; uiEpoch++; }
  else if (id == "phone") { phoneFrom = player::current().url; screen = PHONE; uiEpoch++; }
  else if (id == "reset") {
    if (millis() < resetArmedUntil) net::factoryReset();
    else { resetArmedUntil = millis() + 4000; uiEpoch++; }
  }
  else if (id == "up") { if (listTop >= ROWS) { listTop -= ROWS; uiEpoch++; } }
  else if (id == "down") { if (listTop + ROWS < (int)directory::stations.size()) { listTop += ROWS; uiEpoch++; } }
  else if (id.startsWith("row:")) { player::playIndex(id.substring(4).toInt()); screen = HOME; uiEpoch++; }
  else if (id == "fav") {
    const Station& c = player::current();
    if (!c.url.isEmpty()) {
      bool now = favourites::toggle(c);
      note(now ? "Saved to favourites" : "Removed from favourites");
      if (directory::source == "favourites") directory::load("favourites");
    }
  }
  else if (id.startsWith("star:")) {
    int i = id.substring(5).toInt();
    if (i >= 0 && i < (int)directory::stations.size()) {
      Station s = directory::stations[i];
      bool now = favourites::toggle(s);
      note(now ? "Saved" : "Removed");
      if (directory::source == "favourites") directory::load("favourites");
      uiEpoch++;
    }
  }
  else if (id == "showfavs") { lastSource = directory::source.isEmpty() ? "popular" : directory::source; directory::load("favourites"); listTop = 0; uiEpoch++; }
  else if (id.startsWith("load:")) { wantSource = id.substring(5); listLoading = true; wantLoad = true; listTop = 0; uiEpoch++; }
}

// Touch: a press on a volume button acts at once and repeats while held;
// everything else acts on release, as a tap or a swipe.
static bool down = false;
static int px0, py0, pxl, pyl;
static unsigned long t0 = 0, lastInt = 0, nextRepeat = 0;
static String pressed;
static bool held = false;                 // the hold already acted; ignore the release
static unsigned long ignoreUntil = 0;     // presses starting before this are ghosts

static void touchLoop() {
  if (!tp) return;
  if (digitalRead(PIN_TP_INT) == LOW) lastInt = millis();
  bool active = millis() - lastInt < 150;  // INT pulses while a finger is down
  if (!active && !down) return;
  static unsigned long last = 0;
  if (millis() - last < 20) return;
  last = millis();
  uint8_t fingers = 0; int x = 0, y = 0;
  // While a finger is down the chip is awake and answers, so ask it directly:
  // INT goes quiet under a still finger and would otherwise look like a release.
  if (!touchRead(fingers, x, y)) {
    if (active) return;                    // a bad read mid-pulse: try again next time
    fingers = 0;                           // quiet line and no answer: the finger is gone
  }

  if (fingers && !down) {
    if (millis() < ignoreUntil) return;    // the tail of a hold, not a new press
    down = true; px0 = pxl = x; py0 = pyl = y; t0 = millis();
    pressed = hitAt(x, y); held = false;
    if (pressed == "vol-" || pressed == "vol+") { act(pressed); nextRepeat = millis() + 450; }
  } else if (fingers && down) {
    pxl = x; pyl = y;
    if ((pressed == "vol-" || pressed == "vol+") && millis() >= nextRepeat) { act(pressed); nextRepeat = millis() + 160; }
    if (!held && pressed == "home" && screen == HOME && millis() - t0 >= 600 &&
        abs(pxl - px0) < 30 && abs(pyl - py0) < 30) {
      held = true; act("about");          // hold the logo: the connect screen
    }
  } else if (!fingers && down) {
    down = false;
    int dx = pxl - px0, dy = pyl - py0;
    bool swipe = abs(dx) > 50 || abs(dy) > 50;
    if (held) { ignoreUntil = millis() + 400; return; }         // the hold already acted
    if (pressed == "vol-" || pressed == "vol+") return;          // already handled
    if (swipe) {
      if (screen == HOME) {
        if (abs(dx) > abs(dy)) { if (dx < 0) player::next(); else player::prev(); }
        else if (dy < 0) openList();
      } else {
        if (abs(dy) >= abs(dx)) act(dy < 0 ? "down" : "up");
        else if (dx > 0) act("home");
      }
    } else if (millis() - t0 < 800) {
      act(pressed);
    }
  }
}

// ---- Drawing ------------------------------------------------------------
static const uint16_t BTN = 0x1904;      // surface-300 #1e2226

static void centred(const String& s, int y, const uint8_t* font, uint16_t colour) {
  int16_t x1, y1; uint16_t w, h;
  gfx->setFont(font);
  gfx->setTextColor(colour);
  gfx->getTextBounds(s.c_str(), 0, y, &x1, &y1, &w, &h);
  gfx->setCursor(C - w / 2 - x1, y);
  gfx->print(s);
}

static int textWidth(const String& s, const uint8_t* font) {
  int16_t x1, y1; uint16_t w, h;
  gfx->setFont(font);
  gfx->getTextBounds(s.c_str(), 0, 0, &x1, &y1, &w, &h);
  return w;
}

// Cut to fit, with an ellipsis.
static String fit(const String& s, const uint8_t* font, int maxW) {
  if (textWidth(s, font) <= maxW) return s;
  String t = s;
  while (t.length() > 1 && textWidth(t + "...", font) > maxW) t.remove(t.length() - 1);
  t.trim();
  return t + "...";
}

// Break into at most `lines` lines that fit `maxW`; the last gets an ellipsis
// when something was left out.
static std::vector<String> wrap(const String& text, const uint8_t* font, int maxW, int lines) {
  std::vector<String> out, words;
  String word;
  for (size_t i = 0; i <= text.length(); i++) {
    char c = i < text.length() ? text[i] : ' ';
    if (c == ' ') { if (word.length()) words.push_back(word); word = ""; }
    else word += c;
  }
  size_t i = 0; String line;
  while (i < words.size() && (int)out.size() < lines) {
    String t = line.isEmpty() ? words[i] : line + " " + words[i];
    if (textWidth(t, font) <= maxW) { line = t; i++; }
    else if (line.isEmpty()) {  // one word wider than the line: cut it
      line = words[i++];
      while (textWidth(line, font) > maxW && line.length() > 1) line.remove(line.length() - 1);
    } else { out.push_back(line); line = ""; }
  }
  bool cut = i < words.size() || (!line.isEmpty() && (int)out.size() == lines);
  if (!line.isEmpty() && (int)out.size() < lines) out.push_back(line);
  if (cut && !out.empty()) out.back() = fit(out.back() + " " + (i < words.size() ? words[i] : ""), font, maxW);
  return out;
}

// Width available on a row of the round screen at height y (both sides).
static int chord(int y, int margin) {
  int d = y - C;
  int half = (int)sqrtf((float)(C * C - d * d));
  return max(40, 2 * half - 2 * margin);
}

static void volumeRing(int vol) {
  // A ring around the edge, 270 degrees from bottom-left round to bottom-right.
  gfx->fillArc(C, C, C - 1, C - 9, 135, 405, RING);
  if (vol > 0) gfx->fillArc(C, C, C - 1, C - 9, 135, 135 + 270 * vol / 21, ACCENT);
}

// A round button with a simple icon, registered as a tap target.
static void button(int cx, int cy, int r, const String& id, bool primary = false) {
  gfx->fillCircle(cx, cy, r, primary ? ACCENT : BTN);
  uint16_t ink = primary ? ON_GREEN : FG;
  if (id == "vol-") {
    gfx->fillRoundRect(cx - 10, cy - 2, 20, 4, 2, ink);
  } else if (id == "vol+") {
    gfx->fillRoundRect(cx - 10, cy - 2, 20, 4, 2, ink);
    gfx->fillRoundRect(cx - 2, cy - 10, 4, 20, 2, ink);
  } else if (id == "prev") {
    gfx->fillTriangle(cx - 12, cy, cx, cy - 9, cx, cy + 9, ink);
    gfx->fillTriangle(cx, cy, cx + 12, cy - 9, cx + 12, cy + 9, ink);
  } else if (id == "next") {
    gfx->fillTriangle(cx - 12, cy - 9, cx - 12, cy + 9, cx, cy, ink);
    gfx->fillTriangle(cx, cy - 9, cx, cy + 9, cx + 12, cy, ink);
  } else if (id == "play") {
    gfx->fillTriangle(cx - 7, cy - 11, cx - 7, cy + 11, cx + 11, cy, ink);
  } else if (id == "pause") {
    gfx->fillRect(cx - 9, cy - 10, 6, 20, ink);
    gfx->fillRect(cx + 3, cy - 10, 6, 20, ink);
  } else if (id == "up") {
    gfx->fillTriangle(cx - 10, cy + 5, cx + 10, cy + 5, cx, cy - 7, ink);
  } else if (id == "down") {
    gfx->fillTriangle(cx - 10, cy - 5, cx + 10, cy - 5, cx, cy + 7, ink);
  }
  addHit(cx - r - 6, cy - r - 6, 2 * r + 12, 2 * r + 12, id == "pause" ? "play" : id);
}

// A QR code, black on a white tile so any phone camera reads it.
static void drawQR(const String& text, int cx, int cy, int module) {
  QRCode qr;
  uint8_t buf[qrcode_getBufferSize(3)];   // 29 modules: up to 53 bytes, enough for both strings
  if (qrcode_initText(&qr, buf, 3, ECC_LOW, text.c_str()) != 0) return;
  int size = qr.size * module, quiet = module * 3;
  int x0 = cx - size / 2, y0 = cy - size / 2;
  gfx->fillRoundRect(x0 - quiet, y0 - quiet, size + 2 * quiet, size + 2 * quiet, 8, 0xFFFF);
  for (int y = 0; y < qr.size; y++)
    for (int x = 0; x < qr.size; x++)
      if (qrcode_getModule(&qr, x, y)) gfx->fillRect(x0 + x * module, y0 + y * module, module, module, 0x0000);
}

static String pageUrl() { return "http://" + net::ip(); }

// A pill button: a rounded bar with a label, registered as a tap target.
static void pill(int cy, int w, const String& label, uint16_t bg, uint16_t ink, const String& id, const uint8_t* font = u8g2_font_helvB14_tf, int h = 36) {
  gfx->fillRoundRect(C - w / 2, cy - h / 2, w, h, h / 2, bg);
  centred(label, cy + 5, font, ink);
  addHit(C - w / 2 - 6, cy - h / 2 - 6, w + 12, h + 12, id);
}

// The phone screen: the page's QR code, for picking a station there.
static void drawPhone() {
  addHit(0, 0, LCD_SIZE, LCD_SIZE, "home");
  brand(42, 20);
  drawQR(pageUrl(), C, 156, 4);
  centred("Scan, or open " HOSTNAME ".local", 256, u8g2_font_helvR12_tf, FG);
  centred("to search and pick on your phone", 274, u8g2_font_helvR10_tf, DIM);
  pill(312, 96, "Back", BTN, FG, "home", u8g2_font_helvB12_tf, 28);
}

static void drawAbout() {
  addHit(0, 0, LCD_SIZE, LCD_SIZE, "home");   // anything not covered below leads home
  brand(42, 20);
  drawQR(pageUrl(), C, 156, 4);            // the tile spans y 86..226
  centred(String(HOSTNAME) + ".local  \u00b7  " + net::ip(), 256, u8g2_font_helvB12_tf, FG);
  centred(WiFi.SSID() + "  " + WiFi.RSSI() + " dBm  \u00b7  v" AD_VERSION, 276, u8g2_font_helvR10_tf, DIM);
  bool armed = millis() < resetArmedUntil;
  pill(306, armed ? 150 : 120, armed ? "Tap again to reset" : "Factory reset", armed ? ACCENT : BTN, armed ? 0x0000 : FG, "reset", u8g2_font_helvB12_tf, 28);
}

static void drawHome() {
  const Station& s = player::current();
  bool running = player::running();
  brand(50, 22);

  if (s.name.isEmpty() || previewName == "empty") {
    // Nothing to control yet: offer the two ways in and nothing else.
    centred("Nothing playing yet", 134, u8g2_font_helvB18_tf, FG);
    pill(190, 200, "Pick a station", ACCENT, 0x0000, "list");
    pill(242, 200, "Use my phone", BTN, FG, "phone");
    centred("Hold the logo for settings", 300, u8g2_font_helvR10_tf, DIM);
    return;
  } else {
    int y = 108;
    for (const String& l : wrap(s.name, u8g2_font_helvB24_tf, 290, 2)) { centred(l, y, u8g2_font_helvB24_tf, FG); y += 32; }
    y += 6;
    String title = player::title();
    bool connecting = player::connecting();
    if (connecting) { title = "Connecting"; for (int d = 0; d <= (int)(millis() / 400) % 3; d++) title += "."; }
    else if (!running) title = player::paused() ? "Paused" : "Stopped";
    for (const String& l : wrap(title, u8g2_font_helvR14_tf, 300, 2)) { centred(l, y, u8g2_font_helvR14_tf, connecting ? ACCENT : running ? INK2 : DIM); y += 22; }
    addHit(30, 80, LCD_SIZE - 60, 150, "list");   // the station text opens the list
    String meta = player::codec();
    if (s.country.length()) meta = s.country + (meta.isEmpty() ? "" : "  ·  " + meta);
    if (millis() >= flashUntil) centred(meta, 296, u8g2_font_helvR10_tf, DIM);
  }
  if (millis() < flashUntil) centred(flash, 298, u8g2_font_helvB14_tf, ACCENT);

  // The controls: volume down, previous, play/pause, next, volume up.
  int y = 238;
  button(C - 116, y, 23, "vol-");
  button(C - 58, y, 23, "prev");
  button(C, y, 28, running && !player::paused() ? "pause" : "play", true);
  button(C + 58, y, 23, "next");
  button(C + 116, y, 23, "vol+");

  // The bottom row: save the playing station, and the station list.
  // The bottom row: a star that saves the station, and the list.
  int ph = 30, py = 328;
  {
    bool fav = favourites::has(s.id.length() ? s.id : s.url);
    int sw = 38, pw = 96, gap = 8, x = C - (sw + gap + pw) / 2;
    gfx->fillRoundRect(x, py - ph / 2, sw, ph, ph / 2, fav ? ACCENT : BTN);
    star(x + sw / 2, py, 7, true, fav ? ON_GREEN : ACCENT);
    addHit(x - 6, py - ph / 2 - 6, sw + 12, ph + 12, "fav");
    x += sw + gap;
    gfx->fillRoundRect(x, py - ph / 2, pw, ph, ph / 2, BTN);
    gfx->setFont(u8g2_font_helvB12_tf);
    int w = textWidth("Stations", u8g2_font_helvB12_tf);
    gfx->setTextColor(FG); gfx->setCursor(x + pw / 2 - w / 2, py + 4); gfx->print("Stations");
    addHit(x - 2, py - ph / 2 - 6, pw + 8, ph + 12, "list");
  }
}

static void drawList() {
  brand(36, 18);
  // Three ways in, side by side: Favourites, Popular, and whatever other list
  // was last loaded (a search, a country, a genre), which keeps its pill so
  // Popular is never pushed out. The one showing is lit.
  String src = directory::source;
  bool favs = src == "favourites";
  // Three pills, always the same three: Favourites, Popular, and Search,
  // which has no keyboard here and so opens the QR code for the web page.
  // The one lit is the list showing; a country or genre loaded from the page
  // shows its rows with none lit.
  struct Pill { String id, label; int w; bool on; char icon; };
  std::vector<Pill> pills;
  pills.push_back({favs ? "" : "showfavs", "", 38, favs, 's'});
  pills.push_back({"load:popular", "Popular", 96, src == "popular", 0});
  pills.push_back({"phone", "", 38, src.startsWith("search:"), 'q'});
  int ph = 28, py = 78, gap = 6, total = -gap;
  for (auto& p : pills) total += p.w + gap;
  int x = C - total / 2;
  for (auto& p : pills) {
    uint16_t ink = p.on ? ON_GREEN : p.icon ? ACCENT : FG;
    gfx->fillRoundRect(x, py - ph / 2, p.w, ph, ph / 2, p.on ? ACCENT : BTN);
    int cx = x + p.w / 2;
    if (p.icon == 's') star(cx, py, 7, true, ink);
    else if (p.icon == 'q') {   // a magnifier
      for (int r = 5; r <= 6; r++) gfx->drawCircle(cx - 2, py - 2, r, ink);
      for (int d = 0; d < 2; d++) gfx->drawLine(cx + 2 + d, py + 2, cx + 7 + d, py + 7, ink);
    } else { int w = textWidth(p.label, u8g2_font_helvB12_tf); gfx->setFont(u8g2_font_helvB12_tf); gfx->setTextColor(ink); gfx->setCursor(cx - w / 2, py + 4); gfx->print(p.label); }
    addHit(x, py - ph / 2 - 6, p.w, ph + 12, p.on && p.icon != 'q' ? "" : p.id);
    x += p.w + gap;
  }

  if (listLoading) {
    centred("Loading...", C, u8g2_font_helvR14_tf, DIM);
    return;
  }
  int n = directory::stations.size();
  if (!n) {
    if (favs) {
      centred("No favourites yet", C - 10, u8g2_font_helvR14_tf, DIM);
      centred("Tap the star while a station plays", C + 14, u8g2_font_helvR10_tf, DIM);
    } else {
      centred("No stations loaded", C - 10, u8g2_font_helvR14_tf, DIM);
      centred("Pick a list at airdrift.local", C + 14, u8g2_font_helvR10_tf, DIM);
    }
    return;
  }
  if (!directory::error.isEmpty() && n == 0) centred(directory::error, C, u8g2_font_helvR10_tf, DIM);
  int cur = player::currentIndex();
  for (int r = 0; r < ROWS && listTop + r < n; r++) {
    int i = listTop + r;
    int top = LIST_Y + r * ROW_H, cy = top + ROW_H / 2;
    const Station& s = directory::stations[i];
    bool on = i == cur;
    int w = chord(cy, 26);
    if (on) gfx->fillRoundRect(C - w / 2, top + 3, w, ROW_H - 6, 10, BTN);
    gfx->drawFastHLine(C - w / 2 + 8, top + ROW_H - 1, w - 16, RING);
    bool fav = favourites::has(s.id.length() ? s.id : s.url);
    String name = fit(s.name, u8g2_font_helvR14_tf, w - 56);
    gfx->setFont(u8g2_font_helvR14_tf); gfx->setTextColor(on ? ACCENT : FG);
    gfx->setCursor(C - w / 2 + 14, cy + 5); gfx->print(name);
    star(C + w / 2 - 20, cy, 8, fav, fav ? ACCENT : RING);
    addHit(C - w / 2, top, w - 40, ROW_H, "row:" + String(i));
    addHit(C + w / 2 - 40, top, 40, ROW_H, "star:" + String(i));
  }
  int page = listTop / ROWS + 1, pages = (n + ROWS - 1) / ROWS;
  int y = 320;
  if (listTop >= ROWS) button(C - 60, y, 17, "up");
  if (listTop + ROWS < n) button(C + 60, y, 17, "down");
  centred(String(page) + " / " + pages, y + 5, u8g2_font_helvR10_tf, DIM);
}

static void draw() {
  hits.clear();
  gfx->fillScreen(BG);
  volumeRing(player::volume());
  String pv = previewName;
  if (pv == "busy" || busyText.length()) {
    brand(C - 40, 24);
    centred(busyText.length() ? busyText : "Updating firmware", C + 30, u8g2_font_helvB18_tf, FG);
    centred("Keep the radio powered", C + 58, u8g2_font_helvR12_tf, DIM);
  } else if (pv == "setup" || (pv.isEmpty() && net::portalOpen())) {
    brand(44, 20);
    centred("Let's get you online", 78, u8g2_font_helvB14_tf, FG);
    drawQR("WIFI:T:nopass;S:" SETUP_AP_NAME ";;", C, 168, 4);   // the tile spans y 98..238
    String joining = net::joining();
    if (joining.length()) {
      String t = "Joining \"" + joining + "\""; for (int d = 0; d <= (int)(millis() / 400) % 3; d++) t += ".";
      centred(t, 266, u8g2_font_helvB12_tf, ACCENT);
      centred("almost there", 286, u8g2_font_helvR10_tf, DIM);
    } else {
      centred("Scan to join \"" SETUP_AP_NAME "\"", 262, u8g2_font_helvR12_tf, FG);
      centred("then pick your home WiFi", 280, u8g2_font_helvR10_tf, DIM);
      centred("or join it from WiFi settings", 296, u8g2_font_helvR10_tf, DIM);
    }
  } else if (pv.isEmpty() && !net::connected()) {
    centred("Connecting to WiFi", C, u8g2_font_helvB18_tf, FG);
  } else if (pv == "list" || (pv.isEmpty() && screen == LIST)) {
    drawList();
  } else if (pv == "about" || (pv.isEmpty() && screen == ABOUT)) {
    drawAbout();
  } else if (pv == "phone" || (pv.isEmpty() && screen == PHONE)) {
    drawPhone();
  } else {
    drawHome();
  }
  pushFrame();
}

// ---- Public ----------------------------------------------------------------
void display::begin() {
  Wire.begin(PIN_SYS_SDA, PIN_SYS_SCL, 400000);
  scanBus();
  resetPanelAndTouch();
  ledcAttach(PIN_LCD_BL, 5000, 8);
  ledcWrite(PIN_LCD_BL, 0);
  chunk[0] = (uint16_t*)heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  chunk[1] = (uint16_t*)heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!chunk[0] || !chunk[1] || !gfx->begin(GFX_SKIP_OUTPUT_BEGIN) || !panelBegin()) { Serial.println("[display] screen did not start"); io = nullptr; return; }
  gfx->setUTF8Print(true);  // station names and titles are UTF-8
  gfx->fillScreen(BG);
  logoTile(C - 48, C - 70, 96);
  brand(C + 56, 28);
  centred(AD_VERSION, C + 92, u8g2_font_helvR10_tf, DIM);
  pushFrame();
  ledcWrite(PIN_LCD_BL, 220);
  touchBegin();
  Serial.println("[display] ready");
}

void display::loop() {
  if (!io) return;
  touchLoop();
  static unsigned long last = 0;
  static String shown;
  if (millis() - last < 100) return;
  last = millis();
  const Station& s = player::current();
  if (screen == PHONE && s.url != phoneFrom) { screen = HOME; uiEpoch++; }   // a station was picked on the phone
  String key = s.name + "|" + busyText + previewName + "|" + (net::joining().length() ? String((millis() / 400) % 3) : String("-")) + "|" + player::title() + "|" + player::codec() + "|" + player::volume() + "|" +
               player::running() + player::paused() + "|" + net::connected() + net::portalOpen() + "|" +
               (millis() < flashUntil ? flash : "") + "|" + screen + "|" + listTop + "|" + listLoading + "|" +
               directory::source + "|" + directory::stations.size() + "|" + player::currentIndex() + "|" + uiEpoch +
               "|" + (player::connecting() ? String((millis() / 400) % 3) : String("-")) + (millis() < resetArmedUntil);
  if (key != shown) { shown = key; draw(); }
  if (wantLoad) {   // after the "Loading..." frame is on screen
    wantLoad = false;
    String src = wantSource.length() ? wantSource : (lastSource.isEmpty() || lastSource == "favourites" ? "popular" : lastSource);
    wantSource = "";
    if (net::connected()) directory::load(src);
    listLoading = false;
    uiEpoch++;
  }

}

// Drawn at once: a firmware upload holds the main loop until it is done.
void display::busy(const String& text) { busyText = text; uiEpoch++; if (io) draw(); }
const uint16_t* display::frame() { return io ? gfx->getFramebuffer() : nullptr; }
int display::side() { return LCD_SIZE; }
void display::preview(const String& name) {
  previewName = name == "off" ? "" : name;
  if (previewName == "list" && directory::stations.empty()) openList();
  uiEpoch++;
}

#else
void display::begin() {}
void display::loop() {}
void display::busy(const String&) {}
const uint16_t* display::frame() { return nullptr; }
int display::side() { return 0; }
void display::preview(const String&) {}
#endif
