#include "workbook.h"

#include <chrono>
#include <cmath>
#include <ctime>
#include <string>

#include "eval.h"
#include "threadpool.h"

namespace sheet {

namespace {

bool ValidName(std::string_view name) {
  if (name.empty() || name.size() > 255) return false;
  char c0 = name[0];
  bool start_ok = (c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || c0 == '_' || c0 == '\\' ||
                  static_cast<unsigned char>(c0) >= 0x80;
  if (!start_ok) return false;
  for (char c : name) {
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '.' || c == '\\' || c == '?' ||
              static_cast<unsigned char>(c) >= 0x80;
    if (!ok) return false;
  }
  int r;
  int c;
  if (ParseCellName(name, r, c)) return false;
  if (EqualsText(name, "TRUE") || EqualsText(name, "FALSE")) return false;
  return true;
}

bool IsEmptyCell(const Cell& c) {
  return c.formula == 0 && c.value.IsBlank();
}

}

Workbook::Workbook() {
  blocks_.emplace_back(new FormulaCell[kBlockSize]);
  dates_.current_year = 2026;
  std::time_t t = std::time(nullptr);
  std::tm tmv{};
#if defined(_WIN32)
  localtime_s(&tmv, &t);
#else
  localtime_r(&t, &tmv);
#endif
  dates_.current_year = tmv.tm_year + 1900;
}

Workbook::~Workbook() = default;

int Workbook::AddSheet(std::string_view name) {
  std::string n(name);
  if (n.empty()) n = "Sheet" + std::to_string(sheets_.size() + 1);
  if (FindSheet(n) >= 0) return -1;
  SheetData sd;
  sd.name = n;
  sheets_.push_back(std::move(sd));
  return static_cast<int>(sheets_.size() - 1);
}

int Workbook::FindSheet(std::string_view name) const {
  for (size_t i = 0; i < sheets_.size(); ++i) {
    if (EqualsText(sheets_[i].name, name)) return static_cast<int>(i);
  }
  return -1;
}

bool Workbook::RenameSheet(int i, std::string_view name) {
  if (i < 0 || i >= SheetCount() || name.empty()) return false;
  int other = FindSheet(name);
  if (other >= 0 && other != i) return false;
  sheets_[i].name = std::string(name);
  return true;
}

Cell& Workbook::CellAt(int sheet, int row, int col) {
  SheetData& sd = sheets_[sheet];
  if (col >= static_cast<int>(sd.cols.size())) sd.cols.resize(static_cast<size_t>(col) + 1);
  Column& c = sd.cols[col];
  size_t k = static_cast<size_t>(row) >> kChunkShift;
  if (k >= c.chunks.size()) c.chunks.resize(k + 1);
  if (!c.chunks[k]) c.chunks[k] = std::make_unique<Chunk>();
  Touch(sheet, row, col);
  return c.chunks[k]->cells[row & (kChunkRows - 1)];
}

void Workbook::Touch(int sheet, int row, int col) {
  SheetData& sd = sheets_[sheet];
  if (row > sd.max_row) sd.max_row = row;
  if (col > sd.max_col) sd.max_col = col;
}

uint32_t Workbook::NewFormula() {
  uint32_t id;
  if (!free_formulas_.empty()) {
    id = free_formulas_.back();
    free_formulas_.pop_back();
  } else {
    id = next_formula_++;
    if ((id >> kBlockShift) >= blocks_.size()) blocks_.emplace_back(new FormulaCell[kBlockSize]);
  }
  FormulaCell* f = FormulaAt(id);
  f->alive = true;
  f->flags = 0;
  f->level = 0;
  f->visit = 0;
  ++formula_count_;
  ++graph_version_;
  return id;
}

void Workbook::FreeFormula(uint32_t id) {
  FormulaCell* f = FormulaAt(id);
  f->alive = false;
  f->prog.reset();
  if (f->cell) f->cell->state.store(kClean, std::memory_order_relaxed);
  f->cell = nullptr;
  free_formulas_.push_back(id);
  --formula_count_;
  ++graph_version_;
}

void Workbook::DropFormula(Cell& cell) {
  uint32_t id = cell.formula;
  if (id == 0) return;
  FormulaCell* f = FormulaAt(id);
  SheetData& sd = sheets_[f->sheet];
  Chunk* ch = sd.cols[f->col].chunks[static_cast<size_t>(f->row) >> kChunkShift].get();
  if (deps_valid_) Unregister(id);
  FreeFormula(id);
  cell.formula = 0;
  --ch->formulas;
}

void Workbook::CollectNameAreas(const Program& p, int sheet, int row, int col, int depth,
                                std::vector<Area>& out) const {
  for (size_t i = 0; i < p.refs.size(); ++i) {
    const RefSpec& s = p.refs[i];
    if (s.address_only) continue;
    Area a;
    if (!ResolveRef(s, row, col, a)) continue;
    a.sheet = s.sheet >= 0 ? s.sheet : sheet;
    if (s.sheet2 > s.sheet) {
      for (int sh = s.sheet; sh <= s.sheet2; ++sh) {
        Area x = a;
        x.sheet = sh;
        out.push_back(x);
      }
      continue;
    }
    out.push_back(a);
  }
  if (!p.uses_names || depth > 16) return;
  for (const Instr& in : p.code) {
    if (in.op != Op::PushName) continue;
    const std::string& key = p.names[in.a];
    int scope = sheet;
    std::string_view name = key;
    size_t bang = key.find('!');
    if (bang != std::string::npos) {
      scope = std::atoi(key.substr(0, bang).c_str());
      name = std::string_view(key).substr(bang + 1);
    }
    const DefinedName* dn = FindName(name, scope);
    if (dn && dn->prog) CollectNameAreas(*dn->prog, sheet, row, col, depth + 1, out);
  }
}

void Workbook::CollectPrecedents(const FormulaCell& f, std::vector<Area>& out) const {
  if (!f.prog) return;
  CollectNameAreas(*f.prog, f.sheet, f.base_row, f.base_col, 0, out);
}

void Workbook::Register(uint32_t id) {
  ++graph_version_;
  FormulaCell* f = FormulaAt(id);
  std::vector<Area> areas;
  CollectPrecedents(*f, areas);
  for (const Area& a : areas) {
    if (a.r1 == a.r2 && a.c1 == a.c2) {
      deps_.AddCell(CellKey(a.sheet, a.r1, a.c1), id);
    } else {
      deps_.AddArea(a, id);
    }
  }
}

void Workbook::Unregister(uint32_t id) {
  ++graph_version_;
  FormulaCell* f = FormulaAt(id);
  std::vector<Area> areas;
  CollectPrecedents(*f, areas);
  for (const Area& a : areas) {
    if (a.r1 == a.r2 && a.c1 == a.c2) {
      deps_.RemoveCell(CellKey(a.sheet, a.r1, a.c1), id);
    } else {
      deps_.RemoveArea(a, id);
    }
  }
}

void Workbook::RebuildDependencies() {
  ++graph_version_;
  deps_.Clear();
  for (uint32_t id = 1; id < next_formula_; ++id) {
    FormulaCell* f = FormulaAt(id);
    if (f->alive) Register(id);
  }
  deps_valid_ = true;
}

void Workbook::MarkChanged(int sheet, int row, int col) {
  changed_.push_back(CellPos{sheet, row, col});
}

bool Workbook::ArrayBlocked(int sheet, int row, int col) const {
  const Cell* c = FindCell(sheet, row, col);
  if (!c || c->formula == 0) return false;
  return (FormulaAt(c->formula)->flags & kFlagArray) != 0;
}

void Workbook::ReleaseCell(int sheet, int row, int col) {
  Cell* c = FindCell(sheet, row, col);
  if (!c) return;
  if (c->formula != 0 && (FormulaAt(c->formula)->flags & kFlagArray)) {
    FormulaCell* f = FormulaAt(c->formula);
    auto it = arrays_.find(CellKey(sheet, f->base_row, f->base_col));
    if (it != arrays_.end()) {
      Area g = it->second;
      arrays_.erase(it);
      for (int r = g.r1; r <= g.r2; ++r) {
        for (int k = g.c1; k <= g.c2; ++k) {
          Cell* m = FindCell(sheet, r, k);
          if (!m) continue;
          bool before = !IsEmptyCell(*m);
          DropFormula(*m);
          m->value = Value();
          if (before) --sheets_[sheet].cols[k].chunks[static_cast<size_t>(r) >> kChunkShift]->used;
          MarkChanged(sheet, r, k);
        }
      }
      return;
    }
  }
  bool before = !IsEmptyCell(*c);
  DropFormula(*c);
  c->value = Value();
  if (before) --sheets_[sheet].cols[col].chunks[static_cast<size_t>(row) >> kChunkShift]->used;
}

bool Workbook::PartOfArray(int sheet, int row, int col) const {
  if (!ArrayBlocked(sheet, row, col)) return false;
  Area g;
  return ArrayGroup(sheet, row, col, g) && (g.rows() > 1 || g.cols() > 1);
}

bool Workbook::SetValue(int sheet, int row, int col, const Value& v) {
  if (sheet < 0 || sheet >= SheetCount() || row < 0 || row >= kMaxRows || col < 0 ||
      col >= kMaxCols) {
    return false;
  }
  if (PartOfArray(sheet, row, col)) return false;
  ReleaseCell(sheet, row, col);
  if (!v.IsBlank()) {
    Cell& c = CellAt(sheet, row, col);
    c.value = v.IsArray() ? v.at(0, 0) : v;
    ++sheets_[sheet].cols[col].chunks[static_cast<size_t>(row) >> kChunkShift]->used;
  }
  MarkChanged(sheet, row, col);
  return true;
}

void Workbook::Clear(int sheet, int row, int col) {
  if (sheet < 0 || sheet >= SheetCount() || row < 0 || row >= kMaxRows || col < 0 ||
      col >= kMaxCols)
    return;
  if (FindCell(sheet, row, col) == nullptr) return;
  ReleaseCell(sheet, row, col);
  MarkChanged(sheet, row, col);
}

ProgramRef Workbook::Intern(ProgramRef p) {
  std::vector<ProgramRef>& bucket = programs_[p->hash];
  for (const ProgramRef& q : bucket) {
    if (SameProgram(*q, *p)) return q;
  }
  bucket.push_back(p);
  return p;
}

bool Workbook::SetProgram(int sheet, int row, int col, ProgramRef prog) {
  if (!prog || sheet < 0 || sheet >= SheetCount() || row < 0 || row >= kMaxRows || col < 0 ||
      col >= kMaxCols || PartOfArray(sheet, row, col)) {
    return false;
  }
  prog = Intern(prog);
  ReleaseCell(sheet, row, col);
  Cell& c = CellAt(sheet, row, col);
  uint32_t id = NewFormula();
  FormulaCell* f = FormulaAt(id);
  f->sheet = sheet;
  f->row = row;
  f->col = col;
  f->base_row = row;
  f->base_col = col;
  f->prog = prog;
  f->cell = &c;
  c.state.store(kClean, std::memory_order_relaxed);
  if (prog->is_volatile) {
    f->flags |= kFlagVolatile;
    volatiles_.push_back(id);
  }
  if (prog->dynamic) f->flags |= kFlagDynamic;
  c.formula = id;
  c.value = Value();
  Chunk* ch = sheets_[sheet].cols[col].chunks[static_cast<size_t>(row) >> kChunkShift].get();
  ++ch->formulas;
  ++ch->used;
  if (deps_valid_) Register(id);
  fresh_.push_back(id);
  MarkChanged(sheet, row, col);
  return true;
}

bool Workbook::SetFormula(int sheet, int row, int col, std::string_view text, std::string* error) {
  if (sheet >= 0 && sheet < SheetCount() && PartOfArray(sheet, row, col)) {
    if (error) *error = "cannot change part of an array formula";
    return false;
  }
  ParseError pe;
  TableResolver tables = Tables(sheet);
  ProgramRef p = CompileFormula(text, row, col, Resolver(), false, &pe, &tables);
  if (!p) {
    if (error) *error = pe.message;
    return false;
  }
  return SetProgram(sheet, row, col, p);
}

bool Workbook::SetArrayFormula(int sheet, const Area& area, std::string_view text,
                               std::string* error) {
  if (sheet < 0 || sheet >= SheetCount()) return false;
  ParseError pe;
  TableResolver tables = Tables(sheet);
  ProgramRef p = CompileFormula(text, area.r1, area.c1, Resolver(), true, &pe, &tables);
  if (!p) {
    if (error) *error = pe.message;
    return false;
  }
  p = Intern(p);
  for (int r = area.r1; r <= area.r2; ++r) {
    for (int c = area.c1; c <= area.c2; ++c) ReleaseCell(sheet, r, c);
  }
  for (int r = area.r1; r <= area.r2; ++r) {
    for (int c = area.c1; c <= area.c2; ++c) {
      Cell& cell = CellAt(sheet, r, c);
      uint32_t id = NewFormula();
      FormulaCell* f = FormulaAt(id);
      f->sheet = sheet;
      f->row = r;
      f->col = c;
      f->base_row = area.r1;
      f->base_col = area.c1;
      f->prog = p;
      f->cell = &cell;
      cell.state.store(kClean, std::memory_order_relaxed);
      f->flags = kFlagArray;
      if (p->is_volatile) {
        f->flags |= kFlagVolatile;
        volatiles_.push_back(id);
      }
      if (p->dynamic) f->flags |= kFlagDynamic;
      cell.formula = id;
      cell.value = Value();
      Chunk* ch = sheets_[sheet].cols[c].chunks[static_cast<size_t>(r) >> kChunkShift].get();
      ++ch->formulas;
      ++ch->used;
      if (deps_valid_) Register(id);
      fresh_.push_back(id);
      MarkChanged(sheet, r, c);
    }
  }
  Area g = area;
  g.sheet = sheet;
  arrays_[CellKey(sheet, area.r1, area.c1)] = g;
  return true;
}

bool Workbook::SetInput(int sheet, int row, int col, std::string_view input, std::string* error) {
  if (input.size() > 1 && input[0] == '=') return SetFormula(sheet, row, col, input, error);
  if (input.empty()) {
    Clear(sheet, row, col);
    return true;
  }
  Value v;
  Err e;
  double d;
  if (input[0] == '\'') {
    v = Value::Text(input.substr(1));
  } else if (EqualsText(input, "TRUE") || EqualsText(input, "FALSE")) {
    v = Value::Bool(EqualsText(input, "TRUE"));
  } else if (ParseErrorText(input, e)) {
    v = Value::Error(e);
  } else if (ParseNumberText(input, dates_, d)) {
    v = Value::Number(d);
  } else {
    v = Value::Text(input);
  }
  if (SetValue(sheet, row, col, v)) return true;
  if (error)
    *error = PartOfArray(sheet, row, col) ? "cannot change part of an array formula" : "bad cell";
  return false;
}

Value Workbook::Get(int sheet, int row, int col) const {
  const Cell* c = FindCell(sheet, row, col);
  return c ? c->value : Value();
}

bool Workbook::HasFormula(int sheet, int row, int col) const {
  const Cell* c = FindCell(sheet, row, col);
  return c && c->formula != 0;
}

std::string Workbook::FormulaText(int sheet, int row, int col, bool xlsx) const {
  const Cell* c = FindCell(sheet, row, col);
  if (!c || c->formula == 0) return "";
  const FormulaCell* f = FormulaAt(c->formula);
  return "=" + PrintFormula(*f->prog, f->base_row, f->base_col, Namer(), xlsx);
}

std::vector<Area> Workbook::Precedents(int sheet, int row, int col) const {
  std::vector<Area> out;
  const Cell* c = FindCell(sheet, row, col);
  if (c && c->formula != 0) CollectPrecedents(*FormulaAt(c->formula), out);
  return out;
}

const Program* Workbook::ProgramOf(int sheet, int row, int col) const {
  const Cell* c = FindCell(sheet, row, col);
  if (!c || c->formula == 0) return nullptr;
  return FormulaAt(c->formula)->prog.get();
}

bool Workbook::ArrayGroup(int sheet, int row, int col, Area& out) const {
  const Cell* c = FindCell(sheet, row, col);
  if (!c || c->formula == 0) return false;
  const FormulaCell* f = FormulaAt(c->formula);
  if (!(f->flags & kFlagArray)) return false;
  auto it = arrays_.find(CellKey(sheet, f->base_row, f->base_col));
  if (it == arrays_.end()) return false;
  out = it->second;
  return true;
}

bool Workbook::InCycle(int sheet, int row, int col) const {
  const Cell* c = FindCell(sheet, row, col);
  if (!c || c->formula == 0) return false;
  return (FormulaAt(c->formula)->flags & kFlagCycle) != 0;
}

void Workbook::SetThreads(int n) {
  if (n < 1) n = 1;
  threads_ = n;
  if (n > 1) {
    if (!pool_ || pool_->size() != n) pool_ = std::make_unique<ThreadPool>(n);
  } else {
    pool_.reset();
  }
}

void Workbook::SetIteration(bool on, int max_iterations, double max_change) {
  iterate_ = on;
  max_iterations_ = max_iterations < 1 ? 1 : max_iterations;
  max_change_ = max_change;
  for (uint32_t id = 1; id < next_formula_; ++id) {
    FormulaCell* f = FormulaAt(id);
    if (f->alive && (f->flags & kFlagCycle)) fresh_.push_back(id);
  }
}

void Workbook::SetNow(double serial) {
  now_ = serial;
}

double Workbook::Now() const {
  if (!std::isnan(now_)) return now_;
  using namespace std::chrono;
  std::time_t t = system_clock::to_time_t(system_clock::now());
  std::tm tmv{};
#if defined(_WIN32)
  localtime_s(&tmv, &t);
#else
  localtime_r(&t, &tmv);
#endif
  double serial;
  if (!SerialFromDate(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, dates_, serial)) return 0;
  return serial + (tmv.tm_hour * 3600.0 + tmv.tm_min * 60.0 + tmv.tm_sec) / 86400.0;
}

bool Workbook::DefineName(std::string_view name, std::string_view formula, int scope,
                          std::string* error) {
  if (!ValidName(name)) {
    if (error) *error = "invalid name";
    return false;
  }
  ParseError pe;
  TableResolver tables = Tables(scope >= 0 ? scope : 0);
  ProgramRef p = CompileFormula(formula, 0, 0, Resolver(), false, &pe, &tables);
  if (!p) {
    if (error) *error = pe.message;
    return false;
  }
  bool found = false;
  for (DefinedName& dn : names_) {
    if (dn.scope == scope && EqualsText(dn.name, name)) {
      dn.prog = p;
      found = true;
    }
  }
  if (!found) names_.push_back(DefinedName{std::string(name), scope, p});
  deps_valid_ = false;
  ++graph_version_;
  for (uint32_t id = 1; id < next_formula_; ++id) {
    FormulaCell* f = FormulaAt(id);
    if (f->alive && f->prog->uses_names) fresh_.push_back(id);
  }
  return true;
}

const DefinedName* Workbook::FindName(std::string_view name, int sheet) const {
  const DefinedName* global = nullptr;
  for (const DefinedName& dn : names_) {
    if (!EqualsText(dn.name, name)) continue;
    if (dn.scope == sheet && sheet >= 0) return &dn;
    if (dn.scope < 0) global = &dn;
  }
  return global;
}

size_t Workbook::CellCount() const {
  size_t n = 0;
  for (const SheetData& sd : sheets_) {
    for (const Column& c : sd.cols) {
      for (const auto& ch : c.chunks) {
        if (ch) n += ch->used;
      }
    }
  }
  return n;
}

size_t Workbook::ProgramCount() const {
  size_t n = 0;
  for (const auto& kv : programs_) n += kv.second.size();
  return n;
}

SheetResolver Workbook::Resolver() const {
  return [this](std::string_view name) { return FindSheet(name); };
}

void Workbook::SetRowHidden(int sheet, int row, bool hidden, bool filtered) {
  if (sheet < 0 || sheet >= SheetCount() || row < 0 || row >= kMaxRows) return;
  std::vector<uint8_t>& f = sheets_[sheet].row_flags;
  uint8_t flags = hidden ? static_cast<uint8_t>(kRowHidden | (filtered ? kRowFiltered : 0)) : 0;
  if (static_cast<size_t>(row) >= f.size()) {
    if (flags == 0) return;
    f.resize(static_cast<size_t>(row) + 1, 0);
  }
  if (f[static_cast<size_t>(row)] == flags) return;
  f[static_cast<size_t>(row)] = flags;
  static const int sub = FindFunction("SUBTOTAL");
  static const int agg = FindFunction("AGGREGATE");
  for (uint32_t id = 1; id < next_formula_; ++id) {
    FormulaCell* fc = FormulaAt(id);
    if (!fc->alive) continue;
    for (const Instr& in : fc->prog->code) {
      if (in.op == Op::Call && !(in.a & 0x80000000u) &&
          (static_cast<int>(in.a) == sub || static_cast<int>(in.a) == agg)) {
        fresh_.push_back(id);
        break;
      }
    }
  }
}

bool Workbook::AddTable(const Table& t, std::string* error) {
  if (!ValidName(t.name) || FindTable(t.name)) {
    if (error) *error = "invalid or duplicate table name";
    return false;
  }
  if (t.area.sheet < 0 || t.area.sheet >= SheetCount() ||
      static_cast<int>(t.columns.size()) != t.area.cols() ||
      t.area.rows() <= t.header_rows + t.totals_rows) {
    if (error) *error = "bad table range";
    return false;
  }
  tables_.push_back(t);
  return true;
}

const Table* Workbook::FindTable(std::string_view name) const {
  for (const Table& t : tables_) {
    if (EqualsText(t.name, name)) return &t;
  }
  return nullptr;
}

const Table* Workbook::TableAt(int sheet, int row, int col) const {
  for (const Table& t : tables_) {
    if (t.area.sheet == sheet && t.area.Contains(row, col)) return &t;
  }
  return nullptr;
}

TableResolver Workbook::Tables(int sheet) const {
  if (tables_.empty()) return nullptr;
  return [this, sheet](std::string_view name, int row, int col, TableRef& out) {
    const Table* t = name.empty() ? TableAt(sheet, row, col) : FindTable(name);
    if (!t) return false;
    out.sheet = t->area.sheet;
    out.r1 = t->area.r1;
    out.c1 = t->area.c1;
    out.r2 = t->area.r2;
    out.c2 = t->area.c2;
    out.header_rows = t->header_rows;
    out.totals_rows = t->totals_rows;
    out.columns = t->columns;
    return true;
  };
}

SheetNamer Workbook::Namer() const {
  return
      [this](int i) { return i >= 0 && i < SheetCount() ? sheets_[i].name : std::string("#REF"); };
}

}
