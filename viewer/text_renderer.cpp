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

int TextRenderer::drawScaled(const std::string& s, float x, float y, float scale, SDL_Color c) {
  Cached* e = get(s, c);
  if (!e) return 0;
  SDL_FRect dst{x, y, float(e->w) * scale, float(e->h) * scale};
  SDL_RenderCopyF(r_, e->tex, nullptr, &dst);
  return int(e->w * scale);
}

void TextRenderer::drawAligned(const std::string& s, const SDL_Rect& box, SDL_Color c, int halign, int valign, float scale) {
  Cached* e = get(s, c);
  if (!e) return;
  float sVal = (scale > 0.0f) ? scale : 1.0f;
  if (box.w > 0 && float(e->w) * sVal > float(box.w)) {
    sVal = float(box.w) / float(e->w);
  }
  if (box.h > 0 && float(e->h) * sVal > float(box.h)) {
    sVal = std::min(sVal, float(box.h) / float(e->h));
  }
  sVal = std::max(0.1f, sVal);
  float w = float(e->w) * sVal;
  float h = float(e->h) * sVal;
  float x = float(box.x);
  if (halign == 0) x += (float(box.w) - w) * 0.5f;
  else if (halign > 0) x += float(box.w) - w;

  float y = float(box.y);
  if (valign == 0) y += (float(box.h) - h) * 0.5f;
  else if (valign > 0) y += float(box.h) - h;

  SDL_FRect dst{x, y, w, h};
  SDL_RenderCopyF(r_, e->tex, nullptr, &dst);
}

void TextRenderer::drawAlignedScaled(const std::string& s, const SDL_FRect& box, SDL_Color c, float scale, int halign, int valign) {
  Cached* e = get(s, c);
  if (!e) return;
  float w = float(e->w) * scale;
  float h = float(e->h) * scale;
  float x = box.x;
  if (halign == 0) x = box.x + (box.w - w) * 0.5f;
  else if (halign > 0) x = box.x + box.w - w;

  float y = box.y;
  if (valign == 0) y = box.y + (box.h - h) * 0.5f;
  else if (valign > 0) y = box.y + box.h - h;

  SDL_FRect dst{x, y, w, h};
  SDL_RenderCopyF(r_, e->tex, nullptr, &dst);
}

void TextRenderer::drawWrapped(const std::string& s, const SDL_Rect& box, SDL_Color c, int halign, int valign) {
  if (!font_ || s.empty() || box.w <= 0 || box.h <= 0) return;
  std::vector<std::string> lines;
  size_t pos = 0;
  while (pos <= s.size()) {
    size_t nl = s.find('\n', pos);
    std::string para = (nl == std::string::npos) ? s.substr(pos) : s.substr(pos, nl - pos);
    if (!para.empty() && para.back() == '\r') para.pop_back();

    // Trim spaces-only paragraph to an empty line
    bool onlySpaces = true;
    for (char ch : para) {
      if (ch != ' ' && ch != '\t') {
        onlySpaces = false;
        break;
      }
    }
    if (onlySpaces) {
      lines.emplace_back();
    } else {
      std::string curLine;
      size_t wpos = 0;
      while (wpos < para.size()) {
        while (wpos < para.size() && para[wpos] == ' ') ++wpos;
        if (wpos >= para.size()) break;
        size_t wend = para.find(' ', wpos);
        std::string word = (wend == std::string::npos) ? para.substr(wpos) : para.substr(wpos, wend - wpos);
        std::string candidate = curLine.empty() ? word : (curLine + " " + word);
        if (!curLine.empty() && width(candidate) > box.w) {
          lines.push_back(curLine);
          curLine = word;
        } else {
          curLine = std::move(candidate);
        }
        if (wend == std::string::npos) break;
        wpos = wend + 1;
      }
      if (!curLine.empty()) lines.push_back(curLine);
    }

    if (nl == std::string::npos) break;
    pos = nl + 1;
  }

  int lh = std::max(14, lineHeight());
  int totalH = (int)lines.size() * lh;
  int startY = box.y;
  if (valign == 0) startY = box.y + std::max(0, (box.h - totalH) / 2);
  else if (valign > 0) startY = box.y + std::max(0, box.h - totalH);

  for (size_t i = 0; i < lines.size(); ++i) {
    int y = startY + (int)i * lh;
    if (y + lh > box.y + box.h + 4) break;
    if (lines[i].empty()) continue;
    SDL_Rect lineBox{box.x, y, box.w, lh};
    drawAligned(lines[i], lineBox, c, halign, -1);
  }
}

}  // namespace nfh
