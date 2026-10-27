#include "core/tga.h"

#include <cstring>

namespace nfh {

const char* pixelFormatName(TgaPixelFormat f) {
  switch (f) {
    case TgaPixelFormat::ARGB4444: return "ARGB4444";
    case TgaPixelFormat::RGB565: return "RGB565";
    case TgaPixelFormat::RGB555: return "RGB555";
    case TgaPixelFormat::BGR24: return "BGR24";
    case TgaPixelFormat::BGRA32: return "BGRA32";
    case TgaPixelFormat::Unknown: break;
  }
  return "unknown";
}

namespace {

inline uint8_t x4(uint32_t v) { return uint8_t(v * 17); }
inline uint8_t x5(uint32_t v) { return uint8_t((v << 3) | (v >> 2)); }
inline uint8_t x6(uint32_t v) { return uint8_t((v << 2) | (v >> 4)); }

void convertPixel(const uint8_t* src, TgaPixelFormat fmt, uint8_t* dst) {
  switch (fmt) {
    case TgaPixelFormat::ARGB4444: {
      uint32_t v = src[0] | (src[1] << 8);
      dst[0] = x4((v >> 8) & 15);
      dst[1] = x4((v >> 4) & 15);
      dst[2] = x4(v & 15);
      dst[3] = x4((v >> 12) & 15);
      break;
    }
    case TgaPixelFormat::RGB565: {
      uint32_t v = src[0] | (src[1] << 8);
      dst[0] = x5((v >> 11) & 31);
      dst[1] = x6((v >> 5) & 63);
      dst[2] = x5(v & 31);
      dst[3] = 255;
      break;
    }
    case TgaPixelFormat::RGB555: {
      uint32_t v = src[0] | (src[1] << 8);
      dst[0] = x5((v >> 10) & 31);
      dst[1] = x5((v >> 5) & 31);
      dst[2] = x5(v & 31);
      dst[3] = 255;
      break;
    }
    case TgaPixelFormat::BGR24:
      dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = 255;
      break;
    case TgaPixelFormat::BGRA32:
      dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = src[3];
      break;
    case TgaPixelFormat::Unknown:
      std::memset(dst, 0, 4);
      break;
  }
}

}  // namespace

bool decodeTga(const uint8_t* d, size_t n, Image& out, std::string* err) {
  auto fail = [&](const char* m) {
    if (err) *err = m;
    return false;
  };
  if (n < 18) return fail("TGA: file too small");

  const uint8_t idLen = d[0], cmapType = d[1], type = d[2];
  const int w = d[12] | (d[13] << 8);
  const int h = d[14] | (d[15] << 8);
  const uint8_t bpp = d[16], desc = d[17];
  const int alphaBits = desc & 0x0F;
  const bool topDown = (desc & 0x20) != 0;
  const bool rightToLeft = (desc & 0x10) != 0;

  if (cmapType != 0) return fail("TGA: colour-mapped images are not supported");
  if (type != 2 && type != 10) return fail("TGA: only true-colour images (type 2/10) are supported");
  if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return fail("TGA: bad dimensions");

  TgaPixelFormat fmt;
  switch (bpp) {
    case 16: fmt = alphaBits == 4 ? TgaPixelFormat::ARGB4444 : alphaBits == 1 ? TgaPixelFormat::RGB555 : TgaPixelFormat::RGB565; break;
    case 24: fmt = TgaPixelFormat::BGR24; break;
    case 32: fmt = TgaPixelFormat::BGRA32; break;
    default: return fail("TGA: unsupported bits per pixel");
  }
  const size_t bytesPP = bpp / 8;

  out.width = w;
  out.height = h;
  out.sourceFormat = fmt;
  out.rgba.assign(size_t(w) * h * 4, 0);

  size_t p = 18 + idLen;
  const size_t total = size_t(w) * h;

  auto store = [&](size_t idx, const uint8_t* px) {
    int x = int(idx % w), y = int(idx / w);
    if (!topDown) y = h - 1 - y;
    if (rightToLeft) x = w - 1 - x;
    convertPixel(px, fmt, &out.rgba[(size_t(y) * w + x) * 4]);
  };

  if (type == 2) {
    if (p + total * bytesPP > n) return fail("TGA: truncated pixel data");
    for (size_t i = 0; i < total; ++i, p += bytesPP) store(i, d + p);
  } else {
    size_t i = 0;
    while (i < total) {
      if (p >= n) return fail("TGA: truncated RLE data");
      const uint8_t hdr = d[p++];
      const size_t cnt = (hdr & 0x7F) + 1;
      if (hdr & 0x80) {
        if (p + bytesPP > n) return fail("TGA: truncated RLE packet");
        for (size_t k = 0; k < cnt && i < total; ++k) store(i++, d + p);
        p += bytesPP;
      } else {
        if (p + cnt * bytesPP > n) return fail("TGA: truncated raw packet");
        for (size_t k = 0; k < cnt && i < total; ++k, p += bytesPP) store(i++, d + p);
      }
    }
  }
  return true;
}

}  // namespace nfh
