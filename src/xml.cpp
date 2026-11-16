#include "xml.h"

#include <cstdio>
#include <cstdlib>

#include "core.h"

namespace sheet {

namespace {

std::string_view LocalName(std::string_view q) {
  size_t colon = q.rfind(':');
  return colon == std::string_view::npos ? q : q.substr(colon + 1);
}

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

}

std::string XmlDecode(std::string_view raw) {
  if (raw.find('&') == std::string_view::npos) return std::string(raw);
  std::string out;
  out.reserve(raw.size());
  for (size_t i = 0; i < raw.size(); ++i) {
    char c = raw[i];
    if (c != '&') {
      out.push_back(c);
      continue;
    }
    size_t semi = raw.find(';', i);
    if (semi == std::string_view::npos || semi - i > 12) {
      out.push_back(c);
      continue;
    }
    std::string_view ent = raw.substr(i + 1, semi - i - 1);
    if (ent == "lt") {
      out.push_back('<');
    } else if (ent == "gt") {
      out.push_back('>');
    } else if (ent == "amp") {
      out.push_back('&');
    } else if (ent == "quot") {
      out.push_back('"');
    } else if (ent == "apos") {
      out.push_back('\'');
    } else if (!ent.empty() && ent[0] == '#') {
      unsigned long cp;
      if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')) {
        cp = std::strtoul(std::string(ent.substr(2)).c_str(), nullptr, 16);
      } else {
        cp = std::strtoul(std::string(ent.substr(1)).c_str(), nullptr, 10);
      }
      out += Utf8Encode(static_cast<uint32_t>(cp));
    } else {
      out.append(raw.substr(i, semi - i + 1));
    }
    i = semi;
  }
  return out;
}

std::string XmlEscape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '&':
        out += "&amp;";
        break;
      case '"':
        out += "&quot;";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20 && c != '\t' && c != '\n' && c != '\r') {
          char buf[16];
          std::snprintf(buf, sizeof buf, "_x%04X_",
                        static_cast<unsigned>(static_cast<unsigned char>(c)));
          out += buf;
        } else {
          out.push_back(c);
        }
    }
  }
  return out;
}

bool XmlReader::Attr(std::string_view local, std::string& out) const {
  for (const auto& a : attrs_) {
    if (LocalName(a.first) == local) {
      out = XmlDecode(a.second);
      return true;
    }
  }
  return false;
}

bool XmlReader::QAttr(std::string_view qname, std::string& out) const {
  for (const auto& a : attrs_) {
    if (a.first == qname) {
      out = XmlDecode(a.second);
      return true;
    }
  }
  return false;
}

bool XmlReader::Has(std::string_view local) const {
  for (const auto& a : attrs_) {
    if (LocalName(a.first) == local) return true;
  }
  return false;
}

bool XmlReader::ParseTag(Event& ev) {
  size_t i = pos_ + 1;
  bool closing = false;
  if (i < doc_.size() && doc_[i] == '/') {
    closing = true;
    ++i;
  }
  size_t name_start = i;
  while (i < doc_.size() && !IsSpace(doc_[i]) && doc_[i] != '>' && doc_[i] != '/') ++i;
  std::string_view qname = doc_.substr(name_start, i - name_start);
  if (qname.empty()) return false;
  local_ = LocalName(qname);
  attrs_.clear();
  if (closing) {
    size_t gt = doc_.find('>', i);
    if (gt == std::string_view::npos) return false;
    pos_ = gt + 1;
    --depth_;
    ev = kEnd;
    return true;
  }
  while (true) {
    while (i < doc_.size() && IsSpace(doc_[i])) ++i;
    if (i >= doc_.size()) return false;
    if (doc_[i] == '>') {
      pos_ = i + 1;
      ++depth_;
      ev = kStart;
      return true;
    }
    if (doc_[i] == '/') {
      if (i + 1 >= doc_.size() || doc_[i + 1] != '>') return false;
      pos_ = i + 2;
      ++depth_;
      pending_end_ = true;
      ev = kStart;
      return true;
    }
    size_t an = i;
    while (i < doc_.size() && doc_[i] != '=' && !IsSpace(doc_[i]) && doc_[i] != '>') ++i;
    std::string_view aname = doc_.substr(an, i - an);
    while (i < doc_.size() && IsSpace(doc_[i])) ++i;
    if (i >= doc_.size() || doc_[i] != '=') return false;
    ++i;
    while (i < doc_.size() && IsSpace(doc_[i])) ++i;
    if (i >= doc_.size() || (doc_[i] != '"' && doc_[i] != '\'')) return false;
    char q = doc_[i++];
    size_t end = doc_.find(q, i);
    if (end == std::string_view::npos) return false;
    attrs_.emplace_back(aname, doc_.substr(i, end - i));
    i = end + 1;
  }
}

XmlReader::Event XmlReader::Next() {
  if (pending_end_) {
    pending_end_ = false;
    attrs_.clear();
    --depth_;
    return kEnd;
  }
  while (true) {
    if (pos_ >= doc_.size()) return kEof;
    if (doc_[pos_] != '<') {
      size_t lt = doc_.find('<', pos_);
      if (lt == std::string_view::npos) lt = doc_.size();
      text_ = XmlDecode(doc_.substr(pos_, lt - pos_));
      pos_ = lt;
      return kText;
    }
    if (doc_.compare(pos_, 4, "<!--") == 0) {
      size_t end = doc_.find("-->", pos_ + 4);
      if (end == std::string_view::npos) return kError;
      pos_ = end + 3;
      continue;
    }
    if (doc_.compare(pos_, 9, "<![CDATA[") == 0) {
      size_t end = doc_.find("]]>", pos_ + 9);
      if (end == std::string_view::npos) return kError;
      text_ = std::string(doc_.substr(pos_ + 9, end - pos_ - 9));
      pos_ = end + 3;
      return kText;
    }
    if (doc_.compare(pos_, 2, "<?") == 0) {
      size_t end = doc_.find("?>", pos_ + 2);
      if (end == std::string_view::npos) return kError;
      pos_ = end + 2;
      continue;
    }
    if (doc_.compare(pos_, 2, "<!") == 0) {
      int nest = 0;
      size_t i = pos_ + 2;
      for (; i < doc_.size(); ++i) {
        if (doc_[i] == '[') ++nest;
        if (doc_[i] == ']') --nest;
        if (doc_[i] == '>' && nest <= 0) break;
      }
      pos_ = i + 1;
      continue;
    }
    Event ev;
    if (!ParseTag(ev)) return kError;
    return ev;
  }
}

void XmlReader::Skip() {
  int target = depth_ - 1;
  while (depth_ > target) {
    Event e = Next();
    if (e == kEof || e == kError) return;
  }
}

}
