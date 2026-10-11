// The first-boot admin password: random, drawn from an RNG the board provides, never shown.
//
// Host-portable: no Arduino, no MeshCore. spec/SYSTEM.md S-13 says no default password is ever on
// the air; a repeater that boots with no saved preferences gives itself a password nobody knows,
// and a person at the USB port (`password <new>`) or the Walter over the link sets the real one.
// The board side (src/app/FirstBoot.cpp) decides *when* and supplies the radio's RNG; this file
// decides *what* a password looks like, and the native tests (test/test_password) hold it to that.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace twinoak {

// Where random bytes come from. On the board it is the radio's noise listener; in a test, a fake.
class RandomSource {
 public:
  virtual ~RandomSource() = default;
  virtual void fill(uint8_t* dest, size_t len) = 0;
};

// The alphabet a password is drawn from: A-Z, a-z, 0-9 - 62 characters, none of them a space, a
// quote or anything upstream's command line or login payload treats specially.
extern const char kPasswordAlphabet[];
constexpr size_t kPasswordAlphabetSize = 62;

// Fills out with out_size - 1 characters from the alphabet and a terminating NUL; writes nothing
// when out_size is 0. Each character is one random byte reduced modulo 62, and a byte of 248 or
// more is thrown away first (248 is the largest multiple of 62 below 256), so every character of
// the alphabet is equally likely. Returns the number of characters written.
size_t generatePassword(char* out, size_t out_size, RandomSource& rng);

// Whether the admin password in the preferences must be replaced by a fresh one: when no saved
// preferences were found at boot (upstream's constructor default is then still in it), and also
// when the password equals the build's default whatever the file said - belt and braces, so the
// build's ADMIN_PASSWORD define never survives a boot.
bool needsFreshPassword(bool saved_prefs_found, const char* current, const char* build_default);

}  // namespace twinoak
