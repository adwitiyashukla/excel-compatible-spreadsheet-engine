#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core.h"
#include "format.h"
#include "formula.h"
#include "workbook.h"

namespace sheet {

struct PendingCell {
  uint32_t formula;
};

struct Operand {
  enum class Tag : uint8_t { Val, Ref, Refs, Missing };
  Tag tag = Tag::Val;
  Value value;
  Area area;
  std::shared_ptr<const std::vector<Area>> list;

  static Operand Of(Value v) {
    Operand o;
    o.value = std::move(v);
    return o;
  }
  static Operand OfRef(const Area& a) {
    Operand o;
    o.tag = Tag::Ref;
    o.area = a;
    return o;
  }
  static Operand OfMissing() {
    Operand o;
    o.tag = Tag::Missing;
    return o;
  }
  bool IsRef() const { return tag == Tag::Ref || tag == Tag::Refs; }
  bool IsMissing() const { return tag == Tag::Missing; }
  bool IsError() const { return tag == Tag::Val && value.IsError(); }
  bool IsArray() const { return tag == Tag::Val && value.IsArray(); }
};

class EvalContext {
  const Workbook& wb_;

 public:
  EvalContext(const Workbook& wb, int sheet, int row, int col)
      : wb_(wb), sheet(sheet), row(row), col(col) {}

  const Workbook& wb() const { return wb_; }

  const Value& Read(int s, int r, int c) const {
    const Cell* cell = wb_.FindCell(s, r, c);
    if (!cell) return blank_;
    if (cell->formula != 0) Check(*cell);
    return cell->value;
  }

  bool IsFormula(int s, int r, int c) const {
    const Cell* cell = wb_.FindCell(s, r, c);
    return cell && cell->formula != 0;
  }

  template <typename F>
  void Scan(const Area& a, F&& fn) const {
    if (a.sheet < 0 || a.sheet >= wb_.SheetCount()) return;
    const SheetData& sd = wb_.sheets_[a.sheet];
    int c2 = std::min(a.c2, static_cast<int>(sd.cols.size()) - 1);
    for (int c = a.c1; c <= c2; ++c) {
      const Column& column = sd.cols[c];
      int k1 = a.r1 >> kChunkShift;
      int k2 = std::min(a.r2 >> kChunkShift, static_cast<int>(column.chunks.size()) - 1);
      for (int k = k1; k <= k2; ++k) {
        const Chunk* ch = column.chunks[k].get();
        if (!ch || ch->used == 0) continue;
        int i1 = k == k1 ? (a.r1 & (kChunkRows - 1)) : 0;
        int i2 = k == (a.r2 >> kChunkShift) ? (a.r2 & (kChunkRows - 1)) : kChunkRows - 1;
        for (int i = i1; i <= i2; ++i) {
          const Cell& cell = ch->cells[i];
          if (cell.formula != 0) Check(cell);
          if (cell.value.IsBlank()) continue;
          fn((k << kChunkShift) + i, c, cell.value);
        }
      }
    }
  }

  template <typename F>
  void ScanOrdered(const Area& a, F&& fn) const {
    if (a.sheet < 0 || a.sheet >= wb_.SheetCount()) return;
    int r2 = std::min(a.r2, UsedRows(a.sheet) - 1);
    int c2 = std::min(a.c2, UsedCols(a.sheet) - 1);
    for (int r = a.r1; r <= r2; ++r) {
      for (int c = a.c1; c <= c2; ++c) {
        const Value& v = Read(a.sheet, r, c);
        if (!v.IsBlank()) fn(r, c, v);
      }
    }
  }

  int UsedRows(int s) const { return wb_.sheets_[s].max_row + 1; }
  int UsedCols(int s) const { return wb_.sheets_[s].max_col + 1; }

  Value Intersect(const Area& a) const;
  Value Materialize(const Area& a) const;
  Value Deref(const Operand& op, bool array_mode) const;
  Value Scalar(const Operand& op) const;

  const DateSystem& dates() const { return wb_.dates_; }
  double Now() const { return wb_.Now(); }
  double Random(uint64_t salt) const;
  Operand EvalName(std::string_view name) const;
  bool ParseRef(std::string_view text, bool a1, Area& out) const;
  bool CellHasFormula(int s, int r, int c) const { return IsFormula(s, r, c); }
  const Program* ProgramAt(int s, int r, int c) const {
    const Cell* cell = wb_.FindCell(s, r, c);
    if (!cell || cell->formula == 0) return nullptr;
    return wb_.FormulaAt(cell->formula)->prog.get();
  }
  std::string FormulaTextAt(int s, int r, int c) const { return wb_.FormulaText(s, r, c); }

  int sheet;
  int row;
  int col;
  mutable int depth = 0;
  mutable uint64_t random_calls = 0;
  mutable bool array_call = false;
  int array_rows = 1;
  int array_cols = 1;

 private:
  static void Check(const Cell& cell) {
    uint8_t st = cell.state.load(std::memory_order_acquire);
    if (st == kClean || st == kIterating) return;
    throw PendingCell{cell.formula};
  }

  static inline const Value blank_{};
};

using FuncImpl = Operand (*)(EvalContext& ctx, Operand* args, int argc);

enum FuncFlag : uint32_t {
  kFnVolatile = 1,
  kFnDynamic = 2,
  kFnXlfn = 4,
  kFnLift = 8,
  kFnRefResult = 16,
};

struct FuncInfo {
  const char* name;
  int min_args;
  int max_args;
  const char* params;
  int repeat;
  uint32_t flags;
  FuncImpl impl;
};

int FindFunction(std::string_view name);
const FuncInfo& FunctionAt(int id);
char ParamClass(const FuncInfo& f, int index);
int FunctionCount();

void RegisterMath(std::vector<FuncInfo>& out);
void RegisterStats(std::vector<FuncInfo>& out);
void RegisterText(std::vector<FuncInfo>& out);
void RegisterLookup(std::vector<FuncInfo>& out);
void RegisterDate(std::vector<FuncInfo>& out);
void RegisterInfo(std::vector<FuncInfo>& out);
void RegisterFinance(std::vector<FuncInfo>& out);

Operand Execute(const Program& p, EvalContext& ctx);
Value EvaluateFormula(const Program& p, EvalContext& ctx, bool array_mode);

Err ToNumber(const Value& v, const EvalContext& ctx, double& out);
std::string ToText(const Value& v);
Err ToBool(const Value& v, bool& out);
int CompareValues(const Value& a, const Value& b);

inline Operand MakeNum(double d) {
  if (!std::isfinite(d)) return Operand::Of(Value::Error(Err::Num));
  return Operand::Of(Value::Number(d));
}
inline Operand MakeErr(Err e) {
  return Operand::Of(Value::Error(e));
}
inline Operand MakeText(std::string_view s) {
  return Operand::Of(Value::Text(s));
}
inline Operand MakeBool(bool b) {
  return Operand::Of(Value::Bool(b));
}

Err ArgNumber(EvalContext& ctx, const Operand& a, double& out);
Err ArgNumberOr(EvalContext& ctx, Operand* args, int argc, int i, double def, double& out);
Err ArgInt(EvalContext& ctx, const Operand& a, long long& out);
Err ArgText(EvalContext& ctx, const Operand& a, std::string& out);
Err ArgBool(EvalContext& ctx, const Operand& a, bool& out);
Err ArgBoolOr(EvalContext& ctx, Operand* args, int argc, int i, bool def, bool& out);

struct Grid {
  int rows = 0;
  int cols = 0;
  const EvalContext* ctx = nullptr;
  Area area;
  Value array;
  Value scalar;
  int kind = 0;
  const Value& At(int r, int c) const {
    if (kind == 0) return ctx->Read(area.sheet, area.r1 + r, area.c1 + c);
    if (kind == 1) return array.at(static_cast<uint32_t>(r), static_cast<uint32_t>(c));
    return scalar;
  }
};

bool MakeGrid(EvalContext& ctx, const Operand& op, Grid& g, bool clamp = true);

struct Criteria {
  enum class Kind : uint8_t { Number, Text, Bool, Error, Empty, NonEmpty, Blank };
  enum class Cmp : uint8_t { Eq, Ne, Lt, Le, Gt, Ge };
  Kind kind = Kind::Blank;
  Cmp cmp = Cmp::Eq;
  double num = 0;
  bool flag = false;
  Err err = Err::None;
  std::string text;
  bool wildcard = false;
};

Criteria ParseCriteria(const EvalContext& ctx, const Value& crit);
bool MatchCriteria(const Criteria& c, const Value& v, const EvalContext& ctx);
bool WildcardMatch(std::string_view pattern, std::string_view text);
bool HasWildcards(std::string_view s);
bool AreaOf(const Operand& op, Area& out);

enum class CollectMode { Numbers, NumbersA, All };
Err CollectNumbers(EvalContext& ctx, Operand* args, int argc, std::vector<double>& out,
                   CollectMode mode = CollectMode::Numbers, bool skip_errors = false);

}
