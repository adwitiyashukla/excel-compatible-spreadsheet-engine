#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sheet {

class ZipReader {
 public:
  bool Open(std::string data, std::string* error);
  bool Has(std::string_view name) const;
  bool Read(std::string_view name, std::string& out, std::string* error) const;
  std::vector<std::string> Names() const;

 private:
  struct Entry {
    std::string name;
    uint16_t method = 0;
    uint16_t flags = 0;
    uint32_t crc = 0;
    uint64_t csize = 0;
    uint64_t usize = 0;
    uint64_t offset = 0;
  };
  const Entry* Find(std::string_view name) const;
  std::string data_;
  std::vector<Entry> entries_;
};

class ZipWriter {
 public:
  void Add(std::string name, std::string_view content, int level = 4);
  std::string Finish();

 private:
  struct Entry {
    std::string name;
    uint32_t crc;
    uint32_t csize;
    uint32_t usize;
    uint32_t offset;
    uint16_t method;
  };
  std::string out_;
  std::vector<Entry> entries_;
};

bool ReadFile(const std::string& path, std::string& out);
bool WriteFile(const std::string& path, std::string_view data);

}
