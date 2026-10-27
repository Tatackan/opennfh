#include "core/text.h"

namespace nfh {

const char* encodingName(TextEncoding e) {
  switch (e) {
    case TextEncoding::Ascii: return "ASCII";
    case TextEncoding::Utf8: return "UTF-8";
    case TextEncoding::Utf8Bom: return "UTF-8 (BOM)";
    case TextEncoding::Utf16LE: return "UTF-16LE";
    case TextEncoding::Utf16BE: return "UTF-16BE";
    case TextEncoding::Legacy8Bit: return "8-bit (cp1251/cp1252)";
  }
  return "?";
}

void appendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += char(cp);
  } else if (cp < 0x800) {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  } else {
    out += char(0xF0 | (cp >> 18));
    out += char(0x80 | ((cp >> 12) & 0x3F));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
}

uint32_t nextUtf8(const std::string& s, size_t& i) {
  auto b = [&](size_t k) -> uint8_t { return k < s.size() ? uint8_t(s[k]) : 0; };
  uint8_t c = b(i);
  if (c < 0x80) { i += 1; return c; }
  int len = (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
  if (len == 0 || i + len > s.size()) { i += 1; return 0xFFFD; }
  uint32_t cp = c & (0x7F >> len);
  for (int k = 1; k < len; ++k) {
    uint8_t cc = b(i + k);
    if ((cc & 0xC0) != 0x80) { i += 1; return 0xFFFD; }
    cp = (cp << 6) | (cc & 0x3F);
  }
  i += len;
  return cp;
}

bool isValidUtf8(const uint8_t* d, size_t n) {
  size_t i = 0;
  while (i < n) {
    uint8_t c = d[i];
    if (c < 0x80) { ++i; continue; }
    int len = (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (len == 0 || i + len > n) return false;
    for (int k = 1; k < len; ++k)
      if ((d[i + k] & 0xC0) != 0x80) return false;
    i += len;
  }
  return true;
}

// Windows-1251 upper half (0x80..0xFF) -> Unicode.
static const uint16_t kCp1251[128] = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, 0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
    0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0xFFFD, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
    0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, 0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
    0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7, 0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
    0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417, 0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F,
    0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427, 0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,
    0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437, 0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,
    0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447, 0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F,
};

// Windows-1252 0x80..0x9F (0xA0..0xFF map 1:1 to Latin-1).
static const uint16_t kCp1252Low[32] = {
    0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
    0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178,
};

std::string decodeToUtf8(const uint8_t* d, size_t n, TextEncoding* detected) {
  std::string out;
  auto set = [&](TextEncoding e) { if (detected) *detected = e; };

  if (n >= 2 && ((d[0] == 0xFF && d[1] == 0xFE) || (d[0] == 0xFE && d[1] == 0xFF) ||
                 (n >= 4 && d[1] == 0 && d[3] == 0 && d[0] != 0))) {
    bool hasBom = (d[0] == 0xFF && d[1] == 0xFE) || (d[0] == 0xFE && d[1] == 0xFF);
    bool be = d[0] == 0xFE && d[1] == 0xFF;
    set(be ? TextEncoding::Utf16BE : TextEncoding::Utf16LE);
    out.reserve(n / 2);
    for (size_t i = hasBom ? 2 : 0; i + 1 < n; i += 2) {
      uint32_t u = be ? (uint32_t(d[i]) << 8 | d[i + 1]) : (uint32_t(d[i + 1]) << 8 | d[i]);
      if (u >= 0xD800 && u < 0xDC00 && i + 3 < n) {
        uint32_t lo = be ? (uint32_t(d[i + 2]) << 8 | d[i + 3]) : (uint32_t(d[i + 3]) << 8 | d[i + 2]);
        if (lo >= 0xDC00 && lo < 0xE000) {
          u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
          i += 2;
        }
      }
      appendUtf8(out, u);
    }
    return out;
  }

  if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) {
    set(TextEncoding::Utf8Bom);
    return std::string(reinterpret_cast<const char*>(d + 3), n - 3);
  }

  bool ascii = true;
  size_t cyr = 0, high = 0;
  for (size_t i = 0; i < n; ++i) {
    if (d[i] >= 0x80) {
      ascii = false;
      ++high;
      if (d[i] >= 0xC0) ++cyr;
    }
  }
  if (ascii) {
    set(TextEncoding::Ascii);
    return std::string(reinterpret_cast<const char*>(d), n);
  }
  if (isValidUtf8(d, n)) {
    set(TextEncoding::Utf8);
    return std::string(reinterpret_cast<const char*>(d), n);
  }

  set(TextEncoding::Legacy8Bit);
  // Heuristic: Russian text is dominated by 0xC0..0xFF; German comments only
  // contain a handful of umlauts.
  const bool cyrillic = high > 16 && cyr * 10 >= high * 8;
  out.reserve(n + high);
  for (size_t i = 0; i < n; ++i) {
    uint8_t c = d[i];
    if (c < 0x80) out += char(c);
    else if (cyrillic) appendUtf8(out, kCp1251[c - 0x80]);
    else if (c < 0xA0) appendUtf8(out, kCp1252Low[c - 0x80]);
    else appendUtf8(out, c);
  }
  return out;
}

}  // namespace nfh
