#include "game/engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <queue>
#include <sstream>
#include <unordered_set>

#include "core/platform.h"
#include "core/png_writer.h"
#include "core/text.h"
#include "core/tga.h"
#include "core/xml.h"

namespace nfh {

namespace {

constexpr int kTicksPerSecond = 12;  // original game tick & animation rate (12 Hz: 3600 ticks = 5:00)
constexpr int kTickMs = 1000 / kTicksPerSecond;

std::vector<fs::path> systemFonts() {
#ifdef _WIN32
  const char* win = std::getenv("WINDIR");
  fs::path f = fs::path(win ? win : "C:\\Windows") / "Fonts";
  return {f / "segoeui.ttf", f / "arial.ttf", f / "tahoma.ttf"};
#else
  return {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
          "/usr/share/fonts/TTF/DejaVuSans.ttf",
          "/usr/share/fonts/dejavu/DejaVuSans.ttf",
          "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
          "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"};
#endif
}

}  // namespace

GameEngine::GameEngine() = default;

GameEngine::~GameEngine() {
  clearTextures();
  for (auto& kv : sfxCache_)
    if (kv.second) Mix_FreeChunk(kv.second);
  sfxCache_.clear();
  if (music_) Mix_FreeMusic(music_);
  fontUI_.close();
  fontBig_.close();
  fontHud_.close();
  fontSmall_.close();
  fontTitle_.close();
  if (audioOk_) Mix_CloseAudio();
  Mix_Quit();
  if (ren_) SDL_DestroyRenderer(ren_);
  if (window_) SDL_DestroyWindow(window_);
  TTF_Quit();
  SDL_Quit();
}

bool GameEngine::init(const GameOptions& opts) {
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
  if (audioOk_) Mix_AllocateChannels(16);

  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
  if (opts.windowW > 0 && opts.windowH > 0) {
    winW_ = opts.windowW;
    winH_ = opts.windowH;
  }
  window_ = SDL_CreateWindow("OpenNFH - Как достать соседа: Сладкая месть", SDL_WINDOWPOS_CENTERED,
                             SDL_WINDOWPOS_CENTERED, winW_, winH_, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if (!window_) return false;
  ren_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!ren_) ren_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
  if (!ren_) return false;
  SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
  SDL_GetRendererOutputSize(ren_, &winW_, &winH_);
  SDL_ShowCursor(SDL_DISABLE);

  const fs::path exeDir = executableDir();
  const fs::path gamePath = GameData::locate(opts.gamePath, exeDir);
  gd_ = std::make_unique<GameData>();
  std::string err;
  if (gamePath.empty() || !gd_->open(gamePath, &err)) {
    std::fprintf(stderr, "Failed to open game data: %s\n", err.c_str());
    return false;
  }
  if (!opts.gamePath.empty()) GameData::rememberPath(exeDir, gamePath);

  auto sysFonts = systemFonts();
  fontSmall_.load(ren_, sysFonts, 14);
  fontUI_.load(ren_, sysFonts, 17);
  fontBig_.load(ren_, sysFonts, 22);
  fontTitle_.load(ren_, sysFonts, 28);
  std::vector<uint8_t> ttfData;
  if (gd_->read("loose/bluehigh.ttf", ttfData)) {
    fontHud_.loadFromMemory(ren_, ttfData, 26);
  } else {
    fontHud_.load(ren_, sysFonts, 24);
  }

  levels_ = loadLevelList(*gd_);
  if (levels_.empty()) {
    std::fprintf(stderr, "No levels found in gamedata/leveldata.xml\n");
    return false;
  }

  loadMenuData();
  loadUserProgress();
  applyAudioVolumes();

  int startIdx = 0;
  for (size_t i = 0; i < levels_.size(); ++i) {
    if (toLowerAscii(levels_[i].name) == toLowerAscii(opts.levelName)) {
      startIdx = (int)i;
      break;
    }
  }
  previewLevelIdx_ = startIdx;
  if (!selectLevel(startIdx)) return false;

  const std::string scr = toLowerAscii(opts.initialScreen);
  if (scr == "mainmenu" || scr == "menu") {
    enterScreen(ScreenState::MainMenu);
  } else if (scr == "episodes" || scr == "newgame") {
    enterScreen(ScreenState::EpisodeSelect);
  } else if (scr == "options") {
    optionsReturnScreen_ = ScreenState::MainMenu;
    enterScreen(ScreenState::Options);
  } else if (scr == "pause") {
    enterScreen(ScreenState::PauseMenu);
  } else if (scr == "credits") {
    enterScreen(ScreenState::Credits);
  } else if (scr == "summary" || scr == "gameover") {
    quota_ = 85;
    tricksDone_ = level_.info.reachable;
    finishEpisode(false, false);
  } else if (scr == "victory") {
    completedSeason_ = 1;
    seasonTrophyTier_ = 3;
    enterScreen(ScreenState::SeasonVictory);
  } else if (scr == "confirm") {
    enterScreen(ScreenState::InGame);
    confirmAction_ = ConfirmAction::RestartEpisode;
    confirmText_ = "Вы действительно хотите начать съемки заново?";
  } else if (scr == "test_decline") {
    enterScreen(ScreenState::InGame);
    for (size_t i = 0; i < level_.objects.size(); ++i) {
      PlacedObject& po = level_.objects[i];
      const ObjectDef* od = level_.findObjectDef(po.name);
      if (od && (od->hasFlag("container") || !po.items.empty())) {
        po.items.clear();
        woody_.room = po.roomName;
        Point targetPos = od->hotspotOr("woody", Point{450, 420});
        woody_.pos = Point{po.position.x + targetPos.x, po.position.y + targetPos.y};
        woodyQueue_.clear();
        TaskStep inter{};
        inter.type = TaskStep::Type::InteractObject;
        inter.objectIndex = (int)i;
        woodyQueue_.push_back(inter);
        std::printf("Triggered test_decline on object %s (idx %d)\n", po.name.c_str(), (int)i);
        break;
      }
    }
  } else if (scr == "test_progress") {
    enterScreen(ScreenState::InGame);
    woodyActionTotalTicks_ = 100;
    woodyActionTicks_ = 27;
  } else if (opts.startInLevel || !opts.screenshot.empty() || !opts.recordDir.empty()) {
    enterScreen(ScreenState::InGame);
  } else {
    enterScreen(ScreenState::MainMenu);
  }
  return true;
}

const GameEngine::TexEntry* GameEngine::getTexture(const std::string& vpath) {
  if (vpath.empty()) return nullptr;
  const std::string key = toLowerAscii(vpath);
  auto it = texCache_.find(key);
  if (it != texCache_.end()) return it->second.tex ? &it->second : nullptr;

  TexEntry entry;
  std::vector<uint8_t> raw;
  Image img;
  if (gd_->read(vpath, raw) && decodeTga(raw, img)) {
    entry.tex = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, img.width, img.height);
    if (entry.tex) {
      SDL_UpdateTexture(entry.tex, nullptr, img.rgba.data(), img.width * 4);
      SDL_SetTextureBlendMode(entry.tex, SDL_BLENDMODE_BLEND);
      entry.w = img.width;
      entry.h = img.height;
    }
  }
  auto [ins, _] = texCache_.emplace(key, entry);
  return ins->second.tex ? &ins->second : nullptr;
}

void GameEngine::clearTextures() {
  for (auto& kv : texCache_)
    if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
  texCache_.clear();
}

void GameEngine::playSfx(const std::string& sfxPath) {
  if (!audioOk_ || muteMaster_ || sfxPath.empty()) return;
  std::string vpath = sfxPath;
  if (toLowerAscii(vpath).rfind("sfx/", 0) == 0 && !gd_->find(vpath)) {
    vpath = "sfx/" + vpath;  // "sfx/wod_step1a.wav" -> "sfx/sfx/wod_step1a.wav"
  }
  const std::string key = toLowerAscii(vpath);
  auto it = sfxCache_.find(key);
  if (it == sfxCache_.end()) {
    std::vector<uint8_t> buf;
    Mix_Chunk* ch = nullptr;
    if (gd_->read(vpath, buf)) {
      sfxMem_[key] = std::move(buf);
      auto& stored = sfxMem_[key];
      ch = Mix_LoadWAV_RW(SDL_RWFromConstMem(stored.data(), (int)stored.size()), 1);
    }
    it = sfxCache_.emplace(key, ch).first;
  }
  if (it->second) Mix_PlayChannel(-1, it->second, 0);
}

void GameEngine::startMusic(const std::string& musicVpath) {
  if (!audioOk_ || musicVpath.empty()) return;
  if (toLowerAscii(currentMusicVpath_) == toLowerAscii(musicVpath) && music_) return;
  currentMusicVpath_ = musicVpath;
  if (music_) {
    Mix_FreeMusic(music_);
    music_ = nullptr;
  }
  if (gd_->read(musicVpath, musicMem_)) {
    music_ = Mix_LoadMUSType_RW(SDL_RWFromConstMem(musicMem_.data(), (int)musicMem_.size()), MUS_MP3, 1);
    if (!music_) {
      music_ = Mix_LoadMUS_RW(SDL_RWFromConstMem(musicMem_.data(), (int)musicMem_.size()), 1);
    }
    if (music_) {
      applyAudioVolumes();
      Mix_PlayMusic(music_, -1);
    }
  }
}

void GameEngine::applyAudioVolumes() {
  if (!audioOk_) return;
  int sfxVol = muteMaster_ ? 0 : (masterVolume_ * MIX_MAX_VOLUME / 100);
  int musVol = (muteMaster_ || muteMusic_) ? 0 : (musicVolume_ * masterVolume_ * MIX_MAX_VOLUME / 10000);
  Mix_Volume(-1, std::clamp(sfxVol, 0, MIX_MAX_VOLUME));
  Mix_VolumeMusic(std::clamp(musVol, 0, MIX_MAX_VOLUME));
}

void GameEngine::loadMenuData() {
  auto parseStringsFile = [&](const std::string& vpath) {
    std::vector<uint8_t> raw;
    if (!gd_->read(vpath, raw)) return;
    XmlDocument doc = parseXml(decodeToUtf8(raw));
    const XmlNode* root = doc.root.child("strings");
    if (!root) root = &doc.root;
    for (const XmlNode* c : root->childrenNamed("string")) {
      std::string name = toLowerAscii(c->attrOr("name"));
      std::string cat = toLowerAscii(c->attrOr("category"));
      std::string txt = c->attrOr("text");
      if (cat == "statusbar") {
        statusStrings_[name] = txt;
      } else if (cat == "gameover") {
        menuStrings_["gameover_" + name] = txt;
      } else {
        menuStrings_[name] = txt;
      }
    }
  };
  parseStringsFile("gamedata/generic/strings.xml");
  parseStringsFile("gamedata/mainmenu/strings.xml");

  struct BtnMeta {
    const char* levelName;
    const char* shortName;
    int x;
    int y;
  };
  static const BtnMeta kBtns[] = {
      {"tutorial_1", "tut1", 25, 86},       {"tutorial_2", "tut2", 112, 86},
      {"tutorial_3", "tut3", 199, 86},      {"level_peep", "peep", 25, 185},
      {"level_sofa", "sofa", 112, 185},     {"level_mail", "mail", 199, 185},
      {"level_pie", "pie", 286, 185},       {"level_piano", "piano", 25, 249},
      {"level_bath", "bath", 112, 249},     {"level_art", "art", 25, 348},
      {"level_suntan", "suntan", 112, 348}, {"level_pig", "pig", 199, 348},
      {"level_barbecue", "barbecue", 286, 348},
      {"level_laundry", "laundry", 25, 447},{"level_fitness", "fitness", 112, 447},
      {"level_DIY", "diy", 199, 447},       {"level_hunter", "hunter", 286, 447},
  };

  briefings_.resize(levels_.size());
  for (size_t i = 0; i < levels_.size(); ++i) {
    BriefingInfo& b = briefings_[i];
    b.title = levels_[i].name;
    b.buttonShortName = "peep";
    for (const auto& m : kBtns) {
      if (toLowerAscii(levels_[i].name) == toLowerAscii(m.levelName)) {
        b.buttonShortName = m.shortName;
        b.menuX = m.x;
        b.menuY = m.y;
        break;
      }
    }

    std::vector<uint8_t> raw;
    if (gd_->read("gamedata/dialogs/briefing/" + levels_[i].name + ".xml", raw)) {
      XmlDocument doc = parseXml(decodeToUtf8(raw));
      const XmlNode* root = doc.root.child("strings");
      if (!root) root = &doc.root;
      for (const XmlNode* c : root->childrenNamed("string")) {
        std::string n = toLowerAscii(c->attrOr("name"));
        std::string t = c->attrOr("text");
        if (n == "title") b.title = t;
        else if (n == "hinttext") b.hintText = t;
        else if (n == "pictext") b.picText = t;
        else if (n == "briefing") b.briefing = t;
      }
    }
  }

  creditsLines_.clear();
  creditsLines_.push_back({"OpenNFH - Кроссплатформенный движок (C++17 / SDL2)", true});
  creditsLines_.push_back({"Как достать соседа: Сладкая месть", false});
  creditsLines_.push_back({"", false});
  std::vector<uint8_t> craw;
  if (gd_->read("gamedata/dialogs/creditstext.xml", craw)) {
    XmlDocument doc = parseXml(decodeToUtf8(craw));
    const XmlNode* root = doc.root.child("strings");
    if (!root) root = &doc.root;
    for (const XmlNode* c : root->childrenNamed("string")) {
      std::string txt = c->attrOr("text");
      std::string nm = c->attrOr("name");
      bool isHeader = (!nm.empty() && nm.back() == 'a');
      creditsLines_.push_back({txt, isHeader});
    }
  }
}

void GameEngine::loadUserProgress() {
  const fs::path savePath = executableDir() / "opennfh_save.cfg";
  std::vector<uint8_t> raw;
  if (!readWholeFile(savePath, raw)) return;
  std::string content(reinterpret_cast<const char*>(raw.data()), raw.size());
  std::istringstream iss(content);
  std::string line;
  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = line.substr(0, eq);
    std::string v = line.substr(eq + 1);
    if (k == "masterVolume") masterVolume_ = std::clamp(std::atoi(v.c_str()), 0, 100);
    else if (k == "musicVolume") musicVolume_ = std::clamp(std::atoi(v.c_str()), 0, 100);
    else if (k == "muteMaster") muteMaster_ = (v == "1");
    else if (k == "muteMusic") muteMusic_ = (v == "1");
    else if (k == "highQuality") optHighQuality_ = (v == "1");
    else if (k == "timeLimit") optTimeLimit_ = (v == "1");
    else if (k == "autoScroll") optAutoScroll_ = (v == "1");
    else if (k == "coloredCursor") optColoredCursor_ = (v == "1");
    else if (k.rfind("level.", 0) == 0) {
      std::string lv = toLowerAscii(k.substr(6));
      size_t comma = v.find(',');
      if (comma != std::string::npos) {
        LevelProgress lp;
        lp.bestQuota = std::clamp(std::atoi(v.substr(0, comma).c_str()), 0, 100);
        lp.bestTricks = std::atoi(v.substr(comma + 1).c_str());
        lp.completed = lp.bestQuota > 0;
        progress_[lv] = lp;
      }
    }
  }
}

void GameEngine::saveUserProgress() const {
  const fs::path savePath = executableDir() / "opennfh_save.cfg";
  std::ostringstream oss;
  oss << "# OpenNFH user profile & settings\n";
  oss << "masterVolume=" << masterVolume_ << "\n";
  oss << "musicVolume=" << musicVolume_ << "\n";
  oss << "muteMaster=" << (muteMaster_ ? 1 : 0) << "\n";
  oss << "muteMusic=" << (muteMusic_ ? 1 : 0) << "\n";
  oss << "highQuality=" << (optHighQuality_ ? 1 : 0) << "\n";
  oss << "timeLimit=" << (optTimeLimit_ ? 1 : 0) << "\n";
  oss << "autoScroll=" << (optAutoScroll_ ? 1 : 0) << "\n";
  oss << "coloredCursor=" << (optColoredCursor_ ? 1 : 0) << "\n";
  for (const auto& kv : progress_) {
    oss << "level." << kv.first << "=" << std::clamp(kv.second.bestQuota, 0, 100) << "," << kv.second.bestTricks << "\n";
  }
  std::string out = oss.str();
  writeWholeFile(savePath, reinterpret_cast<const uint8_t*>(out.data()), out.size());
}

void GameEngine::resetUserProgress() {
  progress_.clear();
  saveUserProgress();
}

void GameEngine::enterScreen(ScreenState st) {
  screen_ = st;
  confirmAction_ = ConfirmAction::None;
  selectedInv_ = -1;
  hoverText_.clear();
  cursorVpath_ = "gfx/cursor/normal.tga";

  if (st == ScreenState::MainMenu || st == ScreenState::EpisodeSelect || st == ScreenState::Options ||
      st == ScreenState::Credits) {
    startMusic("sfx/music/titel.mp3");
  } else if (st == ScreenState::InGame) {
    std::string setLow = toLowerAscii(level_.info.setName);
    startMusic((setLow == "set02" || setLow == "set03") ? "sfx/music/ingame2_normal.mp3" : "sfx/music/ingame1_slow.mp3");
  }

  if (st == ScreenState::Options) {
    bakMasterVolume_ = masterVolume_;
    bakMusicVolume_ = musicVolume_;
    bakMuteMaster_ = muteMaster_;
    bakMuteMusic_ = muteMusic_;
    bakHighQuality_ = optHighQuality_;
    bakTimeLimit_ = optTimeLimit_;
    bakAutoScroll_ = optAutoScroll_;
    bakColoredCursor_ = optColoredCursor_;
  } else if (st == ScreenState::Credits) {
    creditsScrollY_ = 540.0f;
  }
}

void GameEngine::finishEpisode(bool timeExpired, bool caughtOut) {
  if (!timeExpired && !caughtOut && level_.info.reachable > 0 && tricksDone_ >= level_.info.reachable) {
    quota_ = 100;
  }
  quota_ = std::clamp(quota_, 0, 100);
  summaryTimeExpired_ = timeExpired;
  summarySuccess_ = !timeExpired && !caughtOut && (quota_ >= level_.info.minQuota);
  summaryPerfect_ = summarySuccess_ && (tricksDone_ >= level_.info.reachable);

  if (summarySuccess_) {
    std::string key = toLowerAscii(level_.info.name);
    LevelProgress& lp = progress_[key];
    lp.completed = true;
    if (quota_ > lp.bestQuota) lp.bestQuota = quota_;
    if (tricksDone_ > lp.bestTricks) lp.bestTricks = tricksDone_;
    if (summaryPerfect_) lp.perfect = true;
    saveUserProgress();
  }

  playSfx(summaryPerfect_ ? "sfx/applause2.wav" : (summarySuccess_ ? "sfx/applause1.wav" : "sfx/boo1.wav"));
  enterScreen(ScreenState::LevelSummary);
}

bool GameEngine::selectLevel(int index) {
  if (levels_.empty()) return false;
  currentLevelIdx_ = (index % (int)levels_.size() + (int)levels_.size()) % (int)levels_.size();
  previewLevelIdx_ = currentLevelIdx_;
  std::string err;
  if (!loadLevel(*gd_, levels_[currentLevelIdx_], level_, &err)) {
    std::fprintf(stderr, "loadLevel failed: %s\n", err.c_str());
    return false;
  }

  woodyQueue_.clear();
  neighborQueue_.clear();
  woodyBusyInAnim_ = false;
  neighborBusyInAnim_ = false;
  woodyActionTicks_ = woodyActionTotalTicks_ = 0;
  inventory_.clear();
  selectedInv_ = -1;
  invScroll_ = 0;
  quota_ = 0;
  lastQuotaGain_ = 0;
  tricksDone_ = 0;
  angerTicks_ = 0;
  angerMaxTicks_ = std::max(1, level_.defaultAngryTime);
  remainingTicks_ = level_.info.timeTicks;
  catchSequenceActive_ = false;
  catchTicks_ = 0;
  catchesCount_ = 0;
  neighborChasing_ = false;
  neighborAlertTicks_ = 0;

  // Initialize actors from spawns
  woody_ = ActorState{"woody", "fro", Point{110, 218}, 4, 2, false, -1, "ms2", "ms2", 0};
  neighbor_ = ActorState{"neighbor", "kit", Point{400, 420}, 4, 2, false, -1, "ms2", "ms2", 0};
  bool hasNeighbor = false;
  for (const ActorSpawn& sp : level_.spawns) {
    if (toLowerAscii(sp.name) == "woody") {
      woody_.room = sp.room;
      woody_.pos = sp.position;
      woody_.layer = sp.layer;
      if (toLowerAscii(woody_.room) == "fro" && woody_.pos.x > 140) {
        woody_.pos.x = 190;
        TaskStep introWalk{};
        introWalk.type = TaskStep::Type::WalkToX;
        introWalk.targetX = 105;
        introWalk.sneak = false;
        woodyQueue_.push_back(introWalk);
      }
    } else if (toLowerAscii(sp.name) == "neighbor") {
      neighbor_.room = sp.room;
      neighbor_.pos = sp.position;
      neighbor_.layer = sp.layer;
      hasNeighbor = true;
    }
  }
  if (!hasNeighbor) neighbor_.hidden = true;  // tutorial_1 has HAL instead of neighbor

  // In level_hunter, lir/tabacbox is placed on the living room table by the Neighbor at the start
  if (level_.findObjectDef("lir/tabacbox") && findObjectIndex("lir/tabacbox", "lir") < 0) {
    PlacedObject tb;
    tb.name = "lir/tabacbox";
    tb.roomName = "lir";
    tb.layer = 1;
    tb.visible = true;
    level_.objects.push_back(std::move(tb));
  }

  buildNeighborRoutine();

  std::string epTitle = (currentLevelIdx_ < (int)briefings_.size()) ? briefings_[currentLevelIdx_].title : level_.info.name;
  std::string title = "OpenNFH - " + epTitle + " [" + level_.info.name + "]";
  SDL_SetWindowTitle(window_, title.c_str());
  statusBanner_ = "Серия: «" + epTitle + "»  (Esc - меню, V - камера, F11 - полный экран, R - рестарт)";
  bannerTicks_ = kTicksPerSecond * 4;
  return true;
}

int GameEngine::animFrameCount(const std::string& gfxName, const std::string& animName, int fallback) const {
  if (gfxName.empty() || animName.empty() || toLowerAscii(animName) == "inv") return fallback;
  const GfxObjectDef* gd = level_.findGfxObject(gfxName);
  if (!gd) return fallback;
  auto it = gd->anims.find(toLowerAscii(animName));
  if (it == gd->anims.end() || it->second.frames.empty()) return fallback;
  return (int)it->second.frames.size();
}

int GameEngine::actorSpeed(const std::string& actorName, const std::string& speedName, int fallback) const {
  auto it = level_.actors.find(toLowerAscii(actorName));
  if (it == level_.actors.end()) return fallback;
  auto sit = it->second.speeds.find(toLowerAscii(speedName));
  return sit != it->second.speeds.end() ? sit->second : fallback;
}

std::string GameEngine::objectOriginalName(const std::string& trickObjName) const {
  const std::string key = toLowerAscii(trickObjName);
  for (const auto& c : level_.combinations) {
    if (toLowerAscii(c.resultObject) == key && c.removeTarget) return c.target;
  }
  return {};
}

void GameEngine::buildNeighborRoutine() {
  routine_.clear();
  routineIdx_ = 0;
  currentBubbleIcon_.clear();

  auto stripUnderscores = [](const std::string& s) {
    std::string out;
    for (char c : s)
      if (c != '_') out.push_back(c);
    return out;
  };

  struct Candidate {
    RoutineStop stop;
    std::string shortRaw;
    std::string shortNorm;
    bool used = false;
  };
  std::vector<Candidate> candidates;

  for (const PlacedObject& po : level_.objects) {
    if (po.isDoor) continue;
    const std::string lowerPo = toLowerAscii(po.name);
    // Helper objects driven as part of another stop's animation sequence:
    if (lowerPo == "kit/window" || lowerPo == "bed/alarmclock" || lowerPo == "toi/shower" ||
        lowerPo == "toi/towel" || lowerPo == "lir/pieces" || lowerPo == "bed/posingspot" ||
        lowerPo == "aux" || lowerPo == "anc/wardrobe" || lowerPo == "bal/plant") {
      continue;
    }
    if (lowerPo == "bed/bed" && !level_.findObjectDef("bed/bed_sleep")) continue;

    const ObjectDef* od = level_.findObjectDef(po.name);
    if (!od) continue;

    bool hasNormalNeighborAct = (od->hotspots.count("neighbor") > 0 && !od->hasFlag("hideout") &&
                                 (!od->hasFlag("container") || lowerPo == "kit/fridge" ||
                                  lowerPo == "bed/keyboard" || lowerPo == "bed/magnesiumbottle"));
    for (const ActionDef& a : od->actions) {
      if (toLowerAscii(a.actor) == "neighbor" && a.name != "clean" && a.name != "repair") {
        hasNormalNeighborAct = true;
        break;
      }
    }

    std::string trickObjName;
    bool hasTrickNeighborAct = false;
    for (const auto& c : level_.combinations) {
      if (toLowerAscii(c.target) == lowerPo && c.isTrick) {
        trickObjName = c.resultObject;
        hasTrickNeighborAct = true;
        break;
      }
    }

    if (!hasNormalNeighborAct && !hasTrickNeighborAct) continue;

    RoutineStop stop;
    stop.room = po.roomName;
    stop.normalObj = po.name;
    stop.trickObj = trickObjName;
    if (lowerPo == "kit/binoculars") stop.fallbackObj = "kit/window";
    else if (lowerPo == "bed/teeth_empty") {
      stop.fallbackObj = "bed/teeth";
      stop.trickObj = "bed/teeth_tabasco";
    } else if (lowerPo == "toi/tub_empty") {
      stop.fallbackObj = "toi/tub";
      stop.trickObj = "toi/tub_hair";
    } else if (lowerPo == "bal/noclothes") {
      stop.fallbackObj = "bal/clothes";
      stop.trickObj = "bal/clothes_food";
    } else if (lowerPo == "bed/ironingboard") {
      stop.fallbackObj = "bed/ironingboard_clothes";
      stop.trickObj = "bed/ironingboard_burn";
    } else if (lowerPo == "lir/lockedphono") {
      stop.fallbackObj = "lir/phono_open";
      stop.trickObj = "lir/phono_nail";
    }
    stop.onlyWhenTricked = !hasNormalNeighborAct;

    size_t slash = po.name.find('/');
    std::string shortRaw = toLowerAscii(slash == std::string::npos ? po.name : po.name.substr(slash + 1));
    std::string shortNorm = stripUnderscores(shortRaw);
    stop.iconName = shortRaw;
    candidates.push_back(Candidate{std::move(stop), shortRaw, shortNorm, false});
  }

  // 1. Match normal routine stops in the exact order of <icon> tags in <level>/objects.xml
  for (const std::string& icon : level_.levelIcons) {
    std::string iconNorm = stripUnderscores(icon);
    int matched = -1;
    for (size_t i = 0; i < candidates.size(); ++i) {
      if (candidates[i].used || candidates[i].stop.onlyWhenTricked) continue;
      const std::string& sr = candidates[i].shortRaw;
      const std::string& sn = candidates[i].shortNorm;
      if (sr == icon || sn == iconNorm ||
          (icon == "milk_bottle" && (sr == "fridge" || sr == "milkbottle" || sr == "babybottle")) ||
          (icon == "mail" && sr == "mailbox") ||
          (icon == "parrot" && sr == "chili") ||
          (icon == "piano" && sr == "score") ||
          (icon == "football" && sr == "bowlingbag") ||
          (icon == "photo_album" && sr == "book") ||
          (icon == "bath" && (sr == "tub_empty" || sr == "tub")) ||
          (icon == "magnesium" && sr == "magnesiumbottle") ||
          (icon == "statue" && (sr == "footstool" || sr == "statue")) ||
          (icon == "painting" && (sr == "picture_empty" || sr == "picture")) ||
          (icon == "toothbrush" && sr == "toothbrushset") ||
          (icon == "coffee" && sr == "coffeebox") ||
          (icon == "teeth" && (sr == "teeth_empty" || sr == "teeth")) ||
          (icon == "pig_key" && sr == "keyboard") ||
          (icon == "cookies" && sr == "cookiebox") ||
          ((icon == "alarm_clock" || icon == "bed" || icon == "sleep") && sr == "bed") ||
          (icon == "bbq" && sr == "barbecue") ||
          (icon == "plant" && (sr == "spray" || sr == "plant")) ||
          (icon == "laundry_rack" && (sr == "noclothes" || sr == "clothes")) ||
          (icon == "ironing" && sn.find("ironing") != std::string::npos) ||
          (icon == "yoga_mat" && sr == "mat") ||
          (icon == "powertool" && sr == "anglegrinder") ||
          (icon == "valve" && (sr == "valve_on" || sr == "valve_off")) ||
          (icon == "heat_valve" && (sr == "heatvalve_off" || sr == "heatvalve_on")) ||
          (icon == "basin" && (sr == "basin_flooded" || sr == "basin")) ||
          (icon == "chairkit" && sr == "stoolkit") ||
          (icon == "phonograph" && (sr == "lockedphono" || sr == "phono")) ||
          (icon == "medals" && sr == "medalbox") ||
          (icon == "horn" && sr == "woodhorn") ||
          (icon == "smoke" && (sn.find("pipe") != std::string::npos || sn.find("tabac") != std::string::npos))) {
        matched = (int)i;
        break;
      }
    }
    if (matched >= 0) {
      candidates[matched].used = true;
      candidates[matched].stop.iconName = icon;
      routine_.push_back(candidates[matched].stop);
    }
  }

  // 2. Fallback if a level had no explicit <icon> tags (e.g. tutorial_1)
  if (routine_.empty()) {
    for (auto& c : candidates) {
      if (!c.stop.onlyWhenTricked && c.shortRaw != "fridge") {
        c.used = true;
        routine_.push_back(c.stop);
      }
    }
  }

  if (!routine_.empty()) {
    currentBubbleIcon_ = routine_[0].iconName;
  }
}

int GameEngine::findObjectIndex(const std::string& name, const std::string& room) const {
  const std::string key = toLowerAscii(name);
  const std::string rkey = toLowerAscii(room);
  for (size_t i = 0; i < level_.objects.size(); ++i) {
    if (!level_.objects[i].visible) continue;
    if (toLowerAscii(level_.objects[i].name) == key &&
        (rkey.empty() || toLowerAscii(level_.objects[i].roomName) == rkey)) {
      return (int)i;
    }
  }
  return -1;
}

std::vector<RoomEdge> GameEngine::findRoomPath(const std::string& fromRoom, const std::string& toRoom) const {
  const std::string src = toLowerAscii(fromRoom);
  const std::string dst = toLowerAscii(toRoom);
  if (src == dst) return {};

  std::queue<std::string> q;
  std::unordered_map<std::string, std::pair<std::string, RoomEdge>> parent;
  std::unordered_set<std::string> visited;
  q.push(src);
  visited.insert(src);

  while (!q.empty()) {
    std::string cur = q.front();
    q.pop();
    const RoomDef* r = level_.findRoom(cur);
    if (!r) continue;
    for (const RoomEdge& e : r->neighbors) {
      std::string nxt = toLowerAscii(e.targetRoom);
      if (visited.count(nxt)) continue;
      visited.insert(nxt);
      parent[nxt] = {cur, e};
      if (nxt == dst) {
        std::vector<RoomEdge> path;
        std::string step = dst;
        while (step != src) {
          auto it = parent.find(step);
          path.push_back(it->second.second);
          step = it->second.first;
        }
        std::reverse(path.begin(), path.end());
        return path;
      }
      q.push(nxt);
    }
  }
  return {};
}

Rect GameEngine::objectWorldHitbox(const PlacedObject& po) const {
  const RoomDef* r = level_.findRoom(po.roomName);
  Point base{ po.position.x + (r ? r->offset.x : 0), po.position.y + (r ? r->offset.y : 0) };
  const ObjectDef* od = level_.findObjectDef(po.name);
  const std::string gfxName = od ? od->gfx : po.name;
  if (const GfxObjectDef* gd = level_.findGfxObject(gfxName)) {
    if (!gd->regions.empty()) {
      Rect reg = gd->regions[0];
      return Rect{base.x + reg.x, base.y + reg.y, reg.w, reg.h};
    }
    // Try current animation first frame bounds
    auto it = gd->anims.find(toLowerAscii(po.currentAnim));
    if (it == gd->anims.end() && !gd->anims.empty()) it = gd->anims.begin();
    if (it != gd->anims.end() && !it->second.frames.empty()) {
      const AnimFrame& fr = it->second.frames[0];
      const TexEntry* t = const_cast<GameEngine*>(this)->getTexture(fr.vpath);
      int w = t ? t->w : 64, h = t ? t->h : 64;
      return Rect{base.x + fr.offset.x, base.y + fr.offset.y, w, h};
    }
  }
  if (od) {
    Point hs = od->hotspotOr("woody", od->hotspotOr("neighbor", Point{450, 390}));
    return Rect{base.x + hs.x - 35, base.y + hs.y - 90, 70, 90};
  }
  return Rect{base.x, base.y, 50, 50};
}

const CombinationDef* GameEngine::findCombination(const std::string& targetName, const std::string& itemName) const {
  const std::string tKey = toLowerAscii(targetName);
  const std::string iKey = toLowerAscii(itemName);
  for (const auto& c : level_.combinations) {
    if (toLowerAscii(c.target) == tKey && toLowerAscii(c.item) == iKey) return &c;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Player interaction & Hover
// ---------------------------------------------------------------------------

void GameEngine::updateHover(int worldX, int worldY, int screenX, int screenY) {
  hoveredObject_ = -1;
  hoveredRoom_.clear();
  hoverText_.clear();
  cursorVpath_ = "gfx/cursor/normal.tga";
  if (screen_ != ScreenState::InGame) return;

  // Check if mouse is over bottom HUD
  const float hudScale = std::min(float(winW_) / 800.0f, float(winH_) / 480.0f);
  const float hudX = (float(winW_) - 800.0f * hudScale) * 0.5f;
  const float hudY = float(winH_) - 600.0f * hudScale;
  if (screenY >= winH_ - int(134 * hudScale)) {
    int hx = int((screenX - hudX) / hudScale);
    int hy = int((screenY - hudY) / hudScale);
    Rect missionBtn{581 + 167, 471 + 20, 48, 36};
    if (missionBtn.contains(hx, hy)) {
      hoverText_ = "Игровое меню (Esc)";
      cursorVpath_ = "gfx/cursor/use.tga";
      return;
    }
    for (int slot = 0; slot < 5; ++slot) {
      int idx = invScroll_ + slot;
      if (idx >= (int)inventory_.size()) break;
      Rect r{123 + 41 + slot * 75, 511 + 26, 60, 55};
      if (r.contains(hx, hy)) {
        std::string itemName = level_.getString(inventory_[idx], "name");
        hoverText_ = itemName.empty() ? inventory_[idx] : itemName;
        cursorVpath_ = "gfx/cursor/use.tga";
        return;
      }
    }
    return;
  }

  const std::string heldItem = (selectedInv_ >= 0 && selectedInv_ < (int)inventory_.size()) ? inventory_[selectedInv_] : std::string();

  // Hit-test objects (prefer higher layers and doors/objects)
  int bestLayer = -1;
  for (int i = (int)level_.objects.size() - 1; i >= 0; --i) {
    const PlacedObject& po = level_.objects[i];
    if (!po.visible || toLowerAscii(po.name) == "house") continue;
    Rect hb = objectWorldHitbox(po);
    if (hb.contains(worldX, worldY) && po.layer >= bestLayer) {
      bestLayer = po.layer;
      hoveredObject_ = i;
    }
  }

  // Hit-test room floors
  for (const RoomDef& r : level_.rooms) {
    for (const FloorRect& fl : r.floors) {
      Rect wr{r.offset.x + fl.rect.x, r.offset.y + fl.rect.y, fl.rect.w, fl.rect.h};
      if (wr.contains(worldX, worldY)) {
        hoveredRoom_ = r.name;
        break;
      }
    }
    if (!hoveredRoom_.empty()) break;
  }

  if (hoveredObject_ >= 0) {
    const PlacedObject& po = level_.objects[hoveredObject_];
    const ObjectDef* od = level_.findObjectDef(po.name);
    std::string objTitle = level_.getString(po.name, "name");
    if (objTitle.empty()) objTitle = po.name;

    if (!heldItem.empty()) {
      std::string itemTitle = level_.getString(heldItem, "name");
      if (itemTitle.empty()) itemTitle = heldItem;
      std::string withTitle = level_.getString(po.name, "with");
      if (withTitle.empty()) withTitle = objTitle;
      hoverText_ = level_.getUIString("use") + " " + itemTitle + " " + level_.getUIString("with") + " " + withTitle;
      const CombinationDef* combo = findCombination(po.name, heldItem);
      cursorVpath_ = (combo && !combo->isWrong) ? "gfx/cursor/use_with.tga" : "gfx/cursor/not_possible.tga";
      return;
    }

    // Check bare-hand combination (like twisting the TV antenna in level_peep or valves in level_DIY)
    if (const CombinationDef* bareCombo = findCombination(po.name, "")) {
      if (!bareCombo->isWrong) {
        hoverText_ = level_.getUIString("use") + " " + objTitle;
        cursorVpath_ = "gfx/cursor/use.tga";
        return;
      }
    }

    const std::string sa = od ? toLowerAscii(od->stdAction) : "look";
    if (po.isDoor || sa == "goto" || sa == "end") {
      std::string verb = level_.getUIString(sa == "end" ? "end" : "goto");
      if (verb.empty()) verb = "Идти в";
      hoverText_ = verb + " " + objTitle;
      if (od && od->hasFlag("doorleft")) cursorVpath_ = "gfx/cursor/use_door_3.tga";
      else if (od && od->hasFlag("doorright")) cursorVpath_ = "gfx/cursor/use_door_1.tga";
      else if (od && od->hasFlag("doordown")) cursorVpath_ = "gfx/cursor/use_door_2.tga";
      else cursorVpath_ = "gfx/cursor/use_door_0.tga";
    } else if (od && od->hasFlag("hideout")) {
      std::string hideWhere = level_.getString(po.name, "hide");
      hoverText_ = level_.getUIString("hide") + " " + (hideWhere.empty() ? objTitle : hideWhere);
      cursorVpath_ = "gfx/cursor/hide.tga";
    } else if (od && (od->hasFlag("container") || !po.items.empty())) {
      std::string verb = level_.getUIString(sa == "examine" ? "examine" : "open");
      if (verb.empty()) verb = "Осмотреть";
      hoverText_ = verb + " " + objTitle;
      cursorVpath_ = "gfx/cursor/use.tga";
    } else {
      std::string verb = level_.getUIString("look");
      if (verb.empty()) verb = "Посмотреть на";
      hoverText_ = verb + " " + objTitle;
      cursorVpath_ = "gfx/cursor/look.tga";
    }
  } else if (!hoveredRoom_.empty()) {
    if (!heldItem.empty()) {
      const CombinationDef* roomCombo = findCombination(hoveredRoom_, heldItem);
      std::string itemTitle = level_.getString(heldItem, "name");
      std::string roomTitle = level_.getString(hoveredRoom_, "name");
      hoverText_ = level_.getUIString("use") + " " + itemTitle + " (" + (roomTitle.empty() ? hoveredRoom_ : roomTitle) + ")";
      cursorVpath_ = (roomCombo && !roomCombo->isWrong) ? "gfx/cursor/use_with.tga" : "gfx/cursor/not_possible.tga";
    } else {
      std::string floorTitle = level_.getString("floor", "name");
      hoverText_ = (floorTitle.empty() ? "Идти" : floorTitle);
      cursorVpath_ = "gfx/cursor/goto.tga";
    }
  }
}

void GameEngine::enqueueRoomWalk(std::deque<TaskStep>& q, const std::string& roomName, Point& curPos, Point targetPos,
                                 bool sneak) {
  const RoomDef* r = level_.findRoom(roomName);
  int pathY = (r && r->path1.y > 0) ? r->path1.y : (curPos.y > 0 ? curPos.y : 420);
  if (r) {
    int lo = std::min(r->path1.x, r->path2.x);
    int hi = std::max(r->path1.x, r->path2.x);
    if (hi > lo) targetPos.x = std::clamp(targetPos.x, lo, hi);
  }
  if (targetPos.y <= 0) targetPos.y = pathY;

  auto pushSeg = [&](int tx, int ty) {
    if (curPos.x == tx && curPos.y == ty) return;
    TaskStep w{};
    w.type = TaskStep::Type::WalkToX;
    w.targetX = tx;
    w.targetY = ty;
    w.sneak = sneak;
    q.push_back(w);
    curPos = Point{tx, ty};
  };

  if (curPos.x != targetPos.x) {
    if (curPos.y != pathY) pushSeg(curPos.x, pathY);
    pushSeg(targetPos.x, pathY);
    if (targetPos.y != pathY) pushSeg(targetPos.x, targetPos.y);
  } else {
    pushSeg(targetPos.x, targetPos.y);
  }
}

void GameEngine::queuePathToRoom(const std::string& destRoom, bool sneak) {
  std::string curRoom = woody_.room;
  Point curPos = woody_.pos;
  for (const TaskStep& st : woodyQueue_) {
    if (st.type == TaskStep::Type::TraverseDoor && !st.targetRoom.empty()) {
      curRoom = st.targetRoom;
      curPos = st.outPos;
    } else if (st.type == TaskStep::Type::WalkToX) {
      curPos.x = st.targetX;
      if (st.targetY > 0) curPos.y = st.targetY;
    }
  }
  auto edges = findRoomPath(curRoom, destRoom);
  for (const RoomEdge& e : edges) {
    const ObjectDef* dIn = level_.findObjectDef(e.doorIn);
    const ObjectDef* dOut = level_.findObjectDef(e.doorOut);
    int inIdx = findObjectIndex(e.doorIn, curRoom);
    int outIdx = findObjectIndex(e.doorOut, e.targetRoom);
    Point inPos = dIn ? dIn->hotspotOr("woody", Point{450, 420}) : Point{450, 420};
    if (inIdx >= 0) {
      inPos.x += level_.objects[inIdx].position.x;
      inPos.y += level_.objects[inIdx].position.y;
    }
    Point outPos = dOut ? dOut->hotspotOr("woody_out", dOut->hotspotOr("woody", Point{450, 420})) : Point{450, 420};
    if (outIdx >= 0) {
      outPos.x += level_.objects[outIdx].position.x;
      outPos.y += level_.objects[outIdx].position.y;
    }
    if (const RoomDef* rOut = level_.findRoom(e.targetRoom)) {
      int lo = std::min(rOut->path1.x, rOut->path2.x);
      int hi = std::max(rOut->path1.x, rOut->path2.x);
      if (hi > lo) outPos.x = std::clamp(outPos.x, lo, hi);
    }

    enqueueRoomWalk(woodyQueue_, curRoom, curPos, inPos, sneak);

    // Synchronized door traversal: doorIn (woody_enter) and doorOut (woody_leave) start together at tick 0!
    TaskStep doorStep{};
    doorStep.type = TaskStep::Type::TraverseDoor;
    doorStep.doorName = e.doorIn;
    doorStep.objectIndex = inIdx;
    doorStep.outObjectIndex = outIdx;
    doorStep.targetRoom = e.targetRoom;
    doorStep.outPos = outPos;
    doorStep.outAnim = "ms2";
    int enterFrames = dIn ? animFrameCount(dIn->gfx, "woody_enter", 12) : 12;
    int leaveFrames = dOut ? animFrameCount(dOut->gfx, "woody_leave", 24) : 24;
    if (dIn) {
      if (const ActionDef* aEn = dIn->findAction("enter", "woody")) {
        if (aEn->timeTicks > 0) enterFrames = std::max(enterFrames, aEn->timeTicks);
      }
    }
    if (dOut) {
      if (const ActionDef* aLv = dOut->findAction("leave", "woody")) {
        if (!aLv->actorNextAnim.empty()) doorStep.outAnim = aLv->actorNextAnim;
        if (aLv->timeTicks > 0) leaveFrames = std::max(leaveFrames, aLv->timeTicks);
      }
    }
    doorStep.enterTicks = std::max(1, enterFrames);
    doorStep.totalTicks = std::max(doorStep.enterTicks, leaveFrames);
    woodyQueue_.push_back(doorStep);

    curRoom = e.targetRoom;
    curPos = outPos;
  }
}

void GameEngine::onLeftClick(int worldX, int worldY, int screenX, int screenY, bool doubleClick) {
  if (catchSequenceActive_) return;

  // Check HUD clicks first
  const float hudScale = std::min(float(winW_) / 800.0f, float(winH_) / 480.0f);
  const float hudX = (float(winW_) - 800.0f * hudScale) * 0.5f;
  const float hudY = float(winH_) - 600.0f * hudScale;
  if (screenY >= winH_ - int(134 * hudScale)) {
    int hx = int((screenX - hudX) / hudScale);
    int hy = int((screenY - hudY) / hudScale);
    Rect missionBtn{581 + 167, 471 + 20, 48, 36};
    if (missionBtn.contains(hx, hy)) {
      playSfx("sfx/but1.wav");
      enterScreen(ScreenState::PauseMenu);
      return;
    }
    Rect leftArr{123, 547, 36, 36};
    Rect rightArr{123 + 422, 547, 36, 36};
    if (leftArr.contains(hx, hy)) {
      if (invScroll_ > 0) invScroll_--;
      playSfx("sfx/but1.wav");
      return;
    }
    if (rightArr.contains(hx, hy)) {
      if (invScroll_ + 5 < (int)inventory_.size()) invScroll_++;
      playSfx("sfx/but1.wav");
      return;
    }
    if (hy >= 530) {
      for (int slot = 0; slot < 5; ++slot) {
        int idx = invScroll_ + slot;
        if (idx >= (int)inventory_.size()) break;
        Rect r{123 + 41 + slot * 75, 511 + 26, 60, 55};
        if (r.contains(hx, hy)) {
          selectedInv_ = (selectedInv_ == idx) ? -1 : idx;
          playSfx("sfx/but1.wav");
          return;
        }
      }
      return;
    }
  }

  // Preserve active non-interruptible step (e.g. mid-door traversal or mid-hideout exit/entry or active trick/prank/item pickup)
  if (!woodyQueue_.empty()) {
    const TaskStep& front = woodyQueue_.front();
    bool keepFront = (front.type == TaskStep::Type::TraverseDoor && front.elapsedTicks > 0) ||
                     (front.type == TaskStep::Type::PlayAction && front.nonInterruptible);
    if (keepFront) {
      return;
    }
    if (front.type == TaskStep::Type::PlayAction && front.objectIndex >= 0 &&
          front.objectIndex < (int)level_.objects.size()) {
        PlacedObject& po = level_.objects[front.objectIndex];
        po.currentAnim = "ms";
        po.nextAnim = "ms";
        po.animFrame = 0;
      } else if (front.type == TaskStep::Type::TraverseDoor) {
        if (front.objectIndex >= 0 && front.objectIndex < (int)level_.objects.size()) {
          PlacedObject& doorIn = level_.objects[front.objectIndex];
          doorIn.currentAnim = "ms";
          doorIn.nextAnim = "ms";
          doorIn.animFrame = 0;
        }
        if (front.outObjectIndex >= 0 && front.outObjectIndex < (int)level_.objects.size()) {
          PlacedObject& doorOut = level_.objects[front.outObjectIndex];
          doorOut.currentAnim = "ms";
          doorOut.nextAnim = "ms";
          doorOut.animFrame = 0;
        }
      }
      if (woody_.hidingInObject < 0) {
        woody_.hidden = false;
        if (woody_.currentAnim == "inv") woody_.currentAnim = "ms2";
      }
      woodyQueue_.clear();
      woodyActionTicks_ = woodyActionTotalTicks_ = 0;
  } else {
    woodyActionTicks_ = woodyActionTotalTicks_ = 0;
  }

  // If Woody is currently hidden inside a wardrobe, first step is to leave the hideout
  if (woody_.hidden && woody_.hidingInObject >= 0) {
    bool alreadyLeaving = (!woodyQueue_.empty() && woodyQueue_.front().type == TaskStep::Type::PlayAction &&
                           woodyQueue_.front().leaveHideout);
    if (!alreadyLeaving) {
      const PlacedObject& hpo = level_.objects[woody_.hidingInObject];
      const ObjectDef* hod = level_.findObjectDef(hpo.name);
      int lvFrames = hod ? animFrameCount(hod->gfx, "woody_leave", 20) : 20;
      TaskStep leaveHide{};
      leaveHide.type = TaskStep::Type::PlayAction;
      leaveHide.objectIndex = woody_.hidingInObject;
      leaveHide.actorAnim = "inv";
      leaveHide.actorNextAnim = "ms2";
      leaveHide.objAnim = "woody_leave";
      leaveHide.objNextAnim = "ms";
      leaveHide.durationTicks = lvFrames;
      leaveHide.nonInterruptible = true;
      leaveHide.leaveHideout = true;
      woodyQueue_.push_back(leaveHide);
    }
    if (hoveredObject_ == woody_.hidingInObject) return;
  }

  const std::string heldItem =
      (selectedInv_ >= 0 && selectedInv_ < (int)inventory_.size()) ? inventory_[selectedInv_] : std::string();

  auto currentWoodyEndPos = [&]() {
    Point p = woody_.pos;
    for (const TaskStep& st : woodyQueue_) {
      if (st.type == TaskStep::Type::TraverseDoor) p = st.outPos;
      else if (st.type == TaskStep::Type::WalkToX) {
        p.x = st.targetX;
        if (st.targetY > 0) p.y = st.targetY;
      }
    }
    return p;
  };

  if (hoveredObject_ >= 0) {
    const PlacedObject& po = level_.objects[hoveredObject_];
    const ObjectDef* od = level_.findObjectDef(po.name);

    // Exit door (hal/door_entrance) when minimum quota has been reached finishes the episode!
    if (od && toLowerAscii(od->stdAction) == "end" && quota_ >= level_.info.minQuota) {
      finishEpisode(false, false);
      return;
    }

    // Door click: navigate to the connected room
    if (po.isDoor) {
      selectedInv_ = -1;
      const RoomDef* r = level_.findRoom(po.roomName);
      if (r) {
        for (const RoomEdge& e : r->neighbors) {
          if (toLowerAscii(e.doorIn) == toLowerAscii(po.name)) {
            queuePathToRoom(po.roomName, doubleClick);
            queuePathToRoom(e.targetRoom, doubleClick);
            return;
          }
        }
      }
    }

    // Check combination (with held item, or bare-hand trick without item)
    const CombinationDef* combo = nullptr;
    if (!heldItem.empty()) {
      combo = findCombination(po.name, heldItem);
      if (!combo || combo->isWrong) {
        playSfx("sfx/wod_nono.wav");
        statusBanner_ = "Эту вещь здесь не применить!";
        bannerTicks_ = kTicksPerSecond * 2;
        selectedInv_ = -1;
        return;
      }
    } else {
      combo = findCombination(po.name, "");
    }

    Point targetPos = od ? od->hotspotOr("woody", Point{450, 420}) : Point{450, 420};
    targetPos.x += po.position.x;
    targetPos.y += po.position.y;

    queuePathToRoom(po.roomName, doubleClick);
    Point curPos = currentWoodyEndPos();
    enqueueRoomWalk(woodyQueue_, po.roomName, curPos, targetPos, doubleClick);

    if (combo && !combo->isWrong) {
      TaskStep apply{};
      apply.type = TaskStep::Type::ApplyCombination;
      apply.objectIndex = hoveredObject_;
      apply.combo = *combo;
      woodyQueue_.push_back(apply);
      selectedInv_ = -1;
    } else {
      TaskStep inter{};
      inter.type = TaskStep::Type::InteractObject;
      inter.objectIndex = hoveredObject_;
      woodyQueue_.push_back(inter);
    }
    return;
  }

  // Floor / Room click
  if (!hoveredRoom_.empty()) {
    const RoomDef* r = level_.findRoom(hoveredRoom_);
    if (!r) return;
    int localX = std::clamp(worldX - r->offset.x, std::min(r->path1.x, r->path2.x), std::max(r->path1.x, r->path2.x));
    int pathY = r->path1.y > 0 ? r->path1.y : 420;

    if (!heldItem.empty()) {
      const CombinationDef* combo = findCombination(hoveredRoom_, heldItem);
      if (combo && !combo->isWrong) {
        queuePathToRoom(hoveredRoom_, doubleClick);
        Point curPos = currentWoodyEndPos();
        enqueueRoomWalk(woodyQueue_, hoveredRoom_, curPos, Point{localX, pathY}, doubleClick);

        TaskStep apply{};
        apply.type = TaskStep::Type::ApplyCombination;
        apply.targetRoom = hoveredRoom_;
        apply.targetX = localX;
        apply.combo = *combo;
        woodyQueue_.push_back(apply);
        selectedInv_ = -1;
        return;
      } else {
        playSfx("sfx/wod_nono.wav");
        selectedInv_ = -1;
        return;
      }
    }

    queuePathToRoom(hoveredRoom_, doubleClick);
    Point curPos = currentWoodyEndPos();
    enqueueRoomWalk(woodyQueue_, hoveredRoom_, curPos, Point{localX, pathY}, doubleClick);
  }
}

void GameEngine::onRightClick(int worldX, int worldY, int screenX, int screenY) {
  if (selectedInv_ >= 0) {
    selectedInv_ = -1;
    return;
  }
  onLeftClick(worldX, worldY, screenX, screenY, true);
}

// ---------------------------------------------------------------------------
// Simulation (12 Hz original game tick)
// ---------------------------------------------------------------------------

void GameEngine::advanceObjectAnims() {
  for (PlacedObject& po : level_.objects) {
    if (!po.visible || po.currentAnim == "inv") continue;
    const ObjectDef* od = level_.findObjectDef(po.name);
    const std::string gfxName = od ? od->gfx : po.name;
    const GfxObjectDef* gd = level_.findGfxObject(gfxName);
    if (!gd) continue;
    auto it = gd->anims.find(toLowerAscii(po.currentAnim));
    if (it == gd->anims.end() || it->second.frames.empty()) continue;
    const AnimationDef& anim = it->second;
    if (po.animFrame < (int)anim.frames.size() && !anim.frames[po.animFrame].sfx.empty()) {
      playSfx(anim.frames[po.animFrame].sfx);
    }
    po.animFrame++;
    if (po.animFrame >= (int)anim.frames.size()) {
      if (anim.loop) {
        po.animFrame = 0;
      } else {
        if (po.isDoor || po.currentAnim == "woody_enter" || po.currentAnim == "woody_leave") {
          po.currentAnim = "ms";
          po.nextAnim = "ms";
          po.animFrame = 0;
        } else {
          po.animFrame = (int)anim.frames.size() - 1;
        }
      }
    }
  }
}

void GameEngine::advanceActorAnim(ActorState& a, bool* finishedOut) {
  if (finishedOut) *finishedOut = false;
  if (a.hidden || a.currentAnim == "inv") {
    if (finishedOut) *finishedOut = true;
    return;
  }
  const GfxObjectDef* gd = level_.findGfxObject(a.name);
  if (!gd) {
    if (finishedOut) *finishedOut = true;
    return;
  }
  auto it = gd->anims.find(toLowerAscii(a.currentAnim));
  if (it == gd->anims.end() || it->second.frames.empty()) {
    if (finishedOut) *finishedOut = true;
    return;
  }
  const AnimationDef& anim = it->second;
  if (a.animFrame < (int)anim.frames.size() && !anim.frames[a.animFrame].sfx.empty()) {
    playSfx(anim.frames[a.animFrame].sfx);
  }
  a.animFrame++;
  if (a.animFrame >= (int)anim.frames.size()) {
    if (anim.loop) {
      a.animFrame = 0;
    } else {
      a.animFrame = (int)anim.frames.size() - 1;
      if (finishedOut) *finishedOut = true;
    }
  }
}

void GameEngine::updateWoody() {
  bool animDone = false;
  advanceActorAnim(woody_, &animDone);

  while (!woodyQueue_.empty()) {
    TaskStep& step = woodyQueue_.front();

    switch (step.type) {
      case TaskStep::Type::WalkToX: {
        woodyBusyInAnim_ = false;
        if (woody_.hidingInObject < 0 && woody_.hidden) {
          woody_.hidden = false;
        }
        int dx = step.targetX - woody_.pos.x;
        int dy = (step.targetY > 0 ? step.targetY : woody_.pos.y) - woody_.pos.y;
        if (dx == 0 && dy == 0) {
          woodyQueue_.pop_front();
          if (woodyQueue_.empty()) {
            woody_.currentAnim = "ms2";
            woody_.nextAnim = "ms2";
            woody_.animFrame = 0;
          }
          continue;
        }
        if (std::abs(dx) > 0) {
          std::string want = (dx > 0) ? (step.sneak ? "sn1" : "mg1") : (step.sneak ? "sn3" : "mg3");
          int speed = actorSpeed("woody", want, step.sneak ? 5 : 17);
          if (woody_.currentAnim != want) {
            woody_.currentAnim = want;
            woody_.nextAnim = want;
            woody_.animFrame = 0;
          }
          if (std::abs(dx) <= speed) {
            woody_.pos.x = step.targetX;
            if (dy == 0) {
              std::string stopAnim = (dx > 0) ? "ms1" : "ms3";
              woodyQueue_.pop_front();
              if (woodyQueue_.empty()) {
                woody_.currentAnim = stopAnim;
                woody_.nextAnim = stopAnim;
                woody_.animFrame = 0;
              }
              continue;
            }
          } else {
            woody_.pos.x += (dx > 0 ? speed : -speed);
          }
        } else {
          std::string want = (dy < 0) ? (step.sneak ? "sn0" : "mg0") : (step.sneak ? "sn2" : "mg2");
          int speed = actorSpeed("woody", want, step.sneak ? 2 : 6);
          if (woody_.currentAnim != want) {
            woody_.currentAnim = want;
            woody_.nextAnim = want;
            woody_.animFrame = 0;
          }
          if (std::abs(dy) <= speed) {
            woody_.pos.y = step.targetY;
            std::string stopAnim = (dy < 0) ? "ms0" : "ms2";
            woodyQueue_.pop_front();
            if (woodyQueue_.empty()) {
              woody_.currentAnim = stopAnim;
              woody_.nextAnim = stopAnim;
              woody_.animFrame = 0;
            }
            continue;
          } else {
            woody_.pos.y += (dy > 0 ? speed : -speed);
          }
        }
        return;
      }

      case TaskStep::Type::TraverseDoor: {
        if (step.elapsedTicks >= step.totalTicks) {
          if (step.objectIndex >= 0 && step.objectIndex < (int)level_.objects.size()) {
            PlacedObject& doorIn = level_.objects[step.objectIndex];
            doorIn.currentAnim = "ms";
            doorIn.nextAnim = "ms";
            doorIn.animFrame = 0;
          }
          if (step.outObjectIndex >= 0 && step.outObjectIndex < (int)level_.objects.size()) {
            PlacedObject& doorOut = level_.objects[step.outObjectIndex];
            doorOut.currentAnim = "ms";
            doorOut.nextAnim = "ms";
            doorOut.animFrame = 0;
          }
          woody_.room = step.targetRoom;
          woody_.pos = step.outPos;
          woody_.hidden = false;
          woody_.currentAnim = step.outAnim;
          woody_.nextAnim = step.outAnim;
          woody_.animFrame = 0;
          woodyQueue_.pop_front();
          continue;
        }
        if (step.elapsedTicks == 0) {
          woody_.hidden = true;
          if (step.objectIndex >= 0 && step.objectIndex < (int)level_.objects.size()) {
            PlacedObject& doorIn = level_.objects[step.objectIndex];
            doorIn.currentAnim = "woody_enter";
            doorIn.nextAnim = "ms";
            doorIn.animFrame = 0;
          }
          if (step.outObjectIndex >= 0 && step.outObjectIndex < (int)level_.objects.size()) {
            PlacedObject& doorOut = level_.objects[step.outObjectIndex];
            doorOut.currentAnim = "woody_leave";
            doorOut.nextAnim = "ms";
            doorOut.animFrame = 0;
          }
        }
        step.elapsedTicks++;
        if (step.elapsedTicks == step.enterTicks) {
          woody_.room = step.targetRoom;
          woody_.pos = step.outPos;
        }
        return;
      }

      case TaskStep::Type::PlayAction: {
        if (step.elapsedTicks >= step.durationTicks) {
          if (step.showProgressBar) {
            woodyActionTicks_ = woodyActionTotalTicks_ = 0;
          }
          const bool resultIsInv = step.applyComboOnFinish &&
                                   level_.inventar.count(toLowerAscii(step.combo.resultObject)) > 0;
          if (step.objectIndex >= 0 && step.objectIndex < (int)level_.objects.size()) {
            PlacedObject& po = level_.objects[step.objectIndex];
            if (!step.objNextAnim.empty()) {
              po.currentAnim = step.objNextAnim;
              po.nextAnim = step.objNextAnim;
              po.animFrame = 0;
            }
            if (step.giveContainerItems && !po.items.empty()) {
              std::string gained;
              for (const std::string& it : po.items) {
                if (std::find(inventory_.begin(), inventory_.end(), it) == inventory_.end()) {
                  inventory_.push_back(it);
                }
                std::string n = level_.getString(it, "name");
                if (!gained.empty()) gained += ", ";
                gained += n.empty() ? it : n;
              }
              po.items.clear();
              playSfx("sfx/give_take2.wav");
              statusBanner_ = "Получено: " + gained;
              bannerTicks_ = kTicksPerSecond * 3;
            }
            if (step.applyComboOnFinish && !resultIsInv && step.combo.removeTarget) {
              po.name = step.combo.resultObject;
              po.currentAnim = "ms";
              po.nextAnim = "ms";
              po.animFrame = 0;
              po.items.clear();
              if (const ObjectDef* newOd = level_.findObjectDef(po.name)) {
                for (const auto& [itemName, cnt] : newOd->contents) {
                  for (int i = 0; i < cnt; ++i) po.items.push_back(itemName);
                }
              }
            }
          } else if (step.applyComboOnFinish && !resultIsInv && !step.targetRoom.empty()) {
            PlacedObject trap;
            trap.name = step.combo.resultObject;
            trap.roomName = step.targetRoom;
            trap.layer = 2;
            trap.position = Point{step.targetX, 415};
            trap.visible = true;
            level_.objects.push_back(std::move(trap));
          }

          if (step.applyComboOnFinish) {
            if (!step.combo.item.empty() && step.combo.removeItem) {
              auto it = std::find(inventory_.begin(), inventory_.end(), step.combo.item);
              if (it != inventory_.end()) inventory_.erase(it);
            }
            if (resultIsInv) {
              if (std::find(inventory_.begin(), inventory_.end(), step.combo.resultObject) == inventory_.end()) {
                inventory_.push_back(step.combo.resultObject);
              }
            }
            std::string trickTitle = level_.getString(step.combo.resultObject, "name");
            statusBanner_ = (step.combo.isTrick ? "Пакость готова: " : "Готово: ") +
                            (trickTitle.empty() ? step.combo.resultObject : trickTitle) + "!";
            bannerTicks_ = kTicksPerSecond * 3;
          }

          if (step.enterHideout) {
            woody_.hidden = true;
            woody_.hidingInObject = step.objectIndex;
            statusBanner_ = "Вуди спрятался! Кликните в любое место, чтобы выйти.";
            bannerTicks_ = kTicksPerSecond * 3;
          } else if (step.leaveHideout) {
            woody_.hidden = false;
            woody_.hidingInObject = -1;
            woody_.currentAnim = step.actorNextAnim.empty() ? "ms2" : step.actorNextAnim;
            woody_.nextAnim = woody_.currentAnim;
            woody_.animFrame = 0;
          } else {
            woody_.hidden = false;
            woody_.currentAnim = step.actorNextAnim.empty() ? "ms2" : step.actorNextAnim;
            woody_.nextAnim = woody_.currentAnim;
            woody_.animFrame = 0;
          }
          woodyQueue_.pop_front();
          continue;
        }

        if (step.elapsedTicks == 0) {
          if (step.actorAnim == "inv") {
            woody_.hidden = true;
            woody_.currentAnim = "inv";
          } else if (!step.actorAnim.empty()) {
            woody_.hidden = false;
            woody_.currentAnim = step.actorAnim;
            woody_.nextAnim = step.actorNextAnim;
            woody_.animFrame = 0;
          }
          if (step.objectIndex >= 0 && step.objectIndex < (int)level_.objects.size() && !step.objAnim.empty()) {
            PlacedObject& po = level_.objects[step.objectIndex];
            po.currentAnim = step.objAnim;
            po.nextAnim = step.objNextAnim;
            po.animFrame = 0;
          }
          if (step.showProgressBar) {
            woodyActionTotalTicks_ = std::max(1, step.durationTicks);
            woodyActionTicks_ = woodyActionTotalTicks_;
          }
        }
        step.elapsedTicks++;
        if (step.showProgressBar && woodyActionTicks_ > 0) woodyActionTicks_--;
        return;
      }

      case TaskStep::Type::InteractObject: {
        int idx = step.objectIndex;
        woodyQueue_.pop_front();
        if (idx < 0 || idx >= (int)level_.objects.size()) continue;
        PlacedObject& po = level_.objects[idx];
        const ObjectDef* od = level_.findObjectDef(po.name);
        if (!od) continue;

        if (od->hasFlag("hideout")) {
          int enFrames = animFrameCount(od->gfx, "woody_enter", 20);
          TaskStep hideStep{};
          hideStep.type = TaskStep::Type::PlayAction;
          hideStep.objectIndex = idx;
          hideStep.actorAnim = "inv";
          hideStep.objAnim = "woody_enter";
          hideStep.objNextAnim = "ms";
          hideStep.durationTicks = enFrames;
          hideStep.nonInterruptible = true;
          hideStep.enterHideout = true;
          woodyQueue_.push_front(hideStep);
        } else if (od->hasFlag("container") || !po.items.empty()) {
          const ActionDef* aOpen = od->findAction("open", "woody");
          const ActionDef* aTake = od->findAction("take", "woody");
          const ActionDef* aClose = od->findAction("close", "woody");

          std::string openActorAnim = (aOpen && !aOpen->actorAnim.empty()) ? aOpen->actorAnim : "";
          std::string openObjAnim = (aOpen && !aOpen->objAnim.empty()) ? aOpen->objAnim : "";
          std::string takeActorAnim = (aTake && !aTake->actorAnim.empty()) ? aTake->actorAnim : "take0";
          std::string closeActorAnim = (aClose && !aClose->actorAnim.empty()) ? aClose->actorAnim : "";
          std::string finalNextAnim = (aClose && !aClose->actorNextAnim.empty())
                                          ? aClose->actorNextAnim
                                          : ((aTake && !aTake->actorNextAnim.empty()) ? aTake->actorNextAnim : "ms0");

          std::vector<TaskStep> seq;
          if (!openActorAnim.empty() || !openObjAnim.empty()) {
            TaskStep stOpen{};
            stOpen.type = TaskStep::Type::PlayAction;
            stOpen.objectIndex = idx;
            stOpen.actorAnim = openActorAnim;
            stOpen.actorNextAnim = finalNextAnim;
            stOpen.objAnim = openObjAnim;
            stOpen.objNextAnim = openObjAnim.empty() ? "ms" : openObjAnim;
            stOpen.durationTicks = animFrameCount("woody", openActorAnim, 10);
            seq.push_back(stOpen);
          }
          if (!po.items.empty()) {
            TaskStep stTake{};
            stTake.type = TaskStep::Type::PlayAction;
            stTake.objectIndex = idx;
            stTake.actorAnim = takeActorAnim;
            stTake.actorNextAnim = finalNextAnim;
            stTake.objAnim = openObjAnim;
            stTake.objNextAnim = openObjAnim.empty() ? "ms" : openObjAnim;
            stTake.durationTicks = animFrameCount("woody", takeActorAnim, 12);
            stTake.giveContainerItems = true;
            stTake.nonInterruptible = true;
            seq.push_back(stTake);
          } else {
            TaskStep stDecline{};
            stDecline.type = TaskStep::Type::PlayAction;
            stDecline.objectIndex = idx;
            stDecline.actorAnim = "decline";
            stDecline.actorNextAnim = finalNextAnim;
            stDecline.objAnim = openObjAnim;
            stDecline.objNextAnim = openObjAnim.empty() ? "ms" : openObjAnim;
            stDecline.durationTicks = animFrameCount("woody", "decline", 19);
            seq.push_back(stDecline);
            statusBanner_ = "Пусто!";
            bannerTicks_ = kTicksPerSecond * 2;
          }
          if (!closeActorAnim.empty() || !openObjAnim.empty()) {
            TaskStep stClose{};
            stClose.type = TaskStep::Type::PlayAction;
            stClose.objectIndex = idx;
            stClose.actorAnim = closeActorAnim.empty() ? "close" : closeActorAnim;
            stClose.actorNextAnim = finalNextAnim;
            stClose.objAnim = "ms";
            stClose.objNextAnim = "ms";
            stClose.durationTicks = animFrameCount("woody", stClose.actorAnim, 10);
            seq.push_back(stClose);
          }
          for (auto it = seq.rbegin(); it != seq.rend(); ++it) woodyQueue_.push_front(*it);
        } else {
          std::string desc = level_.getString(po.name, "description");
          if (desc.empty()) desc = level_.getString(po.name, "name");
          if (!desc.empty()) {
            statusBanner_ = desc;
            bannerTicks_ = kTicksPerSecond * 3;
          }
          TaskStep lookStep{};
          lookStep.type = TaskStep::Type::PlayAction;
          lookStep.actorAnim = "smile";
          lookStep.actorNextAnim = "ms2";
          lookStep.durationTicks = animFrameCount("woody", "smile", 14);
          woodyQueue_.push_front(lookStep);
        }
        continue;
      }

      case TaskStep::Type::ApplyCombination: {
        CombinationDef combo = step.combo;
        int idx = step.objectIndex;
        std::string roomName = step.targetRoom;
        int dropX = step.targetX;
        woodyQueue_.pop_front();

        std::string actActorAnim = "use_mid";
        std::string actNextAnim = "smile";
        std::string actObjAnim;
        int actTicks = 12;

        if (idx >= 0 && idx < (int)level_.objects.size()) {
          const PlacedObject& po = level_.objects[idx];
          if (const ObjectDef* od = level_.findObjectDef(po.name)) {
            const ActionDef* a = !combo.item.empty() ? od->findAction(combo.item, "woody") : nullptr;
            if (!a) a = od->findAction("use", "woody");
            if (!a) {
              for (const ActionDef& cand : od->actions) {
                if (toLowerAscii(cand.actor) == "woody") {
                  a = &cand;
                  break;
                }
              }
            }
            if (a) {
              if (!a->actorAnim.empty()) actActorAnim = a->actorAnim;
              if (!a->actorNextAnim.empty()) actNextAnim = a->actorNextAnim;
              if (!a->objAnim.empty()) actObjAnim = a->objAnim;
              if (a->timeTicks > 0) {
                actTicks = a->timeTicks;
              } else if (!actObjAnim.empty() && actObjAnim != "ms") {
                actTicks = animFrameCount(od->gfx, actObjAnim, 16);
              } else {
                actTicks = animFrameCount("woody", actActorAnim, 14);
              }
            }
          }
        }

        playSfx("sfx/use1.wav");

        TaskStep smileStep{};
        smileStep.type = TaskStep::Type::PlayAction;
        smileStep.actorAnim = actNextAnim;
        smileStep.actorNextAnim = "ms2";
        smileStep.durationTicks = animFrameCount("woody", actNextAnim, 14);

        TaskStep doCombo{};
        doCombo.type = TaskStep::Type::PlayAction;
        doCombo.objectIndex = idx;
        doCombo.targetRoom = roomName;
        doCombo.targetX = dropX;
        doCombo.actorAnim = actActorAnim;
        doCombo.actorNextAnim = actNextAnim;
        doCombo.objAnim = actObjAnim;
        doCombo.objNextAnim = "ms";
        doCombo.durationTicks = std::max(1, actTicks);
        doCombo.showProgressBar = true;
        doCombo.nonInterruptible = true;
        doCombo.applyComboOnFinish = true;
        doCombo.combo = combo;

        woodyQueue_.push_front(smileStep);
        woodyQueue_.push_front(doCombo);
        continue;
      }
    }
  }
}

void GameEngine::awardPrankScore(const std::string& objName, const TrickDef& trick) {
  int bonus = (angerTicks_ > 0) ? 5 : 0;
  lastQuotaGain_ = trick.quota + bonus;
  quota_ = std::clamp(quota_ + lastQuotaGain_, 0, 100);
  tricksDone_ = std::min(level_.info.reachable, tricksDone_ + 1);
  angerMaxTicks_ = std::max(1, trick.angryTime > 0 ? trick.angryTime : level_.defaultAngryTime);
  angerTicks_ = angerMaxTicks_;

  playSfx(bonus > 0 ? "sfx/verybig1.wav" : "sfx/small1.wav");
  if (!woody_.hidden && woodyQueue_.empty()) {
    TaskStep tr{};
    tr.type = TaskStep::Type::PlayAction;
    tr.actorAnim = "triumph";
    tr.actorNextAnim = "ms2";
    tr.durationTicks = animFrameCount("woody", "triumph", 18);
    woodyQueue_.push_back(tr);
  }

  std::string trickTitle = level_.getString(objName, "name");
  statusBanner_ = "ПАКОСТЬ СРАБОТАЛА! +" + std::to_string(lastQuotaGain_) + "% (" +
                  (trickTitle.empty() ? objName : trickTitle) + ")";
  bannerTicks_ = kTicksPerSecond * 4;
}

void GameEngine::enqueueNeighborObjectActions(int objIdx) {
  if (objIdx < 0 || objIdx >= (int)level_.objects.size()) return;
  const PlacedObject& po = level_.objects[objIdx];
  const ObjectDef* od = level_.findObjectDef(po.name);
  if (!od) return;

  const std::string lowerName = toLowerAscii(po.name);
  auto trickIt = level_.tricks.find(lowerName);
  const bool isTricked = (trickIt != level_.tricks.end());
  const std::string origName = isTricked ? objectOriginalName(po.name) : std::string();

  auto pushStep = [&](int targetIdx, const std::string& actAnim, const std::string& actNext,
                      const std::string& obAnim, const std::string& obNext, int fallbackTicks = 24,
                      const std::string& transformTo = "", bool scoreTrick = false,
                      const TrickDef* trickPtr = nullptr, int secObjIdx = -1) {
    TaskStep st{};
    st.type = TaskStep::Type::PlayAction;
    st.objectIndex = targetIdx;
    st.outObjectIndex = secObjIdx;
    st.actorAnim = actAnim;
    st.actorNextAnim = actNext;
    st.objAnim = obAnim;
    st.objNextAnim = obNext;
    const ObjectDef* tOd = (targetIdx >= 0 && targetIdx < (int)level_.objects.size())
                                ? level_.findObjectDef(level_.objects[targetIdx].name)
                                : od;
    std::string gfxName = (tOd && !tOd->gfx.empty()) ? tOd->gfx : (od ? od->gfx : "");
    if (actAnim == "inv" && !obAnim.empty()) {
      st.durationTicks = animFrameCount(gfxName, obAnim, fallbackTicks);
    } else {
      st.durationTicks = animFrameCount("neighbor", actAnim, fallbackTicks);
    }
    st.transformObjectTo = transformTo;
    if (scoreTrick && trickPtr) {
      st.triggerPrankScore = true;
      st.trick = *trickPtr;
    }
    neighborQueue_.push_back(st);
  };

  // Special multi-phase handling for armchair (lir/sofa, lir/sofa_fartbag, lir/sofa_broken)
  if (lowerName == "lir/sofa" || lowerName == "lir/sofa_fartbag" || lowerName == "lir/sofa_broken") {
    TaskStep enterSt{};
    enterSt.type = TaskStep::Type::PlayAction;
    enterSt.objectIndex = objIdx;
    enterSt.actorAnim = "inv";
    enterSt.actorNextAnim = "ms3";
    enterSt.objAnim = "neighbor_enter";
    enterSt.objNextAnim = isTricked ? (lowerName == "lir/sofa_broken" ? "neighbor_fall" : "neighbor_surprise") : "neighbor_sit_remo";
    enterSt.durationTicks = animFrameCount(od->gfx, "neighbor_enter", 6);
    neighborQueue_.push_back(enterSt);

    if (isTricked) {
      std::string surpAnim = (lowerName == "lir/sofa_broken") ? "neighbor_fall" : "neighbor_surprise";
      TaskStep surpSt{};
      surpSt.type = TaskStep::Type::PlayAction;
      surpSt.objectIndex = objIdx;
      surpSt.actorAnim = "inv";
      surpSt.actorNextAnim = "ms3";
      surpSt.objAnim = surpAnim;
      surpSt.objNextAnim = "ms";
      surpSt.durationTicks = animFrameCount(od->gfx, surpAnim, 36);
      surpSt.triggerPrankScore = true;
      surpSt.trick = trickIt->second;
      surpSt.transformObjectTo = origName.empty() ? "lir/sofa" : origName;
      neighborQueue_.push_back(surpSt);
    } else {
      TaskStep remoSt{};
      remoSt.type = TaskStep::Type::PlayAction;
      remoSt.objectIndex = objIdx;
      remoSt.actorAnim = "inv";
      remoSt.objAnim = "neighbor_sit_remo";
      remoSt.objNextAnim = "neighbor_sit";
      remoSt.durationTicks = animFrameCount(od->gfx, "neighbor_sit_remo", 27);
      neighborQueue_.push_back(remoSt);

      // If TV antenna was twisted (lir/twistedantenna exists in lir), Neighbor gets up immediately to fix it!
      int tvTrickIdx = findObjectIndex("lir/twistedantenna", po.roomName);
      if (tvTrickIdx < 0) {
        TaskStep sitSt{};
        sitSt.type = TaskStep::Type::PlayAction;
        sitSt.objectIndex = objIdx;
        sitSt.actorAnim = "inv";
        sitSt.objAnim = "neighbor_sit";
        sitSt.objNextAnim = "neighbor_leave";
        sitSt.durationTicks = animFrameCount(od->gfx, "neighbor_sit", 32);
        neighborQueue_.push_back(sitSt);
      }

      TaskStep leaveSt{};
      leaveSt.type = TaskStep::Type::PlayAction;
      leaveSt.objectIndex = objIdx;
      leaveSt.actorAnim = "inv";
      leaveSt.actorNextAnim = "ms3";
      leaveSt.objAnim = "neighbor_leave";
      leaveSt.objNextAnim = "ms";
      leaveSt.durationTicks = animFrameCount(od->gfx, "neighbor_leave", 6);
      neighborQueue_.push_back(leaveSt);
    }

    // After getting up from the sofa, if lir/twistedantenna is active in the room, walk over and repair it!
    int tvTrickIdx = findObjectIndex("lir/twistedantenna", po.roomName);
    if (tvTrickIdx >= 0) {
      const PlacedObject& tvPo = level_.objects[tvTrickIdx];
      const ObjectDef* tvOd = level_.findObjectDef(tvPo.name);
      Point tvHs = tvOd ? tvOd->hotspotOr("neighbor", Point{200, 420}) : Point{200, 420};
      tvHs.x += tvPo.position.x;
      tvHs.y += tvPo.position.y;
      Point curPos = neighbor_.pos;
      enqueueRoomWalk(neighborQueue_, po.roomName, curPos, tvHs, false);
      TaskStep fixTv{};
      fixTv.type = TaskStep::Type::InteractObject;
      fixTv.objectIndex = tvTrickIdx;
      neighborQueue_.push_back(fixTv);
    }
    return;
  }

  // Bed & Alarm Clock sequence on level_pig (bed/bed, bed/bed_pins + bed/alarmclock, bed/cactusclock)
  if (lowerName == "bed/bed_pins") {
    pushStep(objIdx, "inv", "ms1", "enter", "ms", 36, "bed/bed", isTricked, isTricked ? &trickIt->second : nullptr);
    pushStep(objIdx, "use_mid", "ms0", "", "ms", 24);
    return;
  }
  if (lowerName == "bed/bed") {
    int cactusIdx = findObjectIndex("bed/cactusclock", po.roomName);
    int alarmIdx = (cactusIdx >= 0) ? cactusIdx : findObjectIndex("bed/alarmclock", po.roomName);
    pushStep(objIdx, "inv", "inv", "enter", "sleep", 12);
    pushStep(objIdx, "inv", "inv", "sleep", "sleep", 36);
    if (cactusIdx >= 0) {
      auto cTrick = level_.tricks.find("bed/cactusclock");
      pushStep(objIdx, "inv", "inv", "hit_cactus", "leave", 36, "",
               cTrick != level_.tricks.end(), cTrick != level_.tricks.end() ? &cTrick->second : nullptr, cactusIdx);
    } else {
      pushStep(objIdx, "inv", "inv", "hit_alarm", "leave", 24, "", false, nullptr, alarmIdx);
    }
    pushStep(objIdx, "inv", "ms1", "leave", "ms", 18);
    return;
  }

  // Teeth glass, baby bottle, and parrot on level_pig
  if (lowerName == "bed/teeth_empty") {
    pushStep(objIdx, "give_short", "ms3", "", "ms", 12, "bed/teeth");
    return;
  }
  if (lowerName == "bed/teeth") {
    pushStep(objIdx, "take_short", "give_short", "", "ms", 12);
    pushStep(objIdx, "give_short", "ms3", "", "ms", 12);
    return;
  }
  if (lowerName == "bed/teeth_tabasco") {
    pushStep(objIdx, "take_short", "spit_fire", "", "ms", 12);
    pushStep(objIdx, "spit_fire", "ms1", "", "ms", 36, "bed/teeth", isTricked, isTricked ? &trickIt->second : nullptr);
    return;
  }
  if (lowerName == "kit/babybottle") {
    pushStep(objIdx, "take", "shake_bottle", "inv", "inv", 16);
    pushStep(objIdx, "shake_bottle", "give", "inv", "inv", 24);
    pushStep(objIdx, "give", "ms1", "ms", "ms", 16);
    return;
  }
  if (lowerName == "kit/babybottle_nitro") {
    pushStep(objIdx, "take", "shake_bottle_expo", "inv", "inv", 16);
    pushStep(objIdx, "shake_bottle_expo", "ms2", "ms", "ms", 36, "kit/babybottle", isTricked, isTricked ? &trickIt->second : nullptr);
    return;
  }
  if (lowerName == "chili") {
    pushStep(objIdx, "give", "ms1", "eat_cookie", "sleep", 24);
    return;
  }

  // Tub, Shower, Towel & Toilet on level_bath / level_piano
  if (lowerName == "toi/tub_empty" || lowerName == "toi/tub" || lowerName == "toi/tub_hair") {
    int shIdx = findObjectIndex("toi/shower", po.roomName);
    int dirtyTowelIdx = findObjectIndex("toi/dirtytowel", po.roomName);
    int cleanTowelIdx = findObjectIndex("toi/towel", po.roomName);
    if (lowerName == "toi/tub_empty") {
      pushStep(objIdx, "give_short", "ms3", "", "ms", 12, "toi/tub");
    } else if (lowerName == "toi/tub_hair") {
      pushStep(objIdx, "take_short", "inv", "", "ms", 12, "toi/tub", isTricked, isTricked ? &trickIt->second : nullptr);
    } else {
      pushStep(objIdx, "take_short", "inv", "", "ms", 12);
    }
    if (shIdx >= 0) {
      if (lowerName == "toi/tub_hair") {
        pushStep(shIdx, "inv", "ms2", "shower", "ms", 36);
        pushStep(objIdx, "show_hair", "inv", "", "ms", 30);
      } else {
        pushStep(shIdx, "inv", "inv", "shower", "take_towel", 36);
      }
      pushStep(shIdx, "inv", "inv", "take_towel", "dry", 12, "", false, nullptr,
               dirtyTowelIdx >= 0 ? dirtyTowelIdx : cleanTowelIdx);
      pushStep(shIdx, "inv", "ms1", "dry", "leave", 24);
      if (dirtyTowelIdx >= 0) {
        auto dtTrick = level_.tricks.find("toi/dirtytowel");
        pushStep(dirtyTowelIdx, "show_black", "inv", "ms", "ms", 30, "toi/towel",
                 dtTrick != level_.tricks.end(), dtTrick != level_.tricks.end() ? &dtTrick->second : nullptr);
      }
      pushStep(shIdx, "inv", "ms1", "leave", "ms", 16);
    }
    return;
  }
  if (lowerName == "toi/toilet") {
    pushStep(objIdx, "pee", "ms0", "", "ms", 36);
    return;
  }
  if (lowerName == "lir/score") {
    pushStep(objIdx, "inv", "ms3", "play", "ms", 45);
    return;
  }
  if (lowerName == "lir/manipulatedscore") {
    pushStep(objIdx, "inv", "inv", "play", "playslow_angry", 30);
    pushStep(objIdx, "inv", "ms2", "playslow_angry", "ms", 36, "lir/score", isTricked, isTricked ? &trickIt->second : nullptr);
    return;
  }

  // Season 1 Finale (level_suntan) objects
  if (lowerName == "toi/toothbrushset") {
    pushStep(objIdx, "take3", "brush_teeth", "", "ms", 12);
    pushStep(objIdx, "brush_teeth", "ms2", "", "ms", 30);
    return;
  }
  if (lowerName == "toi/shoebrushset") {
    pushStep(objIdx, "take3", "brush_black_teeth", "", "ms", 12);
    pushStep(objIdx, "brush_black_teeth", "show_black_teeth", "", "ms", 24);
    pushStep(objIdx, "show_black_teeth", "ms2", "", "ms", 30, "toi/toothbrushset", isTricked, isTricked ? &trickIt->second : nullptr);
    return;
  }
  if (lowerName == "kit/coffeebox") {
    pushStep(objIdx, "use_mid", "drink_coffee", "brew", "ms", 20);
    pushStep(objIdx, "drink_coffee", "ms2", "ms", "ms", 28);
    return;
  }
  if (lowerName == "kit/coffeebox_soil") {
    pushStep(objIdx, "use_mid", "drink_coffee_soil", "brew", "ms", 20);
    pushStep(objIdx, "drink_coffee_soil", "spit", "ms", "ms", 24);
    pushStep(objIdx, "spit", "ms1", "ms", "ms", 30, "kit/coffeebox", isTricked, isTricked ? &trickIt->second : nullptr);
    return;
  }
  if (lowerName == "bal/ewer" || lowerName == "bal/ewer_poisoned" ||
      lowerName == "bal/suncream" || lowerName == "bal/suncream_honey" || lowerName == "bal/suncream_oil") {
    pushStep(objIdx, "take3", "ms1", "", "ms", 14);
    return;
  }
  if (lowerName == "anc/flower") {
    int poisIdx = findObjectIndex("bal/ewer_poisoned", "bal");
    if (poisIdx >= 0) {
      auto eTrick = level_.tricks.find("bal/ewer_poisoned");
      pushStep(objIdx, "water_to_death", "ms2", "die", "ms", 36, "",
               eTrick != level_.tricks.end(), eTrick != level_.tricks.end() ? &eTrick->second : nullptr, poisIdx);
    } else {
      pushStep(objIdx, "water", "ms3", "water", "ms", 28);
    }
    return;
  }
  if (lowerName == "bal/foldingchair") {
    int honeyIdx = findObjectIndex("bal/suncream_honey", "bal");
    int oilIdx = findObjectIndex("bal/suncream_oil", "bal");
    if (honeyIdx >= 0) {
      auto hTrick = level_.tricks.find("bal/suncream_honey");
      pushStep(objIdx, "inv", "inv", "sunbath_cream", "beeattack", 28);
      pushStep(objIdx, "inv", "ms2", "beeattack", "ms", 36, "",
               hTrick != level_.tricks.end(), hTrick != level_.tricks.end() ? &hTrick->second : nullptr, honeyIdx);
    } else if (oilIdx >= 0) {
      auto oTrick = level_.tricks.find("bal/suncream_oil");
      pushStep(objIdx, "inv", "ms1", "sunbath_oil", "ms", 36, "",
               oTrick != level_.tricks.end(), oTrick != level_.tricks.end() ? &oTrick->second : nullptr, oilIdx);
    } else {
      pushStep(objIdx, "inv", "ms2", "sunbath", "ms", 36);
    }
    return;
  }

  // Season 3 (level_barbecue, level_laundry, level_fitness, level_DIY, level_hunter) sequences
  if (lowerName == "bal/spray") {
    pushStep(objIdx, "spray_plant", "ms1", "inv", "ms", 28);
    return;
  }
  if (lowerName == "bal/growspray") {
    int plantIdx = findObjectIndex("bal/plant", po.roomName);
    pushStep(objIdx, "spray_plant_manipulated", "inv", "inv", "ms", 20, "bal/spray");
    if (plantIdx >= 0) {
      pushStep(plantIdx, "inv", "inv", "grow", "fight", 18);
      pushStep(plantIdx, "inv", "ms2", "fight", "ms", 36, "", isTricked, isTricked ? &trickIt->second : nullptr);
    }
    return;
  }
  if (lowerName == "bal/noclothes") {
    pushStep(objIdx, "use_mid", "ms2", "ms", "ms", 20, "bal/clothes");
    return;
  }
  if (lowerName == "bal/clothes") {
    pushStep(objIdx, "take3", "ms2", "ms", "ms", 16);
    return;
  }
  if (lowerName == "bed/ironingboard") {
    pushStep(objIdx, "iron", "give_short", "iron", "ms", 28);
    pushStep(objIdx, "give_short", "ms0", "ms", "ms", 12, "bed/ironingboard_clothes");
    return;
  }
  if (lowerName == "bed/ironingboard_clothes") {
    pushStep(objIdx, "iron", "ms0", "iron", "ms", 28);
    return;
  }
  if (lowerName == "anc/skippingrope") {
    pushStep(objIdx, "take_low", "ropejump", "inv", "inv", 12);
    pushStep(objIdx, "ropejump", "ms2", "ms", "ms", 32);
    return;
  }
  if (lowerName == "anc/skippingrope_knotted") {
    pushStep(objIdx, "take_low", "jumpcrash", "inv", "inv", 12);
    pushStep(objIdx, "jumpcrash", "ms2", "ms", "ms", 36, "anc/skippingrope", isTricked, isTricked ? &trickIt->second : nullptr);
    return;
  }
  if (lowerName == "kit/mixer") {
    pushStep(objIdx, "mix", "drink", "mix", "drink", 24);
    pushStep(objIdx, "drink", "ms1", "drink", "ms", 24);
    return;
  }
  if (lowerName == "lir/stoolkit") {
    int piecesIdx = findObjectIndex("lir/pieces", po.roomName);
    pushStep(objIdx, "take_low", "inv", "ms", "ms", 12);
    if (piecesIdx >= 0) {
      pushStep(piecesIdx, "inv", "inv", "assemble", "disassemble", 30);
      pushStep(piecesIdx, "inv", "ms2", "disassemble", "ms", 24);
    }
    return;
  }
  if (lowerName == "lir/stoolkit_glued") {
    int piecesIdx = findObjectIndex("lir/pieces", po.roomName);
    pushStep(objIdx, "take_low", "inv", "ms", "ms", 12, "lir/stoolkit");
    if (piecesIdx >= 0) {
      pushStep(piecesIdx, "inv", "ms2", "glue", "ms", 42, "", isTricked, isTricked ? &trickIt->second : nullptr);
    }
    return;
  }
  if (lowerName == "wor/ladder") {
    pushStep(objIdx, "inv", "inv", "enter", "drill", 16);
    pushStep(objIdx, "inv", "inv", "drill", "touch", 24);
    pushStep(objIdx, "inv", "inv", "touch", "climb_down", 18);
    pushStep(objIdx, "inv", "ms2", "climb_down", "ms", 16);
    return;
  }
  if (lowerName == "wor/ladder_sawed") {
    pushStep(objIdx, "inv", "ms2", "break", "ms", 40, "wor/ladder", isTricked, isTricked ? &trickIt->second : nullptr);
    return;
  }
  if (lowerName == "lir/lockedphono") {
    pushStep(objIdx, "use_mid", "ms0", "open", "ms", 20, "lir/phono_open");
    return;
  }
  if (lowerName == "lir/phono_open") {
    pushStep(objIdx, "use_mid", "listen", "play", "play", 16);
    pushStep(objIdx, "listen", "ms0", "play", "ms", 30);
    return;
  }
  if (lowerName == "lir/phono_nail") {
    pushStep(objIdx, "use_mid", "listen_angry", "destroy", "ms", 20);
    pushStep(objIdx, "listen_angry", "ms2", "ms", "ms", 32, "lir/phono_open", isTricked, isTricked ? &trickIt->second : nullptr);
    return;
  }
  if (lowerName == "lir/tabacbox") {
    pushStep(objIdx, "take", "smoke_pipe", "ms", "ms", 14);
    pushStep(objIdx, "smoke_pipe", "ms2", "ms", "ms", 32);
    return;
  }
  if (lowerName == "lir/tabacbox_powder") {
    pushStep(objIdx, "take", "pipe_explo", "ms", "ms", 14);
    pushStep(objIdx, "pipe_explo", "ms2", "ms", "ms", 36, "lir/tabacbox", isTricked, isTricked ? &trickIt->second : nullptr);
    return;
  }
  if (lowerName == "bas/gun") {
    pushStep(objIdx, "take_short", "aim_rifle", "inv", "inv", 12);
    pushStep(objIdx, "aim_rifle", "give_short", "inv", "inv", 28);
    pushStep(objIdx, "give_short", "ms2", "ms", "ms", 12);
    return;
  }
  if (lowerName == "bas/gun_loaded" || lowerName == "bas/gun_bended") {
    std::string trickAnim = (lowerName == "bas/gun_loaded") ? "shoot_rifle" : "rifle_explode";
    pushStep(objIdx, "take_short", trickAnim, "inv", "inv", 12);
    pushStep(objIdx, trickAnim, "ms2", "ms", "ms", 36, "bas/gun", isTricked, isTricked ? &trickIt->second : nullptr);
    return;
  }
  if (lowerName == "bed/hat") {
    pushStep(objIdx, "take_short", "pickhat", "inv", "inv", 12);
    pushStep(objIdx, "pickhat", "putbackhat", "inv", "inv", 24);
    pushStep(objIdx, "putbackhat", "ms2", "ms", "ms", 14);
    return;
  }
  if (lowerName == "bed/hat_small") {
    pushStep(objIdx, "take_short", "riphat", "inv", "inv", 12);
    pushStep(objIdx, "riphat", "ms2", "ms", "ms", 36, "bed/hat", isTricked, isTricked ? &trickIt->second : nullptr);
    return;
  }
  if (lowerName == "bed/medalbox") {
    pushStep(objIdx, "take_short", "trymedals", "ms", "ms", 12);
    pushStep(objIdx, "trymedals", "ms2", "ms", "ms", 28);
    return;
  }
  if (lowerName == "bed/medalbox_trap") {
    pushStep(objIdx, "take_short", "handrat", "ms", "ms", 12);
    pushStep(objIdx, "handrat", "ms2", "ms", "ms", 36, "bed/medalbox", isTricked, isTricked ? &trickIt->second : nullptr);
    return;
  }
  if (lowerName == "bal/woodhorn") {
    pushStep(objIdx, "take_short", "blowhorn", "inv", "inv", 12);
    pushStep(objIdx, "blowhorn", "give_short", "inv", "inv", 28);
    pushStep(objIdx, "give_short", "ms2", "ms", "ms", 12);
    return;
  }
  if (lowerName == "bal/woodhorn_balloon") {
    pushStep(objIdx, "take_short", "blowballoon", "inv", "inv", 12);
    pushStep(objIdx, "blowballoon", "ms2", "ms", "ms", 36, "bal/woodhorn", isTricked, isTricked ? &trickIt->second : nullptr);
    return;
  }

  // Container handling for Neighbor (e.g. kit/fridge on level_bath/level_pig: open -> take -> close)
  if (od->hasFlag("container")) {
    TaskStep stOpen{};
    stOpen.type = TaskStep::Type::PlayAction;
    stOpen.objectIndex = objIdx;
    stOpen.actorAnim = "open";
    stOpen.actorNextAnim = "ms0";
    stOpen.objAnim = "open";
    stOpen.objNextAnim = "open";
    stOpen.durationTicks = animFrameCount("neighbor", "open", 4);
    neighborQueue_.push_back(stOpen);

    TaskStep stTake{};
    stTake.type = TaskStep::Type::PlayAction;
    stTake.objectIndex = objIdx;
    stTake.actorAnim = "take";
    stTake.actorNextAnim = "ms0";
    stTake.objAnim = "open";
    stTake.objNextAnim = "open";
    stTake.durationTicks = animFrameCount("neighbor", "take", 16);
    neighborQueue_.push_back(stTake);

    TaskStep stClose{};
    stClose.type = TaskStep::Type::PlayAction;
    stClose.objectIndex = objIdx;
    stClose.actorAnim = "close";
    stClose.actorNextAnim = "ms0";
    stClose.objAnim = "ms";
    stClose.objNextAnim = "ms";
    stClose.durationTicks = animFrameCount("neighbor", "close", 4);
    neighborQueue_.push_back(stClose);
    return;
  }

  // Tricked object handling (supports multi-step prank chains like anc/mailbox_trap on level_mail)
  if (isTricked) {
    std::vector<const ActionDef*> prankActs;
    const ActionDef* cleanAct = nullptr;
    for (const ActionDef& a : od->actions) {
      if (toLowerAscii(a.actor) != "neighbor") continue;
      if (a.name == "clean" || a.name == "repair") cleanAct = &a;
      else prankActs.push_back(&a);
    }

    if (!prankActs.empty()) {
      for (size_t i = 0; i < prankActs.size(); ++i) {
        const ActionDef* prankAct = prankActs[i];
        bool isLast = (i + 1 == prankActs.size());
        bool destroysObject = isLast && (toLowerAscii(prankAct->objNextAnim) == "inv");
        TaskStep st{};
        st.type = TaskStep::Type::PlayAction;
        st.objectIndex = objIdx;
        st.actorAnim = prankAct->actorAnim.empty() ? "shout0" : prankAct->actorAnim;
        st.actorNextAnim = prankAct->actorNextAnim.empty() ? (isLast ? "ms2" : st.actorAnim) : prankAct->actorNextAnim;
        st.objAnim = prankAct->objAnim;
        st.objNextAnim = (prankAct->objNextAnim.empty() || destroysObject) ? "ms" : prankAct->objNextAnim;
        if (prankAct->timeTicks > 0) {
          st.durationTicks = prankAct->timeTicks;
        } else if (st.actorAnim == "inv" && !st.objAnim.empty()) {
          st.durationTicks = animFrameCount(od->gfx, st.objAnim, 45);
        } else {
          st.durationTicks = animFrameCount("neighbor", st.actorAnim, 36);
        }
        if (isLast) {
          st.triggerPrankScore = true;
          st.trick = trickIt->second;
          st.transformObjectTo = destroysObject ? std::string() : origName;
          st.hideObjectOnFinish = destroysObject || origName.empty();
        }
        neighborQueue_.push_back(st);
      }
    } else {
      TaskStep shoutSt{};
      shoutSt.type = TaskStep::Type::PlayAction;
      shoutSt.objectIndex = objIdx;
      shoutSt.actorAnim = "shout0";
      shoutSt.actorNextAnim = "ms0";
      shoutSt.durationTicks = animFrameCount("neighbor", "shout0", 26);
      shoutSt.triggerPrankScore = true;
      shoutSt.trick = trickIt->second;
      if (!cleanAct) {
        shoutSt.transformObjectTo = origName;
        shoutSt.hideObjectOnFinish = origName.empty();
      }
      neighborQueue_.push_back(shoutSt);

      if (cleanAct) {
        TaskStep clSt{};
        clSt.type = TaskStep::Type::PlayAction;
        clSt.objectIndex = objIdx;
        clSt.actorAnim = cleanAct->actorAnim.empty() ? "use_mid" : cleanAct->actorAnim;
        clSt.actorNextAnim = cleanAct->actorNextAnim.empty() ? "ms0" : cleanAct->actorNextAnim;
        clSt.durationTicks = cleanAct->timeTicks > 0 ? cleanAct->timeTicks
                                                     : animFrameCount("neighbor", clSt.actorAnim, 24);
        clSt.transformObjectTo = origName;
        clSt.hideObjectOnFinish = origName.empty();
        neighborQueue_.push_back(clSt);
      }
    }
    return;
  }

  // Normal routine object interaction (supports chained multi-phase actions via actorNextAnim or objNextAnim)
  std::string expectedActorAnim;
  std::string expectedObjAnim;
  for (const ActionDef& a : od->actions) {
    if (toLowerAscii(a.actor) != "neighbor" || a.name == "clean" || a.name == "repair") continue;
    if (!expectedActorAnim.empty() && toLowerAscii(a.actorAnim) != toLowerAscii(expectedActorAnim)) break;
    if (expectedActorAnim.empty() && !expectedObjAnim.empty() &&
        toLowerAscii(a.objAnim) != toLowerAscii(expectedObjAnim)) {
      break;
    }
    TaskStep st{};
    st.type = TaskStep::Type::PlayAction;
    st.objectIndex = objIdx;
    st.actorAnim = a.actorAnim.empty() ? "use_mid" : a.actorAnim;
    st.actorNextAnim = a.actorNextAnim.empty() ? (st.actorAnim == "inv" ? "inv" : "ms2") : a.actorNextAnim;
    st.objAnim = a.objAnim;
    st.objNextAnim = a.objNextAnim.empty() ? "ms" : a.objNextAnim;
    if (a.timeTicks > 0) {
      st.durationTicks = a.timeTicks;
    } else if (st.actorAnim == "inv" && !st.objAnim.empty()) {
      st.durationTicks = animFrameCount(od->gfx, st.objAnim, 36);
    } else {
      st.durationTicks = animFrameCount("neighbor", st.actorAnim, 24);
    }
    expectedActorAnim = a.actorNextAnim;
    expectedObjAnim = a.objNextAnim;
    bool endsHere = (!expectedActorAnim.empty() && expectedActorAnim.rfind("ms", 0) == 0) ||
                    (expectedActorAnim.empty() && (expectedObjAnim.empty() || expectedObjAnim == "ms"));
    if (endsHere && st.actorNextAnim == "inv") st.actorNextAnim = "ms2";
    neighborQueue_.push_back(st);
    if (endsHere) break;
  }
}

void GameEngine::updateNeighbor() {
  if (neighbor_.hidden && neighborQueue_.empty()) return;

  bool animDone = false;
  advanceActorAnim(neighbor_, &animDone);

  if (neighborWaitTicks_ > 0) {
    neighborWaitTicks_--;
    return;
  }

  // Check if any room/pass-by trigger trick object is in Neighbor's current room and close to him
  bool alreadyActing = (!neighborQueue_.empty() && neighborQueue_.front().type != TaskStep::Type::WalkToX);
  if (!alreadyActing && !neighbor_.hidden) {
    for (size_t i = 0; i < level_.objects.size(); ++i) {
      const PlacedObject& po = level_.objects[i];
      if (!po.visible || toLowerAscii(po.roomName) != toLowerAscii(neighbor_.room)) continue;
      auto trickIt = level_.tricks.find(toLowerAscii(po.name));
      if (trickIt == level_.tricks.end()) continue;
      bool isPassTrigger = false;
      for (const TriggerDef& tr : level_.triggers) {
        if (toLowerAscii(tr.object) == toLowerAscii(po.name)) {
          isPassTrigger = true;
          break;
        }
      }
      if (isPassTrigger) {
        const ObjectDef* od = level_.findObjectDef(po.name);
        Point hs = od ? od->hotspotOr("neighbor", Point{neighbor_.pos.x, 420}) : Point{neighbor_.pos.x, 420};
        hs.x += po.position.x;
        hs.y += po.position.y;
        if (std::abs(neighbor_.pos.x - hs.x) <= 75) {
          neighborQueue_.clear();
          Point curPos = neighbor_.pos;
          enqueueRoomWalk(neighborQueue_, neighbor_.room, curPos, hs, false);
          TaskStep inter{};
          inter.type = TaskStep::Type::InteractObject;
          inter.objectIndex = (int)i;
          neighborQueue_.push_back(inter);
          break;
        }
      }
    }
  }

  while (!neighborQueue_.empty()) {
    TaskStep& step = neighborQueue_.front();
    switch (step.type) {
      case TaskStep::Type::WalkToX: {
        int dx = step.targetX - neighbor_.pos.x;
        int dy = (step.targetY > 0 ? step.targetY : neighbor_.pos.y) - neighbor_.pos.y;
        if (dx == 0 && dy == 0) {
          neighborQueue_.pop_front();
          if (neighborQueue_.empty()) {
            neighbor_.currentAnim = "ms2";
            neighbor_.nextAnim = "ms2";
            neighbor_.animFrame = 0;
          }
          continue;
        }
        if (std::abs(dx) > 0) {
          std::string want = (dx > 0) ? "mg1" : "mg3";
          int speed = actorSpeed("neighbor", want, 8);
          if (neighbor_.currentAnim != want) {
            neighbor_.currentAnim = want;
            neighbor_.nextAnim = want;
            neighbor_.animFrame = 0;
          }
          if (std::abs(dx) <= speed) {
            neighbor_.pos.x = step.targetX;
            if (dy == 0) {
              std::string stopAnim = (dx > 0) ? "ms1" : "ms3";
              neighborQueue_.pop_front();
              if (neighborQueue_.empty()) {
                neighbor_.currentAnim = stopAnim;
                neighbor_.nextAnim = stopAnim;
                neighbor_.animFrame = 0;
              }
              continue;
            }
          } else {
            neighbor_.pos.x += (dx > 0 ? speed : -speed);
          }
        } else {
          std::string want = (dy < 0) ? "mg0" : "mg2";
          int speed = actorSpeed("neighbor", want, 3);
          if (neighbor_.currentAnim != want) {
            neighbor_.currentAnim = want;
            neighbor_.nextAnim = want;
            neighbor_.animFrame = 0;
          }
          if (std::abs(dy) <= speed) {
            neighbor_.pos.y = step.targetY;
            std::string stopAnim = (dy < 0) ? "ms0" : "ms2";
            neighborQueue_.pop_front();
            if (neighborQueue_.empty()) {
              neighbor_.currentAnim = stopAnim;
              neighbor_.nextAnim = stopAnim;
              neighbor_.animFrame = 0;
            }
            continue;
          } else {
            neighbor_.pos.y += (dy > 0 ? speed : -speed);
          }
        }
        return;
      }

      case TaskStep::Type::TraverseDoor: {
        if (step.elapsedTicks >= step.totalTicks) {
          if (step.objectIndex >= 0 && step.objectIndex < (int)level_.objects.size()) {
            PlacedObject& doorIn = level_.objects[step.objectIndex];
            doorIn.currentAnim = "ms";
            doorIn.nextAnim = "ms";
            doorIn.animFrame = 0;
          }
          if (step.outObjectIndex >= 0 && step.outObjectIndex < (int)level_.objects.size()) {
            PlacedObject& doorOut = level_.objects[step.outObjectIndex];
            doorOut.currentAnim = "ms";
            doorOut.nextAnim = "ms";
            doorOut.animFrame = 0;
          }
          neighbor_.room = step.targetRoom;
          neighbor_.pos = step.outPos;
          neighbor_.hidden = false;
          neighbor_.currentAnim = step.outAnim;
          neighbor_.nextAnim = step.outAnim;
          neighbor_.animFrame = 0;
          neighborQueue_.pop_front();
          continue;
        }
        if (step.elapsedTicks == 0) {
          neighbor_.hidden = true;
          if (step.objectIndex >= 0 && step.objectIndex < (int)level_.objects.size()) {
            PlacedObject& doorIn = level_.objects[step.objectIndex];
            doorIn.currentAnim = "neighbor_enter";
            doorIn.nextAnim = "ms";
            doorIn.animFrame = 0;
          }
          if (step.outObjectIndex >= 0 && step.outObjectIndex < (int)level_.objects.size()) {
            PlacedObject& doorOut = level_.objects[step.outObjectIndex];
            doorOut.currentAnim = "neighbor_leave";
            doorOut.nextAnim = "ms";
            doorOut.animFrame = 0;
          }
        }
        step.elapsedTicks++;
        if (step.elapsedTicks == step.enterTicks) {
          neighbor_.room = step.targetRoom;
          neighbor_.pos = step.outPos;
        }
        return;
      }

      case TaskStep::Type::PlayAction: {
        if (step.elapsedTicks >= step.durationTicks) {
          if (step.objectIndex >= 0 && step.objectIndex < (int)level_.objects.size()) {
            PlacedObject& po = level_.objects[step.objectIndex];
            if (!step.transformObjectTo.empty()) {
              po.name = step.transformObjectTo;
              po.currentAnim = "ms";
              po.nextAnim = "ms";
              po.animFrame = 0;
            } else if (step.hideObjectOnFinish) {
              po.visible = false;
            } else if (!step.objNextAnim.empty()) {
              po.currentAnim = step.objNextAnim;
              po.nextAnim = step.objNextAnim;
              po.animFrame = 0;
            }
          }
          if (step.outObjectIndex >= 0 && step.outObjectIndex < (int)level_.objects.size()) {
            PlacedObject& secPo = level_.objects[step.outObjectIndex];
            secPo.currentAnim = "ms";
            secPo.nextAnim = "ms";
            secPo.animFrame = 0;
            std::string secLower = toLowerAscii(secPo.name);
            if (secLower == "bed/cactusclock") secPo.name = "bed/alarmclock";
            else if (secLower == "bal/ewer_poisoned") secPo.name = "bal/ewer";
            else if (secLower == "bal/suncream_honey" || secLower == "bal/suncream_oil") secPo.name = "bal/suncream";
          }
          neighbor_.hidden = false;
          neighbor_.currentAnim = step.actorNextAnim.empty() ? "ms2" : step.actorNextAnim;
          neighbor_.nextAnim = neighbor_.currentAnim;
          neighbor_.animFrame = 0;
          neighborQueue_.pop_front();
          continue;
        }
        if (step.elapsedTicks == 0) {
          if (step.triggerPrankScore && step.objectIndex >= 0 && step.objectIndex < (int)level_.objects.size()) {
            int scoreIdx = (step.outObjectIndex >= 0 && step.outObjectIndex < (int)level_.objects.size())
                               ? step.outObjectIndex
                               : step.objectIndex;
            awardPrankScore(level_.objects[scoreIdx].name, step.trick);
          }
          if (step.actorAnim == "inv") {
            neighbor_.hidden = true;
            neighbor_.currentAnim = "inv";
          } else if (!step.actorAnim.empty()) {
            neighbor_.hidden = false;
            neighbor_.currentAnim = step.actorAnim;
            neighbor_.nextAnim = step.actorNextAnim;
            neighbor_.animFrame = 0;
          }
          if (step.objectIndex >= 0 && step.objectIndex < (int)level_.objects.size() && !step.objAnim.empty()) {
            PlacedObject& po = level_.objects[step.objectIndex];
            po.currentAnim = step.objAnim;
            po.nextAnim = step.objNextAnim;
            po.animFrame = 0;
          }
          if (step.outObjectIndex >= 0 && step.outObjectIndex < (int)level_.objects.size()) {
            PlacedObject& secPo = level_.objects[step.outObjectIndex];
            std::string secLower = toLowerAscii(secPo.name);
            if (secLower == "bed/alarmclock" || secLower == "bed/cactusclock") {
              secPo.currentAnim = "ring";
              secPo.nextAnim = "ms";
              secPo.animFrame = 0;
            } else if (secLower == "toi/towel" || secLower == "toi/dirtytowel") {
              secPo.currentAnim = "inv";
              secPo.nextAnim = "ms";
              secPo.animFrame = 0;
            }
          }
        }
        step.elapsedTicks++;
        return;
      }

      case TaskStep::Type::InteractObject: {
        int idx = step.objectIndex;
        neighborQueue_.pop_front();
        enqueueNeighborObjectActions(idx);
        continue;
      }

      default:
        neighborQueue_.pop_front();
        continue;
    }
  }

  // Plan next routine stop
  if (routine_.empty()) return;
  int targetObjIdx = -1;
  const RoutineStop* chosenStop = nullptr;
  for (size_t attempt = 0; attempt < routine_.size(); ++attempt) {
    const RoutineStop& st = routine_[routineIdx_];
    routineIdx_ = (routineIdx_ + 1) % (int)routine_.size();
    if (st.onlyWhenTricked) {
      if (!st.trickObj.empty()) {
        int tidx = findObjectIndex(st.trickObj, st.room);
        if (tidx >= 0) {
          targetObjIdx = tidx;
          chosenStop = &st;
          break;
        }
      }
      continue;
    }
    int nidx = findObjectIndex(st.normalObj, st.room);
    if (nidx < 0 && !st.trickObj.empty()) {
      nidx = findObjectIndex(st.trickObj, st.room);
    }
    if (nidx < 0 && !st.fallbackObj.empty()) {
      nidx = findObjectIndex(st.fallbackObj, st.room);
    }
    if (nidx >= 0) {
      targetObjIdx = nidx;
      chosenStop = &st;
      break;
    }
  }
  if (targetObjIdx < 0 || !chosenStop) return;
  if (!chosenStop->iconName.empty()) currentBubbleIcon_ = chosenStop->iconName;

  // Enqueue room path for Neighbor with 2.5D walk lines and synchronized door animations
  std::string curRoom = neighbor_.room;
  Point curPos = neighbor_.pos;
  for (const RoomEdge& e : findRoomPath(curRoom, chosenStop->room)) {
    const ObjectDef* dIn = level_.findObjectDef(e.doorIn);
    const ObjectDef* dOut = level_.findObjectDef(e.doorOut);
    int inIdx = findObjectIndex(e.doorIn, curRoom);
    int outIdx = findObjectIndex(e.doorOut, e.targetRoom);
    Point inPos = dIn ? dIn->hotspotOr("neighbor", Point{450, 420}) : Point{450, 420};
    if (inIdx >= 0) {
      inPos.x += level_.objects[inIdx].position.x;
      inPos.y += level_.objects[inIdx].position.y;
    }
    Point outPos = dOut ? dOut->hotspotOr("neighbor_out", dOut->hotspotOr("neighbor", Point{450, 420})) : Point{450, 420};
    if (outIdx >= 0) {
      outPos.x += level_.objects[outIdx].position.x;
      outPos.y += level_.objects[outIdx].position.y;
    }
    if (const RoomDef* rOut = level_.findRoom(e.targetRoom)) {
      int lo = std::min(rOut->path1.x, rOut->path2.x);
      int hi = std::max(rOut->path1.x, rOut->path2.x);
      if (hi > lo) outPos.x = std::clamp(outPos.x, lo, hi);
    }

    enqueueRoomWalk(neighborQueue_, curRoom, curPos, inPos, false);

    TaskStep doorStep{};
    doorStep.type = TaskStep::Type::TraverseDoor;
    doorStep.doorName = e.doorIn;
    doorStep.objectIndex = inIdx;
    doorStep.outObjectIndex = outIdx;
    doorStep.targetRoom = e.targetRoom;
    doorStep.outPos = outPos;
    doorStep.outAnim = "ms2";
    int enterFrames = dIn ? animFrameCount(dIn->gfx, "neighbor_enter", 12) : 12;
    int leaveFrames = dOut ? animFrameCount(dOut->gfx, "neighbor_leave", 23) : 23;
    if (dIn) {
      if (const ActionDef* aEn = dIn->findAction("enter", "neighbor")) {
        if (aEn->timeTicks > 0) enterFrames = std::max(enterFrames, aEn->timeTicks);
      }
    }
    if (dOut) {
      if (const ActionDef* aLv = dOut->findAction("leave", "neighbor")) {
        if (!aLv->actorNextAnim.empty()) doorStep.outAnim = aLv->actorNextAnim;
        if (aLv->timeTicks > 0) leaveFrames = std::max(leaveFrames, aLv->timeTicks);
      }
    }
    doorStep.enterTicks = std::max(1, enterFrames);
    doorStep.totalTicks = std::max(doorStep.enterTicks, leaveFrames);
    neighborQueue_.push_back(doorStep);

    curRoom = e.targetRoom;
    curPos = outPos;
  }

  const PlacedObject& targetPo = level_.objects[targetObjIdx];
  const ObjectDef* targetOd = level_.findObjectDef(targetPo.name);
  Point hs = targetOd ? targetOd->hotspotOr("neighbor", Point{450, 420}) : Point{450, 420};
  hs.x += targetPo.position.x;
  hs.y += targetPo.position.y;

  enqueueRoomWalk(neighborQueue_, curRoom, curPos, hs, false);

  TaskStep inter{};
  inter.type = TaskStep::Type::InteractObject;
  inter.objectIndex = targetObjIdx;
  neighborQueue_.push_back(inter);
}

void GameEngine::checkStealthDetection() {
  if (catchSequenceActive_) {
    catchTicks_--;
    advanceActorAnim(neighbor_);
    if (catchTicks_ <= 0) {
      catchSequenceActive_ = false;
      neighborChasing_ = false;
      neighborAlertTicks_ = 0;
      catchesCount_++;
      if (catchesCount_ >= 3) {
        finishEpisode(false, true);
        return;
      }
      woody_.room = "fro";
      woody_.pos = Point{105, 218};
      woody_.hidden = false;
      woody_.hidingInObject = -1;
      woody_.currentAnim = "ms2";
      woody_.nextAnim = "ms2";
      woody_.animFrame = 0;
      woodyQueue_.clear();
      for (PlacedObject& po : level_.objects) {
        if (po.isDoor && (po.currentAnim == "woody_enter" || po.currentAnim == "woody_leave")) {
          po.currentAnim = "ms";
          po.nextAnim = "ms";
          po.animFrame = 0;
        }
        if (po.currentAnim.rfind("woody_", 0) == 0 || po.currentAnim == "decline") {
          po.currentAnim = "ms";
          po.nextAnim = "ms";
          po.animFrame = 0;
        }
      }
      neighbor_.currentAnim = "ms2";
      neighbor_.nextAnim = "ms2";
      neighbor_.animFrame = 0;
      neighborWaitTicks_ = 12;
    }
    return;
  }

  bool woodyTrulyHidden = (woody_.hidden && woody_.hidingInObject >= 0);
  if (woodyTrulyHidden || neighbor_.hidden || toLowerAscii(woody_.room) != toLowerAscii(neighbor_.room)) {
    if (neighborChasing_) {
      neighborChasing_ = false;
      neighborAlertTicks_ = 0;
      neighbor_.currentAnim = "ms2";
      neighbor_.nextAnim = "ms2";
      neighbor_.animFrame = 0;
      neighborWaitTicks_ = 12;
    }
    return;
  }

  // Woody and Neighbor are in the same room!
  int dist = std::abs(woody_.pos.x - neighbor_.pos.x);
  if (dist <= 45) {
    neighborChasing_ = false;
    neighborAlertTicks_ = 0;
    catchSequenceActive_ = true;
    catchTicks_ = animFrameCount("neighbor", "kill1", 28);
    woodyQueue_.clear();
    neighborQueue_.clear();
    woodyActionTicks_ = woodyActionTotalTicks_ = 0;
    woody_.hidden = true;  // fight cloud is drawn on neighbor's kill1 animation
    neighbor_.pos = woody_.pos;
    neighbor_.currentAnim = "kill1";
    neighbor_.nextAnim = "ms2";
    neighbor_.animFrame = 0;
    playSfx("sfx/wod_fear1.wav");
    playSfx("sfx/na_beats1.wav");
    playSfx("sfx/wod_au1.wav");
    statusBanner_ = "СОСЕД ПОЙМАЛ ВУДИ! (" + std::to_string(catchesCount_ + 1) + "/3)";
    bannerTicks_ = kTicksPerSecond * 3;

    for (PlacedObject& po : level_.objects) {
      if (po.isDoor && (po.currentAnim == "woody_enter" || po.currentAnim == "woody_leave")) {
        po.currentAnim = "ms";
        po.nextAnim = "ms";
        po.animFrame = 0;
      }
      if (po.currentAnim.rfind("woody_", 0) == 0 || po.currentAnim == "decline") {
        po.currentAnim = "ms";
        po.nextAnim = "ms";
        po.animFrame = 0;
      }
    }
  } else {
    // Neighbor discovers Woody and starts chase
    if (!neighborChasing_) {
      neighborChasing_ = true;
      neighborAlertTicks_ = 6;
      neighborQueue_.clear();
      neighborBusyInAnim_ = false;
      neighbor_.facing = (woody_.pos.x > neighbor_.pos.x) ? 1 : 3;
      playSfx("sfx/na_killshout1.wav");
      playSfx("sfx/wod_fear1.wav");
      statusBanner_ = "СОСЕД ЗАМЕТИЛ ВАС!";
      bannerTicks_ = kTicksPerSecond * 2;
      if (!woodyQueue_.empty() && woodyQueue_.front().type != TaskStep::Type::TraverseDoor) {
        woodyQueue_.clear();
        woodyActionTicks_ = woodyActionTotalTicks_ = 0;
      }
    }

    if (neighborAlertTicks_ > 0) {
      neighborAlertTicks_--;
      neighbor_.currentAnim = (neighbor_.facing == 1) ? "find1" : "find3";
      advanceActorAnim(neighbor_);
    } else {
      int dir = (woody_.pos.x > neighbor_.pos.x) ? 1 : -1;
      neighbor_.facing = (dir > 0) ? 1 : 3;
      neighbor_.currentAnim = (dir > 0) ? "mr1" : "mr3";
      neighbor_.pos.x += dir * 14;
      neighbor_.pos.y = woody_.pos.y;
      advanceActorAnim(neighbor_);
    }
  }
}

void GameEngine::tickSimulation() {
  if (screen_ == ScreenState::Credits) {
    creditsScrollY_ -= 2.2f;
    float totalH = float(creditsLines_.size()) * 28.0f + 560.0f;
    if (creditsScrollY_ < -totalH) creditsScrollY_ = 540.0f;
    return;
  }

  if (screen_ != ScreenState::InGame || paused_) return;
  if (bannerTicks_ > 0) bannerTicks_--;
  if (angerTicks_ > 0) angerTicks_--;
  if (optTimeLimit_ && level_.info.timeTicks > 0) {
    if (remainingTicks_ > 0) remainingTicks_--;
    if (remainingTicks_ <= 0) {
      finishEpisode(true, false);
      return;
    }
  }

  advanceObjectAnims();
  checkStealthDetection();
  if (screen_ != ScreenState::InGame) return;
  if (!catchSequenceActive_) {
    updateWoody();
    if (!neighborChasing_) updateNeighbor();
  }

  // Auto-finish episode once all tricks have been performed and Neighbor finished reacting
  if (level_.info.reachable > 0 && tricksDone_ >= level_.info.reachable && !catchSequenceActive_) {
    bool neighborPlayingPrank = (!neighborQueue_.empty() && neighborQueue_.front().type == TaskStep::Type::PlayAction);
    if (!neighborPlayingPrank) {
      finishEpisode(false, false);
    }
  }
}

// ---------------------------------------------------------------------------
// Coordinate transforms & Rendering
// ---------------------------------------------------------------------------

Point GameEngine::screenToWorld(int sx, int sy) const {
  return Point{int((sx - camX_) / camZoom_), int((sy - camY_) / camZoom_)};
}

SDL_FRect GameEngine::worldToScreen(float wx, float wy, float ww, float wh) const {
  return SDL_FRect{camX_ + wx * camZoom_, camY_ + wy * camZoom_, ww * camZoom_, wh * camZoom_};
}

float GameEngine::menuScale() const {
  return std::min(float(winW_) / 800.0f, float(winH_) / 600.0f);
}

Point GameEngine::screenToMenu(int sx, int sy) const {
  float s = menuScale();
  float ox = (winW_ - 800.0f * s) * 0.5f;
  float oy = (winH_ - 600.0f * s) * 0.5f;
  return Point{int(std::round((sx - ox) / s)), int(std::round((sy - oy) / s))};
}

SDL_Rect GameEngine::menuRect(int x, int y, int w, int h) const {
  float s = menuScale();
  float ox = (winW_ - 800.0f * s) * 0.5f;
  float oy = (winH_ - 600.0f * s) * 0.5f;
  return SDL_Rect{int(std::round(ox + x * s)), int(std::round(oy + y * s)),
                  std::max(1, int(std::round(w * s))), std::max(1, int(std::round(h * s)))};
}

void GameEngine::drawMenuImage(const std::string& vpath, int x, int y) {
  if (const TexEntry* t = getTexture(vpath)) {
    SDL_Rect dst = menuRect(x, y, t->w, t->h);
    SDL_RenderCopy(ren_, t->tex, nullptr, &dst);
  }
}

bool GameEngine::drawMenuButton(const std::string& stdImg, const std::string& hovImg, const std::string& downImg,
                                int x, int y, int w, int h, const std::string& caption, TextRenderer& font,
                                SDL_Color stdColor, SDL_Color hovColor, Point menuMouse, bool mouseDown,
                                bool* outHovered) {
  const TexEntry* tStd = getTexture(stdImg);
  int bw = w > 0 ? w : (tStd ? tStd->w : 100);
  int bh = h > 0 ? h : (tStd ? tStd->h : 40);
  bool hovered = (confirmAction_ == ConfirmAction::None) && Rect{x, y, bw, bh}.contains(menuMouse.x, menuMouse.y);
  if (outHovered) *outHovered = hovered;

  const std::string& chosenImg = hovered ? (mouseDown && !downImg.empty() ? downImg : (hovImg.empty() ? stdImg : hovImg)) : stdImg;
  if (const TexEntry* t = getTexture(chosenImg)) {
    SDL_Rect dst = menuRect(x, y, t->w, t->h);
    SDL_RenderCopy(ren_, t->tex, nullptr, &dst);
  }
  if (!caption.empty()) {
    SDL_Rect textBox = menuRect(x + 8, y + (hovered && mouseDown ? 2 : 0), bw - 16, bh);
    font.drawAligned(caption, textBox, hovered ? hovColor : stdColor, 0, 0, menuScale());
  }
  return hovered;
}

void GameEngine::renderObject(const PlacedObject& po) {
  if (!po.visible || po.currentAnim == "inv" || toLowerAscii(po.name) == "house") return;
  const RoomDef* r = level_.findRoom(po.roomName);
  const Point base{po.position.x + (r ? r->offset.x : 0), po.position.y + (r ? r->offset.y : 0)};

  const ObjectDef* od = level_.findObjectDef(po.name);
  const std::string gfxName = od ? od->gfx : po.name;
  const GfxObjectDef* gd = level_.findGfxObject(gfxName);
  if (!gd) return;

  auto it = gd->anims.find(toLowerAscii(po.currentAnim));
  if (it == gd->anims.end() || it->second.frames.empty()) {
    it = gd->anims.find("ms");
    if (it == gd->anims.end() || it->second.frames.empty()) return;
  }
  const AnimationDef& anim = it->second;
  int idx = std::clamp(po.animFrame, 0, (int)anim.frames.size() - 1);
  const AnimFrame& fr = anim.frames[idx];
  const TexEntry* tex = getTexture(fr.vpath);
  if (!tex) return;

  SDL_FRect dst = worldToScreen(float(base.x + fr.offset.x), float(base.y + fr.offset.y), float(tex->w), float(tex->h));
  SDL_RenderCopyF(ren_, tex->tex, nullptr, &dst);
}

void GameEngine::renderActor(const ActorState& a) {
  if (a.hidden || a.currentAnim == "inv") return;
  const RoomDef* r = level_.findRoom(a.room);
  if (!r) return;
  const ActorDef* ad = nullptr;
  auto ait = level_.actors.find(toLowerAscii(a.name));
  if (ait != level_.actors.end()) ad = &ait->second;
  const Point hotspot = ad ? ad->hotspot : Point{460, 420};

  const GfxObjectDef* gd = level_.findGfxObject(a.name);
  if (!gd) return;
  auto it = gd->anims.find(toLowerAscii(a.currentAnim));
  if (it == gd->anims.end() || it->second.frames.empty()) {
    it = gd->anims.find("ms2");
    if (it == gd->anims.end() || it->second.frames.empty()) return;
  }
  const AnimationDef& anim = it->second;
  int idx = std::clamp(a.animFrame, 0, (int)anim.frames.size() - 1);
  const AnimFrame& fr = anim.frames[idx];
  const TexEntry* tex = getTexture(fr.vpath);
  if (!tex) return;

  float wx = float(r->offset.x + (a.pos.x - hotspot.x) + fr.offset.x);
  float wy = float(r->offset.y + (a.pos.y - hotspot.y) + fr.offset.y);
  SDL_FRect dst = worldToScreen(wx, wy, float(tex->w), float(tex->h));
  SDL_RenderCopyF(ren_, tex->tex, nullptr, &dst);

  // Draw progress bar above Woody when setting a trap
  if (toLowerAscii(a.name) == "woody" && woodyActionTotalTicks_ > 0 && woodyActionTicks_ > 0) {
    const TexEntry* pbBack = getTexture("gfx/gui/ingame/ingame_progress_back.tga");
    const TexEntry* pbFront = getTexture("gfx/gui/ingame/ingame_progress_front.tga");
    if (pbBack && pbFront) {
      float pbx = float(r->offset.x + a.pos.x) - float(pbBack->w) * 0.5f;
      float pby = float(r->offset.y + a.pos.y - 130);
      SDL_FRect bDst = worldToScreen(pbx, pby, float(pbBack->w), float(pbBack->h));
      SDL_RenderCopyF(ren_, pbBack->tex, nullptr, &bDst);
      float frac = 1.0f - float(woodyActionTicks_) / float(woodyActionTotalTicks_);
      int srcW = std::clamp(int(pbFront->w * frac), 0, pbFront->w);
      SDL_Rect src{0, 0, srcW, pbFront->h};
      SDL_FRect fDst = worldToScreen(pbx, pby, float(srcW), float(pbFront->h));
      SDL_RenderCopyF(ren_, pbFront->tex, &src, &fDst);

      // Centered percentage text with black outline (e.g. "73%")
      int pct = std::clamp(int(std::round(frac * 100.0f)), 0, 100);
      char pctBuf[16];
      std::snprintf(pctBuf, sizeof pctBuf, "%d%%", pct);
      float tScale = (bDst.h > 0.0f) ? (bDst.h * 0.72f / 26.0f) : 1.0f;
      float o = std::max(1.0f, tScale * 1.5f);
      SDL_Color black{0, 0, 0, 255};
      SDL_Color yellow{255, 235, 40, 255};
      fontHud_.drawAlignedScaled(pctBuf, SDL_FRect{bDst.x + o, bDst.y, bDst.w, bDst.h}, black, tScale, 0, 0);
      fontHud_.drawAlignedScaled(pctBuf, SDL_FRect{bDst.x - o, bDst.y, bDst.w, bDst.h}, black, tScale, 0, 0);
      fontHud_.drawAlignedScaled(pctBuf, SDL_FRect{bDst.x, bDst.y + o, bDst.w, bDst.h}, black, tScale, 0, 0);
      fontHud_.drawAlignedScaled(pctBuf, SDL_FRect{bDst.x, bDst.y - o, bDst.w, bDst.h}, black, tScale, 0, 0);
      fontHud_.drawAlignedScaled(pctBuf, SDL_FRect{bDst.x + o, bDst.y + o, bDst.w, bDst.h}, black, tScale, 0, 0);
      fontHud_.drawAlignedScaled(pctBuf, SDL_FRect{bDst.x - o, bDst.y - o, bDst.w, bDst.h}, black, tScale, 0, 0);
      fontHud_.drawAlignedScaled(pctBuf, SDL_FRect{bDst.x + o, bDst.y - o, bDst.w, bDst.h}, black, tScale, 0, 0);
      fontHud_.drawAlignedScaled(pctBuf, SDL_FRect{bDst.x - o, bDst.y + o, bDst.w, bDst.h}, black, tScale, 0, 0);
      fontHud_.drawAlignedScaled(pctBuf, bDst, yellow, tScale, 0, 0);
    }
  }
}

void GameEngine::renderWorld() {
  // Update camera placement
  const float hudScale = std::min(float(winW_) / 800.0f, float(winH_) / 480.0f);
  const int hudHeight = int(134 * hudScale);
  const int availH = std::max(200, winH_ - hudHeight);
  if (fullHouseView_) {
    float zx = float(winW_) / float(level_.size.x);
    float zy = float(availH) / float(level_.size.y);
    camZoom_ = std::min(zx, zy);
    camX_ = (winW_ - level_.size.x * camZoom_) * 0.5f;
    camY_ = (availH - level_.size.y * camZoom_) * 0.5f;
  } else {
    camZoom_ = std::max(1.0f, float(winW_) / 950.0f);
    if (!camDragging_) {
      if (const RoomDef* wr = level_.findRoom(woody_.room)) {
        float targetX = winW_ * 0.5f - (wr->offset.x + woody_.pos.x) * camZoom_;
        float targetY = availH * 0.5f - (wr->offset.y + woody_.pos.y - 80) * camZoom_;

        // Edge scrolling when mouse cursor approaches window edges
        const int edgeMargin = 50;
        const float scrollSpeed = 16.0f;
        bool edgeScrolling = false;
        if (screen_ == ScreenState::InGame && confirmAction_ == ConfirmAction::None) {
          if (mouseX_ >= 0 && mouseX_ <= winW_ && mouseY_ >= 0 && mouseY_ <= availH) {
            if (mouseX_ < edgeMargin) {
              camEdgeOffsetX_ += scrollSpeed;
              edgeScrolling = true;
            } else if (mouseX_ > winW_ - edgeMargin) {
              camEdgeOffsetX_ -= scrollSpeed;
              edgeScrolling = true;
            }
            if (mouseY_ < edgeMargin) {
              camEdgeOffsetY_ += scrollSpeed;
              edgeScrolling = true;
            } else if (mouseY_ > availH - edgeMargin) {
              camEdgeOffsetY_ -= scrollSpeed;
              edgeScrolling = true;
            }
          }
        }
        if (!edgeScrolling) {
          camEdgeOffsetX_ *= 0.90f;
          camEdgeOffsetY_ *= 0.90f;
          if (std::abs(camEdgeOffsetX_) < 0.5f) camEdgeOffsetX_ = 0.0f;
          if (std::abs(camEdgeOffsetY_) < 0.5f) camEdgeOffsetY_ = 0.0f;
        } else {
          float maxScrollX = float(level_.size.x) * camZoom_;
          float maxScrollY = float(level_.size.y) * camZoom_;
          camEdgeOffsetX_ = std::clamp(camEdgeOffsetX_, -maxScrollX, maxScrollX);
          camEdgeOffsetY_ = std::clamp(camEdgeOffsetY_, -maxScrollY, maxScrollY);
        }

        targetX += camEdgeOffsetX_;
        targetY += camEdgeOffsetY_;

        camX_ += (targetX - camX_) * 0.15f;
        camY_ += (targetY - camY_) * 0.15f;
      }
    }
    float houseW = float(level_.size.x) * camZoom_;
    float houseH = float(level_.size.y) * camZoom_;
    if (houseW <= float(winW_)) {
      camX_ = (float(winW_) - houseW) * 0.5f;
    } else {
      camX_ = std::clamp(camX_, float(winW_) - houseW, 0.0f);
    }
    if (houseH <= float(availH)) {
      camY_ = (float(availH) - houseH) * 0.5f;
    } else {
      camY_ = std::clamp(camY_, float(availH) - houseH, 0.0f);
    }
  }

  // Layer 0: House background
  if (const TexEntry* bg = getTexture(level_.houseBackgroundVpath)) {
    SDL_FRect dst = worldToScreen(0, 0, float(bg->w), float(bg->h));
    SDL_RenderCopyF(ren_, bg->tex, nullptr, &dst);
  }

  // Layers 1..3: Objects and Doors
  for (int layer = 1; layer <= 3; ++layer) {
    for (const PlacedObject& po : level_.objects) {
      if (po.layer == layer) renderObject(po);
    }
  }

  // Highlight hovered object subtly
  if (hoveredObject_ >= 0 && hoveredObject_ < (int)level_.objects.size()) {
    Rect hb = objectWorldHitbox(level_.objects[hoveredObject_]);
    SDL_FRect sr = worldToScreen(float(hb.x), float(hb.y), float(hb.w), float(hb.h));
    SDL_SetRenderDrawColor(ren_, 255, 220, 90, 90);
    SDL_RenderDrawRectF(ren_, &sr);
  }

  // Layer 4: Actors
  renderActor(neighbor_);
  renderActor(woody_);

  if (showDebug_) {
    for (const PlacedObject& po : level_.objects) {
      if (!po.visible || toLowerAscii(po.name) == "house") continue;
      Rect hb = objectWorldHitbox(po);
      SDL_FRect sr = worldToScreen(float(hb.x), float(hb.y), float(hb.w), float(hb.h));
      SDL_SetRenderDrawColor(ren_, po.isDoor ? 80 : 255, po.isDoor ? 220 : 120, 255, 160);
      SDL_RenderDrawRectF(ren_, &sr);
    }
  }
}

void GameEngine::renderHud() {
  const float hudScale = std::min(float(winW_) / 800.0f, float(winH_) / 480.0f);
  const float hudW = 800.0f * hudScale;
  const float hudH = 600.0f * hudScale;
  const float hudX = (float(winW_) - hudW) * 0.5f;
  const float hudY = float(winH_) - hudH;

  auto drawGui = [&](const std::string& vpath, float ox, float oy) {
    if (const TexEntry* t = getTexture(vpath)) {
      SDL_FRect dst{hudX + ox * hudScale, hudY + oy * hudScale, float(t->w) * hudScale, float(t->h) * hudScale};
      SDL_RenderCopyF(ren_, t->tex, nullptr, &dst);
    }
  };

  // Darken strip behind HUD
  int hudStripH = int(134 * hudScale);
  SDL_SetRenderDrawColor(ren_, 18, 20, 26, 235);
  SDL_Rect strip{0, winH_ - hudStripH, winW_, hudStripH};
  SDL_RenderFillRect(ren_, &strip);

  // Left: thought bubble, rage-o-meter, neighbor head
  drawGui("gfx/gui/ingame/interface_left_bubble_norm.tga", 0, 466);
  if (!currentBubbleIcon_.empty()) {
    auto it = level_.icons.find(toLowerAscii(currentBubbleIcon_));
    if (it != level_.icons.end()) drawGui(it->second, 0, 466);
  }

  drawGui("gfx/gui/ingame/interface_bar_dialog.tga", 85, 482);
  drawGui("gfx/gui/ingame/interface_bar_back.tga", 85, 486);
  if (angerTicks_ > 0) {
    if (const TexEntry* front = getTexture("gfx/gui/ingame/interface_bar_front.tga")) {
      float frac = std::clamp(float(angerTicks_) / float(angerMaxTicks_), 0.0f, 1.0f);
      int h = int(front->h * frac);
      int dy = front->h - h;
      SDL_Rect src{0, dy, front->w, h};
      SDL_FRect dst{hudX + 85.0f * hudScale, hudY + (486.0f + dy) * hudScale, float(front->w) * hudScale, float(h) * hudScale};
      SDL_RenderCopyF(ren_, front->tex, &src, &dst);
    }
  }

  drawGui("gfx/gui/ingame/interface_left_bottom.tga", 0, 530);
  const char* headSprite = (angerTicks_ > angerMaxTicks_ / 2) ? "gfx/gui/ingame/head_03_norm.tga"
                         : (angerTicks_ > 0)                  ? "gfx/gui/ingame/head_02_norm.tga"
                                                              : "gfx/gui/ingame/head_01_norm.tga";
  drawGui(headSprite, 0, 530);

  // Center: tooltip bar + 5 inventory slots
  drawGui("gfx/gui/ingame/interface_center_top.tga", 123, 506);
  drawGui("gfx/gui/ingame/interface_center.tga", 123, 511);
  drawGui("gfx/gui/ingame/scroll_left_std.tga", 123, 547);
  drawGui("gfx/gui/ingame/scroll_right_std.tga", 123 + 422, 547);

  if (!hoverText_.empty()) {
    fontUI_.drawScaled(hoverText_, hudX + 135.0f * hudScale, hudY + 513.0f * hudScale, hudScale, SDL_Color{255, 255, 255, 255});
  }

  for (int slot = 0; slot < 5; ++slot) {
    int idx = invScroll_ + slot;
    if (idx >= (int)inventory_.size()) break;
    float sx = hudX + (123.0f + 41.0f + slot * 75.0f) * hudScale;
    float sy = hudY + (511.0f + 26.0f) * hudScale;
    if (idx == selectedInv_) {
      SDL_SetRenderDrawColor(ren_, 240, 190, 60, 110);
      SDL_FRect hi{sx - 2.0f * hudScale, sy - 2.0f * hudScale, 64.0f * hudScale, 56.0f * hudScale};
      SDL_RenderFillRectF(ren_, &hi);
    }
    auto it = level_.inventar.find(toLowerAscii(inventory_[idx]));
    if (it != level_.inventar.end()) {
      const std::string& iconPath = (idx == selectedInv_ && !it->second.gfxHover.empty()) ? it->second.gfxHover : it->second.gfxStd;
      if (const TexEntry* t = getTexture(iconPath)) {
        SDL_FRect dst{sx, sy, float(t->w) * hudScale, float(t->h) * hudScale};
        SDL_RenderCopyF(ren_, t->tex, nullptr, &dst);
      }
    }
  }

  // Right: TV panel (timer, quota %, tricks count, and interactive Mission/Menu button)
  drawGui("gfx/gui/ingame/interface_right.tga", 581, 471);
  float hx = (float(mouseX_) - hudX) / hudScale;
  float hy = (float(mouseY_) - hudY) / hudScale;
  bool missionHov = Rect{581 + 167, 471 + 20, 48, 36}.contains(int(hx), int(hy));
  drawGui(missionHov ? (mouseLeftDown_ ? "gfx/gui/ingame/mission_pressed.tga" : "gfx/gui/ingame/mission_hover.tga")
                     : "gfx/gui/ingame/mission_normal.tga",
          581 + 167, 471 + 20);

  char timeBuf[32];
  if (!optTimeLimit_ || level_.info.timeTicks <= 0) {
    std::snprintf(timeBuf, sizeof timeBuf, "--:--");
  } else {
    int secs = std::max(0, remainingTicks_ / kTicksPerSecond);
    std::snprintf(timeBuf, sizeof timeBuf, "%02d:%02d", secs / 60, secs % 60);
  }
  fontHud_.drawScaled(timeBuf, hudX + (581.0f + 98.0f) * hudScale, hudY + (471.0f + 20.0f) * hudScale, hudScale, SDL_Color{140, 215, 215, 255});

  char quotaBuf[32];
  std::snprintf(quotaBuf, sizeof quotaBuf, "%d%%", quota_);
  SDL_Color qColor = (quota_ >= level_.info.minQuota) ? SDL_Color{120, 240, 130, 255} : SDL_Color{255, 255, 255, 255};
  fontHud_.drawScaled(quotaBuf, hudX + (581.0f + 78.0f) * hudScale, hudY + (471.0f + 90.0f) * hudScale, hudScale, qColor);

  char trickBuf[32];
  std::snprintf(trickBuf, sizeof trickBuf, "%d/%d", tricksDone_, level_.info.reachable);
  fontUI_.drawScaled(trickBuf, hudX + (581.0f + 170.0f) * hudScale, hudY + (471.0f + 68.0f) * hudScale, hudScale, SDL_Color{245, 240, 90, 255});

  // Top status banner
  if (bannerTicks_ > 0 && !statusBanner_.empty()) {
    int bw = fontBig_.width(statusBanner_) + 32;
    SDL_Rect box{(winW_ - bw) / 2, 12, bw, 36};
    SDL_SetRenderDrawColor(ren_, 24, 28, 36, 220);
    SDL_RenderFillRect(ren_, &box);
    SDL_SetRenderDrawColor(ren_, 235, 175, 65, 255);
    SDL_RenderDrawRect(ren_, &box);
    fontBig_.draw(statusBanner_, box.x + 16, box.y + 5, SDL_Color{255, 235, 170, 255});
  }
}

// ---------------------------------------------------------------------------
// Original Menu & Dialog Rendering (800x600 virtual coordinate space)
// ---------------------------------------------------------------------------

void GameEngine::renderMainMenu() {
  Point mp = screenToMenu(mouseX_, mouseY_);
  drawMenuImage("gfx/gui/main/mainmenu.tga", 0, 0);
  drawMenuImage("gfx/gui/logo/logo_main.tga", 235, 15);

  SDL_Color cStd{215, 215, 215, 255};
  SDL_Color cHov{255, 255, 255, 255};
  drawMenuButton("gfx/gui/main/mainmenu_btn3_hover.tga", "gfx/gui/main/mainmenu_btn3_hover.tga",
                 "gfx/gui/main/mainmenu_btn3_pressed.tga", 248, 286, 333, 54,
                 menuStrings_["mainmenu_start"].empty() ? "Начать игру" : menuStrings_["mainmenu_start"],
                 fontBig_, cStd, cHov, mp, mouseLeftDown_);
  drawMenuButton("gfx/gui/main/mainmenu_btn2_hover.tga", "gfx/gui/main/mainmenu_btn2_hover.tga",
                 "gfx/gui/main/mainmenu_btn2_pressed.tga", 248, 341, 333, 54,
                 menuStrings_["mainmenu_credits"].empty() ? "Авторы" : menuStrings_["mainmenu_credits"],
                 fontBig_, cStd, cHov, mp, mouseLeftDown_);
  drawMenuButton("gfx/gui/main/mainmenu_btn2_hover.tga", "gfx/gui/main/mainmenu_btn2_hover.tga",
                 "gfx/gui/main/mainmenu_btn2_pressed.tga", 248, 396, 333, 54,
                 menuStrings_["mainmenu_options"].empty() ? "Настройки" : menuStrings_["mainmenu_options"],
                 fontBig_, cStd, cHov, mp, mouseLeftDown_);
  drawMenuButton("gfx/gui/main/mainmenu_btn1_hover.tga", "gfx/gui/main/mainmenu_btn1_hover.tga",
                 "gfx/gui/main/mainmenu_btn1_pressed.tga", 248, 451, 333, 54,
                 menuStrings_["mainmenu_quit"].empty() ? "Выход" : menuStrings_["mainmenu_quit"],
                 fontBig_, cStd, cHov, mp, mouseLeftDown_);

  fontSmall_.drawAligned("OpenNFH v0.2.0 (Buka / JoWooD 1.11)", menuRect(12, 570, 770, 24),
                         SDL_Color{230, 230, 230, 220}, -1, 1, menuScale());
}

void GameEngine::renderBriefingRightPanel(int levelIdx, bool isInGameMenu) {
  if (levels_.empty()) return;
  levelIdx = std::clamp(levelIdx, 0, (int)levels_.size() - 1);
  const LevelInfo& li = levels_[levelIdx];
  const BriefingInfo& bi = briefings_[levelIdx];
  Point mp = screenToMenu(mouseX_, mouseY_);

  drawMenuImage("gfx/gui/main/newgameright.tga", 400, 0);

  // Caption ("Описание серии")
  std::string rightCap = menuStrings_["newgamerightcaption"].empty() ? "Описание серии" : menuStrings_["newgamerightcaption"];
  fontBig_.drawAligned(rightCap, menuRect(400, 0, 375, 50), SDL_Color{255, 255, 255, 255}, 1, 0);

  // Episode title
  fontBig_.drawAligned(bi.title, menuRect(400 + 32, 61, 335, 53), SDL_Color{0, 0, 0, 255}, -1, 0);

  // Level preview image
  drawMenuImage("gfx/gui/main/" + li.name + ".tga", 400 + 28, 121);

  // Min quota & Time limit
  char qNum[16];
  std::snprintf(qNum, sizeof qNum, "%d", li.minQuota);
  fontBig_.drawAligned(qNum, menuRect(400 + 212, 147, 46, 26), SDL_Color{255, 255, 255, 255}, 1, 1);
  fontSmall_.drawAligned("%", menuRect(400 + 260, 150, 18, 22), SDL_Color{255, 255, 255, 255}, -1, 1);
  fontSmall_.drawAligned(menuStrings_["minquotacap"].empty() ? "Миним. оценка" : menuStrings_["minquotacap"],
                         menuRect(400 + 187, 178, 88, 20), SDL_Color{255, 255, 255, 255}, 0, -1);

  char tNum[16];
  if (li.timeTicks <= 0) {
    std::snprintf(tNum, sizeof tNum, "--");
  } else {
    int mins = (li.timeTicks / kTicksPerSecond) / 60;
    std::snprintf(tNum, sizeof tNum, "%d", mins);
  }
  fontBig_.drawAligned(tNum, menuRect(400 + 290, 147, 50, 26), SDL_Color{255, 255, 255, 255}, 1, 1);
  fontSmall_.drawAligned(menuStrings_["timeunit"].empty() ? "мин" : menuStrings_["timeunit"],
                         menuRect(400 + 342, 150, 30, 22), SDL_Color{255, 255, 255, 255}, -1, 1);
  fontSmall_.drawAligned(menuStrings_["timecap"].empty() ? "Время" : menuStrings_["timecap"],
                         menuRect(400 + 282, 178, 88, 20), SDL_Color{255, 255, 255, 255}, 0, -1);

  // Hint
  fontUI_.drawAligned(menuStrings_["hinttitle"].empty() ? "Подсказка!" : menuStrings_["hinttitle"],
                      menuRect(400 + 190, 202, 177, 22), SDL_Color{0, 96, 255, 255}, 0, 0);
  fontSmall_.drawWrapped(bi.hintText, menuRect(400 + 190, 223, 177, 56), SDL_Color{0, 0, 0, 255}, 0, 0);

  // Photo caption & Story briefing
  fontSmall_.drawAligned(bi.picText, menuRect(400 + 28, 264, 155, 22), SDL_Color{0, 0, 0, 255}, -1, -1);
  fontSmall_.drawWrapped(bi.briefing, menuRect(400 + 28, 287, 343, 178), SDL_Color{0, 0, 0, 255}, -1, -1);

  // Record score for this episode
  const LevelProgress& lp = progress_[toLowerAscii(li.name)];
  fontUI_.drawAligned(menuStrings_["lastscoretitle"].empty() ? "Рекорд серии:" : menuStrings_["lastscoretitle"],
                      menuRect(400 + 67, 475, 189, 23), SDL_Color{0, 0, 0, 255}, -1, 0);
  fontSmall_.drawAligned(menuStrings_["lasttrickscap"].empty() ? "Пакостей" : menuStrings_["lasttrickscap"],
                         menuRect(400 + 30, 499, 155, 23), SDL_Color{0, 0, 0, 255}, 0, 0);
  char trRec[32];
  std::snprintf(trRec, sizeof trRec, "%d / %d", lp.bestTricks, li.reachable);
  fontUI_.drawAligned(trRec, menuRect(400 + 55, 521, 115, 24), SDL_Color{0, 0, 0, 255}, 0, 0);

  fontSmall_.drawAligned(menuStrings_["lastquotacap"].empty() ? "Оценка" : menuStrings_["lastquotacap"],
                         menuRect(400 + 191, 499, 63, 23), SDL_Color{0, 0, 0, 255}, 0, 0);
  char qRec[32];
  std::snprintf(qRec, sizeof qRec, "%d %%", lp.bestQuota);
  fontUI_.drawAligned(qRec, menuRect(400 + 191, 521, 63, 24), SDL_Color{0, 0, 0, 255}, 0, 0);

  // Play / Continue label & button
  std::string actLabel = isInGameMenu ? (menuStrings_["continuetext"].empty() ? "Назад в игру" : menuStrings_["continuetext"])
                                      : (menuStrings_["play"].empty() ? "Съемка" : menuStrings_["play"]);
  fontUI_.drawWrapped(actLabel, menuRect(400 + 269, 475, 103, 55), SDL_Color{0, 0, 0, 255}, 0, 0);
  drawMenuButton("gfx/gui/main/start_right_std.tga", "gfx/gui/main/start_right_hover.tga",
                 "gfx/gui/main/start_right_pressed.tga", 400 + 320, 530, 0, 0, "", fontUI_,
                 SDL_Color{}, SDL_Color{}, mp, mouseLeftDown_);
}

void GameEngine::renderEpisodeSelect() {
  Point mp = screenToMenu(mouseX_, mouseY_);
  drawMenuImage("gfx/gui/main/newgameleft.tga", 0, 0);

  // Header & Season titles
  fontBig_.drawAligned(menuStrings_["newgamecaption"].empty() ? "Список серий" : menuStrings_["newgamecaption"],
                       menuRect(75, 0, 350, 50), SDL_Color{255, 255, 255, 255}, -1, 0);
  fontUI_.drawAligned(menuStrings_["tutseason"].empty() ? "Пилотные серии" : menuStrings_["tutseason"],
                      menuRect(46, 61, 342, 23), SDL_Color{0, 0, 0, 255}, -1, 0);
  fontUI_.drawAligned(menuStrings_["season1"].empty() ? "Сезон 1" : menuStrings_["season1"],
                      menuRect(46, 160, 342, 23), SDL_Color{0, 0, 0, 255}, -1, 0);
  fontUI_.drawAligned(menuStrings_["season2"].empty() ? "Сезон 2" : menuStrings_["season2"],
                      menuRect(46, 323, 342, 23), SDL_Color{0, 0, 0, 255}, -1, 0);
  fontUI_.drawAligned(menuStrings_["season3"].empty() ? "Сезон 3" : menuStrings_["season3"],
                      menuRect(46, 422, 342, 23), SDL_Color{0, 0, 0, 255}, -1, 0);

  auto isSeasonCompleted = [&](const std::initializer_list<const char*>& levelNames) {
    for (const char* name : levelNames) {
      auto it = progress_.find(toLowerAscii(name));
      if (it == progress_.end() || !it->second.completed) return false;
    }
    return true;
  };

  // Season trophy icons on the left margin (only displayed when corresponding season is completed)
  if (isSeasonCompleted({"level_peep", "level_sofa", "level_mail", "level_pie", "level_piano", "level_bath"})) {
    drawMenuImage("gfx/gui/main/briefingscreen_bronze.tga", 2, 150);
  }
  if (isSeasonCompleted({"level_art", "level_suntan", "level_pig", "level_barbecue"})) {
    drawMenuImage("gfx/gui/main/briefingscreen_silver.tga", 2, 313);
  }
  if (isSeasonCompleted({"level_laundry", "level_fitness", "level_DIY", "level_hunter"})) {
    drawMenuImage("gfx/gui/main/briefingscreen_gold.tga", 2, 412);
  }

  // 17 Episode buttons
  for (size_t i = 0; i < levels_.size(); ++i) {
    const LevelInfo& li = levels_[i];
    const BriefingInfo& bi = briefings_[i];
    const LevelProgress& lp = progress_[toLowerAscii(li.name)];
    std::string stateTag = lp.perfect ? "perfect" : (lp.completed ? "passed" : "normal");
    std::string base = "gfx/gui/main/button_" + bi.buttonShortName + "_" + stateTag;
    std::string stdImg = base + ".tga";
    std::string hovImg = base + "_hover.tga";
    std::string prsImg = base + "_pressed.tga";

    bool hovered = false;
    drawMenuButton(stdImg, hovImg, prsImg, bi.menuX, bi.menuY, 75, 60, "", fontSmall_,
                   SDL_Color{}, SDL_Color{}, mp, mouseLeftDown_, &hovered);

    // Highlight selected episode button
    if ((int)i == previewLevelIdx_) {
      SDL_Rect sel = menuRect(bi.menuX - 2, bi.menuY - 2, 79, 64);
      SDL_SetRenderDrawColor(ren_, 255, 215, 50, 235);
      SDL_RenderDrawRect(ren_, &sel);
    }

    // Best score % badge at the bottom of the button
    char scoreTxt[16];
    std::snprintf(scoreTxt, sizeof scoreTxt, "%d %%", lp.bestQuota);
    fontSmall_.drawAligned(scoreTxt, menuRect(bi.menuX, bi.menuY + 41, 75, 18),
                           hovered ? SDL_Color{255, 255, 160, 255} : SDL_Color{255, 255, 255, 255}, 0, 0);
  }

  // Back to main menu button
  fontUI_.drawAligned(menuStrings_["canceltext"].empty() ? "Вернуться в главное меню" : menuStrings_["canceltext"],
                      menuRect(90, 521, 282, 25), SDL_Color{0, 0, 0, 255}, -1, 0);
  drawMenuButton("gfx/gui/main/exit_std.tga", "gfx/gui/main/exit_hover.tga", "gfx/gui/main/exit_pressed.tga",
                 25, 530, 0, 0, "", fontUI_, SDL_Color{}, SDL_Color{}, mp, mouseLeftDown_);

  // Right panel: Briefing for selected episode
  renderBriefingRightPanel(previewLevelIdx_, false);
}

void GameEngine::renderPauseMenu() {
  Point mp = screenToMenu(mouseX_, mouseY_);
  drawMenuImage("gfx/gui/main/ingamemenu_left.tga", 0, 0);

  fontBig_.drawAligned(menuStrings_["mainmenuwhilegame_title"].empty() ? "Игровое меню" : menuStrings_["mainmenuwhilegame_title"],
                       menuRect(32, 61, 335, 53), SDL_Color{0, 0, 0, 255}, -1, 0);

  SDL_Color cStd{215, 215, 215, 255};
  SDL_Color cHov{255, 255, 255, 255};
  drawMenuButton("gfx/gui/main/mainmenu_btn3_hover.tga", "gfx/gui/main/mainmenu_btn3_hover.tga",
                 "gfx/gui/main/mainmenu_btn3_pressed.tga", 33, 170, 333, 54,
                 menuStrings_["mainmenu_back2game"].empty() ? "Продолжить игру" : menuStrings_["mainmenu_back2game"],
                 fontBig_, cStd, cHov, mp, mouseLeftDown_);
  drawMenuButton("gfx/gui/main/mainmenu_btn2_hover.tga", "gfx/gui/main/mainmenu_btn2_hover.tga",
                 "gfx/gui/main/mainmenu_btn2_pressed.tga", 33, 225, 333, 54,
                 menuStrings_["mainmenu_restart"].empty() ? "Начать съемку заново" : menuStrings_["mainmenu_restart"],
                 fontBig_, cStd, cHov, mp, mouseLeftDown_);
  drawMenuButton("gfx/gui/main/mainmenu_btn2_hover.tga", "gfx/gui/main/mainmenu_btn2_hover.tga",
                 "gfx/gui/main/mainmenu_btn2_pressed.tga", 33, 280, 333, 54,
                 menuStrings_["mainmenu_startnew"].empty() ? "Снимать новую серию" : menuStrings_["mainmenu_startnew"],
                 fontBig_, cStd, cHov, mp, mouseLeftDown_);
  drawMenuButton("gfx/gui/main/mainmenu_btn2_hover.tga", "gfx/gui/main/mainmenu_btn2_hover.tga",
                 "gfx/gui/main/mainmenu_btn2_pressed.tga", 33, 335, 333, 54,
                 menuStrings_["mainmenu_options"].empty() ? "Настройки" : menuStrings_["mainmenu_options"],
                 fontBig_, cStd, cHov, mp, mouseLeftDown_);
  drawMenuButton("gfx/gui/main/mainmenu_btn1_hover.tga", "gfx/gui/main/mainmenu_btn1_hover.tga",
                 "gfx/gui/main/mainmenu_btn1_pressed.tga", 33, 390, 333, 54,
                 menuStrings_["mainmenu_backtomain"].empty() ? "Вернуться в главное меню" : menuStrings_["mainmenu_backtomain"],
                 fontBig_, cStd, cHov, mp, mouseLeftDown_);

  renderBriefingRightPanel(currentLevelIdx_, true);
}

void GameEngine::renderOptionsScreen() {
  Point mp = screenToMenu(mouseX_, mouseY_);
  drawMenuImage("gfx/gui/main/optionsback.tga", 0, 0);

  SDL_Color green{75, 255, 0, 255};
  SDL_Color yellow{255, 255, 0, 255};
  std::string statusHint;

  // Handle live dragging on volume bars
  if (mouseLeftDown_ && confirmAction_ == ConfirmAction::None) {
    if (const TexEntry* vb = getTexture("gfx/gui/volume_dark.tga")) {
      if (Rect{295, 252, vb->w, vb->h}.contains(mp.x, mp.y)) {
        masterVolume_ = std::clamp((mp.x - 295) * 100 / std::max(1, vb->w), 0, 100);
        muteMaster_ = false;
        applyAudioVolumes();
      } else if (Rect{295, 290, vb->w, vb->h}.contains(mp.x, mp.y)) {
        musicVolume_ = std::clamp((mp.x - 295) * 100 / std::max(1, vb->w), 0, 100);
        muteMusic_ = false;
        applyAudioVolumes();
      }
    }
  }

  // Master Volume
  bool hov = false;
  drawMenuButton(muteMaster_ ? "gfx/gui/main/sound_off.tga" : "gfx/gui/main/sound_on.tga",
                 muteMaster_ ? "gfx/gui/main/sound_off_hov.tga" : "gfx/gui/main/sound_on_hov.tga",
                 muteMaster_ ? "gfx/gui/main/sound_off_hov.tga" : "gfx/gui/main/sound_on_hov.tga",
                 251, 248, 0, 0, "", fontSmall_, green, yellow, mp, mouseLeftDown_, &hov);
  if (hov) statusHint = statusStrings_["mute_master"];

  if (const TexEntry* vDark = getTexture("gfx/gui/volume_dark.tga")) {
    drawMenuImage("gfx/gui/volume_dark.tga", 295, 252);
    bool barHov = Rect{295, 252, vDark->w, vDark->h}.contains(mp.x, mp.y);
    if (barHov) statusHint = statusStrings_["mastervolume"];
    const TexEntry* vFront = getTexture(barHov ? "gfx/gui/volume_hov.tga" : "gfx/gui/volume_norm.tga");
    if (vFront && !muteMaster_) {
      int fillW = vFront->w * masterVolume_ / 100;
      SDL_Rect src{0, 0, fillW, vFront->h};
      SDL_Rect dst = menuRect(295, 252, fillW, vFront->h);
      SDL_RenderCopy(ren_, vFront->tex, &src, &dst);
    }
  }
  fontSmall_.drawAligned(menuStrings_["master volume"].empty() ? "Громкость" : menuStrings_["master volume"],
                         menuRect(410, 249, 237, 44), green, -1, 0);

  // Music Volume
  drawMenuButton(muteMusic_ ? "gfx/gui/main/sound_off.tga" : "gfx/gui/main/sound_on.tga",
                 muteMusic_ ? "gfx/gui/main/sound_off_hov.tga" : "gfx/gui/main/sound_on_hov.tga",
                 muteMusic_ ? "gfx/gui/main/sound_off_hov.tga" : "gfx/gui/main/sound_on_hov.tga",
                 251, 292, 0, 0, "", fontSmall_, green, yellow, mp, mouseLeftDown_, &hov);
  if (hov) statusHint = statusStrings_["mute_music"];

  if (const TexEntry* vDark = getTexture("gfx/gui/volume_dark.tga")) {
    drawMenuImage("gfx/gui/volume_dark.tga", 295, 290);
    bool barHov = Rect{295, 290, vDark->w, vDark->h}.contains(mp.x, mp.y);
    if (barHov) statusHint = statusStrings_["musicvolume"];
    const TexEntry* vFront = getTexture(barHov ? "gfx/gui/volume_hov.tga" : "gfx/gui/volume_norm.tga");
    if (vFront && !muteMusic_ && !muteMaster_) {
      int fillW = vFront->w * musicVolume_ / 100;
      SDL_Rect src{0, 0, fillW, vFront->h};
      SDL_Rect dst = menuRect(295, 290, fillW, vFront->h);
      SDL_RenderCopy(ren_, vFront->tex, &src, &dst);
    }
  }
  fontSmall_.drawAligned(menuStrings_["music volume"].empty() ? "Музыка" : menuStrings_["music volume"],
                         menuRect(410, 287, 237, 44), green, -1, 0);

  // Reset user data button
  drawMenuButton("gfx/gui/main/spielstand_norm.tga", "gfx/gui/main/spielstand_hov.tga", "gfx/gui/main/spielstand_hov.tga",
                 262, 335, 0, 0,
                 menuStrings_["reset_userdata"].empty() ? "Удалить сохраненные игры" : menuStrings_["reset_userdata"],
                 fontSmall_, green, yellow, mp, mouseLeftDown_, &hov);
  if (hov) statusHint = statusStrings_["reset_userdata"];

  // 4 Checkboxes
  auto drawCheckbox = [&](int y, bool checked, const std::string& labelKey, const std::string& fallback,
                          const std::string& statusKey) {
    bool cHov = (confirmAction_ == ConfirmAction::None) && Rect{296, y, 250, 19}.contains(mp.x, mp.y);
    std::string img = checked ? (cHov ? "gfx/gui/main/checkbox_on_hov.tga" : "gfx/gui/main/checkbox_on.tga")
                              : (cHov ? "gfx/gui/main/checkbox_off_hov.tga" : "gfx/gui/main/checkbox_off.tga");
    drawMenuImage(img, 296, y);
    std::string txt = menuStrings_[labelKey].empty() ? fallback : menuStrings_[labelKey];
    fontSmall_.drawAligned(txt, menuRect(320, y, 230, 18), cHov ? yellow : green, -1, 0);
    if (cHov) statusHint = statusStrings_[statusKey];
  };

  drawCheckbox(367, optHighQuality_, "sound_quality1", "Высокое качество звука", "soundquality1");
  drawCheckbox(388, optTimeLimit_, "timelimit", "Ограничение по времени", "timelimit");
  drawCheckbox(409, optAutoScroll_, "autoscroll", "Отчет о пакостях", "autoscroll");
  drawCheckbox(430, optColoredCursor_, "colored cursors", "Цветной курсор", "coloredcursors");

  // Cancel & OK buttons
  drawMenuButton("gfx/gui/main/choice_norm.tga", "gfx/gui/main/choice_hov.tga", "gfx/gui/main/choice_hov.tga",
                 262, 455, 0, 0, menuStrings_["option_cancel"].empty() ? "Отмена" : menuStrings_["option_cancel"],
                 fontSmall_, green, yellow, mp, mouseLeftDown_, &hov);
  if (hov) statusHint = statusStrings_["cancel"];

  drawMenuButton("gfx/gui/main/choice_norm.tga", "gfx/gui/main/choice_hov.tga", "gfx/gui/main/choice_hov.tga",
                 402, 455, 0, 0, menuStrings_["option_ok"].empty() ? "ОК" : menuStrings_["option_ok"],
                 fontSmall_, green, yellow, mp, mouseLeftDown_, &hov);
  if (hov) statusHint = statusStrings_["ok"];

  if (!statusHint.empty()) {
    fontUI_.drawAligned(statusHint, menuRect(20, 540, 760, 46), SDL_Color{0, 0, 0, 255}, 0, 0);
  }
}

void GameEngine::renderCreditsScreen() {
  drawMenuImage("gfx/gui/main/credits.tga", 0, 0);
  SDL_Rect clip = menuRect(120, 90, 560, 430);
  SDL_RenderSetClipRect(ren_, &clip);

  float y = creditsScrollY_;
  for (const auto& item : creditsLines_) {
    if (y >= 60.0f && y <= 530.0f && !item.first.empty()) {
      SDL_Rect lineRect = menuRect(130, int(y), 540, 28);
      if (item.second) {
        fontBig_.drawAligned(item.first, lineRect, SDL_Color{255, 220, 90, 255}, 0, 0);
      } else {
        fontUI_.drawAligned(item.first, lineRect, SDL_Color{255, 255, 255, 255}, 0, 0);
      }
    }
    y += 28.0f;
  }
  SDL_RenderSetClipRect(ren_, nullptr);
  fontSmall_.drawAligned("Щелкните мышью или нажмите Esc для возврата в меню", menuRect(0, 555, 800, 30),
                         SDL_Color{230, 230, 230, 230}, 0, 0);
}

void GameEngine::renderLevelSummaryOverlay() {
  // Dim the house behind the summary screen
  SDL_SetRenderDrawColor(ren_, 0, 0, 0, 145);
  SDL_Rect full{0, 0, winW_, winH_};
  SDL_RenderFillRect(ren_, &full);

  Point mp = screenToMenu(mouseX_, mouseY_);
  drawMenuImage("gfx/gui/ingame/abrechnungsscreen.tga", 125, 70);

  SDL_Color green{75, 255, 0, 255};
  SDL_Color yellow{255, 255, 0, 255};
  std::string ratingTitle;
  if (summaryTimeExpired_) {
    ratingTitle = menuStrings_["gameover_timeover"].empty() ? "ВРЕМЯ ВЫШЛО!" : menuStrings_["gameover_timeover"];
  } else if (summaryPerfect_) {
    ratingTitle = menuStrings_["gameover_perfect"].empty() ? "ОТЛИЧНО!" : menuStrings_["gameover_perfect"];
  } else if (summarySuccess_) {
    ratingTitle = menuStrings_["gameover_success"].empty() ? "УСПЕШНО!" : menuStrings_["gameover_success"];
  } else {
    ratingTitle = menuStrings_["gameover_failed"].empty() ? "ПРОВАЛ" : menuStrings_["gameover_failed"];
  }

  fontTitle_.drawAligned(ratingTitle, menuRect(125 + 168, 70 + 118, 349, 42),
                         summarySuccess_ ? green : SDL_Color{255, 110, 80, 255}, 0, 0);

  fontSmall_.drawAligned(menuStrings_["gameover_tricks"].empty() ? "ПАКОСТЕЙ:" : menuStrings_["gameover_tricks"],
                         menuRect(125 + 168, 70 + 185, 349, 20), green, 0, 1);
  char trBuf[32];
  std::snprintf(trBuf, sizeof trBuf, "%d / %d", tricksDone_, level_.info.reachable);
  fontTitle_.drawAligned(trBuf, menuRect(125 + 168, 70 + 202, 349, 40), green, 0, 0);

  fontSmall_.drawAligned(menuStrings_["gameover_quota"].empty() ? "ЗРИТЕЛЬСКАЯ ОЦЕНКА:" : menuStrings_["gameover_quota"],
                         menuRect(125 + 168, 70 + 245, 349, 20), green, 0, 1);
  char qBuf[32];
  std::snprintf(qBuf, sizeof qBuf, "%d %%", quota_);
  fontTitle_.drawAligned(qBuf, menuRect(125 + 168, 70 + 262, 349, 40), green, 0, 0);

  drawMenuButton("gfx/gui/main/choice_norm.tga", "gfx/gui/main/choice_hov.tga", "gfx/gui/main/choice_hov.tga",
                 125 + 190, 70 + 312, 0, 0,
                 menuStrings_["gameover_restart"].empty() ? "ПЕРЕСНЯТЬ" : menuStrings_["gameover_restart"],
                 fontSmall_, green, yellow, mp, mouseLeftDown_);
  drawMenuButton("gfx/gui/main/choice_norm.tga", "gfx/gui/main/choice_hov.tga", "gfx/gui/main/choice_hov.tga",
                 125 + 342, 70 + 312, 0, 0,
                 menuStrings_["gameover_continue"].empty() ? "ОК" : menuStrings_["gameover_continue"],
                 fontSmall_, green, yellow, mp, mouseLeftDown_);
}

void GameEngine::renderSeasonVictoryScreen() {
  const char* bg = (seasonTrophyTier_ >= 3) ? "gfx/gui/main/endscreen_gold.tga"
                 : (seasonTrophyTier_ == 2) ? "gfx/gui/main/endscreen_silver.tga"
                 : (seasonTrophyTier_ == 1) ? "gfx/gui/main/endscreen_bronze.tga"
                                            : "gfx/gui/main/endscreen_default.tga";
  const char* tKey = (seasonTrophyTier_ >= 3) ? "victory_gold_title"
                   : (seasonTrophyTier_ == 2) ? "victory_silver_title"
                   : (seasonTrophyTier_ == 1) ? "victory_bronze_title"
                                              : "victory_default_title";
  const char* bKey = (seasonTrophyTier_ >= 3) ? "victory_gold"
                   : (seasonTrophyTier_ == 2) ? "victory_silver"
                   : (seasonTrophyTier_ == 1) ? "victory_bronze"
                                              : "victory_default";

  drawMenuImage(bg, 0, 0);
  std::string title = menuStrings_[tKey];
  std::string body = menuStrings_[bKey];
  fontTitle_.drawAligned(title, menuRect(3, 67, 720, 60), SDL_Color{0, 0, 0, 255}, 1, 0);
  fontTitle_.drawAligned(title, menuRect(0, 65, 720, 60), SDL_Color{255, 255, 255, 255}, 1, 0);
  fontBig_.drawWrapped(body, menuRect(463, 172, 300, 360), SDL_Color{0, 0, 0, 255}, 0, -1);
  fontBig_.drawWrapped(body, menuRect(460, 170, 300, 360), SDL_Color{255, 255, 255, 255}, 0, -1);
}

void GameEngine::renderConfirmModal() {
  if (confirmAction_ == ConfirmAction::None) return;
  SDL_SetRenderDrawColor(ren_, 0, 0, 0, 155);
  SDL_Rect full{0, 0, winW_, winH_};
  SDL_RenderFillRect(ren_, &full);

  Point mp = screenToMenu(mouseX_, mouseY_);
  int bx = 180, by = 170;

  drawMenuImage("gfx/gui/ingame/messagebox_top.tga", bx, by);
  drawMenuImage("gfx/gui/ingame/mb_head_01.tga", bx + 172, by);
  for (int i = 0; i < 3; ++i) {
    drawMenuImage("gfx/gui/ingame/messagebox_mid.tga", bx, by + 100 + i * 20);
  }
  int lowY = by + 160;
  drawMenuImage("gfx/gui/ingame/messagebox_low2.tga", bx, lowY);

  fontUI_.drawWrapped(confirmText_, menuRect(bx + 35, by + 75, 370, 75), SDL_Color{255, 255, 255, 255}, 0, 0);

  bool hovYes = Rect{bx + 13, lowY + 18, 52, 42}.contains(mp.x, mp.y);
  bool hovNo = Rect{bx + 375, lowY + 18, 52, 42}.contains(mp.x, mp.y);
  drawMenuImage(hovYes ? (mouseLeftDown_ ? "gfx/gui/ingame/messagebox_ok_pressed.tga" : "gfx/gui/ingame/messagebox_ok_hover.tga")
                       : "gfx/gui/ingame/messagebox_ok_std.tga",
                bx + 13, lowY + 18);
  drawMenuImage(hovNo ? (mouseLeftDown_ ? "gfx/gui/ingame/messagebox_cancel_pressed.tga" : "gfx/gui/ingame/messagebox_cancel_hover.tga")
                      : "gfx/gui/ingame/messagebox_cancel_std.tga",
                bx + 375, lowY + 18);
}

void GameEngine::onMenuClick(Point mp, bool doubleClick) {
  // Handle confirmation modal first if active
  if (confirmAction_ != ConfirmAction::None) {
    int bx = 180, lowY = 170 + 160;
    if (Rect{bx + 13, lowY + 18, 52, 42}.contains(mp.x, mp.y)) {
      playSfx("sfx/but1.wav");
      ConfirmAction act = confirmAction_;
      confirmAction_ = ConfirmAction::None;
      if (act == ConfirmAction::QuitGame) running_ = false;
      else if (act == ConfirmAction::ResetProgress) resetUserProgress();
      else if (act == ConfirmAction::AbortEpisode) enterScreen(ScreenState::MainMenu);
      else if (act == ConfirmAction::RestartEpisode) {
        selectLevel(currentLevelIdx_);
        enterScreen(ScreenState::InGame);
      }
      return;
    }
    if (Rect{bx + 375, lowY + 18, 52, 42}.contains(mp.x, mp.y)) {
      playSfx("sfx/but1.wav");
      confirmAction_ = ConfirmAction::None;
      return;
    }
    return;
  }

  switch (screen_) {
    case ScreenState::MainMenu: {
      if (Rect{248, 286, 333, 54}.contains(mp.x, mp.y)) {
        playSfx("sfx/but1.wav");
        enterScreen(ScreenState::EpisodeSelect);
      } else if (Rect{248, 341, 333, 54}.contains(mp.x, mp.y)) {
        playSfx("sfx/but1.wav");
        enterScreen(ScreenState::Credits);
      } else if (Rect{248, 396, 333, 54}.contains(mp.x, mp.y)) {
        playSfx("sfx/but1.wav");
        optionsReturnScreen_ = ScreenState::MainMenu;
        enterScreen(ScreenState::Options);
      } else if (Rect{248, 451, 333, 54}.contains(mp.x, mp.y)) {
        playSfx("sfx/but1.wav");
        confirmAction_ = ConfirmAction::QuitGame;
        confirmText_ = menuStrings_["reallyquit"].empty() ? "Вы действительно хотите выйти из игры?" : menuStrings_["reallyquit"];
      }
      break;
    }

    case ScreenState::EpisodeSelect: {
      for (size_t i = 0; i < briefings_.size(); ++i) {
        const BriefingInfo& bi = briefings_[i];
        if (Rect{bi.menuX, bi.menuY, 75, 60}.contains(mp.x, mp.y)) {
          playSfx("sfx/but1.wav");
          previewLevelIdx_ = (int)i;
          if (doubleClick) {
            selectLevel(previewLevelIdx_);
            enterScreen(ScreenState::InGame);
          }
          return;
        }
      }
      // Back to Main Menu button
      if (Rect{25, 520, 340, 55}.contains(mp.x, mp.y)) {
        playSfx("sfx/but1.wav");
        enterScreen(ScreenState::MainMenu);
        return;
      }
      // Start Episode button ("Съемка")
      if (Rect{400 + 270, 480, 110, 95}.contains(mp.x, mp.y)) {
        playSfx("sfx/but1.wav");
        selectLevel(previewLevelIdx_);
        enterScreen(ScreenState::InGame);
        return;
      }
      break;
    }

    case ScreenState::PauseMenu: {
      if (Rect{33, 170, 333, 54}.contains(mp.x, mp.y) || Rect{400 + 270, 480, 110, 95}.contains(mp.x, mp.y)) {
        playSfx("sfx/but1.wav");
        enterScreen(ScreenState::InGame);
      } else if (Rect{33, 225, 333, 54}.contains(mp.x, mp.y)) {
        playSfx("sfx/but1.wav");
        confirmAction_ = ConfirmAction::RestartEpisode;
        confirmText_ = menuStrings_["mm_restartlevel"].empty() ? "Вы действительно хотите начать съемки заново?"
                                                               : menuStrings_["mm_restartlevel"];
      } else if (Rect{33, 280, 333, 54}.contains(mp.x, mp.y)) {
        playSfx("sfx/but1.wav");
        enterScreen(ScreenState::EpisodeSelect);
      } else if (Rect{33, 335, 333, 54}.contains(mp.x, mp.y)) {
        playSfx("sfx/but1.wav");
        optionsReturnScreen_ = ScreenState::PauseMenu;
        enterScreen(ScreenState::Options);
      } else if (Rect{33, 390, 333, 54}.contains(mp.x, mp.y)) {
        playSfx("sfx/but1.wav");
        confirmAction_ = ConfirmAction::AbortEpisode;
        confirmText_ = menuStrings_["mm_exitlevel"].empty() ? "Вы действительно хотите завершить серию?"
                                                            : menuStrings_["mm_exitlevel"];
      }
      break;
    }

    case ScreenState::Options: {
      if (Rect{251, 248, 36, 36}.contains(mp.x, mp.y)) {
        playSfx("sfx/but1.wav");
        muteMaster_ = !muteMaster_;
        applyAudioVolumes();
      } else if (Rect{251, 292, 36, 36}.contains(mp.x, mp.y)) {
        playSfx("sfx/but1.wav");
        muteMusic_ = !muteMusic_;
        applyAudioVolumes();
      } else if (Rect{262, 335, 220, 28}.contains(mp.x, mp.y)) {
        playSfx("sfx/but1.wav");
        confirmAction_ = ConfirmAction::ResetProgress;
        confirmText_ = menuStrings_["mm_resetuserdata"].empty()
                           ? "Вы действительно хотите удалить сохраненные игры?"
                           : menuStrings_["mm_resetuserdata"];
      } else if (Rect{296, 367, 250, 19}.contains(mp.x, mp.y)) {
        playSfx("sfx/but1.wav");
        optHighQuality_ = !optHighQuality_;
      } else if (Rect{296, 388, 250, 19}.contains(mp.x, mp.y)) {
        playSfx("sfx/but1.wav");
        optTimeLimit_ = !optTimeLimit_;
      } else if (Rect{296, 409, 250, 19}.contains(mp.x, mp.y)) {
        playSfx("sfx/but1.wav");
        optAutoScroll_ = !optAutoScroll_;
      } else if (Rect{296, 430, 250, 19}.contains(mp.x, mp.y)) {
        playSfx("sfx/but1.wav");
        optColoredCursor_ = !optColoredCursor_;
      } else if (Rect{262, 455, 115, 32}.contains(mp.x, mp.y)) {
        // Cancel: restore backed up options
        playSfx("sfx/but1.wav");
        masterVolume_ = bakMasterVolume_;
        musicVolume_ = bakMusicVolume_;
        muteMaster_ = bakMuteMaster_;
        muteMusic_ = bakMuteMusic_;
        optHighQuality_ = bakHighQuality_;
        optTimeLimit_ = bakTimeLimit_;
        optAutoScroll_ = bakAutoScroll_;
        optColoredCursor_ = bakColoredCursor_;
        applyAudioVolumes();
        enterScreen(optionsReturnScreen_);
      } else if (Rect{402, 455, 115, 32}.contains(mp.x, mp.y)) {
        // OK: save options
        playSfx("sfx/but1.wav");
        saveUserProgress();
        enterScreen(optionsReturnScreen_);
      }
      break;
    }

    case ScreenState::Credits: {
      playSfx("sfx/but1.wav");
      enterScreen(ScreenState::MainMenu);
      break;
    }

    case ScreenState::LevelSummary: {
      if (Rect{125 + 190, 70 + 312, 115, 32}.contains(mp.x, mp.y)) {
        playSfx("sfx/but1.wav");
        selectLevel(currentLevelIdx_);
        enterScreen(ScreenState::InGame);
      } else if (Rect{125 + 342, 70 + 312, 115, 32}.contains(mp.x, mp.y)) {
        playSfx("sfx/but1.wav");
        if (summarySuccess_ && currentLevelIdx_ + 1 < (int)levels_.size()) {
          previewLevelIdx_ = currentLevelIdx_ + 1;
        }
        enterScreen(ScreenState::EpisodeSelect);
      }
      break;
    }

    case ScreenState::SeasonVictory: {
      playSfx("sfx/but1.wav");
      enterScreen(ScreenState::EpisodeSelect);
      break;
    }

    default:
      break;
  }
}

void GameEngine::toggleFullscreen() {
  fullscreen_ = !fullscreen_;
  SDL_SetWindowFullscreen(window_, fullscreen_ ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
  SDL_GetRendererOutputSize(ren_, &winW_, &winH_);
}

void GameEngine::renderCursor() {
  const float scale = std::max(1.0f, std::min(float(winW_) / 800.0f, float(winH_) / 480.0f));
  if (screen_ == ScreenState::InGame && selectedInv_ >= 0 && selectedInv_ < (int)inventory_.size()) {
    auto it = level_.inventar.find(toLowerAscii(inventory_[selectedInv_]));
    if (it != level_.inventar.end()) {
      if (const TexEntry* itex = getTexture(it->second.gfxStd)) {
        SDL_FRect dst{float(mouseX_) + 10.0f * scale, float(mouseY_) + 10.0f * scale,
                      float(itex->w) * 0.75f * scale, float(itex->h) * 0.75f * scale};
        SDL_RenderCopyF(ren_, itex->tex, nullptr, &dst);
      }
    }
  }
  const std::string& curPath = (screen_ == ScreenState::InGame) ? cursorVpath_ : std::string("gfx/cursor/normal.tga");
  if (const TexEntry* ctex = getTexture(curPath)) {
    SDL_FRect dst{float(mouseX_), float(mouseY_), float(ctex->w) * scale, float(ctex->h) * scale};
    SDL_RenderCopyF(ren_, ctex->tex, nullptr, &dst);
  }
}

void GameEngine::render() {
  SDL_SetRenderDrawColor(ren_, 14, 16, 22, 255);
  SDL_RenderClear(ren_);

  switch (screen_) {
    case ScreenState::MainMenu:
      renderMainMenu();
      break;
    case ScreenState::EpisodeSelect:
      renderEpisodeSelect();
      break;
    case ScreenState::PauseMenu:
      renderPauseMenu();
      break;
    case ScreenState::Options:
      renderOptionsScreen();
      break;
    case ScreenState::Credits:
      renderCreditsScreen();
      break;
    case ScreenState::SeasonVictory:
      renderSeasonVictoryScreen();
      break;
    case ScreenState::InGame:
      renderWorld();
      renderHud();
      break;
    case ScreenState::LevelSummary:
      renderWorld();
      renderHud();
      renderLevelSummaryOverlay();
      break;
  }

  renderConfirmModal();
  renderCursor();
  if (!pendingScreenshotFile_.empty()) {
    saveScreenshot(pendingScreenshotFile_);
    pendingScreenshotFile_.clear();
  }
  SDL_RenderPresent(ren_);
}

void GameEngine::saveScreenshot(const std::string& file) {
  Image shot;
  shot.width = winW_;
  shot.height = winH_;
  shot.rgba.resize(size_t(winW_) * winH_ * 4);
  if (SDL_RenderReadPixels(ren_, nullptr, SDL_PIXELFORMAT_RGBA32, shot.rgba.data(), winW_ * 4) == 0) {
    auto png = encodePng(shot);
    writeWholeFile(pathFromUtf8(file), png.data(), png.size());
  }
}

int GameEngine::run() {
  uint32_t lastTick = SDL_GetTicks();
  int simulatedTicks = 0;

  while (running_) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      switch (e.type) {
        case SDL_QUIT:
          running_ = false;
          break;
        case SDL_WINDOWEVENT:
          if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            SDL_GetRendererOutputSize(ren_, &winW_, &winH_);
          }
          break;
        case SDL_MOUSEMOTION: {
          int ww, wh;
          SDL_GetWindowSize(window_, &ww, &wh);
          mouseX_ = e.motion.x * winW_ / std::max(1, ww);
          mouseY_ = e.motion.y * winH_ / std::max(1, wh);
          if (screen_ == ScreenState::InGame && camDragging_) {
            camX_ += e.motion.xrel * float(winW_) / std::max(1, ww);
            camY_ += e.motion.yrel * float(winH_) / std::max(1, wh);
          }
          break;
        }
        case SDL_MOUSEBUTTONDOWN: {
          int ww, wh;
          SDL_GetWindowSize(window_, &ww, &wh);
          mouseX_ = e.button.x * winW_ / std::max(1, ww);
          mouseY_ = e.button.y * winH_ / std::max(1, wh);
          if (e.button.button == SDL_BUTTON_LEFT) mouseLeftDown_ = true;

          if (screen_ != ScreenState::InGame || confirmAction_ != ConfirmAction::None) {
            if (e.button.button == SDL_BUTTON_LEFT) {
              onMenuClick(screenToMenu(mouseX_, mouseY_), e.button.clicks >= 2);
            }
          } else {
            Point wp = screenToWorld(mouseX_, mouseY_);
            updateHover(wp.x, wp.y, mouseX_, mouseY_);
            if (e.button.button == SDL_BUTTON_LEFT) {
              onLeftClick(wp.x, wp.y, mouseX_, mouseY_, e.button.clicks >= 2);
            } else if (e.button.button == SDL_BUTTON_RIGHT) {
              onRightClick(wp.x, wp.y, mouseX_, mouseY_);
            } else if (e.button.button == SDL_BUTTON_MIDDLE) {
              fullHouseView_ = false;
              camDragging_ = true;
            }
          }
          break;
        }
        case SDL_MOUSEBUTTONUP:
          if (e.button.button == SDL_BUTTON_LEFT) mouseLeftDown_ = false;
          if (e.button.button == SDL_BUTTON_MIDDLE) camDragging_ = false;
          break;
        case SDL_KEYDOWN: {
          switch (e.key.keysym.sym) {
            case SDLK_ESCAPE:
              if (confirmAction_ != ConfirmAction::None) {
                confirmAction_ = ConfirmAction::None;
              } else if (screen_ == ScreenState::InGame) {
                if (selectedInv_ >= 0) selectedInv_ = -1;
                else {
                  playSfx("sfx/but1.wav");
                  enterScreen(ScreenState::PauseMenu);
                }
              } else if (screen_ == ScreenState::PauseMenu) {
                playSfx("sfx/but1.wav");
                enterScreen(ScreenState::InGame);
              } else if (screen_ == ScreenState::EpisodeSelect || screen_ == ScreenState::Credits) {
                playSfx("sfx/but1.wav");
                enterScreen(ScreenState::MainMenu);
              } else if (screen_ == ScreenState::Options) {
                playSfx("sfx/but1.wav");
                enterScreen(optionsReturnScreen_);
              } else if (screen_ == ScreenState::LevelSummary || screen_ == ScreenState::SeasonVictory) {
                playSfx("sfx/but1.wav");
                enterScreen(ScreenState::EpisodeSelect);
              } else if (screen_ == ScreenState::MainMenu) {
                confirmAction_ = ConfirmAction::QuitGame;
                confirmText_ = menuStrings_["reallyquit"].empty() ? "Вы действительно хотите выйти из игры?"
                                                                  : menuStrings_["reallyquit"];
              }
              break;
            case SDLK_SPACE:
              if (screen_ == ScreenState::InGame) {
                paused_ = !paused_;
                statusBanner_ = paused_ ? "ПАУЗА" : "Игра продолжается";
                bannerTicks_ = kTicksPerSecond * 2;
              }
              break;
            case SDLK_v:
            case SDLK_TAB:
              if (screen_ == ScreenState::InGame) {
                fullHouseView_ = !fullHouseView_;
                statusBanner_ = fullHouseView_ ? "Камера: весь дом" : "Камера: слежение за Вуди";
                bannerTicks_ = kTicksPerSecond * 2;
              }
              break;
            case SDLK_h:
              showDebug_ = !showDebug_;
              break;
            case SDLK_r:
              if (screen_ == ScreenState::InGame) {
                confirmAction_ = ConfirmAction::RestartEpisode;
                confirmText_ = menuStrings_["mm_restartlevel"].empty() ? "Вы действительно хотите начать съемки заново?"
                                                                       : menuStrings_["mm_restartlevel"];
              } else if (screen_ == ScreenState::LevelSummary) {
                selectLevel(currentLevelIdx_);
                enterScreen(ScreenState::InGame);
              }
              break;
            case SDLK_F11:
            case SDLK_t:
              toggleFullscreen();
              break;
            case SDLK_LEFTBRACKET:
            case SDLK_F1:
              selectLevel(currentLevelIdx_ - 1);
              break;
            case SDLK_RIGHTBRACKET:
            case SDLK_F2:
              selectLevel(currentLevelIdx_ + 1);
              break;
            default:
              break;
          }
          break;
        }
      }
    }

    const bool fastSim = !opts_.screenshot.empty() || !opts_.recordDir.empty();
    if (fastSim) {
      tickSimulation();
      simulatedTicks++;

      if (!opts_.recordDir.empty()) {
        fs::create_directories(pathFromUtf8(opts_.recordDir));
        if (simulatedTicks == 1 || (simulatedTicks % opts_.recordStride) == 0) {
          char frameBuf[64];
          std::snprintf(frameBuf, sizeof frameBuf, "/frame_%05d.png", simulatedTicks);
          pendingScreenshotFile_ = opts_.recordDir + frameBuf;
          render();
        }
      }

      if (simulatedTicks >= opts_.screenshotTicks) {
        if (!opts_.screenshot.empty()) {
          pendingScreenshotFile_ = opts_.screenshot;
          render();
        }
        running_ = false;
        break;
      }
      continue;
    }

    uint32_t now = SDL_GetTicks();
    while (now - lastTick >= (uint32_t)kTickMs) {
      lastTick += kTickMs;
      tickSimulation();
      simulatedTicks++;
    }

    Point wp = screenToWorld(mouseX_, mouseY_);
    updateHover(wp.x, wp.y, mouseX_, mouseY_);
    render();

    if (!opts_.screenshot.empty() && simulatedTicks >= opts_.screenshotTicks) {
      pendingScreenshotFile_ = opts_.screenshot;
      render();
      running_ = false;
      break;
    }
    SDL_Delay(4);
  }
  return 0;
}

}  // namespace nfh
