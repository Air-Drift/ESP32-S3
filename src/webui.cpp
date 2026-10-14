#include "webui.h"
#include "config.h"
#include "net.h"
#include "directory.h"
#include "player.h"
#include "devinfo.h"
#include "favourites.h"
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include "icons.h"
#include "screenshot.h"
#include "display.h"
#include <ArduinoJson.h>

static WebServer server(80);
static bool started = false;

static const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><link rel="icon" href="/favicon.svg" type="image/svg+xml"><link rel="icon" href="/favicon.png" sizes="32x32" type="image/png"><link rel="apple-touch-icon" href="/apple-touch-icon.png"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Air Drift</title>
<link rel="preconnect" href="https://fonts.googleapis.com"><link href="https://fonts.googleapis.com/css2?family=Archivo:wght@400;500;700&display=swap" rel="stylesheet">
<style>
:root{--bg:#08090a;--card:#16191c;--line:#242a2f;--line2:#3b4248;--fg:#eef2f5;--fg2:#b3bcc4;--mute:#8b97a2;--green:#46d98a;--green-hi:#a7f0c8;--on-green:#04150c;--tile:#46d98a;--bars:#04150c}
@media(prefers-color-scheme:light){:root{--bg:#dce2e6;--card:#ffffff;--line:#c9d2d8;--line2:#aab5bd;--fg:#0b0f12;--fg2:#2c343a;--mute:#4f5b64;--green:#117a47;--green-hi:#1f9c5e;--on-green:#ffffff;--tile:#117a47;--bars:#ffffff}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.4 Archivo,system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
main{max-width:640px;margin:0 auto;padding:16px}
.brand{display:flex;align-items:center;gap:12px;margin:4px 0 16px}.brand svg{width:40px;height:40px;flex:none}
.brand .word{font-size:29px;font-weight:500;letter-spacing:-.01em;line-height:1}.brand .word b{color:var(--green);font-weight:700}.brand small{color:var(--mute);font-size:13px;margin-left:auto}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px;margin-bottom:12px}
.now{display:flex;gap:12px;align-items:center}
.art{position:relative;flex:none;display:grid;place-items:center;border-radius:12px;overflow:hidden;background:var(--line);color:var(--mute);font-weight:600;font-family:"IBM Plex Mono",ui-monospace,SFMono-Regular,Menlo,monospace}
.art img{position:absolute;inset:0;width:100%;height:100%;object-fit:contain;background:#fff}.art img[hidden]{display:none}
#artwrap{display:contents}.now .art{width:56px;height:56px;font-size:17px}li .art{width:36px;height:36px;border-radius:8px;font-size:12px}
.now .t{min-width:0}.now b{display:block;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;font-weight:700}.now .t span{color:var(--fg2);font-size:14px;display:block;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.now .t span+span{color:var(--mute)}
.row{display:flex;gap:8px;align-items:center;margin-top:12px;flex-wrap:wrap}
button{font:inherit;font-weight:500;border:1px solid var(--line2);background:var(--card);color:var(--fg);border-radius:12px;padding:8px 12px;cursor:pointer}button:hover{border-color:var(--green)}
button.p{background:var(--green);color:var(--on-green);border-color:var(--green);font-weight:700}button.p:hover{background:var(--green-hi);border-color:var(--green-hi)}
input[type=range]{flex:1;min-width:120px;accent-color:var(--green)}input[type=text]{font:inherit;flex:1;min-width:0;padding:8px 10px;border:1px solid var(--line2);border-radius:8px;background:var(--bg);color:var(--fg)}input[type=text]:focus{outline:2px solid var(--green);outline-offset:1px}
.tabs button[aria-pressed=true]{background:var(--green);color:var(--on-green);border-color:var(--green);font-weight:700}
ul{list-style:none;margin:0;padding:0}li{display:flex;gap:10px;align-items:center;padding:8px 0;border-top:1px solid var(--line)}li:first-child{border-top:0}
li .n{flex:1;min-width:0}li .n b{display:block;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;font-weight:500}
li .n span{color:var(--mute);font-size:13px}li.on .n b{color:var(--green)}
.tabs2{display:flex;gap:6px;margin:0 0 12px}.tabs2 button{border-radius:999px;padding:6px 14px}.tabs2 button[aria-pressed=true]{background:var(--green);color:var(--on-green);border-color:var(--green);font-weight:700}
.kv{display:grid;grid-template-columns:minmax(9em,auto) 1fr;gap:5px 12px;font-size:14px;margin:0}.kv dt{color:var(--mute)}.kv dd{margin:0;min-width:0;overflow-wrap:anywhere}
.bar{height:6px;background:var(--line);border-radius:3px;overflow:hidden;margin-top:4px}.bar i{display:block;height:100%;background:var(--green)}
#fw input[type=file]{font:inherit;color:var(--fg2);flex:1;min-width:0}#fw .st{margin-top:10px;font-size:14px;color:var(--fg2)}
h2{font-size:13px;letter-spacing:.06em;text-transform:uppercase;color:var(--mute);margin:0 0 10px;font-weight:500}
table{width:100%;border-collapse:collapse;font-size:13px;font-family:"IBM Plex Mono",ui-monospace,SFMono-Regular,Menlo,monospace}td,th{text-align:left;padding:4px 6px;border-top:1px solid var(--line)}th{color:var(--mute);font-weight:500;border-top:0}td.r,th.r{text-align:right}
button.wait{opacity:.7}button.wait::after{content:"";display:inline-block;width:9px;height:9px;margin-left:7px;vertical-align:middle;border:2px solid currentColor;border-right-color:transparent;border-radius:50%;animation:spin .7s linear infinite}@keyframes spin{to{transform:rotate(360deg)}}
button.star{flex:none;width:44px;height:44px;border-radius:50%;padding:0;font-size:22px;line-height:1;color:var(--mute)}button.star.on{color:var(--green);border-color:var(--green)}
li .n b .f{color:var(--green);font-size:12px;margin-left:6px}
.err{color:#ff7d95;font-size:14px;margin-top:8px}.mute{color:var(--mute);font-size:14px}.mute a{color:var(--green)}
</style></head><body><main>
<div class="brand"><svg viewBox="0 0 512 512" aria-hidden="true"><rect fill="var(--tile)" width="512" height="512" rx="143"/><g stroke="var(--bars)" fill="none" stroke-width="2.8" stroke-linecap="round" transform="translate(71.68 71.68) scale(15.36)"><path d="M4 10v4"/><path d="M8 7v10"/><path d="M12 3v18"/><path d="M16 7v10"/><path d="M20 10v4"/></g></svg><span class="word">air<b>drift</b></span><small id="ver"></small></div>
<div class="tabs2"><button id="tabRadio" aria-pressed="true" onclick="show('radio')">Radio</button><button id="tabDevice" aria-pressed="false" onclick="show('device')">Device</button></div>
<div id="radio">
<div class="card">
 <div class="now"><span id="artwrap"><span class="art"></span></span><div class="t"><b id="name">Nothing playing</b><span id="title"></span><span id="codec"></span></div><button class="star" id="fav" title="Favourite" onclick="toggleFav(this)">&#9734;</button></div>
 <div class="row"><button onclick="go('/api/prev',this)">&#9664;&#9664;</button><button class="p" id="pp" onclick="togglePP(this)">&#9654;</button><button onclick="go('/api/next',this)">&#9654;&#9654;</button><button onclick="go('/api/stop',this)">&#9632;</button>
 <input type="range" id="vol" min="0" max="21" oninput="go('/api/volume?v='+this.value)"></div>
 <div class="row"><input type="text" id="link" placeholder="Paste an airdrift.stream link or a stream URL"><button onclick="playLink(this)">Play</button></div>
 <div class="err" id="err"></div>
</div>
<div class="card">
 <div class="row tabs" style="margin-top:0"><button onclick="load('favourites')" id="tabFav">&#9733; Favourites</button><button onclick="load('popular')">Popular</button><button onclick="load('trending')">Trending</button>
 <input type="text" id="cc" placeholder="Country (US)" size="9" style="flex:0 0 7.5em" onchange="load('country:'+this.value)"><input type="text" id="tag" placeholder="Genre (jazz)" style="flex:1 1 7em" onchange="load('tag:'+this.value)"></div>
 <div class="row"><input type="text" id="q" placeholder="Search stations by name" onkeydown="if(event.key=='Enter')search()"><button onclick="search()">Search</button></div>
 <p class="mute" id="src"></p><ul id="list"></ul>
</div>
<p class="mute">Lists come from <a href="https://airdrift.stream">airdrift.stream</a>. Logos load in your browser, not on the radio.</p>
</div>
<div id="device" hidden><div id="stats"></div>
<div class="card" id="fw"><h2>Firmware</h2><p class="mute" style="margin:0 0 10px">Running <span id="fwver"></span>. Upload a new build (<code>firmware.bin</code> from PlatformIO, or a release) and the radio restarts on it.</p>
<div class="row" style="margin-top:0"><input type="file" id="fwfile" accept=".bin,application/octet-stream"><button class="p" id="fwgo" onclick="update()">Update</button></div>
<div class="st" id="fwst"></div></div>
<p class="mute" id="shot" hidden>The screen right now: <a href="/screen.bmp">screen.bmp</a></p></div>
</main><script>
const $=id=>document.getElementById(id);let busy=false,inflight=0,ppOn=false;
function esc(s){return (s||'').replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
function initials(n){const w=String(n||'?').replace(/[^\p{L}\p{N}\s]/gu,' ').trim().split(/\s+/).filter(Boolean);if(!w.length)return '?';return (w.length===1?w[0].slice(0,2):w[0][0]+w[1][0]).toUpperCase()}
function hue(s){let h=0;for(let i=0;i<s.length;i++)h=(h*31+s.charCodeAt(i))|0;return Math.abs(h)%360}
function art(s){const h=hue(s.id||s.name||'');return `<span class="art" style="background:hsl(${h} 32% 17%);color:hsl(${h} 55% 68%)">${esc(initials(s.name))}${s.icon?`<img src="${esc(s.icon)}" alt="" loading="lazy" onerror="this.hidden=true" referrerpolicy="no-referrer">`:''}</span>`}
let artKey='';
let updating=false;
async function state(force){if(updating||(inflight&&!force))return;try{const s=await (await fetch('/api/state')).json();$('ver').textContent=s.version+' · '+s.ip;
 $('name').textContent=s.name||'Nothing playing';$('title').textContent=s.connecting?'Connecting…':(s.title||'');$('title').style.color=s.connecting?'var(--green)':'';$('codec').textContent=s.codec||'';
 const k=s.name+'|'+s.icon;if(k!==artKey){artKey=k;$('artwrap').innerHTML=s.name?art(s):'<span class="art"></span>';}
 ppOn=s.playing&&!s.paused;$('pp').innerHTML=ppOn?'&#10074;&#10074;':'&#9654;';if(document.activeElement!==$('vol'))$('vol').value=s.volume;
 $('err').textContent=s.error||'';$('fav').innerHTML=s.fav?'&#9733;':'&#9734;';$('fav').classList.toggle('on',!!s.fav);$('fav').hidden=!s.name;$('tabFav').textContent='\u2605 Favourites'+(s.favCount?' ('+s.favCount+')':'');document.querySelectorAll('#list li').forEach((li,i)=>li.classList.toggle('on',i===s.index&&s.source===curSource));
 if(s.source!==curSource&&!busy)list();}catch(e){}}
let curSource='';
function render(d){curSource=d.source;$('src').textContent=d.source?d.source.replace(':',': ')+' · '+d.stations.length+' stations':'';
 $('list').innerHTML=d.stations.map((s,i)=>`<li>${art(s)}<div class="n"><b>${esc(s.name)}${s.fav?'<span class="f">&#9733;</span>':''}</b><span>${esc(s.country)} · ${esc(s.codec)} ${s.bitrate||''}</span></div><button onclick="go('/api/play?i=${i}',this)">Play</button></li>`).join('');
 document.querySelectorAll('.tabs button').forEach(b=>b.setAttribute('aria-pressed',b.textContent.toLowerCase().replace(/[^a-z]/g,'')===(d.source||'').replace(/[^a-z]/g,'')));}
async function list(){try{render(await (await fetch('/api/list')).json())}catch(e){}}
async function load(src){if(!src||src.endsWith(':'))return;busy=true;$('src').textContent='Loading '+src+' …';try{const d=await (await fetch('/api/load?source='+encodeURIComponent(src))).json();if(d.error)$('err').textContent=d.error;else render(d)}catch(e){$('err').textContent='The radio did not answer'}busy=false}
async function search(){const q=$('q').value.trim();if(!q)return;busy=true;$('src').textContent='Searching …';try{const d=await (await fetch('/api/search?q='+encodeURIComponent(q))).json();if(d.error)$('err').textContent=d.error;else render(d)}catch(e){}busy=false}
async function go(u,btn){inflight++;if(btn)btn.classList.add('wait');try{await fetch(u)}catch(e){}inflight--;if(btn)btn.classList.remove('wait');state(true);setTimeout(()=>state(true),1500)}
async function toggleFav(b){b.classList.add('wait');try{const r=await (await fetch('/api/fav')).json();if(r.error)$('err').textContent=r.error;}catch(e){}b.classList.remove('wait');state(true);if(curSource==='favourites')list()}
function togglePP(b){ppOn=!ppOn;b.innerHTML=ppOn?'&#10074;&#10074;':'&#9654;';go('/api/pause',b)}
function playLink(b){const v=$('link').value.trim();if(v)go('/api/play?link='+encodeURIComponent(v),b)}
let view='radio';
function show(v){view=v;location.hash=v==='device'?'#device':'';$('radio').hidden=v!=='radio';$('device').hidden=v!=='device';$('tabRadio').setAttribute('aria-pressed',v==='radio');$('tabDevice').setAttribute('aria-pressed',v==='device');if(v==='device')device();}
const kb=n=>n>=1048576?(n/1048576).toFixed(2)+' MB':n>=1024?(n/1024).toFixed(1)+' KB':n+' B';
const dur=s=>{const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return (d?d+'d ':'')+(h?h+'h ':'')+m+'m '+(s%60)+'s'};
const sec=(t,rows)=>`<div class="card"><h2>${t}</h2><dl class="kv">${rows.map(([k,v])=>`<dt>${k}</dt><dd>${v}</dd>`).join('')}</dl></div>`;
const bar=(used,total,label)=>`${label}<div class="bar"><i style="width:${total?Math.min(100,100*used/total):0}%"></i></div>`;
async function device(){if(view!=='device'||updating)return;try{const d=await (await fetch('/api/device')).json();const w=d.wifi||{},a=d.audio||{},c=d.chip||{},f=d.flash||{},h=d.heap||{},p=d.psram||{},u=d.cpu||{};
 $('fwver').textContent=`${d.firmware}, built ${d.built}`;$('shot').hidden=!d.display||d.display==='none';$('stats').innerHTML=
  sec('Board',[['Board',esc(d.board)],['Chip',`${esc(c.model)} rev ${c.revision}, ${c.cores} cores at ${c.mhz} MHz`],['Features',esc(c.features)],['MAC',c.mac],['Flash',`${kb(f.size)} at ${f.mhz} MHz`],['PSRAM',kb(p.size)],['Display',esc(d.display)],['Temperature',(+d.temperature).toFixed(1)+' °C'],['Firmware',`${esc(d.firmware)}, built ${esc(d.built)}`],['ESP-IDF',esc(d.idf)],['Arduino core',esc(d.arduino)],['Uptime',dur(d.uptime)],['Last reset',esc(d.reset)]])+
  sec('Processor',[['Core 0 (WiFi, decoder)',bar(u.core0,100,u.core0+' %')],['Core 1 (app, display)',bar(u.core1,100,u.core1+' %')]])+
  sec('Memory',[['Internal RAM',bar(h.size-h.free,h.size,`${kb(h.size-h.free)} of ${kb(h.size)} used, ${kb(h.free)} free`)],['Lowest free',kb(h.minFree)],['Largest free block',kb(h.maxBlock)],['PSRAM',bar(p.size-p.free,p.size,`${kb(p.size-p.free)} of ${kb(p.size)} used, ${kb(p.free)} free`)],['PSRAM lowest free',kb(p.minFree)],['Program',bar(f.sketch,f.sketch+f.sketchFree,`${kb(f.sketch)} of ${kb(f.sketch+f.sketchFree)}`)]])+
  (w.connected?sec('WiFi',[['Network',esc(w.ssid)],['Signal',bar(Math.max(0,w.rssi+100),70,`${w.rssi} dBm`)],['Channel',w.channel],['Access point',w.bssid],['Address',w.ip],['Gateway',w.gateway],['Subnet',w.mask],['DNS',w.dns],['Hostname',w.hostname],['TX power',w.txPower+' dBm']]):sec('WiFi',[['Status',w.portal?'setup portal open':'not connected']]))+
  sec('Audio',[['Station',esc(a.station)||'—'],['Stream',a.url?`<a href="${esc(a.url)}" style="color:var(--green)">${esc(a.url)}</a>`:'—'],['State',a.running?(a.paused?'paused':'playing'):'stopped'],['Codec',esc(a.codec)||'—'],['Sample rate',a.sampleRate?a.sampleRate+' Hz':'—'],['Stream buffer',bar(a.buffer,a.bufferSize,`${kb(a.buffer)} of ${kb(a.bufferSize)}`)],['Volume',a.volume+' / 21'],['Decoder core',a.decodeCore],['I2S pins',esc(a.i2s)],['Loaded list',a.list?`${esc(a.list)} (${a.listCount})`:'none']])+
  `<div class="card"><h2>Tasks</h2><table><tr><th>Task</th><th class="r">Core</th><th class="r">Prio</th><th class="r">Stack free</th><th class="r">CPU</th></tr>${(d.tasks||[]).sort((x,y)=>y.cpu-x.cpu).map(t=>`<tr><td>${esc(t.name)}</td><td class="r">${t.core<0?'any':t.core}</td><td class="r">${t.prio}</td><td class="r">${kb(t.stackFree)}</td><td class="r">${t.cpu} %</td></tr>`).join('')}</table></div>`;
 }catch(e){$('stats').innerHTML='<div class="card err">The radio did not answer</div>'}}
function update(){const f=$('fwfile').files[0];if(!f){$('fwst').textContent='Choose a firmware.bin first.';return}
 if(!/\.bin$/i.test(f.name)){$('fwst').textContent='That is not a .bin file.';return}
 updating=true;$('fwgo').disabled=true;$('fwgo').classList.add('wait');const st=$('fwst');
 const x=new XMLHttpRequest();x.open('POST','/api/update');
 x.upload.onprogress=e=>{if(e.lengthComputable)st.innerHTML=bar(e.loaded,e.total,`Sending ${kb(e.loaded)} of ${kb(e.total)} — keep this page open`)};
 x.onerror=()=>{st.textContent='Upload failed. Is the radio still on the network?';done()};
 x.onload=()=>{let r={};try{r=JSON.parse(x.responseText)}catch(e){}
  if(x.status===200&&r.ok){st.textContent='Installed. The radio is restarting…';waitBack()}else{st.textContent='Update failed: '+(r.error||x.statusText);done()}};
 const fd=new FormData();fd.append('firmware',f,f.name);x.send(fd);
 function done(){updating=false;$('fwgo').disabled=false;$('fwgo').classList.remove('wait')}
 async function waitBack(){for(let i=0;i<60;i++){await new Promise(r=>setTimeout(r,2000));try{const s=await (await fetch('/api/state',{cache:'no-store'})).json();if(s.version){st.textContent='Back on '+s.version+'.';location.reload();return}}catch(e){}}st.textContent='The radio has not come back yet. Give it a moment, then reload this page.';done()}}
if(location.hash==='#device')show('device');state();list();setInterval(state,2000);setInterval(device,3000);
</script></body></html>)HTML";

// One client at a time is all this server does, so every answer closes its
// connection: a browser holding one open would lock everyone else out.
static void sendJson(JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Connection", "close");
  server.send(200, "application/json", out);
}

static void sendList() {
  JsonDocument doc;
  doc["source"] = directory::source;
  JsonArray arr = doc["stations"].to<JsonArray>();
  for (const Station& s : directory::stations) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = s.id; o["name"] = s.name; o["country"] = s.country; o["codec"] = s.codec;
    o["bitrate"] = s.bitrate; o["icon"] = s.icon; o["fav"] = favourites::has(s.id);
  }
  sendJson(doc);
}

static void sendError(const String& msg) {
  JsonDocument doc;
  doc["error"] = msg;
  sendJson(doc);
}

static void handleState() {
  JsonDocument doc;
  const Station& c = player::current();
  doc["version"] = AD_VERSION;
  doc["ip"] = net::ip();
  doc["playing"] = player::running();
  doc["paused"] = player::paused();
  doc["connecting"] = player::connecting();
  doc["fav"] = favourites::has(c.id.length() ? c.id : c.url);
  doc["favCount"] = favourites::list().size();
  doc["name"] = c.name;
  doc["id"] = c.id;
  doc["icon"] = c.icon;
  doc["title"] = player::title();
  doc["codec"] = player::codec();
  doc["volume"] = player::volume();
  doc["source"] = directory::source;
  doc["index"] = player::currentIndex();
  doc["count"] = directory::stations.size();
  doc["error"] = player::lastError();
  sendJson(doc);
}

static void ok() { server.sendHeader("Cache-Control", "no-store"); server.sendHeader("Connection", "close"); server.send(200, "application/json", "{\"ok\":true}"); }

static void handlePlay() {
  if (server.hasArg("i")) {
    if (player::playIndex(server.arg("i").toInt())) ok(); else sendError(player::lastError());
    return;
  }
  String link = server.hasArg("link") ? server.arg("link") : server.arg("id");
  link.trim();
  if (link.isEmpty()) { sendError("nothing to play"); return; }
  Station s;
  String id = directory::stationIdFromLink(link);
  if (id.length()) {
    if (!directory::byId(id, s)) { sendError(directory::error); return; }
  } else if (link.indexOf("://") > 0) {
    s.url = link; s.name = link;
  } else { sendError("not a station link or a stream URL"); return; }
  if (player::play(s)) ok(); else sendError(player::lastError());
}

// The logo as the tab icon: the site's icon.svg, following the OS theme.
static const char FAVICON_SVG[] PROGMEM =
  "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 512 512'>"
  "<style>.t{fill:#46d98a}.b{stroke:#04150c}@media(prefers-color-scheme:light){.t{fill:#117a47}.b{stroke:#fff}}</style>"
  "<rect class='t' width='512' height='512' rx='143'/>"
  "<g class='b' fill='none' stroke-width='2.8' stroke-linecap='round' transform='translate(71.68 71.68) scale(15.36)'>"
  "<path d='M4 10v4'/><path d='M8 7v10'/><path d='M12 3v18'/><path d='M16 7v10'/><path d='M20 10v4'/></g></svg>";

static void sendIcon(const char* type, const uint8_t* data, size_t len) {
  server.sendHeader("Connection", "close");
  server.sendHeader("Cache-Control", "public, max-age=604800");
  server.send_P(200, type, (const char*)data, len);
}

void webui::begin() {
  server.on("/", HTTP_GET, [] { server.sendHeader("Connection", "close"); server.send_P(200, "text/html; charset=utf-8", PAGE); });
  // The screen as it is right now, for documentation and for the curious.
  server.on("/screen.bmp", HTTP_GET, [] {
    const uint16_t* fb = display::frame();
    if (!fb) { server.sendHeader("Connection", "close"); server.send(404, "text/plain", "this board has no screen"); return; }
    const int N = display::side();
    uint8_t hdr[BMP_HEADER]; bmpHeader(hdr, N, N);
    server.sendHeader("Connection", "close");
    server.sendHeader("Cache-Control", "no-store");
    server.setContentLength(BMP_HEADER + (size_t)N * N * 2);
    server.send(200, "image/bmp", "");
    WiFiClient c = server.client();
    c.write(hdr, BMP_HEADER);
    for (int y = N - 1; y >= 0; y--) c.write((const uint8_t*)(fb + (size_t)y * N), N * 2);
  });
  server.on("/favicon.svg", HTTP_GET, [] { sendIcon("image/svg+xml", (const uint8_t*)FAVICON_SVG, strlen_P(FAVICON_SVG)); });
  server.on("/favicon.png", HTTP_GET, [] { sendIcon("image/png", FAVICON_PNG, FAVICON_PNG_len); });
  server.on("/favicon.ico", HTTP_GET, [] { sendIcon("image/png", FAVICON_PNG, FAVICON_PNG_len); });
  server.on("/apple-touch-icon.png", HTTP_GET, [] { sendIcon("image/png", TOUCH_ICON_PNG, TOUCH_ICON_PNG_len); });
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/device", HTTP_GET, [] { JsonDocument doc; devinfo::fill(doc); sendJson(doc); });
  server.on("/api/list", HTTP_GET, sendList);
  server.on("/api/load", HTTP_GET, [] {
    if (!net::connected()) { sendError("no WiFi"); return; }
    if (directory::load(server.arg("source"))) sendList(); else sendError(directory::error);
  });
  server.on("/api/search", HTTP_GET, [] {
    if (!net::connected()) { sendError("no WiFi"); return; }
    if (directory::search(server.arg("q"))) sendList(); else sendError(directory::error);
  });
  server.on("/api/play", HTTP_GET, handlePlay);
  server.on("/api/fav", HTTP_GET, [] {
    const Station& c = player::current();
    if (c.url.isEmpty()) { sendError("nothing playing to save"); return; }
    JsonDocument doc; doc["fav"] = favourites::toggle(c); doc["favCount"] = favourites::list().size();
    if (directory::source == "favourites") directory::load("favourites");
    sendJson(doc);
  });
  server.on("/api/stop", HTTP_GET, [] { player::stop(); ok(); });
  server.on("/api/pause", HTTP_GET, [] { player::togglePause(); ok(); });
  server.on("/api/next", HTTP_GET, [] { player::next(); ok(); });
  server.on("/api/prev", HTTP_GET, [] { player::prev(); ok(); });
  server.on("/api/volume", HTTP_GET, [] { player::setVolume(server.arg("v").toInt()); ok(); });
  // Firmware upload: the .bin as a multipart form file, written straight into
  // the spare app partition; the radio restarts on it if the image checks out.
  server.on("/api/update", HTTP_POST, [] {
    server.sendHeader("Connection", "close");
    if (Update.hasError()) {
      String msg = Update.errorString();
      Serial.printf("[web] firmware update failed: %s\n", msg.c_str());
      display::busy("");
      server.send(500, "application/json", "{\"error\":\"" + msg + "\"}");
    } else {
      Serial.println("[web] firmware updated; restarting");
      server.send(200, "application/json", "{\"ok\":true}");
      delay(500);
      ESP.restart();
    }
  }, [] {
    HTTPUpload& up = server.upload();
    if (up.status == UPLOAD_FILE_START) {
      Serial.printf("[web] firmware upload: %s\n", up.filename.c_str());
      player::stop();                       // the decoder's CPU and RAM go to the flash writes
      display::busy("Updating firmware");
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Serial.printf("[web] update begin failed: %s\n", Update.errorString());
    } else if (up.status == UPLOAD_FILE_WRITE) {
      if (!Update.hasError() && Update.write(up.buf, up.currentSize) != up.currentSize)
        Serial.printf("[web] update write failed: %s\n", Update.errorString());
    } else if (up.status == UPLOAD_FILE_END) {
      if (Update.end(true)) Serial.printf("[web] firmware image written: %u bytes\n", up.totalSize);
      else Serial.printf("[web] update end failed: %s\n", Update.errorString());
    } else if (up.status == UPLOAD_FILE_ABORTED) {
      Update.abort();
      display::busy("");
      Serial.println("[web] firmware upload aborted");
    }
  });
  server.onNotFound([] { server.send(404, "text/plain", "not found"); });
}

// The setup portal has its own server on port 80, so ours starts a moment
// after WiFi is up and the portal is gone.
void webui::loop() {
  static unsigned long readyAt = 0;
  if (!started) {
    if (!net::connected() || net::portalOpen()) { readyAt = 0; return; }
    if (!readyAt) { readyAt = millis() + 1500; return; }
    if (millis() < readyAt) return;
    server.begin();
    started = true;
    Serial.printf("[web] http://%s\n", net::ip().c_str());
    return;
  }
  server.handleClient();
}
