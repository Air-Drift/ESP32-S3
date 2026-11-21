#include "config.h"
#include "display.h"

#if HAS_DISPLAY

// The round screen on the Waveshare ESP32-S3-Touch-LCD-1.85 / 1.85C, and the
// same panel and touch chip in the Guition JC3636K518C knob.
//
// The panel is driven the way Waveshare's own demo drives it: ESP-IDF's
// esp_lcd SPI panel IO in quad mode, commands as 0x02 00 cc 00, pixels as
// 0x32 00 2C 00 followed by the whole frame in one DMA transfer. Drawing
// happens in a framebuffer (Arduino_GFX's canvas, for its fonts and shapes),
// and the frame is pushed whenever something changed.
#include <Arduino.h>
#include <atomic>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <U8g2lib.h>
#include <qrcode.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_heap_caps.h>
#include "boards/waveshare185_init.h"
#include "boards/guition_w518_init.h"
#ifndef PANEL_INIT
#define PANEL_INIT waveshare185_st77916_init   // the 1.85's, which the knob shares
#endif
#include "net.h"
#include "directory.h"
#include "player.h"
#include "favourites.h"
#include "power.h"
#include "spectrum.h"
#include "coverart.h"
#include "drift.h"
#include <WiFi.h>
#include <Preferences.h>

static const int C = LCD_SIZE / 2;  // centre

static Arduino_Canvas* gfx = new Arduino_Canvas(LCD_SIZE, LCD_SIZE, nullptr);
static esp_lcd_panel_io_handle_t io = nullptr;
// The SPI driver DMAs from internal RAM only, so the frame goes out in row
// chunks through two small internal buffers, swapped to the panel's byte order.
static const int CHUNK_ROWS = 10;   // 2 x 7 KB of internal RAM; TLS needs the rest
static const size_t CHUNK_BYTES = LCD_SIZE * CHUNK_ROWS * 2;
static uint16_t* chunk[2] = {nullptr, nullptr};
// Chunks handed to the SPI driver and not yet sent. The driver's interrupt
// counts them down while the loop counts them up, so it must be atomic: a lost
// decrement once left every frame waiting out the timeout (2 fps).
static std::atomic<int> inFlight{0};

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
// The visualiser passes its own heights, in the same 24-unit grid, to make
// the bars move with the music.
static void logoTile(int x, int y, int size, const float* bars = nullptr) {
  gfx->fillRoundRect(x, y, size, size, size * 28 / 100, ACCENT);
  float u = size * 0.72f / 24.0f, off = size * 0.14f;
  const float heights[5] = {4, 10, 18, 10, 4};
  // Each bar is a rectangle with a round cap at either end: a rounded rect
  // with a radius of half its width leaves notches at these small sizes.
  // Small (the header's lockup) they are rounded rects, which read well
  // there; large (the visualiser's disc) rounded rects grow notches, so the
  // bars get round caps instead.
  int r = (int)lroundf(1.4f * u - 0.5f), w = 2 * r + 1;
  int cy = (int)lroundf(y + off + u * 12);
  for (int i = 0; i < 5; i++) {
    float h = (bars ? bars[i] : heights[i]) * u;
    if (size < 48) {
      float cxf = x + off + u * (4 + 4 * i), wf = 2.8f * u;
      gfx->fillRoundRect((int)(cxf - wf / 2), (int)(y + off + u * 12 - h / 2 - wf / 2), (int)wf, (int)(h + wf), (int)(wf / 2), ON_GREEN);
      continue;
    }
    int cx = (int)lroundf(x + off + u * (4 + 4 * i)), half = (int)lroundf(h / 2);
    gfx->fillRect(cx - r, cy - half, w, 2 * half + 1, ON_GREEN);
    gfx->fillCircle(cx, cy - half, r, ON_GREEN);
    gfx->fillCircle(cx, cy + half, r, ON_GREEN);
  }
}

static void addHit(int x, int y, int w, int h, const String& id);

// The lockup: tile, then "airdrift" with "drift" in green, centred at y.
// Tapping it always leads home.
static void brand(int cy, int tile, int cx = C) {
  int16_t x1, y1; uint16_t w1, w2, h;
  gfx->setFont(tile >= 40 ? u8g2_font_helvB24_tf : tile >= 30 ? u8g2_font_helvB18_tf : u8g2_font_helvB14_tf);   // the word grows with the tile
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

// ---- Resets. On the Waveshare board the panel's and touch's reset lines
// live on a TCA9554 expander. ----
#ifdef TCA9554_ADDR
static void tcaWrite(uint8_t reg, uint8_t v) {
  Wire.beginTransmission(TCA9554_ADDR);
  Wire.write(reg); Wire.write(v);
  Wire.endTransmission();
}
#endif

static void resetPanelAndTouch() {
#ifdef TCA9554_ADDR
  uint8_t bits = (1 << EXIO_TP_RST) | (1 << EXIO_LCD_RST);
  tcaWrite(0x03, (uint8_t)~bits);  // those two as outputs, the rest inputs
  tcaWrite(0x01, 0x00);
  delay(20);
  tcaWrite(0x01, bits);
#else
  // The Guition boards have no expander: both reset lines are plain pins.
  pinMode(PIN_LCD_RST, OUTPUT); pinMode(PIN_TP_RST, OUTPUT);
  digitalWrite(PIN_LCD_RST, LOW); digitalWrite(PIN_TP_RST, LOW);
  delay(20);
  digitalWrite(PIN_LCD_RST, HIGH); digitalWrite(PIN_TP_RST, HIGH);
#endif
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

  for (const PanelCmd& c : PANEL_INIT) {
    panelCmd(c.cmd, c.len ? c.data : nullptr, c.len);
    if (c.delayMs) delay(c.delayMs);
  }
  return true;
}

static void waitFor(int pending) {
  unsigned long t0 = millis();
  while (inFlight > pending && millis() - t0 < 200) delay(1);
  if (inFlight > pending) inFlight = pending;   // a completion went missing: carry on rather than stall every frame
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

// INT pulses are short; caught by an interrupt they are not missed while a
// frame is being pushed (the charge screen animates, so that is often).
static volatile unsigned long intAt = 0;
static void IRAM_ATTR onTouchInt() { intAt = millis(); }

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
  if (tp) attachInterrupt(digitalPinToInterrupt(PIN_TP_INT), onTouchInt, FALLING);
  Serial.printf("[display] touch %s\n", tp ? (tp == &Wire ? "on the system bus" : "on its own bus") : "not found");
}

// ---- The UI ---------------------------------------------------------------
// Home is Drift, as on the site: the station and its country, then volume
// down, Drift and volume up, and two ways on: Remote (the QR code for this
// radio's page, where the whole directory is) and Favourites (the saved
// stations, three at a time, and the one playing to add). Every target is a
// thumb wide; the ring round the edge shows the volume; swipes are shortcuts.
enum Screen { HOME, FAVS, ABOUT, PHONE, MENU };
static Screen screen = HOME;
static int favTop = 0;                   // the first favourite showing
static const int FAV_ROWS = 3;
static String flash;                     // a short note shown for a moment
static unsigned long flashUntil = 0;
static unsigned long resetArmedUntil = 0;
static String busyText;                     // a full-screen notice, e.g. while firmware is written
static String previewName;                  // a screen forced by the console, for screenshots
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

static void note(const String& s) { flash = s; flashUntil = millis() + 1400; uiEpoch++; }

static String phoneFrom;                // what was playing when the phone screen opened
static bool menuPaused = false;         // the button menu paused the music, so closing it resumes
static unsigned long wifiArmedUntil = 0; // "New WiFi" wants a second tap before this
// A drift is not taken again while one is tuning or connecting, for 10 s at
// most: past that, a stuck one can be left for a new one.
static unsigned long driftAt = 0;
static bool driftLocked() {
  return (drift::state() == drift::TUNING || player::connecting()) && driftAt && millis() - driftAt < 10000;
}

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

static void volumeBy(int d);
static String keyOf(const Station& s) { return s.id.length() ? s.id : s.url; }

static void act(const String& id) {
  if (id == "vol-") volumeBy(-1);
  else if (id == "vol+") volumeBy(+1);
  else if (id == "play") player::togglePause();
  else if (id == "drift") { if (!driftLocked()) { driftAt = millis(); drift::start(""); } screen = HOME; uiEpoch++; }
  else if (id == "favs") {
    screen = FAVS;
    int i = 0; favTop = 0;
    for (const Station& f : favourites::list()) { if (keyOf(f) == keyOf(player::current())) favTop = (i / FAV_ROWS) * FAV_ROWS; i++; }
    uiEpoch++;
  }
  else if (id == "home") { screen = HOME; uiEpoch++; }
  else if (id == "about") { screen = ABOUT; uiEpoch++; }
  else if (id == "phone") { phoneFrom = player::current().url; screen = PHONE; uiEpoch++; }
  else if (id == "reset") {
    if (millis() < resetArmedUntil) net::factoryReset();
    else { resetArmedUntil = millis() + 4000; uiEpoch++; }
  }
  else if (id == "wifi") {
    if (millis() < wifiArmedUntil) { display::busy("Setting up WiFi"); net::forget(); }
    else { wifiArmedUntil = millis() + 4000; uiEpoch++; }
  }
  else if (id == "resume") display::button();
  else if (id == "fup" || id == "up") { if (favTop >= FAV_ROWS) { favTop -= FAV_ROWS; uiEpoch++; } }
  else if (id == "fdown" || id == "down") { if (favTop + FAV_ROWS < (int)favourites::list().size()) { favTop += FAV_ROWS; uiEpoch++; } }
  else if (id.startsWith("favplay:")) {
    // Playing a favourite makes the favourites the list, so next and previous walk them.
    directory::load("favourites");
    player::playIndex(id.substring(8).toInt());
    screen = HOME; uiEpoch++;
  }
  else if (id.startsWith("favstar:")) {
    int i = id.substring(8).toInt();
    const std::vector<Station>& l = favourites::list();
    if (i >= 0 && i < (int)l.size()) {
      favourites::remove(keyOf(l[i]));
      if (directory::source == "favourites") directory::load("favourites");
      if (favTop >= (int)favourites::list().size() && favTop >= FAV_ROWS) favTop -= FAV_ROWS;
      note("Removed from favourites");
    }
  }
  else if (id == "fav") {
    const Station& c = player::current();
    if (!c.url.isEmpty()) {
      bool now = favourites::toggle(c);
      note(now ? "Saved to favourites" : "Removed from favourites");
      if (directory::source == "favourites") directory::load("favourites");
    }
  }
  else if (id == "radio") power::wake();
}

// Touch: a press on a volume button acts at once and repeats while held;
// everything else acts on release, as a tap or a swipe.
static bool down = false;
static int px0, py0, pxl, pyl;
static unsigned long t0 = 0, lastInt = 0, nextRepeat = 0;
static String pressed;
static bool held = false;                 // the hold already acted; ignore the release
static bool dimmed = false;               // the charge screen's backlight is down
static bool asleep = false;               // low power: the screen is off while a station plays
static bool lowPowerOn = false;
static unsigned long touchAt = 0;         // the screen was last touched
static bool viz = false;                  // the visualiser is showing
static unsigned long ignoreUntil = 0;     // presses starting before this are ghosts

static void volumeBy(int d) {
  player::setVolume(player::volume() + d);
  note("Volume " + String(player::volume()));
}

static void touchLoop() {
  if (!tp) return;
  if (digitalRead(PIN_TP_INT) == LOW) lastInt = millis();
  if (intAt > lastInt) lastInt = intAt;
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
    touchAt = millis();
    if (dimmed) { dimmed = false; ledcWrite(PIN_LCD_BL, 220); held = true; return; }   // this touch only wakes the screen
    if (asleep) { asleep = false; ledcWrite(PIN_LCD_BL, 220); uiEpoch++; held = true; return; }
    if (viz) { viz = false; spectrum::enable(false); uiEpoch++; held = true; return; }  // and this one only leaves the visualiser
    if (pressed == "vol-" || pressed == "vol+") { act(pressed); nextRepeat = millis() + 450; }
  } else if (fingers && down) {
    pxl = x; pyl = y;
    if ((pressed == "vol-" || pressed == "vol+") && millis() >= nextRepeat) { act(pressed); nextRepeat = millis() + 160; }
    if (!held && pressed == "home" && screen == HOME && !power::standby() && millis() - t0 >= 600 &&
        abs(pxl - px0) < 30 && abs(pyl - py0) < 30) {
      held = true; act("about");          // hold the logo: the connect screen
    }
#if LONG_PRESS_MENU
    // No button on this board: a still finger, held, does what the button does.
    if (!held && !power::standby() && pressed != "vol-" && pressed != "vol+" && millis() - t0 >= 1000 &&
        abs(pxl - px0) < 30 && abs(pyl - py0) < 30) {
      held = true; display::button();
    }
#endif
  } else if (!fingers && down) {
    down = false;
    int dx = pxl - px0, dy = pyl - py0;
    bool swipe = abs(dx) > 50 || abs(dy) > 50;
    if (held) { ignoreUntil = millis() + 400; return; }         // the hold already acted
    if (pressed == "vol-" || pressed == "vol+") return;          // already handled
    if (swipe) {
      bool across = abs(dx) > abs(dy);
      if (screen == HOME) { if (across) { if (dx < 0) player::next(); else player::prev(); } }
      else if (screen == FAVS && !across) act(dy < 0 ? "fdown" : "fup");
      else if (across && dx > 0) act("home");                   // swipe right: back
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
  brand(42, 20);
  drawQR(pageUrl(), C, 156, 4);
  centred("Scan, or open " HOSTNAME ".local", 252, u8g2_font_helvR12_tf, FG);
  centred("to pick stations and favourites", 270, u8g2_font_helvR10_tf, DIM);
  pill(308, 130, "Back", BTN, FG, "home", u8g2_font_helvB14_tf, 44);
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

// A small battery at the top: its level, and the percentage beside it,
// green while a computer is charging it.
static void batteryBadge(int cy) {
  int pct = power::percent();
  if (pct < 0) return;
  String t = String(pct) + "%";
  uint16_t ink = power::usb() ? ACCENT : pct <= 15 ? 0xFA49 : DIM;   // red #ff4a4a when low
  int tw = textWidth(t, u8g2_font_helvB10_tf), bw = 20, bh = 10, gap = 5;
  int x = C - (bw + 2 + gap + tw) / 2, y = cy - bh / 2;
  gfx->drawRoundRect(x, y, bw, bh, 2, ink);
  gfx->fillRect(x + bw, y + 3, 2, bh - 6, ink);
  int fill = (bw - 4) * pct / 100;
  if (fill > 0) gfx->fillRect(x + 2, y + 2, fill, bh - 4, ink);
  gfx->setFont(u8g2_font_helvB10_tf); gfx->setTextColor(ink);
  gfx->setCursor(x + bw + 2 + gap, cy + 5); gfx->print(t);
}

// Switched on by a USB cable: the battery, and a way into the radio.
static void drawCharge() {
  int pct = power::percent();
  bool charging = power::usb();   // the cable can come out while this screen is up
  // The ring around the edge is the charge.
  gfx->fillArc(C, C, C - 1, C - 9, 135, 405, RING);
  if (pct > 0) gfx->fillArc(C, C, C - 1, C - 9, 135, 135 + 270 * pct / 100, ACCENT);
  brand(56, 20);
  bool full = pct >= 100, filling = charging && !full;
  // A battery that fills: solid to the level it is at, then a paler green
  // sweeping on to full, over and over, while it charges.
  int bw = 112, bh = 52, bx = C - bw / 2 - 4, by = 146 - bh / 2;
  for (int i = 0; i < 3; i++) gfx->drawRoundRect(bx + i, by + i, bw - 2 * i, bh - 2 * i, 10 - i, INK2);
  gfx->fillRoundRect(bx + bw + 2, by + bh / 2 - 10, 7, 20, 3, INK2);
  int ix = bx + 7, iy = by + 7, iw = bw - 14, ih = bh - 14;
  int base = iw * max(pct, 0) / 100;
  if (filling) {
    float phase = (millis() % 2000) / 2000.0f;
    int sweep = base + (int)((iw - base) * phase);
    if (sweep > 0) gfx->fillRoundRect(ix, iy, sweep, ih, 5, 0x1B48);   // green-800 #1f6b45
  }
  if (base > 0) gfx->fillRoundRect(ix, iy, base, ih, 5, ACCENT);
  if (filling) {   // a bolt across it
    int cx = C - 4, cy = 146;
    gfx->fillTriangle(cx + 5, cy - 18, cx - 9, cy + 3, cx + 1, cy + 3, FG);
    gfx->fillTriangle(cx - 1, cy - 3, cx + 9, cy - 3, cx - 5, cy + 18, FG);
  }
  if (pct < 0) {
    // The battery switch is off: it charges, but the level cannot be read.
    centred("Charging", 214, u8g2_font_helvB18_tf, FG);
    centred("Battery switch is off, so the level", 238, u8g2_font_helvR12_tf, DIM);
    centred("shows here when it is on", 256, u8g2_font_helvR12_tf, DIM);
  } else {
    centred(String(pct) + "%", 228, u8g2_font_logisoso32_tr, FG);
    centred(!charging ? "On battery" : full ? "Charged" : "Charging", 256, u8g2_font_helvR14_tf, charging && full ? ACCENT : INK2);
  }
  pill(296, 170, "Play radio", ACCENT, 0x0000, "radio");
}

// ---- The visualiser ------------------------------------------------------
// Left alone while a station plays, the screen becomes a sunburst: the logo in
// the middle with its five bars moving to the music (bass in the middle, the
// highs at the edges), and the spectrum radiating out from it, mirrored, bass
// at the top. The site's analyser, in the round: fast attack, slow release,
// a white peak cap on every bar that falls slowly.
static const int VB = 18;                 // bands each side
static float vBars[VB], vPeaks[VB];
static const uint16_t G700 = 0x1CEB, G300 = 0xA799, SLOT = 0x10A2;   // green-700, green-300, an empty slot

// ---- Words round the rim ------------------------------------------------
// The line is drawn once, flat, into a strip (station in green, then the song
// in white), and the ring maps every pixel between two radii back onto that
// strip: its angle is how far along, its radius how far down. A table made
// once holds each ring pixel's place, so a frame is lookups and a blend.
static const int RING_OUT = C - 6, RING_IN = RING_OUT - 22;   // the band the words run in
static const float RING_MID = (RING_IN + RING_OUT) / 2.0f;     // where the strip's scale is true
struct RingPx { uint32_t at; uint16_t s, r; };                   // framebuffer index; along and down, x16
static RingPx* ringPx = nullptr;
static int ringN = 0;
static Arduino_Canvas* strip = nullptr;
static int stripW = 0;
static String stripFor;

static void ringTable() {
  int n = 0;
  for (int pass = 0; pass < 2; pass++) {
    n = 0;
    for (int y = C - RING_OUT; y < C + RING_OUT; y++)
      for (int x = C - RING_OUT; x < C + RING_OUT; x++) {
        float dx = x + 0.5f - C, dy = y + 0.5f - C, r = sqrtf(dx * dx + dy * dy);
        if (r < RING_IN || r >= RING_OUT) continue;
        if (pass) {
          float a = atan2f(dy, dx) + PI / 2;   // 0 at the top, growing clockwise
          if (a < 0) a += 2 * PI;
          ringPx[n] = { (uint32_t)(y * LCD_SIZE + x), (uint16_t)(a * RING_MID * 16), (uint16_t)((RING_OUT - r) * 16) };
        }
        n++;
      }
    if (!pass) ringPx = (RingPx*)heap_caps_malloc(n * sizeof(RingPx), MALLOC_CAP_SPIRAM);
    if (!ringPx) return;
  }
  ringN = n;
}

static void ringStrip(const String& name, const String& songTitle) {
  String title = songTitle, title_ = songTitle;
  const uint8_t* font = u8g2_font_helvB14_tf;
  String sep = "   \u00b7   ";
  stripW = (int)(2 * PI * RING_MID);       // one strip pixel per ring pixel at mid radius
  if (strip) { delete strip; strip = nullptr; }
  strip = new Arduino_Canvas(stripW, 24, nullptr);
  if (!strip->begin(GFX_SKIP_OUTPUT_BEGIN)) { delete strip; strip = nullptr; return; }
  strip->setUTF8Print(true);
  strip->setFont(font);
  strip->setTextWrap(false);
  auto line = [&](int x) {                 // the line once from x; returns where it ends
    strip->setCursor(x, 18); strip->setTextColor(ACCENT); strip->print(name);
    strip->setTextColor(DIM); strip->print(sep);
    if (title.length()) { strip->setTextColor(FG); strip->print(title); strip->setTextColor(DIM); strip->print(sep); }
    return (int)strip->getCursorX();
  };
  // Measured by laying it out: text bounds leave out the separator's spaces.
  int w = line(0);
  if (w > stripW && title.length()) {      // longer than the ring: the song gives way, cut with an ellipsis
    String t = title;
    while (w > stripW && t.length() > 4) {
      t.remove(t.length() - 4); t.trim(); t += "...";
      strip->fillScreen(0);
      strip->setCursor(0, 18); strip->print(name); strip->print(sep); strip->print(t); strip->print(sep);
      w = strip->getCursorX();
      title_ = t;
    }
  }
  strip->fillScreen(0);
  title = title_;
  int copies = max(1, stripW / max(1, w));   // short lines go round more than once, evenly
  float step = (float)stripW / copies;
  for (int c = 0; c < copies; c++) line((int)(c * step + (step - w) / 2));
}

static void textRing(const String& name, const String& title) {
  if (!ringPx) ringTable();
  if (!ringPx) return;
  String key = name + "\n" + title;
  if (key != stripFor || !strip) { stripFor = key; ringStrip(name, title); }
  if (!strip) return;
  const uint16_t* sp = strip->getFramebuffer();
  uint16_t* fb = gfx->getFramebuffer();
  int W16 = stripW * 16;
  int shift = (int)((millis() / 25) % stripW) * 16;   // 40 px a second, clockwise
  for (int i = 0; i < ringN; i++) {
    const RingPx& p = ringPx[i];
    int s = (int)p.s - shift; if (s < 0) s += W16;
    int sx = s >> 4, sy = p.r >> 4, fx = s & 15, fy = p.r & 15;
    if (sy >= 23) continue;
    // Bilinear between the four strip pixels around the point, per channel.
    int sx1 = sx + 1 == stripW ? 0 : sx + 1;
    uint16_t a = sp[sy * stripW + sx], b = sp[sy * stripW + sx1], c = sp[(sy + 1) * stripW + sx], d = sp[(sy + 1) * stripW + sx1];
    if (!(a | b | c | d)) continue;
    int w00 = (16 - fx) * (16 - fy), w10 = fx * (16 - fy), w01 = (16 - fx) * fy, w11 = fx * fy;
    int r = ((a >> 11) * w00 + (b >> 11) * w10 + (c >> 11) * w01 + (d >> 11) * w11) >> 8;
    int g = (((a >> 5) & 63) * w00 + ((b >> 5) & 63) * w10 + ((c >> 5) & 63) * w01 + ((d >> 5) & 63) * w11) >> 8;
    int bl = ((a & 31) * w00 + (b & 31) * w10 + (c & 31) * w01 + (d & 31) * w11) >> 8;
    fb[p.at] = (r << 11) | (g << 5) | bl;
  }
}

static void ray(float a, float r0, float r1, float w, uint16_t c) {
  if (r1 <= r0) return;
  float ca = cosf(a), sa = sinf(a), px = -sa * w / 2, py = ca * w / 2;
  float x0 = C + ca * r0, y0 = C + sa * r0, x1 = C + ca * r1, y1 = C + sa * r1;
  gfx->fillTriangle(x0 + px, y0 + py, x0 - px, y0 - py, x1 + px, y1 + py, c);
  gfx->fillTriangle(x0 - px, y0 - py, x1 - px, y1 - py, x1 + px, y1 + py, c);
}

static void drawViz() {
  float in[VB] = {0};
  bool live = spectrum::read(in, VB, player::sampleRate());
  for (int b = 0; b < VB; b++) {
    float t = live ? in[b] : 0;
    vBars[b] = t > vBars[b] ? t : vBars[b] + (t - vBars[b]) * 0.3f;
    vPeaks[b] = max(vPeaks[b] - 0.02f, vBars[b]);
  }
  const float R0 = 74, LEN = RING_IN - R0 - 6, W = 7;   // up to the ring of words
  for (int side = -1; side <= 1; side += 2)
    for (int b = 0; b < VB; b++) {
      float a = -PI / 2 + side * (b + 0.5f) * PI / VB;
      float r = R0 + 2 + vBars[b] * LEN, pk = R0 + 2 + vPeaks[b] * LEN;
      ray(a, R0, r, W, G700);
      ray(a, R0 + (r - R0) * 0.45f, r, W, ACCENT);
      ray(a, max(R0, r - 3), r, W, G300);
      if (vPeaks[b] > 0.06f) ray(a, pk + 2, pk + 4, W, FG);
    }
  // The song's cover when MusicBrainz knows it, as a disc; else the logo.
  int side = 0;
  if (const uint16_t* art = coverart::image(side)) {
    int r = min(58, side / 2), x0 = C - side / 2, y0 = C - side / 2;
    for (int y = -r; y <= r; y++) {
      int span = (int)sqrtf((float)(r * r - y * y)), sy = C + y - y0;
      for (int x = -span; x <= span; x++) gfx->writePixelPreclipped(C + x, C + y, art[sy * 128 + (C + x - x0)]);
    }
    coverart::release();
    gfx->drawCircle(C, C, r, BG); gfx->drawCircle(C, C, r + 1, BG);
  } else {
    // No cover: the logo as a disc the same size, its green with the five bars.
    gfx->fillCircle(C, C, 58, ACCENT);
    logoTile(C - 39, C - 39, 78);
  }

  // Round the rim, like the words on a record's label: the station and the
  // song, going round.
  const Station& s = player::current();
  String title = player::title();
  textRing(s.country.length() ? s.name + ", " + drift::countryName(s.country) : s.name, title);
}

// ---- Drift on the menu ---------------------------------------------------
// No animation here (the visualiser is the moving picture): while it looks
// for a station a line says so, and when it lands the station and its
// country show as for any other.
static bool tunerShowing() {
  drift::State d = drift::state();
  return d == drift::TUNING || d == drift::FAILED && millis() - drift::since() < 4000;
}

static void drawTuner() {
  String mood = drift::mood();
  if (drift::state() == drift::FAILED) {
    centred("No signal", 92, u8g2_font_helvB18_tf, FG);
    centred(fit(drift::message(), u8g2_font_helvR12_tf, 250), 116, u8g2_font_helvR12_tf, DIM);
    return;
  }
  centred(mood.length() ? "Drifting to " + mood : String("Drifting"), 92, u8g2_font_helvB18_tf, FG);
  if (drift::message().length()) centred(fit(drift::message(), u8g2_font_helvR12_tf, 250), 116, u8g2_font_helvR12_tf, DIM);
}

// A big round button, with its icon; registered as a tap target.
static void bigButton(int cx, int cy, int r, const String& id, uint16_t bg, bool enabled = true) {
  gfx->fillCircle(cx, cy, r, enabled ? bg : BTN);
  uint16_t ink = bg == ACCENT && enabled ? ON_GREEN : enabled ? FG : DIM;
  if (id == "fav" || id == "faved") star(cx, cy, r * 45 / 100, true, id == "faved" ? ON_GREEN : enabled ? ACCENT : DIM);
  else if (id == "vol-") gfx->fillRoundRect(cx - 14, cy - 3, 28, 6, 3, ink);
  else if (id == "vol+") { gfx->fillRoundRect(cx - 14, cy - 3, 28, 6, 3, ink); gfx->fillRoundRect(cx - 3, cy - 14, 6, 28, 3, ink); }
  else if (id == "play") gfx->fillTriangle(cx - 11, cy - 17, cx - 11, cy + 17, cx + 17, cy, ink);
  else if (id == "pause") { gfx->fillRoundRect(cx - 13, cy - 16, 10, 32, 3, ink); gfx->fillRoundRect(cx + 3, cy - 16, 10, 32, 3, ink); }
  if (enabled) addHit(cx - r - 6, cy - r - 6, 2 * r + 12, 2 * r + 12, id == "faved" ? "fav" : id == "pause" ? "play" : id);
}

// Home: the station (no song: the visualiser has that), then the three big
// buttons and the two ways on. The battery sits in the volume ring's gap.
// The button's menu: Resume, a new WiFi network, a reset. The two that undo
// something ask for a second tap.
static void drawMenu() {
  brand(44, 24);
  centred(menuPaused ? "Paused" : net::connected() ? "Menu" : "Not connected", 92, u8g2_font_helvB18_tf, FG);
  String ssid = WiFi.SSID();
  if (ssid.length()) centred(fit("WiFi: " + ssid, u8g2_font_helvR12_tf, 260), 116, u8g2_font_helvR12_tf, DIM);
  pill(162, 220, menuPaused ? "Resume" : "Close", ACCENT, ON_GREEN, "resume", u8g2_font_helvB18_tf, 52);
  bool wArmed = millis() < wifiArmedUntil, rArmed = millis() < resetArmedUntil;
  pill(226, 220, wArmed ? "Tap again to set up" : "New WiFi", wArmed ? ACCENT : BTN, wArmed ? ON_GREEN : FG, "wifi", u8g2_font_helvB14_tf, 48);
  pill(286, 200, rArmed ? "Tap again to reset" : "Reset device", rArmed ? 0xFA49 : BTN, rArmed ? FG : FG, "reset", u8g2_font_helvB14_tf, 44);
  centred("or press the button again", 330, u8g2_font_helvR10_tf, DIM);
}

static void drawHome() {
  const Station& s = player::current();
  bool nothing = s.name.isEmpty() || previewName == "empty";
  volumeRing(player::volume());
  batteryBadge(332);
  brand(40, 30);

  if (tunerShowing()) {
    drawTuner();
  } else if (nothing) {
    centred("Press Drift to land", 84, u8g2_font_helvB18_tf, FG);
    centred("somewhere new", 108, u8g2_font_helvB18_tf, FG);
  } else {
    centred(fit(s.name, u8g2_font_helvB18_tf, 250), 92, u8g2_font_helvB18_tf, FG);
    String state = s.country.length() ? drift::countryName(s.country) : String();
    uint16_t ink = DIM;
    if (player::connecting()) { state = "Connecting..."; ink = ACCENT; }
    else if (!player::running()) state = player::paused() ? "Paused" : player::lastError().length() ? player::lastError() : "Stopped";
    if (millis() >= flashUntil) centred(fit(state, u8g2_font_helvR12_tf, 240), 116, u8g2_font_helvR12_tf, ink);
  }
  if (millis() < flashUntil) centred(flash, 116, u8g2_font_helvB12_tf, ACCENT);

  // Volume down, Drift, volume up, with room between. Stopping is the power
  // switch's job.
  int y = 180;
  bigButton(C - 114, y, 36, "vol-", BTN);
  bigButton(C + 114, y, 36, "vol+", BTN);
  int dw = 138, dh = 72;
  bool locked = driftLocked();   // tuning or connecting: greyed, and taps do nothing
  gfx->fillRoundRect(C - dw / 2, y - dh / 2, dw, dh, dh / 2, locked ? BTN : ACCENT);
  centred("Drift", y + 9, u8g2_font_helvB24_tf, locked ? DIM : ON_GREEN);
  addHit(C - dw / 2 - 4, y - dh / 2 - 10, dw + 8, dh + 20, locked ? "" : "drift");

  // Two ways on, inside the ring at this height: Remote (the QR code for
  // this radio's page, where the whole directory is) and the favourites.
  int py = 266, pw = 106, ph = 52, gap = 12;
  int lx = C - gap / 2 - pw, rx = C + gap / 2;
  gfx->fillRoundRect(lx, py - ph / 2, pw, ph, ph / 2, BTN);
  gfx->fillRoundRect(rx, py - ph / 2, pw, ph, ph / 2, BTN);
  gfx->setFont(u8g2_font_helvB14_tf); gfx->setTextColor(FG);
  int w = textWidth("Remote", u8g2_font_helvB14_tf);
  gfx->setCursor(lx + pw / 2 - w / 2, py + 6); gfx->print("Remote");
  const uint8_t* ff = textWidth("Favourites", u8g2_font_helvB14_tf) <= pw - 12 ? u8g2_font_helvB14_tf : u8g2_font_helvB12_tf;
  gfx->setFont(ff);
  w = textWidth("Favourites", ff);
  gfx->setCursor(rx + pw / 2 - w / 2, py + 6); gfx->print("Favourites");
  addHit(lx - 4, py - ph / 2 - 8, pw + 4 + gap / 2, ph + 16, "phone");
  addHit(rx - gap / 2, py - ph / 2 - 8, pw + 4 + gap / 2, ph + 16, "favs");
}

// A round button with a chevron pointing back, leading home.
static void backButton(int cx, int cy, int r) {
  gfx->fillCircle(cx, cy, r, BTN);
  int a = r * 4 / 10;
  for (int k = -1; k <= 1; k++) {
    gfx->drawLine(cx + a / 2 + k, cy - a, cx - a / 2 - 1 + k, cy, FG);
    gfx->drawLine(cx - a / 2 - 1 + k, cy, cx + a / 2 + k, cy + a, FG);
  }
  addHit(cx - r - 10, cy - r - 10, 2 * r + 20, 2 * r + 20, "home");
}

// The saved stations, three at a time: tap one to play it, its star to let
// it go. Above them, the station playing, to add when it is not there yet.
static void drawFavs() {
  centred("Favourites", 50, u8g2_font_helvB14_tf, FG);
  backButton(C - 72, 47, 20);   // left of the title, clear of the rim
  addHit(C - 70, 26, 140, 34, "home");   // the title leads back too
  const std::vector<Station>& l = favourites::list();
  const Station& now = player::current();
  bool canAdd = now.url.length() && !favourites::has(keyOf(now));
  int top = 70;
  if (canAdd) {
    int w = 260, h = 44, cy = 94, x0 = C - w / 2;
    gfx->drawRoundRect(x0, cy - h / 2, w, h, h / 2, ACCENT);
    for (int k = 0; k < 2; k++) star(x0 + 26, cy, 9 - k, false, ACCENT);
    gfx->setFont(u8g2_font_helvB12_tf); gfx->setTextColor(ACCENT);
    gfx->setCursor(x0 + 44, cy + 5); gfx->print(fit("Add " + now.name, u8g2_font_helvB12_tf, w - 64));
    addHit(x0, cy - h / 2 - 6, w, h + 12, "fav");
    top = 124;
  }
  int n = l.size();
  if (!n) {
    centred("No favourites yet", top + 50, u8g2_font_helvB18_tf, FG);
    centred("Add the station playing,", top + 80, u8g2_font_helvR12_tf, DIM);
    centred("or on your phone", top + 98, u8g2_font_helvR12_tf, DIM);
  }
  const int ROW_H = 54;
  String cur = keyOf(now);
  for (int r = 0; r < FAV_ROWS && favTop + r < n; r++) {
    int i = favTop + r, rt = top + r * ROW_H, cy = rt + ROW_H / 2;
    const Station& st = l[i];
    bool on = keyOf(st) == cur;
    int w = min(chord(cy, 22), 290), x0 = C - w / 2;
    gfx->fillRoundRect(x0, rt + 3, w, ROW_H - 6, 14, on ? 0x2A27 : BTN);   // the playing one, lit a little
    gfx->setFont(u8g2_font_helvB14_tf); gfx->setTextColor(on ? ACCENT : FG);
    gfx->setCursor(x0 + 16, cy + 6); gfx->print(fit(st.name, u8g2_font_helvB14_tf, w - 72));
    star(x0 + w - 26, cy, 10, true, ACCENT);
    addHit(x0, rt + 1, w - 52, ROW_H - 2, "favplay:" + String(i));
    addHit(x0 + w - 52, rt + 1, 52, ROW_H - 2, "favstar:" + String(i));
  }
  if (n > FAV_ROWS) {
    int y = top + FAV_ROWS * ROW_H + 26;
    if (favTop >= FAV_ROWS) button(C - 70, y, 22, "up");
    if (favTop + FAV_ROWS < n) button(C + 70, y, 22, "down");
    centred(String(favTop / FAV_ROWS + 1) + " / " + String((n + FAV_ROWS - 1) / FAV_ROWS), y + 5, u8g2_font_helvR12_tf, DIM);
  }
}

static void draw() {
  hits.clear();
  gfx->fillScreen(BG);
  String pv = previewName;
  if (!busyText.length() && (pv == "charge" || (pv.isEmpty() && power::standby()))) { drawCharge(); pushFrame(); return; }
  if (!busyText.length() && pv.isEmpty() && viz) { drawViz(); pushFrame(); return; }
  if (pv == "busy" || busyText.length()) {
    brand(C - 44, 46);
    centred(busyText.length() ? busyText : "Updating firmware", C + 30, u8g2_font_helvB18_tf, FG);
    centred(busyText.startsWith("Starting") ? "Joining WiFi" : "Keep the radio powered", C + 58, u8g2_font_helvR12_tf, DIM);
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
  } else if (pv == "menu" || (pv.isEmpty() && screen == MENU)) {
    drawMenu();
  } else if (pv.isEmpty() && !net::connected()) {
    centred("Connecting to WiFi", C, u8g2_font_helvB18_tf, FG);
    centred("Press the button to set up WiFi", C + 28, u8g2_font_helvR12_tf, DIM);
  } else if (pv == "favs" || (pv.isEmpty() && screen == FAVS)) {
    drawFavs();
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
  Preferences p; p.begin("display", true); lowPowerOn = p.getBool("lowpower", false); p.end();
  Serial.println("[display] ready");
}

void display::loop() {
  if (!io) return;
  bool standing = power::standby();
  touchLoop();
  if (standing && !power::standby()) return;   // "Play radio": the player is not up until main starts it
  static unsigned long last = 0;
  static String shown;
  if (millis() - last < (viz ? 33UL : 100UL)) return;
  last = millis();
  if (power::standby()) {
    // Nothing of the radio has started: only the battery changes. The screen
    // dims after a minute; a touch brings it back (and does no more).
    if (!dimmed && millis() - touchAt > 60000) { dimmed = true; ledcWrite(PIN_LCD_BL, 18); }
    int pct = power::percent();
    String frame = pct < 100 && power::usb() && !dimmed ? String(millis() / 100) : "-";   // the fill moves while it charges
    String key = String(pct) + "|" + power::usb() + "|" + busyText + "|" + previewName + "|" + uiEpoch + "|" + frame;
    if (key != shown) { shown = key; draw(); }
    return;
  }
  if (dimmed) { dimmed = false; ledcWrite(PIN_LCD_BL, 220); }
  const Station& s = player::current();
  if (screen == PHONE && s.url != phoneFrom) { screen = HOME; uiEpoch++; }   // a station was picked on the phone
  // The visualiser: on the home screen, playing, untouched for a few seconds.
  bool canViz = screen == HOME && previewName.isEmpty() && busyText.isEmpty() && net::connected() && !net::portalOpen() &&
                player::running() && !player::paused() && !player::connecting() && millis() >= flashUntil && !tunerShowing();
  if (viz && (!canViz || lowPowerOn)) { viz = false; spectrum::enable(false); uiEpoch++; }
  // Low power: the same moment puts the screen to sleep instead. It wakes on a
  // touch, or when there is something to see (the station stopped or failed).
  if (asleep && !canViz) { asleep = false; ledcWrite(PIN_LCD_BL, 220); uiEpoch++; }
  if (lowPowerOn && !asleep && canViz && !down && millis() - touchAt > 8000) { asleep = true; ledcWrite(PIN_LCD_BL, 0); }
  if (asleep) return;
  if (!lowPowerOn && !viz && canViz && !down && millis() - touchAt > 8000) { viz = true; spectrum::enable(true); }
  if (viz) { draw(); shown = ""; return; }
  String key = String(power::percent()) + power::usb() + "|" + s.name + "|" + busyText + previewName + "|" + (net::joining().length() ? String((millis() / 400) % 3) : String("-")) + "|" + player::title() + "|" + player::codec() + "|" + player::volume() + "|" +
               player::running() + player::paused() + "|" + net::connected() + net::portalOpen() + "|" +
               (millis() < flashUntil ? flash : "") + "|" + screen + "|" + favTop + "|" + favourites::list().size() + "|" +
               drift::state() + drift::message() + tunerShowing() + driftLocked() + (millis() < wifiArmedUntil) + "|" + directory::source + "|" + directory::stations.size() + "|" + player::currentIndex() + "|" + uiEpoch +
               "|" + player::connecting() + (millis() < resetArmedUntil);
  if (key != shown) { shown = key; draw(); }
}

// Drawn at once: a firmware upload holds the main loop until it is done.
void display::busy(const String& text) { busyText = text; uiEpoch++; if (io) draw(); }
const uint16_t* display::frame() { return io ? gfx->getFramebuffer() : nullptr; }
int display::side() { return LCD_SIZE; }
void display::setLowPower(bool on) {
  lowPowerOn = on;
  Preferences p; p.begin("display", false); p.putBool("lowpower", on); p.end();
  if (!on && asleep) { asleep = false; ledcWrite(PIN_LCD_BL, 220); }
  touchAt = millis();   // either way, from now
  uiEpoch++;
}
bool display::lowPower() { return lowPowerOn; }

// The radio's own button: a menu, with the music paused while it shows.
// Pressed again (or Resume tapped), the menu goes and the music comes back.
void display::button() {
  if (!io) { player::togglePause(); return; }
  touchAt = millis();
  if (viz) { viz = false; spectrum::enable(false); }
  if (asleep) { asleep = false; ledcWrite(PIN_LCD_BL, 220); }
  if (screen != MENU) {
    screen = MENU; wifiArmedUntil = resetArmedUntil = 0;
    menuPaused = player::running() && !player::paused() && !player::connecting();
    if (menuPaused) player::togglePause();
  } else {
    screen = HOME;
    if (menuPaused && player::paused()) player::togglePause();
    menuPaused = false;
  }
  uiEpoch++;
}

// The knob: the volume, shown as a touch on − or + shows it. Like a touch,
// a turn brings the screen back from the visualiser or low-power sleep.
void display::knob(int steps) {
  if (!io) { player::setVolume(player::volume() + steps); return; }
  touchAt = millis();
  if (viz) { viz = false; spectrum::enable(false); uiEpoch++; }
  if (asleep) { asleep = false; ledcWrite(PIN_LCD_BL, 220); uiEpoch++; }
  volumeBy(steps);
}

void display::preview(const String& name) {
  if (name == "viz") { touchAt = 0; screen = HOME; previewName = ""; uiEpoch++; return; }   // shows once it may
  previewName = name == "off" ? "" : name;
  uiEpoch++;
}

#else
#include "player.h"
void display::begin() {}
void display::loop() {}
void display::busy(const String&) {}
const uint16_t* display::frame() { return nullptr; }
int display::side() { return 0; }
void display::preview(const String&) {}
void display::setLowPower(bool) {}
void display::button() { player::togglePause(); }
void display::knob(int steps) { player::setVolume(player::volume() + steps); }
bool display::lowPower() { return false; }
#endif
