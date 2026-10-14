#include "net.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <vector>
#include <algorithm>
#include "player.h"
#include "favourites.h"

static WiFiManager wm;

// ---- The setup pages ---------------------------------------------------------
// Our own, in Air Drift's colours, bound before WiFiManager's so they win the
// route lookup; WiFiManager still does the captive-portal detection and the
// access point. The logo is icons/icon-dark.svg from the site, inlined.
static const char SETUP_CSS[] PROGMEM =
  "*{box-sizing:border-box}"
  "body{margin:0;background:#08090a;color:#eef2f5;font-family:Archivo,system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;-webkit-text-size-adjust:100%}"
  "main{max-width:420px;margin:0 auto;padding:40px 20px 48px}"
  ".logo{width:64px;height:64px;background:url(\"data:image/svg+xml,"
  "%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 512 512'%3E%3Crect fill='%2346d98a' width='512' height='512' rx='143'/%3E"
  "%3Cg stroke='%2304150c' fill='none' stroke-width='2.8' stroke-linecap='round' transform='translate(71.68 71.68) scale(15.36)'%3E"
  "%3Cpath d='M4 10v4'/%3E%3Cpath d='M8 7v10'/%3E%3Cpath d='M12 3v18'/%3E%3Cpath d='M16 7v10'/%3E%3Cpath d='M20 10v4'/%3E%3C/g%3E%3C/svg%3E\") center/contain no-repeat}"
  ".brand{font-weight:700;font-size:26px;letter-spacing:-.01em;margin:14px 0 2px}.brand span{color:#46d98a}"
  ".tag{color:#8b97a2;margin:0 0 32px;font-size:15px}"
  "h2{font-size:24px;margin:0 0 6px;letter-spacing:-.01em}"
  ".lead{color:#b4bec7;margin:0 0 20px;line-height:1.45}"
  ".msg{background:#16191c;border-left:4px solid #e5533d;padding:10px 14px;border-radius:8px;margin:0 0 16px;line-height:1.4}"
  ".nets{display:flex;flex-direction:column;gap:8px;margin:0 0 18px}"
  ".net{display:flex;justify-content:space-between;align-items:center;width:100%;background:#16191c;color:#eef2f5;border:1px solid #23282d;border-radius:12px;padding:12px 14px;font:inherit;font-size:16px;text-align:left}"
  ".net.on{border-color:#46d98a;background:#0f1a14}"
  ".net .n{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
  ".r{display:flex;align-items:center;flex:none}"
  ".sig{display:inline-flex;gap:2px;align-items:flex-end;height:14px;margin-left:10px}.sig i{display:block;width:4px;background:#3b4248;border-radius:1px}.sig i.on{background:#46d98a}"
  ".sig i:nth-child(1){height:5px}.sig i:nth-child(2){height:8px}.sig i:nth-child(3){height:11px}.sig i:nth-child(4){height:14px}"
  ".lock{color:#8b97a2;font-size:12px;margin-left:8px}"
  "label{display:block;color:#8b97a2;font-size:13px;margin:0 0 12px}"
  "input{display:block;width:100%;margin-top:6px;background:#16191c;color:#eef2f5;border:1px solid #3b4248;border-radius:10px;padding:12px;font:inherit;font-size:16px}"
  "input:focus{outline:none;border-color:#46d98a}"
  "button[type=submit]{width:100%;background:#46d98a;color:#04150c;border:0;border-radius:12px;font:inherit;font-weight:700;font-size:17px;padding:14px;margin-top:6px}"
  ".hint{color:#8b97a2;font-size:13px;line-height:1.5;margin:20px 0 0}a{color:#46d98a}";

static const char SETUP_SCRIPT[] PROGMEM =
  "<script>function pick(b){document.getElementById('s').value=b.dataset.s;document.getElementById('p').focus();"
  "for(const n of document.querySelectorAll('.net'))n.classList.toggle('on',n===b)}</script>";

static bool portal = false;
static bool mdnsUp = false;
static String attemptSsid, failedSsid;   // the network the portal is trying, and the last one that failed
static unsigned long attemptAt = 0;

static String htmlEsc(const String& in) {
  String o; o.reserve(in.length() + 8);
  for (char c : in) {
    if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;"; else if (c == '\'') o += "&#39;"; else o += c;
  }
  return o;
}

static String setupPage(const String& body) {
  String page;
  page.reserve(6000);
  page += F("<!doctype html><html lang='en'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>Air Drift</title><style>");
  page += FPSTR(SETUP_CSS);
  page += F("</style></head><body><main><div class='logo'></div><div class='brand'>air<span>drift</span></div>"
            "<p class='tag'>Live radio from anywhere on earth.</p>");
  page += body;
  page += F("</main></body></html>");
  return page;
}

// The networks in range, strongest first, one entry per name. Cached briefly:
// captive-portal helpers reload the page more often than a person does.
static String networkList() {
  static String cached; static unsigned long cachedAt = 0;
  if (cached.length() && millis() - cachedAt < 15000) return cached;
  int n = WiFi.scanNetworks(false, false);
  struct Net { String ssid; int rssi; bool open; };
  std::vector<Net> nets;
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;
    bool dup = false;
    for (auto& e : nets) if (e.ssid == ssid) { dup = true; if (WiFi.RSSI(i) > e.rssi) e.rssi = WiFi.RSSI(i); break; }
    if (!dup) nets.push_back({ssid, WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN});
  }
  std::sort(nets.begin(), nets.end(), [](const Net& a, const Net& b) { return a.rssi > b.rssi; });
  String out;
  for (size_t i = 0; i < nets.size() && i < 12; i++) {
    int bars = nets[i].rssi >= -55 ? 4 : nets[i].rssi >= -67 ? 3 : nets[i].rssi >= -78 ? 2 : 1;
    out += "<button type='button' class='net' data-s=\"" + htmlEsc(nets[i].ssid) + "\" onclick='pick(this)'><span class='n'>" + htmlEsc(nets[i].ssid) + "</span><span class='r'><span class='sig'>";
    for (int b = 1; b <= 4; b++) out += b <= bars ? "<i class='on'></i>" : "<i></i>";
    out += "</span>";
    if (!nets[i].open) out += "<span class='lock'>&#128274;</span>";
    out += "</span></button>";
  }
  WiFi.scanDelete();
  cached = out; cachedAt = millis();
  return cached;
}

static void sendPage(const String& page) {
  wm.server->sendHeader("Cache-Control", "no-store");
  wm.server->send(200, "text/html; charset=utf-8", page);
}

static void handleSetup() {
  String body;
  if (attemptAt) {
    body += "<h2>Joining &ldquo;" + htmlEsc(attemptSsid) + "&rdquo;&hellip;</h2>"
            "<p class='lead'>Give it a few seconds. When the radio's screen shows a QR code it is online, and this setup network switches itself off.</p>"
            "<p class='lead'>Back on your own WiFi, scan that code or open <a href='http://" HOSTNAME ".local'>" HOSTNAME ".local</a> to pick a station.</p>"
            "<p class='hint'>Still here after a while? <a href='/'>Try again</a>.</p>";
    sendPage(setupPage(body));
    return;
  }
  body += F("<h2>You're almost there</h2><p class='lead'>Pick your home WiFi and the radio does the rest.</p>");
  if (failedSsid.length())
    body += "<div class='msg'>Couldn't join &ldquo;" + htmlEsc(failedSsid) + "&rdquo;. Check the password and try again.</div>";
  body += F("<form method='post' action='/wifisave'><div class='nets'>");
  body += networkList();
  body += F("</div><label>Network<input id='s' name='s' autocomplete='off' autocapitalize='none' required></label>"
            "<label>Password<input id='p' name='p' type='password' autocomplete='current-password'></label>"
            "<button type='submit'>Connect</button></form>"
            "<p class='hint'>Can't see your network? <a href='/'>Look again</a>. The radio joins 2.4&nbsp;GHz networks.</p>");
  body += FPSTR(SETUP_SCRIPT);
  sendPage(setupPage(body));
}

static void handleSave() {
  String ssid = wm.server->arg("s"), pass = wm.server->arg("p");
  ssid.trim();
  if (ssid.isEmpty()) { wm.server->sendHeader("Location", "/"); wm.server->send(302, "text/plain", ""); return; }
  attemptSsid = ssid; attemptAt = millis(); failedSsid = "";
  net::setCredentials(ssid, pass);
  handleSetup();
}

static void handleHidden() {   // WiFiManager's own pages: not part of this product
  wm.server->sendHeader("Location", "/");
  wm.server->send(302, "text/plain", "");
}

static void bindSetupPages() {
  wm.server->on("/", handleSetup);
  wm.server->on("/wifi", handleSetup);
  wm.server->on("/0wifi", handleSetup);
  wm.server->on("/wifisave", handleSave);
  for (const char* r : {"/info", "/i", "/param", "/paramsave", "/restart", "/r", "/exit", "/close", "/erase", "/status", "/update", "/u"})
    wm.server->on(r, handleHidden);
}

static void onConnected() {
  Serial.printf("[net] connected to \"%s\" as %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  if (!mdnsUp && MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    mdnsUp = true;
  }
  Serial.printf("[net] web page: http://%s.local  or  http://%s\n", HOSTNAME, WiFi.localIP().toString().c_str());
}

void net::begin() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setSleep(false);  // modem sleep stalls streams
  WiFi.setAutoReconnect(true);

  wm.setConfigPortalBlocking(false);
  wm.setConfigPortalTimeout(0);
  wm.setConnectTimeout(15);
  wm.setHostname(HOSTNAME);
  wm.setTitle("Air Drift");
  wm.setWebServerCallback(bindSetupPages);

  // Stored credentials first, without the portal: its web server would sit on
  // port 80 and the radio's own page could not take it afterwards.
  String saved = wm.getWiFiSSID(true);
  if (saved.length()) {
    Serial.printf("[net] joining \"%s\" ...\n", saved.c_str());
    WiFi.begin();
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(100);
  }
  if (WiFi.status() == WL_CONNECTED) {
    onConnected();
  } else {
    portal = true;
    wm.startConfigPortal(SETUP_AP_NAME);
    Serial.printf("[net] no WiFi yet. Join the \"%s\" network and pick yours,\n"
                  "      or type here:  wifi <ssid> <password>\n", SETUP_AP_NAME);
  }
}

void net::loop() {
  if (portal) {
    wm.process();
    if (WiFi.status() == WL_CONNECTED) {
      portal = false;
      attemptAt = 0;
      wm.stopConfigPortal();
      WiFi.mode(WIFI_STA);
      onConnected();
    } else if (attemptAt && millis() - attemptAt > 25000) {
      Serial.printf("[net] could not join \"%s\"; back to the setup page\n", attemptSsid.c_str());
      failedSsid = attemptSsid; attemptAt = 0;
      WiFi.disconnect(false, false);
      WiFi.mode(WIFI_AP_STA);
    }
  }
}

String net::joining() { return attemptAt ? attemptSsid : String(); }

bool   net::connected()  { return WiFi.status() == WL_CONNECTED; }
bool   net::portalOpen() { return portal; }
String net::ip()         { return WiFi.localIP().toString(); }

void net::setCredentials(const String& ssid, const String& pass) {
  Serial.printf("[net] joining \"%s\" ...\n", ssid.c_str());
  WiFi.persistent(true);
  WiFi.begin(ssid.c_str(), pass.c_str());
  if (!portal) {
    // Not in the portal: wait here so the console gets a straight answer.
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(100);
    if (WiFi.status() == WL_CONNECTED) onConnected();
    else Serial.println("[net] could not join; check the name and password");
  }
}

void net::factoryReset() {
  Serial.println("[net] factory reset: forgetting WiFi, favourites and the last station");
  favourites::clear();
  player::forgetState();
  wm.resetSettings();
  delay(200);
  ESP.restart();
}

void net::forget() {
  Serial.println("[net] forgetting WiFi and restarting into setup");
  wm.resetSettings();
  delay(200);
  ESP.restart();
}
