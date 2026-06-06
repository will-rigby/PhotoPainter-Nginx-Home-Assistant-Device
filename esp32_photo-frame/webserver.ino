// SPDX-License-Identifier: 0BSD

// --------------------------------------------------
// Embedded HTTP server: photo gallery + upload + WiFi/MQTT settings
// --------------------------------------------------
//
// Runs only on USB power ("server mode"). Serves:
//   GET  /            gallery page
//   GET  /wifi        WiFi settings page
//   GET  /mqtt        MQTT settings page
//   GET  /api/photos  JSON list of photos on the SD card
//   GET  /api/config  current (non-secret) config for prefilling forms
//   GET  /thumb?file= small BMP preview generated from a stored .bin
//   GET  /preview?file= full 800x480 BMP generated from a stored .bin
//   POST /upload      multipart upload -> decode/resize/dither -> .bin
//   POST /delete?file= remove a photo
//   POST /show?file=  display a photo on the panel now
//   POST /api/wifi    save WiFi credentials
//   POST /api/mqtt    save MQTT settings
//

#include <WebServer.h>
#include <DNSServer.h>
#include "config.h"

static WebServer server(80);
static DNSServer dnsServer;
static File      uploadFile;
static String    uploadTmpPath;
static String    uploadOrigName;

#define PHOTO_DIR "/photos"

// --------------------------------------------------
// Small helpers
// --------------------------------------------------

static void ensurePhotosDir() {
  if (!SD.exists(PHOTO_DIR)) SD.mkdir(PHOTO_DIR);
}

static String baseNameOf(const String& p) {
  int s = p.lastIndexOf('/');
  return (s >= 0) ? p.substring(s + 1) : p;
}

// Keep only safe filename characters (defends against path traversal).
static String safeBinName(const String& in) {
  String out;
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-') out += c;
  }
  return out;
}

static bool validBin(const String& name) {
  if (name.length() == 0 || name.indexOf("..") >= 0) return false;
  String low = name;
  low.toLowerCase();
  return low.endsWith(".bin");
}

// Derive a safe base name (no dir, no extension) from an uploaded filename.
static String sanitizeBase(const String& filename) {
  int s = max(filename.lastIndexOf('/'), filename.lastIndexOf('\\'));
  String base = (s >= 0) ? filename.substring(s + 1) : filename;
  int dot = base.lastIndexOf('.');
  if (dot > 0) base = base.substring(0, dot);

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

static void writeLE16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void writeLE32(uint8_t* p, uint32_t v) {
  p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

// --------------------------------------------------
// Stream a stored .bin as a 24-bit BMP (step = downsample factor)
// --------------------------------------------------

static void streamBinAsBMP(const String& path, int step) {
  File f = SD.open(path);
  if (!f || f.size() != (uint32_t)EPD_BUF_SIZE) {
    if (f) f.close();
    server.send(404, "text/plain", "not found");
    return;
  }

  int outW = EPD_WIDTH / step;
  int outH = EPD_HEIGHT / step;
  int rowSize = (outW * 3 + 3) & ~3;             // pad rows to 4 bytes
  uint32_t dataSize = (uint32_t)rowSize * outH;
  uint32_t fileSize = 54 + dataSize;

  uint8_t hdr[54];
  memset(hdr, 0, sizeof(hdr));
  hdr[0] = 'B'; hdr[1] = 'M';
  writeLE32(&hdr[2], fileSize);
  writeLE32(&hdr[10], 54);
  writeLE32(&hdr[14], 40);
  writeLE32(&hdr[18], (uint32_t)outW);
  writeLE32(&hdr[22], (uint32_t)outH);           // positive height = bottom-up
  writeLE16(&hdr[26], 1);
  writeLE16(&hdr[28], 24);
  writeLE32(&hdr[34], dataSize);

  uint8_t binRow[EPD_ROW_BYTES];
  uint8_t* outRow = (uint8_t*)malloc(rowSize);
  if (!outRow) { f.close(); server.send(500, "text/plain", "oom"); return; }

  server.setContentLength(fileSize);
  server.send(200, "image/bmp", "");
  WiFiClient client = server.client();
  client.write(hdr, 54);

  // BMP is bottom-up: emit image rows from bottom to top.
  for (int oy = outH - 1; oy >= 0; oy--) {
    int srcY = oy * step;
    f.seek((uint32_t)srcY * EPD_ROW_BYTES);
    f.read(binRow, EPD_ROW_BYTES);
    memset(outRow, 0, rowSize);

    for (int ox = 0; ox < outW; ox++) {
      int srcX = ox * step;
      uint8_t packed = binRow[srcX >> 1];
      uint8_t code = (srcX & 1) ? (packed & 0x0F) : (packed >> 4);
      uint8_t r, g, b;
      epdCodeToRGB(code, r, g, b);
      outRow[ox * 3 + 0] = b;   // BMP pixel order is BGR
      outRow[ox * 3 + 1] = g;
      outRow[ox * 3 + 2] = r;
    }
    client.write(outRow, rowSize);
  }

  free(outRow);
  f.close();
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
  "button:hover{background:#47d}.del{background:#a33}.del:hover{background:#c44}"
  "#grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:12px;margin-top:16px}"
  ".card{background:#1c1c1c;border-radius:8px;overflow:hidden}"
  ".card img{width:100%;display:block;background:#000}.name{padding:6px 8px;font-size:.8rem;"
  "white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.btns{display:flex;gap:6px;padding:8px}"
  ".btns button{flex:1}#status{display:block;margin-top:8px;color:#9f9}</style></head><body>"
  "<nav><a href='/'>Gallery</a><a href='/wifi'>WiFi</a><a href='/mqtt'>MQTT</a></nav><main>"
  "<h1>Photo Gallery</h1>"
  "<form onsubmit='return up(event)'><input type=file id=file accept='.jpg,.jpeg,.bmp'>"
  "<button>Upload</button></form><span id=status></span>"
  "<div id=grid></div></main><script>"
  "async function load(){const r=await fetch('/api/photos');const a=await r.json();"
  "const g=document.getElementById('grid');g.innerHTML='';"
  "if(!a.length){g.innerHTML='<p>No photos yet. Upload one above.</p>';return;}"
  "for(const p of a){const d=document.createElement('div');d.className='card';"
  "const n=p.name;d.innerHTML=\"<img loading=lazy src='/thumb?file=\"+encodeURIComponent(n)+\"'>\"+"
  "\"<div class=name>\"+n+\"</div><div class=btns>\"+"
  "\"<button onclick=\\\"show('\"+n+\"')\\\">Show</button>\"+"
  "\"<button class=del onclick=\\\"del('\"+n+\"')\\\">Delete</button></div>\";"
  "g.appendChild(d);}}"
  "async function show(n){if(!confirm('Display this photo on the frame now?'))return;"
  "await fetch('/show?file='+encodeURIComponent(n),{method:'POST'});"
  "alert('Refreshing the panel — this takes ~30s.');}"
  "async function del(n){if(!confirm('Delete '+n+'?'))return;"
  "await fetch('/delete?file='+encodeURIComponent(n),{method:'POST'});load();}"
  "async function up(e){e.preventDefault();const f=document.getElementById('file').files[0];"
  "if(!f)return false;const fd=new FormData();fd.append('file',f);"
  "const s=document.getElementById('status');s.textContent='Uploading & processing…';"
  "const r=await fetch('/upload',{method:'POST',body:fd});const j=await r.json();"
  "s.textContent=j.ok?('Added '+j.name):('Error: '+(j.error||'failed'));load();return false;}"
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
  File dir = SD.open(PHOTO_DIR);
  bool first = true;
  if (dir) {
    File e;
    while ((e = dir.openNextFile())) {
      if (!e.isDirectory()) {
        String n = baseNameOf(String(e.name()));
        String low = n; low.toLowerCase();
        if (low.endsWith(".bin")) {
          if (!first) json += ",";
          first = false;
          json += "{\"name\":\"" + jsonEscape(n) + "\",\"size\":" +
                  String((uint32_t)e.size()) + "}";
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

static void handleThumb() {
  String name = safeBinName(server.arg("file"));
  if (!validBin(name)) { server.send(400, "text/plain", "bad name"); return; }
  streamBinAsBMP(String(PHOTO_DIR "/") + name, 4);
}

static void handlePreview() {
  String name = safeBinName(server.arg("file"));
  if (!validBin(name)) { server.send(400, "text/plain", "bad name"); return; }
  streamBinAsBMP(String(PHOTO_DIR "/") + name, 1);
}

static void handleDelete() {
  String name = safeBinName(server.arg("file"));
  if (!validBin(name)) { server.send(400, "application/json", "{\"ok\":false}"); return; }
  String path = String(PHOTO_DIR "/") + name;
  bool ok = SD.exists(path) && SD.remove(path);
  server.send(ok ? 200 : 404, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

static void handleShow() {
  String name = safeBinName(server.arg("file"));
  if (!validBin(name)) { server.send(400, "application/json", "{\"ok\":false}"); return; }
  String path = String(PHOTO_DIR "/") + name;
  if (!SD.exists(path) || !epdLoadBufferFromFile(path.c_str())) {
    server.send(404, "application/json", "{\"ok\":false}");
    return;
  }
  server.send(200, "application/json", "{\"ok\":true}");
  epdDisplayCurrentBuffer();   // slow refresh; client already acknowledged
}

// Upload: chunk callback (streams to SD) + completion callback (processes)
static void handleUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    uploadTmpPath = "";
    String low = up.filename; low.toLowerCase();
    String ext;
    if      (low.endsWith(".jpg"))  ext = ".jpg";
    else if (low.endsWith(".jpeg")) ext = ".jpeg";
    else if (low.endsWith(".bmp"))  ext = ".bmp";
    else { Serial.println("Upload rejected (unsupported type)"); return; }

    uploadOrigName = up.filename;
    String tmp = String("/upload_tmp") + ext;
    if (SD.exists(tmp)) SD.remove(tmp);
    uploadFile = SD.open(tmp.c_str(), FILE_WRITE);
    if (uploadFile) uploadTmpPath = tmp;
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) uploadFile.write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    if (uploadFile) uploadFile.close();
  }
}

static void handleUploadDone() {
  if (uploadTmpPath.length() == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"unsupported type\"}");
    return;
  }

  ensurePhotosDir();
  String base = sanitizeBase(uploadOrigName);
  String finalPath = String(PHOTO_DIR "/") + base + ".bin";
  int n = 1;
  while (SD.exists(finalPath)) {
    finalPath = String(PHOTO_DIR "/") + base + "_" + String(n++) + ".bin";
  }

  bool ok = processUploadToBin(uploadTmpPath.c_str(), finalPath.c_str());
  SD.remove(uploadTmpPath);
  uploadTmpPath = "";

  if (ok) {
    String name = baseNameOf(finalPath);
    server.send(200, "application/json", "{\"ok\":true,\"name\":\"" + jsonEscape(name) + "\"}");
  } else {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"processing failed\"}");
  }
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
  ensurePhotosDir();

  if (apMode) dnsServer.start(53, "*", WiFi.softAPIP());

  server.on("/",           HTTP_GET,  handleRoot);
  server.on("/wifi",       HTTP_GET,  handleWifiPage);
  server.on("/mqtt",       HTTP_GET,  handleMqttPage);
  server.on("/api/photos", HTTP_GET,  handleApiPhotos);
  server.on("/api/config", HTTP_GET,  handleApiConfig);
  server.on("/thumb",      HTTP_GET,  handleThumb);
  server.on("/preview",    HTTP_GET,  handlePreview);
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
