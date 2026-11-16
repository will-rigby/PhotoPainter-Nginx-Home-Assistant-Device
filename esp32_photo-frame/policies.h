// SPDX-License-Identifier: 0BSD
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

inline bool parsePort(const char* text, uint16_t& result) {
  if (!*text) { result = 1883; return true; }
  uint32_t value = 0;
  for (; *text; ++text) {
    if (*text < '0' || *text > '9') return false;
    value = value * 10 + (*text - '0');
    if (value > 65535) return false;
  }
  if (!value) return false;
  result = uint16_t(value); return true;
}
inline bool validHostname(const char* name) {
  size_t n = strlen(name);
  if (!n || n > 63 || name[0] == '-' || name[n - 1] == '-') return false;
  for (size_t i = 0; i < n; ++i)
    if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= '0' && name[i] <= '9') || name[i] == '-')) return false;
  return true;
}
inline bool imageSignature(const uint8_t* b, size_t n, bool bmp) {
  return bmp ? n >= 54 && b[0] == 'B' && b[1] == 'M' :
               n >= 3 && b[0] == 0xff && b[1] == 0xd8 && b[2] == 0xff;
}
struct UploadAccounting {
  size_t received = 0;
  bool failed = false;
  bool add(size_t n, size_t written, size_t limit) {
    if (failed || n > limit || received > limit - n || written != n) { failed = true; return false; }
    received += n; return true;
  }
  bool complete(size_t reported) const { return !failed && received && received == reported; }
};
