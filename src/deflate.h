#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace sheet {

uint32_t Crc32(const void* data, size_t n, uint32_t crc = 0);
bool Inflate(std::string_view in, std::string& out, size_t size_hint = 0, size_t limit = 1u << 30);
std::string Deflate(std::string_view in, int level = 6);

}
