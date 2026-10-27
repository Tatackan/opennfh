// nfh-viewer: interactive browser for the original game's resources.
#pragma once

#include <SDL.h>
#include <SDL_mixer.h>

#include <memory>
#include <string>
#include <vector>

#include "core/game_data.h"
#include "core/tga.h"
#include "viewer/text_renderer.h"

namespace nfh {

struct ViewerOptions {
  std::string game;        // --game PATH
  std::string filter;      // --filter TEXT: initial filter, first match selected
  bool play = false;       // --play: start animation / audio immediately
  std::string screenshot;  // --screenshot FILE.png: render a few frames, save, exit
};

class Viewer {
 public:
  Viewer();
  ~Viewer();

  bool init(const ViewerOptions& opts);
  int run();

 private:
  enum class Kind { None, Image, Audio, Text, Font, Other };

  void saveScreenshot(const std::string& file);
  ViewerOptions opts_;

  // --- setup / data ---
  bool openGame(const fs::path& p);
  void rebuildFilter();
  void select(int filteredIndex);
  void loadPreview();
  void unloadPreview();
  void buildSequence(const std::string& vpath);
  void showFrame(int idx);
  bool loadImageTexture(const std::string& vpath, std::string* err);

  // --- events ---
  void handleEvent(const SDL_Event& e);
  void handleKey(const SDL_KeyboardEvent& k);
  void ensureVisible();
  void moveSelection(int delta);

  // --- drawing ---
  void draw();
  void drawList(const SDL_Rect& area);
  void drawPreview(const SDL_Rect& area);
  void drawImage(const SDL_Rect& area);
  void drawText(const SDL_Rect& area);
  void drawStatus(const SDL_Rect& area);
  void drawCheckerboard(const SDL_Rect& r);
  void fillRect(const SDL_Rect& r, SDL_Color c);
  int listRowHeight() const;
  SDL_Rect listArea() const;
  SDL_Rect previewArea() const;

  void playAudio();
  void stopAudio();

  SDL_Window* window_ = nullptr;
  SDL_Renderer* ren_ = nullptr;
  bool audioOk_ = false;
  bool running_ = true;
  int winW_ = 1280, winH_ = 800;

  TextRenderer ui_, mono_, big_, sample_;

  std::unique_ptr<GameData> game_;
  fs::path exeDir_;
  std::string message_;  // error / hint shown when no game is loaded

  // File list
  std::string filter_;
  bool filterFocus_ = false;
  std::vector<int> filtered_;  // indices into game_->files()
  int selected_ = -1;          // index into filtered_
  int listScroll_ = 0;         // first visible row

  // Preview state
  Kind kind_ = Kind::None;
  std::string previewPath_;
  std::string previewInfo_;
  std::string previewError_;
  // image
  SDL_Texture* tex_ = nullptr;
  Image image_;
  float zoom_ = 1.0f;
  float panX_ = 0, panY_ = 0;
  bool fitMode_ = true;
  bool dragging_ = false;
  bool checker_ = true;
  // animation sequence (files named *_0000.tga, *_0001.tga, ...)
  std::vector<std::string> sequence_;
  std::vector<SDL_Point> seqOffsets_;  // per-frame offset from gfxdata.xml
  bool seqHasOffsets_ = false;
  SDL_Rect seqBox_{0, 0, 0, 0};        // union of all frames in offset space
  int seqIndex_ = 0;
  bool playing_ = false;
  int fps_ = 15;
  uint32_t lastFrameTick_ = 0;
  // text
  std::vector<std::string> lines_;
  int textScroll_ = 0;
  // audio
  std::vector<uint8_t> audioData_;
  Mix_Chunk* chunk_ = nullptr;
  Mix_Music* music_ = nullptr;
  uint32_t audioStartTick_ = 0;
  // font sample
  std::vector<uint8_t> fontData_;
};

}  // namespace nfh
