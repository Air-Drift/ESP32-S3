#include "player.h"
#include "config.h"
#include "directory.h"
#include <Audio.h>
#include <Preferences.h>
#ifdef ES8311_ADDR
#include "boards/es8311.h"
#endif

static Audio             audio;
static SemaphoreHandle_t lock;       // held while audio.loop() runs, and around connect/stop
static SemaphoreHandle_t titleLock;  // the stream title is written on the audio task
static Preferences       prefs;
static Station           cur;
static int               curIndex = -1;
static String            streamTitle, lastErr;
static uint32_t          bitrate = 0;
static bool              isPaused = false, wantPlaying = false;
static volatile bool     isConnecting = false;
static Station           pendingSt;             // a play request waiting for the audio task
static int               pendingIdx = -1;
static volatile bool     pendingSet = false;
static volatile bool     wantStop = false;       // stop, carried out by the audio task
static volatile bool     justConnected = false;  // a stream opened; the main loop notes it
static SemaphoreHandle_t pendLock;               // guards pendingSt between the two tasks
static bool              pendingToggle = false, volDirty = false;
static String            codecCache;
static unsigned long     pendingAt = 0, connectAt = 0;
static volatile bool     eofFlag = false;
static unsigned long     eofAt = 0;
static int               retries = 0;
static int               vol = DEFAULT_VOLUME;
static bool              debug = false;   // print the decoder's own info lines

#define LOCK()   xSemaphoreTakeRecursive(lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGiveRecursive(lock)

// Events arrive on the task that calls audio.loop(), so share them under the lock.
static void onAudio(Audio::msg_t m) {
  switch (m.e) {
    case Audio::evt_streamtitle:
      xSemaphoreTake(titleLock, portMAX_DELAY);
      streamTitle = m.msg ? m.msg : "";
      xSemaphoreGive(titleLock);
      isConnecting = false;
      Serial.printf("[now] %s\n", streamTitle.c_str());
      break;
    case Audio::evt_name:
      if (m.msg && *m.msg && cur.name.isEmpty()) cur.name = m.msg;
      break;
    case Audio::evt_bitrate:
      bitrate = m.msg ? strtoul(m.msg, nullptr, 10) / 1000 : 0;
      isConnecting = false;
      break;
    case Audio::evt_eof:
      eofFlag = true;
      break;
    case Audio::evt_log:
      Serial.printf("[audio] %s\n", m.msg ? m.msg : "");
      break;
    case Audio::evt_info:
      if (m.msg && strstr(m.msg, "stream ready")) isConnecting = false;
      if (debug) Serial.printf("[info] %s\n", m.msg ? m.msg : "");
      break;
    case Audio::evt_lasthost:
    case Audio::evt_icyurl:
    case Audio::evt_icydescription:
    default:
      break;
  }
}

static void connectNow();

// Everything that talks to the stream runs here: opening one takes DNS, a
// TCP (often TLS) handshake and the headers, seconds in which the main loop
// must keep drawing and answering taps.
static void audioTask(void*) {
  for (;;) {
    if (wantStop) {
      wantStop = false;
      LOCK(); audio.stopSong(); UNLOCK();
    }
    if (pendingSet && millis() - pendingAt >= 120) connectNow();
    LOCK();
    audio.loop();
    UNLOCK();
    vTaskDelay(1);
  }
}

// The play report goes to Radio Browser over TLS: its own short-lived task,
// so neither the decoder nor the screen waits for it.
static void reportTask(void* p) {
  String* id = (String*)p;
  directory::reportPlay(*id);
  delete id;
  vTaskDelete(nullptr);
}

static void remember() {
  prefs.putString("id", cur.id);
  prefs.putString("name", cur.name);
  prefs.putString("url", cur.url);
  prefs.putString("codec", cur.codec);
  prefs.putString("country", cur.country);
  prefs.putString("icon", cur.icon);
  prefs.putUShort("bitrate", cur.bitrate);
}

void player::forgetState() { prefs.clear(); }

void player::begin() {
  lock = xSemaphoreCreateRecursiveMutex();
  titleLock = xSemaphoreCreateMutex();
  pendLock = xSemaphoreCreateMutex();
  prefs.begin("airdrift", false);
  vol = prefs.getInt("vol", DEFAULT_VOLUME);
  Audio::audio_info_callback = onAudio;
#ifdef ES8311_ADDR
  // Waveshare 1.85C: codec first, then the amplifier, and a fixed 48 kHz so
  // the codec's clock setup holds for every stream.
  bool codec = es8311Init(ES8311_ADDR);
  if (codec) {
    Serial.println("[player] ES8311 codec up (1.85C)");
  } else {
    Serial.println("[player] no ES8311: plain PCM5101 board (1.85)");
  }
#endif
#if PIN_PA_EN >= 0
  pinMode(PIN_PA_EN, OUTPUT);
  digitalWrite(PIN_PA_EN, HIGH);
#endif
  audio.setPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT, PIN_I2S_MCLK >= 0 ? PIN_I2S_MCLK : I2S_GPIO_UNUSED);
  // The codec follows the master clock at any rate, so streams play at their
  // own rate: the library's resampler falls behind real time (`resample on`
  // to try it).
  audio.setVolume(vol);
  audio.setConnectionTimeout(6000, 12000);
  // Core 1 does audio; WiFi, the web page and the console share core 0.
  xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 2, nullptr, 1);
  Serial.printf("[player] I2S on BCLK %d, LRC %d, DOUT %d; volume %d\n", PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT, vol);
}

// A play request: noted at once so the screen and the page can show
// "connecting", and carried out by the audio task, so the connect (which
// can take seconds) blocks nobody who is drawing.
bool player::play(const Station& s, int index) {
  if (s.url.isEmpty()) { lastErr = "station has no stream"; return false; }
  cur = s; curIndex = index;
  streamTitle = ""; bitrate = 0; lastErr = "";
  isPaused = false; eofFlag = false; retries = 0;
  wantPlaying = true; isConnecting = true;
  xSemaphoreTake(pendLock, portMAX_DELAY);
  pendingSt = s; pendingIdx = index;
  xSemaphoreGive(pendLock);
  pendingAt = millis(); pendingSet = true;
  return true;
}

// On the audio task.
static void connectNow() {
  xSemaphoreTake(pendLock, portMAX_DELAY);
  Station s = pendingSt;
  xSemaphoreGive(pendLock);
  pendingSet = false;
  LOCK();
  audio.stopSong();
  Serial.printf("[player] %s  (%s %s %u kbps)\n", s.name.c_str(), s.country.c_str(), s.codec.c_str(), s.bitrate);
  Serial.printf("[player] %s\n", s.url.c_str());
  bool ok = audio.connecttohost(s.url.c_str());
  if (ok && !wantPlaying && !pendingSet) { audio.stopSong(); ok = false; }   // stopped while it was opening
  UNLOCK();
  connectAt = millis();
  if (ok) {
    justConnected = true;
  } else if (wantPlaying) {
    wantPlaying = false; isConnecting = false;
    lastErr = "could not open the stream";
    Serial.println("[player] " + lastErr);
  }
}

bool player::playIndex(int i) {
  if (i < 0 || i >= (int)directory::stations.size()) { lastErr = "no such station number"; return false; }
  return play(directory::stations[i], i);
}

void player::stop() {
  wantPlaying = false; isConnecting = false; pendingSet = false;
  wantStop = true;                       // the audio task stops it; never wait behind a connect here
  isPaused = false; streamTitle = "";
  Serial.println("[player] stopped");
}

// Pause and volume are noted at once and applied on the next loop, so a tap
// or a click never waits behind the decoder's network reads.
void player::togglePause() {
  if (isConnecting || pendingSet) return;
  if (wantPlaying || isPaused) { isPaused = !isPaused; pendingToggle = !pendingToggle; }
  else if (!cur.url.isEmpty()) play(cur, curIndex);
}

void player::next() {
  int n = directory::stations.size();
  if (!n) return;
  playIndex(curIndex < 0 ? 0 : (curIndex + 1) % n);
}

void player::prev() {
  int n = directory::stations.size();
  if (!n) return;
  playIndex(curIndex <= 0 ? n - 1 : curIndex - 1);
}

void player::setVolume(int v) {
  vol = constrain(v, 0, 21);
  volDirty = true;
}

int  player::volume()       { return vol; }
bool player::running()      { return (wantPlaying && !isConnecting) || isPaused; }
bool player::paused()       { return isPaused; }
bool player::connecting()   { return isConnecting; }
const Station& player::current() { return cur; }
int  player::currentIndex() { return curIndex; }
String player::title()      { xSemaphoreTake(titleLock, portMAX_DELAY); String t = streamTitle; xSemaphoreGive(titleLock); return t; }
String player::lastError()  { return lastErr; }
uint32_t player::buffered()  { return audio.inBufferFilled(); }
uint32_t player::bufferSize(){ return audio.getInBufferSize(); }
uint32_t player::sampleRate(){ return audio.getSampleRate(); }
void player::setDebug(bool on) { debug = on; }
static int decodeCoreId = 0;
int  player::decodeCore() { return decodeCoreId; }
void player::setDecodeCore(int core) {
  decodeCoreId = core;
  LOCK(); audio.setAudioTaskCore(core); UNLOCK();
  Serial.printf("[player] decoding on core %d\n", core);
}
void player::setResample(bool on) {
  LOCK(); audio.setOutputSampleRate(on ? Audio::SR_48000 : Audio::SR_ORIGIN); UNLOCK();  // reconfigures I2S: keep the lock
  Serial.printf("[player] output %s\n", on ? "resampled to 48 kHz" : "at the stream's own rate");
}

String player::codec()      { return codecCache; }

static void refreshCodec() {
  String c = audio.isRunning() ? audio.getCodecname() : cur.codec;
  uint32_t br = bitrate ? bitrate : audio.getBitRate() / 1000;
  if (!br) br = cur.bitrate;
  codecCache = c.isEmpty() ? String() : (br ? c + " " + br + " kbps" : c);
}

void player::resumeLast() {
  if (!prefs.isKey("url")) return;
  Station s;
  s.id = prefs.getString("id", ""); s.name = prefs.getString("name", ""); s.url = prefs.getString("url", "");
  s.codec = prefs.getString("codec", ""); s.country = prefs.getString("country", ""); s.icon = prefs.getString("icon", "");
  s.bitrate = prefs.getUShort("bitrate", 0);
  if (s.url.length()) play(s);
}

// A dropped stream comes back on its own: three tries, a few seconds apart.
static void watchdog() {
  if (!eofFlag) return;
  if (!eofAt) { eofAt = millis(); return; }
  if (millis() - eofAt < 4000) return;
  eofFlag = false; eofAt = 0;
  if (!wantPlaying || cur.url.isEmpty()) return;
  if (retries++ >= 3) { lastErr = "stream ended"; wantPlaying = false; Serial.println("[player] stream ended; giving up"); return; }
  Serial.printf("[player] stream ended; reconnecting (%d/3)\n", retries);
  int r = retries;
  player::play(cur, curIndex);
  retries = r;
}

namespace player {
void loop() {
  if (pendingToggle) {
    pendingToggle = false;
    audio.pauseResume();   // a flag in the library; safe from here, as its examples do
    Serial.println(isPaused ? "[player] paused" : "[player] playing");
  }
  if (volDirty) {
    volDirty = false;
    audio.setVolume(vol);
    prefs.putInt("vol", vol);
  }
  static unsigned long codecAt = 0;
  if (millis() - codecAt > 500) { codecAt = millis(); refreshCodec(); }
  if (justConnected) {
    justConnected = false;
    remember();
    String* id = new String(cur.id);
    if (xTaskCreate(reportTask, "report", 12288, id, 1, nullptr) != pdPASS) delete id;
  }
  if (isConnecting && !pendingSet && millis() - connectAt > 1500) {
    bool running = audio.isRunning() && audio.inBufferFilled() > 4096;
    if (running) isConnecting = false;
    else if (millis() - connectAt > 20000) { isConnecting = false; if (lastErr.isEmpty()) lastErr = "stream is not answering"; }
  }
  watchdog();
}
}
