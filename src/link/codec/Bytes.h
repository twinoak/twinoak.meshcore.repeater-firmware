// A view over bytes, and the Reader and Writer every message function uses for the link's
// little-endian scalars - so that no message function does pointer arithmetic by hand
// (tasks/phase-1/02-the-link-codec.md).
//
// Host-portable: no Arduino, no MeshCore. No heap, no recursion: a Reader never reads past the
// span it was given and a Writer never leaves the one it was given. Both are "sticky": the first
// read or put that does not fit sets a flag, every later one does nothing, and the caller asks
// once at the end (`ok()`). That is what lets a decoder read its fields in a straight line and
// still say *malformed* rather than reading out of bounds (LINK-PROTOCOL.md section 3,
// kErrMalformed).

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace twinoak {
namespace link {

// A borrowed run of bytes: a str8's characters, a password, a path's hashes, an advert's
// application data. It owns nothing. A view a decoder hands back points into the buffer the
// packet was decoded into and lives exactly as long as that buffer's contents (FrameReceiver).
struct Bytes {
  const uint8_t* ptr = nullptr;
  uint8_t len = 0;

  bool empty() const { return len == 0; }
};

// A view over a NUL-terminated string, for the strings the firmware hands to an encoder (a
// version, a board name, a CLI reply). The NUL is not part of the view: a str8 on the wire is a
// length and then the characters (LINK-PROTOCOL.md section 2).
Bytes bytesOfText(const char* text);

// A view over a run of bytes, truncated to what a payload field can hold.
Bytes bytesOf(const void* data, size_t len);

class Reader {
 public:
  Reader(const uint8_t* data, size_t len) : d_(data), n_(len) {}
  explicit Reader(Bytes b) : d_(b.ptr), n_(b.len) {}

  // True while every read so far fitted.
  bool ok() const { return !failed_; }
  size_t position() const { return i_; }
  size_t remaining() const { return failed_ ? 0 : n_ - i_; }

  uint8_t u8();
  int8_t i8();
  uint16_t u16();
  int16_t i16();
  uint32_t u32();

  // n bytes of the span. An empty view and a failed reader when fewer than n remain.
  Bytes take(size_t n);
  // A u8 length and that many bytes: the protocol's str8.
  Bytes str8();
  // Everything left. The protocol's *rest*.
  Bytes rest();

 private:
  const uint8_t* d_;
  size_t n_;
  size_t i_ = 0;
  bool failed_ = false;
};

class Writer {
 public:
  Writer(uint8_t* out, size_t cap) : d_(out), cap_(cap) {}

  // True while everything put so far fitted.
  bool ok() const { return !failed_; }
  // How many bytes have been put. Meaningless unless ok().
  size_t size() const { return i_; }

  void u8(uint8_t v);
  void i8(int8_t v);
  void u16(uint16_t v);
  void i16(int16_t v);
  void u32(uint32_t v);

  void bytes(Bytes b);
  // A u8 length and the bytes: the protocol's str8. Fails when the view is longer than 255.
  void str8(Bytes b);
  void raw(const uint8_t* data, size_t len);

 private:
  uint8_t* d_;
  size_t cap_;
  size_t i_ = 0;
  bool failed_ = false;
};

}  // namespace link
}  // namespace twinoak
