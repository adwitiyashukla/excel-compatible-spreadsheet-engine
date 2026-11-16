#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "core.h"

namespace sheet {

constexpr uint32_t kNil = 0xFFFFFFFFu;

inline uint64_t CellKey(int sheet, int row, int col) {
  return (static_cast<uint64_t>(sheet) << 34) | (static_cast<uint64_t>(row) << 14) |
         static_cast<uint64_t>(col);
}

class DepGraph {
 public:
  DepGraph();

  void AddCell(uint64_t key, uint32_t f);
  void RemoveCell(uint64_t key, uint32_t f);
  void AddArea(const Area& a, uint32_t f);
  void RemoveArea(const Area& a, uint32_t f);
  void Clear();

  template <typename F>
  void CellDependents(uint64_t key, F&& fn) const {
    uint32_t head = Find(key);
    for (uint32_t n = head; n != kNil; n = nodes_[n].next) fn(nodes_[n].formula);
  }

  template <typename F>
  void Stab(int sheet, int row, int col, uint32_t epoch, F&& fn) {
    if (sheet >= static_cast<int>(sheets_.size())) return;
    SheetIndex& si = sheets_[sheet];
    if (col < static_cast<int>(si.buckets.size())) {
      Bucket& b = si.buckets[col];
      if (!b.entries.empty()) {
        if (b.dirty) Rebuild(b);
        StabBucket(b, row, col, false, epoch, fn);
      }
    }
    size_t block = static_cast<size_t>(col) / kBlockCols;
    if (block < si.blocks.size()) {
      Bucket& b = si.blocks[block];
      if (!b.entries.empty()) {
        if (b.dirty) Rebuild(b);
        StabBucket(b, row, col, true, epoch, fn);
      }
    }
  }

  void EndEpoch();

  size_t cell_edges() const { return cell_edges_; }
  size_t area_edges() const { return area_edges_; }
  size_t live_rects() const { return live_rects_; }

 private:
  struct Node {
    uint32_t formula;
    uint32_t next;
  };
  struct Slot {
    uint64_t key;
    uint32_t head;
  };
  struct Rect {
    Area area;
    uint32_t head = kNil;
    uint32_t count = 0;
    uint32_t fired = 0;
    bool wide = false;
  };
  struct Entry {
    int32_t r1;
    int32_t r2;
    uint32_t rect;
  };
  struct Bucket {
    std::vector<Entry> entries;
    std::vector<int32_t> tree;
    uint32_t dead = 0;
    bool dirty = false;
  };
  struct SheetIndex {
    std::vector<Bucket> buckets;
    std::vector<Bucket> blocks;
  };
  static constexpr int kBlockCols = 64;
  struct AreaHash {
    size_t operator()(const Area& a) const {
      uint64_t h = Mix64(static_cast<uint64_t>(a.sheet) * 0x9E3779B97F4A7C15ULL ^
                         (static_cast<uint64_t>(a.r1) << 20) ^ static_cast<uint64_t>(a.c1));
      return static_cast<size_t>(
          Mix64(h ^ (static_cast<uint64_t>(a.r2) << 24) ^ static_cast<uint64_t>(a.c2)));
    }
  };

  uint32_t Find(uint64_t key) const;
  uint32_t* FindOrInsert(uint64_t key);
  void Grow();
  uint32_t NewNode(uint32_t formula, uint32_t next);
  void FreeNode(uint32_t n);
  bool RemoveFromList(uint32_t& head, uint32_t formula);
  void Rebuild(Bucket& b);
  void SetLeaf(Bucket& b, size_t i, int32_t v);

  template <typename F>
  void Emit(const Rect& r, F&& fn) {
    for (uint32_t n = r.head; n != kNil; n = nodes_[n].next) fn(nodes_[n].formula);
  }

  template <typename F>
  void StabBucket(Bucket& b, int row, int col, bool check_col, uint32_t epoch, F&& fn) {
    size_t n = b.entries.size();
    size_t lo = 0;
    size_t hi = n;
    while (lo < hi) {
      size_t mid = (lo + hi) / 2;
      if (b.entries[mid].r1 <= row) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    size_t limit = lo;
    if (limit == 0) return;
    size_t size = b.tree.size() / 2;
    struct Frame {
      size_t node;
      size_t lo;
      size_t hi;
    };
    Frame stack[80];
    int sp = 0;
    stack[sp++] = {1, 0, size};
    while (sp > 0) {
      Frame f = stack[--sp];
      if (f.lo >= limit || b.tree[f.node] < row) continue;
      if (f.hi - f.lo == 1) {
        Rect& r = rects_[b.entries[f.lo].rect];
        bool dead = r.count == 0 || r.fired == epoch;
        if (!dead && check_col && (col < r.area.c1 || col > r.area.c2)) continue;
        SetLeaf(b, f.lo, -1);
        restore_.push_back({&b, f.lo});
        if (dead) continue;
        r.fired = epoch;
        Emit(r, fn);
        continue;
      }
      size_t mid = (f.lo + f.hi) / 2;
      stack[sp++] = {f.node * 2 + 1, mid, f.hi};
      stack[sp++] = {f.node * 2, f.lo, mid};
    }
  }

  std::vector<Slot> slots_;
  size_t used_slots_ = 0;
  std::vector<Node> nodes_;
  uint32_t free_node_ = kNil;
  std::vector<Rect> rects_;
  std::unordered_map<Area, uint32_t, AreaHash> rect_ids_;
  std::vector<SheetIndex> sheets_;
  struct Restore {
    Bucket* bucket;
    size_t index;
  };
  std::vector<Restore> restore_;
  size_t cell_edges_ = 0;
  size_t area_edges_ = 0;
  size_t live_rects_ = 0;
};

}
