// Small platform helpers: UTF-8 paths, UTF-8 command line, whole-file I/O.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nfh {

namespace fs = std::filesystem;

// Converts a UTF-8 string to a filesystem path (works with Cyrillic on Windows).
fs::path pathFromUtf8(const std::string& utf8);
// Converts a path to a UTF-8 string for display / logging.
std::string pathToUtf8(const fs::path& p);

// Returns the command line as UTF-8 strings. On Windows argv is in the ANSI
// code page, so the wide command line is used instead.
std::vector<std::string> utf8Args(int argc, char** argv);

// Makes the console accept UTF-8 output (no-op outside Windows).
void setupConsoleUtf8();

// Directory containing the running executable (falls back to current dir).
fs::path executableDir();

bool readWholeFile(const fs::path& p, std::vector<uint8_t>& out, std::string* err = nullptr);
bool writeWholeFile(const fs::path& p, const void* data, size_t size, std::string* err = nullptr);

// Finds `name` inside `dir` ignoring ASCII case (Linux filesystems are
// case-sensitive, original game files are not consistently cased).
bool findFileNoCase(const fs::path& dir, const std::string& name, fs::path& out);

std::string toLowerAscii(std::string s);

}  // namespace nfh
