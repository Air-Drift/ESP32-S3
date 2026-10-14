#include "controls.h"
#include "config.h"
#include "player.h"
#include <Arduino.h>

struct Button { int pin; bool down = false; unsigned long at = 0; };
static Button btnPlay{PIN_BTN_PLAY}, btnNext{PIN_BTN_NEXT}, btnPrev{PIN_BTN_PREV};
static volatile int encDelta = 0;
static volatile unsigned long encAt = 0;

// One count per falling edge of A; B says which way. Mechanical encoders
// bounce, so edges closer than 2 ms are the same click.
static void IRAM_ATTR onEncoder() {
#if PIN_ENC_A >= 0 && PIN_ENC_B >= 0
  unsigned long now = millis();
  if (now - encAt < 2) return;
  encAt = now;
  encDelta += digitalRead(PIN_ENC_B) ? 1 : -1;
#endif
}

static bool pressed(Button& b) {
  if (b.pin < 0) return false;
  bool now = digitalRead(b.pin) == LOW;
  if (now != b.down && millis() - b.at > 40) {
    b.down = now; b.at = millis();
    return now;
  }
  return false;
}

void controls::begin() {
  for (Button* b : { &btnPlay, &btnNext, &btnPrev }) if (b->pin >= 0) pinMode(b->pin, INPUT_PULLUP);
#if PIN_ENC_A >= 0 && PIN_ENC_B >= 0
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), onEncoder, FALLING);
#endif
}

void controls::loop() {
  if (pressed(btnPlay)) player::togglePause();
  if (pressed(btnNext)) player::next();
  if (pressed(btnPrev)) player::prev();
  if (encDelta) {
    int d = encDelta; encDelta = 0;
    player::setVolume(player::volume() + d);
    Serial.printf("[controls] volume %d\n", player::volume());
  }
}
