#include "zip.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "core.h"
#include "deflate.h"

namespace sheet {

namespace {

uint16_t U16(const std::string& d, uint64_t at) {
  if (at + 2 > d.size()) return 0;
  return static_cast<uint16_t>(static_cast<uint8_t>(d[at]) |
                               (static_cast<uint8_t>(d[at + 1]) << 8));
}

uint32_t U32(const std::string& d, uint64_t at) {
  if (at + 4 > d.size()) return 0;
  return static_cast<uint32_t>(U16(d, at)) | (static_cast<uint32_t>(U16(d, at + 2)) << 16);
}

uint64_t U64(const std::string& d, uint64_t at) {
  return static_cast<uint64_t>(U32(d, at)) | (static_cast<uint64_t>(U32(d, at + 4)) << 32);
}

void Put16(std::string& o, uint32_t v) {
  o.push_back(static_cast<char>(v & 0xFF));
  o.push_back(static_cast<char>((v >> 8) & 0xFF));
}

void Put32(std::string& o, uint32_t v) {
  Put16(o, v & 0xFFFF);
  Put16(o, v >> 16);
}

}

bool ReadFile(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

bool WriteFile(const std::string& path, std::string_view data) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(data.data(), static_cast<std::streamsize>(data.size()));
  return static_cast<bool>(f);
}

bool ZipReader::Open(std::string data, std::string* error) {
  data_ = std::move(data);
  entries_.clear();
  auto fail = [&](const char* msg) {
    if (error) *error = msg;
    return false;
  };
  if (data_.size() < 22) return fail("not a zip file");
  uint64_t eocd = 0;
  bool found = false;
  uint64_t lowest = data_.size() > 65557 ? data_.size() - 65557 : 0;
  for (uint64_t p = data_.size() - 22;; --p) {
    if (U32(data_, p) == 0x06054b50) {
      eocd = p;
      found = true;
      break;
    }
    if (p == lowest) break;
  }
  if (!found) return fail("zip end record not found");
  uint64_t count = U16(data_, eocd + 10);
  uint64_t cd_size = U32(data_, eocd + 12);
  uint64_t cd_off = U32(data_, eocd + 16);
  if (eocd >= 20 && U32(data_, eocd - 20) == 0x07064b50) {
    uint64_t rec = U64(data_, eocd - 20 + 8);
    if (rec + 56 <= data_.size() && U32(data_, rec) == 0x06064b50) {
      count = U64(data_, rec + 32);
      cd_size = U64(data_, rec + 40);
      cd_off = U64(data_, rec + 48);
    }
  }
  if (cd_off + cd_size > data_.size()) return fail("zip directory out of range");
  uint64_t p = cd_off;
  for (uint64_t i = 0; i < count; ++i) {
    if (p + 46 > data_.size() || U32(data_, p) != 0x02014b50)
      return fail("bad zip directory entry");
    Entry e;
    e.flags = U16(data_, p + 8);
    e.method = U16(data_, p + 10);
    e.crc = U32(data_, p + 16);
    e.csize = U32(data_, p + 20);
    e.usize = U32(data_, p + 24);
    uint16_t name_len = U16(data_, p + 28);
    uint16_t extra_len = U16(data_, p + 30);
    uint16_t comment_len = U16(data_, p + 32);
    e.offset = U32(data_, p + 42);
    if (p + 46 + name_len + extra_len + comment_len > data_.size())
      return fail("bad zip entry name");
    e.name = data_.substr(p + 46, name_len);
    uint64_t x = p + 46 + name_len;
    uint64_t x_end = x + extra_len;
    while (x + 4 <= x_end) {
      uint16_t id = U16(data_, x);
      uint16_t sz = U16(data_, x + 2);
      if (id == 0x0001) {
        uint64_t q = x + 4;
        if (e.usize == 0xFFFFFFFFu) {
          e.usize = U64(data_, q);
          q += 8;
        }
        if (e.csize == 0xFFFFFFFFu) {
          e.csize = U64(data_, q);
          q += 8;
        }
        if (e.offset == 0xFFFFFFFFu) e.offset = U64(data_, q);
      }
      x += 4 + sz;
    }
    for (char& c : e.name) {
      if (c == '\\') c = '/';
    }
    entries_.push_back(std::move(e));
    p += 46 + name_len + extra_len + comment_len;
  }
  return true;
}

const ZipReader::Entry* ZipReader::Find(std::string_view name) const {
  if (!name.empty() && name[0] == '/') name.remove_prefix(1);
  for (const Entry& e : entries_) {
    if (e.name == name) return &e;
  }
  for (const Entry& e : entries_) {
    if (EqualsText(e.name, name)) return &e;
  }
  return nullptr;
}

bool ZipReader::Has(std::string_view name) const {
  return Find(name) != nullptr;
}

std::vector<std::string> ZipReader::Names() const {
  std::vector<std::string> out;
  for (const Entry& e : entries_) out.push_back(e.name);
  return out;
}

bool ZipReader::Read(std::string_view name, std::string& out, std::string* error) const {
  const Entry* e = Find(name);
  auto fail = [&](const std::string& msg) {
    if (error) *error = msg;
    return false;
  };
  if (!e) return fail("missing zip entry " + std::string(name));
  if (e->flags & 1) return fail("encrypted zip entry " + e->name);
  uint64_t h = e->offset;
  if (h + 30 > data_.size() || U32(data_, h) != 0x04034b50)
    return fail("bad local header for " + e->name);
  uint64_t start = h + 30 + U16(data_, h + 26) + U16(data_, h + 28);
  if (start + e->csize > data_.size()) return fail("zip entry out of range: " + e->name);
  std::string_view raw(data_.data() + start, static_cast<size_t>(e->csize));
  if (e->method == 0) {
    out.assign(raw);
  } else if (e->method == 8) {
    constexpr uint64_t kMaxEntry = 1ULL << 30;
    if (e->usize > kMaxEntry) return fail("zip entry too large: " + e->name);
    if (!Inflate(raw, out, static_cast<size_t>(e->usize), static_cast<size_t>(e->usize))) {
      return fail("corrupt deflate data in " + e->name);
    }
  } else {
    return fail("unsupported zip compression in " + e->name);
  }
  if (Crc32(out.data(), out.size()) != e->crc) return fail("crc mismatch in " + e->name);
  return true;
}

void ZipWriter::Add(std::string name, std::string_view content, int level) {
  Entry e;
  e.name = std::move(name);
  e.crc = Crc32(content.data(), content.size());
  e.usize = static_cast<uint32_t>(content.size());
  std::string packed = level > 0 ? Deflate(content, level) : std::string();
  bool use_deflate = level > 0 && packed.size() < content.size();
  e.method = use_deflate ? 8 : 0;
  e.csize = static_cast<uint32_t>(use_deflate ? packed.size() : content.size());
  e.offset = static_cast<uint32_t>(out_.size());
  Put32(out_, 0x04034b50);
  Put16(out_, 20);
  Put16(out_, 0x0800);
  Put16(out_, e.method);
  Put16(out_, 0);
  Put16(out_, 0x21);
  Put32(out_, e.crc);
  Put32(out_, e.csize);
  Put32(out_, e.usize);
  Put16(out_, static_cast<uint32_t>(e.name.size()));
  Put16(out_, 0);
  out_ += e.name;
  if (use_deflate) {
    out_ += packed;
  } else {
    out_.append(content);
  }
  entries_.push_back(std::move(e));
}

std::string ZipWriter::Finish() {
  uint32_t cd_start = static_cast<uint32_t>(out_.size());
  for (const Entry& e : entries_) {
    Put32(out_, 0x02014b50);
    Put16(out_, 20);
    Put16(out_, 20);
    Put16(out_, 0x0800);
    Put16(out_, e.method);
    Put16(out_, 0);
    Put16(out_, 0x21);
    Put32(out_, e.crc);
    Put32(out_, e.csize);
    Put32(out_, e.usize);
    Put16(out_, static_cast<uint32_t>(e.name.size()));
    Put16(out_, 0);
    Put16(out_, 0);
    Put16(out_, 0);
    Put16(out_, 0);
    Put32(out_, 0);
    Put32(out_, e.offset);
    out_ += e.name;
  }
  uint32_t cd_size = static_cast<uint32_t>(out_.size()) - cd_start;
  Put32(out_, 0x06054b50);
  Put16(out_, 0);
  Put16(out_, 0);
  Put16(out_, static_cast<uint32_t>(entries_.size()));
  Put16(out_, static_cast<uint32_t>(entries_.size()));
  Put32(out_, cd_size);
  Put32(out_, cd_start);
  Put16(out_, 0);
  return std::move(out_);
}

}
