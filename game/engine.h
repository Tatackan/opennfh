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
  bool startInLevel = false;             // true if --level was explicitly passed
  std::string initialScreen;             // optional --screen mainmenu|episodes|options|pause|credits|summary
  std::string screenshot;                // optional --screenshot FILE.png for automated testing
  int screenshotTicks = 60;              // how many ticks to simulate before screenshot
  std::string recordDir;                 // optional --record-dir DIR to dump frames & anomaly log
  int recordStride = 2;                  // save every Nth tick when recording video frames
  int windowW = 0;                       // custom window width
  int windowH = 0;                       // custom window height
};

class GameEngine {
 public:
  enum class ScreenState {
    MainMenu,
    EpisodeSelect,
    InGame,
    PauseMenu,
    Options,
    Credits,
    LevelSummary,
    SeasonVictory,
  };

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

  struct BriefingInfo {
    std::string title;
    std::string hintText;
    std::string picText;
    std::string briefing;
    std::string buttonShortName;
    int menuX = 25;
    int menuY = 185;
  };

  struct LevelProgress {
    int bestQuota = 0;
    int bestTricks = 0;
    bool completed = false;
    bool perfect = false;
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
    int targetY = 0;
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
    bool isExitLevel = false;
  };

  struct RoutineStop {
    std::string room;
    std::string normalObj;
    std::string trickObj;
    std::string fallbackObj;
    std::string iconName;
    bool onlyWhenTricked = false;
  };

  // --- asset helpers ---
  const TexEntry* getTexture(const std::string& vpath);
  void clearTextures();
  void playSfx(const std::string& sfxPath);
  void startMusic(const std::string& musicVpath);
  void stopMusic();
  void applyAudioVolumes();

  // --- menu & profile data ---
  void loadMenuData();
  void loadUserProgress();
  void saveUserProgress() const;
  void resetUserProgress();
  void enterScreen(ScreenState st);
  void finishEpisode(bool timeExpired, bool caughtOut);

  float menuScale() const;
  Point screenToMenu(int sx, int sy) const;
  SDL_Rect menuRect(int x, int y, int w, int h) const;
  void drawMenuImage(const std::string& vpath, int x, int y);
  bool drawMenuButton(const std::string& stdImg, const std::string& hovImg, const std::string& downImg,
                      int x, int y, int w, int h, const std::string& caption, TextRenderer& font,
                      SDL_Color stdColor, SDL_Color hovColor, Point menuMouse, bool mouseDown, bool* outHovered = nullptr);
  void drawMenuText(TextRenderer& font, const std::string& text, int mx, int my, int mw, int mh,
                    SDL_Color c, int halign = 0, int valign = 0, float maxScaleMul = 1.0f);
  void drawMenuTextWrapped(TextRenderer& font, const std::string& text, int mx, int my, int mw, int mh,
                           SDL_Color c, int halign = 0, int valign = 0);

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
  void enqueueRoomWalk(std::deque<TaskStep>& q, const std::string& roomName, Point& curPos, Point targetPos, bool sneak);

  // --- player commands ---
  void updateHover(int worldX, int worldY, int screenX, int screenY);
  void onLeftClick(int worldX, int worldY, int screenX, int screenY, bool doubleClick);
  void onRightClick(int worldX, int worldY, int screenX, int screenY);
  void onMenuClick(Point mp, bool doubleClick);
  void queuePathToRoom(const std::string& destRoom, bool sneak);
  void queueNeighborToRoom(const std::string& destRoom, Point destPos = Point{-1, -1}, bool fast = false);
  const CombinationDef* findCombination(const std::string& targetName, const std::string& itemName) const;
  void toggleFullscreen();
  float getHudScale() const;

  // --- rendering ---
  void render();
  void renderWorld();
  void renderObject(const PlacedObject& po);
  bool isLevelUnlocked(int levelIdx) const;
  void renderActor(const ActorState& a);
  void renderWoodyProgressBar();
  void renderHud();
  void renderMainMenu();
  void renderEpisodeSelect();
  void renderLevelIntroOverlay();
  void startLevelWithSplash(int levelIdx);
  void renderPauseMenu();
  void renderBriefingRightPanel(int levelIdx, bool isInGameMenu);
  void renderOptionsScreen();
  void renderCreditsScreen();
  void renderLevelSummaryOverlay();
  void renderSeasonVictoryScreen();
  void renderConfirmModal();
  void renderCursor();
  void saveScreenshot(const std::string& file);

  Point screenToWorld(int sx, int sy) const;
  SDL_FRect worldToScreen(float wx, float wy, float ww, float wh) const;

  GameOptions opts_;
  std::string pendingScreenshotFile_;
  SDL_Window* window_ = nullptr;
  SDL_Renderer* ren_ = nullptr;
  bool audioOk_ = false;
  bool running_ = true;
  bool paused_ = false;
  bool showDebug_ = false;
  bool fullHouseView_ = true;
  bool fullscreen_ = false;
  int winW_ = 1280, winH_ = 800;

  ScreenState screen_ = ScreenState::MainMenu;
  ScreenState optionsReturnScreen_ = ScreenState::MainMenu;
  int previewLevelIdx_ = 0;  // selected level in EpisodeSelect
  int levelIntroPhase_ = 0;  // 0=none, 1=JoWooD TV dog logo, 2=Game logo & episode title
  int levelIntroTicks_ = 0;
  std::string currentMusicVpath_;

  // Confirmation modal state
  enum class ConfirmAction { None, QuitGame, ResetProgress, AbortEpisode, RestartEpisode, ExitEpisode };
  ConfirmAction confirmAction_ = ConfirmAction::None;
  std::string confirmText_;

  // Options state
  int masterVolume_ = 85;
  int musicVolume_ = 75;
  bool muteMaster_ = false;
  bool muteMusic_ = false;
  bool optHighQuality_ = true;
  bool optTimeLimit_ = true;
  bool optAutoScroll_ = true;
  bool optColoredCursor_ = true;
  // Backup of options before editing in Options screen
  int bakMasterVolume_ = 85;
  int bakMusicVolume_ = 75;
  bool bakMuteMaster_ = false;
  bool bakMuteMusic_ = false;
  bool bakHighQuality_ = true;
  bool bakTimeLimit_ = true;
  bool bakAutoScroll_ = true;
  bool bakColoredCursor_ = true;

  // Summary & credits state
  bool summarySuccess_ = false;
  bool summaryPerfect_ = false;
  bool summaryTimeExpired_ = false;
  int completedSeason_ = 0;  // 1..3 when SeasonVictory triggers
  int seasonTrophyTier_ = 0; // 0=default, 1=bronze, 2=silver, 3=gold
  float creditsScrollY_ = 560.0f;
  std::vector<std::pair<std::string, bool>> creditsLines_;  // (text, isHeader)

  TextRenderer fontUI_, fontBig_, fontHud_, fontSmall_, fontTitle_;
  TextRenderer fontComic_, fontComicBig_, fontComicSmall_, fontSeason_;
  std::unique_ptr<GameData> gd_;
  std::vector<LevelInfo> levels_;
  std::vector<BriefingInfo> briefings_;
  std::unordered_map<std::string, LevelProgress> progress_;
  std::unordered_map<std::string, std::string> menuStrings_;
  std::unordered_map<std::string, std::string> statusStrings_;
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
  bool camInitialized_ = false;
  float camEdgeOffsetX_ = 0.0f, camEdgeOffsetY_ = 0.0f;
  void resetCameraOnWoody();

  // Game state
  ActorState woody_;
  ActorState neighbor_;
  std::vector<ActorState> pets_;
  std::deque<TaskStep> woodyQueue_;
  bool woodyBusyInAnim_ = false;
  int woodyActionTicks_ = 0;
  int woodyActionTotalTicks_ = 0;

  // Thought bubble for "look"
  std::string lookBubbleText_;
  int lookBubbleTicks_ = 0;
  Point lookBubblePos_;

  // Neighbor AI state
  std::vector<RoutineStop> routine_;
  int routineIdx_ = 0;
  int currentRoutineIdx_ = 0;
  int interruptedRoutineIdx_ = -1;
  std::deque<TaskStep> neighborQueue_;
  bool neighborBusyInAnim_ = false;
  int neighborWaitTicks_ = 20;
  std::string currentBubbleIcon_;
  bool catchSequenceActive_ = false;
  int catchTicks_ = 0;
  int catchesCount_ = 0;
  bool neighborChasing_ = false;
  int neighborAlertTicks_ = 0;
  bool neighborHasLaxativeBeer_ = false;

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
  bool mouseLeftDown_ = false;
  std::string lastHoveredBtn_;
  int hoveredObject_ = -1;
  std::string hoveredRoom_;
  std::string hoverText_;
  int hoveredInvSlot_ = -1;
  int hoveredInvTicks_ = 0;
  std::string cursorVpath_ = "gfx/cursor/normal.tga";
};

}  // namespace nfh
