#include "drift.h"
#include "config.h"
#include "directory.h"
#include "player.h"
#include "net.h"
#include "countries.h"
#include <vector>

static const int POOL_COUNTRY = 40, POOL_MOOD = 60;   // the site takes 80 for a mood; the radio keeps 60
static const int MAX_RETRIES = 3;
static const unsigned long ANSWER_WITHIN = 20000;    // a drifted station that has not started by then failed

static drift::State st = drift::IDLE;
static unsigned long stAt = 0;
static String moodNow, placeNow, msg;
static int retries = 0;
static std::vector<String> recent;                   // ids of the last 40 picks

static volatile bool working = false, done = false;
static std::vector<Station> pool;                    // the task's find, handed over when done
static String taskErr;
static TaskHandle_t task = nullptr;

static void set(drift::State s) { st = s; stAt = millis(); }

// Weighted by the square root of a country's stations, as the site does: the
// big ones come up more often without drowning out the rest.
static const Country& weightedCountry() {
  float total = 0;
  for (int i = 0; i < COUNTRY_COUNT; i++) total += sqrtf(COUNTRIES[i].count);
  float r = (esp_random() / (float)UINT32_MAX) * total;
  for (int i = 0; i < COUNTRY_COUNT; i++) { r -= sqrtf(COUNTRIES[i].count); if (r <= 0) return COUNTRIES[i]; }
  return COUNTRIES[0];
}

static void fresh(std::vector<Station>& rows, int cap) {
  std::vector<Station> out;
  for (const Station& s : rows) {
    if ((int)out.size() >= cap) break;
    bool seen = false;
    for (const String& id : recent) if (id == s.id) seen = true;
    if (!seen && s.url.length()) out.push_back(s);
  }
  rows.swap(out);
}

static void pickTask(void* arg) {
  String* m = (String*)arg;
  String mood = *m; delete m;
  std::vector<Station> rows;
  taskErr = "";
  if (mood.length()) {
    if (directory::fetch("tag:" + mood, rows)) fresh(rows, POOL_MOOD);
  } else {
    for (int attempt = 0; attempt < 4 && rows.empty(); attempt++) {
      if (directory::fetch(String("country:") + weightedCountry().code, rows)) fresh(rows, POOL_COUNTRY);
    }
  }
  if (rows.empty()) taskErr = "Nothing answered on that part of the dial. Try again.";
  pool.swap(rows);
  done = true;
  working = false;
  vTaskSuspend(nullptr);   // the loop deletes it, as with the cover task
}

static void begin(const String& mood) {
  using namespace drift;
  if (working || !net::connected()) return;
  moodNow = mood; placeNow = "";
  set(TUNING);
  working = true; done = false;
  if (xTaskCreatePinnedToCoreWithCaps(pickTask, "drift", 12288, new String(mood), 1, &task, tskNO_AFFINITY, MALLOC_CAP_SPIRAM) != pdPASS) {
    working = false; task = nullptr; msg = "Could not start a drift"; set(FAILED);
  }
}

void drift::start(const String& mood) { retries = 0; msg = ""; begin(mood); }

String drift::countryName(const String& code) {
  for (int i = 0; i < COUNTRY_COUNT; i++) if (code.equalsIgnoreCase(COUNTRIES[i].code)) return COUNTRIES[i].name;
  return code.length() ? code : String("Somewhere");
}

void drift::loop() {
  if (!working && task) { vTaskDeleteWithCaps(task); task = nullptr; }
  if (done) {
    done = false;
    if (taskErr.length() || pool.empty()) { msg = taskErr; set(FAILED); return; }
    // The pool becomes the list, so next and previous move along it.
    directory::stations.swap(pool); pool.clear();
    directory::source = "drift:" + moodNow;
    int i = esp_random() % directory::stations.size();
    const Station& s = directory::stations[i];
    recent.push_back(s.id); if (recent.size() > 40) recent.erase(recent.begin());
    placeNow = countryName(s.country);
    player::play(s, i);
    set(LANDED);
    return;
  }
  // A drifted station that does not answer: drift on rather than sit there.
  if (st == LANDED && millis() - stAt < ANSWER_WITHIN + 1000) {
    bool failed = !player::running() && !player::connecting() && !player::paused() && player::lastError().length();
    bool stuck = player::connecting() && millis() - stAt > ANSWER_WITHIN;
    if ((failed || stuck) && retries < MAX_RETRIES) {
      retries++;
      msg = player::current().name + " didn't answer. Drifting on...";
      begin(moodNow);
    }
  }
}

drift::State drift::state() { return st; }
unsigned long drift::since() { return stAt; }
String drift::mood() { return moodNow; }
String drift::place() { return placeNow; }
String drift::message() { return msg; }
