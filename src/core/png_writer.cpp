#include "core/png_writer.h"

#include <zlib.h>

#include <cstring>

namespace nfh {

namespace {

void put32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(uint8_t(x >> 24));
  v.push_back(uint8_t(x >> 16));
  v.push_back(uint8_t(x >> 8));
  v.push_back(uint8_t(x));
}

void chunk(std::vector<uint8_t>& out, const char type[4], const uint8_t* data, size_t len) {
  put32(out, (uint32_t)len);
  size_t start = out.size();
  out.insert(out.end(), type, type + 4);
  if (len) out.insert(out.end(), data, data + len);
  uint32_t crc = (uint32_t)crc32(0L, out.data() + start, (uInt)(len + 4));
  put32(out, crc);
}

}  // namespace

std::vector<uint8_t> encodePng(const Image& img) {
  std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

  uint8_t ihdr[13];
  auto be32 = [](uint8_t* p, uint32_t x) { p[0] = uint8_t(x >> 24); p[1] = uint8_t(x >> 16); p[2] = uint8_t(x >> 8); p[3] = uint8_t(x); };
  be32(ihdr, (uint32_t)img.width);
  be32(ihdr + 4, (uint32_t)img.height);
  ihdr[8] = 8;   // bit depth
  ihdr[9] = 6;   // RGBA
  ihdr[10] = 0;  // deflate
  ihdr[11] = 0;  // adaptive filtering
  ihdr[12] = 0;  // no interlace
  chunk(out, "IHDR", ihdr, sizeof ihdr);

  // Filter type 1 (Sub) on every row: cheap and compresses sprites well.
  const size_t stride = size_t(img.width) * 4;
  std::vector<uint8_t> raw((stride + 1) * img.height);
  for (int y = 0; y < img.height; ++y) {
    uint8_t* dst = &raw[y * (stride + 1)];
    const uint8_t* src = &img.rgba[y * stride];
    dst[0] = 1;
    for (size_t i = 0; i < stride; ++i) dst[1 + i] = uint8_t(src[i] - (i >= 4 ? src[i - 4] : 0));
  }

  uLongf zlen = compressBound((uLong)raw.size());
  std::vector<uint8_t> z(zlen);
  compress2(z.data(), &zlen, raw.data(), (uLong)raw.size(), 6);
  chunk(out, "IDAT", z.data(), zlen);
  chunk(out, "IEND", nullptr, 0);
  return out;
}

}  // namespace nfh
