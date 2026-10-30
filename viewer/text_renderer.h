// Cached UTF-8 text rendering on top of SDL2_ttf.
#pragma once

#include <SDL.h>
#include <SDL_ttf.h>

#include <list>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/platform.h"

namespace nfh {

class TextRenderer {
 public:
  ~TextRenderer();

  // Opens the first usable font from `candidates` at `ptSize`. Returns false if none load.
  bool load(SDL_Renderer* r, const std::vector<fs::path>& candidates, int ptSize);
  bool loadFromMemory(SDL_Renderer* r, const std::vector<uint8_t>& ttf, int ptSize);
  bool ok() const { return font_ != nullptr; }
  const std::string& fontName() const { return fontName_; }

  int lineHeight() const { return font_ ? TTF_FontLineSkip(font_) : 16; }
  int width(const std::string& s);

  // Draws text, returns its width. Optional `maxWidth` clips the texture.
  int draw(const std::string& s, int x, int y, SDL_Color c, int maxWidth = 0);
  int drawScaled(const std::string& s, float x, float y, float scale, SDL_Color c);

  // halign: -1=left, 0=center, 1=right; valign: -1=top, 0=center, 1=bottom
  void drawAligned(const std::string& s, const SDL_Rect& box, SDL_Color c, int halign = 0, int valign = 0, float scale = 1.0f);
  void drawAlignedScaled(const std::string& s, const SDL_FRect& box, SDL_Color c, float scale, int halign = 0, int valign = 0);
  void drawWrapped(const std::string& s, const SDL_Rect& box, SDL_Color c, int halign = -1, int valign = -1, float scale = 1.0f);

  void clearCache();
  void close();

 private:
  struct Cached {
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0;
    std::list<std::string>::iterator lru;
  };
  SDL_Renderer* r_ = nullptr;
  TTF_Font* font_ = nullptr;
  std::vector<uint8_t> memFont_;
  std::string fontName_;
  std::unordered_map<std::string, Cached> cache_;
  std::list<std::string> lru_;
  static constexpr size_t kMaxCached = 1500;

  Cached* get(const std::string& s, SDL_Color c);
};

}  // namespace nfh
