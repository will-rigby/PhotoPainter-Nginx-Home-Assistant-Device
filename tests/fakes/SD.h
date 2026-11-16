#pragma once
#include "Arduino.h"
#include <map>
#include <memory>
#include <vector>
#define FILE_READ "r"
#define FILE_WRITE "w"
struct Entry { std::vector<uint8_t> bytes; bool directory=false; time_t modified=123456; };
struct Handle { std::shared_ptr<Entry> entry; std::string path; size_t cursor=0; };
class File {
  std::shared_ptr<Handle> handle;
public:
  File()=default;
  File(std::shared_ptr<Entry> e,const std::string& p):handle(std::make_shared<Handle>()){handle->entry=e;handle->path=p;}
  explicit operator bool() const {return bool(handle);}
  size_t size() const {return handle?handle->entry->bytes.size():0;}
  bool isDirectory(){return handle&&handle->entry->directory;}
  const char* name() const {return handle?handle->path.c_str():"";}
  time_t getLastWrite(){return handle?handle->entry->modified:0;}
  size_t read(uint8_t* b,size_t n){if(!handle)return 0;n=std::min(n,size()-handle->cursor);memcpy(b,handle->entry->bytes.data()+handle->cursor,n);handle->cursor+=n;return n;}
  size_t write(const uint8_t*,size_t);
  bool seek(uint32_t p){if(!handle||p>size())return false;handle->cursor=p;return true;}
  void flush(){}
  void close(){handle.reset();}
  File openNextFile();
};
class FakeSD {
public:
  std::map<std::string,std::shared_ptr<Entry>> entries;
  bool failWrites=false,failRenames=false;
  bool exists(const String& p){return entries.count(p.c_str());}
  File open(const String& p,const char* mode=FILE_READ){
    auto it=entries.find(p.c_str());
    if(mode[0]=='w'){
      if(failWrites)return {};
      auto node=std::make_shared<Entry>();entries[p.c_str()]=node;return File(node,p.c_str());
    }
    return it==entries.end()?File():File(it->second,p.c_str());
  }
  bool mkdir(const String& p){auto e=std::make_shared<Entry>();e->directory=true;entries[p.c_str()]=e;return true;}
  bool remove(const String& p){return entries.erase(p.c_str())!=0;}
  bool rename(const String& a,const String& b){if(failRenames||!exists(a)||exists(b))return false;entries[b.c_str()]=entries[a.c_str()];entries.erase(a.c_str());return true;}
  uint64_t totalBytes(){return 1024*1024;}
  uint64_t usedBytes(){uint64_t n=0;for(auto& e:entries)n+=e.second->bytes.size();return n;}
};
inline FakeSD SD;
inline size_t File::write(const uint8_t* bytes,size_t n){
  if(!handle||SD.failWrites)return 0;
  if(handle->cursor+n>size())handle->entry->bytes.resize(handle->cursor+n);
  memcpy(handle->entry->bytes.data()+handle->cursor,bytes,n);handle->cursor+=n;return n;
}
inline File File::openNextFile(){
  if(!isDirectory())return {};
  std::string prefix=handle->path+"/";size_t index=0;
  for(auto& item:SD.entries){
    if(item.first.compare(0,prefix.size(),prefix)!=0||item.first.find('/',prefix.size())!=std::string::npos)continue;
    if(index++==handle->cursor){++handle->cursor;return File(item.second,item.first);}
  }
  return {};
}
