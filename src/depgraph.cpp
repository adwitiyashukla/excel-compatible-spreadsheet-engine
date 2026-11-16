#include "depgraph.h"

#include <algorithm>

namespace sheet {

namespace {

constexpr uint64_t kEmptyKey = ~0ULL;
constexpr int kNarrowCols = 16;

}

DepGraph::DepGraph() {
  slots_.assign(1024, Slot{kEmptyKey, kNil});
}

uint32_t DepGraph::Find(uint64_t key) const {
  size_t mask = slots_.size() - 1;
  size_t i = static_cast<size_t>(Mix64(key)) & mask;
  while (true) {
    const Slot& s = slots_[i];
    if (s.key == key) return s.head;
    if (s.key == kEmptyKey) return kNil;
    i = (i + 1) & mask;
  }
}

void DepGraph::Grow() {
  std::vector<Slot> old;
  old.swap(slots_);
  slots_.assign(old.size() * 2, Slot{kEmptyKey, kNil});
  used_slots_ = 0;
  size_t mask = slots_.size() - 1;
  for (const Slot& s : old) {
    if (s.key == kEmptyKey || s.head == kNil) continue;
    size_t i = static_cast<size_t>(Mix64(s.key)) & mask;
    while (slots_[i].key != kEmptyKey) i = (i + 1) & mask;
    slots_[i] = s;
    ++used_slots_;
  }
}

uint32_t* DepGraph::FindOrInsert(uint64_t key) {
  if ((used_slots_ + 1) * 4 >= slots_.size() * 3) Grow();
  size_t mask = slots_.size() - 1;
  size_t i = static_cast<size_t>(Mix64(key)) & mask;
  while (true) {
    Slot& s = slots_[i];
    if (s.key == key) return &s.head;
    if (s.key == kEmptyKey) {
      s.key = key;
      s.head = kNil;
      ++used_slots_;
      return &s.head;
    }
    i = (i + 1) & mask;
  }
}

uint32_t DepGraph::NewNode(uint32_t formula, uint32_t next) {
  if (free_node_ != kNil) {
    uint32_t n = free_node_;
    free_node_ = nodes_[n].next;
    nodes_[n] = Node{formula, next};
    return n;
  }
  nodes_.push_back(Node{formula, next});
  return static_cast<uint32_t>(nodes_.size() - 1);
}

void DepGraph::FreeNode(uint32_t n) {
  nodes_[n].next = free_node_;
  free_node_ = n;
}

bool DepGraph::RemoveFromList(uint32_t& head, uint32_t formula) {
  uint32_t* link = &head;
  while (*link != kNil) {
    uint32_t n = *link;
    if (nodes_[n].formula == formula) {
      *link = nodes_[n].next;
      FreeNode(n);
      return true;
    }
    link = &nodes_[n].next;
  }
  return false;
}

void DepGraph::AddCell(uint64_t key, uint32_t f) {
  uint32_t* head = FindOrInsert(key);
  *head = NewNode(f, *head);
  ++cell_edges_;
}

void DepGraph::RemoveCell(uint64_t key, uint32_t f) {
  size_t mask = slots_.size() - 1;
  size_t i = static_cast<size_t>(Mix64(key)) & mask;
  while (true) {
    Slot& s = slots_[i];
    if (s.key == key) {
      if (RemoveFromList(s.head, f)) --cell_edges_;
      return;
    }
    if (s.key == kEmptyKey) return;
    i = (i + 1) & mask;
  }
}

void DepGraph::AddArea(const Area& a, uint32_t f) {
  auto it = rect_ids_.find(a);
  uint32_t id;
  if (it == rect_ids_.end()) {
    id = static_cast<uint32_t>(rects_.size());
    Rect r;
    r.area = a;
    r.wide = a.cols() > kNarrowCols;
    rects_.push_back(r);
    rect_ids_.emplace(a, id);
    if (static_cast<int>(sheets_.size()) <= a.sheet)
      sheets_.resize(static_cast<size_t>(a.sheet) + 1);
    SheetIndex& si = sheets_[a.sheet];
    if (r.wide) {
      size_t last = static_cast<size_t>(a.c2) / kBlockCols;
      if (si.blocks.size() <= last) si.blocks.resize(last + 1);
      for (size_t blk = static_cast<size_t>(a.c1) / kBlockCols; blk <= last; ++blk) {
        Bucket& b = si.blocks[blk];
        b.entries.push_back(Entry{a.r1, a.r2, id});
        b.dirty = true;
      }
    } else {
      if (static_cast<int>(si.buckets.size()) <= a.c2)
        si.buckets.resize(static_cast<size_t>(a.c2) + 1);
      for (int c = a.c1; c <= a.c2; ++c) {
        Bucket& b = si.buckets[c];
        b.entries.push_back(Entry{a.r1, a.r2, id});
        b.dirty = true;
      }
    }
    ++live_rects_;
  } else {
    id = it->second;
  }
  Rect& r = rects_[id];
  r.head = NewNode(f, r.head);
  ++r.count;
  ++area_edges_;
}

void DepGraph::RemoveArea(const Area& a, uint32_t f) {
  auto it = rect_ids_.find(a);
  if (it == rect_ids_.end()) return;
  uint32_t id = it->second;
  Rect& r = rects_[id];
  if (!RemoveFromList(r.head, f)) return;
  --area_edges_;
  if (--r.count > 0) return;
  rect_ids_.erase(it);
  --live_rects_;
  SheetIndex& si = sheets_[a.sheet];
  if (r.wide) {
    for (size_t blk = static_cast<size_t>(a.c1) / kBlockCols;
         blk <= static_cast<size_t>(a.c2) / kBlockCols; ++blk) {
      Bucket& b = si.blocks[blk];
      if (++b.dead * 2 > b.entries.size()) b.dirty = true;
    }
    return;
  }
  for (int c = a.c1; c <= a.c2; ++c) {
    Bucket& b = si.buckets[c];
    if (++b.dead * 2 > b.entries.size()) b.dirty = true;
  }
}

void DepGraph::Rebuild(Bucket& b) {
  size_t keep = 0;
  for (size_t i = 0; i < b.entries.size(); ++i) {
    if (rects_[b.entries[i].rect].count > 0) b.entries[keep++] = b.entries[i];
  }
  b.entries.resize(keep);
  b.dead = 0;
  std::sort(b.entries.begin(), b.entries.end(), [](const Entry& x, const Entry& y) {
    if (x.r1 != y.r1) return x.r1 < y.r1;
    return x.rect < y.rect;
  });
  size_t size = 1;
  while (size < b.entries.size()) size <<= 1;
  b.tree.assign(size * 2, -1);
  for (size_t i = 0; i < b.entries.size(); ++i) b.tree[size + i] = b.entries[i].r2;
  for (size_t i = size; i-- > 1;) b.tree[i] = std::max(b.tree[2 * i], b.tree[2 * i + 1]);
  b.dirty = false;
}

void DepGraph::SetLeaf(Bucket& b, size_t i, int32_t v) {
  size_t size = b.tree.size() / 2;
  size_t idx = size + i;
  b.tree[idx] = v;
  idx >>= 1;
  while (idx >= 1) {
    int32_t m = std::max(b.tree[2 * idx], b.tree[2 * idx + 1]);
    if (b.tree[idx] == m) break;
    b.tree[idx] = m;
    idx >>= 1;
  }
}

void DepGraph::EndEpoch() {
  for (auto it = restore_.rbegin(); it != restore_.rend(); ++it) {
    Bucket& b = *it->bucket;
    if (it->index < b.entries.size() && b.tree.size() / 2 > it->index) {
      size_t size = b.tree.size() / 2;
      size_t idx = size + it->index;
      b.tree[idx] = b.entries[it->index].r2;
      idx >>= 1;
      while (idx >= 1) {
        b.tree[idx] = std::max(b.tree[2 * idx], b.tree[2 * idx + 1]);
        idx >>= 1;
      }
    }
  }
  restore_.clear();
}

void DepGraph::Clear() {
  slots_.assign(1024, Slot{kEmptyKey, kNil});
  used_slots_ = 0;
  nodes_.clear();
  free_node_ = kNil;
  rects_.clear();
  rect_ids_.clear();
  sheets_.clear();
  restore_.clear();
  cell_edges_ = 0;
  area_edges_ = 0;
  live_rects_ = 0;
}

}
