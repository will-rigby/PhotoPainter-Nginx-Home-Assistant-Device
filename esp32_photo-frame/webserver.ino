// SPDX-License-Identifier: 0BSD

// --------------------------------------------------
// Embedded HTTP server: photo gallery + upload + WiFi/MQTT settings
// --------------------------------------------------
//
// Runs only on USB power ("server mode"). Serves:
//   GET  /             gallery page (shows original photos)
//   GET  /wifi         WiFi settings page
//   GET  /mqtt         MQTT settings page
//   GET  /api/photos   JSON list of originals with cached/failed status
//   GET  /api/config   current (non-secret) config for prefilling forms
//   GET  /original?file= stream an original image from /originals
//   POST /upload       multipart upload -> store original in /originals
//   POST /delete?file= remove a photo (original + cached buffer)
//   POST /show?file=   render (if uncached) and display a photo now
//   POST /api/wifi     save WiFi credentials
//   POST /api/mqtt     save MQTT settings
//
// Photos are decoded + dithered lazily at display time (ensureDitheredInBuffer
// in esp32_photo-frame.ino) and cached as /dithered/<base>.bin, so uploads just
// store the original and return.
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

// Thumbnail upload state (browser sends one after each photo upload)
static File   thumbFile;
static bool   thumbStored;

// --------------------------------------------------
// Small helpers
// --------------------------------------------------

static void ensureDirs() {
  if (!SD.exists(ORIGINALS_DIR)) SD.mkdir(ORIGINALS_DIR);
  if (!SD.exists(DITHERED_DIR))  SD.mkdir(DITHERED_DIR);
  if (!SD.exists(THUMBS_DIR))    SD.mkdir(THUMBS_DIR);
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

// The gallery's upload JS re-encodes each image in the browser before
// sending: cover + centred crop onto an 800x480 canvas (keep the literals in
// prep() in sync with EPD_WIDTH/EPD_HEIGHT), exported via toBlob() which is
// always baseline JPEG — so progressive originals never reach the device and
// uploads shrink to ~100 KB. If decoding fails, the raw file is sent as-is.
// After each upload it also POSTs a 400x240 JPEG to /thumb; the gallery grid
// loads /thumb per photo and falls back to /original if none exists yet.
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
  "<button>Upload</button> <button type=button onclick='redither()'>Clear dither cache</button>"
  " <button type=button onclick='testpat()'>Test pattern</button>"
  "</form><span id=status></span>"
  "<div id=grid></div></main><script>"
  "async function load(){const a=await(await fetch('/api/photos')).json();"
  "const g=document.getElementById('grid');g.innerHTML='';"
  "if(!a.length){g.innerHTML='<p>No photos yet. Upload some above.</p>';}"
  "for(const p of a){const n=p.name;const d=document.createElement('div');d.className='card';"
  "let badge=p.cached?\"<span class=ok>cached</span>\":(p.failed?\"<span class=bad>failed</span>\":\"<span class=proc>not cached</span>\");"
  "const u=encodeURIComponent(n);"
  "d.innerHTML=\"<img loading=lazy src='/thumb?file=\"+u+\"' onerror=\\\"this.onerror=null;this.src='/original?file=\"+u+\"'\\\">\"+"
  "\"<div class=name>\"+n+\" \"+badge+\"</div><div class=btns>\"+"
  "\"<button onclick=\\\"show('\"+n+\"')\\\">Show</button>\"+"
  "\"<button class=del onclick=\\\"del('\"+n+\"')\\\">Delete</button></div>\";"
  "g.appendChild(d);}}"
  "async function show(n){if(!confirm('Display this photo on the frame now? Uncached photos take a few extra seconds to prepare.'))return;"
  "const r=await fetch('/show?file='+encodeURIComponent(n),{method:'POST'});"
  "if(!r.ok){alert('Processing failed.');load();return;}"
  "alert('Refreshing the panel \\u2014 this takes ~30s.');load();}"
  "async function del(n){if(!confirm('Delete '+n+'?'))return;"
  "await fetch('/delete?file='+encodeURIComponent(n),{method:'POST'});load();}"
  "async function redither(){if(!confirm('Clear the dither cache? Cached panel images are rebuilt automatically the next time each photo is displayed.'))return;"
  "const s=document.getElementById('status');s.textContent='Clearing cache\\u2026';"
  "await fetch('/redither',{method:'POST'});s.textContent='Cache cleared.';load();}"
  "async function testpat(){await fetch('/test',{method:'POST'});"
  "alert('Drawing colour bars \\u2014 the panel should fill top-to-bottom with 6 stripes (~30s).');}"
  "async function prep(f){try{"
  "const bm=await createImageBitmap(f,{imageOrientation:'from-image'});"
  "const c=document.createElement('canvas');c.width=800;c.height=480;"
  "const k=Math.max(800/bm.width,480/bm.height);const sw=800/k,sh=480/k;"
  "c.getContext('2d').drawImage(bm,(bm.width-sw)/2,(bm.height-sh)/2,sw,sh,0,0,800,480);bm.close();"
  "const b=await new Promise(r=>c.toBlob(r,'image/jpeg',0.9));if(!b)return f;"
  "return new File([b],f.name.replace(/\\.[^.]*$/,'')+'.jpg',{type:'image/jpeg'});"
  "}catch(err){return f;}}"
  "async function mkthumb(f){try{"
  "const bm=await createImageBitmap(f);"
  "const c=document.createElement('canvas');c.width=400;c.height=240;"
  "const k=Math.max(400/bm.width,240/bm.height);const sw=400/k,sh=240/k;"
  "c.getContext('2d').drawImage(bm,(bm.width-sw)/2,(bm.height-sh)/2,sw,sh,0,0,400,240);bm.close();"
  "return await new Promise(r=>c.toBlob(r,'image/jpeg',0.8));"
  "}catch(err){return null;}}"
  "async function up(e){e.preventDefault();const fs=document.getElementById('file').files;"
  "if(!fs.length)return false;const s=document.getElementById('status');"
  "for(let i=0;i<fs.length;i++){s.textContent='Converting '+(i+1)+'/'+fs.length+'\\u2026';"
  "const f=await prep(fs[i]);"
  "s.textContent='Uploading '+(i+1)+'/'+fs.length+'\\u2026';"
  "const fd=new FormData();fd.append('file',f);"
  "try{const r=await fetch('/upload',{method:'POST',body:fd});const j=await r.json();"
  "if(j.ok&&j.name){const tb=await mkthumb(f);"
  "if(tb){const td=new FormData();td.append('file',tb,'t.jpg');"
  "await fetch('/thumb?name='+encodeURIComponent(j.name),{method:'POST',body:td});}}"
  "}catch(err){}}"
  "s.textContent='Uploaded '+fs.length+' file(s).';"
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
          bool cached = SD.exists(String(DITHERED_DIR "/") + base + ".bin");
          bool failed = !cached && isFailedBase(base);
          if (!first) json += ",";
          first = false;
          json += "{\"name\":\"" + jsonEscape(n) + "\",\"cached\":" +
                  (cached ? "true" : "false") + ",\"failed\":" +
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

// A photo's content never changes under a given name (uploads de-duplicate
// names, thumbs are written once), so let the browser cache images for a week
// instead of re-downloading the whole gallery on every visit.
static const char CACHE_WEEK[] = "max-age=604800";

// streamFile() reads the SD card ~1.4 KB at a time; reading in much larger
// chunks roughly doubles throughput. Buffer lives in internal RAM (the
// SD-over-SPI driver's DMA can't touch PSRAM).
static void streamFileFast(File& f, const String& type) {
  static uint8_t chunk[16384];
  server.setContentLength(f.size());
  server.send(200, type.c_str(), "");
  WiFiClient client = server.client();
  int n;
  while ((n = f.read(chunk, sizeof(chunk))) > 0) {
    size_t off = 0;
    while (off < (size_t)n) {
      size_t w = client.write(chunk + off, n - off);
      if (!w) return;   // client gone
      off += w;
    }
  }
}

static void handleOriginal() {
  String name = safeName(server.arg("file"));
  if (!validImageName(name)) { server.send(400, "text/plain", "bad name"); return; }
  File f = SD.open(String(ORIGINALS_DIR "/") + name);
  if (!f) { server.send(404, "text/plain", "not found"); return; }
  server.sendHeader("Cache-Control", CACHE_WEEK);
  streamFileFast(f, contentTypeFor(name));
  f.close();
}

// Serve /thumbs/<base>.jpg (browser-uploaded) or .bmp (device-generated).
// 404 makes the gallery <img> fall back to the full original.
static void handleThumb() {
  String name = safeName(server.arg("file"));
  if (!validImageName(name)) { server.send(400, "text/plain", "bad name"); return; }
  String base = baseNoExt(name);
  String type = "image/jpeg";
  File f = SD.open(String(THUMBS_DIR "/") + base + ".jpg");
  if (!f) { f = SD.open(String(THUMBS_DIR "/") + base + ".bmp"); type = "image/bmp"; }
  if (!f) { server.send(404, "text/plain", "no thumb"); return; }
  server.sendHeader("Cache-Control", CACHE_WEEK);
  streamFileFast(f, type);
  f.close();
}

// Browser-generated JPEG thumbnail, sent right after a photo upload. The
// original's final (de-duplicated) name arrives in the query string.
static void handleThumbUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    thumbStored = false;
    String name = safeName(server.arg("name"));
    if (!validImageName(name)) return;
    ensureDirs();
    thumbFile = SD.open((String(THUMBS_DIR "/") + baseNoExt(name) + ".jpg").c_str(), FILE_WRITE);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (thumbFile) thumbFile.write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    if (thumbFile) {
      thumbFile.close();
      thumbStored = true;
    }
  }
}

static void handleThumbDone() {
  server.send(thumbStored ? 200 : 400, "application/json",
              thumbStored ? "{\"ok\":true}" : "{\"ok\":false}");
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
  String tj = String(THUMBS_DIR "/") + base + ".jpg";
  String tb = String(THUMBS_DIR "/") + base + ".bmp";
  if (SD.exists(tj)) SD.remove(tj);
  if (SD.exists(tb)) SD.remove(tb);
  clearFailedBase(base);

  server.send(removed ? 200 : 404, "application/json",
              removed ? "{\"ok\":true}" : "{\"ok\":false}");
}

static void handleShow() {
  String name = safeName(server.arg("file"));
  if (!validImageName(name)) { server.send(400, "application/json", "{\"ok\":false}"); return; }
  if (!SD.exists(String(ORIGINALS_DIR "/") + name)) {
    server.send(404, "application/json", "{\"ok\":false,\"error\":\"no such photo\"}");
    return;
  }
  // May take a few seconds on a cache miss (decode + dither); the WebServer is
  // synchronous, so the client just waits for the response.
  if (!ensureDitheredInBuffer(name)) {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"processing failed\"}");
    return;
  }
  server.send(200, "application/json", "{\"ok\":true}");
  epdDisplayCurrentBuffer();   // slow refresh; client already acknowledged
}

// Upload: stream the original straight into /originals. Dithering happens
// lazily the first time the photo is displayed (ensureDitheredInBuffer).
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

// Clear the dither cache: delete every cached buffer. Each photo is rebuilt
// lazily from its original the next time it is displayed. Use after changing
// the dither pipeline or to recover corrupt .bins.
static void handleRedither() {
  int removed = 0;
  while (true) {
    File dir = SD.open(DITHERED_DIR);
    if (!dir) break;
    String victim;
    File e;
    while ((e = dir.openNextFile())) {
      if (!e.isDirectory()) {
        String n = baseNameOf(String(e.name()));
        String low = n; low.toLowerCase();
        if (low.endsWith(".bin")) { victim = String(DITHERED_DIR "/") + n; e.close(); break; }
      }
      e.close();
    }
    dir.close();
    if (victim.length() == 0) break;     // no more .bin files
    if (!SD.remove(victim)) break;       // stop if a delete fails
    removed++;
  }

  clearAllFailed();
  server.send(200, "application/json", "{\"ok\":true,\"removed\":" + String(removed) + "}");
}

// Diagnostic: draw 6 colour bars straight into the framebuffer and display them.
// Bypasses SD / decode / dither / .bin entirely, so it isolates the panel write
// path from the image pipeline. If the bars fill the screen but photos don't,
// the problem is in the image/.bin path, not the EPD driver.
static void handleTest() {
  epdDrawHorizontalBars();
  server.send(200, "application/json", "{\"ok\":true}");
  epdDisplayCurrentBuffer();
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
  server.on("/thumb",      HTTP_GET,  handleThumb);
  server.on("/thumb",      HTTP_POST, handleThumbDone, handleThumbUpload);
  server.on("/delete",     HTTP_POST, handleDelete);
  server.on("/show",       HTTP_POST, handleShow);
  server.on("/redither",   HTTP_POST, handleRedither);
  server.on("/test",       HTTP_POST, handleTest);
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
