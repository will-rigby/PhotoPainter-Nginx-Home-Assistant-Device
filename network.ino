// --------------------------------------------------
// WiFi + HTTP (nginx image server)
// --------------------------------------------------

#define CONNECT_TIMEOUT   15000
#define HTTP_TIMEOUT      30000
#define DOWNLOAD_TIMEOUT  120000

// --------------------------------------------------
// WiFi
// --------------------------------------------------

bool wifiConnect() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("ESP32-PhotoFrame");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > CONNECT_TIMEOUT) {
      Serial.println("\nWiFi connection timeout!");
      return false;
    }
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("Connected — IP: ");
  Serial.println(WiFi.localIP());
  return true;
}

void wifiDisconnect() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("WiFi disconnected");
}

// --------------------------------------------------
// Fetch image list (parse nginx autoindex HTML)
// --------------------------------------------------

bool fetchImageList() {
  imageCount = 0;

  HTTPClient http;
  String url = String(SERVER) + "/";

  Serial.print("Fetching image list from: ");
  Serial.println(url);

  http.begin(url);
  http.setTimeout(HTTP_TIMEOUT);

  int httpCode = http.GET();
  if (httpCode != 200) {
    Serial.print("HTTP error: ");
    Serial.println(httpCode);
    http.end();
    return false;
  }

  String html = http.getString();
  http.end();

  // Parse <a href="filename"> entries from nginx autoindex
  int pos = 0;
  while (pos < (int)html.length() && imageCount < MAX_IMAGES) {
    int hrefStart = html.indexOf("href=\"", pos);
    if (hrefStart < 0) break;
    hrefStart += 6;  // skip past  href="

    int hrefEnd = html.indexOf("\"", hrefStart);
    if (hrefEnd < 0) break;

    String filename = html.substring(hrefStart, hrefEnd);
    pos = hrefEnd + 1;

    // Skip parent directory links and query strings
    if (filename.startsWith("..") || filename.startsWith("/") ||
        filename.startsWith("?")  || filename.endsWith("/"))
      continue;

    // Filter by image extension
    String lower = filename;
    lower.toLowerCase();
    if (lower.endsWith(".jpg") || lower.endsWith(".jpeg") ||
        lower.endsWith(".bmp")) {
      imageList[imageCount++] = filename;
    }
  }

  Serial.print("Found ");
  Serial.print(imageCount);
  Serial.println(" images");

  for (int i = 0; i < imageCount; i++) {
    Serial.print("  [");
    Serial.print(i);
    Serial.print("] ");
    Serial.println(imageList[i]);
  }

  return imageCount > 0;
}

// --------------------------------------------------
// Download image to SD card
// --------------------------------------------------

bool downloadImageToSD(const String& filename, const String& localPath) {
  HTTPClient http;
  String url = String(SERVER) + "/" + filename;

  Serial.print("Downloading: ");
  Serial.println(url);

  http.begin(url);
  http.setTimeout(DOWNLOAD_TIMEOUT);

  int httpCode = http.GET();
  if (httpCode != 200) {
    Serial.print("Download HTTP error: ");
    Serial.println(httpCode);
    http.end();
    return false;
  }

  int contentLength = http.getSize();
  Serial.print("Content length: ");
  Serial.println(contentLength);

  if (SD.exists(localPath)) {
    SD.remove(localPath);
  }

  File f = SD.open(localPath, FILE_WRITE);
  if (!f) {
    Serial.println("Failed to open SD file for writing!");
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t dlBuf[4096];
  int totalRead = 0;
  uint32_t lastProgress = 0;

  while (http.connected() && (contentLength < 0 || totalRead < contentLength)) {
    int avail = stream->available();
    if (avail <= 0) { delay(1); continue; }

    int toRead = min(avail, (int)sizeof(dlBuf));
    int bytesRead = stream->read(dlBuf, toRead);
    if (bytesRead <= 0) break;

    f.write(dlBuf, bytesRead);
    totalRead += bytesRead;

    if (totalRead - lastProgress >= 51200) {
      Serial.print("  ");
      Serial.print(totalRead / 1024);
      Serial.println(" KB");
      lastProgress = totalRead;
    }
  }

  f.close();
  http.end();

  Serial.print("Download complete: ");
  Serial.print(totalRead / 1024);
  Serial.println(" KB");

  return totalRead > 0;
}
