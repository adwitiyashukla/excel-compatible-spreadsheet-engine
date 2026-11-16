#include <algorithm>
#include <string>
#include <vector>

#include "eval.h"
#include "workbook.h"

namespace sheet {

struct StructureEdit {
  int sheet;
  int at;
  int count;
  bool rows;
  bool insert;

  bool MapIndex(long& v) const {
    if (insert) {
      if (v >= at) v += count;
      return true;
    }
    if (v >= at && v < at + count) return false;
    if (v >= at + count) v -= count;
    return true;
  }

  bool MapSpan(long& lo, long& hi, long limit) const {
    if (insert) {
      if (lo >= at) {
        lo += count;
        hi += count;
      } else if (hi >= at) {
        hi += count;
      }
      if (hi >= limit) hi = limit - 1;
      return lo <= hi;
    }
    long nlo = lo < at ? lo : (lo >= at + count ? lo - count : at);
    long nhi = hi < at ? hi : (hi >= at + count ? hi - count : at - 1);
    lo = nlo;
    hi = nhi;
    return lo <= hi;
  }

  bool Touches(const RefSpec& s) const { return rows ? !s.full_cols : !s.full_rows; }

  bool MapRef(RefSpec& s, int formula_sheet, int old_row, int old_col, int new_row,
              int new_col) const {
    long r1 = s.r1_abs ? s.r1 : static_cast<long>(old_row) + s.r1;
    long r2 = s.r2_abs ? s.r2 : static_cast<long>(old_row) + s.r2;
    long c1 = s.c1_abs ? s.c1 : static_cast<long>(old_col) + s.c1;
    long c2 = s.c2_abs ? s.c2 : static_cast<long>(old_col) + s.c2;
    int target = s.sheet >= 0 ? s.sheet : formula_sheet;
    bool alive = true;
    if (target == sheet && s.sheet2 < 0 && Touches(s)) {
      long& lo = rows ? r1 : c1;
      long& hi = rows ? r2 : c2;
      long limit = rows ? kMaxRows : kMaxCols;
      if (lo >= 0 && hi >= 0) {
        if (!s.area && lo == hi) {
          alive = MapIndex(lo);
          hi = lo;
        } else {
          alive = MapSpan(lo, hi, limit);
        }
      }
    }
    s.r1 = static_cast<int32_t>(s.r1_abs ? r1 : r1 - new_row);
    s.r2 = static_cast<int32_t>(s.r2_abs ? r2 : r2 - new_row);
    s.c1 = static_cast<int32_t>(s.c1_abs ? c1 : c1 - new_col);
    s.c2 = static_cast<int32_t>(s.c2_abs ? c2 : c2 - new_col);
    return alive;
  }

  ProgramRef Rewrite(const Program& p, int formula_sheet, int old_row, int old_col, int new_row,
                     int new_col) const {
    auto q = std::make_shared<Program>(p);
    q->refs.clear();
    std::vector<int> remap(p.refs.size(), -1);
    for (size_t i = 0; i < p.refs.size(); ++i) {
      RefSpec s = p.refs[i];
      if (MapRef(s, formula_sheet, old_row, old_col, new_row, new_col)) {
        remap[i] = static_cast<int>(q->refs.size());
        q->refs.push_back(s);
      }
    }
    for (Instr& in : q->code) {
      if (in.op != Op::PushRef) continue;
      int to = remap[in.a];
      if (to < 0) {
        in.op = Op::PushRefErr;
        in.a = 0;
      } else {
        in.a = static_cast<uint32_t>(to);
      }
    }
    q->hash = HashProgram(*q);
    return q;
  }

  bool MapCell(int& row, int& col) const {
    long v = rows ? row : col;
    if (!MapIndex(v)) return false;
    (rows ? row : col) = static_cast<int>(v);
    return true;
  }

  static bool Apply(Workbook& wb, int sheet, int at, int count, bool rows, bool insert,
                    std::string* error) {
    if (sheet < 0 || sheet >= wb.SheetCount() || count <= 0) {
      if (error) *error = "bad arguments";
      return false;
    }
    int limit = rows ? kMaxRows : kMaxCols;
    if (at < 0 || at >= limit || (!insert && at + count > limit)) {
      if (error) *error = "position out of range";
      return false;
    }
    StructureEdit e{sheet, at, count, rows, insert};
    SheetData& sd = wb.sheets_[sheet];
    if (insert) {
      int used = rows ? sd.max_row : sd.max_col;
      if (used >= at && used + count >= limit) {
        if (error) *error = "cells would be pushed off the sheet";
        return false;
      }
    }
    for (const auto& kv : wb.arrays_) {
      const Area& g = kv.second;
      if (g.sheet != sheet) continue;
      int lo = rows ? g.r1 : g.c1;
      int hi = rows ? g.r2 : g.c2;
      bool split = insert ? (lo < at && hi >= at)
                          : !(hi < at || lo >= at + count || (lo >= at && hi < at + count));
      if (split) {
        if (error) *error = "cannot change part of an array formula";
        return false;
      }
    }
    wb.deps_valid_ = false;
    ++wb.graph_version_;
    struct Moved {
      uint32_t id;
      int row;
      int col;
      int base_row;
      int base_col;
      ProgramRef prog;
    };
    std::vector<Moved> moved;
    for (uint32_t id = 1; id < wb.next_formula_; ++id) {
      FormulaCell* f = wb.FormulaAt(id);
      if (!f->alive) continue;
      int row = f->row;
      int col = f->col;
      int brow = f->base_row;
      int bcol = f->base_col;
      if (f->sheet == sheet) {
        int r = row;
        int c = col;
        if (!e.MapCell(r, c)) continue;
        row = r;
        col = c;
        int br = brow;
        int bc = bcol;
        if (e.MapCell(br, bc)) {
          brow = br;
          bcol = bc;
        }
      }
      ProgramRef np = e.Rewrite(*f->prog, f->sheet, f->base_row, f->base_col, brow, bcol);
      moved.push_back(Moved{id, row, col, brow, bcol, wb.Intern(np)});
    }
    std::vector<Column> fresh(rows ? sd.cols.size() : 0);
    if (!rows) {
      size_t ncols = sd.cols.size() + (insert ? static_cast<size_t>(count) : 0);
      fresh.resize(ncols);
    }
    int max_row = -1;
    int max_col = -1;
    for (size_t c = 0; c < sd.cols.size(); ++c) {
      Column& column = sd.cols[c];
      for (size_t k = 0; k < column.chunks.size(); ++k) {
        Chunk* ch = column.chunks[k].get();
        if (!ch || ch->used == 0) continue;
        for (int i = 0; i < kChunkRows; ++i) {
          Cell& cell = ch->cells[i];
          if (cell.value.IsBlank() && cell.formula == 0) continue;
          int r = static_cast<int>(k) * kChunkRows + i;
          int cc = static_cast<int>(c);
          if (!e.MapCell(r, cc)) {
            if (cell.formula != 0) wb.FreeFormula(cell.formula);
            continue;
          }
          if (static_cast<size_t>(cc) >= fresh.size()) fresh.resize(static_cast<size_t>(cc) + 1);
          Column& dst = fresh[static_cast<size_t>(cc)];
          size_t dk = static_cast<size_t>(r) >> kChunkShift;
          if (dk >= dst.chunks.size()) dst.chunks.resize(dk + 1);
          if (!dst.chunks[dk]) dst.chunks[dk] = std::make_unique<Chunk>();
          Chunk* d = dst.chunks[dk].get();
          Cell& out = d->cells[r & (kChunkRows - 1)];
          out.value = std::move(cell.value);
          out.formula = cell.formula;
          out.state.store(cell.state.load(std::memory_order_relaxed), std::memory_order_relaxed);
          ++d->used;
          if (out.formula != 0) ++d->formulas;
          max_row = std::max(max_row, r);
          max_col = std::max(max_col, cc);
        }
      }
    }
    sd.cols = std::move(fresh);
    sd.max_row = max_row;
    sd.max_col = max_col;
    if (rows && !sd.row_flags.empty()) {
      std::vector<uint8_t> flags;
      for (size_t r = 0; r < sd.row_flags.size(); ++r) {
        if (sd.row_flags[r] == 0) continue;
        long v = static_cast<long>(r);
        if (!e.MapIndex(v) || v >= kMaxRows) continue;
        if (static_cast<size_t>(v) >= flags.size()) flags.resize(static_cast<size_t>(v) + 1, 0);
        flags[static_cast<size_t>(v)] = sd.row_flags[r];
      }
      sd.row_flags = std::move(flags);
    }
    for (size_t t = wb.tables_.size(); t-- > 0;) {
      Table& tb = wb.tables_[t];
      if (tb.area.sheet != sheet) continue;
      long lo = rows ? tb.area.r1 : tb.area.c1;
      long hi = rows ? tb.area.r2 : tb.area.c2;
      long old_lo = lo;
      if (!e.MapSpan(lo, hi, limit)) {
        wb.tables_.erase(wb.tables_.begin() + static_cast<long>(t));
        continue;
      }
      if (!rows) {
        std::vector<std::string> names;
        int width = static_cast<int>(hi - lo + 1);
        if (insert) {
          names = tb.columns;
          if (at > old_lo && at <= old_lo + static_cast<long>(tb.columns.size()) - 1) {
            for (int k = 0; k < count; ++k) {
              names.insert(names.begin() + (at - old_lo) + k,
                           "Column" + std::to_string(names.size() + 1));
            }
          }
        } else {
          for (size_t k = 0; k < tb.columns.size(); ++k) {
            long cidx = old_lo + static_cast<long>(k);
            if (cidx < at || cidx >= at + count) names.push_back(tb.columns[k]);
          }
        }
        if (static_cast<int>(names.size()) != width) {
          wb.tables_.erase(wb.tables_.begin() + static_cast<long>(t));
          continue;
        }
        tb.columns = std::move(names);
        tb.area.c1 = static_cast<int>(lo);
        tb.area.c2 = static_cast<int>(hi);
      } else {
        tb.area.r1 = static_cast<int>(lo);
        tb.area.r2 = static_cast<int>(hi);
        if (tb.area.rows() <= tb.header_rows + tb.totals_rows) {
          wb.tables_.erase(wb.tables_.begin() + static_cast<long>(t));
        }
      }
    }
    for (const Moved& m : moved) {
      FormulaCell* f = wb.FormulaAt(m.id);
      if (!f->alive) continue;
      f->row = m.row;
      f->col = m.col;
      f->base_row = m.base_row;
      f->base_col = m.base_col;
      f->prog = m.prog;
      f->cell = wb.FindCell(f->sheet, f->row, f->col);
    }
    std::unordered_map<uint64_t, Area> groups;
    for (const auto& kv : wb.arrays_) {
      Area g = kv.second;
      if (g.sheet == sheet) {
        long lo = rows ? g.r1 : g.c1;
        long hi = rows ? g.r2 : g.c2;
        if (!e.MapSpan(lo, hi, limit)) continue;
        if (rows) {
          g.r1 = static_cast<int>(lo);
          g.r2 = static_cast<int>(hi);
        } else {
          g.c1 = static_cast<int>(lo);
          g.c2 = static_cast<int>(hi);
        }
      }
      groups[CellKey(g.sheet, g.r1, g.c1)] = g;
    }
    wb.arrays_ = std::move(groups);
    for (DefinedName& dn : wb.names_) {
      if (dn.prog) dn.prog = e.Rewrite(*dn.prog, sheet, 0, 0, 0, 0);
    }
    wb.changed_.clear();
    wb.fresh_.clear();
    for (uint32_t id = 1; id < wb.next_formula_; ++id) {
      if (wb.FormulaAt(id)->alive) wb.fresh_.push_back(id);
    }
    wb.RebuildDependencies();
    return true;
  }
};

bool Workbook::InsertRows(int sheet, int at, int count, std::string* error) {
  return StructureEdit::Apply(*this, sheet, at, count, true, true, error);
}
bool Workbook::DeleteRows(int sheet, int at, int count, std::string* error) {
  return StructureEdit::Apply(*this, sheet, at, count, true, false, error);
}
bool Workbook::InsertCols(int sheet, int at, int count, std::string* error) {
  return StructureEdit::Apply(*this, sheet, at, count, false, true, error);
}
bool Workbook::DeleteCols(int sheet, int at, int count, std::string* error) {
  return StructureEdit::Apply(*this, sheet, at, count, false, false, error);
}

}
