// Tolerant XML parser for the game's data files.
//
// The original files are "XML-ish": several of them (e.g. gfxdata.xml) have
// multiple top-level elements, some contain stray characters. This parser
// therefore always returns a synthetic document root whose children are all
// top-level elements, and recovers from minor errors instead of failing.
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace nfh {

struct XmlNode {
  std::string name;
  std::vector<std::pair<std::string, std::string>> attrs;
  std::vector<std::unique_ptr<XmlNode>> children;
  std::string text;  // concatenated character data (trimmed)
  int line = 0;

  const std::string* attr(const std::string& key) const;
  std::string attrOr(const std::string& key, const std::string& def = {}) const;
  int attrInt(const std::string& key, int def = 0) const;
  // Parses "x/y" pairs used everywhere in the game ("628/315").
  bool attrPair(const std::string& key, int& a, int& b) const;

  const XmlNode* child(const std::string& name) const;
  std::vector<const XmlNode*> childrenNamed(const std::string& name) const;
};

struct XmlDocument {
  XmlNode root;  // synthetic, name = "#document"
  std::vector<std::string> warnings;
};

// `utf8` must already be UTF-8 (use decodeToUtf8 first).
XmlDocument parseXml(const std::string& utf8);

// Pretty-prints a node tree (used by the viewer and for debugging).
std::string dumpXml(const XmlNode& node, int indent = 0);

}  // namespace nfh
