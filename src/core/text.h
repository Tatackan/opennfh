// Text encoding helpers. Original game XML files come in three flavours:
//   * UTF-16LE with BOM (strings.xml, leveldata.xml, dialogs, ...)
//   * plain ASCII
//   * 8-bit legacy text (German comments in level.xml, Windows-1252/1251)
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nfh {

enum class TextEncoding { Ascii, Utf8, Utf8Bom, Utf16LE, Utf16BE, Legacy8Bit };

const char* encodingName(TextEncoding e);

// Detects the encoding and converts the buffer to UTF-8.
// Legacy 8-bit text is decoded as Windows-1251 when it looks Cyrillic,
// otherwise as Windows-1252.
std::string decodeToUtf8(const uint8_t* data, size_t size, TextEncoding* detected = nullptr);
inline std::string decodeToUtf8(const std::vector<uint8_t>& v, TextEncoding* detected = nullptr) {
  return decodeToUtf8(v.data(), v.size(), detected);
}

void appendUtf8(std::string& out, uint32_t cp);
// Decodes one code point starting at s[i] and advances i. Invalid bytes yield U+FFFD.
uint32_t nextUtf8(const std::string& s, size_t& i);
bool isValidUtf8(const uint8_t* data, size_t size);

}  // namespace nfh
