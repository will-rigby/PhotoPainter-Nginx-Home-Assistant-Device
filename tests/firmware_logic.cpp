// SPDX-License-Identifier: 0BSD
#include "../esp32_photo-frame/policies.h"
#include "../esp32_photo-frame/transactions.h"
#include <assert.h>
#include <stdio.h>
#include <string>
#include <map>
#include <stdexcept>

struct FakeFS {
  std::map<std::string,std::string> files;
  int operation = 0, failAt = 0, powerLossAt = 0;
  bool mutation() {
    ++operation;
    if (operation == powerLossAt) throw std::runtime_error("power loss");
    return operation != failAt;
  }
  bool exists(const std::string& name) { return files.count(name); }
  bool remove(const std::string& name) { return mutation() && files.erase(name); }
  bool rename(const std::string& a, const std::string& b) {
    if (!mutation() || !exists(a) || exists(b)) return false;
    files[b] = files[a]; files.erase(a); return true;
  }
};
struct FakePreferences {
  bool open = true, shortWrite = false, ended = false;
  int writes = 0;
  bool begin(const char*,bool) { return open; }
  size_t putBytes(const char*,const void*,size_t n) { ++writes; return shortWrite ? n-1 : n; }
  void end() { ended = true; }
};
struct Job { int id = 0; };

int main() {
  uint16_t port = 0;
  assert(parsePort("",port) && port == 1883);
  assert(parsePort("1",port) && port == 1);
  assert(parsePort("65535",port) && port == 65535);
  for (const char* bad : {"0","65536","-1","1.5","abc","1883x","999999999999999999999"}) assert(!parsePort(bad,port));
  assert(validHostname("photoframe-2"));
  for (const char* bad : {"","-frame","frame-","frame.local","Frame","photo frame"}) assert(!validHostname(bad));
  assert(std::string(savedPassword("secret","",false)) == "secret");
  assert(std::string(savedPassword("secret","new",false)) == "new");
  assert(std::string(savedPassword("secret","",true)).empty());

  UploadAccounting upload;
  assert(upload.add(1024,1024,4096));
  assert(!upload.complete(2048)); // disconnected before the final chunk
  assert(!upload.add(1024,512,4096)); // full SD card / short write
  assert(!upload.complete(2048));
  upload = {}; assert(upload.add(4096,4096,4096)); assert(upload.complete(4096));
  assert(!upload.add(1,1,4096)); assert(!upload.complete(4096));
  upload = {}; assert(!upload.complete(0)); assert(!upload.add(SIZE_MAX,SIZE_MAX,4096));
  uint8_t jpeg[] = {0xff,0xd8,0xff}; assert(imageSignature(jpeg,3,false));
  assert(!imageSignature(jpeg,2,false)); assert(!imageSignature(jpeg,3,true));
  uint8_t bmp[54] = {'B','M'}; assert(imageSignature(bmp,54,true)); assert(!imageSignature(bmp,53,true));

  for (int failure = 0; failure <= 3; ++failure) {
    FakeFS fs; fs.files = {{"record","old"},{"temp","new"}}; fs.failAt = failure;
    bool ok = replaceRecord(fs,std::string("temp"),std::string("record"),std::string("backup"));
    if (ok) assert(fs.files["record"] == "new");
    else assert((fs.exists("record") && fs.files["record"] == "old") || (fs.exists("backup") && fs.files["backup"] == "old"));
  }
  for (int interrupted = 1; interrupted <= 3; ++interrupted) {
    FakeFS fs; fs.files = {{"record","old"},{"temp","new"}}; fs.powerLossAt = interrupted;
    try { replaceRecord(fs,std::string("temp"),std::string("record"),std::string("backup")); } catch (const std::runtime_error&) {}
    assert(fs.exists("record") || fs.exists("backup"));
    if (fs.exists("record")) assert(fs.files["record"] == "old" || fs.files["record"] == "new");
  }
  for (int failure = 0; failure < 3; ++failure) {
    FakePreferences prefs; prefs.open = failure != 1; prefs.shortWrite = failure == 2;
    struct Record { int version, port; } next{2,1884}, current{2,1883};
    if (persistSettings(prefs,"mqtt_v2",next)) current = next;
    assert(current.port == (failure ? 1883 : 1884)); assert(prefs.ended);
    if (!prefs.open) assert(!prefs.writes);
  }
  Job active{}, pending{}, taken{};
  assert(enqueueLatest(active,pending,Job{1},true));
  assert(startPending(active,pending,taken) && taken.id == 1);
  assert(enqueueLatest(active,pending,Job{2},true));
  assert(enqueueLatest(active,pending,Job{3},true));
  assert(active.id == 1 && pending.id == 3);
  assert(!enqueueLatest(active,pending,Job{4},false)); // automatic rotation cannot displace manual work
  assert(!startPending(active,pending,taken));
  active = {}; assert(startPending(active,pending,taken) && taken.id == 3);
  puts("Firmware logic: upload failures, record recovery, settings, and latest-request scheduling passed.");
}
