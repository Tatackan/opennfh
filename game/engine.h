// Interactive game engine for OpenNFH (Stage 2: playable level prototype).
#pragma once

#include <SDL.h>
#include <SDL_mixer.h>

#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/game_data.h"
#include "game/level_data.h"
#include "viewer/text_renderer.h"

namespace nfh {

struct GameOptions {
  std::string gamePath;
  std::string levelName = "level_peep";  // first playable episode by default
  std::string screenshot;                // optional --screenshot FILE.png for automated testing
  int screenshotTicks = 60;              // how many 20 Hz ticks to simulate before screenshot
};

class GameEngine {
 public:
  GameEngine();
  ~GameEngine();

  bool init(const GameOptions& opts);
  int run();

 private:
  struct TexEntry {
    SDL_Texture* tex = nullptr;
    int w = 0;
    int h = 0;
  };

  struct ActorState {
    std::string name;
    std::string room;
    Point pos;
    int layer = 4;
    int facing = 2;  // 0=back, 1=right, 2=front, 3=left
    bool hidden = false;
    int hidingInObject = -1;
    std::string currentAnim = "ms2";
    std::string nextAnim = "ms2";
    int animFrame = 0;
  };

  struct TaskStep {
    enum class Type {
      WalkToX,
      TraverseDoor,
      PlayAction,
      InteractObject,
      ApplyCombination,
    };
    Type type = Type::WalkToX;
    int targetX = 0;
    bool sneak = false;

    // TraverseDoor fields
    int objectIndex = -1;     // doorIn or target object index
    int outObjectIndex = -1;  // doorOut object index
    std::string doorName;
    std::string targetRoom;
    Point outPos;
    std::string outAnim = "ms2";
    int enterTicks = 10;
    int totalTicks = 24;
    int elapsedTicks = 0;

    // PlayAction fields
    std::string actorAnim;
    std::string actorNextAnim = "ms2";
    std::string objAnim;
    std::string objNextAnim = "ms";
    int durationTicks = 0;
    bool showProgressBar = false;
    bool nonInterruptible = false;
    bool giveContainerItems = false;
    bool enterHideout = false;
    bool leaveHideout = false;
    bool applyComboOnFinish = false;
    CombinationDef combo;
    bool triggerPrankScore = false;
    TrickDef trick;
    std::string transformObjectTo;
    bool hideObjectOnFinish = false;
  };

  struct RoutineStop {
    std::string room;
    std::string normalObj;
    std::string trickObj;
    std::string iconName;
    bool onlyWhenTricked = false;
  };

  // --- asset helpers ---
  const TexEntry* getTexture(const std::string& vpath);
  void clearTextures();
  void playSfx(const std::string& sfxPath);
  void startMusic(const std::string& musicVpath);

  // --- level management ---
  bool selectLevel(int index);
  void buildNeighborRoutine();
  std::vector<RoomEdge> findRoomPath(const std::string& fromRoom, const std::string& toRoom) const;
  int findObjectIndex(const std::string& name, const std::string& room = {}) const;
  Rect objectWorldHitbox(const PlacedObject& po) const;
  std::string objectOriginalName(const std::string& trickObjName) const;
  int animFrameCount(const std::string& gfxName, const std::string& animName, int fallback = 12) const;
  int actorSpeed(const std::string& actorName, const std::string& speedName, int fallback) const;

  // --- simulation (12 Hz original game tick) ---
  void tickSimulation();
  void advanceObjectAnims();
  void advanceActorAnim(ActorState& a, bool* finishedOut = nullptr);
  void updateWoody();
  void updateNeighbor();
  void checkStealthDetection();
  void awardPrankScore(const std::string& objName, const TrickDef& trick);
  void enqueueNeighborObjectActions(int objIdx);

  // --- player commands ---
  void updateHover(int worldX, int worldY, int screenX, int screenY);
  void onLeftClick(int worldX, int worldY, int screenX, int screenY, bool doubleClick);
  void onRightClick(int worldX, int worldY, int screenX, int screenY);
  void queuePathToRoom(const std::string& destRoom, bool sneak);
  const CombinationDef* findCombination(const std::string& targetName, const std::string& itemName) const;

  // --- rendering ---
  void render();
  void renderWorld();
  void renderObject(const PlacedObject& po);
  void renderActor(const ActorState& a);
  void renderHud();
  void renderCursor();
  void saveScreenshot(const std::string& file);

  Point screenToWorld(int sx, int sy) const;
  SDL_FRect worldToScreen(float wx, float wy, float ww, float wh) const;

  GameOptions opts_;
  SDL_Window* window_ = nullptr;
  SDL_Renderer* ren_ = nullptr;
  bool audioOk_ = false;
  bool running_ = true;
  bool paused_ = false;
  bool showDebug_ = false;
  bool fullHouseView_ = true;
  int winW_ = 1280, winH_ = 800;

  TextRenderer fontUI_, fontBig_, fontHud_;
  std::unique_ptr<GameData> gd_;
  std::vector<LevelInfo> levels_;
  int currentLevelIdx_ = 0;
  LevelData level_;

  std::unordered_map<std::string, TexEntry> texCache_;
  std::unordered_map<std::string, Mix_Chunk*> sfxCache_;
  std::unordered_map<std::string, std::vector<uint8_t>> sfxMem_;
  std::vector<uint8_t> musicMem_;
  Mix_Music* music_ = nullptr;

  // Camera
  float camX_ = 0.0f, camY_ = 0.0f;
  float camZoom_ = 1.0f;
  bool camDragging_ = false;

  // Game state
  ActorState woody_;
  ActorState neighbor_;
  std::deque<TaskStep> woodyQueue_;
  bool woodyBusyInAnim_ = false;
  int woodyActionTicks_ = 0;
  int woodyActionTotalTicks_ = 0;

  // Neighbor AI state
  std::vector<RoutineStop> routine_;
  int routineIdx_ = 0;
  std::deque<TaskStep> neighborQueue_;
  bool neighborBusyInAnim_ = false;
  int neighborWaitTicks_ = 20;
  std::string currentBubbleIcon_;
  bool catchSequenceActive_ = false;
  int catchTicks_ = 0;

  // Inventory & score
  std::vector<std::string> inventory_;
  int selectedInv_ = -1;  // index into inventory_
  int invScroll_ = 0;
  int quota_ = 0;         // current % rating
  int lastQuotaGain_ = 0;
  int tricksDone_ = 0;
  int angerTicks_ = 0;
  int angerMaxTicks_ = 200;
  int remainingTicks_ = 0;
  std::string statusBanner_;
  int bannerTicks_ = 0;

  // Hover & cursor
  int mouseX_ = 0, mouseY_ = 0;
  int hoveredObject_ = -1;
  std::string hoveredRoom_;
  std::string hoverText_;
  std::string cursorVpath_ = "gfx/cursor/normal.tga";
};

}  // namespace nfh
