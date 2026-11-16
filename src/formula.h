#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core.h"

namespace sheet {

struct RefSpec {
  int32_t r1 = 0;
  int32_t c1 = 0;
  int32_t r2 = 0;
  int32_t c2 = 0;
  int16_t sheet = -1;
  int16_t sheet2 = -1;
  bool r1_abs = false;
  bool c1_abs = false;
  bool r2_abs = false;
  bool c2_abs = false;
  bool full_cols = false;
  bool full_rows = false;
  bool area = false;
  bool explicit_sheet = false;
  bool address_only = false;
};

enum class Op : uint8_t {
  PushNum,
  PushStr,
  PushBool,
  PushErr,
  PushMissing,
  PushArray,
  PushRef,
  PushRefErr,
  PushName,
  Neg,
  Plus,
  Percent,
  Add,
  Sub,
  Mul,
  Div,
  Pow,
  Concat,
  Eq,
  Ne,
  Lt,
  Le,
  Gt,
  Ge,
  Range,
  Union,
  Intersect,
  Paren,
  Call,
  If,
  Else,
  EndIf,
  IfErr,
  EndIfErr,
};

struct Instr {
  Op op;
  uint8_t mode;
  uint16_t b;
  uint32_t a;
};

struct Program {
  std::vector<Instr> code;
  std::vector<double> nums;
  std::vector<Value> consts;
  std::vector<RefSpec> refs;
  std::vector<std::string> names;
  bool is_volatile = false;
  bool dynamic = false;
  bool uses_names = false;
  bool array_formula = false;
  uint64_t hash = 0;
  int max_stack = 0;
};

using ProgramRef = std::shared_ptr<const Program>;

struct ParseError {
  std::string message;
  size_t pos = 0;
};

using SheetResolver = std::function<int(std::string_view)>;
using SheetNamer = std::function<std::string(int)>;

struct TableRef {
  int sheet = -1;
  int r1 = 0;
  int c1 = 0;
  int r2 = 0;
  int c2 = 0;
  int header_rows = 1;
  int totals_rows = 0;
  std::vector<std::string> columns;
};

using TableResolver = std::function<bool(std::string_view name, int row, int col, TableRef& out)>;

ProgramRef CompileFormula(std::string_view text, int base_row, int base_col,
                          const SheetResolver& sheets, bool array_mode, ParseError* err,
                          const TableResolver* tables = nullptr);

std::string PrintFormula(const Program& p, int base_row, int base_col, const SheetNamer& sheets,
                         bool xlsx_prefixes = false);

uint64_t HashProgram(const Program& p);
bool SameProgram(const Program& a, const Program& b);

std::string QuoteSheetName(std::string_view name);

inline bool ResolveRef(const RefSpec& s, int base_row, int base_col, Area& out) {
  long r1 = s.r1_abs ? s.r1 : static_cast<long>(base_row) + s.r1;
  long r2 = s.r2_abs ? s.r2 : static_cast<long>(base_row) + s.r2;
  long c1 = s.c1_abs ? s.c1 : static_cast<long>(base_col) + s.c1;
  long c2 = s.c2_abs ? s.c2 : static_cast<long>(base_col) + s.c2;
  if (s.full_cols) {
    r1 = 0;
    r2 = kMaxRows - 1;
  }
  if (s.full_rows) {
    c1 = 0;
    c2 = kMaxCols - 1;
  }
  if (r1 < 0 || r2 < 0 || c1 < 0 || c2 < 0 || r1 >= kMaxRows || r2 >= kMaxRows || c1 >= kMaxCols ||
      c2 >= kMaxCols) {
    return false;
  }
  if (r1 > r2) std::swap(r1, r2);
  if (c1 > c2) std::swap(c1, c2);
  out.r1 = static_cast<int>(r1);
  out.r2 = static_cast<int>(r2);
  out.c1 = static_cast<int>(c1);
  out.c2 = static_cast<int>(c2);
  return true;
}

void MarkAddressRefs(Program& p);

std::string TranslateOpenFormula(std::string_view text, bool& ok);

}
