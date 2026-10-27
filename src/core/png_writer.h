// Minimal PNG encoder (RGBA8, zlib-compressed) used by nfh-extract --convert.
#pragma once

#include <cstdint>
#include <vector>

#include "core/tga.h"

namespace nfh {

std::vector<uint8_t> encodePng(const Image& img);

}  // namespace nfh
