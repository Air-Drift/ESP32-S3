#include "devinfo.h"
#include "config.h"
#include "net.h"
#include "player.h"
#include "directory.h"
#include <WiFi.h>
#include <esp_system.h>
#include <esp_chip_info.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <map>

// ---- CPU load from FreeRTOS run-time stats -------------------------------
// Each core's idle task accounts for its idle time; the rest is load.
struct TaskSample { uint32_t runtime; int core; UBaseType_t prio; uint32_t stackFree; float pct; };
static std::map<String, TaskSample> tasks;
static std::map<String, uint32_t>  lastRun;
static uint64_t lastTotal = 0;
static float coreLoad[2] = {0, 0};
static unsigned long lastSample = 0;

static void sample() {
  UBaseType_t n = uxTaskGetNumberOfTasks();
  TaskStatus_t* st = (TaskStatus_t*)malloc(n * sizeof(TaskStatus_t));
  if (!st) return;
  uint32_t total32 = 0;
  n = uxTaskGetSystemState(st, n, &total32);
  uint64_t now = esp_timer_get_time();                 // the stats clock is esp_timer, in µs
  uint64_t elapsed = lastTotal ? now - lastTotal : 0;
  std::map<String, TaskSample> fresh;
  float idle[2] = {0, 0};
  for (UBaseType_t i = 0; i < n; i++) {
    String name = st[i].pcTaskName;
    uint32_t prev = lastRun.count(name) ? lastRun[name] : st[i].ulRunTimeCounter;
    uint32_t delta = st[i].ulRunTimeCounter - prev;
    float pct = elapsed ? 100.0f * delta / (float)elapsed : 0;
    int core = -1;
#if CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
    core = (st[i].xCoreID == tskNO_AFFINITY) ? -1 : (int)st[i].xCoreID;
#endif
    if (name == "IDLE0") { idle[0] = pct; core = 0; }
    if (name == "IDLE1") { idle[1] = pct; core = 1; }
    fresh[name] = { st[i].ulRunTimeCounter, core, st[i].uxCurrentPriority, (uint32_t)st[i].usStackHighWaterMark * sizeof(StackType_t), pct };
    lastRun[name] = st[i].ulRunTimeCounter;
  }
  free(st);
  if (elapsed) {
    coreLoad[0] = constrain(100.0f - idle[0], 0.0f, 100.0f);
    coreLoad[1] = constrain(100.0f - idle[1], 0.0f, 100.0f);
    tasks.swap(fresh);
  }
  lastTotal = now;
}

void devinfo::begin() { sample(); }

void devinfo::loop() {
  if (millis() - lastSample < 2000) return;
  lastSample = millis();
  sample();
}

static const char* resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "power on";
    case ESP_RST_SW:        return "software restart";
    case ESP_RST_PANIC:     return "crash (panic)";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_EXT:       return "external reset pin";
    case ESP_RST_USB:       return "USB (flashing tool)";
    case ESP_RST_JTAG:      return "JTAG";
    case ESP_RST_EFUSE:     return "eFuse error";
    case ESP_RST_PWR_GLITCH:return "power glitch";
    case ESP_RST_CPU_LOCKUP:return "CPU lockup";
    default:                return "unknown";
  }
}

void devinfo::fill(JsonDocument& d) {
  d["board"] = BOARD_NAME;
  d["firmware"] = AD_VERSION;
  d["built"] = __DATE__ " " __TIME__;
  d["idf"] = ESP.getSdkVersion();
  d["arduino"] = ESP.getCoreVersion();
  d["uptime"] = (uint32_t)(esp_timer_get_time() / 1000000ULL);
  d["reset"] = resetReason();
  d["temperature"] = temperatureRead();

  esp_chip_info_t ci; esp_chip_info(&ci);
  JsonObject chip = d["chip"].to<JsonObject>();
  chip["model"] = ESP.getChipModel();
  chip["revision"] = ESP.getChipRevision();
  chip["cores"] = ci.cores;
  chip["mhz"] = ESP.getCpuFreqMHz();
  String feat;
  if (ci.features & CHIP_FEATURE_WIFI_BGN) feat += "WiFi 2.4 GHz";
  if (ci.features & CHIP_FEATURE_BLE) feat += ", Bluetooth LE";
  if (ci.features & CHIP_FEATURE_EMB_PSRAM) feat += ", embedded PSRAM";
  if (ci.features & CHIP_FEATURE_EMB_FLASH) feat += ", embedded flash";
  chip["features"] = feat;
  chip["mac"] = WiFi.macAddress();

  JsonObject flash = d["flash"].to<JsonObject>();
  flash["size"] = ESP.getFlashChipSize();
  flash["mhz"] = ESP.getFlashChipSpeed() / 1000000;
  flash["sketch"] = ESP.getSketchSize();
  flash["sketchFree"] = ESP.getFreeSketchSpace();

  JsonObject heap = d["heap"].to<JsonObject>();
  heap["size"] = ESP.getHeapSize();
  heap["free"] = ESP.getFreeHeap();
  heap["minFree"] = ESP.getMinFreeHeap();
  heap["maxBlock"] = ESP.getMaxAllocHeap();

  JsonObject psram = d["psram"].to<JsonObject>();
  psram["size"] = ESP.getPsramSize();
  psram["free"] = ESP.getFreePsram();
  psram["minFree"] = ESP.getMinFreePsram();

  JsonObject cpu = d["cpu"].to<JsonObject>();
  cpu["core0"] = serialized(String(coreLoad[0], 1));
  cpu["core1"] = serialized(String(coreLoad[1], 1));

  JsonObject wifi = d["wifi"].to<JsonObject>();
  wifi["connected"] = net::connected();
  wifi["portal"] = net::portalOpen();
  if (net::connected()) {
    wifi["ssid"] = WiFi.SSID();
    wifi["bssid"] = WiFi.BSSIDstr();
    wifi["rssi"] = WiFi.RSSI();
    wifi["channel"] = WiFi.channel();
    wifi["ip"] = WiFi.localIP().toString();
    wifi["gateway"] = WiFi.gatewayIP().toString();
    wifi["mask"] = WiFi.subnetMask().toString();
    wifi["dns"] = WiFi.dnsIP().toString();
    wifi["hostname"] = String(HOSTNAME) + ".local";
    wifi["txPower"] = serialized(String(WiFi.getTxPower() * 0.25f, 2));
  }

  const Station& s = player::current();
  JsonObject audio = d["audio"].to<JsonObject>();
  audio["running"] = player::running();
  audio["paused"] = player::paused();
  audio["station"] = s.name;
  audio["url"] = s.url;
  audio["codec"] = player::codec();
  audio["sampleRate"] = player::sampleRate();
  audio["buffer"] = player::buffered();
  audio["bufferSize"] = player::bufferSize();
  audio["volume"] = player::volume();
  audio["decodeCore"] = player::decodeCore();
  audio["i2s"] = String("BCLK ") + PIN_I2S_BCLK + ", LRC " + PIN_I2S_LRC + ", DOUT " + PIN_I2S_DOUT + (PIN_I2S_MCLK >= 0 ? String(", MCLK ") + PIN_I2S_MCLK : String());
  audio["list"] = directory::source;
  audio["listCount"] = directory::stations.size();

  d["display"] = HAS_DISPLAY ? "round 360×360 touch screen" : "none";

  JsonArray tl = d["tasks"].to<JsonArray>();
  for (auto& kv : tasks) {
    JsonObject t = tl.add<JsonObject>();
    t["name"] = kv.first;
    t["core"] = kv.second.core;
    t["prio"] = kv.second.prio;
    t["stackFree"] = kv.second.stackFree;
    t["cpu"] = serialized(String(kv.second.pct, 1));
  }
}
