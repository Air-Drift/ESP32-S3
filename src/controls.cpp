#include "controls.h"
#include "config.h"
#include "player.h"
#include "display.h"
#include <Arduino.h>
#include <atomic>
#if KNOB_TWO_SWITCH
#include <esp_timer.h>
#endif

struct Button { int pin; bool down = false; unsigned long at = 0; };
static Button btnPlay{PIN_BTN_PLAY}, btnNext{PIN_BTN_NEXT}, btnPrev{PIN_BTN_PREV};
// Counted by the encoder's interrupt or the knob's timer task, taken by the loop.
static std::atomic<int> encDelta{0};

#if KNOB_TWO_SWITCH
// The knob's turn is two switches, not a quadrature encoder: one direction
// pulses A low, the other pulses B. Both are polled every 3 ms, and a pin
// coming back high counts only if it stayed low for two polls, which drops
// contact bounce. The vendor's own demo reads it this way.
struct Channel { int pin; int dir; uint8_t prev = 1, lowTicks = 0; };
static Channel chA{PIN_ENC_A, +1}, chB{PIN_ENC_B, -1};
static esp_timer_handle_t knobTimer = nullptr;

static void pollChannel(Channel& c) {
  uint8_t now = digitalRead(c.pin);
  if (now == LOW) {
    c.lowTicks = c.prev == LOW ? c.lowTicks + 1 : 0;
  } else {
    if (c.prev == LOW && c.lowTicks + 1 >= 2) encDelta += c.dir;
    c.lowTicks = 0;
  }
  c.prev = now;
}

static void onKnobTick(void*) { pollChannel(chA); pollChannel(chB); }
#elif PIN_ENC_A >= 0 && PIN_ENC_B >= 0
static volatile unsigned long encAt = 0;

// One count per falling edge of A; B says which way. Mechanical encoders
// bounce, so edges closer than 2 ms are the same click.
static void IRAM_ATTR onEncoder() {
  unsigned long now = millis();
  if (now - encAt < 2) return;
  encAt = now;
  encDelta += digitalRead(PIN_ENC_B) ? 1 : -1;
}
#endif

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
#if KNOB_TWO_SWITCH
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  esp_timer_create_args_t t = {};
  t.callback = onKnobTick;
  t.dispatch_method = ESP_TIMER_TASK;
  t.name = "knob";
  if (esp_timer_create(&t, &knobTimer) == ESP_OK) esp_timer_start_periodic(knobTimer, 3000);
#elif PIN_ENC_A >= 0 && PIN_ENC_B >= 0
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), onEncoder, FALLING);
#endif
}

void controls::loop() {
  if (pressed(btnPlay)) display::button();   // a menu on the round screen; play/pause without one
  if (pressed(btnNext)) player::next();
  if (pressed(btnPrev)) player::prev();
  if (int d = encDelta.exchange(0)) {
    display::knob(d);
    Serial.printf("[controls] volume %d\n", player::volume());
  }
}
