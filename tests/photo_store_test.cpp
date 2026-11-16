// Executes the production photo store with an in-memory filesystem.
#include "../esp32_photo-frame/config.h"
#include <assert.h>
#include <stdio.h>
static bool leased=false;
void operationError(const String&){}
void storageChanged(){}
bool photoInUse(const String&){return leased;}
bool maintenanceBusy(){return false;}
static void source(const String& name,const char* bytes){
  File f=SD.open(String(ORIGINALS_DIR "/")+name,FILE_WRITE);
  assert(f.write((const uint8_t*)bytes,strlen(bytes))==strlen(bytes));
}
int main(){
  synchronizationBegin();
  SD.mkdir(ORIGINALS_DIR);SD.mkdir(DITHERED_DIR);SD.mkdir(THUMBS_DIR);
  source("holiday.jpg","JPEG data");source("holiday.bmp","BMP bytes");
  File old=SD.open("/dithered/holiday.bin",FILE_WRITE);old.write((const uint8_t*)"old",3);old.close();
  assert(photoStoreBegin());
  assert(SD.exists("/originals/holiday.jpg") && SD.exists("/originals/holiday.bmp"));
  assert(!SD.exists("/dithered/holiday.bin"));
  PhotoMeta jpg,bmp,again;
  assert(photoMeta("holiday.jpg",jpg)&&photoMeta("holiday.bmp",bmp));
  assert(photoKey("holiday.jpg")!=photoKey("holiday.bmp"));
  assert(photoKey("HOLIDAY.JPG")==photoKey("holiday.jpg"));
  assert(photoCachePath(jpg)!=photoCachePath(bmp));assert(photoThumbPath(jpg,true)!=photoThumbPath(bmp,true));
  assert(photoMeta("holiday.jpg",again)&&String(jpg.revision)==again.revision);

  // A valid record copied under the wrong key must not change photo identity.
  SD.entries[(String("/photo-meta/")+photoKey("holiday.jpg")).c_str()] =
    std::make_shared<Entry>(*SD.entries[(String("/photo-meta/")+photoKey("holiday.bmp")).c_str()]);
  assert(photoMeta("holiday.jpg",again)&&String(again.name)=="holiday.jpg");

  // A receipt is only replayable after the source commit and while its revision matches.
  PhotoMeta upload{};strlcpy(upload.name,"retry.jpg",sizeof(upload.name));upload.size=4;upload.modified=123456;
  strlcpy(upload.revision,newRevision().c_str(),33);strlcpy(upload.thumbnail,newRevision().c_str(),33);
  strlcpy(upload.token,newRevision().c_str(),33);
  assert(saveUploadReceipt(upload));assert(savePhotoMeta(upload));
  assert(!findUploadToken(upload.token,again));source("retry.jpg","1234");
  assert(findUploadToken(upload.token,again)&&String(again.name)=="retry.jpg");
  PhotoMeta replacement=upload;strlcpy(replacement.revision,newRevision().c_str(),33);
  assert(savePhotoMeta(replacement));assert(!findUploadToken(upload.token,again));

  // Metadata corruption recovers from a valid backup.
  String p=String("/photo-meta/")+photoKey("retry.jpg");
  SD.entries[(p+".bak").c_str()]=std::make_shared<Entry>(*SD.entries[p.c_str()]);
  SD.entries[p.c_str()]->bytes[0]=0;
  assert(photoMeta("retry.jpg",again)&&String(again.revision)==replacement.revision);
  SD.failRenames=true;assert(!savePhotoMeta(again));SD.failRenames=false;
  assert(photoMeta("retry.jpg",again)&&String(again.revision)==replacement.revision);

  // Full cards do not hide existing photos or prevent freeing their space.
  source("legacy.jpg","existing photo");SD.failWrites=true;
  assert(photoMeta("legacy.jpg",again));PhotoMeta fallback;
  assert(photoMeta("legacy.jpg",fallback)&&String(fallback.revision)==again.revision);
  String error;leased=true;assert(!deletePhoto("legacy.jpg",error));assert(SD.exists("/originals/legacy.jpg"));
  leased=false;assert(deletePhoto("legacy.jpg",error));assert(!SD.exists("/originals/legacy.jpg"));SD.failWrites=false;

  File partial=SD.open("/upload-temp/interrupted.part",FILE_WRITE);partial.write((const uint8_t*)"partial",7);partial.close();
  cleanupTemporaryFiles();assert(!SD.exists("/upload-temp/interrupted.part"));assert(SD.exists("/originals/holiday.jpg"));
  assert(!validPhotoName("../holiday.jpg")&&!validPhotoName("a/b.jpg"));
  assert(jsonEscape("a\n\"b\\") == "a\\u000a\\\"b\\\\");
  puts("Photo store: migration, cache identities, receipt replay, corruption recovery, full cards, and leases passed.");
}
