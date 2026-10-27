#include "viewer/text_renderer.h"

#include <cstdio>

namespace nfh {

TextRenderer::~TextRenderer() { close(); }

void TextRenderer::close() {
  clearCache();
  if (font_) TTF_CloseFont(font_);
  font_ = nullptr;
}

bool TextRenderer::load(SDL_Renderer* r, const std::vector<fs::path>& candidates, int ptSize) {
  r_ = r;
  for (const fs::path& p : candidates) {
    std::error_code ec;
    if (!fs::exists(p, ec)) continue;
    // Read through our own I/O so non-ASCII paths work on Windows.
    std::vector<uint8_t> data;
    if (!readWholeFile(p, data)) continue;
    if (loadFromMemory(r, data, ptSize)) {
      fontName_ = pathToUtf8(p.filename());
      return true;
    }
  }
  return false;
}

bool TextRenderer::loadFromMemory(SDL_Renderer* r, const std::vector<uint8_t>& ttf, int ptSize) {
  close();
  r_ = r;
  memFont_ = ttf;  // TTF keeps reading from this memory
  SDL_RWops* rw = SDL_RWFromConstMem(memFont_.data(), (int)memFont_.size());
  font_ = TTF_OpenFontRW(rw, 1, ptSize);
  if (!font_) return false;
  TTF_SetFontHinting(font_, TTF_HINTING_LIGHT);
  fontName_ = "memory";
  return true;
}

void TextRenderer::clearCache() {
  for (auto& kv : cache_)
    if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
  cache_.clear();
  lru_.clear();
}

TextRenderer::Cached* TextRenderer::get(const std::string& s, SDL_Color c) {
  if (!font_ || s.empty()) return nullptr;
  char key[16];
  std::snprintf(key, sizeof key, "%02x%02x%02x%02x|", c.r, c.g, c.b, c.a);
  std::string k = key + s;
  auto it = cache_.find(k);
  if (it != cache_.end()) {
    lru_.splice(lru_.begin(), lru_, it->second.lru);
    return &it->second;
  }
  SDL_Surface* surf = TTF_RenderUTF8_Blended(font_, s.c_str(), c);
  if (!surf) return nullptr;
  Cached e;
  e.tex = SDL_CreateTextureFromSurface(r_, surf);
  e.w = surf->w;
  e.h = surf->h;
  SDL_FreeSurface(surf);
  if (!e.tex) return nullptr;

  if (cache_.size() >= kMaxCached) {
    auto& old = cache_[lru_.back()];
    SDL_DestroyTexture(old.tex);
    cache_.erase(lru_.back());
    lru_.pop_back();
  }
  lru_.push_front(k);
  e.lru = lru_.begin();
  return &(cache_[k] = e);
}

int TextRenderer::width(const std::string& s) {
  if (!font_ || s.empty()) return 0;
  int w = 0, h = 0;
  TTF_SizeUTF8(font_, s.c_str(), &w, &h);
  return w;
}

int TextRenderer::draw(const std::string& s, int x, int y, SDL_Color c, int maxWidth) {
  Cached* e = get(s, c);
  if (!e) return 0;
  int w = e->w;
  if (maxWidth > 0 && w > maxWidth) w = maxWidth;
  SDL_Rect src{0, 0, w, e->h};
  SDL_Rect dst{x, y, w, e->h};
  SDL_RenderCopy(r_, e->tex, &src, &dst);
  return w;
}

}  // namespace nfh
