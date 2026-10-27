// nfh-extract: lists, unpacks and converts resources of the original
// "Neighbours from Hell" (Как достать соседа) game.
//
//   nfh-extract [--game PATH] info
//   nfh-extract [--game PATH] list [FILTER]
//   nfh-extract [--game PATH] extract OUTDIR [--filter TEXT] [--raw | --both]
//   nfh-extract [--game PATH] cat VIRTUAL_PATH
//
// By default `extract` converts TGA -> PNG (fixing the game's ARGB4444/RGB565
// colours) and XML -> UTF-8. --raw keeps the original bytes, --both writes both.
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/game_data.h"
#include "core/platform.h"
#include "core/png_writer.h"
#include "core/text.h"
#include "core/tga.h"
#include "core/xml.h"

using namespace nfh;

namespace {

void usage() {
  std::printf(
      "OpenNFH resource extractor\n\n"
      "usage: nfh-extract [--game PATH] <command> [args]\n\n"
      "commands:\n"
      "  info                          show archives and statistics\n"
      "  list [FILTER]                 list virtual paths (substring filter, case-insensitive)\n"
      "  extract OUTDIR [options]      unpack files\n"
      "        --filter TEXT           only paths containing TEXT\n"
      "        --raw                   keep original TGA/XML bytes (no conversion)\n"
      "        --both                  write converted and original files\n"
      "  cat VIRTUAL_PATH              print a text file as UTF-8 (e.g. gamedata/leveldata.xml)\n\n"
      "PATH may be the game folder or its data/ folder. It is remembered in\n"
      "opennfh.cfg next to the executable, or can be set via $NFH_DATA.\n");
}

std::string ext(const std::string& p) {
  size_t dot = p.find_last_of('.');
  return dot == std::string::npos ? std::string() : toLowerAscii(p.substr(dot));
}

bool contains(const std::string& hay, const std::string& needleLower) {
  return needleLower.empty() || toLowerAscii(hay).find(needleLower) != std::string::npos;
}

std::string humanSize(uint64_t b) {
  char buf[32];
  if (b >= 1024 * 1024) std::snprintf(buf, sizeof buf, "%.1f MiB", b / 1048576.0);
  else if (b >= 1024) std::snprintf(buf, sizeof buf, "%.1f KiB", b / 1024.0);
  else std::snprintf(buf, sizeof buf, "%llu B", (unsigned long long)b);
  return buf;
}

int cmdInfo(const GameData& gd) {
  std::printf("data dir: %s\n\n", pathToUtf8(gd.dataDir()).c_str());
  for (auto& a : gd.archives()) std::printf("  %-9s %5zu files  %s\n", (a.prefix + "/").c_str(), a.fileCount, pathToUtf8(a.path).c_str());
  std::map<std::string, std::pair<size_t, uint64_t>> byExt;
  for (auto& f : gd.files()) {
    auto& s = byExt[ext(f.virtualPath)];
    s.first++;
    s.second += f.size;
  }
  std::printf("\nby type:\n");
  for (auto& [e, s] : byExt) std::printf("  %-6s %6zu files  %s\n", e.empty() ? "(none)" : e.c_str(), s.first, humanSize(s.second).c_str());
  return 0;
}

int cmdList(const GameData& gd, const std::string& filter) {
  const std::string f = toLowerAscii(filter);
  size_t n = 0;
  for (auto& e : gd.files()) {
    if (!contains(e.virtualPath, f)) continue;
    std::printf("%10u  %s\n", e.size, e.virtualPath.c_str());
    ++n;
  }
  std::fprintf(stderr, "%zu file(s)\n", n);
  return 0;
}

int cmdCat(const GameData& gd, const std::string& vpath) {
  std::vector<uint8_t> data;
  std::string err;
  if (!gd.read(vpath, data, &err)) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 1;
  }
  std::string text = decodeToUtf8(data);
  std::fwrite(text.data(), 1, text.size(), stdout);
  return 0;
}

int cmdExtract(const GameData& gd, const fs::path& outDir, const std::string& filter, bool convert, bool keepRaw) {
  const std::string f = toLowerAscii(filter);
  size_t ok = 0, failed = 0, total = 0;
  for (auto& e : gd.files())
    if (contains(e.virtualPath, f)) ++total;

  std::vector<uint8_t> data;
  std::string err;
  for (auto& e : gd.files()) {
    if (!contains(e.virtualPath, f)) continue;
    if (!gd.read(e, data, &err)) {
      std::fprintf(stderr, "\nerror: %s\n", err.c_str());
      ++failed;
      continue;
    }
    const fs::path target = outDir / pathFromUtf8(e.virtualPath);
    const std::string x = ext(e.virtualPath);
    bool good = true;

    if (convert && x == ".tga") {
      Image img;
      if (decodeTga(data, img, &err)) {
        auto png = encodePng(img);
        fs::path p = target;
        p.replace_extension(".png");
        good = writeWholeFile(p, png.data(), png.size(), &err);
      } else {
        std::fprintf(stderr, "\nwarning: %s: %s (saved raw)\n", e.virtualPath.c_str(), err.c_str());
        good = writeWholeFile(target, data.data(), data.size(), &err);
      }
      if (keepRaw && good) good = writeWholeFile(target, data.data(), data.size(), &err);
    } else if (convert && x == ".xml") {
      std::string utf8 = decodeToUtf8(data);
      if (keepRaw) {
        fs::path p = target;
        p.replace_extension(".utf8.xml");
        good = writeWholeFile(p, utf8.data(), utf8.size(), &err) && writeWholeFile(target, data.data(), data.size(), &err);
      } else {
        good = writeWholeFile(target, utf8.data(), utf8.size(), &err);
      }
    } else {
      good = writeWholeFile(target, data.data(), data.size(), &err);
    }

    if (good) ++ok;
    else {
      std::fprintf(stderr, "\nerror: %s\n", err.c_str());
      ++failed;
    }
    if ((ok + failed) % 50 == 0 || ok + failed == total) {
      std::fprintf(stderr, "\r%zu / %zu", ok + failed, total);
      std::fflush(stderr);
    }
  }
  std::fprintf(stderr, "\nextracted %zu file(s), %zu error(s) -> %s\n", ok, failed, pathToUtf8(outDir).c_str());
  return failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  setupConsoleUtf8();
  std::vector<std::string> args = utf8Args(argc, argv);

  std::string gameArg, filter;
  bool raw = false, both = false;
  std::vector<std::string> pos;
  for (size_t i = 1; i < args.size(); ++i) {
    const std::string& a = args[i];
    if ((a == "--game" || a == "-g") && i + 1 < args.size()) gameArg = args[++i];
    else if (a == "--filter" && i + 1 < args.size()) filter = args[++i];
    else if (a == "--raw") raw = true;
    else if (a == "--both") both = true;
    else if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else pos.push_back(a);
  }
  if (pos.empty()) {
    usage();
    return 1;
  }

  const fs::path exeDir = executableDir();
  const fs::path gamePath = GameData::locate(gameArg, exeDir);
  if (gamePath.empty()) {
    std::fprintf(stderr, "error: game path unknown. Use --game PATH (or set NFH_DATA).\n");
    return 1;
  }
  GameData gd;
  std::string err;
  if (!gd.open(gamePath, &err)) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 1;
  }
  if (!gameArg.empty()) GameData::rememberPath(exeDir, gamePath);

  const std::string& cmd = pos[0];
  if (cmd == "info") return cmdInfo(gd);
  if (cmd == "list") return cmdList(gd, pos.size() > 1 ? pos[1] : filter);
  if (cmd == "cat" && pos.size() > 1) return cmdCat(gd, pos[1]);
  if (cmd == "extract" && pos.size() > 1) return cmdExtract(gd, pathFromUtf8(pos[1]), filter, !raw, raw || both);
  usage();
  return 1;
}
