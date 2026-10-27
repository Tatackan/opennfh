#include "game/engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <queue>
#include <unordered_set>

#include "core/platform.h"
#include "core/png_writer.h"
#include "core/tga.h"

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
  window_ = SDL_CreateWindow("OpenNFH — Как достать соседа", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, winW_, winH_,
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
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

  // Load fonts (prefer original game's acmesa.TTF for numbers, system font for full Cyrillic including 'ё')
  auto sysFonts = systemFonts();
  fontUI_.load(ren_, sysFonts, 16);
  fontBig_.load(ren_, sysFonts, 22);
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

  int startIdx = 0;
  for (size_t i = 0; i < levels_.size(); ++i) {
    if (toLowerAscii(levels_[i].name) == toLowerAscii(opts.levelName)) {
      startIdx = (int)i;
      break;
    }
  }
  if (!selectLevel(startIdx)) return false;
  startMusic("sfx/music/ingame1_slow.mp3");
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
  if (!audioOk_ || sfxPath.empty()) return;
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
  if (!audioOk_) return;
  if (music_) {
    Mix_FreeMusic(music_);
    music_ = nullptr;
  }
  if (gd_->read(musicVpath, musicMem_)) {
    music_ = Mix_LoadMUS_RW(SDL_RWFromConstMem(musicMem_.data(), (int)musicMem_.size()), 1);
    if (music_) Mix_PlayMusic(music_, -1);
  }
}

bool GameEngine::selectLevel(int index) {
  if (levels_.empty()) return false;
  currentLevelIdx_ = (index % (int)levels_.size() + (int)levels_.size()) % (int)levels_.size();
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

  buildNeighborRoutine();

  std::string title = "OpenNFH — " + level_.info.name + "  (мин. рейтинг: " + std::to_string(level_.info.minQuota) +
                      "%, пакостей: " + std::to_string(level_.info.reachable) + ")";
  SDL_SetWindowTitle(window_, title.c_str());
  statusBanner_ = "Уровень: " + level_.info.name + "  ([ / ] — сменить уровень, V — камера, R — рестарт)";
  bannerTicks_ = kTicksPerSecond * 5;
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

  for (const PlacedObject& po : level_.objects) {
    if (po.isDoor) continue;
    const ObjectDef* od = level_.findObjectDef(po.name);
    if (!od) continue;

    bool hasNormalNeighborAct = false;
    for (const ActionDef& a : od->actions) {
      if (toLowerAscii(a.actor) == "neighbor" && a.name != "clean" && a.name != "repair") {
        hasNormalNeighborAct = true;
        break;
      }
    }

    std::string trickObjName;
    bool hasTrickNeighborAct = false;
    for (const auto& c : level_.combinations) {
      if (toLowerAscii(c.target) == toLowerAscii(po.name) && c.isTrick) {
        trickObjName = c.resultObject;
        if (const ObjectDef* tod = level_.findObjectDef(trickObjName)) {
          for (const ActionDef& a : tod->actions) {
            if (toLowerAscii(a.actor) == "neighbor") {
              hasTrickNeighborAct = true;
              break;
            }
          }
        }
        break;
      }
    }

    if (!hasNormalNeighborAct && !hasTrickNeighborAct) continue;

    RoutineStop stop;
    stop.room = po.roomName;
    stop.normalObj = po.name;
    stop.trickObj = trickObjName;
    stop.onlyWhenTricked = !hasNormalNeighborAct;

    size_t slash = po.name.find('/');
    std::string shortName = toLowerAscii(slash == std::string::npos ? po.name : po.name.substr(slash + 1));
    if (level_.icons.count(shortName)) stop.iconName = shortName;
    else if (!level_.icons.empty()) stop.iconName = level_.icons.begin()->first;
    routine_.push_back(std::move(stop));
  }
  for (const auto& st : routine_) {
    if (!st.onlyWhenTricked && !st.iconName.empty()) {
      currentBubbleIcon_ = st.iconName;
      break;
    }
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

  // Check if mouse is over bottom HUD
  if (screenY >= winH_ - 110) {
    int hudLeft = (winW_ - 800) / 2;
    for (int slot = 0; slot < 5; ++slot) {
      int idx = invScroll_ + slot;
      if (idx >= (int)inventory_.size()) break;
      Rect r{hudLeft + 123 + 41 + slot * 75, winH_ - 89 + 26, 60, 55};
      if (r.contains(screenX, screenY)) {
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
      cursorVpath_ = (combo && combo->isTrick) ? "gfx/cursor/use_with.tga" : "gfx/cursor/not_possible.tga";
      return;
    }

    // Check bare-hand combination (like twisting the TV antenna in level_peep)
    if (const CombinationDef* bareCombo = findCombination(po.name, "")) {
      if (bareCombo->isTrick) {
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
      cursorVpath_ = (roomCombo && roomCombo->isTrick) ? "gfx/cursor/use_with.tga" : "gfx/cursor/not_possible.tga";
    } else {
      std::string floorTitle = level_.getString("floor", "name");
      hoverText_ = (floorTitle.empty() ? "Идти" : floorTitle);
      cursorVpath_ = "gfx/cursor/goto.tga";
    }
  }
}

void GameEngine::queuePathToRoom(const std::string& destRoom, bool sneak) {
  std::string curRoom = woody_.room;
  if (!woodyQueue_.empty()) {
    for (auto it = woodyQueue_.rbegin(); it != woodyQueue_.rend(); ++it) {
      if (!it->targetRoom.empty()) {
        curRoom = it->targetRoom;
        break;
      }
    }
  }
  auto edges = findRoomPath(curRoom, destRoom);
  for (const RoomEdge& e : edges) {
    const ObjectDef* dIn = level_.findObjectDef(e.doorIn);
    const ObjectDef* dOut = level_.findObjectDef(e.doorOut);
    int inIdx = findObjectIndex(e.doorIn, curRoom);
    int outIdx = findObjectIndex(e.doorOut, e.targetRoom);
    Point inPos = dIn ? dIn->hotspotOr("woody", Point{450, 400}) : Point{450, 400};
    if (inIdx >= 0) {
      inPos.x += level_.objects[inIdx].position.x;
      inPos.y += level_.objects[inIdx].position.y;
    }
    if (const RoomDef* rIn = level_.findRoom(curRoom)) {
      int lo = std::min(rIn->path1.x, rIn->path2.x);
      int hi = std::max(rIn->path1.x, rIn->path2.x);
      if (hi > lo) inPos.x = std::clamp(inPos.x, lo, hi);
    }
    Point outPos = dOut ? dOut->hotspotOr("woody_out", dOut->hotspotOr("woody", Point{450, 400})) : Point{450, 400};
    if (outIdx >= 0) {
      outPos.x += level_.objects[outIdx].position.x;
      outPos.y += level_.objects[outIdx].position.y;
    }
    if (const RoomDef* rOut = level_.findRoom(e.targetRoom)) {
      int lo = std::min(rOut->path1.x, rOut->path2.x);
      int hi = std::max(rOut->path1.x, rOut->path2.x);
      if (hi > lo) outPos.x = std::clamp(outPos.x, lo, hi);
    }

    TaskStep walk{};
    walk.type = TaskStep::Type::WalkToX;
    walk.targetX = inPos.x;
    walk.sneak = sneak;
    woodyQueue_.push_back(walk);

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
        if (aEn->timeTicks > 0) enterFrames = aEn->timeTicks;
      }
    }
    if (dOut) {
      if (const ActionDef* aLv = dOut->findAction("leave", "woody")) {
        if (!aLv->actorNextAnim.empty()) doorStep.outAnim = aLv->actorNextAnim;
      }
    }
    doorStep.enterTicks = std::max(1, enterFrames);
    doorStep.totalTicks = std::max(doorStep.enterTicks, leaveFrames);
    woodyQueue_.push_back(doorStep);

    curRoom = e.targetRoom;
  }
}

void GameEngine::onLeftClick(int worldX, int worldY, int screenX, int screenY, bool doubleClick) {
  if (catchSequenceActive_) return;

  // Check HUD clicks first
  if (screenY >= winH_ - 110) {
    int hudLeft = (winW_ - 800) / 2;
    for (int slot = 0; slot < 5; ++slot) {
      int idx = invScroll_ + slot;
      if (idx >= (int)inventory_.size()) break;
      Rect r{hudLeft + 123 + 41 + slot * 75, winH_ - 89 + 26, 60, 55};
      if (r.contains(screenX, screenY)) {
        selectedInv_ = (selectedInv_ == idx) ? -1 : idx;
        playSfx("sfx/but1.wav");
        return;
      }
    }
    return;
  }

  // Preserve active non-interruptible step (e.g. mid-door traversal or mid-hideout exit/entry)
  if (!woodyQueue_.empty()) {
    const TaskStep& front = woodyQueue_.front();
    bool keepFront = (front.type == TaskStep::Type::TraverseDoor && front.elapsedTicks > 0) ||
                     (front.type == TaskStep::Type::PlayAction && front.nonInterruptible && front.elapsedTicks > 0);
    if (keepFront) {
      TaskStep saved = front;
      woodyQueue_.clear();
      woodyQueue_.push_back(saved);
    } else {
      if (front.type == TaskStep::Type::PlayAction && front.objectIndex >= 0 &&
          front.objectIndex < (int)level_.objects.size()) {
        PlacedObject& po = level_.objects[front.objectIndex];
        if (po.currentAnim == "open") po.currentAnim = "ms";
      }
      woodyQueue_.clear();
      woodyActionTicks_ = woodyActionTotalTicks_ = 0;
    }
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

  if (hoveredObject_ >= 0) {
    const PlacedObject& po = level_.objects[hoveredObject_];
    const ObjectDef* od = level_.findObjectDef(po.name);

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

    // Check combination (with held item, or bare-hand trick, or auto-match if player has the needed item!)
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
      if (!combo) {
        for (const std::string& invItem : inventory_) {
          if (const CombinationDef* c = findCombination(po.name, invItem)) {
            if (c->isTrick) {
              combo = c;
              break;
            }
          }
        }
      }
    }

    Point targetPos = od ? od->hotspotOr("woody", Point{450, 400}) : Point{450, 400};
    targetPos.x += po.position.x;
    targetPos.y += po.position.y;

    queuePathToRoom(po.roomName, doubleClick);
    TaskStep walk{};
    walk.type = TaskStep::Type::WalkToX;
    walk.targetX = targetPos.x;
    walk.sneak = doubleClick;
    woodyQueue_.push_back(walk);

    if (combo && combo->isTrick) {
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

    if (!heldItem.empty()) {
      const CombinationDef* combo = findCombination(hoveredRoom_, heldItem);
      if (combo && combo->isTrick) {
        queuePathToRoom(hoveredRoom_, doubleClick);
        TaskStep walk{};
        walk.type = TaskStep::Type::WalkToX;
        walk.targetX = localX;
        walk.sneak = doubleClick;
        woodyQueue_.push_back(walk);

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
    TaskStep walk{};
    walk.type = TaskStep::Type::WalkToX;
    walk.targetX = localX;
    walk.sneak = doubleClick;
    woodyQueue_.push_back(walk);
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
        po.currentAnim = po.nextAnim.empty() ? "ms" : po.nextAnim;
        po.animFrame = 0;
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
      a.currentAnim = a.nextAnim.empty() ? "ms2" : a.nextAnim;
      a.animFrame = 0;
      if (finishedOut) *finishedOut = true;
    }
  }
}

void GameEngine::updateWoody() {
  bool animDone = false;
  advanceActorAnim(woody_, &animDone);

  if (woodyQueue_.empty()) return;
  TaskStep& step = woodyQueue_.front();

  switch (step.type) {
    case TaskStep::Type::WalkToX: {
      int dx = step.targetX - woody_.pos.x;
      std::string want = (dx >= 0) ? (step.sneak ? "sn1" : "mg1") : (step.sneak ? "sn3" : "mg3");
      int speed = actorSpeed("woody", want, step.sneak ? 5 : 17);
      if (std::abs(dx) <= speed) {
        woody_.pos.x = step.targetX;
        woody_.currentAnim = (dx >= 0) ? "ms1" : "ms3";
        woody_.nextAnim = woody_.currentAnim;
        woody_.animFrame = 0;
        woodyQueue_.pop_front();
      } else {
        woody_.pos.x += (dx > 0 ? speed : -speed);
        if (woody_.currentAnim != want) {
          woody_.currentAnim = want;
          woody_.nextAnim = want;
          woody_.animFrame = 0;
        }
      }
      break;
    }

    case TaskStep::Type::TraverseDoor: {
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
      if (step.elapsedTicks >= step.totalTicks) {
        woody_.room = step.targetRoom;
        woody_.pos = step.outPos;
        woody_.hidden = false;
        woody_.currentAnim = step.outAnim;
        woody_.nextAnim = step.outAnim;
        woody_.animFrame = 0;
        woodyQueue_.pop_front();
      }
      break;
    }

    case TaskStep::Type::PlayAction: {
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

      if (step.elapsedTicks >= step.durationTicks) {
        if (step.showProgressBar) {
          woodyActionTicks_ = woodyActionTotalTicks_ = 0;
        }
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
          if (step.applyComboOnFinish && step.combo.removeTarget) {
            po.name = step.combo.resultObject;
            po.currentAnim = "ms";
            po.nextAnim = "ms";
            po.animFrame = 0;
          }
        } else if (step.applyComboOnFinish && !step.targetRoom.empty()) {
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
          std::string trickTitle = level_.getString(step.combo.resultObject, "name");
          statusBanner_ = "Пакость готова: " + (trickTitle.empty() ? step.combo.resultObject : trickTitle) + "!";
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
      }
      break;
    }

    case TaskStep::Type::InteractObject: {
      int idx = step.objectIndex;
      woodyQueue_.pop_front();
      if (idx < 0 || idx >= (int)level_.objects.size()) break;
      PlacedObject& po = level_.objects[idx];
      const ObjectDef* od = level_.findObjectDef(po.name);
      if (!od) break;

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
          seq.push_back(stTake);
        } else {
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
      break;
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
      doCombo.applyComboOnFinish = true;
      doCombo.combo = combo;

      woodyQueue_.push_front(smileStep);
      woodyQueue_.push_front(doCombo);
      break;
    }
  }
}

void GameEngine::awardPrankScore(const std::string& objName, const TrickDef& trick) {
  int bonus = (angerTicks_ > 0) ? 5 : 0;
  lastQuotaGain_ = trick.quota + bonus;
  quota_ += lastQuotaGain_;
  tricksDone_++;
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

  // Special multi-phase handling for armchair (lir/sofa and lir/sofa_fartbag)
  if (lowerName == "lir/sofa" || lowerName == "lir/sofa_fartbag") {
    TaskStep enterSt{};
    enterSt.type = TaskStep::Type::PlayAction;
    enterSt.objectIndex = objIdx;
    enterSt.actorAnim = "inv";
    enterSt.actorNextAnim = "ms3";
    enterSt.objAnim = "neighbor_enter";
    enterSt.objNextAnim = isTricked ? "neighbor_surprise" : "neighbor_sit_remo";
    enterSt.durationTicks = animFrameCount(od->gfx, "neighbor_enter", 6);
    neighborQueue_.push_back(enterSt);

    if (isTricked) {
      TaskStep surpSt{};
      surpSt.type = TaskStep::Type::PlayAction;
      surpSt.objectIndex = objIdx;
      surpSt.actorAnim = "inv";
      surpSt.actorNextAnim = "ms3";
      surpSt.objAnim = "neighbor_surprise";
      surpSt.objNextAnim = "ms";
      surpSt.durationTicks = animFrameCount(od->gfx, "neighbor_surprise", 36);
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

      TaskStep sitSt{};
      sitSt.type = TaskStep::Type::PlayAction;
      sitSt.objectIndex = objIdx;
      sitSt.actorAnim = "inv";
      sitSt.objAnim = "neighbor_sit";
      sitSt.objNextAnim = "neighbor_leave";
      sitSt.durationTicks = animFrameCount(od->gfx, "neighbor_sit", 32);
      neighborQueue_.push_back(sitSt);

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
    return;
  }

  // Container handling for Neighbor (e.g. kit/fridge: open -> take -> close)
  if (od->hasFlag("container")) {
    TaskStep stOpen{};
    stOpen.type = TaskStep::Type::PlayAction;
    stOpen.objectIndex = objIdx;
    stOpen.actorAnim = "open";
    stOpen.actorNextAnim = "ms0";
    stOpen.objAnim = "open";
    stOpen.objNextAnim = "open";
    stOpen.durationTicks = animFrameCount("neighbor", "open", 10);
    neighborQueue_.push_back(stOpen);

    TaskStep stTake{};
    stTake.type = TaskStep::Type::PlayAction;
    stTake.objectIndex = objIdx;
    stTake.actorAnim = "take";
    stTake.actorNextAnim = "ms0";
    stTake.objAnim = "open";
    stTake.objNextAnim = "open";
    stTake.durationTicks = animFrameCount("neighbor", "take", 12);
    neighborQueue_.push_back(stTake);

    TaskStep stClose{};
    stClose.type = TaskStep::Type::PlayAction;
    stClose.objectIndex = objIdx;
    stClose.actorAnim = "close";
    stClose.actorNextAnim = "ms0";
    stClose.objAnim = "ms";
    stClose.objNextAnim = "ms";
    stClose.durationTicks = animFrameCount("neighbor", "close", 10);
    neighborQueue_.push_back(stClose);
    return;
  }

  // Tricked object handling
  if (isTricked) {
    const ActionDef* prankAct = nullptr;
    const ActionDef* cleanAct = nullptr;
    for (const ActionDef& a : od->actions) {
      if (toLowerAscii(a.actor) != "neighbor") continue;
      if (a.name == "clean" || a.name == "repair") cleanAct = &a;
      else if (!prankAct) prankAct = &a;
    }

    if (prankAct) {
      TaskStep st{};
      st.type = TaskStep::Type::PlayAction;
      st.objectIndex = objIdx;
      st.actorAnim = prankAct->actorAnim.empty() ? "shout0" : prankAct->actorAnim;
      st.actorNextAnim = prankAct->actorNextAnim.empty() ? "ms2" : prankAct->actorNextAnim;
      st.objAnim = prankAct->objAnim;
      st.objNextAnim = (prankAct->objNextAnim.empty() || prankAct->objNextAnim == "inv") ? "ms" : prankAct->objNextAnim;
      if (prankAct->timeTicks > 0) {
        st.durationTicks = prankAct->timeTicks;
      } else if (st.actorAnim == "inv" && !st.objAnim.empty()) {
        st.durationTicks = animFrameCount(od->gfx, st.objAnim, 45);
      } else {
        st.durationTicks = animFrameCount("neighbor", st.actorAnim, 36);
      }
      st.triggerPrankScore = true;
      st.trick = trickIt->second;
      st.transformObjectTo = origName;
      st.hideObjectOnFinish = origName.empty();
      neighborQueue_.push_back(st);
    } else {
      TaskStep shoutSt{};
      shoutSt.type = TaskStep::Type::PlayAction;
      shoutSt.objectIndex = objIdx;
      shoutSt.actorAnim = "shout0";
      shoutSt.actorNextAnim = "ms0";
      shoutSt.durationTicks = animFrameCount("neighbor", "shout0", 24);
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
                                                     : animFrameCount("neighbor", clSt.actorAnim, 23);
        clSt.transformObjectTo = origName;
        clSt.hideObjectOnFinish = origName.empty();
        neighborQueue_.push_back(clSt);
      }
    }
    return;
  }

  // Normal routine object interaction (e.g. kit/binoculars peep, kit/whippedcream put_cream, etc.)
  for (const ActionDef& a : od->actions) {
    if (toLowerAscii(a.actor) != "neighbor" || a.name == "clean" || a.name == "repair") continue;
    TaskStep st{};
    st.type = TaskStep::Type::PlayAction;
    st.objectIndex = objIdx;
    st.actorAnim = a.actorAnim.empty() ? "use_mid" : a.actorAnim;
    st.actorNextAnim = a.actorNextAnim.empty() ? "ms2" : a.actorNextAnim;
    st.objAnim = a.objAnim;
    st.objNextAnim = a.objNextAnim.empty() ? "ms" : a.objNextAnim;
    if (a.timeTicks > 0) {
      st.durationTicks = a.timeTicks;
    } else if (st.actorAnim == "inv" && !st.objAnim.empty()) {
      st.durationTicks = animFrameCount(od->gfx, st.objAnim, 36);
    } else {
      st.durationTicks = animFrameCount("neighbor", st.actorAnim, 24);
    }
    neighborQueue_.push_back(st);
    break;
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
        int hx = po.position.x + (od ? od->hotspotOr("neighbor", Point{neighbor_.pos.x, 420}).x : neighbor_.pos.x);
        if (std::abs(neighbor_.pos.x - hx) <= 60) {
          neighborQueue_.clear();
          enqueueNeighborObjectActions((int)i);
          return;
        }
      }
    }
  }

  if (!neighborQueue_.empty()) {
    TaskStep& step = neighborQueue_.front();
    switch (step.type) {
      case TaskStep::Type::WalkToX: {
        int dx = step.targetX - neighbor_.pos.x;
        std::string want = (dx >= 0) ? "mg1" : "mg3";
        int speed = actorSpeed("neighbor", want, 8);
        if (std::abs(dx) <= speed) {
          neighbor_.pos.x = step.targetX;
          neighbor_.currentAnim = (dx >= 0) ? "ms1" : "ms3";
          neighbor_.nextAnim = neighbor_.currentAnim;
          neighbor_.animFrame = 0;
          neighborQueue_.pop_front();
        } else {
          neighbor_.pos.x += (dx > 0 ? speed : -speed);
          if (neighbor_.currentAnim != want) {
            neighbor_.currentAnim = want;
            neighbor_.nextAnim = want;
            neighbor_.animFrame = 0;
          }
        }
        break;
      }

      case TaskStep::Type::TraverseDoor: {
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
        if (step.elapsedTicks >= step.totalTicks) {
          neighbor_.room = step.targetRoom;
          neighbor_.pos = step.outPos;
          neighbor_.hidden = false;
          neighbor_.currentAnim = step.outAnim;
          neighbor_.nextAnim = step.outAnim;
          neighbor_.animFrame = 0;
          neighborQueue_.pop_front();
        }
        break;
      }

      case TaskStep::Type::PlayAction: {
        if (step.elapsedTicks == 0) {
          if (step.triggerPrankScore && step.objectIndex >= 0 && step.objectIndex < (int)level_.objects.size()) {
            awardPrankScore(level_.objects[step.objectIndex].name, step.trick);
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
        }
        step.elapsedTicks++;
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
          neighbor_.hidden = false;
          neighbor_.currentAnim = step.actorNextAnim.empty() ? "ms2" : step.actorNextAnim;
          neighbor_.nextAnim = neighbor_.currentAnim;
          neighbor_.animFrame = 0;
          neighborQueue_.pop_front();
        }
        break;
      }

      case TaskStep::Type::InteractObject: {
        int idx = step.objectIndex;
        neighborQueue_.pop_front();
        enqueueNeighborObjectActions(idx);
        break;
      }

      default:
        neighborQueue_.pop_front();
        break;
    }
    return;
  }

  // Plan next routine stop (skip stops that are only visited when tricked unless currently tricked!)
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
    if (nidx >= 0) {
      targetObjIdx = nidx;
      chosenStop = &st;
      break;
    }
  }
  if (targetObjIdx < 0 || !chosenStop) return;
  if (!chosenStop->iconName.empty()) currentBubbleIcon_ = chosenStop->iconName;

  // Enqueue room path for Neighbor with synchronized door animations
  std::string curRoom = neighbor_.room;
  for (const RoomEdge& e : findRoomPath(curRoom, chosenStop->room)) {
    const ObjectDef* dIn = level_.findObjectDef(e.doorIn);
    const ObjectDef* dOut = level_.findObjectDef(e.doorOut);
    int inIdx = findObjectIndex(e.doorIn, curRoom);
    int outIdx = findObjectIndex(e.doorOut, e.targetRoom);
    Point inPos = dIn ? dIn->hotspotOr("neighbor", Point{450, 400}) : Point{450, 400};
    if (inIdx >= 0) {
      inPos.x += level_.objects[inIdx].position.x;
      inPos.y += level_.objects[inIdx].position.y;
    }
    if (const RoomDef* rIn = level_.findRoom(curRoom)) {
      int lo = std::min(rIn->path1.x, rIn->path2.x);
      int hi = std::max(rIn->path1.x, rIn->path2.x);
      if (hi > lo) inPos.x = std::clamp(inPos.x, lo, hi);
    }
    Point outPos = dOut ? dOut->hotspotOr("neighbor_out", dOut->hotspotOr("neighbor", Point{450, 400})) : Point{450, 400};
    if (outIdx >= 0) {
      outPos.x += level_.objects[outIdx].position.x;
      outPos.y += level_.objects[outIdx].position.y;
    }
    if (const RoomDef* rOut = level_.findRoom(e.targetRoom)) {
      int lo = std::min(rOut->path1.x, rOut->path2.x);
      int hi = std::max(rOut->path1.x, rOut->path2.x);
      if (hi > lo) outPos.x = std::clamp(outPos.x, lo, hi);
    }

    TaskStep walk{};
    walk.type = TaskStep::Type::WalkToX;
    walk.targetX = inPos.x;
    neighborQueue_.push_back(walk);

    TaskStep doorStep{};
    doorStep.type = TaskStep::Type::TraverseDoor;
    doorStep.doorName = e.doorIn;
    doorStep.objectIndex = inIdx;
    doorStep.outObjectIndex = outIdx;
    doorStep.targetRoom = e.targetRoom;
    doorStep.outPos = outPos;
    doorStep.outAnim = "ms2";
    int enterFrames = dIn ? animFrameCount(dIn->gfx, "neighbor_enter", 12) : 12;
    int leaveFrames = dOut ? animFrameCount(dOut->gfx, "neighbor_leave", 22) : 22;
    if (dIn) {
      if (const ActionDef* aEn = dIn->findAction("enter", "neighbor")) {
        if (aEn->timeTicks > 0) enterFrames = aEn->timeTicks;
      }
    }
    if (dOut) {
      if (const ActionDef* aLv = dOut->findAction("leave", "neighbor")) {
        if (!aLv->actorNextAnim.empty()) doorStep.outAnim = aLv->actorNextAnim;
      }
    }
    doorStep.enterTicks = std::max(1, enterFrames);
    doorStep.totalTicks = std::max(doorStep.enterTicks, leaveFrames);
    neighborQueue_.push_back(doorStep);

    curRoom = e.targetRoom;
  }

  const PlacedObject& targetPo = level_.objects[targetObjIdx];
  const ObjectDef* targetOd = level_.findObjectDef(targetPo.name);
  Point hs = targetOd ? targetOd->hotspotOr("neighbor", Point{450, 400}) : Point{450, 400};
  hs.x += targetPo.position.x;

  TaskStep walk{};
  walk.type = TaskStep::Type::WalkToX;
  walk.targetX = hs.x;
  neighborQueue_.push_back(walk);

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
      woody_.room = "fro";
      woody_.pos = Point{105, 218};
      woody_.hidden = false;
      woody_.currentAnim = "ms2";
      woody_.nextAnim = "ms2";
      woody_.animFrame = 0;
      woodyQueue_.clear();
      neighbor_.currentAnim = "ms2";
      neighbor_.nextAnim = "ms2";
      neighbor_.animFrame = 0;
      neighborWaitTicks_ = 12;
    }
    return;
  }

  if (woody_.hidden || neighbor_.hidden) return;
  if (toLowerAscii(woody_.room) != toLowerAscii(neighbor_.room)) return;

  // Caught in the same room!
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
  statusBanner_ = "СОСЕД ПОЙМАЛ ВУДИ! Возвращаемся на крыльцо...";
  bannerTicks_ = kTicksPerSecond * 3;
}

void GameEngine::tickSimulation() {
  if (paused_) return;
  if (bannerTicks_ > 0) bannerTicks_--;
  if (angerTicks_ > 0) angerTicks_--;
  if (remainingTicks_ > 0) remainingTicks_--;

  advanceObjectAnims();
  checkStealthDetection();
  if (!catchSequenceActive_) {
    updateWoody();
    updateNeighbor();
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

void GameEngine::renderObject(const PlacedObject& po) {
  if (!po.visible || po.currentAnim == "inv" || toLowerAscii(po.name) == "house") return;
  const RoomDef* r = level_.findRoom(po.roomName);
  const Point base{po.position.x + (r ? r->offset.x : 0), po.position.y + (r ? r->offset.y : 0)};

  const ObjectDef* od = level_.findObjectDef(po.name);
  const std::string gfxName = od ? od->gfx : po.name;
  const GfxObjectDef* gd = level_.findGfxObject(gfxName);
  if (!gd) return;

  auto it = gd->anims.find(toLowerAscii(po.currentAnim));
  if (it == gd->anims.end() || it->second.frames.empty()) return;
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
      float pbx = float(r->offset.x + a.pos.x - 30);
      float pby = float(r->offset.y + a.pos.y - 130);
      SDL_FRect bDst = worldToScreen(pbx, pby, float(pbBack->w), float(pbBack->h));
      SDL_RenderCopyF(ren_, pbBack->tex, nullptr, &bDst);
      float frac = 1.0f - float(woodyActionTicks_) / float(woodyActionTotalTicks_);
      int srcW = std::clamp(int(pbFront->w * frac), 0, pbFront->w);
      SDL_Rect src{0, 0, srcW, pbFront->h};
      SDL_FRect fDst = worldToScreen(pbx, pby, float(srcW), float(pbFront->h));
      SDL_RenderCopyF(ren_, pbFront->tex, &src, &fDst);
    }
  }
}

void GameEngine::renderWorld() {
  // Update camera placement
  const int availH = std::max(200, winH_ - 100);
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
        camX_ += (targetX - camX_) * 0.15f;
        camY_ += (targetY - camY_) * 0.15f;
      }
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
  // Original 800x600 bottom HUD anchored at the bottom center of the window
  const int hudX = (winW_ - 800) / 2;
  const int hudY = winH_ - 600;

  auto drawGui = [&](const std::string& vpath, int ox, int oy) {
    if (const TexEntry* t = getTexture(vpath)) {
      SDL_Rect dst{hudX + ox, hudY + oy, t->w, t->h};
      SDL_RenderCopy(ren_, t->tex, nullptr, &dst);
    }
  };

  // Darken strip behind HUD
  SDL_SetRenderDrawColor(ren_, 18, 20, 26, 235);
  SDL_Rect strip{0, winH_ - 95, winW_, 95};
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
      SDL_Rect dst{hudX + 85, hudY + 486 + dy, front->w, h};
      SDL_RenderCopy(ren_, front->tex, &src, &dst);
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
    fontUI_.draw(hoverText_, hudX + 135, hudY + 513, SDL_Color{255, 255, 255, 255}, 440);
  }

  for (int slot = 0; slot < 5; ++slot) {
    int idx = invScroll_ + slot;
    if (idx >= (int)inventory_.size()) break;
    int sx = hudX + 123 + 41 + slot * 75;
    int sy = hudY + 511 + 26;
    if (idx == selectedInv_) {
      SDL_SetRenderDrawColor(ren_, 240, 190, 60, 110);
      SDL_Rect hi{sx - 2, sy - 2, 64, 56};
      SDL_RenderFillRect(ren_, &hi);
    }
    auto it = level_.inventar.find(toLowerAscii(inventory_[idx]));
    if (it != level_.inventar.end()) {
      const std::string& iconPath = (idx == selectedInv_ && !it->second.gfxHover.empty()) ? it->second.gfxHover : it->second.gfxStd;
      if (const TexEntry* t = getTexture(iconPath)) {
        SDL_Rect dst{sx, sy, t->w, t->h};
        SDL_RenderCopy(ren_, t->tex, nullptr, &dst);
      }
    }
  }

  // Right: TV panel (timer, quota %, tricks count)
  drawGui("gfx/gui/ingame/interface_right.tga", 581, 471);
  char timeBuf[32];
  if (level_.info.timeTicks <= 0) {
    std::snprintf(timeBuf, sizeof timeBuf, "--:--");
  } else {
    int secs = std::max(0, remainingTicks_ / kTicksPerSecond);
    std::snprintf(timeBuf, sizeof timeBuf, "%02d:%02d", secs / 60, secs % 60);
  }
  fontHud_.draw(timeBuf, hudX + 581 + 98, hudY + 471 + 20, SDL_Color{140, 215, 215, 255});

  char quotaBuf[32];
  std::snprintf(quotaBuf, sizeof quotaBuf, "%d%%", quota_);
  SDL_Color qColor = (quota_ >= level_.info.minQuota) ? SDL_Color{120, 240, 130, 255} : SDL_Color{255, 255, 255, 255};
  fontHud_.draw(quotaBuf, hudX + 581 + 78, hudY + 471 + 90, qColor);

  char trickBuf[32];
  std::snprintf(trickBuf, sizeof trickBuf, "%d/%d", tricksDone_, level_.info.reachable);
  fontUI_.draw(trickBuf, hudX + 581 + 170, hudY + 471 + 68, SDL_Color{245, 240, 90, 255});

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

void GameEngine::renderCursor() {
  // If an inventory item is selected, draw its icon next to the cursor
  if (selectedInv_ >= 0 && selectedInv_ < (int)inventory_.size()) {
    auto it = level_.inventar.find(toLowerAscii(inventory_[selectedInv_]));
    if (it != level_.inventar.end()) {
      if (const TexEntry* itex = getTexture(it->second.gfxStd)) {
        SDL_Rect dst{mouseX_ + 10, mouseY_ + 10, itex->w * 3 / 4, itex->h * 3 / 4};
        SDL_RenderCopy(ren_, itex->tex, nullptr, &dst);
      }
    }
  }
  if (const TexEntry* ctex = getTexture(cursorVpath_)) {
    SDL_Rect dst{mouseX_, mouseY_, ctex->w, ctex->h};
    SDL_RenderCopy(ren_, ctex->tex, nullptr, &dst);
  }
}

void GameEngine::render() {
  SDL_SetRenderDrawColor(ren_, 20, 22, 28, 255);
  SDL_RenderClear(ren_);
  renderWorld();
  renderHud();
  renderCursor();
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
          if (camDragging_) {
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
          break;
        }
        case SDL_MOUSEBUTTONUP:
          if (e.button.button == SDL_BUTTON_MIDDLE) camDragging_ = false;
          break;
        case SDL_KEYDOWN: {
          switch (e.key.keysym.sym) {
            case SDLK_ESCAPE:
              if (selectedInv_ >= 0) selectedInv_ = -1;
              else running_ = false;
              break;
            case SDLK_SPACE:
              paused_ = !paused_;
              statusBanner_ = paused_ ? "ПАУЗА" : "Игра продолжается";
              bannerTicks_ = kTicksPerSecond * 2;
              break;
            case SDLK_v:
            case SDLK_TAB:
              fullHouseView_ = !fullHouseView_;
              statusBanner_ = fullHouseView_ ? "Камера: весь дом" : "Камера: слежение за Вуди";
              bannerTicks_ = kTicksPerSecond * 2;
              break;
            case SDLK_h:
              showDebug_ = !showDebug_;
              break;
            case SDLK_r:
              selectLevel(currentLevelIdx_);
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
      saveScreenshot(opts_.screenshot);
      running_ = false;
      break;
    }
    SDL_Delay(4);
  }
  return 0;
}

}  // namespace nfh
