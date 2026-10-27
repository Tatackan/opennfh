// Decoder for the game's TGA sprites.
//
// The original engine stores almost everything as uncompressed true-colour
// TGA (image type 2) with 16 bits per pixel, top-left origin, but uses the
// image-descriptor alpha bits in a non-standard way:
//   * descriptor 0x24 (4 alpha bits)  -> ARGB4444   (sprites, characters)
//   * descriptor 0x20 (0 alpha bits)  -> RGB565     (backgrounds, GUI)
//     (standard TGA would be X1R5G5B5 here - that is why other tools show
//      purple/green garbage on the house backgrounds)
// 24/32-bit TGAs (e.g. loading.tga) are decoded the standard way.
// RLE variants (type 10) are supported too for modded files.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nfh {

enum class TgaPixelFormat { ARGB4444, RGB565, RGB555, BGR24, BGRA32, Unknown };

struct Image {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba;  // width*height*4, row-major, top-down, straight alpha
  TgaPixelFormat sourceFormat = TgaPixelFormat::Unknown;
  bool hasAlpha() const { return sourceFormat == TgaPixelFormat::ARGB4444 || sourceFormat == TgaPixelFormat::BGRA32; }
};

const char* pixelFormatName(TgaPixelFormat f);

// Decodes a TGA file to straight-alpha RGBA8.
bool decodeTga(const uint8_t* data, size_t size, Image& out, std::string* err = nullptr);
inline bool decodeTga(const std::vector<uint8_t>& v, Image& out, std::string* err = nullptr) {
  return decodeTga(v.data(), v.size(), out, err);
}

}  // namespace nfh
