#include "viewer/viewer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <unordered_map>

#include "core/png_writer.h"
#include "core/text.h"
#include "core/xml.h"

namespace nfh {

namespace {

constexpr int kListWidth = 400;
constexpr int kFilterHeight = 34;
constexpr int kStatusHeight = 26;
constexpr int kHeaderHeight = 52;

const SDL_Color kBg{30, 32, 38, 255};
const SDL_Color kPanel{40, 43, 51, 255};
const SDL_Color kPanelLight{52, 56, 66, 255};
const SDL_Color kAccent{232, 160, 64, 255};
const SDL_Color kSelect{72, 92, 140, 255};
const SDL_Color kText{224, 226, 232, 255};
const SDL_Color kDim{140, 146, 160, 255};
const SDL_Color kError{240, 110, 100, 255};
const SDL_Color kGood{130, 210, 140, 255};

std::string extOf(const std::string& p) {
  size_t dot = p.find_last_of('.');
  return dot == std::string::npos ? std::string() : toLowerAscii(p.substr(dot));
}

std::string humanSize(uint64_t b) {
  char buf[32];
  if (b >= 1024 * 1024) std::snprintf(buf, sizeof buf, "%.1f MiB", b / 1048576.0);
  else if (b >= 1024) std::snprintf(buf, sizeof buf, "%.1f KiB", b / 1024.0);
  else std::snprintf(buf, sizeof buf, "%llu B", (unsigned long long)b);
  return buf;
}

std::vector<fs::path> uiFontCandidates(bool mono) {
  std::vector<fs::path> c;
#ifdef _WIN32
  const char* win = std::getenv("WINDIR");
  fs::path fonts = fs::path(win ? win : "C:\\Windows") / "Fonts";
  if (mono) {
    c = {fonts / "consola.ttf", fonts / "cour.ttf", fonts / "lucon.ttf"};
  } else {
    c = {fonts / "segoeui.ttf", fonts / "arial.ttf", fonts / "tahoma.ttf"};
  }
#else
  if (mono) {
    c = {"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
         "/usr/share/fonts/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf",
         "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf", "/usr/share/fonts/liberation-mono/LiberationMono-Regular.ttf",
         "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf"};
  } else {
    c = {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
         "/usr/share/fonts/dejavu/DejaVuSans.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
         "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf",
         "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf"};
  }
#endif
  return c;
}

// Splits "dir/name_0012.tga" into ("dir/name_", 12). The game names animation
// frames with an underscore and exactly four digits.
bool splitFrameName(const std::string& vpath, std::string& prefix, int& number) {
  if (extOf(vpath) != ".tga") return false;
  size_t dot = vpath.find_last_of('.');
  size_t i = dot;
  while (i > 0 && std::isdigit((unsigned char)vpath[i - 1])) --i;
  if (dot - i != 4 || i == 0 || vpath[i - 1] != '_') return false;
  prefix = toLowerAscii(vpath.substr(0, i));
  number = std::atoi(vpath.substr(i, dot - i).c_str());
  return true;
}

// Offsets of sprites as declared in gamedata/*/gfxdata.xml, keyed by lowercase gfx vpath.
std::unordered_map<std::string, SDL_Point> g_offsets;

void loadOffsets(const GameData& gd) {
  g_offsets.clear();
  std::vector<uint8_t> data;
  for (const VfsEntry& e : gd.files()) {
    const std::string lower = toLowerAscii(e.virtualPath);
    if (lower.rfind("gamedata/", 0) != 0 || lower.size() < 12 || lower.compare(lower.size() - 12, 12, "/gfxdata.xml") != 0) continue;
    if (!gd.read(e, data)) continue;
    XmlDocument doc = parseXml(decodeToUtf8(data));
    for (const XmlNode* obj : doc.root.childrenNamed("object")) {
      const std::string objName = obj->attrOr("name");
      for (const XmlNode* gfx : obj->childrenNamed("gfxdata")) {
        for (const XmlNode* f : gfx->childrenNamed("file")) {
          int x = 0, y = 0;
          if (!f->attrPair("offset", x, y)) continue;
          g_offsets[toLowerAscii("gfx/" + objName + "/" + f->attrOr("image"))] = SDL_Point{x, y};
        }
      }
    }
  }
}

}  // namespace

Viewer::Viewer() = default;

Viewer::~Viewer() {
  unloadPreview();
  ui_.clearCache();
  mono_.clearCache();
  big_.clearCache();
  sample_.clearCache();
  if (audioOk_) Mix_CloseAudio();
  Mix_Quit();
  if (ren_) SDL_DestroyRenderer(ren_);
  if (window_) SDL_DestroyWindow(window_);
  TTF_Quit();
  SDL_Quit();
}

bool Viewer::init(const ViewerOptions& opts) {
  opts_ = opts;
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return false;
  }
  if (TTF_Init() != 0) {
    std::fprintf(stderr, "TTF_Init: %s\n", TTF_GetError());
    return false;
  }
  Mix_Init(MIX_INIT_MP3);
  audioOk_ = Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) == 0;
  if (!audioOk_) std::fprintf(stderr, "audio disabled: %s\n", Mix_GetError());

  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
  window_ = SDL_CreateWindow("OpenNFH viewer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, winW_, winH_,
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if (!window_) {
    std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
    return false;
  }
  ren_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!ren_) ren_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
  if (!ren_) {
    std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
    return false;
  }
  SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
  SDL_GetRendererOutputSize(ren_, &winW_, &winH_);

  if (!ui_.load(ren_, uiFontCandidates(false), 15) || !mono_.load(ren_, uiFontCandidates(true), 14) ||
      !big_.load(ren_, uiFontCandidates(false), 20)) {
    std::fprintf(stderr, "no usable UI font found (install DejaVu fonts: fonts-dejavu-core)\n");
    return false;
  }

  exeDir_ = executableDir();
  fs::path p = GameData::locate(opts.game, exeDir_);
  if (p.empty()) {
    message_ = "Путь к игре не задан. Перетащите папку игры в это окно или запустите: nfh-viewer --game ПУТЬ";
  } else if (openGame(p) && !opts.game.empty()) {
    GameData::rememberPath(exeDir_, p);
  }
  if (game_ && !opts.filter.empty()) {
    filter_ = opts.filter;
    rebuildFilter();
    selected_ = -1;
    select(0);
  }
  if (opts.play && kind_ == Kind::Image && !sequence_.empty()) {
    playing_ = true;
    lastFrameTick_ = SDL_GetTicks();
  }
  return true;
}

void Viewer::saveScreenshot(const std::string& file) {
  Image shot;
  shot.width = winW_;
  shot.height = winH_;
  shot.rgba.resize(size_t(winW_) * winH_ * 4);
  if (SDL_RenderReadPixels(ren_, nullptr, SDL_PIXELFORMAT_RGBA32, shot.rgba.data(), winW_ * 4) != 0) {
    std::fprintf(stderr, "screenshot failed: %s\n", SDL_GetError());
    return;
  }
  auto png = encodePng(shot);
  std::string err;
  if (!writeWholeFile(pathFromUtf8(file), png.data(), png.size(), &err)) std::fprintf(stderr, "%s\n", err.c_str());
}

bool Viewer::openGame(const fs::path& p) {
  auto gd = std::make_unique<GameData>();
  std::string err;
  if (!gd->open(p, &err)) {
    message_ = "Ошибка: " + err;
    return false;
  }
  unloadPreview();
  game_ = std::move(gd);
  message_.clear();
  loadOffsets(*game_);
  filter_.clear();
  rebuildFilter();
  select(0);
  SDL_SetWindowTitle(window_, ("OpenNFH viewer - " + pathToUtf8(game_->dataDir())).c_str());
  return true;
}

// ---------------------------------------------------------------------------
// File list
// ---------------------------------------------------------------------------

void Viewer::rebuildFilter() {
  filtered_.clear();
  if (!game_) return;
  std::vector<std::string> terms;
  std::string lower = toLowerAscii(filter_);
  size_t i = 0;
  while (i < lower.size()) {
    size_t j = lower.find(' ', i);
    if (j == std::string::npos) j = lower.size();
    if (j > i) terms.push_back(lower.substr(i, j - i));
    i = j + 1;
  }
  const auto& files = game_->files();
  for (size_t k = 0; k < files.size(); ++k) {
    const std::string name = toLowerAscii(files[k].virtualPath);
    bool ok = true;
    for (auto& t : terms)
      if (name.find(t) == std::string::npos) {
        ok = false;
        break;
      }
    if (ok) filtered_.push_back((int)k);
  }
  listScroll_ = 0;
}

int Viewer::listRowHeight() const { return const_cast<TextRenderer&>(ui_).lineHeight() + 4; }

SDL_Rect Viewer::listArea() const { return SDL_Rect{0, kFilterHeight, kListWidth, winH_ - kFilterHeight - kStatusHeight}; }

SDL_Rect Viewer::previewArea() const { return SDL_Rect{kListWidth, 0, winW_ - kListWidth, winH_ - kStatusHeight}; }

void Viewer::select(int idx) {
  if (filtered_.empty()) {
    selected_ = -1;
    unloadPreview();
    return;
  }
  idx = std::clamp(idx, 0, (int)filtered_.size() - 1);
  if (idx == selected_ && !previewPath_.empty() && previewPath_ == game_->files()[filtered_[idx]].virtualPath) return;
  selected_ = idx;
  ensureVisible();
  loadPreview();
}

void Viewer::moveSelection(int delta) { select(selected_ < 0 ? 0 : selected_ + delta); }

void Viewer::ensureVisible() {
  const int rows = std::max(1, listArea().h / listRowHeight());
  if (selected_ < listScroll_) listScroll_ = selected_;
  if (selected_ >= listScroll_ + rows) listScroll_ = selected_ - rows + 1;
  listScroll_ = std::clamp(listScroll_, 0, std::max(0, (int)filtered_.size() - rows));
}

// ---------------------------------------------------------------------------
// Preview loading
// ---------------------------------------------------------------------------

void Viewer::unloadPreview() {
  stopAudio();
  if (chunk_) Mix_FreeChunk(chunk_);
  if (music_) Mix_FreeMusic(music_);
  chunk_ = nullptr;
  music_ = nullptr;
  audioData_.clear();
  if (tex_) SDL_DestroyTexture(tex_);
  tex_ = nullptr;
  image_ = Image{};
  lines_.clear();
  sequence_.clear();
  playing_ = false;
  sample_.clearCache();
  kind_ = Kind::None;
  previewPath_.clear();
  previewInfo_.clear();
  previewError_.clear();
}

bool Viewer::loadImageTexture(const std::string& vpath, std::string* err) {
  std::vector<uint8_t> data;
  if (!game_->read(vpath, data, err)) return false;
  Image img;
  if (!decodeTga(data, img, err)) return false;
  SDL_Texture* t = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, img.width, img.height);
  if (!t) {
    if (err) *err = SDL_GetError();
    return false;
  }
  SDL_UpdateTexture(t, nullptr, img.rgba.data(), img.width * 4);
  SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
  if (tex_) SDL_DestroyTexture(tex_);
  tex_ = t;
  image_ = std::move(img);
  return true;
}

void Viewer::buildSequence(const std::string& vpath) {
  sequence_.clear();
  seqIndex_ = 0;
  std::string prefix;
  int num = 0;
  if (!splitFrameName(vpath, prefix, num)) return;
  std::vector<std::pair<int, std::string>> frames;
  for (const VfsEntry& e : game_->files()) {
    std::string p;
    int n = 0;
    if (splitFrameName(e.virtualPath, p, n) && p == prefix) frames.emplace_back(n, e.virtualPath);
  }
  if (frames.size() < 2) return;
  std::sort(frames.begin(), frames.end());
  for (size_t i = 0; i < frames.size(); ++i) {
    if (frames[i].second == vpath) seqIndex_ = (int)i;
    sequence_.push_back(frames[i].second);
  }

  // Bounding box of the whole animation in gfxdata.xml offset space, so that
  // frames are drawn at their real relative positions (no jitter).
  seqOffsets_.clear();
  seqHasOffsets_ = true;
  int minX = INT32_MAX, minY = INT32_MAX, maxX = INT32_MIN, maxY = INT32_MIN;
  std::vector<uint8_t> data;
  for (const std::string& f : sequence_) {
    auto it = g_offsets.find(toLowerAscii(f));
    if (it == g_offsets.end() || !game_->read(f, data) || data.size() < 18) {
      seqHasOffsets_ = false;
      break;
    }
    const int w = data[12] | (data[13] << 8), h = data[14] | (data[15] << 8);
    seqOffsets_.push_back(it->second);
    minX = std::min(minX, it->second.x);
    minY = std::min(minY, it->second.y);
    maxX = std::max(maxX, it->second.x + w);
    maxY = std::max(maxY, it->second.y + h);
  }
  if (seqHasOffsets_) seqBox_ = SDL_Rect{minX, minY, maxX - minX, maxY - minY};
}

void Viewer::showFrame(int idx) {
  if (sequence_.empty()) return;
  seqIndex_ = (idx % (int)sequence_.size() + (int)sequence_.size()) % (int)sequence_.size();
  std::string err;
  if (!loadImageTexture(sequence_[seqIndex_], &err)) previewError_ = err;
}

void Viewer::loadPreview() {
  unloadPreview();
  if (!game_ || selected_ < 0) return;
  const VfsEntry& e = game_->files()[filtered_[selected_]];
  previewPath_ = e.virtualPath;
  const std::string x = extOf(e.virtualPath);
  std::string err;
  zoom_ = 1.0f;
  panX_ = panY_ = 0;
  fitMode_ = true;

  if (x == ".tga") {
    kind_ = Kind::Image;
    if (!loadImageTexture(e.virtualPath, &err)) {
      previewError_ = err;
      return;
    }
    buildSequence(e.virtualPath);
  } else if (x == ".wav" || x == ".mp3") {
    kind_ = Kind::Audio;
    if (!game_->read(e, audioData_, &err)) {
      previewError_ = err;
      return;
    }
    if (!audioOk_) {
      previewError_ = "звук недоступен (аудиоустройство не открылось)";
      return;
    }
    playAudio();
  } else if (x == ".xml" || x == ".txt" || x == ".fot" || x == ".ini") {
    kind_ = Kind::Text;
    std::vector<uint8_t> data;
    if (!game_->read(e, data, &err)) {
      previewError_ = err;
      return;
    }
    TextEncoding enc;
    std::string text = decodeToUtf8(data, &enc);
    if (x == ".fot") {  // binary font resource: hex dump would be nicer, keep it simple
      text = "(бинарный файл шрифта Windows .fot, " + std::to_string(data.size()) + " байт)";
    }
    std::string line;
    for (size_t i = 0; i <= text.size(); ++i) {
      char c = i < text.size() ? text[i] : '\n';
      if (c == '\r') continue;
      if (c == '\n') {
        lines_.push_back(line);
        line.clear();
      } else if (c == '\t') {
        line += "    ";
      } else {
        line += c;
      }
    }
    textScroll_ = 0;
    if (x == ".xml") {
      XmlDocument doc = parseXml(text);
      previewInfo_ = std::string("кодировка: ") + encodingName(enc) + "   строк: " + std::to_string(lines_.size()) +
                     "   корневых элементов: " + std::to_string(doc.root.children.size());
      if (!doc.warnings.empty()) previewInfo_ += "   предупреждений XML: " + std::to_string(doc.warnings.size());
    } else {
      previewInfo_ = std::string("кодировка: ") + encodingName(enc);
    }
  } else if (x == ".ttf") {
    kind_ = Kind::Font;
    if (!game_->read(e, fontData_, &err)) {
      previewError_ = err;
      return;
    }
    if (!sample_.loadFromMemory(ren_, fontData_, 30)) previewError_ = TTF_GetError();
  } else {
    kind_ = Kind::Other;
  }
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

void Viewer::playAudio() {
  if (!audioOk_ || audioData_.empty()) return;
  stopAudio();
  const bool isMp3 = extOf(previewPath_) == ".mp3";
  if (isMp3) {
    if (!music_) music_ = Mix_LoadMUS_RW(SDL_RWFromConstMem(audioData_.data(), (int)audioData_.size()), 1);
    if (!music_ || Mix_PlayMusic(music_, 0) != 0) previewError_ = Mix_GetError();
  } else {
    if (!chunk_) chunk_ = Mix_LoadWAV_RW(SDL_RWFromConstMem(audioData_.data(), (int)audioData_.size()), 1);
    if (!chunk_ || Mix_PlayChannel(0, chunk_, 0) < 0) previewError_ = Mix_GetError();
  }
  audioStartTick_ = SDL_GetTicks();
}

void Viewer::stopAudio() {
  if (!audioOk_) return;
  Mix_HaltChannel(-1);
  Mix_HaltMusic();
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

int Viewer::run() {
  SDL_StopTextInput();
  const uint32_t startTick = SDL_GetTicks();
  while (running_) {
    SDL_Event e;
    // Block when idle to save CPU, wake up regularly for animation/audio.
    if (SDL_WaitEventTimeout(&e, playing_ ? 5 : 100)) {
      handleEvent(e);
      while (SDL_PollEvent(&e)) handleEvent(e);
    }
    if (playing_ && !sequence_.empty()) {
      uint32_t now = SDL_GetTicks();
      if (now - lastFrameTick_ >= (uint32_t)(1000 / std::max(1, fps_))) {
        lastFrameTick_ = now;
        showFrame(seqIndex_ + 1);
      }
    }
    if (!opts_.screenshot.empty() && SDL_GetTicks() - startTick > 700) {
      fillRect(SDL_Rect{0, 0, winW_, winH_}, kBg);
      drawList(listArea());
      drawPreview(previewArea());
      drawStatus(SDL_Rect{0, winH_ - kStatusHeight, winW_, kStatusHeight});
      saveScreenshot(opts_.screenshot);
      running_ = false;
      break;
    }
    draw();
  }
  return 0;
}

void Viewer::handleEvent(const SDL_Event& e) {
  switch (e.type) {
    case SDL_QUIT:
      running_ = false;
      break;
    case SDL_WINDOWEVENT:
      if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
        SDL_GetRendererOutputSize(ren_, &winW_, &winH_);
        ensureVisible();
      }
      break;
    case SDL_DROPFILE: {
      std::string path = e.drop.file;
      SDL_free(e.drop.file);
      fs::path p = pathFromUtf8(path);
      std::error_code ec;
      if (!fs::is_directory(p, ec)) p = p.parent_path();
      if (openGame(p)) GameData::rememberPath(exeDir_, p);
      break;
    }
    case SDL_TEXTINPUT:
      if (filterFocus_) {
        filter_ += e.text.text;
        rebuildFilter();
        selected_ = -1;
        select(0);
      }
      break;
    case SDL_KEYDOWN:
      handleKey(e.key);
      break;
    case SDL_MOUSEWHEEL: {
      int mx, my;
      SDL_GetMouseState(&mx, &my);
      // Mouse coordinates are in window units; convert for HiDPI.
      int ww, wh;
      SDL_GetWindowSize(window_, &ww, &wh);
      mx = mx * winW_ / std::max(1, ww);
      my = my * winH_ / std::max(1, wh);
      if (mx < kListWidth) {
        const int rows = std::max(1, listArea().h / listRowHeight());
        listScroll_ = std::clamp(listScroll_ - e.wheel.y * 3, 0, std::max(0, (int)filtered_.size() - rows));
      } else if (kind_ == Kind::Text) {
        textScroll_ = std::clamp(textScroll_ - e.wheel.y * 3, 0, std::max(0, (int)lines_.size() - 1));
      } else if (kind_ == Kind::Image && tex_) {
        SDL_Rect a = previewArea();
        float cx = mx - (a.x + a.w / 2.0f), cy = my - (a.y + kHeaderHeight + (a.h - kHeaderHeight) / 2.0f);
        float old = zoom_;
        fitMode_ = false;
        zoom_ = std::clamp(zoom_ * (e.wheel.y > 0 ? 1.25f : 0.8f), 0.05f, 32.0f);
        // Zoom around the cursor.
        panX_ = cx - (cx - panX_) * (zoom_ / old);
        panY_ = cy - (cy - panY_) * (zoom_ / old);
      }
      break;
    }
    case SDL_MOUSEBUTTONDOWN: {
      int ww, wh;
      SDL_GetWindowSize(window_, &ww, &wh);
      int mx = e.button.x * winW_ / std::max(1, ww), my = e.button.y * winH_ / std::max(1, wh);
      if (mx < kListWidth && my < kFilterHeight) {
        filterFocus_ = true;
        SDL_StartTextInput();
      } else if (mx < kListWidth) {
        filterFocus_ = false;
        SDL_StopTextInput();
        SDL_Rect la = listArea();
        int row = (my - la.y) / listRowHeight() + listScroll_;
        if (row >= 0 && row < (int)filtered_.size()) select(row);
      } else if (e.button.button == SDL_BUTTON_LEFT) {
        if (kind_ == Kind::Image) dragging_ = true;
        if (kind_ == Kind::Audio) playAudio();
      }
      break;
    }
    case SDL_MOUSEBUTTONUP:
      dragging_ = false;
      break;
    case SDL_MOUSEMOTION:
      if (dragging_) {
        int ww, wh;
        SDL_GetWindowSize(window_, &ww, &wh);
        panX_ += e.motion.xrel * (float)winW_ / std::max(1, ww);
        panY_ += e.motion.yrel * (float)winH_ / std::max(1, wh);
        fitMode_ = false;
      }
      break;
    default:
      break;
  }
}

void Viewer::handleKey(const SDL_KeyboardEvent& k) {
  const SDL_Keycode key = k.keysym.sym;
  const bool ctrl = (k.keysym.mod & KMOD_CTRL) != 0;
  const int rows = std::max(1, listArea().h / listRowHeight());

  if (ctrl && key == SDLK_q) {
    running_ = false;
    return;
  }
  if (ctrl && key == SDLK_f) {
    filterFocus_ = true;
    SDL_StartTextInput();
    return;
  }

  if (filterFocus_) {
    switch (key) {
      case SDLK_BACKSPACE:
        if (!filter_.empty()) {
          // Remove one UTF-8 code point.
          size_t n = filter_.size() - 1;
          while (n > 0 && (uint8_t(filter_[n]) & 0xC0) == 0x80) --n;
          filter_.erase(n);
          rebuildFilter();
          selected_ = -1;
          select(0);
        }
        return;
      case SDLK_ESCAPE:
        if (filter_.empty()) {
          filterFocus_ = false;
          SDL_StopTextInput();
        } else {
          filter_.clear();
          rebuildFilter();
          selected_ = -1;
          select(0);
        }
        return;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
      case SDLK_TAB:
        filterFocus_ = false;
        SDL_StopTextInput();
        return;
      case SDLK_UP: moveSelection(-1); return;
      case SDLK_DOWN: moveSelection(1); return;
      default: return;  // letters arrive via SDL_TEXTINPUT
    }
  }

  switch (key) {
    case SDLK_ESCAPE:
      if (!filter_.empty()) {
        filter_.clear();
        rebuildFilter();
        selected_ = -1;
        select(0);
      }
      break;
    case SDLK_SLASH:
      filterFocus_ = true;
      SDL_StartTextInput();
      break;
    case SDLK_UP: moveSelection(-1); break;
    case SDLK_DOWN: moveSelection(1); break;
    case SDLK_PAGEUP:
      if (kind_ == Kind::Text && (k.keysym.mod & KMOD_SHIFT)) textScroll_ = std::max(0, textScroll_ - 30);
      else moveSelection(-rows);
      break;
    case SDLK_PAGEDOWN:
      if (kind_ == Kind::Text && (k.keysym.mod & KMOD_SHIFT)) textScroll_ = std::min((int)lines_.size() - 1, textScroll_ + 30);
      else moveSelection(rows);
      break;
    case SDLK_HOME: select(0); break;
    case SDLK_END: select((int)filtered_.size() - 1); break;
    case SDLK_SPACE:
      if (kind_ == Kind::Image && !sequence_.empty()) {
        playing_ = !playing_;
        lastFrameTick_ = SDL_GetTicks();
      } else if (kind_ == Kind::Audio) {
        playAudio();
      }
      break;
    case SDLK_s: stopAudio(); break;
    case SDLK_LEFT:
      if (!sequence_.empty()) { playing_ = false; showFrame(seqIndex_ - 1); }
      break;
    case SDLK_RIGHT:
      if (!sequence_.empty()) { playing_ = false; showFrame(seqIndex_ + 1); }
      break;
    case SDLK_LEFTBRACKET: fps_ = std::max(1, fps_ - 1); break;
    case SDLK_RIGHTBRACKET: fps_ = std::min(60, fps_ + 1); break;
    case SDLK_PLUS:
    case SDLK_EQUALS:
    case SDLK_KP_PLUS: fitMode_ = false; zoom_ = std::min(32.0f, zoom_ * 1.25f); break;
    case SDLK_MINUS:
    case SDLK_KP_MINUS: fitMode_ = false; zoom_ = std::max(0.05f, zoom_ * 0.8f); break;
    case SDLK_1: fitMode_ = false; zoom_ = 1.0f; panX_ = panY_ = 0; break;
    case SDLK_2: fitMode_ = false; zoom_ = 2.0f; panX_ = panY_ = 0; break;
    case SDLK_f: fitMode_ = true; panX_ = panY_ = 0; break;
    case SDLK_b: checker_ = !checker_; break;
    default: break;
  }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void Viewer::fillRect(const SDL_Rect& r, SDL_Color c) {
  SDL_SetRenderDrawColor(ren_, c.r, c.g, c.b, c.a);
  SDL_RenderFillRect(ren_, &r);
}

void Viewer::drawCheckerboard(const SDL_Rect& r) {
  const int s = 12;
  SDL_RenderSetClipRect(ren_, &r);
  for (int y = r.y; y < r.y + r.h; y += s)
    for (int x = r.x; x < r.x + r.w; x += s) {
      bool odd = (((x - r.x) / s) + ((y - r.y) / s)) & 1;
      fillRect(SDL_Rect{x, y, s, s}, odd ? SDL_Color{150, 150, 150, 255} : SDL_Color{110, 110, 110, 255});
    }
  SDL_RenderSetClipRect(ren_, nullptr);
}

void Viewer::draw() {
  fillRect(SDL_Rect{0, 0, winW_, winH_}, kBg);
  drawList(listArea());
  drawPreview(previewArea());
  drawStatus(SDL_Rect{0, winH_ - kStatusHeight, winW_, kStatusHeight});
  SDL_RenderPresent(ren_);
}

void Viewer::drawList(const SDL_Rect& a) {
  // Filter box
  SDL_Rect fb{0, 0, kListWidth, kFilterHeight};
  fillRect(fb, kPanelLight);
  SDL_Rect inner{6, 5, kListWidth - 12, kFilterHeight - 10};
  fillRect(inner, filterFocus_ ? SDL_Color{24, 26, 30, 255} : SDL_Color{34, 36, 42, 255});
  if (filterFocus_) {
    SDL_SetRenderDrawColor(ren_, kAccent.r, kAccent.g, kAccent.b, 255);
    SDL_RenderDrawRect(ren_, &inner);
  }
  const int ty = inner.y + (inner.h - ui_.lineHeight()) / 2;
  if (filter_.empty() && !filterFocus_) {
    ui_.draw("Фильтр  (Ctrl+F или /)", inner.x + 8, ty, kDim);
  } else {
    int w = ui_.draw(filter_, inner.x + 8, ty, kText, inner.w - 16);
    if (filterFocus_ && (SDL_GetTicks() / 500) % 2 == 0) fillRect(SDL_Rect{inner.x + 9 + w, ty + 2, 2, ui_.lineHeight() - 4}, kAccent);
  }

  // Rows
  fillRect(a, kPanel);
  SDL_RenderSetClipRect(ren_, &a);
  const int rh = listRowHeight();
  const int rows = a.h / rh + 1;
  for (int i = 0; i < rows; ++i) {
    int idx = listScroll_ + i;
    if (idx >= (int)filtered_.size()) break;
    const VfsEntry& e = game_->files()[filtered_[idx]];
    SDL_Rect row{a.x, a.y + i * rh, a.w, rh};
    if (idx == selected_) fillRect(row, kSelect);
    const std::string x = extOf(e.virtualPath);
    SDL_Color c = x == ".tga" ? SDL_Color{150, 200, 255, 255}
                : (x == ".wav" || x == ".mp3") ? SDL_Color{150, 230, 160, 255}
                : x == ".xml" ? SDL_Color{240, 210, 140, 255}
                : kText;
    // Show the tail of long paths.
    std::string label = e.virtualPath;
    const int maxW = a.w - 16;
    if (ui_.width(label) > maxW) {
      while (label.size() > 4 && ui_.width("…" + label) > maxW) {
        size_t n = 1;
        while (n < label.size() && (uint8_t(label[n]) & 0xC0) == 0x80) ++n;
        label.erase(0, n);
      }
      label = "…" + label;
    }
    ui_.draw(label, row.x + 8, row.y + 2, c);
  }
  SDL_RenderSetClipRect(ren_, nullptr);

  // Scrollbar
  if (!filtered_.empty()) {
    const int visible = a.h / rh;
    if ((int)filtered_.size() > visible) {
      int barH = std::max(20, a.h * visible / (int)filtered_.size());
      int barY = a.y + (a.h - barH) * listScroll_ / std::max(1, (int)filtered_.size() - visible);
      fillRect(SDL_Rect{a.x + a.w - 5, barY, 4, barH}, SDL_Color{110, 116, 130, 255});
    }
  }
}

void Viewer::drawPreview(const SDL_Rect& a) {
  if (!game_) {
    big_.draw("OpenNFH viewer", a.x + 30, a.y + 30, kAccent);
    ui_.draw(message_, a.x + 30, a.y + 80, message_.rfind("Ошибка", 0) == 0 ? kError : kText, a.w - 60);
    return;
  }

  // Header
  fillRect(SDL_Rect{a.x, a.y, a.w, kHeaderHeight}, kPanelLight);
  big_.draw(previewPath_.empty() ? "(нет выбранного файла)" : previewPath_, a.x + 14, a.y + 4, kText, a.w - 28);
  std::string info = previewInfo_;
  if (kind_ == Kind::Image && tex_) {
    char buf[200];
    std::snprintf(buf, sizeof buf, "%d×%d  %s  масштаб %.0f%%", image_.width, image_.height, pixelFormatName(image_.sourceFormat),
                  zoom_ * 100);
    info = buf;
    auto off = g_offsets.find(toLowerAscii(sequence_.empty() ? previewPath_ : sequence_[seqIndex_]));
    if (off != g_offsets.end()) info += "  offset " + std::to_string(off->second.x) + "/" + std::to_string(off->second.y);
    if (!sequence_.empty()) {
      std::snprintf(buf, sizeof buf, "   кадр %d/%d  %d fps  %s", seqIndex_ + 1, (int)sequence_.size(), fps_,
                    playing_ ? "воспроизведение (Space - пауза)" : "пауза (Space - играть)");
      info += buf;
    }
  }
  ui_.draw(info, a.x + 14, a.y + kHeaderHeight - ui_.lineHeight() - 4, kDim, a.w - 28);

  SDL_Rect body{a.x, a.y + kHeaderHeight, a.w, a.h - kHeaderHeight};
  if (!previewError_.empty()) {
    ui_.draw("Ошибка: " + previewError_, body.x + 20, body.y + 20, kError, body.w - 40);
    if (kind_ != Kind::Image || !tex_) return;
  }

  switch (kind_) {
    case Kind::Image: drawImage(body); break;
    case Kind::Text: drawText(body); break;
    case Kind::Audio: {
      const bool isMp3 = extOf(previewPath_) == ".mp3";
      bool active = audioOk_ && (isMp3 ? Mix_PlayingMusic() : Mix_Playing(0));
      double elapsed = (SDL_GetTicks() - audioStartTick_) / 1000.0;
      double duration = 0;
      if (chunk_) {
        int freq = 0, ch = 0;
        Uint16 fmt = 0;
        Mix_QuerySpec(&freq, &fmt, &ch);
        int bytesPerSample = SDL_AUDIO_BITSIZE(fmt) / 8;
        if (freq && ch && bytesPerSample) duration = chunk_->alen / double(freq * ch * bytesPerSample);
      }
#if SDL_MIXER_VERSION_ATLEAST(2, 6, 0)
      if (music_) duration = Mix_MusicDuration(music_);
#endif
      char buf[128];
      if (duration > 0) std::snprintf(buf, sizeof buf, "%s  %.1f / %.1f c", active ? "Играет" : "Стоп", active ? elapsed : 0.0, duration);
      else std::snprintf(buf, sizeof buf, "%s  %.1f c", active ? "Играет" : "Стоп", active ? elapsed : 0.0);
      big_.draw(buf, body.x + 30, body.y + 30, active ? kGood : kText);
      ui_.draw("Space или клик — играть снова, S — стоп. Размер: " + humanSize(audioData_.size()), body.x + 30, body.y + 70, kDim);
      if (duration > 0) {
        SDL_Rect bar{body.x + 30, body.y + 110, std::min(600, body.w - 60), 8};
        fillRect(bar, kPanelLight);
        double t = active ? std::min(1.0, elapsed / duration) : 0.0;
        fillRect(SDL_Rect{bar.x, bar.y, int(bar.w * t), bar.h}, kAccent);
      }
      break;
    }
    case Kind::Font: {
      int y = body.y + 30;
      if (sample_.ok()) {
        for (const char* s : {"Как достать соседа", "Neighbours from Hell", "АБВГДЕЁЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯ",
                              "абвгдеёжзийклмнопрстуфхцчшщъыьэюя", "ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789 !?%"}) {
          sample_.draw(s, body.x + 30, y, kText, body.w - 60);
          y += sample_.lineHeight() + 8;
        }
      }
      break;
    }
    case Kind::Other:
      ui_.draw("Предпросмотр для этого типа файлов пока не поддерживается.", body.x + 20, body.y + 20, kDim);
      break;
    case Kind::None: break;
  }
}

void Viewer::drawImage(const SDL_Rect& body) {
  if (!tex_) return;

  // For animation sequences use the declared offsets so frames don't jitter.
  int frameW = image_.width, frameH = image_.height;
  int dx = 0, dy = 0;  // position of current frame inside the sequence bounding box
  if (!sequence_.empty() && seqHasOffsets_ && seqIndex_ < (int)seqOffsets_.size()) {
    frameW = std::max(1, seqBox_.w);
    frameH = std::max(1, seqBox_.h);
    dx = seqOffsets_[seqIndex_].x - seqBox_.x;
    dy = seqOffsets_[seqIndex_].y - seqBox_.y;
  }

  if (fitMode_) {
    float zx = (body.w - 40) / float(frameW), zy = (body.h - 40) / float(frameH);
    zoom_ = std::clamp(std::min(zx, zy), 0.05f, 4.0f);
  }
  SDL_SetTextureScaleMode(tex_, zoom_ >= 2.0f ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);

  const float boxW = frameW * zoom_, boxH = frameH * zoom_;
  const float bx = body.x + body.w / 2.0f - boxW / 2.0f + panX_;
  const float by = body.y + body.h / 2.0f - boxH / 2.0f + panY_;

  SDL_RenderSetClipRect(ren_, &body);
  SDL_Rect box{int(bx), int(by), int(std::ceil(boxW)), int(std::ceil(boxH))};
  SDL_Rect visible;
  if (SDL_IntersectRect(&box, &body, &visible)) {
    if (checker_) drawCheckerboard(visible);
    else fillRect(visible, SDL_Color{0, 0, 0, 255});
  }
  SDL_RenderSetClipRect(ren_, &body);
  SDL_FRect dst{bx + dx * zoom_, by + dy * zoom_, image_.width * zoom_, image_.height * zoom_};
  SDL_RenderCopyF(ren_, tex_, nullptr, &dst);
  SDL_SetRenderDrawColor(ren_, 255, 255, 255, 40);
  SDL_RenderDrawRect(ren_, &box);
  SDL_RenderSetClipRect(ren_, nullptr);
}

void Viewer::drawText(const SDL_Rect& body) {
  SDL_RenderSetClipRect(ren_, &body);
  const int lh = mono_.lineHeight();
  const int gutter = mono_.width("00000") + 12;
  int y = body.y + 8;
  for (int i = textScroll_; i < (int)lines_.size() && y < body.y + body.h; ++i, y += lh) {
    char num[16];
    std::snprintf(num, sizeof num, "%5d", i + 1);
    mono_.draw(num, body.x + 6, y, SDL_Color{90, 96, 110, 255});
    const std::string& l = lines_[i];
    // Light highlighting: tags vs comments vs plain.
    SDL_Color c = kText;
    size_t first = l.find_first_not_of(' ');
    if (first != std::string::npos) {
      if (l.compare(first, 4, "<!--") == 0) c = SDL_Color{120, 160, 120, 255};
      else if (l[first] == '<') c = SDL_Color{170, 200, 240, 255};
    }
    mono_.draw(l, body.x + gutter, y, c, body.w - gutter - 10);
  }
  SDL_RenderSetClipRect(ren_, nullptr);
}

void Viewer::drawStatus(const SDL_Rect& a) {
  fillRect(a, kPanelLight);
  std::string left;
  if (game_) left = std::to_string(filtered_.size()) + " / " + std::to_string(game_->files().size()) + " файлов";
  ui_.draw(left, a.x + 10, a.y + (a.h - ui_.lineHeight()) / 2, kDim);
  std::string help;
  switch (kind_) {
    case Kind::Image:
      help = sequence_.empty() ? "колесо/+/- масштаб · перетаскивание · F вписать · 1/2 100%/200% · B фон"
                               : "Space анимация · ←/→ кадр · [ ] fps · колесо масштаб · F вписать · B фон";
      break;
    case Kind::Audio: help = "Space играть · S стоп"; break;
    case Kind::Text: help = "колесо или Shift+PgUp/PgDn — прокрутка"; break;
    default: help = "↑/↓ выбор · Ctrl+F фильтр · Ctrl+Q выход"; break;
  }
  int w = ui_.width(help);
  ui_.draw(help, a.x + a.w - w - 10, a.y + (a.h - ui_.lineHeight()) / 2, kDim);
}

}  // namespace nfh
