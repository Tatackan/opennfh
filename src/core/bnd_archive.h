// Reader for the original game's .bnd archives.
//
// Format notes (verified on the Buka/JoWooD release of NFH1):
//   * .bnd is a standard PKZIP archive: local headers "PK\3\4", central
//     directory "PK\1\2", end record "PK\5\6".
//   * Methods used: 0 (stored: mp3/wav, some dirs) and 8 (deflate: xml/tga).
//   * No encryption, no ZIP64, paths use '/' and are ASCII.
#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/platform.h"

namespace nfh {

struct BndEntry {
  std::string name;        // original path inside archive, e.g. "anc/ark/ark_open.tga"
  uint16_t method = 0;     // 0 = stored, 8 = deflate
  uint32_t crc32 = 0;
  uint32_t compressedSize = 0;
  uint32_t size = 0;       // uncompressed size
  uint32_t localHeaderOffset = 0;
  uint16_t dosTime = 0, dosDate = 0;
};

class BndArchive {
 public:
  bool open(const fs::path& path, std::string* err = nullptr);
  bool isOpen() const { return file_.is_open(); }

  const fs::path& path() const { return path_; }
  const std::vector<BndEntry>& entries() const { return entries_; }

  // Case-insensitive lookup; returns nullptr if missing.
  const BndEntry* find(const std::string& name) const;

  // Reads and decompresses one entry, verifying CRC32.
  bool read(const BndEntry& e, std::vector<uint8_t>& out, std::string* err = nullptr) const;

 private:
  fs::path path_;
  mutable std::ifstream file_;
  uint64_t fileSize_ = 0;
  std::vector<BndEntry> entries_;
  std::unordered_map<std::string, size_t> index_;  // lowercase name -> entry
};

}  // namespace nfh
