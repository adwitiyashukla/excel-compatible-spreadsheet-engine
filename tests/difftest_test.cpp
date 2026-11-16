#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "test.h"
#include "util.h"
#include "workbook.h"
#include "xlsx.h"

using namespace sheet;

namespace {

constexpr int kRows = 30;
constexpr int kCols = 8;

struct Rng {
  uint64_t s;
  int rows = kRows;
  uint64_t Next() {
    s += 0x9E3779B97F4A7C15ULL;
    return Mix64(s);
  }
  int Below(int n) { return static_cast<int>(Next() % static_cast<uint64_t>(n)); }
  bool Chance(int percent) { return Below(100) < percent; }
};

std::string RandomCell(Rng& rng, bool allow_sheet) {
  std::string out;
  if (allow_sheet && rng.Chance(15)) out = rng.Chance(50) ? "S1!" : "S2!";
  bool col_abs = rng.Chance(25);
  bool row_abs = rng.Chance(25);
  out += (col_abs ? "$" : "") + ColumnName(rng.Below(kCols)) + (row_abs ? "$" : "") +
         std::to_string(rng.Below(rng.rows) + 1);
  return out;
}

std::string Range(Rng& rng) {
  int r1 = rng.Below(rng.rows);
  int c1 = rng.Below(kCols);
  int r2 = std::min(rng.rows - 1, r1 + rng.Below(6));
  int c2 = std::min(kCols - 1, c1 + rng.Below(3));
  std::string sheet = rng.Chance(15) ? (rng.Chance(50) ? "S1!" : "S2!") : "";
  return sheet + CellName(r1, c1) + ":" + CellName(r2, c2);
}

std::string Formula(Rng& rng) {
  auto R = [&]() { return RandomCell(rng, true); };
  auto G = [&]() { return Range(rng); };
  switch (rng.Below(24)) {
    case 0:
      return "=" + R() + "+" + R();
    case 1:
      return "=" + R() + "*2-" + R();
    case 2:
      return "=SUM(" + G() + ")";
    case 3:
      return "=AVERAGE(" + G() + ")";
    case 4:
      return "=MAX(" + G() + ")-MIN(" + G() + ")";
    case 5: {
      std::string r = R();
      return "=IF(" + r + ">0," + r + ",-" + R() + ")";
    }
    case 6:
      return "=COUNTIF(" + G() + ",\">0\")";
    case 7:
      return "=IFERROR(" + R() + "/" + R() + ",0)";
    case 8:
      return "=ROUND(" + R() + "/3,2)";
    case 9:
      return "=VLOOKUP(" + R() + "," + G() + ",1,FALSE)";
    case 10:
      return "=INDEX(" + G() + ",1,1)";
    case 11:
      return "=MATCH(" + R() + "," + G() + ",0)";
    case 12: {
      std::string g = G();
      return "=SUMPRODUCT(" + g + "," + g + ")";
    }
    case 13:
      return "=CONCAT(" + R() + ",\"x\")";
    case 14:
      return "=LEN(" + R() + "&\"\")";
    case 15:
      return "=OFFSET(" + RandomCell(rng, false) + "," + std::to_string(rng.Below(3)) + "," +
             std::to_string(rng.Below(3)) + ")";
    case 16:
      return "=INDIRECT(\"B\"&(MOD(ABS(N(" + R() + ")),20)+1))";
    case 17:
      return "=SUM(OFFSET(" + RandomCell(rng, false) + ",0,0,2,2))";
    case 18:
      return "=IF(ISNUMBER(" + R() + ")," + R() + ",0)+N(" + R() + ")";
    case 19:
      return "=AND(" + R() + ">1," + R() + "<10)";
    case 20:
      return "=COUNTA(" + G() + ")";
    case 21:
      return "=SUMIF(" + G() + ",\"<5\")";
    case 22:
      return "=" + R() + "&" + R();
    default:
      return "=XLOOKUP(" + R() + "," + G() + "," + G() + ",\"none\")";
  }
}

std::string Constant(Rng& rng) {
  int k = rng.Below(20);
  if (k < 12) return std::to_string(rng.Below(101) - 50);
  if (k < 14) return std::to_string(rng.Below(1000) - 500) + "." + std::to_string(rng.Below(100));
  const char* words[] = {"a", "b", "apple", "'10", "TRUE", "FALSE"};
  return words[rng.Below(6)];
}

void Populate(Workbook& wb, Rng& rng) {
  wb.AddSheet("S1");
  wb.AddSheet("S2");
  for (int s = 0; s < 2; ++s) {
    for (int r = 0; r < rng.rows; ++r) {
      for (int c = 0; c < kCols; ++c) {
        int k = rng.Below(100);
        if (k < 10) continue;
        std::string err;
        wb.SetInput(s, r, c, k < 60 ? Constant(rng) : Formula(rng), &err);
      }
    }
  }
}

void CopyInto(const Workbook& from, Workbook& to) {
  for (int s = 0; s < from.SheetCount(); ++s) to.AddSheet(from.SheetName(s));
  for (int s = 0; s < from.SheetCount(); ++s) {
    from.ForEachCell(s, [&](int r, int c, const sheet::Cell& cell) {
      if (cell.formula != 0) {
        Area g;
        if (from.ArrayGroup(s, r, c, g)) {
          if (g.r1 == r && g.c1 == c) to.SetArrayFormula(s, g, from.FormulaText(s, r, c));
          return;
        }
        to.SetFormula(s, r, c, from.FormulaText(s, r, c));
      } else {
        to.SetValue(s, r, c, cell.value);
      }
    });
  }
}

int Compare(const Workbook& a, const Workbook& b, const char* what, uint64_t seed, int step) {
  int bad = 0;
  for (int s = 0; s < a.SheetCount(); ++s) {
    int rows = std::max(a.MaxRow(s), b.MaxRow(s));
    int cols = std::max(a.MaxCol(s), b.MaxCol(s));
    for (int r = 0; r <= rows; ++r) {
      for (int c = 0; c <= cols; ++c) {
        Value x = a.Get(s, r, c);
        Value y = b.Get(s, r, c);
        if (x.SameAs(y)) continue;
        if (++bad <= 3) {
          test::Fail(__FILE__, __LINE__,
                     std::string(what) + " seed " + std::to_string(seed) + " step " +
                         std::to_string(step) + " " + a.SheetName(s) + "!" + CellName(r, c) + " " +
                         a.FormulaText(s, r, c) + "  " + test::Display(x) + " vs " +
                         test::Display(y));
        }
      }
    }
  }
  return bad;
}

void RandomEdit(Workbook& wb, Rng& rng) {
  int s = rng.Below(2);
  int r = rng.Below(rng.rows);
  int c = rng.Below(kCols);
  int k = rng.Below(100);
  std::string err;
  if (k < 45) {
    wb.SetInput(s, r, c, Constant(rng), &err);
  } else if (k < 70) {
    wb.SetInput(s, r, c, Formula(rng), &err);
  } else if (k < 80) {
    wb.Clear(s, r, c);
  } else if (k < 85) {
    wb.InsertRows(s, r, 1 + rng.Below(2), &err);
  } else if (k < 90) {
    wb.DeleteRows(s, r, 1 + rng.Below(2), &err);
  } else if (k < 95) {
    wb.InsertCols(s, c, 1, &err);
  } else {
    wb.DeleteCols(s, c, 1, &err);
  }
}

}

TEST(IncrementalMatchesFullRecalc) {
  for (uint64_t seed = 1; seed <= 25; ++seed) {
    Rng rng{seed * 7919};
    Workbook live;
    Populate(live, rng);
    live.RecalcAll();
    for (int step = 1; step <= 150; ++step) {
      RandomEdit(live, rng);
      live.Recalc();
      if (step % 30 != 0) continue;
      Workbook fresh;
      CopyInto(live, fresh);
      fresh.RecalcAll();
      if (Compare(live, fresh, "incremental vs full", seed, step) > 0) return;
    }
  }
}

TEST(ParallelMatchesSequential) {
  for (uint64_t seed = 100; seed < 108; ++seed) {
    Rng rng{seed, 160};
    Workbook one;
    Populate(one, rng);
    Rng rng2{seed, 160};
    Workbook many;
    Populate(many, rng2);
    many.SetThreads(4);
    one.RecalcAll();
    RecalcStats par = many.RecalcAll();
    CHECK(par.levels > 0);
    if (Compare(one, many, "threads", seed, 0) > 0) return;
    Rng edits{seed ^ 0xABCDEF, 160};
    Rng edits2{seed ^ 0xABCDEF, 160};
    for (int step = 1; step <= 60; ++step) {
      RandomEdit(one, edits);
      RandomEdit(many, edits2);
      one.Recalc();
      many.Recalc();
    }
    if (Compare(one, many, "threads after edits", seed, 60) > 0) return;
  }
}

TEST(FullRecalcReusesCalcChain) {
  for (uint64_t seed = 400; seed < 406; ++seed) {
    Rng rng{seed, 80};
    Workbook live;
    Populate(live, rng);
    live.SetThreads(seed % 2 == 0 ? 1 : 4);
    CHECK(!live.RecalcAll().chain_reused);
    CHECK(live.RecalcAll().chain_reused);
    for (int step = 1; step <= 30; ++step) {
      int s = rng.Below(2);
      int r = rng.Below(rng.rows);
      int c = rng.Below(kCols);
      bool formula = !live.FormulaText(s, r, c).empty();
      bool structural = step % 10 == 0;
      std::string err;
      if (structural) {
        live.SetInput(s, r, c, Formula(rng), &err);
      } else if (!formula) {
        live.SetInput(s, r, c, Constant(rng), &err);
      }
      RecalcStats st = live.RecalcAll();
      if (!structural) CHECK(st.chain_reused);
      Workbook fresh;
      CopyInto(live, fresh);
      fresh.RecalcAll();
      if (Compare(live, fresh, "chain reuse", seed, step) > 0) return;
    }
  }
}

TEST(LargeEditsReuseCalcChain) {
  for (int threads : {1, 4}) {
    for (uint64_t seed = 500; seed < 504; ++seed) {
      Rng rng{seed, 160};
      Workbook live;
      Populate(live, rng);
      std::string err;
      for (int r = 0; r < 600; ++r) {
        live.SetNumber(1, r, 10, rng.Below(100));
        std::string prev = r == 0 ? "0" : "L" + std::to_string(r);
        live.SetFormula(1, r, 11, "=" + prev + "+K" + std::to_string(r + 1), &err);
      }
      for (int r = 0; r < 160; ++r) {
        std::string ref = "S2!L" + std::to_string(r * 3 + 1);
        std::string f = r % 3 == 0   ? "=" + ref + "*2-A" + std::to_string(r + 1)
                        : r % 3 == 1 ? "=SUM(S2!L1:" + ref.substr(3) + ")+N(" + Range(rng) + ")"
                                     : "=IF(" + ref + ">1000,INDIRECT(\"S2!L\"&" +
                                           std::to_string(r + 2) + ")," + ref + ")";
        live.SetFormula(0, r, 9, f, &err);
      }
      live.SetThreads(threads);
      live.RecalcAll();
      for (int step = 1; step <= 12; ++step) {
        bool structural = step % 4 == 0;
        if (structural) {
          live.SetFormula(0, 200 + step, 9, "=S2!K1*3+S2!L" + std::to_string(step), &err);
        }
        live.SetNumber(1, rng.Below(20), 10, rng.Below(1000) - 500);
        RecalcStats st = live.Recalc();
        CHECK(st.chain_reused != structural);
        CHECK(st.evaluated >= 256);
        if (structural) live.RecalcAll();
        Workbook fresh;
        CopyInto(live, fresh);
        fresh.RecalcAll();
        if (Compare(live, fresh, "large edit", seed, step) > 0) return;
      }
    }
  }
}

TEST(InsertThenDeleteIsIdentity) {
  for (uint64_t seed = 200; seed < 215; ++seed) {
    Rng rng{seed};
    Workbook wb;
    Populate(wb, rng);
    wb.RecalcAll();
    std::vector<std::string> before;
    for (int s = 0; s < 2; ++s) {
      for (int r = 0; r < kRows; ++r) {
        for (int c = 0; c < kCols; ++c)
          before.push_back(wb.FormulaText(s, r, c) + "|" + test::Display(wb.Get(s, r, c)));
      }
    }
    int s = rng.Below(2);
    int at = rng.Below(kRows);
    int n = 1 + rng.Below(3);
    bool rows = rng.Chance(50);
    std::string err;
    bool ok = rows ? wb.InsertRows(s, at, n, &err) : wb.InsertCols(s, at % kCols, n, &err);
    CHECK(ok);
    ok = rows ? wb.DeleteRows(s, at, n, &err) : wb.DeleteCols(s, at % kCols, n, &err);
    CHECK(ok);
    wb.Recalc();
    size_t i = 0;
    int bad = 0;
    for (int sh = 0; sh < 2; ++sh) {
      for (int r = 0; r < kRows; ++r) {
        for (int c = 0; c < kCols; ++c) {
          std::string now = wb.FormulaText(sh, r, c) + "|" + test::Display(wb.Get(sh, r, c));
          if (now != before[i] && ++bad <= 3) {
            test::Fail(__FILE__, __LINE__,
                       "seed " + std::to_string(seed) + " " + CellName(r, c) + " " + before[i] +
                           " vs " + now);
          }
          ++i;
        }
      }
    }
  }
}

TEST(XlsxRoundTripPreservesRandomWorkbooks) {
  std::string path =
      (std::filesystem::temp_directory_path() / "sheet_difftest_roundtrip.xlsx").string();
  for (uint64_t seed = 300; seed < 310; ++seed) {
    Rng rng{seed};
    Workbook wb;
    Populate(wb, rng);
    wb.RecalcAll();
    std::string error;
    CHECK(SaveXlsx(wb, path, &error));
    Workbook back;
    LoadReport rep;
    CHECK(LoadWorkbook(path, back, rep, &error));
    CHECK_EQ(rep.failed, 0u);
    back.RecalcAll();
    if (Compare(wb, back, "xlsx round trip", seed, 0) > 0) break;
    for (int s = 0; s < 2; ++s) {
      for (int r = 0; r < kRows; ++r) {
        for (int c = 0; c < kCols; ++c)
          CHECK_EQ(wb.FormulaText(s, r, c), back.FormulaText(s, r, c));
      }
    }
  }
  std::filesystem::remove(path);
}

TEST_MAIN
