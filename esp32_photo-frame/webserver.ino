// SPDX-License-Identifier: 0BSD

// --------------------------------------------------
// Embedded HTTP server: photo gallery + upload + WiFi/MQTT settings
// --------------------------------------------------
//
// Runs only on USB power ("server mode"). Serves:
//   GET  /             gallery page (shows original photos)
//   GET  /wifi         WiFi settings page
//   GET  /mqtt         MQTT settings page
//   GET  /api/photos   JSON list of originals with ready/failed status
//   GET  /api/config   current (non-secret) config for prefilling forms
//   GET  /original?file= stream an original image from /originals
//   POST /upload       multipart upload -> store original in /originals (no blocking)
//   POST /delete?file= remove a photo (original + dithered)
//   POST /show?file=   display a photo's dithered buffer on the panel now
//   POST /api/wifi     save WiFi credentials
//   POST /api/mqtt     save MQTT settings
//
// Uploaded originals are dithered into /dithered/<base>.bin in the background by
// the queue in esp32_photo-frame.ino (processNextPending), so uploads never block.
//

#include <WebServer.h>
#include <DNSServer.h>
#include "config.h"

static WebServer server(80);
static DNSServer dnsServer;

// Upload state (one file per request)
static File   uploadFile;
static String uploadFinalName;   // basename stored under /originals
static bool   uploadStored;

// --------------------------------------------------
// Small helpers
// --------------------------------------------------

static void ensureDirs() {
  if (!SD.exists(ORIGINALS_DIR)) SD.mkdir(ORIGINALS_DIR);
  if (!SD.exists(DITHERED_DIR))  SD.mkdir(DITHERED_DIR);
}

static String baseNameOf(const String& p) {
  int s = p.lastIndexOf('/');
  return (s >= 0) ? p.substring(s + 1) : p;
}

static String baseNoExt(const String& name) {
  String b = baseNameOf(name);
  int dot = b.lastIndexOf('.');
  if (dot > 0) b = b.substring(0, dot);
  return b;
}

// Keep only safe filename characters (defends against path traversal).
static String safeName(const String& in) {
  String out;
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-') out += c;
  }
  return out;
}

static bool validImageName(const String& name) {
  if (name.length() == 0 || name.indexOf("..") >= 0) return false;
  String low = name;
  low.toLowerCase();
  return low.endsWith(".jpg") || low.endsWith(".jpeg") || low.endsWith(".bmp");
}

static String contentTypeFor(const String& name) {
  String low = name;
  low.toLowerCase();
  if (low.endsWith(".bmp")) return "image/bmp";
  return "image/jpeg";
}

// Map an upload filename to a sanitised base (no dir, no extension).
static String sanitizeBase(const String& filename) {
  String base = baseNoExt(filename);
  String out;
  for (size_t i = 0; i < base.length() && out.length() < 40; i++) {
    char c = base[i];
    out += (isalnum((unsigned char)c) || c == '_' || c == '-') ? c : '_';
  }
  if (out.length() == 0) out = "photo";
  return out;
}

static String jsonEscape(const String& s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o;
}

// --------------------------------------------------
// Embedded pages (PROGMEM)
// --------------------------------------------------

static const char PAGE_INDEX[] PROGMEM =
  "<!doctype html><html><head><meta charset=utf-8><meta name=viewport "
  "content='width=device-width,initial-scale=1'><title>Photo Frame</title>"
  "<style>body{font-family:system-ui,sans-serif;margin:0;background:#111;color:#eee}"
  "nav{display:flex;gap:1px;background:#000}nav a{flex:1;text-align:center;padding:14px;"
  "color:#9cf;text-decoration:none;background:#1c1c1c}nav a:hover{background:#333}"
  "main{padding:16px;max-width:900px;margin:0 auto}h1{font-size:1.3rem}"
  "input{padding:8px;margin:4px 0;background:#222;color:#eee;border:1px solid #444;border-radius:6px}"
  "button{padding:8px 14px;background:#36c;color:#fff;border:0;border-radius:6px;cursor:pointer}"
  "button:hover{background:#47d}button:disabled{opacity:.4;cursor:default}"
  ".del{background:#a33}.del:hover{background:#c44}"
  "#grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:12px;margin-top:16px}"
  ".card{background:#1c1c1c;border-radius:8px;overflow:hidden}"
  ".card img{width:100%;display:block;background:#000;aspect-ratio:5/3;object-fit:cover}"
  ".name{padding:6px 8px;font-size:.8rem;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}"
  ".btns{display:flex;gap:6px;padding:8px}.btns button{flex:1}"
  ".ok{color:#9f9}.proc{color:#fc9}.bad{color:#f99}#status{display:block;margin-top:8px;color:#9f9}</style></head><body>"
  "<nav><a href='/'>Gallery</a><a href='/wifi'>WiFi</a><a href='/mqtt'>MQTT</a></nav><main>"
  "<h1>Photo Gallery</h1>"
  "<form onsubmit='return up(event)'><input type=file id=file accept='.jpg,.jpeg,.bmp' multiple>"
  "<button>Upload</button></form><span id=status></span>"
  "<div id=grid></div></main><script>"
  "let pollT=null;"
  "async function load(){const a=await(await fetch('/api/photos')).json();"
  "const g=document.getElementById('grid');g.innerHTML='';"
  "if(!a.length){g.innerHTML='<p>No photos yet. Upload some above.</p>';}"
  "let busy=false;"
  "for(const p of a){const n=p.name;const d=document.createElement('div');d.className='card';"
  "let badge=p.ready?\"<span class=ok>ready</span>\":(p.failed?\"<span class=bad>failed</span>\":\"<span class=proc>processing\\u2026</span>\");"
  "if(!p.ready&&!p.failed)busy=true;"
  "d.innerHTML=\"<img loading=lazy src='/original?file=\"+encodeURIComponent(n)+\"'>\"+"
  "\"<div class=name>\"+n+\" \"+badge+\"</div><div class=btns>\"+"
  "\"<button \"+(p.ready?'':'disabled')+\" onclick=\\\"show('\"+n+\"')\\\">Show</button>\"+"
  "\"<button class=del onclick=\\\"del('\"+n+\"')\\\">Delete</button></div>\";"
  "g.appendChild(d);}"
  "if(busy){if(!pollT)pollT=setInterval(load,2000);}else{if(pollT){clearInterval(pollT);pollT=null;}}}"
  "async function show(n){if(!confirm('Display this photo on the frame now?'))return;"
  "await fetch('/show?file='+encodeURIComponent(n),{method:'POST'});"
  "alert('Refreshing the panel \\u2014 this takes ~30s.');}"
  "async function del(n){if(!confirm('Delete '+n+'?'))return;"
  "await fetch('/delete?file='+encodeURIComponent(n),{method:'POST'});load();}"
  "async function up(e){e.preventDefault();const fs=document.getElementById('file').files;"
  "if(!fs.length)return false;const s=document.getElementById('status');"
  "for(let i=0;i<fs.length;i++){s.textContent='Uploading '+(i+1)+'/'+fs.length+'\\u2026';"
  "const fd=new FormData();fd.append('file',fs[i]);try{await fetch('/upload',{method:'POST',body:fd});}catch(err){}}"
  "s.textContent='Uploaded '+fs.length+' file(s). Processing\\u2026';"
  "document.getElementById('file').value='';load();return false;}"
  "window.onload=load;</script></body></html>";

static const char PAGE_WIFI[] PROGMEM =
  "<!doctype html><html><head><meta charset=utf-8><meta name=viewport "
  "content='width=device-width,initial-scale=1'><title>WiFi</title>"
  "<style>body{font-family:system-ui,sans-serif;margin:0;background:#111;color:#eee}"
  "nav{display:flex;gap:1px;background:#000}nav a{flex:1;text-align:center;padding:14px;"
  "color:#9cf;text-decoration:none;background:#1c1c1c}nav a:hover{background:#333}"
  "main{padding:16px;max-width:480px;margin:0 auto}h1{font-size:1.3rem}label{display:block;margin-top:10px}"
  "input{padding:8px;margin:4px 0;width:100%;box-sizing:border-box;background:#222;color:#eee;"
  "border:1px solid #444;border-radius:6px}button{padding:10px 16px;margin-top:10px;background:#36c;"
  "color:#fff;border:0;border-radius:6px;cursor:pointer}#status{display:block;margin-top:10px;color:#9f9}</style></head><body>"
  "<nav><a href='/'>Gallery</a><a href='/wifi'>WiFi</a><a href='/mqtt'>MQTT</a></nav><main>"
  "<h1>WiFi Settings</h1><p>Set the network the frame should join. If it can't be found, "
  "the frame hosts its own setup hotspot instead.</p>"
  "<form onsubmit='return save(event)'>"
  "<label>Network name (SSID)</label><input id=ssid>"
  "<label>Password</label><input id=pass type=password placeholder='(unchanged / leave blank for open)'>"
  "<button>Save</button></form><span id=status></span></main><script>"
  "async function load(){const c=await(await fetch('/api/config')).json();"
  "document.getElementById('ssid').value=c.wifi_ssid||'';}"
  "async function save(e){e.preventDefault();const b=new URLSearchParams();"
  "b.append('ssid',document.getElementById('ssid').value);"
  "b.append('pass',document.getElementById('pass').value);"
  "const j=await(await fetch('/api/wifi',{method:'POST',"
  "headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b})).json();"
  "document.getElementById('status').textContent=j.ok?"
  "'Saved. Unplug and replug (or reboot) the frame to join this network.':'Error saving';return false;}"
  "window.onload=load;</script></body></html>";

static const char PAGE_MQTT[] PROGMEM =
  "<!doctype html><html><head><meta charset=utf-8><meta name=viewport "
  "content='width=device-width,initial-scale=1'><title>MQTT</title>"
  "<style>body{font-family:system-ui,sans-serif;margin:0;background:#111;color:#eee}"
  "nav{display:flex;gap:1px;background:#000}nav a{flex:1;text-align:center;padding:14px;"
  "color:#9cf;text-decoration:none;background:#1c1c1c}nav a:hover{background:#333}"
  "main{padding:16px;max-width:480px;margin:0 auto}h1{font-size:1.3rem}label{display:block;margin-top:10px}"
  "input{padding:8px;margin:4px 0;width:100%;box-sizing:border-box;background:#222;color:#eee;"
  "border:1px solid #444;border-radius:6px}button{padding:10px 16px;margin-top:10px;background:#36c;"
  "color:#fff;border:0;border-radius:6px;cursor:pointer}#status{display:block;margin-top:10px;color:#9f9}</style></head><body>"
  "<nav><a href='/'>Gallery</a><a href='/wifi'>WiFi</a><a href='/mqtt'>MQTT</a></nav><main>"
  "<h1>MQTT Settings</h1><p>Broker used to report temperature, humidity and battery to "
  "Home Assistant. Leave the host blank to disable MQTT.</p>"
  "<form onsubmit='return save(event)'>"
  "<label>Broker host / IP</label><input id=host>"
  "<label>Port</label><input id=port type=number value=1883>"
  "<label>Username</label><input id=user>"
  "<label>Password</label><input id=pass type=password placeholder='(unchanged / blank for none)'>"
  "<button>Save</button></form><span id=status></span></main><script>"
  "async function load(){const c=await(await fetch('/api/config')).json();"
  "document.getElementById('host').value=c.mqtt_host||'';"
  "document.getElementById('port').value=c.mqtt_port||1883;"
  "document.getElementById('user').value=c.mqtt_user||'';}"
  "async function save(e){e.preventDefault();const b=new URLSearchParams();"
  "b.append('host',document.getElementById('host').value);"
  "b.append('port',document.getElementById('port').value);"
  "b.append('user',document.getElementById('user').value);"
  "b.append('pass',document.getElementById('pass').value);"
  "const j=await(await fetch('/api/mqtt',{method:'POST',"
  "headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b})).json();"
  "document.getElementById('status').textContent=j.ok?'Saved.':'Error saving';return false;}"
  "window.onload=load;</script></body></html>";

// --------------------------------------------------
// Route handlers
// --------------------------------------------------

static void handleRoot()     { server.send_P(200, "text/html", PAGE_INDEX); }
static void handleWifiPage()  { server.send_P(200, "text/html", PAGE_WIFI); }
static void handleMqttPage()  { server.send_P(200, "text/html", PAGE_MQTT); }

static void handleApiPhotos() {
  String json = "[";
  File dir = SD.open(ORIGINALS_DIR);
  bool first = true;
  if (dir) {
    File e;
    while ((e = dir.openNextFile())) {
      if (!e.isDirectory()) {
        String n = baseNameOf(String(e.name()));
        if (validImageName(n)) {
          String base = baseNoExt(n);
          bool ready  = SD.exists(String(DITHERED_DIR "/") + base + ".bin");
          bool failed = !ready && isFailedBase(base);
          if (!first) json += ",";
          first = false;
          json += "{\"name\":\"" + jsonEscape(n) + "\",\"ready\":" +
                  (ready ? "true" : "false") + ",\"failed\":" +
                  (failed ? "true" : "false") + "}";
        }
      }
      e.close();
    }
    dir.close();
  }
  json += "]";
  server.send(200, "application/json", json);
}

static void handleApiConfig() {
  String j = "{";
  j += "\"wifi_ssid\":\"" + jsonEscape(cfgWifiSsid) + "\",";
  j += "\"mqtt_host\":\"" + jsonEscape(cfgMqttHost) + "\",";
  j += "\"mqtt_port\":" + String(cfgMqttPort) + ",";
  j += "\"mqtt_user\":\"" + jsonEscape(cfgMqttUser) + "\",";
  j += "\"ap_mode\":" + String(apMode ? "true" : "false");
  j += "}";
  server.send(200, "application/json", j);
}

static void handleOriginal() {
  String name = safeName(server.arg("file"));
  if (!validImageName(name)) { server.send(400, "text/plain", "bad name"); return; }
  File f = SD.open(String(ORIGINALS_DIR "/") + name);
  if (!f) { server.send(404, "text/plain", "not found"); return; }
  server.streamFile(f, contentTypeFor(name));
  f.close();
}

static void handleDelete() {
  String name = safeName(server.arg("file"));
  if (!validImageName(name)) { server.send(400, "application/json", "{\"ok\":false}"); return; }
  String base = baseNoExt(name);
  String orig = String(ORIGINALS_DIR "/") + name;
  String bin  = String(DITHERED_DIR "/") + base + ".bin";

  bool removed = false;
  if (SD.exists(orig)) removed = SD.remove(orig);
  if (SD.exists(bin))  SD.remove(bin);
  clearFailedBase(base);

  server.send(removed ? 200 : 404, "application/json",
              removed ? "{\"ok\":true}" : "{\"ok\":false}");
}

static void handleShow() {
  String name = safeName(server.arg("file"));
  if (!validImageName(name)) { server.send(400, "application/json", "{\"ok\":false}"); return; }
  String bin = String(DITHERED_DIR "/") + baseNoExt(name) + ".bin";
  if (!SD.exists(bin) || !epdLoadBufferFromFile(bin.c_str())) {
    server.send(404, "application/json", "{\"ok\":false,\"error\":\"not ready\"}");
    return;
  }
  server.send(200, "application/json", "{\"ok\":true}");
  epdDisplayCurrentBuffer();   // slow refresh; client already acknowledged
}

// Upload: stream the original straight into /originals. Dithering happens later
// in the background queue (processNextPending in the main sketch).
static void handleUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    uploadStored = false;
    uploadFinalName = "";
    String low = up.filename; low.toLowerCase();
    String ext;
    if      (low.endsWith(".jpeg")) ext = ".jpeg";
    else if (low.endsWith(".jpg"))  ext = ".jpg";
    else if (low.endsWith(".bmp"))  ext = ".bmp";
    else { Serial.println("Upload rejected (unsupported type)"); return; }

    ensureDirs();
    String base = sanitizeBase(up.filename);
    String path = String(ORIGINALS_DIR "/") + base + ext;
    int n = 1;
    while (SD.exists(path)) path = String(ORIGINALS_DIR "/") + base + "_" + String(n++) + ext;

    uploadFile = SD.open(path.c_str(), FILE_WRITE);
    if (uploadFile) uploadFinalName = baseNameOf(path);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) uploadFile.write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      uploadFile.close();
      uploadStored = true;
      workPending = true;          // tell loop() there's dithering to do
      lastUploadMs = millis();
    }
  }
}

static void handleUploadDone() {
  if (!uploadStored) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"unsupported type\"}");
    return;
  }
  server.send(200, "application/json",
              "{\"ok\":true,\"name\":\"" + jsonEscape(uploadFinalName) + "\"}");
}

static void handleSaveWifi() {
  configSaveWifi(server.arg("ssid"), server.arg("pass"));
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleSaveMqtt() {
  uint16_t port = (uint16_t)server.arg("port").toInt();
  if (port == 0) port = 1883;
  configSaveMqtt(server.arg("host"), port, server.arg("user"), server.arg("pass"));
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleNotFound() {
  if (apMode) {
    // Captive-portal style redirect to the gallery.
    server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/");
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "Not found");
  }
}

// --------------------------------------------------
// Public entry points
// --------------------------------------------------

void webServerBegin() {
  ensureDirs();

  if (apMode) dnsServer.start(53, "*", WiFi.softAPIP());

  server.on("/",           HTTP_GET,  handleRoot);
  server.on("/wifi",       HTTP_GET,  handleWifiPage);
  server.on("/mqtt",       HTTP_GET,  handleMqttPage);
  server.on("/api/photos", HTTP_GET,  handleApiPhotos);
  server.on("/api/config", HTTP_GET,  handleApiConfig);
  server.on("/original",   HTTP_GET,  handleOriginal);
  server.on("/delete",     HTTP_POST, handleDelete);
  server.on("/show",       HTTP_POST, handleShow);
  server.on("/upload",     HTTP_POST, handleUploadDone, handleUpload);
  server.on("/api/wifi",   HTTP_POST, handleSaveWifi);
  server.on("/api/mqtt",   HTTP_POST, handleSaveMqtt);
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("Web server started on port 80");
}

void webServerHandle() {
  server.handleClient();
  if (apMode) dnsServer.processNextRequest();
}
