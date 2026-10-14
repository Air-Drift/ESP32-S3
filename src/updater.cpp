#include "updater.h"
#include "config.h"
#include "player.h"
#include "display.h"
#include "net.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>

static volatile bool wantInstall = false, busy = false;

bool updater::latest(String& version, String& error) {
  version = ""; error = "";
  if (!net::connected()) { error = "no WiFi"; return false; }
  WiFiClientSecure client; client.setInsecure();   // public release files; no CA bundle to keep current
  HTTPClient http;
  http.setUserAgent(AD_USER_AGENT);
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, String(UPDATE_BASE) + "version.txt")) { error = "bad url"; return false; }
  int code = http.GET();
  if (code != 200) { error = code > 0 ? "HTTP " + String(code) : http.errorToString(code); http.end(); return false; }
  version = http.getString(); version.trim();
  http.end();
  if (version.isEmpty() || version.length() > 20) { error = "no version in the answer"; version = ""; return false; }
  return true;
}

// "0.3.10" is newer than "0.3.9": compare number by number.
bool updater::newer(const String& v) {
  int a[3] = {0, 0, 0}, b[3] = {0, 0, 0};
  sscanf(v.c_str(), "%d.%d.%d", &a[0], &a[1], &a[2]);
  sscanf(AD_VERSION, "%d.%d.%d", &b[0], &b[1], &b[2]);
  for (int i = 0; i < 3; i++) if (a[i] != b[i]) return a[i] > b[i];
  return false;
}

void updater::install() { wantInstall = true; }
bool updater::installing() { return busy; }

void updater::loop() {
  if (!wantInstall || busy) return;
  wantInstall = false; busy = true;
  String url = String(UPDATE_BASE) + "airdrift-" AD_ENV ".bin";
  Serial.printf("[update] fetching %s\n", url.c_str());
  player::stop();                      // the decoder's CPU and RAM go to the download
  display::busy("Updating firmware");
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  http.setUserAgent(AD_USER_AGENT);
  http.setTimeout(30000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(client, url);
  httpUpdate.rebootOnUpdate(false);
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);   // it resets the client's own setting; the release file is behind a redirect
  httpUpdate.onProgress([](int done, int total) {
    static int shown = -1;
    int pct = total > 0 ? done * 100 / total : 0;
    if (pct / 5 != shown / 5) { shown = pct; display::busy("Updating firmware " + String(pct) + "%"); }
  });
  t_httpUpdate_return r = httpUpdate.update(http, AD_VERSION);
  if (r == HTTP_UPDATE_OK) {
    Serial.println("[update] installed; restarting");
    display::busy("Restarting");
    delay(300);
    ESP.restart();
  }
  Serial.printf("[update] failed: %s\n", httpUpdate.getLastErrorString().c_str());
  display::busy("");
  busy = false;
}
