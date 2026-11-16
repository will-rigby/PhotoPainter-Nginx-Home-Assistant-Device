#pragma once
#include <stdint.h>
#include <stddef.h>
// Hashing is a deterministic stand-in; these tests verify identity inputs and
// storage behavior, not Espressif's independently supplied SHA implementation.
inline int mbedtls_sha256(const uint8_t* data,size_t n,uint8_t* out,int) {
  uint64_t h=14695981039346656037ull;
  for(size_t i=0;i<n;++i) h=(h^data[i])*1099511628211ull;
  for(int i=0;i<32;++i){h^=h<<13;h^=h>>7;h^=h<<17;out[i]=uint8_t(h);}
  return 0;
}
