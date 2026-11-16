#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "eval.h"
#include "workbook.h"
#include "xlsx.h"
#include "zip.h"

namespace sheet {
int RunBench(const std::vector<std::string>& args);
int RunGen(const std::vector<std::string>& args);
}

using namespace sheet;

namespace {

std::string Show(const Value& v) {
  switch (v.kind()) {
    case Kind::Blank:
      return "(blank)";
    case Kind::Number:
      return FormatGeneral(v.number());
    case Kind::Bool:
      return v.boolean() ? "TRUE" : "FALSE";
    case Kind::Text:
      return "\"" + std::string(v.text()) + "\"";
    case Kind::Error:
      return std::string(ErrorText(v.error()));
    case Kind::Array:
      return "(array)";
  }
  return "";
}

std::string Arg(const std::vector<std::string>& args, const std::string& flag,
                const std::string& def) {
  for (size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == flag) return args[i + 1];
  }
  return def;
}

bool Flag(const std::vector<std::string>& args, const std::string& flag) {
  return std::find(args.begin(), args.end(), flag) != args.end();
}

bool IsSwitch(const std::string& a) {
  return a == "--verbose" || a == "--iterate" || a == "--by-function";
}

std::vector<std::string> Positional(const std::vector<std::string>& args) {
  std::vector<std::string> out;
  for (size_t i = 0; i < args.size(); ++i) {
    if (args[i].rfind("--", 0) == 0) {
      if (!IsSwitch(args[i])) ++i;
      continue;
    }
    out.push_back(args[i]);
  }
  return out;
}

std::vector<std::string> ExpandInputs(const std::vector<std::string>& paths) {
  namespace fs = std::filesystem;
  std::vector<std::string> out;
  for (const std::string& p : paths) {
    std::error_code ec;
    if (!fs::is_directory(p, ec)) {
      out.push_back(p);
      continue;
    }
    std::vector<std::string> found;
    for (fs::recursive_directory_iterator it(p, ec), end; !ec && it != end; it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      std::string ext = ToLower(it->path().extension().string());
      if (ext == ".xlsx" || ext == ".fods") found.push_back(it->path().string());
    }
    std::sort(found.begin(), found.end());
    out.insert(out.end(), found.begin(), found.end());
  }
  return out;
}

int Threads(const std::vector<std::string>& args) {
  std::string t = Arg(args, "--threads", "1");
  if (t == "max") return static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
  return std::max(1, std::atoi(t.c_str()));
}

std::string XmlVisible(std::string_view s) {
  std::string out;
  for (char c : s) {
    unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 && c != '\t' && c != '\n' && c != '\r') continue;
    out.push_back(c);
  }
  return out;
}

bool SameResult(const Value& want, const Value& got, bool bool_as_number) {
  if (bool_as_number && want.IsNumber() && got.IsBool())
    return want.number() == (got.boolean() ? 1.0 : 0.0);
  if (want.kind() != got.kind()) {
    if (want.IsText() && want.text().empty() && got.IsBlank()) return true;
    if (want.IsText() && want.text().empty() && got.IsText()) return XmlVisible(got.text()).empty();
    return false;
  }
  switch (want.kind()) {
    case Kind::Number: {
      double a = want.number();
      double b = got.number();
      if (a == b || ApproxEqual(a, b)) return true;
      double scale = std::max({1.0, std::fabs(a), std::fabs(b)});
      if (std::fabs(a - b) <= 1e-9 * scale) return true;
      return FormatGeneral(a) == FormatGeneral(b);
    }
    case Kind::Text:
      return want.text() == got.text() || want.text() == XmlVisible(got.text());
    case Kind::Bool:
      return want.boolean() == got.boolean();
    case Kind::Error:
      return want.error() == got.error();
    default:
      return true;
  }
}

bool CallsAny(const Program& p, const std::set<int>& ids, bool unknown) {
  for (const Instr& in : p.code) {
    if (in.op != Op::Call) continue;
    if (in.a & 0x80000000u) {
      if (unknown) return true;
      continue;
    }
    if (ids.count(static_cast<int>(in.a))) return true;
  }
  return false;
}

std::string LeadFunction(const Workbook& wb, const CellPos& p) {
  std::string f = wb.FormulaText(p.sheet, p.row, p.col);
  size_t open = f.find('(');
  if (open == std::string::npos) return "(no function)";
  size_t start = open;
  while (start > 0 && (std::isalnum(static_cast<unsigned char>(f[start - 1])) ||
                       f[start - 1] == '.' || f[start - 1] == '_')) {
    --start;
  }
  return f.substr(start, open - start);
}

struct CheckTotals {
  size_t files = 0;
  size_t formulas = 0;
  size_t compared = 0;
  size_t matched = 0;
  size_t unsupported = 0;
  size_t volatile_skipped = 0;
  size_t failed = 0;
  size_t no_value = 0;
};

int CheckFile(const std::string& path, int threads, bool verbose, size_t limit, CheckTotals& tot,
              std::map<std::string, std::pair<size_t, size_t>>& by_fn) {
  Workbook wb;
  LoadReport rep;
  std::string error;
  if (!LoadWorkbook(path, wb, rep, &error)) {
    std::cerr << path << ": " << error << "\n";
    return 1;
  }
  wb.SetThreads(threads);
  RecalcStats st = wb.RecalcAll();
  static const std::set<int> volatile_ids = {FindFunction("RAND"), FindFunction("RANDBETWEEN"),
                                             FindFunction("NOW"), FindFunction("TODAY"),
                                             FindFunction("CELL")};
  std::set<uint64_t> tainted;
  std::set<uint64_t> unknown;
  std::vector<CellPos> formulas;
  for (const auto& kv : rep.cached) formulas.push_back(kv.first);
  for (const CellPos& p : formulas) {
    const Program* prog = wb.ProgramOf(p.sheet, p.row, p.col);
    if (!prog) continue;
    uint64_t key = CellKey(p.sheet, p.row, p.col);
    if (CallsAny(*prog, volatile_ids, false)) tainted.insert(key);
    if (CallsAny(*prog, {}, true)) unknown.insert(key);
  }
  for (int pass = 0; pass < 32; ++pass) {
    bool changed = false;
    for (const CellPos& p : formulas) {
      uint64_t key = CellKey(p.sheet, p.row, p.col);
      bool t = tainted.count(key) > 0;
      bool u = unknown.count(key) > 0;
      if (t && u) continue;
      for (const Area& a : wb.Precedents(p.sheet, p.row, p.col)) {
        if (static_cast<long long>(a.rows()) * a.cols() > 200000) continue;
        for (int r = a.r1; r <= std::min(a.r2, wb.MaxRow(a.sheet)); ++r) {
          for (int c = a.c1; c <= std::min(a.c2, wb.MaxCol(a.sheet)); ++c) {
            uint64_t k2 = CellKey(a.sheet, r, c);
            if (!t && tainted.count(k2)) {
              tainted.insert(key);
              t = true;
              changed = true;
            }
            if (!u && unknown.count(k2)) {
              unknown.insert(key);
              u = true;
              changed = true;
            }
          }
        }
      }
    }
    if (!changed) break;
  }
  std::string lower = ToLower(path);
  bool fods = lower.size() >= 5 && lower.compare(lower.size() - 5, 5, ".fods") == 0;
  size_t compared = 0;
  size_t matched = 0;
  size_t unsup = 0;
  size_t vol = 0;
  size_t novalue = 0;
  size_t shown = 0;
  for (const auto& kv : rep.cached) {
    const CellPos& p = kv.first;
    const Value& want = kv.second;
    uint64_t key = CellKey(p.sheet, p.row, p.col);
    if (unknown.count(key)) {
      ++unsup;
      continue;
    }
    if (tainted.count(key)) {
      ++vol;
      continue;
    }
    if (want.IsBlank()) {
      ++novalue;
      continue;
    }
    Value got = wb.Get(p.sheet, p.row, p.col);
    ++compared;
    std::string fn = LeadFunction(wb, p);
    auto& slot = by_fn[fn];
    ++slot.first;
    bool loose = wb.iteration() && wb.InCycle(p.sheet, p.row, p.col) && want.IsNumber() &&
                 got.IsNumber() && std::fabs(want.number() - got.number()) <= wb.max_change();
    if (loose || SameResult(want, got, fods)) {
      ++matched;
      continue;
    }
    ++slot.second;
    if (verbose && shown < limit) {
      ++shown;
      std::cout << "  " << wb.SheetName(p.sheet) << "!" << CellName(p.row, p.col) << "  "
                << wb.FormulaText(p.sheet, p.row, p.col) << "\n      expected " << Show(want)
                << "  got " << Show(got) << "\n";
    }
  }
  std::printf(
      "%-60s formulas %6zu  compared %6zu  matched %6zu  unsupported %5zu  failed %4zu  %.3fs\n",
      path.size() > 60 ? path.substr(path.size() - 60).c_str() : path.c_str(), rep.formulas,
      compared, matched, unsup, rep.failed, st.seconds + rep.seconds);
  if (verbose) {
    for (const std::string& f : rep.failures) std::cout << "  parse failure " << f << "\n";
  }
  ++tot.files;
  tot.formulas += rep.formulas;
  tot.compared += compared;
  tot.matched += matched;
  tot.unsupported += unsup;
  tot.volatile_skipped += vol;
  tot.failed += rep.failed;
  tot.no_value += novalue;
  return 0;
}

int Check(const std::vector<std::string>& args) {
  std::vector<std::string> files = ExpandInputs(Positional(args));
  if (files.empty()) {
    std::cerr << "usage: sheet check <file or folder>... [--verbose] [--limit N] [--threads N] "
                 "[--by-function] [--min PERCENT]\n";
    return 2;
  }
  bool verbose = Flag(args, "--verbose");
  size_t limit = static_cast<size_t>(std::atoi(Arg(args, "--limit", "40").c_str()));
  int threads = Threads(args);
  CheckTotals tot;
  std::map<std::string, std::pair<size_t, size_t>> by_fn;
  int rc = 0;
  for (const std::string& f : files) rc |= CheckFile(f, threads, verbose, limit, tot, by_fn);
  std::printf(
      "\nfiles %zu  formulas %zu  compared %zu  matched %zu (%.2f%%)  unsupported %zu  volatile "
      "%zu  "
      "no cached value %zu  parse failures %zu\n",
      tot.files, tot.formulas, tot.compared, tot.matched,
      tot.compared ? 100.0 * static_cast<double>(tot.matched) / static_cast<double>(tot.compared)
                   : 0.0,
      tot.unsupported, tot.volatile_skipped, tot.no_value, tot.failed);
  double min_pct = std::atof(Arg(args, "--min", "0").c_str());
  double pct = tot.compared
                   ? 100.0 * static_cast<double>(tot.matched) / static_cast<double>(tot.compared)
                   : 0.0;
  if (min_pct > 0) {
    rc = pct < min_pct ? 1 : 0;
    if (rc) std::printf("match rate %.2f%% is below the required %.2f%%\n", pct, min_pct);
  }
  if (Flag(args, "--by-function")) {
    std::vector<std::pair<std::string, std::pair<size_t, size_t>>> rows(by_fn.begin(), by_fn.end());
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
    for (const auto& r : rows) {
      if (r.second.second == 0) continue;
      std::printf("  %-24s compared %6zu  mismatched %5zu\n", r.first.c_str(), r.second.first,
                  r.second.second);
    }
  }
  return rc;
}

int Recalc(const std::vector<std::string>& args) {
  std::vector<std::string> files = Positional(args);
  if (files.empty()) {
    std::cerr << "usage: sheet recalc <file> [--threads N] [--out out.xlsx] [--csv out.csv]\n";
    return 2;
  }
  Workbook wb;
  LoadReport rep;
  std::string error;
  if (!LoadWorkbook(files[0], wb, rep, &error)) {
    std::cerr << error << "\n";
    return 1;
  }
  wb.SetThreads(Threads(args));
  RecalcStats st = wb.RecalcAll();
  std::printf("loaded %zu sheets, %zu cells, %zu formulas (%zu shared, %zu programs) in %.3fs\n",
              rep.sheets, wb.CellCount(), wb.FormulaCount(), rep.shared, wb.ProgramCount(),
              rep.seconds);
  std::printf(
      "recalculated %zu formulas in %.3fs on %d %s (dependency graph %.3fs, ordering %.3fs), %zu "
      "levels, %zu in cycles\n",
      st.evaluated, st.seconds, st.threads, st.threads == 1 ? "thread" : "threads",
      st.graph_seconds, st.order_seconds, st.levels, st.cycles);
  if (rep.failed)
    std::printf("%zu formulas could not be parsed and kept their cached values\n", rep.failed);
  std::string out = Arg(args, "--out", "");
  if (!out.empty()) {
    if (!SaveXlsx(wb, out, &error)) {
      std::cerr << error << "\n";
      return 1;
    }
    std::printf("wrote %s\n", out.c_str());
  }
  std::string csv = Arg(args, "--csv", "");
  if (!csv.empty()) {
    std::string text = SheetCsv(wb, 0);
    if (!WriteFile(csv, text)) {
      std::cerr << "cannot write " << csv << "\n";
      return 1;
    }
    std::printf("wrote %s\n", csv.c_str());
  }
  return 0;
}

bool ParseTarget(const Workbook& wb, int current, const std::string& text, int& sheet, Area& a) {
  std::string ref = text;
  sheet = current;
  size_t bang = ref.rfind('!');
  if (bang != std::string::npos) {
    std::string name = ref.substr(0, bang);
    if (name.size() >= 2 && name.front() == '\'' && name.back() == '\'')
      name = name.substr(1, name.size() - 2);
    sheet = wb.FindSheet(name);
    if (sheet < 0) return false;
    ref = ref.substr(bang + 1);
  }
  if (!ParseAreaName(ref, a.r1, a.c1, a.r2, a.c2)) return false;
  a.sheet = sheet;
  return true;
}

void ShellHelp() {
  std::cout << "commands:\n"
               "  A1 = 42            set a value (text, number, TRUE, a date like 3/14/2026)\n"
               "  A1 = =SUM(B1:B9)   set a formula\n"
               "  A1                 show value and formula\n"
               "  show A1:D10        print a range\n"
               "  deps A1            list the ranges a formula reads\n"
               "  clear A1           clear a cell\n"
               "  insert rows 3 2    insert 2 rows before row 3 (also: insert cols C 1)\n"
               "  delete rows 3 2    delete rows 3..4 (also: delete cols C 1)\n"
               "  name Rate =B1      define a name\n"
               "  sheet Name         switch to a sheet (created if missing)\n"
               "  threads N          set recalc threads\n"
               "  iterate on|off     iterative calculation for circular references\n"
               "  recalc             full recalculation\n"
               "  load file / save file.xlsx\n"
               "  stats, help, quit\n";
}

int Shell(const std::vector<std::string>& args) {
  Workbook wb;
  std::vector<std::string> files = Positional(args);
  int sheet = 0;
  if (!files.empty()) {
    LoadReport rep;
    std::string error;
    if (!LoadWorkbook(files[0], wb, rep, &error)) {
      std::cerr << error << "\n";
      return 1;
    }
    wb.RecalcAll();
    std::cout << "loaded " << files[0] << ": " << wb.SheetCount() << " sheets, "
              << wb.FormulaCount() << " formulas\n";
  }
  if (wb.SheetCount() == 0) wb.AddSheet("Sheet1");
  std::string line;
  std::cout << "sheet shell, type help for commands\n";
  while (true) {
    std::cout << wb.SheetName(sheet) << "> " << std::flush;
    if (!std::getline(std::cin, line)) break;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    size_t lead = line.find_first_not_of(' ');
    if (lead == std::string::npos) continue;
    line = line.substr(lead);
    std::string cmd = line.substr(0, line.find(' '));
    std::string rest = line.size() > cmd.size() ? line.substr(cmd.size() + 1) : "";
    std::string lc = ToLower(cmd);
    std::string error;
    if (lc == "quit" || lc == "exit") break;
    if (lc == "help") {
      ShellHelp();
      continue;
    }
    if (lc == "stats") {
      std::cout << "cells " << wb.CellCount() << ", formulas " << wb.FormulaCount() << ", programs "
                << wb.ProgramCount() << ", dependency edges " << wb.DependencyEdges()
                << ", range nodes " << wb.RangeNodes() << "\n";
      continue;
    }
    if (lc == "recalc") {
      RecalcStats st = wb.RecalcAll();
      std::cout << st.evaluated << " formulas in " << st.seconds * 1000 << " ms\n";
      continue;
    }
    if (lc == "threads") {
      wb.SetThreads(std::atoi(rest.c_str()));
      std::cout << "threads " << wb.threads() << "\n";
      continue;
    }
    if (lc == "iterate") {
      wb.SetIteration(ToLower(rest) == "on");
      RecalcStats st = wb.Recalc();
      std::cout << "iteration " << (ToLower(rest) == "on" ? "on" : "off") << ", " << st.evaluated
                << (st.evaluated == 1 ? " formula" : " formulas") << " recalculated\n";
      continue;
    }
    if (lc == "sheet") {
      int s = wb.FindSheet(rest);
      if (s < 0) s = wb.AddSheet(rest);
      if (s >= 0) sheet = s;
      continue;
    }
    if (lc == "load") {
      Workbook fresh;
      LoadReport rep;
      if (!LoadWorkbook(rest, fresh, rep, &error)) {
        std::cout << error << "\n";
        continue;
      }
      std::cout << "use: sheet shell " << rest << "\n";
      continue;
    }
    if (lc == "save") {
      if (!SaveXlsx(wb, rest, &error)) {
        std::cout << error << "\n";
      } else {
        std::cout << "saved " << rest << "\n";
      }
      continue;
    }
    if (lc == "name") {
      size_t sp = rest.find(' ');
      if (sp == std::string::npos) {
        std::cout << "usage: name Name =formula\n";
        continue;
      }
      if (!wb.DefineName(rest.substr(0, sp), rest.substr(sp + 1), -1, &error)) {
        std::cout << "error: " << error << "\n";
        continue;
      }
      wb.Recalc();
      continue;
    }
    if (lc == "insert" || lc == "delete") {
      char kind[16] = {0};
      char where[32] = {0};
      int count = 1;
      if (std::sscanf(rest.c_str(), "%15s %31s %d", kind, where, &count) < 2) {
        std::cout << "usage: insert rows 3 2\n";
        continue;
      }
      bool rows = ToLower(kind).rfind("row", 0) == 0;
      int at;
      if (rows) {
        at = std::atoi(where) - 1;
      } else if (!ParseColumn(where, at)) {
        std::cout << "bad column\n";
        continue;
      }
      bool ok = lc == "insert" ? (rows ? wb.InsertRows(sheet, at, count, &error)
                                       : wb.InsertCols(sheet, at, count, &error))
                               : (rows ? wb.DeleteRows(sheet, at, count, &error)
                                       : wb.DeleteCols(sheet, at, count, &error));
      if (!ok) {
        std::cout << "error: " << error << "\n";
        continue;
      }
      RecalcStats st = wb.Recalc();
      std::cout << "ok, " << st.evaluated << (st.evaluated == 1 ? " formula" : " formulas")
                << " recalculated\n";
      continue;
    }
    if (lc == "show") {
      int s;
      Area a;
      if (!ParseTarget(wb, sheet, rest, s, a)) {
        std::cout << "bad range\n";
        continue;
      }
      for (int r = a.r1; r <= a.r2 && r < a.r1 + 200; ++r) {
        std::cout << r + 1 << "\t";
        for (int c = a.c1; c <= a.c2 && c < a.c1 + 26; ++c) {
          Value v = wb.Get(s, r, c);
          std::string t = v.IsBlank() ? "" : Show(v);
          if (t.size() > 14) t = t.substr(0, 13) + "~";
          std::printf("%-15s", t.c_str());
        }
        std::cout << "\n";
      }
      continue;
    }
    if (lc == "deps") {
      int s;
      Area a;
      if (!ParseTarget(wb, sheet, rest, s, a)) {
        std::cout << "bad cell\n";
        continue;
      }
      for (const Area& p : wb.Precedents(s, a.r1, a.c1)) {
        std::cout << "  " << wb.SheetName(p.sheet) << "!" << CellName(p.r1, p.c1);
        if (p.r1 != p.r2 || p.c1 != p.c2) std::cout << ":" << CellName(p.r2, p.c2);
        std::cout << "\n";
      }
      continue;
    }
    if (lc == "clear") {
      int s;
      Area a;
      if (!ParseTarget(wb, sheet, rest, s, a)) {
        std::cout << "bad range\n";
        continue;
      }
      for (int r = a.r1; r <= a.r2; ++r) {
        for (int c = a.c1; c <= a.c2; ++c) wb.Clear(s, r, c);
      }
      wb.Recalc();
      continue;
    }
    size_t eq = line.find('=');
    std::string target = line.substr(0, eq == std::string::npos ? line.size() : eq);
    while (!target.empty() && target.back() == ' ') target.pop_back();
    int s;
    Area a;
    if (!ParseTarget(wb, sheet, target, s, a)) {
      std::cout << "unknown command, type help\n";
      continue;
    }
    if (eq == std::string::npos) {
      Value v = wb.Get(s, a.r1, a.c1);
      std::cout << Show(v);
      if (wb.HasFormula(s, a.r1, a.c1)) std::cout << "    " << wb.FormulaText(s, a.r1, a.c1);
      if (wb.InCycle(s, a.r1, a.c1)) std::cout << "    (circular reference)";
      std::cout << "\n";
      continue;
    }
    std::string input = line.substr(eq + 1);
    size_t first = input.find_first_not_of(' ');
    input = first == std::string::npos ? "" : input.substr(first);
    if (!wb.SetInput(s, a.r1, a.c1, input, &error)) {
      std::cout << "error: " << error << "\n";
      continue;
    }
    auto t0 = std::chrono::steady_clock::now();
    RecalcStats st = wb.Recalc();
    double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::cout << Show(wb.Get(s, a.r1, a.c1)) << "    (" << st.evaluated << " recalculated in " << ms
              << " ms)\n";
  }
  return 0;
}

void Usage() {
  std::cout << "usage: sheet <command> [options]\n"
               "  shell [file]                  interactive spreadsheet shell\n"
               "  recalc <file> [--threads N] [--out out.xlsx] [--csv out.csv]\n"
               "  check <files or folders>... [--verbose] [--by-function] [--min PERCENT]\n"
               "  bench [--rows N] [--threads N,N,...] [--repeat N] [--edits N] [--model NAME]\n"
               "  gen <out.xlsx> [--rows N] [--model NAME]\n";
}

}

int main(int argc, char** argv) {
  if (argc < 2) {
    Usage();
    return 2;
  }
  std::string cmd = argv[1];
  std::vector<std::string> args(argv + 2, argv + argc);
  if (cmd == "check") return Check(args);
  if (cmd == "recalc") return Recalc(args);
  if (cmd == "shell") return Shell(args);
  if (cmd == "bench") return RunBench(args);
  if (cmd == "gen") return RunGen(args);
  Usage();
  return 2;
}
