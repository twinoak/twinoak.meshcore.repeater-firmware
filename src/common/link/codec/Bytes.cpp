#include "common/link/codec/Bytes.h"

#include <string.h>

#include "common/link/codec/Numbers.h"

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

bool isPlainText(Bytes b) {
  size_t i = 0;
  while (i < b.len) {
    const uint8_t c = b.ptr[i];
    if (c < 0x20 || c == 0x7F) return false;  // a C0 control, or DEL
    if (c < 0x80) {                           // plain ASCII
      i++;
      continue;
    }
    // A multi-byte sequence: how many continuation bytes, and the smallest code point this many
    // bytes is allowed to encode (which is what rejects an overlong encoding).
    size_t extra = 0;
    uint32_t code = 0;
    uint32_t least = 0;
    if ((c & 0xE0) == 0xC0) {
      extra = 1;
      code = c & 0x1Fu;
      least = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
      extra = 2;
      code = c & 0x0Fu;
      least = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
      extra = 3;
      code = c & 0x07u;
      least = 0x10000;
    } else {
      return false;  // 0x80-0xBF cannot begin a sequence, and 0xF8-0xFF cannot at all
    }
    if (extra > size_t(b.len) - i - 1) return false;  // truncated: too few bytes left
    for (size_t k = 1; k <= extra; k++) {
      const uint8_t cont = b.ptr[i + k];
      if ((cont & 0xC0) != 0x80) return false;
      code = (code << 6) | (cont & 0x3Fu);
    }
    if (code < least) return false;                            // overlong
    if (code > 0x10FFFF) return false;                         // above Unicode
    if (code >= 0xD800 && code <= 0xDFFF) return false;        // a surrogate half
    if (code >= 0x80 && code <= 0x9F) return false;            // a C1 control
    i += extra + 1;
  }
  return true;
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
