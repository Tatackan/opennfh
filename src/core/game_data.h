// Virtual file system over the original game installation.
//
// Mounts the four .bnd archives from <game>/data and exposes them under
// prefixes so that every resource has a unique virtual path:
//
//   gamedata/<path>   from gamedata.bnd     (XML: levels, dialogs, strings)
//   gfx/<path>        from gfxdata.bnd      (TGA sprites)
//   sfx/<path>        from sfxdatahigh.bnd  (falls back to sfxdata.bnd)
//   sfxlow/<path>     from sfxdata.bnd      (low quality audio)
//   loose/<file>      loose files in data/  (TTF fonts, game.xml)
//
// Lookups are case-insensitive (important on Linux).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/bnd_archive.h"
#include "core/platform.h"

namespace nfh {

struct VfsEntry {
  std::string virtualPath;  // e.g. "gfx/anc/ark/ark_open.tga"
  uint32_t size = 0;
  int archive = -1;              // index into GameData archives, -1 for loose file
  const BndEntry* entry = nullptr;
  fs::path loosePath;
};

class GameData {
 public:
  // Accepts either the game root or its data/ folder.
  bool open(const fs::path& gameOrDataDir, std::string* err = nullptr);

  const fs::path& dataDir() const { return dataDir_; }
  const std::vector<VfsEntry>& files() const { return files_; }

  const VfsEntry* find(const std::string& virtualPath) const;
  bool read(const VfsEntry& e, std::vector<uint8_t>& out, std::string* err = nullptr) const;
  bool read(const std::string& virtualPath, std::vector<uint8_t>& out, std::string* err = nullptr) const;

  // Archive summary for diagnostics.
  struct ArchiveInfo {
    std::string prefix;
    fs::path path;
    size_t fileCount = 0;
  };
  std::vector<ArchiveInfo> archives() const;

  // Tries (in order): explicit argument, $NFH_DATA, opennfh.cfg next to the
  // executable / in the current directory. Returns empty path if none found.
  static fs::path locate(const std::string& explicitArg, const fs::path& exeDir);
  // Persists the chosen path into opennfh.cfg in `dir`.
  static void rememberPath(const fs::path& dir, const fs::path& gamePath);

 private:
  struct Mounted {
    std::string prefix;
    std::unique_ptr<BndArchive> archive;
  };
  fs::path dataDir_;
  std::vector<Mounted> mounts_;
  std::vector<VfsEntry> files_;
  std::unordered_map<std::string, size_t> index_;  // lowercase vpath -> files_ index
};

}  // namespace nfh
