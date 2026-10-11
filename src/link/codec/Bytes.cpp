#include "Bytes.h"

#include <string.h>

#include "Numbers.h"

namespace twinoak {
namespace link {

Bytes bytesOfText(const char* text) {
  Bytes b;
  if (text == nullptr) return b;
  const size_t len = strlen(text);
  b.ptr = reinterpret_cast<const uint8_t*>(text);
  b.len = uint8_t(len > kMaxPayload ? kMaxPayload : len);
  return b;
}

Bytes bytesOf(const void* data, size_t len) {
  Bytes b;
  if (data == nullptr) return b;
  b.ptr = static_cast<const uint8_t*>(data);
  b.len = uint8_t(len > kMaxPayload ? kMaxPayload : len);
  return b;
}

uint8_t Reader::u8() {
  if (failed_ || i_ + 1 > n_) {
    failed_ = true;
    return 0;
  }
  return d_[i_++];
}

int8_t Reader::i8() { return int8_t(u8()); }

uint16_t Reader::u16() {
  const uint8_t lo = u8();
  const uint8_t hi = u8();
  return uint16_t(uint16_t(hi) << 8 | lo);
}

int16_t Reader::i16() { return int16_t(u16()); }

uint32_t Reader::u32() {
  const uint8_t b0 = u8();
  const uint8_t b1 = u8();
  const uint8_t b2 = u8();
  const uint8_t b3 = u8();
  return uint32_t(b0) | uint32_t(b1) << 8 | uint32_t(b2) << 16 | uint32_t(b3) << 24;
}

Bytes Reader::take(size_t n) {
  Bytes b;
  if (failed_ || n > kMaxPayload || i_ + n > n_) {
    failed_ = true;
    return b;
  }
  b.ptr = d_ + i_;
  b.len = uint8_t(n);
  i_ += n;
  return b;
}

Bytes Reader::str8() {
  const uint8_t len = u8();
  return take(len);
}

Bytes Reader::rest() {
  if (failed_) return Bytes();
  return take(n_ - i_);
}

void Writer::u8(uint8_t v) {
  if (failed_ || i_ + 1 > cap_) {
    failed_ = true;
    return;
  }
  d_[i_++] = v;
}

void Writer::i8(int8_t v) { u8(uint8_t(v)); }

void Writer::u16(uint16_t v) {
  u8(uint8_t(v & 0xFF));
  u8(uint8_t(v >> 8));
}

void Writer::i16(int16_t v) { u16(uint16_t(v)); }

void Writer::u32(uint32_t v) {
  u8(uint8_t(v & 0xFF));
  u8(uint8_t((v >> 8) & 0xFF));
  u8(uint8_t((v >> 16) & 0xFF));
  u8(uint8_t((v >> 24) & 0xFF));
}

void Writer::bytes(Bytes b) { raw(b.ptr, b.len); }

void Writer::str8(Bytes b) {
  u8(b.len);
  raw(b.ptr, b.len);
}

void Writer::raw(const uint8_t* data, size_t len) {
  if (failed_ || i_ + len > cap_ || (len > 0 && data == nullptr)) {
    failed_ = true;
    return;
  }
  memcpy(d_ + i_, data, len);
  i_ += len;
}

}  // namespace link
}  // namespace twinoak
