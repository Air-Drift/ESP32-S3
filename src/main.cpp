// Air Drift for ESP32-S3.
//
// Boot: WiFi (stored credentials, or a setup portal), then the last station
// plays again. Control it from the web page at http://airdrift.local, the
// serial console, the buttons and encoder in config.h, or the touch screen
// on a board that has one.
#include <Arduino.h>
#include "config.h"
#include "net.h"
#include "directory.h"
#include "player.h"
#include "webui.h"
#include "console.h"
#include "controls.h"
#include "display.h"
#include "devinfo.h"
#include "favourites.h"

static bool resumed = false;

void setup() {
#if PIN_PWR_HOLD >= 0
  pinMode(PIN_PWR_HOLD, OUTPUT);
  digitalWrite(PIN_PWR_HOLD, HIGH);
#endif
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(5);   // never stall the radio because nobody is reading the USB port
#endif
  delay(300);
  Serial.printf("\nAir Drift ESP32 %s on %s  (%s, %u KB PSRAM)\n", AD_VERSION, BOARD_NAME, ESP.getChipModel(), ESP.getPsramSize() / 1024);
  if (!psramFound()) Serial.println("!! no PSRAM: the decoder needs it. Build with the env for this board's PSRAM type.");
  favourites::begin();
  display::begin();
  player::begin();
  controls::begin();
  console::begin();
  net::begin();
  webui::begin();
  devinfo::begin();
}

void loop() {
  net::loop();
  webui::loop();
  console::loop();
  controls::loop();
  player::loop();
  display::loop();
  devinfo::loop();
  if (!resumed && net::connected()) {
    resumed = true;
    if (AUTO_RESUME) player::resumeLast();
  }
  delay(2);
}
