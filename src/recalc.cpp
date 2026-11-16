#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <vector>

#include "eval.h"
#include "threadpool.h"
#include "workbook.h"

namespace sheet {

struct RecalcRun {
  using Component = CalcComponent;

  struct DirtyEntry {
    int32_t sheet;
    int32_t col;
    int32_t row;
    uint32_t id;
  };

  struct Frame {
    uint32_t f;
    uint32_t first;
    uint32_t last;
    uint32_t area;
    int col;
    int row;
    size_t pos;
    bool started;
    bool self;
    uint32_t max_level;
  };

  Workbook& wb;
  RecalcStats& stats;
  std::vector<uint32_t> members;
  std::vector<Component> comps;
  std::vector<uint32_t> level_items;
  std::vector<uint32_t> level_start;
  size_t plain = 0;
  bool filtered = false;
  std::vector<uint8_t> comp_done;
  std::vector<Area> areas;
  std::vector<DirtyEntry> index;
  bool use_index = false;
  uint32_t epoch = 0;
  const std::vector<uint32_t>* owner = nullptr;

  RecalcRun(Workbook& w, RecalcStats& s) : wb(w), stats(s) {}

  uint32_t OwnerOf(uint32_t id) const {
    if (owner) return id < owner->size() ? (*owner)[id] : kNil;
    const FormulaCell* f = wb.FormulaAt(id);
    return f->visit == epoch ? f->comp : kNil;
  }

  size_t Lower(int sheet, int col, int row) const {
    auto it = std::lower_bound(index.begin(), index.end(), DirtyEntry{sheet, col, row, 0},
                               [](const DirtyEntry& x, const DirtyEntry& y) {
                                 if (x.sheet != y.sheet) return x.sheet < y.sheet;
                                 if (x.col != y.col) return x.col < y.col;
                                 return x.row < y.row;
                               });
    return static_cast<size_t>(it - index.begin());
  }

  uint32_t NextIndexed(Frame& it, const Area& a, int c2) {
    if (!it.started) {
      it.started = true;
      it.col = a.c1;
      it.pos = Lower(a.sheet, it.col, a.r1);
    }
    while (it.col <= c2 && it.pos < index.size()) {
      const DirtyEntry& e = index[it.pos];
      if (e.sheet != a.sheet || e.col > c2) break;
      if (e.col == it.col && e.row <= a.r2) {
        ++it.pos;
        return e.id;
      }
      it.col = e.col > it.col ? e.col : it.col + 1;
      it.pos = Lower(a.sheet, it.col, a.r1);
    }
    return kNil;
  }

  uint32_t NextEdge(Frame& it) {
    while (it.area < it.last) {
      const Area a = areas[it.area];
      if (a.sheet < 0 || a.sheet >= wb.SheetCount()) {
        ++it.area;
        it.started = false;
        continue;
      }
      if (a.r1 == a.r2 && a.c1 == a.c2) {
        ++it.area;
        it.started = false;
        const Cell* c = wb.FindCell(a.sheet, a.r1, a.c1);
        if (c && c->formula != 0 && c->state.load(std::memory_order_relaxed) == kDirty) {
          return c->formula;
        }
        continue;
      }
      const SheetData& sd = wb.sheets_[a.sheet];
      int c2 = std::min(a.c2, static_cast<int>(sd.cols.size()) - 1);
      if (use_index) {
        uint32_t g = NextIndexed(it, a, c2);
        if (g != kNil) return g;
        ++it.area;
        it.started = false;
        continue;
      }
      if (!it.started) {
        it.started = true;
        it.col = a.c1;
        it.row = a.r1;
      }
      while (it.col <= c2) {
        const Column& column = sd.cols[it.col];
        int last_row = std::min(a.r2, static_cast<int>(column.chunks.size() * kChunkRows) - 1);
        while (it.row <= last_row) {
          size_t k = static_cast<size_t>(it.row) >> kChunkShift;
          const Chunk* ch = column.chunks[k].get();
          if (!ch || ch->formulas == 0) {
            it.row = static_cast<int>((k + 1) << kChunkShift);
            continue;
          }
          const Cell& cell = ch->cells[it.row & (kChunkRows - 1)];
          ++it.row;
          if (cell.formula != 0 && cell.state.load(std::memory_order_relaxed) == kDirty) {
            return cell.formula;
          }
        }
        ++it.col;
        it.row = a.r1;
      }
      ++it.area;
      it.started = false;
    }
    return kNil;
  }

  void Order(const std::vector<uint32_t>& dirty) {
    epoch = ++wb.visit_epoch_;
    use_index = dirty.size() * 4 < wb.formula_count_;
    if (use_index) {
      index.reserve(dirty.size());
      for (uint32_t id : dirty) {
        const FormulaCell* f = wb.FormulaAt(id);
        index.push_back(DirtyEntry{f->sheet, f->col, f->row, id});
      }
      std::sort(index.begin(), index.end(), [](const DirtyEntry& x, const DirtyEntry& y) {
        if (x.sheet != y.sheet) return x.sheet < y.sheet;
        if (x.col != y.col) return x.col < y.col;
        return x.row < y.row;
      });
    }
    std::vector<Frame> frames;
    std::vector<uint32_t> tstack;
    uint32_t counter = 0;
    auto open = [&](uint32_t f) {
      FormulaCell* fc = wb.FormulaAt(f);
      fc->visit = epoch;
      fc->index = counter;
      fc->low = counter;
      fc->level = 0;
      fc->comp = kNil;
      ++counter;
      tstack.push_back(f);
      Frame fr{};
      fr.f = f;
      fr.first = static_cast<uint32_t>(areas.size());
      wb.CollectPrecedents(*fc, areas);
      fr.last = static_cast<uint32_t>(areas.size());
      fr.area = fr.first;
      frames.push_back(fr);
    };
    for (uint32_t root : dirty) {
      if (wb.FormulaAt(root)->visit == epoch) continue;
      open(root);
      while (!frames.empty()) {
        Frame& fr = frames.back();
        uint32_t g = NextEdge(fr);
        if (g != kNil) {
          FormulaCell* gc = wb.FormulaAt(g);
          if (g == fr.f) {
            fr.self = true;
            continue;
          }
          if (gc->visit != epoch) {
            open(g);
            continue;
          }
          FormulaCell* fc = wb.FormulaAt(fr.f);
          if (gc->comp == kNil) {
            fc->low = std::min(fc->low, gc->index);
          } else {
            fr.max_level = std::max(fr.max_level, gc->level);
          }
          continue;
        }
        uint32_t f = fr.f;
        FormulaCell* fc = wb.FormulaAt(f);
        uint32_t level_seen = fr.max_level;
        bool self = fr.self;
        areas.resize(fr.first);
        frames.pop_back();
        fc->level = level_seen;
        bool closed = false;
        if (fc->low == fc->index) {
          uint32_t start = static_cast<uint32_t>(members.size());
          uint32_t lvl = 0;
          while (true) {
            uint32_t m = tstack.back();
            tstack.pop_back();
            members.push_back(m);
            lvl = std::max(lvl, wb.FormulaAt(m)->level);
            if (m == f) break;
          }
          uint32_t size = static_cast<uint32_t>(members.size()) - start;
          uint32_t level = lvl + 1;
          bool cyclic = size > 1 || self;
          if (cyclic) {
            std::sort(members.begin() + start, members.end(), [&](uint32_t x, uint32_t y) {
              const FormulaCell* a = wb.FormulaAt(x);
              const FormulaCell* b = wb.FormulaAt(y);
              if (a->sheet != b->sheet) return a->sheet < b->sheet;
              if (a->row != b->row) return a->row < b->row;
              return a->col < b->col;
            });
          }
          uint32_t id = static_cast<uint32_t>(comps.size());
          for (uint32_t i = start; i < start + size; ++i) {
            FormulaCell* mc = wb.FormulaAt(members[i]);
            mc->level = level;
            mc->comp = id;
          }
          comps.push_back(Component{start, size, level, cyclic});
          closed = true;
        }
        if (!frames.empty()) {
          Frame& parent = frames.back();
          FormulaCell* pc = wb.FormulaAt(parent.f);
          if (closed) {
            parent.max_level = std::max(parent.max_level, fc->level);
          } else {
            pc->low = std::min(pc->low, fc->low);
            parent.max_level = std::max(parent.max_level, level_seen);
          }
        }
      }
    }
  }

  Value Compute(FormulaCell& f) {
    EvalContext ctx(wb, f.sheet, f.base_row, f.base_col);
    bool array = (f.flags & kFlagArray) != 0;
    if (array) {
      auto it = wb.arrays_.find(CellKey(f.sheet, f.base_row, f.base_col));
      if (it != wb.arrays_.end()) {
        ctx.array_rows = it->second.rows();
        ctx.array_cols = it->second.cols();
      }
    }
    Value v = EvaluateFormula(*f.prog, ctx, array);
    if (array && v.IsArray()) {
      uint32_t dr = static_cast<uint32_t>(f.row - f.base_row);
      uint32_t dc = static_cast<uint32_t>(f.col - f.base_col);
      uint32_t r = v.rows() == 1 ? 0 : dr;
      uint32_t c = v.cols() == 1 ? 0 : dc;
      if (r >= v.rows() || c >= v.cols()) return Value::Error(Err::NA);
      Value e = v.at(r, c);
      return e;
    }
    return v;
  }

  void Store(FormulaCell& f, Value v) {
    f.cell->value = std::move(v);
    f.flags &= static_cast<uint8_t>(~kFlagCycle);
    f.cell->state.store(kClean, std::memory_order_release);
  }

  bool TryEval(uint32_t id) {
    FormulaCell& f = *wb.FormulaAt(id);
    if (f.cell->state.load(std::memory_order_relaxed) == kClean) return true;
    f.cell->state.store(kComputing, std::memory_order_relaxed);
    try {
      Value v = Compute(f);
      Store(f, std::move(v));
      return true;
    } catch (const PendingCell&) {
      f.cell->state.store(kDirty, std::memory_order_relaxed);
      return false;
    }
  }

  void Demand(uint32_t root) {
    std::vector<uint32_t> stack{root};
    while (!stack.empty()) {
      uint32_t id = stack.back();
      FormulaCell& f = *wb.FormulaAt(id);
      if (f.cell->state.load(std::memory_order_relaxed) == kClean) {
        stack.pop_back();
        continue;
      }
      f.cell->state.store(kComputing, std::memory_order_relaxed);
      try {
        Value v = Compute(f);
        Store(f, std::move(v));
        stack.pop_back();
      } catch (const PendingCell& p) {
        ++stats.retries;
        FormulaCell& g = *wb.FormulaAt(p.formula);
        uint8_t st = g.cell->state.load(std::memory_order_relaxed);
        uint32_t k = OwnerOf(p.formula);
        if (st == kDirty && k != kNil && comps[k].cyclic && !comp_done[k]) {
          RunCycle(k);
          continue;
        }
        if (st == kComputing) {
          auto pos = std::find(stack.begin(), stack.end(), p.formula);
          size_t from =
              pos == stack.end() ? stack.size() - 1 : static_cast<size_t>(pos - stack.begin());
          for (size_t i = from; i < stack.size(); ++i) {
            FormulaCell& c = *wb.FormulaAt(stack[i]);
            Store(c, Value::Error(Err::Ref));
            c.flags |= kFlagCycle;
            ++stats.cycles;
          }
          stack.resize(from);
          continue;
        }
        stack.push_back(p.formula);
      }
    }
  }

  void RunCycle(uint32_t k) {
    if (comp_done[k]) return;
    comp_done[k] = 1;
    Cycle(comps[k]);
  }

  void Cycle(const Component& comp) {
    if (!wb.iterate_) {
      for (uint32_t i = comp.start; i < comp.start + comp.size; ++i) {
        FormulaCell& f = *wb.FormulaAt(members[i]);
        Store(f, Value::Error(Err::Ref));
        f.flags |= kFlagCycle;
      }
      stats.cycles += comp.size;
      return;
    }
    for (uint32_t i = comp.start; i < comp.start + comp.size; ++i) {
      FormulaCell& f = *wb.FormulaAt(members[i]);
      if (f.cell->value.IsBlank()) f.cell->value = Value::Number(0);
      f.cell->state.store(kIterating, std::memory_order_relaxed);
    }
    for (int iter = 0; iter < wb.max_iterations_; ++iter) {
      double delta = 0;
      for (uint32_t i = comp.start; i < comp.start + comp.size; ++i) {
        FormulaCell& f = *wb.FormulaAt(members[i]);
        Value v;
        while (true) {
          try {
            v = Compute(f);
            break;
          } catch (const PendingCell& p) {
            ++stats.retries;
            Demand(p.formula);
          }
        }
        const Value& old = f.cell->value;
        if (v.IsNumber() && old.IsNumber()) {
          delta = std::max(delta, std::fabs(v.number() - old.number()));
        } else if (!v.SameAs(old)) {
          delta = 1e300;
        }
        f.cell->value = std::move(v);
      }
      ++stats.evaluated;
      if (delta < wb.max_change_) break;
    }
    for (uint32_t i = comp.start; i < comp.start + comp.size; ++i) {
      FormulaCell& f = *wb.FormulaAt(members[i]);
      f.flags |= kFlagCycle;
      f.cell->state.store(kClean, std::memory_order_release);
    }
    stats.cycles += comp.size;
  }

  bool Clean(const Component& comp) const {
    return wb.FormulaAt(members[comp.start])->cell->state.load(std::memory_order_relaxed) == kClean;
  }

  void Sequential() {
    comp_done.assign(comps.size(), 0);
    for (uint32_t k = 0; k < comps.size(); ++k) {
      const Component& comp = comps[k];
      if (filtered && Clean(comp)) continue;
      if (comp.cyclic) {
        RunCycle(k);
        continue;
      }
      uint32_t id = members[comp.start];
      if (!filtered) ++stats.evaluated;
      if (!TryEval(id)) Demand(id);
    }
  }

  void Levels() {
    if (!level_start.empty()) return;
    uint32_t max_level = 0;
    for (const Component& c : comps) max_level = std::max(max_level, c.level);
    level_start.assign(static_cast<size_t>(max_level) + 2, 0);
    for (const Component& c : comps) ++level_start[c.level + 1];
    for (size_t i = 1; i < level_start.size(); ++i) level_start[i] += level_start[i - 1];
    std::vector<uint32_t> fill(level_start.begin(), level_start.end() - 1);
    level_items.resize(comps.size());
    plain = 0;
    for (uint32_t i = 0; i < comps.size(); ++i) {
      level_items[fill[comps[i].level]++] = i;
      plain += comps[i].cyclic ? 0 : 1;
    }
  }

  void Parallel(ThreadPool& pool) {
    comp_done.assign(comps.size(), 0);
    Levels();
    stats.levels = level_start.size() - 2;
    std::mutex mu;
    std::vector<std::pair<uint32_t, uint32_t>> deferred;
    if (!filtered) stats.evaluated += plain;
    const uint32_t* level = nullptr;
    uint32_t lv = 0;
    auto run = [&](size_t k) {
      const Component& comp = comps[level[k]];
      if (filtered && Clean(comp)) return;
      if (comp.cyclic) {
        std::lock_guard<std::mutex> lock(mu);
        deferred.emplace_back(lv, level[k] | 0x80000000u);
        return;
      }
      uint32_t id = members[comp.start];
      if (!TryEval(id)) {
        std::lock_guard<std::mutex> lock(mu);
        deferred.emplace_back(lv, id);
      }
    };
    std::function<void(size_t)> job = run;
    for (lv = 0; lv + 1 < level_start.size(); ++lv) {
      level = level_items.data() + level_start[lv];
      size_t count = level_start[lv + 1] - level_start[lv];
      if (count <= 128) {
        for (size_t k = 0; k < count; ++k) run(k);
      } else {
        pool.ParallelFor(count, 64, job);
      }
    }
    stats.deferred = deferred.size();
    std::sort(deferred.begin(), deferred.end());
    for (const auto& d : deferred) {
      if (d.second & 0x80000000u) {
        RunCycle(d.second & 0x7FFFFFFFu);
      } else {
        Demand(d.second);
      }
    }
  }
};

RecalcStats Workbook::Run(bool full) {
  auto t0 = std::chrono::steady_clock::now();
  RecalcStats stats;
  stats.threads = threads_;
  ++recalc_serial_;
  double pinned = now_;
  if (std::isnan(now_)) now_ = Now();
  if (!deps_valid_) {
    auto tg = std::chrono::steady_clock::now();
    RebuildDependencies();
    stats.graph_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - tg).count();
  }
  std::vector<uint32_t> dirty;
  bool chain_ok = chain_.valid && chain_.version == graph_version_;
  bool reuse = full && chain_ok;
  size_t count = 0;
  if (reuse) {
    const std::vector<uint32_t>& ids = chain_.members;
    size_t blocks = (ids.size() + 4095) / 4096;
    std::function<void(size_t)> mark = [&](size_t b) {
      size_t end = std::min(ids.size(), (b + 1) * 4096);
      for (size_t k = b * 4096; k < end; ++k) {
        FormulaAt(ids[k])->cell->state.store(kDirty, std::memory_order_relaxed);
      }
    };
    if (pool_ && threads_ > 1) {
      pool_->ParallelFor(blocks, 1, mark);
    } else {
      for (size_t b = 0; b < blocks; ++b) mark(b);
    }
    count = ids.size();
  } else if (full) {
    for (uint32_t id = 1; id < next_formula_; ++id) {
      FormulaCell* f = FormulaAt(id);
      if (!f->alive) continue;
      f->cell->state.store(kDirty, std::memory_order_relaxed);
      dirty.push_back(id);
    }
  } else {
    ++epoch_;
    std::vector<CellPos> work;
    work.swap(changed_);
    auto mark = [&](uint32_t id) {
      FormulaCell* f = FormulaAt(id);
      if (!f->alive || f->cell->state.load(std::memory_order_relaxed) == kDirty) return;
      f->cell->state.store(kDirty, std::memory_order_relaxed);
      dirty.push_back(id);
      work.push_back(CellPos{f->sheet, f->row, f->col});
    };
    for (uint32_t id : fresh_) mark(id);
    size_t keep = 0;
    for (uint32_t id : volatiles_) {
      FormulaCell* f = FormulaAt(id);
      if (!f->alive || !(f->flags & kFlagVolatile)) continue;
      volatiles_[keep++] = id;
      mark(id);
    }
    volatiles_.resize(keep);
    size_t head = 0;
    while (head < work.size()) {
      CellPos p = work[head++];
      deps_.CellDependents(CellKey(p.sheet, p.row, p.col), mark);
      deps_.Stab(p.sheet, p.row, p.col, epoch_, mark);
    }
    deps_.EndEpoch();
  }
  if (!reuse) count = dirty.size();
  bool filtered = !full && chain_ok && count >= 256 && count * 8 >= chain_.members.size();
  bool use_chain = reuse || filtered;
  changed_.clear();
  fresh_.clear();
  if (count > 0) {
    RecalcRun run(*this, stats);
    auto t1 = std::chrono::steady_clock::now();
    if (filtered) {
      run.filtered = true;
      for (uint32_t id : dirty) {
        uint32_t k = id < chain_.owner.size() ? chain_.owner[id] : kNil;
        if (k != kNil && !chain_.comps[k].cyclic) ++stats.evaluated;
      }
    }
    if (use_chain) {
      run.members.swap(chain_.members);
      run.comps.swap(chain_.comps);
      run.level_items.swap(chain_.level_items);
      run.level_start.swap(chain_.level_start);
      run.plain = chain_.plain;
      run.owner = &chain_.owner;
      chain_.valid = false;
      stats.chain_reused = true;
    } else {
      run.Order(dirty);
    }
    stats.order_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
    if (pool_ && threads_ > 1 && count >= 256) {
      run.Parallel(*pool_);
    } else {
      if (run.level_start.empty()) {
        uint32_t max_level = 0;
        for (const auto& c : run.comps) max_level = std::max(max_level, c.level);
        stats.levels = max_level;
      } else {
        stats.levels = run.level_start.size() - 2;
      }
      run.Sequential();
    }
    if (use_chain || full) {
      if (!use_chain) {
        chain_.owner.assign(next_formula_, kNil);
        for (uint32_t k = 0; k < run.comps.size(); ++k) {
          const CalcComponent& c = run.comps[k];
          for (uint32_t i = c.start; i < c.start + c.size; ++i) chain_.owner[run.members[i]] = k;
        }
      }
      chain_.members.swap(run.members);
      chain_.comps.swap(run.comps);
      chain_.level_items.swap(run.level_items);
      chain_.level_start.swap(run.level_start);
      chain_.plain = run.plain;
      chain_.version = graph_version_;
      chain_.valid = true;
    }
  }
  if (full && programs_.size() > 2 * formula_count_ + 1024) {
    for (auto it = programs_.begin(); it != programs_.end();) {
      auto& bucket = it->second;
      bucket.erase(std::remove_if(bucket.begin(), bucket.end(),
                                  [](const ProgramRef& p) { return p.use_count() == 1; }),
                   bucket.end());
      it = bucket.empty() ? programs_.erase(it) : std::next(it);
    }
  }
  now_ = pinned;
  stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return stats;
}

RecalcStats Workbook::Recalc() {
  return Run(false);
}
RecalcStats Workbook::RecalcAll() {
  return Run(true);
}

}
