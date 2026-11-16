// Minimal host adapter for testing the actual photo_store.cpp/storage.cpp.
#pragma once
#include <string>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <ctime>
class String {
  std::string value;
public:
  String() = default;
  String(const char* s) : value(s ? s : "") {}
  String(const std::string& s) : value(s) {}
  String(unsigned int n) : value(std::to_string(n)) {}
  String(long long n) : value(std::to_string(n)) {}
  size_t length() const { return value.size(); }
  const char* c_str() const { return value.c_str(); }
  char operator[](size_t n) const { return value[n]; }
  int indexOf(char c) const { auto n=value.find(c);return n==std::string::npos?-1:int(n); }
  int indexOf(const char* c) const { auto n=value.find(c);return n==std::string::npos?-1:int(n); }
  int lastIndexOf(char c) const { auto n=value.rfind(c);return n==std::string::npos?-1:int(n); }
  String substring(size_t start) const { return start>value.size()?String():String(value.substr(start)); }
  String substring(size_t start,size_t end) const { return String(value.substr(start,end-start)); }
  bool endsWith(const char* text) const { size_t n=strlen(text);return value.size()>=n && value.compare(value.size()-n,n,text)==0; }
  void toLowerCase() { std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return std::tolower(c);}); }
  bool equalsIgnoreCase(const String& other) const { String a=*this,b=other;a.toLowerCase();b.toLowerCase();return a==b; }
  String& operator+=(char c) { value+=c;return *this; }
  String& operator+=(const String& s) { value+=s.value;return *this; }
  friend String operator+(const String& a,const String& b) { return String(a.value+b.value); }
  friend bool operator==(const String& a,const String& b) { return a.value==b.value; }
  friend bool operator!=(const String& a,const String& b) { return !(a==b); }
};
inline size_t strlcpy(char* dst,const char* src,size_t n) { size_t len=strlen(src);if(n){size_t take=std::min(n-1,len);memcpy(dst,src,take);dst[take]=0;}return len; }
inline uint32_t esp_random() { static uint32_t n=12345;n=n*1664525u+1013904223u;return n; }
