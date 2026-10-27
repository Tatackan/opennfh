#include "core/game_data.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>

namespace nfh {

bool GameData::open(const fs::path& gameOrDataDir, std::string* err) {
  mounts_.clear();
  files_.clear();
  index_.clear();

  std::error_code ec;
  fs::path dir = gameOrDataDir;
  fs::path probe;
  if (!findFileNoCase(dir, "gfxdata.bnd", probe)) {
    fs::path sub;
    if (findFileNoCase(dir, "data", sub) && findFileNoCase(sub, "gfxdata.bnd", probe)) {
      dir = sub;
    } else {
      if (err) *err = "gfxdata.bnd not found in " + pathToUtf8(gameOrDataDir) + " (or its data/ folder)";
      return false;
    }
  }
  dataDir_ = dir;

  struct Spec {
    const char* file;
    const char* prefix;
    bool required;
  };
  const Spec specs[] = {
      {"gamedata.bnd", "gamedata", true},
      {"gfxdata.bnd", "gfx", true},
      {"sfxdatahigh.bnd", "sfx", false},
      {"sfxdata.bnd", "sfxlow", false},
  };
  for (const Spec& s : specs) {
    fs::path p;
    if (!findFileNoCase(dir, s.file, p)) {
      if (s.required) {
        if (err) *err = std::string(s.file) + " not found in " + pathToUtf8(dir);
        return false;
      }
      continue;
    }
    auto a = std::make_unique<BndArchive>();
    if (!a->open(p, err)) return false;
    mounts_.push_back({s.prefix, std::move(a)});
  }

  // If only the low-quality sound archive exists, expose it as "sfx/" too.
  bool haveSfx = std::any_of(mounts_.begin(), mounts_.end(), [](const Mounted& m) { return m.prefix == "sfx"; });

  for (size_t mi = 0; mi < mounts_.size(); ++mi) {
    const Mounted& m = mounts_[mi];
    for (const BndEntry& be : m.archive->entries()) {
      VfsEntry v;
      v.virtualPath = m.prefix + "/" + be.name;
      v.size = be.size;
      v.archive = (int)mi;
      v.entry = &be;
      index_[toLowerAscii(v.virtualPath)] = files_.size();
      files_.push_back(v);
      if (!haveSfx && m.prefix == "sfxlow") {
        v.virtualPath = "sfx/" + be.name;
        index_[toLowerAscii(v.virtualPath)] = files_.size();
        files_.push_back(v);
      }
    }
  }

  // Loose files (fonts, game.xml).
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    std::string ext = toLowerAscii(pathToUtf8(it->path().extension()));
    if (ext == ".bnd") continue;
    VfsEntry v;
    v.virtualPath = "loose/" + pathToUtf8(it->path().filename());
    v.size = (uint32_t)it->file_size(ec);
    v.loosePath = it->path();
    index_[toLowerAscii(v.virtualPath)] = files_.size();
    files_.push_back(v);
  }
  return true;
}

const VfsEntry* GameData::find(const std::string& virtualPath) const {
  std::string key = toLowerAscii(virtualPath);
  std::replace(key.begin(), key.end(), '\\', '/');
  auto it = index_.find(key);
  return it == index_.end() ? nullptr : &files_[it->second];
}

bool GameData::read(const VfsEntry& e, std::vector<uint8_t>& out, std::string* err) const {
  if (e.archive < 0) return readWholeFile(e.loosePath, out, err);
  return mounts_[e.archive].archive->read(*e.entry, out, err);
}

bool GameData::read(const std::string& virtualPath, std::vector<uint8_t>& out, std::string* err) const {
  const VfsEntry* e = find(virtualPath);
  if (!e) {
    if (err) *err = "not found: " + virtualPath;
    return false;
  }
  return read(*e, out, err);
}

std::vector<GameData::ArchiveInfo> GameData::archives() const {
  std::vector<ArchiveInfo> out;
  for (auto& m : mounts_) out.push_back({m.prefix, m.archive->path(), m.archive->entries().size()});
  return out;
}

static fs::path readCfg(const fs::path& cfg) {
  std::ifstream f(cfg, std::ios::binary);
  if (!f) return {};
  std::string line;
  while (std::getline(f, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
    if (line.rfind("game_path=", 0) == 0) return pathFromUtf8(line.substr(10));
  }
  return {};
}

fs::path GameData::locate(const std::string& explicitArg, const fs::path& exeDir) {
  if (!explicitArg.empty()) return pathFromUtf8(explicitArg);
  if (const char* env = std::getenv("NFH_DATA"); env && *env) return pathFromUtf8(env);
  for (const fs::path& d : {exeDir, fs::current_path()}) {
    fs::path p = readCfg(d / "opennfh.cfg");
    if (!p.empty()) return p;
  }
  return {};
}

void GameData::rememberPath(const fs::path& dir, const fs::path& gamePath) {
  std::string s = "game_path=" + pathToUtf8(gamePath) + "\n";
  writeWholeFile(dir / "opennfh.cfg", s.data(), s.size());
}

}  // namespace nfh
