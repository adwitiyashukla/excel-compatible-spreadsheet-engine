#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sheet {

class XmlReader {
 public:
  enum Event { kStart, kEnd, kText, kEof, kError };

  explicit XmlReader(std::string_view doc) : doc_(doc) {}

  Event Next();
  std::string_view Name() const { return local_; }
  bool Attr(std::string_view local, std::string& out) const;
  bool QAttr(std::string_view qname, std::string& out) const;
  bool Has(std::string_view local) const;
  const std::string& Text() const { return text_; }
  int depth() const { return depth_; }
  void Skip();

 private:
  bool ParseTag(Event& ev);

  std::string_view doc_;
  size_t pos_ = 0;
  std::string_view local_;
  std::vector<std::pair<std::string_view, std::string_view>> attrs_;
  std::string text_;
  bool pending_end_ = false;
  int depth_ = 0;
};

std::string XmlDecode(std::string_view raw);
std::string XmlEscape(std::string_view s);

}
