// Air Drift for ESP32-S3.
//
// Boot: WiFi (stored credentials, or a setup portal), then the last station
// plays again. Control it from the web page at http://airdrift.local, the
// serial console, the buttons and encoder in config.h, or the touch screen
// on a board that has one. When a USB cable is what switched it on, it waits
// on a charge screen instead, until the screen or the button asks for the radio.
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
#include "updater.h"
#include "power.h"
#include "coverart.h"
#include "drift.h"

static bool resumed = false;
static bool radioOn = false;

static void startRadio() {
  radioOn = true;
  player::begin();
  controls::begin();
  net::begin();
  webui::begin();
  devinfo::begin();
}

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(5);   // never stall the radio because nobody is reading the USB port
#endif
  power::begin();
  delay(300);
  Serial.printf("\nAir Drift ESP32 %s on %s  (%s, %u KB PSRAM)\n", AD_VERSION, BOARD_NAME, ESP.getChipModel(), ESP.getPsramSize() / 1024);
  if (!psramFound()) Serial.println("!! no PSRAM: the decoder needs it. Build with the env for this board's PSRAM type.");
  favourites::begin();
  display::begin();
  console::begin();
  if (power::standby()) {
    Serial.printf("[power] switched on by USB: charge screen (battery %d%%, %d mV)\n", power::percent(), power::millivolts());
    if (PIN_BTN_PLAY >= 0) pinMode(PIN_BTN_PLAY, INPUT_PULLUP);
  } else {
    startRadio();
  }
}

void loop() {
  power::loop();
  console::loop();
  display::loop();
  if (!radioOn) {
    if (PIN_BTN_PLAY >= 0 && digitalRead(PIN_BTN_PLAY) == LOW) power::wake();
    if (!power::standby()) {
      display::busy("Starting the radio");
      startRadio();
      display::busy("");
      while (PIN_BTN_PLAY >= 0 && digitalRead(PIN_BTN_PLAY) == LOW) delay(10);   // that press is spent
    }
    delay(10);
    return;
  }
  net::loop();
  webui::loop();
  controls::loop();
  player::loop();
  updater::loop();
  devinfo::loop();
  drift::loop();
#if HAS_DISPLAY
  coverart::loop();
#endif
  if (!resumed && net::connected()) {
    resumed = true;
    if (AUTO_RESUME) player::resumeLast();
  }
  delay(2);
}
