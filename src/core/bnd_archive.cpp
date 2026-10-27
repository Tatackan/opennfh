#include "core/bnd_archive.h"

#include <zlib.h>

#include <algorithm>
#include <cstring>

namespace nfh {

namespace {

uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

constexpr uint32_t kSigLocal = 0x04034b50;
constexpr uint32_t kSigCentral = 0x02014b50;
constexpr uint32_t kSigEnd = 0x06054b50;

void setErr(std::string* err, std::string msg) {
  if (err) *err = std::move(msg);
}

}  // namespace

bool BndArchive::open(const fs::path& path, std::string* err) {
  path_ = path;
  entries_.clear();
  index_.clear();
  file_.close();
  file_.clear();
  file_.open(path, std::ios::binary);
  if (!file_) {
    setErr(err, "cannot open " + pathToUtf8(path));
    return false;
  }
  file_.seekg(0, std::ios::end);
  fileSize_ = (uint64_t)file_.tellg();
  if (fileSize_ < 22) {
    setErr(err, "file too small: " + pathToUtf8(path));
    return false;
  }

  // Locate the end-of-central-directory record (last 64 KiB + 22 bytes).
  const uint64_t tailLen = std::min<uint64_t>(fileSize_, 65535 + 22);
  std::vector<uint8_t> tail(tailLen);
  file_.seekg((std::streamoff)(fileSize_ - tailLen));
  file_.read(reinterpret_cast<char*>(tail.data()), (std::streamsize)tailLen);
  int64_t eocd = -1;
  for (int64_t i = (int64_t)tailLen - 22; i >= 0; --i) {
    if (rd32(&tail[i]) == kSigEnd) {
      eocd = i;
      break;
    }
  }
  if (eocd < 0) {
    setErr(err, "not a BND/ZIP archive (no end record): " + pathToUtf8(path));
    return false;
  }
  const uint8_t* e = &tail[eocd];
  const uint16_t count = rd16(e + 10);
  const uint32_t cdSize = rd32(e + 12);
  const uint32_t cdOffset = rd32(e + 16);
  if ((uint64_t)cdOffset + cdSize > fileSize_) {
    setErr(err, "corrupt central directory: " + pathToUtf8(path));
    return false;
  }

  std::vector<uint8_t> cd(cdSize);
  file_.seekg(cdOffset);
  file_.read(reinterpret_cast<char*>(cd.data()), cdSize);
  if (!file_) {
    setErr(err, "cannot read central directory: " + pathToUtf8(path));
    return false;
  }

  entries_.reserve(count);
  size_t pos = 0;
  for (uint16_t i = 0; i < count; ++i) {
    if (pos + 46 > cd.size() || rd32(&cd[pos]) != kSigCentral) {
      setErr(err, "bad central directory entry #" + std::to_string(i));
      return false;
    }
    const uint8_t* h = &cd[pos];
    BndEntry be;
    be.method = rd16(h + 10);
    be.dosTime = rd16(h + 12);
    be.dosDate = rd16(h + 14);
    be.crc32 = rd32(h + 16);
    be.compressedSize = rd32(h + 20);
    be.size = rd32(h + 24);
    const uint16_t nameLen = rd16(h + 28), extraLen = rd16(h + 30), commentLen = rd16(h + 32);
    be.localHeaderOffset = rd32(h + 42);
    if (pos + 46 + nameLen > cd.size()) {
      setErr(err, "truncated central directory");
      return false;
    }
    be.name.assign(reinterpret_cast<const char*>(h + 46), nameLen);
    std::replace(be.name.begin(), be.name.end(), '\\', '/');
    pos += 46 + nameLen + extraLen + commentLen;

    if (be.name.empty() || be.name.back() == '/') continue;  // directory record
    index_[toLowerAscii(be.name)] = entries_.size();
    entries_.push_back(std::move(be));
  }
  return true;
}

const BndEntry* BndArchive::find(const std::string& name) const {
  std::string key = toLowerAscii(name);
  std::replace(key.begin(), key.end(), '\\', '/');
  auto it = index_.find(key);
  return it == index_.end() ? nullptr : &entries_[it->second];
}

bool BndArchive::read(const BndEntry& e, std::vector<uint8_t>& out, std::string* err) const {
  uint8_t lh[30];
  file_.clear();
  file_.seekg(e.localHeaderOffset);
  file_.read(reinterpret_cast<char*>(lh), sizeof lh);
  if (!file_ || rd32(lh) != kSigLocal) {
    setErr(err, "bad local header for " + e.name);
    return false;
  }
  const uint64_t dataOffset = (uint64_t)e.localHeaderOffset + 30 + rd16(lh + 26) + rd16(lh + 28);
  if (dataOffset + e.compressedSize > fileSize_) {
    setErr(err, "entry data out of range: " + e.name);
    return false;
  }

  std::vector<uint8_t> comp(e.compressedSize);
  file_.seekg((std::streamoff)dataOffset);
  file_.read(reinterpret_cast<char*>(comp.data()), e.compressedSize);
  if (!file_) {
    setErr(err, "read error: " + e.name);
    return false;
  }

  if (e.method == 0) {
    out = std::move(comp);
  } else if (e.method == 8) {
    out.resize(e.size);
    z_stream zs{};
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
      setErr(err, "inflateInit failed");
      return false;
    }
    zs.next_in = comp.data();
    zs.avail_in = (uInt)comp.size();
    zs.next_out = out.data();
    zs.avail_out = (uInt)out.size();
    int rc = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    if (rc != Z_STREAM_END || zs.total_out != e.size) {
      setErr(err, "inflate failed for " + e.name + " (zlib rc=" + std::to_string(rc) + ")");
      return false;
    }
  } else {
    setErr(err, "unsupported compression method " + std::to_string(e.method) + " for " + e.name);
    return false;
  }

  const uint32_t crc = (uint32_t)crc32(0L, out.data(), (uInt)out.size());
  if (crc != e.crc32) {
    setErr(err, "CRC mismatch for " + e.name);
    return false;
  }
  return true;
}

}  // namespace nfh
