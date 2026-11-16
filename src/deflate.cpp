#include "deflate.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <queue>
#include <vector>

namespace sheet {

namespace {

constexpr uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                   2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                    33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr uint8_t kClenOrder[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                    11, 4,  12, 3, 13, 2, 14, 1, 15};

std::array<uint32_t, 256> MakeCrcTable() {
  std::array<uint32_t, 256> t{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    t[i] = c;
  }
  return t;
}

class BitReader {
 public:
  BitReader(const uint8_t* p, size_t n)
      : p_(p), end_(p + n), avail_(static_cast<uint64_t>(n) * 8) {}

  void Fill(int n) {
    while (count_ < n) {
      uint64_t byte = p_ < end_ ? *p_++ : 0;
      buf_ |= byte << count_;
      count_ += 8;
    }
  }
  uint32_t Peek(int n) const { return static_cast<uint32_t>(buf_ & ((1ULL << n) - 1)); }
  void Drop(int n) {
    buf_ >>= n;
    count_ -= n;
    used_ += static_cast<uint64_t>(n);
  }
  bool Bits(int n, uint32_t& out) {
    if (n == 0) {
      out = 0;
      return true;
    }
    Fill(n);
    out = Peek(n);
    Drop(n);
    return !overran();
  }
  void AlignByte() { Drop(count_ & 7); }
  bool overran() const { return used_ > avail_; }

 private:
  const uint8_t* p_;
  const uint8_t* end_;
  uint64_t avail_;
  uint64_t used_ = 0;
  uint64_t buf_ = 0;
  int count_ = 0;
};

struct Huffman {
  static constexpr int kFast = 10;
  uint16_t fast[1 << kFast];
  uint16_t counts[16];
  uint16_t symbols[320];

  bool Build(const uint8_t* lens, int n) {
    std::memset(counts, 0, sizeof counts);
    std::memset(fast, 0, sizeof fast);
    for (int i = 0; i < n; ++i) ++counts[lens[i]];
    counts[0] = 0;
    int left = 1;
    for (int len = 1; len < 16; ++len) {
      left <<= 1;
      left -= counts[len];
      if (left < 0) return false;
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; ++len)
      offs[len + 1] = static_cast<uint16_t>(offs[len] + counts[len]);
    for (int i = 0; i < n; ++i) {
      if (lens[i]) symbols[offs[lens[i]]++] = static_cast<uint16_t>(i);
    }
    uint32_t code = 0;
    uint32_t next[16];
    for (int len = 1; len < 16; ++len) {
      next[len] = code;
      code = (code + counts[len]) << 1;
    }
    for (int i = 0; i < n; ++i) {
      int len = lens[i];
      if (len == 0) continue;
      uint32_t c = next[len]++;
      if (len > kFast) continue;
      uint32_t rev = 0;
      for (int b = 0; b < len; ++b) rev |= ((c >> b) & 1u) << (len - 1 - b);
      for (uint32_t j = rev; j < (1u << kFast); j += 1u << len) {
        fast[j] = static_cast<uint16_t>((i << 4) | len);
      }
    }
    return true;
  }

  int Decode(BitReader& br) const {
    br.Fill(15);
    uint16_t e = fast[br.Peek(kFast)];
    if (e != 0) {
      br.Drop(e & 15);
      return br.overran() ? -1 : e >> 4;
    }
    int code = 0;
    int first = 0;
    int index = 0;
    for (int len = 1; len < 16; ++len) {
      uint32_t bit;
      if (!br.Bits(1, bit)) return -1;
      code |= static_cast<int>(bit);
      int count = counts[len];
      if (code - count < first) return symbols[index + (code - first)];
      index += count;
      first += count;
      first <<= 1;
      code <<= 1;
    }
    return -1;
  }
};

bool InflateBlock(BitReader& br, const Huffman& lit, const Huffman& dist, std::string& out,
                  size_t limit) {
  while (true) {
    int sym = lit.Decode(br);
    if (sym < 0) return false;
    if (sym < 256) {
      if (out.size() >= limit) return false;
      out.push_back(static_cast<char>(sym));
      continue;
    }
    if (sym == 256) return true;
    sym -= 257;
    if (sym >= 29) return false;
    uint32_t extra;
    if (!br.Bits(kLenExtra[sym], extra)) return false;
    size_t len = kLenBase[sym] + extra;
    int ds = dist.Decode(br);
    if (ds < 0 || ds >= 30) return false;
    if (!br.Bits(kDistExtra[ds], extra)) return false;
    size_t d = kDistBase[ds] + extra;
    if (d > out.size()) return false;
    if (out.size() + len > limit) return false;
    size_t start = out.size();
    out.resize(start + len);
    char* o = &out[0];
    size_t from = start - d;
    for (size_t i = 0; i < len; ++i) o[start + i] = o[from + i];
  }
}

class BitWriter {
 public:
  explicit BitWriter(std::string& out) : out_(out) {}
  void Put(uint32_t bits, int n) {
    buf_ |= static_cast<uint64_t>(bits) << count_;
    count_ += n;
    while (count_ >= 8) {
      out_.push_back(static_cast<char>(buf_ & 0xFF));
      buf_ >>= 8;
      count_ -= 8;
    }
  }
  void Flush() {
    if (count_ > 0) {
      out_.push_back(static_cast<char>(buf_ & 0xFF));
      buf_ = 0;
      count_ = 0;
    }
  }

 private:
  std::string& out_;
  uint64_t buf_ = 0;
  int count_ = 0;
};

void LimitedLengths(const std::vector<uint32_t>& freq, int limit, std::vector<uint8_t>& lens) {
  int n = static_cast<int>(freq.size());
  lens.assign(static_cast<size_t>(n), 0);
  std::vector<int> used;
  for (int i = 0; i < n; ++i) {
    if (freq[static_cast<size_t>(i)] > 0) used.push_back(i);
  }
  if (used.empty()) return;
  if (used.size() == 1) {
    lens[static_cast<size_t>(used[0])] = 1;
    lens[static_cast<size_t>(used[0] == 0 ? 1 : 0)] = 1;
    return;
  }
  struct Node {
    uint64_t w;
    int left;
    int right;
  };
  std::vector<Node> nodes;
  using Item = std::pair<uint64_t, int>;
  std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
  for (int s : used) {
    nodes.push_back(Node{freq[static_cast<size_t>(s)], -1 - s, 0});
    pq.push({freq[static_cast<size_t>(s)], static_cast<int>(nodes.size() - 1)});
  }
  while (pq.size() > 1) {
    Item a = pq.top();
    pq.pop();
    Item b = pq.top();
    pq.pop();
    nodes.push_back(Node{a.first + b.first, a.second, b.second});
    pq.push({a.first + b.first, static_cast<int>(nodes.size() - 1)});
  }
  std::vector<int> depth_count(64, 0);
  std::vector<std::pair<int, int>> stack{{pq.top().second, 0}};
  while (!stack.empty()) {
    auto [idx, d] = stack.back();
    stack.pop_back();
    const Node& node = nodes[static_cast<size_t>(idx)];
    if (node.left < 0) {
      ++depth_count[static_cast<size_t>(std::min(d, 63))];
      continue;
    }
    stack.push_back({node.left, d + 1});
    stack.push_back({node.right, d + 1});
  }
  for (int i = limit + 1; i < 64; ++i) {
    depth_count[static_cast<size_t>(limit)] += depth_count[static_cast<size_t>(i)];
    depth_count[static_cast<size_t>(i)] = 0;
  }
  while (true) {
    uint64_t kraft = 0;
    for (int i = 1; i <= limit; ++i)
      kraft += static_cast<uint64_t>(depth_count[static_cast<size_t>(i)]) << (limit - i);
    if (kraft <= (1ULL << limit)) break;
    int j = limit - 1;
    while (j > 0 && depth_count[static_cast<size_t>(j)] == 0) --j;
    --depth_count[static_cast<size_t>(j)];
    depth_count[static_cast<size_t>(j + 1)] += 2;
    --depth_count[static_cast<size_t>(limit)];
  }
  std::sort(used.begin(), used.end(), [&](int a, int b) {
    if (freq[static_cast<size_t>(a)] != freq[static_cast<size_t>(b)]) {
      return freq[static_cast<size_t>(a)] > freq[static_cast<size_t>(b)];
    }
    return a < b;
  });
  size_t k = 0;
  for (int len = 1; len <= limit; ++len) {
    for (int c = 0; c < depth_count[static_cast<size_t>(len)]; ++c) {
      lens[static_cast<size_t>(used[k++])] = static_cast<uint8_t>(len);
    }
  }
}

void CanonicalCodes(const std::vector<uint8_t>& lens, std::vector<uint32_t>& codes) {
  codes.assign(lens.size(), 0);
  uint32_t counts[16] = {0};
  for (uint8_t l : lens) ++counts[l];
  counts[0] = 0;
  uint32_t next[16] = {0};
  uint32_t code = 0;
  for (int len = 1; len < 16; ++len) {
    code = (code + counts[len - 1]) << 1;
    next[len] = code;
  }
  for (size_t i = 0; i < lens.size(); ++i) {
    int len = lens[i];
    if (len == 0) continue;
    uint32_t c = next[len]++;
    uint32_t rev = 0;
    for (int b = 0; b < len; ++b) rev |= ((c >> b) & 1u) << (len - 1 - b);
    codes[i] = rev;
  }
}

struct Symbol {
  uint16_t lit;
  uint16_t dist;
};

int LenCode(int len) {
  int i = 0;
  while (i < 28 && kLenBase[i + 1] <= len) ++i;
  return i;
}

int DistCode(int d) {
  int i = 0;
  while (i < 29 && kDistBase[i + 1] <= d) ++i;
  return i;
}

void WriteBlock(BitWriter& bw, const std::vector<Symbol>& syms, bool final) {
  std::vector<uint32_t> lf(286, 0);
  std::vector<uint32_t> df(30, 0);
  for (const Symbol& s : syms) {
    if (s.dist == 0) {
      ++lf[s.lit];
    } else {
      ++lf[static_cast<size_t>(257 + LenCode(s.lit))];
      ++df[static_cast<size_t>(DistCode(s.dist))];
    }
  }
  lf[256] = 1;
  bool any_dist = false;
  for (uint32_t f : df) any_dist = any_dist || f > 0;
  if (!any_dist) df[0] = 1;
  std::vector<uint8_t> ll;
  std::vector<uint8_t> dl;
  LimitedLengths(lf, 15, ll);
  LimitedLengths(df, 15, dl);
  int hlit = 286;
  while (hlit > 257 && ll[static_cast<size_t>(hlit - 1)] == 0) --hlit;
  int hdist = 30;
  while (hdist > 1 && dl[static_cast<size_t>(hdist - 1)] == 0) --hdist;
  std::vector<uint8_t> all(ll.begin(), ll.begin() + hlit);
  all.insert(all.end(), dl.begin(), dl.begin() + hdist);
  struct Rle {
    uint8_t sym;
    uint8_t extra;
  };
  std::vector<Rle> rle;
  for (size_t i = 0; i < all.size();) {
    uint8_t v = all[i];
    size_t run = 1;
    while (i + run < all.size() && all[i + run] == v) ++run;
    if (v == 0 && run >= 3) {
      size_t r = std::min<size_t>(run, 138);
      if (r >= 11) {
        rle.push_back({18, static_cast<uint8_t>(r - 11)});
      } else {
        rle.push_back({17, static_cast<uint8_t>(r - 3)});
      }
      i += r;
      continue;
    }
    if (v != 0 && run >= 4) {
      rle.push_back({v, 0});
      size_t r = std::min<size_t>(run - 1, 6);
      rle.push_back({16, static_cast<uint8_t>(r - 3)});
      i += r + 1;
      continue;
    }
    rle.push_back({v, 0});
    ++i;
  }
  std::vector<uint32_t> cf(19, 0);
  for (const Rle& r : rle) ++cf[r.sym];
  std::vector<uint8_t> cl;
  LimitedLengths(cf, 7, cl);
  int hclen = 19;
  while (hclen > 4 && cl[kClenOrder[hclen - 1]] == 0) --hclen;
  std::vector<uint32_t> lc;
  std::vector<uint32_t> dc;
  std::vector<uint32_t> cc;
  CanonicalCodes(ll, lc);
  CanonicalCodes(dl, dc);
  CanonicalCodes(cl, cc);
  bw.Put(final ? 1 : 0, 1);
  bw.Put(2, 2);
  bw.Put(static_cast<uint32_t>(hlit - 257), 5);
  bw.Put(static_cast<uint32_t>(hdist - 1), 5);
  bw.Put(static_cast<uint32_t>(hclen - 4), 4);
  for (int i = 0; i < hclen; ++i) bw.Put(cl[kClenOrder[i]], 3);
  for (const Rle& r : rle) {
    bw.Put(cc[r.sym], cl[r.sym]);
    if (r.sym == 16) bw.Put(r.extra, 2);
    if (r.sym == 17) bw.Put(r.extra, 3);
    if (r.sym == 18) bw.Put(r.extra, 7);
  }
  for (const Symbol& s : syms) {
    if (s.dist == 0) {
      bw.Put(lc[s.lit], ll[s.lit]);
      continue;
    }
    int code = LenCode(s.lit);
    bw.Put(lc[static_cast<size_t>(257 + code)], ll[static_cast<size_t>(257 + code)]);
    bw.Put(static_cast<uint32_t>(s.lit - kLenBase[code]), kLenExtra[code]);
    int dcode = DistCode(s.dist);
    bw.Put(dc[static_cast<size_t>(dcode)], dl[static_cast<size_t>(dcode)]);
    bw.Put(static_cast<uint32_t>(s.dist - kDistBase[dcode]), kDistExtra[dcode]);
  }
  bw.Put(lc[256], ll[256]);
}

const Huffman& FixedLit() {
  static const Huffman h = [] {
    Huffman x;
    uint8_t lens[288];
    for (int i = 0; i < 144; ++i) lens[i] = 8;
    for (int i = 144; i < 256; ++i) lens[i] = 9;
    for (int i = 256; i < 280; ++i) lens[i] = 7;
    for (int i = 280; i < 288; ++i) lens[i] = 8;
    x.Build(lens, 288);
    return x;
  }();
  return h;
}

const Huffman& FixedDist() {
  static const Huffman h = [] {
    Huffman x;
    uint8_t lens[30];
    for (int i = 0; i < 30; ++i) lens[i] = 5;
    x.Build(lens, 30);
    return x;
  }();
  return h;
}

}

uint32_t Crc32(const void* data, size_t n, uint32_t crc) {
  static const std::array<uint32_t, 256> table = MakeCrcTable();
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint32_t c = ~crc;
  for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return ~c;
}

bool Inflate(std::string_view in, std::string& out, size_t size_hint, size_t limit) {
  out.clear();
  if (size_hint) out.reserve(std::min(size_hint, limit));
  BitReader br(reinterpret_cast<const uint8_t*>(in.data()), in.size());
  while (true) {
    uint32_t final;
    uint32_t type;
    if (!br.Bits(1, final) || !br.Bits(2, type)) return false;
    if (type == 0) {
      br.AlignByte();
      uint32_t len;
      uint32_t nlen;
      if (!br.Bits(16, len) || !br.Bits(16, nlen)) return false;
      if ((len ^ 0xFFFF) != nlen) return false;
      if (out.size() + len > limit) return false;
      for (uint32_t i = 0; i < len; ++i) {
        uint32_t b;
        if (!br.Bits(8, b)) return false;
        out.push_back(static_cast<char>(b));
      }
    } else if (type == 1) {
      if (!InflateBlock(br, FixedLit(), FixedDist(), out, limit)) return false;
    } else if (type == 2) {
      uint32_t hlit;
      uint32_t hdist;
      uint32_t hclen;
      if (!br.Bits(5, hlit) || !br.Bits(5, hdist) || !br.Bits(4, hclen)) return false;
      hlit += 257;
      hdist += 1;
      hclen += 4;
      if (hlit > 286 || hdist > 30) return false;
      uint8_t clens[19] = {0};
      for (uint32_t i = 0; i < hclen; ++i) {
        uint32_t v;
        if (!br.Bits(3, v)) return false;
        clens[kClenOrder[i]] = static_cast<uint8_t>(v);
      }
      Huffman ch;
      if (!ch.Build(clens, 19)) return false;
      uint8_t lens[320] = {0};
      uint32_t idx = 0;
      while (idx < hlit + hdist) {
        int sym = ch.Decode(br);
        if (sym < 0) return false;
        if (sym < 16) {
          lens[idx++] = static_cast<uint8_t>(sym);
          continue;
        }
        uint32_t rep;
        uint8_t val = 0;
        if (sym == 16) {
          if (idx == 0) return false;
          val = lens[idx - 1];
          if (!br.Bits(2, rep)) return false;
          rep += 3;
        } else if (sym == 17) {
          if (!br.Bits(3, rep)) return false;
          rep += 3;
        } else {
          if (!br.Bits(7, rep)) return false;
          rep += 11;
        }
        if (idx + rep > hlit + hdist) return false;
        while (rep--) lens[idx++] = val;
      }
      if (lens[256] == 0) return false;
      Huffman lit;
      Huffman dist;
      if (!lit.Build(lens, static_cast<int>(hlit))) return false;
      if (!dist.Build(lens + hlit, static_cast<int>(hdist))) return false;
      if (!InflateBlock(br, lit, dist, out, limit)) return false;
    } else {
      return false;
    }
    if (br.overran()) return false;
    if (final) return true;
  }
}

std::string Deflate(std::string_view in, int level) {
  std::string out;
  BitWriter bw(out);
  if (level <= 0 || in.empty()) {
    size_t pos = 0;
    do {
      size_t n = std::min<size_t>(65535, in.size() - pos);
      bool last = pos + n >= in.size();
      bw.Put(last ? 1 : 0, 1);
      bw.Put(0, 2);
      bw.Flush();
      out.push_back(static_cast<char>(n & 0xFF));
      out.push_back(static_cast<char>(n >> 8));
      out.push_back(static_cast<char>(~n & 0xFF));
      out.push_back(static_cast<char>((~n >> 8) & 0xFF));
      out.append(in.substr(pos, n));
      pos += n;
    } while (pos < in.size());
    return out;
  }
  constexpr size_t kWindow = 32768;
  constexpr size_t kHashBits = 15;
  const int max_chain = level >= 8 ? 1024 : (level >= 5 ? 128 : 16);
  const bool lazy = level >= 4;
  const uint8_t* data = reinterpret_cast<const uint8_t*>(in.data());
  size_t n = in.size();
  std::vector<int32_t> head(1u << kHashBits, -1);
  std::vector<int32_t> prev(kWindow, -1);
  auto hash = [&](size_t i) {
    uint32_t h = (static_cast<uint32_t>(data[i]) << 16) |
                 (static_cast<uint32_t>(data[i + 1]) << 8) | data[i + 2];
    return (h * 2654435761u) >> (32 - kHashBits);
  };
  auto insert = [&](size_t i) {
    if (i + 2 >= n) return;
    uint32_t h = hash(i);
    prev[i & (kWindow - 1)] = head[h];
    head[h] = static_cast<int32_t>(i);
  };
  auto longest = [&](size_t i, size_t& best_dist) -> size_t {
    if (i + 2 >= n) return 0;
    size_t best = 0;
    int32_t cand = head[hash(i)];
    int chain = max_chain;
    size_t limit = std::min<size_t>(258, n - i);
    while (cand >= 0 && chain-- > 0) {
      size_t c = static_cast<size_t>(cand);
      if (i - c > kWindow - 262) break;
      if (data[c + best] == data[i + best] || best == 0) {
        size_t len = 0;
        while (len < limit && data[c + len] == data[i + len]) ++len;
        if (len > best) {
          best = len;
          best_dist = i - c;
          if (len == limit) break;
        }
      }
      int32_t p = prev[c & (kWindow - 1)];
      if (p >= cand) break;
      cand = p;
    }
    return best >= 3 ? best : 0;
  };
  std::vector<Symbol> syms;
  syms.reserve(1 << 16);
  size_t i = 0;
  while (i < n) {
    size_t dist = 0;
    size_t len = longest(i, dist);
    if (len >= 3 && lazy && i + 1 < n) {
      insert(i);
      size_t dist2 = 0;
      size_t len2 = longest(i + 1, dist2);
      if (len2 > len) {
        syms.push_back(Symbol{data[i], 0});
        ++i;
        syms.push_back(Symbol{static_cast<uint16_t>(len2), static_cast<uint16_t>(dist2)});
        for (size_t k = 0; k < len2; ++k) insert(i + k);
        i += len2;
      } else {
        syms.push_back(Symbol{static_cast<uint16_t>(len), static_cast<uint16_t>(dist)});
        for (size_t k = 1; k < len; ++k) insert(i + k);
        i += len;
      }
    } else if (len >= 3) {
      syms.push_back(Symbol{static_cast<uint16_t>(len), static_cast<uint16_t>(dist)});
      for (size_t k = 0; k < len; ++k) insert(i + k);
      i += len;
    } else {
      syms.push_back(Symbol{data[i], 0});
      insert(i);
      ++i;
    }
    if (syms.size() >= 60000) {
      WriteBlock(bw, syms, false);
      syms.clear();
    }
  }
  WriteBlock(bw, syms, true);
  bw.Flush();
  return out;
}

}
