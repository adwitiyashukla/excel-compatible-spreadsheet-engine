#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "eval.h"
#include "workbook.h"
#include "xlsx.h"

namespace sheet {

namespace {

struct Rng {
  uint64_t s;
  uint64_t Next() {
    s += 0x9E3779B97F4A7C15ULL;
    return Mix64(s);
  }
  double Uniform() { return static_cast<double>(Next() >> 11) * (1.0 / 9007199254740992.0); }
  int Below(int n) { return static_cast<int>(Next() % static_cast<uint64_t>(n)); }
};

std::string Get(const std::vector<std::string>& args, const std::string& flag,
                const std::string& def) {
  for (size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == flag) return args[i + 1];
  }
  return def;
}

double Seconds(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

std::string R(int row) {
  return std::to_string(row + 1);
}

struct Model {
  std::string name;
  int rows = 0;
  std::vector<CellPos> inputs;
};

void BuildFinance(Workbook& wb, int s, int n, Rng& rng, Model& m) {
  const char* cats[] = {"rent",  "payroll", "travel", "software", "hardware", "legal",  "ads",
                        "food",  "fuel",    "taxes",  "rent2",    "repairs",  "energy", "water",
                        "phone", "cloud",   "office", "bank",     "freight",  "misc"};
  wb.SetText(s, 0, 10, "category");
  wb.SetText(s, 0, 11, "factor");
  for (int i = 0; i < 20; ++i) {
    wb.SetText(s, i + 1, 10, cats[i]);
    wb.SetNumber(s, i + 1, 11, 0.5 + i * 0.05);
  }
  const char* heads[] = {"id",   "category", "amount", "rate",   "gross",
                         "gain", "tax",      "net",    "factor", "score"};
  for (int c = 0; c < 10; ++c) wb.SetText(s, 0, c, heads[c]);
  std::string err;
  for (int r = 1; r <= n; ++r) {
    wb.SetNumber(s, r, 0, r);
    wb.SetText(s, r, 1, cats[rng.Below(20)]);
    wb.SetNumber(s, r, 2, std::floor(rng.Uniform() * 100000) / 100);
    wb.SetNumber(s, r, 3, std::floor(rng.Uniform() * 1000) / 10000);
    std::string row = R(r);
    wb.SetFormula(s, r, 4, "=C" + row + "*(1+D" + row + ")", &err);
    wb.SetFormula(s, r, 5, "=E" + row + "-C" + row, &err);
    wb.SetFormula(s, r, 6, "=IF(F" + row + ">50,F" + row + "*0.3,F" + row + "*0.1)", &err);
    wb.SetFormula(s, r, 7, "=ROUND(E" + row + "-G" + row + ",2)", &err);
    wb.SetFormula(s, r, 8, "=VLOOKUP(B" + row + ",$K$2:$L$21,2,FALSE)", &err);
    wb.SetFormula(s, r, 9, "=H" + row + "*I" + row, &err);
    m.inputs.push_back(CellPos{s, r, 2});
  }
  std::string last = R(n);
  wb.SetText(s, 0, 13, "total");
  wb.SetFormula(s, 1, 13, "=SUM(J2:J" + last + ")", &err);
  wb.SetFormula(s, 2, 13, "=AVERAGE(H2:H" + last + ")", &err);
  wb.SetFormula(s, 3, 13, "=COUNTIF(B2:B" + last + ",\"rent\")", &err);
  wb.SetFormula(s, 4, 13, "=MAX(F2:F" + last + ")", &err);
  wb.SetFormula(s, 5, 13, "=SUMIF(B2:B" + last + ",\"cloud\",J2:J" + last + ")", &err);
}

void BuildChain(Workbook& wb, int s, int n, Rng& rng, Model& m) {
  std::string err;
  for (int r = 0; r < n; ++r) {
    wb.SetNumber(s, r, 0, std::floor(rng.Uniform() * 1000));
    if (r == 0) {
      wb.SetFormula(s, r, 1, "=A1", &err);
    } else {
      wb.SetFormula(s, r, 1, "=B" + R(r - 1) + "+A" + R(r), &err);
    }
    m.inputs.push_back(CellPos{s, r, 0});
  }
}

void BuildRunning(Workbook& wb, int s, int n, Rng& rng, Model& m) {
  std::string err;
  for (int r = 0; r < n; ++r) {
    wb.SetNumber(s, r, 0, std::floor(rng.Uniform() * 1000));
    wb.SetFormula(s, r, 1, "=SUM($A$1:A" + R(r) + ")", &err);
    m.inputs.push_back(CellPos{s, r, 0});
  }
}

void BuildLookup(Workbook& wb, int s, int n, Rng& rng, Model& m) {
  std::string err;
  for (int r = 0; r < n; ++r) {
    wb.SetNumber(s, r, 0, r * 7 + 3);
    wb.SetNumber(s, r, 1, std::floor(rng.Uniform() * 1000));
  }
  std::string last = R(n - 1);
  for (int r = 0; r < n; ++r) {
    wb.SetNumber(s, r, 3, (rng.Below(n)) * 7 + 3);
    wb.SetFormula(s, r, 4, "=XLOOKUP(D" + R(r) + ",$A$1:$A$" + last + ",$B$1:$B$" + last + ")",
                  &err);
    wb.SetFormula(s, r, 5, "=MATCH(D" + R(r) + ",$A$1:$A$" + last + ",1)", &err);
    m.inputs.push_back(CellPos{s, r, 1});
  }
}

bool Build(const std::string& model, Workbook& wb, int n, uint64_t seed, Model& m) {
  Rng rng{seed};
  int s = wb.AddSheet("Sheet1");
  m.name = model;
  m.rows = n;
  if (model == "finance") {
    BuildFinance(wb, s, n, rng, m);
  } else if (model == "chain") {
    BuildChain(wb, s, n, rng, m);
  } else if (model == "running") {
    BuildRunning(wb, s, n, rng, m);
  } else if (model == "lookup") {
    BuildLookup(wb, s, n, rng, m);
  } else {
    return false;
  }
  return true;
}

int DefaultRows(const std::string& model) {
  if (model == "running") return 20000;
  if (model == "lookup") return 20000;
  if (model == "chain") return 200000;
  return 100000;
}

double Percentile(std::vector<double> xs, double p) {
  if (xs.empty()) return 0;
  std::sort(xs.begin(), xs.end());
  size_t i = static_cast<size_t>(p * static_cast<double>(xs.size() - 1));
  return xs[i];
}

RecalcStats Timed(Workbook& wb, int repeat) {
  std::vector<double> times;
  RecalcStats st;
  for (int i = 0; i < repeat; ++i) {
    st = wb.RecalcAll();
    times.push_back(st.seconds);
  }
  st.seconds = Percentile(times, 0.5);
  return st;
}

std::vector<int> ThreadList(const std::string& arg) {
  int hw = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
  std::vector<int> out;
  size_t pos = 0;
  while (true) {
    size_t comma = arg.find(',', pos);
    std::string part =
        arg.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    int t = part == "max" ? hw : std::atoi(part.c_str());
    if (t >= 1) out.push_back(t);
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  if (out.empty()) out.push_back(hw);
  return out;
}

}

int RunBench(const std::vector<std::string>& args) {
  std::string model = Get(args, "--model", "finance");
  int rows = std::atoi(Get(args, "--rows", std::to_string(DefaultRows(model))).c_str());
  int edits = std::atoi(Get(args, "--edits", "200").c_str());
  int full_edits = std::atoi(Get(args, "--full-edits", "20").c_str());
  int repeat = std::max(1, std::atoi(Get(args, "--repeat", "5").c_str()));
  std::vector<int> threads = ThreadList(Get(args, "--threads", "max"));
  uint64_t seed = std::strtoull(Get(args, "--seed", "42").c_str(), nullptr, 10);
  Workbook wb;
  Model m;
  auto t0 = std::chrono::steady_clock::now();
  if (!Build(model, wb, rows, seed, m)) {
    std::cerr << "unknown model " << model << " (finance, chain, running, lookup)\n";
    return 2;
  }
  double build = Seconds(t0);
  std::printf("model %s: %d rows, %zu cells, %zu formulas, %zu distinct programs\n", model.c_str(),
              rows, wb.CellCount(), wb.FormulaCount(), wb.ProgramCount());
  std::printf("build %.3f s (parse and compile every formula)\n", build);
  wb.SetThreads(1);
  RecalcStats first = wb.RecalcAll();
  std::printf(
      "first recalc %.3f s (ordering %.3f s), dependency edges %zu, range nodes %zu, levels %zu\n",
      first.seconds, first.order_seconds, wb.DependencyEdges(), wb.RangeNodes(), first.levels);
  std::vector<Value> reference;
  int s = 0;
  int maxr = wb.MaxRow(s);
  int maxc = wb.MaxCol(s);
  RecalcStats one = Timed(wb, repeat);
  for (int c = 0; c <= maxc; ++c) {
    for (int r = 0; r <= maxr; ++r) reference.push_back(wb.Get(s, r, c));
  }
  std::printf("full recalc  1 thread  %8.3f s  %10.0f formulas/s  (median of %d)\n", one.seconds,
              static_cast<double>(one.evaluated) / std::max(one.seconds, 1e-9), repeat);
  for (int t : threads) {
    if (t <= 1) continue;
    wb.SetThreads(t);
    wb.RecalcAll();
    RecalcStats par = Timed(wb, repeat);
    size_t diff = 0;
    size_t k = 0;
    for (int c = 0; c <= maxc; ++c) {
      for (int r = 0; r <= maxr; ++r) {
        if (!wb.Get(s, r, c).SameAs(reference[k++])) ++diff;
      }
    }
    std::printf(
        "full recalc %2d threads %8.3f s  %10.0f formulas/s  speedup %.2fx  mismatches %zu\n", t,
        par.seconds, static_cast<double>(par.evaluated) / std::max(par.seconds, 1e-9),
        one.seconds / std::max(par.seconds, 1e-9), diff);
  }
  wb.SetThreads(1);
  Rng rng{seed ^ 0xABCDEF};
  auto edit = [&]() {
    const CellPos& p = m.inputs[static_cast<size_t>(rng.Below(static_cast<int>(m.inputs.size())))];
    wb.SetNumber(p.sheet, p.row, p.col, std::floor(rng.Uniform() * 100000) / 100);
  };
  std::vector<double> lat;
  std::vector<double> full;
  double dirty = 0;
  int every = full_edits > 0 ? std::max(1, edits / full_edits) : 0;
  for (int e = 0; e < edits && !m.inputs.empty(); ++e) {
    edit();
    auto t1 = std::chrono::steady_clock::now();
    RecalcStats st = wb.Recalc();
    lat.push_back(Seconds(t1) * 1e6);
    dirty += static_cast<double>(st.evaluated);
    if (every > 0 && e % every == every - 1 && static_cast<int>(full.size()) < full_edits) {
      edit();
      auto t2 = std::chrono::steady_clock::now();
      wb.RecalcAll();
      full.push_back(Seconds(t2) * 1e6);
    }
  }
  if (!lat.empty()) {
    std::printf(
        "edit + incremental recalc (1 thread), %zu edits: p50 %.1f us, p99 %.1f us, %.1f "
        "formulas per edit\n",
        lat.size(), Percentile(lat, 0.5), Percentile(lat, 0.99),
        dirty / static_cast<double>(lat.size()));
  }
  if (!full.empty()) {
    std::printf("edit + full recalc (1 thread), %zu edits: p50 %.1f us, p99 %.1f us", full.size(),
                Percentile(full, 0.5), Percentile(full, 0.99));
    if (!lat.empty()) {
      std::printf(", incremental speedup %.1fx", Percentile(full, 0.5) / Percentile(lat, 0.5));
    }
    std::printf("\n");
  }
  return 0;
}

int RunGen(const std::vector<std::string>& args) {
  std::string out;
  for (size_t i = 0; i < args.size(); ++i) {
    if (args[i].rfind("--", 0) == 0) {
      ++i;
      continue;
    }
    out = args[i];
  }
  if (out.empty()) {
    std::cerr << "usage: sheet gen <out.xlsx> [--rows N] [--model finance|chain|running|lookup]\n";
    return 2;
  }
  std::string model = Get(args, "--model", "finance");
  int rows = std::atoi(Get(args, "--rows", std::to_string(DefaultRows(model))).c_str());
  Workbook wb;
  Model m;
  if (!Build(model, wb, rows, 42, m)) {
    std::cerr << "unknown model " << model << "\n";
    return 2;
  }
  wb.RecalcAll();
  std::string error;
  auto t0 = std::chrono::steady_clock::now();
  if (!SaveXlsx(wb, out, &error)) {
    std::cerr << error << "\n";
    return 1;
  }
  std::printf("wrote %s: %zu cells, %zu formulas in %.3f s\n", out.c_str(), wb.CellCount(),
              wb.FormulaCount(), Seconds(t0));
  return 0;
}

}
