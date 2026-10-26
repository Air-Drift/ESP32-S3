#include "power.h"
#include "config.h"
#include <Preferences.h>

#ifdef PIN_BAT_ADC
// The cell feeds GPIO 8 through a 200K/100K divider, so the pin sees a third
// of it; Waveshare's own demo trims the result by 0.9945. On the 1.85C the
// divider hangs after the battery switch: switched off, the cell still
// charges but cannot be measured.
static float mv = 0;                 // smoothed, at the cell
static unsigned long lastRead = 0;
static Preferences prefs;            // "battery": what this radio has haveCurve about its cell

static float readCell() {
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogReadMilliVolts(PIN_BAT_ADC);
  return sum / 8.0f * 3.0f / 0.9945f;
}

// ---- Learning the cell -------------------------------------------------
// Every battery differs in resistance and age, so the radio measures its own.
//
// The liftMv: how far charging raises the reading above where the cell rests.
// It is the step in voltage when the cable goes in or comes out, once settled.
static int liftMv = 50;                // mV, until measured
static bool wasUsb = false;
static float stepFrom = 0;           // the reading as the cable went in or out
static unsigned long stepAt = 0;     // when; 0 when there is nothing to measure
static const unsigned long SETTLE = 30000;

// The curve: one run from full to flat, a sample every two minutes of running
// time (switched off, it pauses; charging throws it away), becomes the voltage
// at each tenth of the run. Until a run completes, a generic Li-ion table.
static const int RUN_MAX = 512;
static const unsigned long RUN_EVERY = 120000, SAVE_EVERY = 600000;
static uint16_t run[RUN_MAX];
static int runN = 0, runSaved = 0;
static unsigned long runAt = 0, savedAt = 0;
static uint16_t curve[11];           // mV at 0%, 10% ... 100%
static bool haveCurve = false;

static void saveRun() { prefs.putBytes("run", run, runN * 2); runSaved = runN; savedAt = millis(); }
static void dropRun() { runN = runSaved = 0; prefs.remove("run"); }
static bool runComplete() { return runN >= 30 && run[0] >= 4050 && run[runN - 1] <= 3600; }

// Sample i of n had (n-1-i)/(n-1) of the run still to go.
static void learnCurve() {
  for (int k = 0; k <= 10; k++) {
    int i = (int)lroundf((runN - 1) * (10 - k) / 10.0f);
    int a = max(0, i - 2), b = min(runN - 1, i + 2), sum = 0;   // a few neighbours, against noise
    for (int j = a; j <= b; j++) sum += run[j];
    curve[k] = sum / (b - a + 1);
  }
  for (int k = 1; k <= 10; k++) if (curve[k] <= curve[k - 1]) curve[k] = curve[k - 1] + 1;
  haveCurve = true;
  prefs.putBytes("curve", curve, sizeof curve);
  Serial.printf("[power] haveCurve this battery from a %d minute run: %u mV empty, %u mV half, %u mV full\n",
                runN * 2, curve[0], curve[5], curve[10]);
  dropRun();
}

// The cable is in: a finished run is haveCurve, an unfinished one is no use.
static void endRun() {
  if (!runN) return;
  if (runComplete()) learnCurve(); else dropRun();
}

static void learnLoop(bool usb) {
  if (usb != wasUsb) { wasUsb = usb; stepFrom = mv; stepAt = millis(); if (usb) endRun(); }
  if (stepAt && millis() - stepAt >= SETTLE) {
    stepAt = 0;
    float d = fabsf(mv - stepFrom);
    if (stepFrom > 2500 && mv > 2500 && d >= 10 && d <= 300) {
      liftMv = (liftMv * 2 + (int)d) / 3;
      prefs.putShort("liftMv", liftMv);
    }
  }
  if (usb || mv < 2500 || stepAt) return;   // on the cable, switched off, or still settling
  if (!runN && mv < 4050) return;            // a run starts from full
  if (runN && mv > run[runN - 1] + 80) { dropRun(); return; }   // rising: a charger the radio cannot see
  if (runN && millis() - runAt < RUN_EVERY) return;
  runAt = millis();
  if (runN < RUN_MAX) run[runN++] = (uint16_t)mv;
  if (runN - runSaved && (runSaved == 0 || millis() - savedAt >= SAVE_EVERY)) saveRun();
}
#endif

static bool standing = false;

static bool hostPresent() {
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
  return HWCDC::isPlugged();
#else
  return false;
#endif
}

void power::begin() {
#if PIN_PWR_HOLD >= 0
  pinMode(PIN_PWR_HOLD, OUTPUT);
  digitalWrite(PIN_PWR_HOLD, HIGH);
#endif
#ifdef PIN_BAT_ADC
  analogSetPinAttenuation(PIN_BAT_ADC, ADC_11db);
  mv = readCell();
  // Only a cold start can be a cable going in; a reboot or an update means
  // the radio was already on. A computer takes a moment to start talking.
  unsigned long t0 = millis();
  if (esp_reset_reason() == ESP_RST_POWERON) {
    while (!hostPresent() && millis() - t0 < 1500) delay(20);
    standing = hostPresent();
  }
  prefs.begin("battery", false);
  liftMv = prefs.getShort("liftMv", 50);
  haveCurve = prefs.getBytes("curve", curve, sizeof curve) == sizeof curve;
  runN = runSaved = prefs.getBytes("run", run, sizeof run) / 2;
  wasUsb = hostPresent();
  if (runN) {
    // The radio went flat, was switched off, or was charged while off since
    // the last sample: flat finishes the run, charged spoils it.
    if (wasUsb || runComplete()) endRun();
    else if (mv > 2500 && mv > run[runN - 1] + 100) dropRun();
  }
#endif
}

void power::loop() {
#ifdef PIN_BAT_ADC
  if (millis() - lastRead < 1000) return;
  lastRead = millis();
  float v = readCell();
  // Smooth the wobble, but follow the switch at once: on the 1.85C the
  // divider is after the battery switch, so off reads nothing at all.
  if (fabsf(v - mv) > 400) mv = v; else mv += (v - mv) * 0.2f;
  learnLoop(hostPresent());
#endif
}

int power::millivolts() {
#ifdef PIN_BAT_ADC
  return mv > 2500 ? (int)mv : 0;   // below that the pin reads no cell at all
#else
  return 0;
#endif
}

int power::percent() {
#ifdef PIN_BAT_ADC
  int v = millivolts();
  if (!v) return -1;
  // Charging lifts the reading above where the cell rests (less as it nears
  // full); take that off so the level does not drop on unplugging.
  if (hostPresent()) v -= liftMv * constrain(4170 - v, 0, 100) / 100;
  if (haveCurve) {
    if (v >= curve[10]) return 100;
    if (v <= curve[0]) return 0;
    for (int k = 1; k <= 10; k++)
      if (v < curve[k]) return (k - 1) * 10 + 10 * (v - curve[k - 1]) / (curve[k] - curve[k - 1]);
    return 100;
  }
  // A Li-ion cell under a light load, until this one has been haveCurve.
  static const int generic[][2] = { {4180, 100}, {4100, 90}, {4000, 78}, {3900, 64}, {3800, 50},
                                    {3740, 38}, {3680, 24}, {3600, 12}, {3500, 5}, {3300, 0} };
  if (v >= generic[0][0]) return 100;
  for (int i = 1; i < 10; i++)
    if (v >= generic[i][0])
      return generic[i][1] + (v - generic[i][0]) * (generic[i - 1][1] - generic[i][1]) / (generic[i - 1][0] - generic[i][0]);
  return 0;
#else
  return -1;
#endif
}

bool power::usb() { return hostPresent(); }
bool power::standby() { return standing; }
void power::wake() { standing = false; }

#ifdef PIN_BAT_ADC
int  power::lift() { return liftMv; }
bool power::learned() { return haveCurve; }
int  power::runMinutes() { return runN * 2; }
void power::forget() { prefs.clear(); liftMv = 50; haveCurve = false; runN = runSaved = 0; }
#else
int  power::lift() { return 0; }
bool power::learned() { return false; }
int  power::runMinutes() { return 0; }
void power::forget() {}
#endif
