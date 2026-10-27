#include "core/xml.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "core/text.h"

namespace nfh {

const std::string* XmlNode::attr(const std::string& key) const {
  for (auto& kv : attrs)
    if (kv.first == key) return &kv.second;
  return nullptr;
}

std::string XmlNode::attrOr(const std::string& key, const std::string& def) const {
  const std::string* v = attr(key);
  return v ? *v : def;
}

int XmlNode::attrInt(const std::string& key, int def) const {
  const std::string* v = attr(key);
  if (!v || v->empty()) return def;
  return std::atoi(v->c_str());
}

bool XmlNode::attrPair(const std::string& key, int& a, int& b) const {
  const std::string* v = attr(key);
  if (!v) return false;
  size_t slash = v->find('/');
  if (slash == std::string::npos) return false;
  a = std::atoi(v->substr(0, slash).c_str());
  b = std::atoi(v->substr(slash + 1).c_str());
  return true;
}

const XmlNode* XmlNode::child(const std::string& n) const {
  for (auto& c : children)
    if (c->name == n) return c.get();
  return nullptr;
}

std::vector<const XmlNode*> XmlNode::childrenNamed(const std::string& n) const {
  std::vector<const XmlNode*> out;
  for (auto& c : children)
    if (c->name == n) out.push_back(c.get());
  return out;
}

namespace {

class Parser {
 public:
  Parser(const std::string& s, XmlDocument& doc) : s_(s), doc_(doc) {}

  void run() {
    doc_.root.name = "#document";
    std::vector<XmlNode*> stack{&doc_.root};
    while (pos_ < s_.size()) {
      if (s_[pos_] != '<') {
        size_t end = s_.find('<', pos_);
        if (end == std::string::npos) end = s_.size();
        appendText(stack.back(), s_.substr(pos_, end - pos_));
        advanceTo(end);
        continue;
      }
      if (startsWith("<!--")) {
        skipPast("-->");
      } else if (startsWith("<?")) {
        skipPast("?>");
      } else if (startsWith("<![CDATA[")) {
        size_t start = pos_ + 9;
        size_t end = s_.find("]]>", start);
        if (end == std::string::npos) end = s_.size();
        stack.back()->text += s_.substr(start, end - start);
        advanceTo(std::min(s_.size(), end + 3));
      } else if (startsWith("<!")) {
        skipPast(">");
      } else if (startsWith("</")) {
        advance(2);
        std::string name = readName();
        skipPast(">");
        // Pop to the matching element; ignore unmatched closing tags.
        for (size_t i = stack.size(); i-- > 1;) {
          if (stack[i]->name == name) {
            if (i != stack.size() - 1) warn("unclosed element(s) before </" + name + ">");
            stack.resize(i);
            break;
          }
          if (i == 1) warn("stray </" + name + ">");
        }
      } else {
        advance(1);
        auto node = std::make_unique<XmlNode>();
        node->line = line_;
        node->name = readName();
        if (node->name.empty()) {
          warn("bad tag");
          skipPast(">");
          continue;
        }
        bool selfClose = readAttributes(*node);
        XmlNode* raw = node.get();
        stack.back()->children.push_back(std::move(node));
        if (!selfClose) stack.push_back(raw);
      }
    }
    if (stack.size() > 1) warn("document ended with unclosed <" + stack.back()->name + ">");
    trimAll(doc_.root);
  }

 private:
  const std::string& s_;
  XmlDocument& doc_;
  size_t pos_ = 0;
  int line_ = 1;

  bool startsWith(const char* p) const { return s_.compare(pos_, std::strlen(p), p) == 0; }
  void advance(size_t n) { advanceTo(std::min(s_.size(), pos_ + n)); }
  void advanceTo(size_t np) {
    for (size_t i = pos_; i < np && i < s_.size(); ++i)
      if (s_[i] == '\n') ++line_;
    pos_ = np;
  }
  void skipPast(const char* term) {
    size_t e = s_.find(term, pos_);
    advanceTo(e == std::string::npos ? s_.size() : e + std::strlen(term));
  }
  void skipWs() {
    while (pos_ < s_.size() && (unsigned char)s_[pos_] <= ' ') advance(1);
  }
  static bool isNameChar(char c) {
    return std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.' || c == ':' || (unsigned char)c >= 0x80;
  }
  std::string readName() {
    size_t st = pos_;
    while (pos_ < s_.size() && isNameChar(s_[pos_])) ++pos_;
    return s_.substr(st, pos_ - st);
  }
  void warn(const std::string& m) { doc_.warnings.push_back("line " + std::to_string(line_) + ": " + m); }

  // Returns true if the tag was self-closing.
  bool readAttributes(XmlNode& n) {
    for (;;) {
      skipWs();
      if (pos_ >= s_.size()) return true;
      char c = s_[pos_];
      if (c == '/') {
        skipPast(">");
        return true;
      }
      if (c == '>') {
        advance(1);
        return false;
      }
      std::string key = readName();
      if (key.empty()) {  // garbage inside tag: skip a char
        advance(1);
        continue;
      }
      skipWs();
      std::string value;
      if (pos_ < s_.size() && s_[pos_] == '=') {
        advance(1);
        skipWs();
        if (pos_ < s_.size() && (s_[pos_] == '"' || s_[pos_] == '\'')) {
          char q = s_[pos_];
          size_t st = pos_ + 1;
          size_t e = s_.find(q, st);
          if (e == std::string::npos) e = s_.size();
          value = unescape(s_.substr(st, e - st));
          advanceTo(std::min(s_.size(), e + 1));
        } else {  // unquoted value
          size_t st = pos_;
          while (pos_ < s_.size() && (unsigned char)s_[pos_] > ' ' && s_[pos_] != '>' && s_[pos_] != '/') ++pos_;
          value = unescape(s_.substr(st, pos_ - st));
          warn("unquoted attribute value for " + key);
        }
      }
      n.attrs.emplace_back(std::move(key), std::move(value));
    }
  }

  static std::string unescape(const std::string& in) {
    if (in.find('&') == std::string::npos) return in;
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
      if (in[i] != '&') {
        out += in[i];
        continue;
      }
      size_t semi = in.find(';', i);
      if (semi == std::string::npos || semi - i > 10) {
        out += '&';
        continue;
      }
      std::string ent = in.substr(i + 1, semi - i - 1);
      if (ent == "amp") out += '&';
      else if (ent == "lt") out += '<';
      else if (ent == "gt") out += '>';
      else if (ent == "quot") out += '"';
      else if (ent == "apos") out += '\'';
      else if (!ent.empty() && ent[0] == '#') {
        uint32_t cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? (uint32_t)std::strtoul(ent.c_str() + 2, nullptr, 16)
                                                                          : (uint32_t)std::strtoul(ent.c_str() + 1, nullptr, 10);
        appendUtf8(out, cp);
      } else {
        out += in.substr(i, semi - i + 1);
      }
      i = semi;
    }
    return out;
  }

  static void appendText(XmlNode* n, const std::string& t) {
    for (char c : t)
      if ((unsigned char)c > ' ') {
        n->text += unescape(t);
        return;
      }
  }

  static void trimAll(XmlNode& n) {
    auto& t = n.text;
    size_t a = 0, b = t.size();
    while (a < b && (unsigned char)t[a] <= ' ') ++a;
    while (b > a && (unsigned char)t[b - 1] <= ' ') --b;
    t = t.substr(a, b - a);
    for (auto& c : n.children) trimAll(*c);
  }
};

void dumpInto(std::string& out, const XmlNode& n, int indent) {
  std::string pad(indent * 2, ' ');
  out += pad + "<" + n.name;
  for (auto& kv : n.attrs) out += " " + kv.first + "=\"" + kv.second + "\"";
  if (n.children.empty() && n.text.empty()) {
    out += "/>\n";
    return;
  }
  out += ">";
  if (!n.text.empty()) out += n.text;
  if (!n.children.empty()) {
    out += "\n";
    for (auto& c : n.children) dumpInto(out, *c, indent + 1);
    out += pad;
  }
  out += "</" + n.name + ">\n";
}

}  // namespace

XmlDocument parseXml(const std::string& utf8) {
  XmlDocument doc;
  Parser(utf8, doc).run();
  return doc;
}

std::string dumpXml(const XmlNode& node, int indent) {
  std::string out;
  if (node.name == "#document") {
    for (auto& c : node.children) dumpInto(out, *c, indent);
  } else {
    dumpInto(out, node, indent);
  }
  return out;
}

}  // namespace nfh
