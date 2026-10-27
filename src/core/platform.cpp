#include "core/platform.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace nfh {

#ifdef _WIN32
static std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}
static std::string narrow(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}
#endif

fs::path pathFromUtf8(const std::string& utf8) {
#ifdef _WIN32
  return fs::path(widen(utf8));
#else
  return fs::path(utf8);
#endif
}

std::string pathToUtf8(const fs::path& p) {
#ifdef _WIN32
  return narrow(p.wstring());
#else
  return p.string();
#endif
}

std::vector<std::string> utf8Args(int argc, char** argv) {
  std::vector<std::string> out;
#ifdef _WIN32
  int n = 0;
  LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &n);
  if (wargv) {
    for (int i = 0; i < n; ++i) out.push_back(narrow(wargv[i]));
    LocalFree(wargv);
    return out;
  }
#endif
  for (int i = 0; i < argc; ++i) out.emplace_back(argv[i]);
  return out;
}

void setupConsoleUtf8() {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
#endif
}

fs::path executableDir() {
  std::error_code ec;
#ifdef _WIN32
  wchar_t buf[32768];
  DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
  if (n > 0 && n < 32768) return fs::path(std::wstring(buf, n)).parent_path();
#else
  fs::path p = fs::read_symlink("/proc/self/exe", ec);
  if (!ec) return p.parent_path();
#endif
  return fs::current_path(ec);
}

bool readWholeFile(const fs::path& p, std::vector<uint8_t>& out, std::string* err) {
  std::ifstream f(p, std::ios::binary);
  if (!f) {
    if (err) *err = "cannot open " + pathToUtf8(p);
    return false;
  }
  f.seekg(0, std::ios::end);
  std::streamoff size = f.tellg();
  f.seekg(0, std::ios::beg);
  out.resize(size > 0 ? (size_t)size : 0);
  if (size > 0 && !f.read(reinterpret_cast<char*>(out.data()), size)) {
    if (err) *err = "read error " + pathToUtf8(p);
    return false;
  }
  return true;
}

bool writeWholeFile(const fs::path& p, const void* data, size_t size, std::string* err) {
  std::error_code ec;
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (!f) {
    if (err) *err = "cannot create " + pathToUtf8(p);
    return false;
  }
  f.write(static_cast<const char*>(data), (std::streamsize)size);
  if (!f) {
    if (err) *err = "write error " + pathToUtf8(p);
    return false;
  }
  return true;
}

std::string toLowerAscii(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return s;
}

bool findFileNoCase(const fs::path& dir, const std::string& name, fs::path& out) {
  std::error_code ec;
  fs::path direct = dir / pathFromUtf8(name);
  if (fs::exists(direct, ec)) {
    out = direct;
    return true;
  }
  const std::string want = toLowerAscii(name);
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (toLowerAscii(pathToUtf8(it->path().filename())) == want) {
      out = it->path();
      return true;
    }
  }
  return false;
}

}  // namespace nfh
