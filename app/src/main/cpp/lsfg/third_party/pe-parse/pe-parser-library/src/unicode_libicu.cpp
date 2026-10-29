// Replacement for the ICU based converter (ICU is not available in the Android NDK).
// Converts UTF-16 to UTF-8, which is all pe-parse needs for resource and export names.
#include <cstdint>
#include <pe-parse/to_string.h>

namespace peparse {
std::string from_utf16(const UCharString &u) {
  std::string out;
  out.reserve(u.size());
  for (size_t i = 0; i < u.size(); i++) {
    uint32_t c = static_cast<uint16_t>(u[i]);
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < u.size()) {
      uint32_t d = static_cast<uint16_t>(u[i + 1]);
      if (d >= 0xDC00 && d < 0xE000) {
        c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
        i++;
      }
    }
    if (c < 0x80) {
      out += static_cast<char>(c);
    } else if (c < 0x800) {
      out += static_cast<char>(0xC0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out += static_cast<char>(0xE0 | (c >> 12));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (c >> 18));
      out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    }
  }
  return out;
}
} // namespace peparse
