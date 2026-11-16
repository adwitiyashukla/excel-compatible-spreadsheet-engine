#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core.h"
#include "depgraph.h"
#include "format.h"
#include "formula.h"

namespace sheet {

constexpr int kChunkShift = 8;
constexpr int kChunkRows = 1 << kChunkShift;

enum CellState : uint8_t { kClean = 0, kDirty = 1, kComputing = 2, kIterating = 3 };

struct Cell {
  Value value;
  uint32_t formula = 0;
  std::atomic<uint8_t> state{kClean};
};

struct Chunk {
  Cell cells[kChunkRows];
  uint16_t used = 0;
  uint16_t formulas = 0;
};

struct Column {
  std::vector<std::unique_ptr<Chunk>> chunks;
};

constexpr uint8_t kRowHidden = 1;
constexpr uint8_t kRowFiltered = 2;

struct SheetData {
  std::string name;
  std::vector<Column> cols;
  std::vector<uint8_t> row_flags;
  int max_row = -1;
  int max_col = -1;
};

enum FormulaFlag : uint8_t {
  kFlagVolatile = 1,
  kFlagDynamic = 2,
  kFlagArray = 4,
  kFlagCycle = 8,
};

struct FormulaCell {
  int32_t sheet = 0;
  int32_t row = 0;
  int32_t col = 0;
  int32_t base_row = 0;
  int32_t base_col = 0;
  ProgramRef prog;
  uint8_t flags = 0;
  bool alive = false;
  uint32_t level = 0;
  uint32_t visit = 0;
  uint32_t index = 0;
  uint32_t low = 0;
  uint32_t comp = 0;
  Cell* cell = nullptr;
};

struct CalcComponent {
  uint32_t start = 0;
  uint32_t size = 0;
  uint32_t level = 0;
  bool cyclic = false;
};

struct CalcChain {
  bool valid = false;
  uint64_t version = 0;
  std::vector<uint32_t> members;
  std::vector<CalcComponent> comps;
  std::vector<uint32_t> owner;
  std::vector<uint32_t> level_items;
  std::vector<uint32_t> level_start;
  size_t plain = 0;
};

struct RecalcStats {
  size_t evaluated = 0;
  size_t levels = 0;
  size_t cycles = 0;
  size_t retries = 0;
  size_t deferred = 0;
  int threads = 1;
  bool chain_reused = false;
  double graph_seconds = 0;
  double order_seconds = 0;
  double seconds = 0;
};

struct DefinedName {
  std::string name;
  int scope = -1;
  ProgramRef prog;
};

struct Table {
  std::string name;
  Area area;
  int header_rows = 1;
  int totals_rows = 0;
  bool filter = false;
  std::vector<std::string> columns;
};

class ThreadPool;
class EvalContext;

class Workbook {
 public:
  Workbook();
  ~Workbook();
  Workbook(const Workbook&) = delete;
  Workbook& operator=(const Workbook&) = delete;

  int AddSheet(std::string_view name);
  int SheetCount() const { return static_cast<int>(sheets_.size()); }
  const std::string& SheetName(int i) const { return sheets_[i].name; }
  int FindSheet(std::string_view name) const;
  bool RenameSheet(int i, std::string_view name);

  bool SetValue(int sheet, int row, int col, const Value& v);
  bool SetNumber(int sheet, int row, int col, double v) {
    return SetValue(sheet, row, col, Value::Number(v));
  }
  bool SetText(int sheet, int row, int col, std::string_view s) {
    return SetValue(sheet, row, col, Value::Text(s));
  }
  bool SetBool(int sheet, int row, int col, bool b) {
    return SetValue(sheet, row, col, Value::Bool(b));
  }
  bool SetFormula(int sheet, int row, int col, std::string_view text, std::string* error = nullptr);
  bool SetProgram(int sheet, int row, int col, ProgramRef prog);
  bool SetArrayFormula(int sheet, const Area& area, std::string_view text,
                       std::string* error = nullptr);
  bool SetInput(int sheet, int row, int col, std::string_view input, std::string* error = nullptr);
  void Clear(int sheet, int row, int col);
  void SetRowHidden(int sheet, int row, bool hidden, bool filtered = false);
  uint8_t RowFlags(int sheet, int row) const {
    const std::vector<uint8_t>& f = sheets_[sheet].row_flags;
    return row >= 0 && static_cast<size_t>(row) < f.size() ? f[static_cast<size_t>(row)] : 0;
  }

  Value Get(int sheet, int row, int col) const;
  bool HasFormula(int sheet, int row, int col) const;
  std::string FormulaText(int sheet, int row, int col, bool xlsx = false) const;
  bool ArrayGroup(int sheet, int row, int col, Area& out) const;
  bool InCycle(int sheet, int row, int col) const;

  RecalcStats Recalc();
  RecalcStats RecalcAll();

  void SetThreads(int n);
  int threads() const { return threads_; }
  void SetIteration(bool on, int max_iterations = 100, double max_change = 0.001);
  bool iteration() const { return iterate_; }
  double max_change() const { return max_change_; }
  void SetDate1904(bool on) { dates_.date1904 = on; }
  const DateSystem& dates() const { return dates_; }
  void SetNow(double serial);
  double Now() const;
  void SetSeed(uint64_t seed) { seed_ = seed; }

  bool DefineName(std::string_view name, std::string_view formula, int scope = -1,
                  std::string* error = nullptr);
  const DefinedName* FindName(std::string_view name, int sheet) const;
  const std::vector<DefinedName>& names() const { return names_; }

  bool AddTable(const Table& t, std::string* error = nullptr);
  const Table* FindTable(std::string_view name) const;
  const Table* TableAt(int sheet, int row, int col) const;
  const std::vector<Table>& tables() const { return tables_; }

  bool InsertRows(int sheet, int at, int count, std::string* error = nullptr);
  bool DeleteRows(int sheet, int at, int count, std::string* error = nullptr);
  bool InsertCols(int sheet, int at, int count, std::string* error = nullptr);
  bool DeleteCols(int sheet, int at, int count, std::string* error = nullptr);

  int MaxRow(int sheet) const { return sheets_[sheet].max_row; }
  int MaxCol(int sheet) const { return sheets_[sheet].max_col; }
  size_t FormulaCount() const { return formula_count_; }
  size_t CellCount() const;
  size_t ProgramCount() const;
  size_t DependencyEdges() const { return deps_.cell_edges() + deps_.area_edges(); }
  size_t RangeNodes() const { return deps_.live_rects(); }
  void SuspendDependencies() {
    deps_valid_ = false;
    ++graph_version_;
  }
  void SetCachedValue(int sheet, int row, int col, const Value& v) {
    Cell* c = FindCell(sheet, row, col);
    if (c) c->value = v;
  }

  template <typename F>
  void ForEachCell(int sheet, F&& fn) const {
    const SheetData& sd = sheets_[sheet];
    for (size_t c = 0; c < sd.cols.size(); ++c) {
      const Column& col = sd.cols[c];
      for (size_t k = 0; k < col.chunks.size(); ++k) {
        const Chunk* ch = col.chunks[k].get();
        if (!ch || ch->used == 0) continue;
        for (int i = 0; i < kChunkRows; ++i) {
          const Cell& cell = ch->cells[i];
          if (cell.value.IsBlank() && cell.formula == 0) continue;
          fn(static_cast<int>(k) * kChunkRows + i, static_cast<int>(c), cell);
        }
      }
    }
  }

  const FormulaCell& formula(uint32_t id) const { return *FormulaAt(id); }
  std::vector<Area> Precedents(int sheet, int row, int col) const;
  const Program* ProgramOf(int sheet, int row, int col) const;
  SheetResolver Resolver() const;
  TableResolver Tables(int sheet) const;
  SheetNamer Namer() const;
  ProgramRef Intern(ProgramRef p);

 private:
  friend class EvalContext;
  friend struct RecalcRun;
  friend struct StructureEdit;

  Cell* FindCell(int sheet, int row, int col) const {
    if (sheet < 0 || sheet >= static_cast<int>(sheets_.size())) return nullptr;
    const SheetData& sd = sheets_[static_cast<size_t>(sheet)];
    if (col < 0 || col >= static_cast<int>(sd.cols.size()) || row < 0) return nullptr;
    const Column& c = sd.cols[static_cast<size_t>(col)];
    size_t k = static_cast<size_t>(row) >> kChunkShift;
    if (k >= c.chunks.size() || !c.chunks[k]) return nullptr;
    return &c.chunks[k]->cells[row & (kChunkRows - 1)];
  }
  Cell& CellAt(int sheet, int row, int col);
  void ReleaseCell(int sheet, int row, int col);
  FormulaCell* FormulaAt(uint32_t id) const {
    return &blocks_[id >> kBlockShift][id & (kBlockSize - 1)];
  }
  uint32_t NewFormula();
  void FreeFormula(uint32_t id);
  void DropFormula(Cell& cell);
  void Register(uint32_t id);
  void Unregister(uint32_t id);
  void CollectPrecedents(const FormulaCell& f, std::vector<Area>& out) const;
  void CollectNameAreas(const Program& p, int sheet, int row, int col, int depth,
                        std::vector<Area>& out) const;
  void MarkChanged(int sheet, int row, int col);
  void RebuildDependencies();
  void Touch(int sheet, int row, int col);
  bool ArrayBlocked(int sheet, int row, int col) const;
  bool PartOfArray(int sheet, int row, int col) const;
  RecalcStats Run(bool full);

  static constexpr int kBlockShift = 12;
  static constexpr uint32_t kBlockSize = 1u << kBlockShift;

  std::vector<SheetData> sheets_;
  std::vector<std::unique_ptr<FormulaCell[]>> blocks_;
  uint32_t next_formula_ = 1;
  std::vector<uint32_t> free_formulas_;
  size_t formula_count_ = 0;
  DepGraph deps_;
  std::vector<CellPos> changed_;
  std::vector<uint32_t> fresh_;
  std::vector<uint32_t> volatiles_;
  std::vector<DefinedName> names_;
  std::vector<Table> tables_;
  std::unordered_map<uint64_t, std::vector<ProgramRef>> programs_;
  std::unordered_map<uint64_t, Area> arrays_;
  std::unique_ptr<ThreadPool> pool_;
  int threads_ = 1;
  bool iterate_ = false;
  int max_iterations_ = 100;
  double max_change_ = 0.001;
  DateSystem dates_;
  double now_ = std::numeric_limits<double>::quiet_NaN();
  uint64_t seed_ = 0x5DEECE66DULL;
  uint64_t recalc_serial_ = 0;
  uint32_t epoch_ = 0;
  uint32_t visit_epoch_ = 0;
  uint64_t graph_version_ = 0;
  CalcChain chain_;
  bool deps_valid_ = true;
};

}
