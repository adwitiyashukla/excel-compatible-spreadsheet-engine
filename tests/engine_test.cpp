#include <cmath>
#include <string>

#include "test.h"
#include "util.h"
#include "workbook.h"

using namespace sheet;
using test::Book;

TEST(EditsPropagateIncrementally) {
  Book b;
  b.Set("A1", "1");
  b.Set("B1", "=A1*2");
  b.Set("C1", "=B1+1");
  b.Set("D1", "=7");
  CHECK_EQ(b.Num("C1"), 3.0);
  b.Set("A1", "5");
  RecalcStats st = b.wb.Recalc();
  CHECK_EQ(st.evaluated, 2u);
  CHECK_EQ(b.Num("C1"), 11.0);
  CHECK_EQ(b.wb.Recalc().evaluated, 0u);
}

TEST(RangeDependencies) {
  Book b;
  b.Set("D1", "=SUM(A1:A100)");
  b.Set("E1", "=SUM(A:A)");
  b.Set("F1", "=COUNT(A2:A3)");
  CHECK_EQ(b.Num("D1"), 0.0);
  b.Set("A50", "7");
  CHECK_EQ(b.Num("D1"), 7.0);
  CHECK_EQ(b.Num("E1"), 7.0);
  b.Set("A200", "3");
  CHECK_EQ(b.Num("D1"), 7.0);
  CHECK_EQ(b.Num("E1"), 10.0);
  b.Set("A2", "1");
  CHECK_EQ(b.Num("F1"), 1.0);
  b.wb.Clear(b.sheet, 49, 0);
  CHECK_EQ(b.Num("D1"), 1.0);
  CHECK_EQ(b.Num("E1"), 4.0);
}

TEST(OnlyAffectedFormulasRun) {
  Book b;
  for (int i = 1; i <= 50; ++i) {
    std::string n = std::to_string(i);
    b.Set("A" + n, "=" + std::string(i == 1 ? "X1" : "A" + std::to_string(i - 1)) + "+1");
    b.Set("B" + n, "=" + std::string(i == 1 ? "Y1" : "B" + std::to_string(i - 1)) + "+1");
  }
  b.wb.Recalc();
  b.Set("X1", "10");
  RecalcStats st = b.wb.Recalc();
  CHECK_EQ(st.evaluated, 50u);
  CHECK_EQ(b.Num("A50"), 60.0);
  CHECK_EQ(b.Num("B50"), 50.0);
}

TEST(DynamicReferencesFollowTheirTargets) {
  Book b;
  b.Set("A1", "10");
  b.Set("A2", "20");
  b.Set("A3", "30");
  b.Set("G1", "2");
  b.Set("F1", "=INDIRECT(\"A\"&G1)");
  b.Set("F2", "=SUM(OFFSET(A1,0,0,G1,1))");
  CHECK_EQ(b.Num("F1"), 20.0);
  CHECK_EQ(b.Num("F2"), 30.0);
  b.Set("A2", "25");
  CHECK_EQ(b.Num("F1"), 25.0);
  CHECK_EQ(b.Num("F2"), 35.0);
  b.Set("G1", "3");
  CHECK_EQ(b.Num("F1"), 30.0);
  CHECK_EQ(b.Num("F2"), 65.0);
  b.Set("A3", "1");
  CHECK_EQ(b.Num("F1"), 1.0);
  CHECK_EQ(b.Num("F2"), 36.0);
}

TEST(DynamicChainOrdering) {
  Book b;
  b.Set("A1", "1");
  b.Set("B1", "=INDIRECT(\"C1\")*2");
  b.Set("C1", "=A1+1");
  b.Set("D1", "=B1+INDIRECT(\"C1\")");
  CHECK_EQ(b.Num("B1"), 4.0);
  CHECK_EQ(b.Num("D1"), 6.0);
  b.Set("A1", "9");
  CHECK_EQ(b.Num("B1"), 20.0);
  CHECK_EQ(b.Num("D1"), 30.0);
}

TEST(CyclesAreDetected) {
  Book b;
  b.Set("A1", "=B1+1");
  b.Set("B1", "=A1+1");
  b.Set("C1", "=A1*2");
  b.Set("D1", "=5");
  CHECK_EQ(b.Show("A1"), std::string("#REF!"));
  CHECK_EQ(b.Show("B1"), std::string("#REF!"));
  CHECK_EQ(b.Show("C1"), std::string("#REF!"));
  CHECK_EQ(b.Num("D1"), 5.0);
  CHECK(b.wb.InCycle(b.sheet, 0, 0));
  CHECK(!b.wb.InCycle(b.sheet, 0, 3));
  b.Set("B1", "5");
  CHECK_EQ(b.Num("A1"), 6.0);
  CHECK_EQ(b.Num("C1"), 12.0);
  CHECK(!b.wb.InCycle(b.sheet, 0, 0));
  b.Set("E1", "=E1+1");
  CHECK_EQ(b.Show("E1"), std::string("#REF!"));
}

TEST(IterativeCalculationConverges) {
  Book b;
  b.wb.SetIteration(true, 200, 1e-9);
  b.Set("A1", "=(A1+10)/2");
  b.Set("B1", "=1000");
  b.Set("B2", "=B1+B3");
  b.Set("B3", "=B2*0.1");
  CHECK_NEAR(b.Num("A1"), 10.0, 1e-6);
  CHECK_NEAR(b.Num("B2"), 1000.0 / 0.9, 1e-6);
  CHECK_NEAR(b.Num("B3"), 100.0 / 0.9, 1e-6);
}

TEST(VolatileFunctionsRecalculate) {
  Book b;
  b.Set("A1", "=RAND()");
  b.Set("A2", "=RANDBETWEEN(1,6)");
  b.Set("A3", "=NOW()");
  b.Set("A4", "=A1*0+1");
  double first = b.Num("A1");
  CHECK(first >= 0 && first < 1);
  bool changed = false;
  for (int i = 0; i < 5 && !changed; ++i) {
    b.wb.Recalc();
    changed = b.Num("A1") != first;
  }
  CHECK(changed);
  for (int i = 0; i < 20; ++i) {
    b.wb.Recalc();
    double d = b.Num("A2");
    CHECK(d >= 1 && d <= 6 && d == std::floor(d));
  }
  CHECK_EQ(b.Num("A3"), 46000.5);
  CHECK_EQ(b.Num("A4"), 1.0);
}

TEST(ArrayFormulaDependencies) {
  Book b;
  b.Set("A1", "1");
  b.Set("A2", "2");
  b.Set("A3", "3");
  CHECK(b.wb.SetArrayFormula(b.sheet, Area{b.sheet, 0, 5, 2, 5}, "=A1:A3*2"));
  b.Set("G1", "=SUM(F1:F3)");
  CHECK_EQ(b.Num("G1"), 12.0);
  b.Set("A2", "10");
  CHECK_EQ(b.Num("F2"), 20.0);
  CHECK_EQ(b.Num("G1"), 28.0);
  Area g;
  CHECK(b.wb.ArrayGroup(b.sheet, 1, 5, g));
  CHECK(g == (Area{b.sheet, 0, 5, 2, 5}));
  std::string err;
  CHECK(!b.wb.SetFormula(b.sheet, 1, 5, "=1", &err));
  CHECK(!err.empty());
  CHECK(!b.wb.SetNumber(b.sheet, 0, 5, 3));
  CHECK_EQ(b.Num("F2"), 20.0);
  b.wb.Clear(b.sheet, 1, 5);
  CHECK(!b.wb.ArrayGroup(b.sheet, 0, 5, g));
  CHECK_EQ(b.Num("G1"), 0.0);
}

TEST(NamesTrackTheirTargets) {
  Book b;
  b.Set("A1", "1");
  b.Set("A2", "2");
  CHECK(b.wb.DefineName("Items", "Sheet1!$A$1:$A$2"));
  b.Set("B1", "=SUM(Items)");
  CHECK_EQ(b.Num("B1"), 3.0);
  b.Set("A2", "5");
  CHECK_EQ(b.Num("B1"), 6.0);
  CHECK(b.wb.DefineName("Items", "Sheet1!$A$1"));
  CHECK_EQ(b.Num("B1"), 1.0);
  int s2 = b.wb.AddSheet("Other");
  CHECK(b.wb.DefineName("Local", "7", s2));
  b.Set("C1", "=Local");
  b.Set("C1", "=Local", s2);
  CHECK_EQ(b.Show("C1"), std::string("#NAME?"));
  CHECK_EQ(b.Num("C1", s2), 7.0);
}

TEST(CrossSheetReferences) {
  Book b;
  int s2 = b.wb.AddSheet("Second Sheet");
  b.Set("A1", "4");
  b.Set("A1", "=Sheet1!A1*2", s2);
  b.Set("B1", "='Second Sheet'!A1+1");
  CHECK_EQ(b.Num("B1"), 9.0);
  b.Set("A1", "10");
  CHECK_EQ(b.Num("B1"), 21.0);
  CHECK(b.wb.RenameSheet(s2, "Renamed"));
  CHECK_EQ(b.Formula("B1"), std::string("=Renamed!A1+1"));
  CHECK_EQ(b.Num("B1"), 21.0);
}

TEST(ErrorsPropagate) {
  Book b;
  b.Set("A1", "=1/0");
  b.Set("A2", "=A1+1");
  b.Set("A3", "=IFERROR(A2,-1)");
  b.Set("A4", "=SUM(A1:A3)");
  CHECK_EQ(b.Show("A2"), std::string("#DIV/0!"));
  CHECK_EQ(b.Num("A3"), -1.0);
  CHECK_EQ(b.Show("A4"), std::string("#DIV/0!"));
  b.Set("A1", "=2");
  CHECK_EQ(b.Num("A2"), 3.0);
  CHECK_EQ(b.Num("A3"), 3.0);
  CHECK_EQ(b.Num("A4"), 8.0);
}

TEST(DeepChainsDoNotRecurse) {
  Book b;
  const int n = 100000;
  b.Set("A1", "1");
  for (int r = 1; r < n; ++r) {
    std::string err;
    CHECK(b.wb.SetFormula(b.sheet, r, 0, "=A" + std::to_string(r) + "+1", &err));
  }
  RecalcStats st = b.wb.RecalcAll();
  CHECK_EQ(st.evaluated, static_cast<size_t>(n - 1));
  CHECK_EQ(b.Num("A" + std::to_string(n)), static_cast<double>(n));
  b.Set("A1", "2");
  CHECK_EQ(b.Num("A" + std::to_string(n)), static_cast<double>(n + 1));
}

TEST(InsertRowsRewritesReferences) {
  Book b;
  b.Set("A1", "1");
  b.Set("A2", "2");
  b.Set("A3", "3");
  b.Set("B1", "=SUM(A1:A3)");
  b.Set("B2", "=A3*10");
  b.Set("B3", "=$A$1+A2");
  CHECK(b.wb.InsertRows(b.sheet, 1, 2));
  CHECK_EQ(b.Formula("B1"), std::string("=SUM(A1:A5)"));
  CHECK_EQ(b.Formula("B4"), std::string("=A5*10"));
  CHECK_EQ(b.Formula("B5"), std::string("=$A$1+A4"));
  CHECK_EQ(b.Num("B1"), 6.0);
  CHECK_EQ(b.Num("B4"), 30.0);
  CHECK_EQ(b.Num("B5"), 3.0);
  b.Set("A2", "100");
  CHECK_EQ(b.Num("B1"), 106.0);
  CHECK(b.wb.InsertRows(b.sheet, 0, 1));
  CHECK_EQ(b.Formula("B2"), std::string("=SUM(A2:A6)"));
  CHECK(b.wb.InsertRows(b.sheet, 6, 1));
  CHECK_EQ(b.Formula("B2"), std::string("=SUM(A2:A6)"));
}

TEST(DeleteRowsRewritesReferences) {
  Book b;
  for (int r = 1; r <= 6; ++r) b.Set("A" + std::to_string(r), std::to_string(r));
  b.Set("C1", "=SUM(A1:A6)");
  b.Set("C2", "=A5");
  b.Set("C3", "=A6*2");
  b.Set("C4", "=SUM(A2:A3)");
  CHECK(b.wb.DeleteRows(b.sheet, 4, 1));
  CHECK_EQ(b.Formula("C1"), std::string("=SUM(A1:A5)"));
  CHECK_EQ(b.Formula("C2"), std::string("=#REF!"));
  CHECK_EQ(b.Formula("C3"), std::string("=A5*2"));
  CHECK_EQ(b.Num("C1"), 16.0);
  CHECK_EQ(b.Show("C2"), std::string("#REF!"));
  CHECK_EQ(b.Num("C3"), 12.0);
  CHECK(b.wb.DeleteRows(b.sheet, 1, 2));
  CHECK_EQ(b.Show("C2"), std::string("#REF!"));
  CHECK_EQ(b.Num("C1"), 11.0);
}

TEST(ColumnEditsRewriteReferences) {
  Book b;
  b.Set("A1", "1");
  b.Set("B1", "2");
  b.Set("C1", "3");
  b.Set("E1", "=SUM(A1:C1)");
  b.Set("E2", "=C1");
  b.Set("E3", "=B1");
  CHECK(b.wb.InsertCols(b.sheet, 1, 1));
  CHECK_EQ(b.Formula("F1"), std::string("=SUM(A1:D1)"));
  CHECK_EQ(b.Formula("F2"), std::string("=D1"));
  CHECK_EQ(b.Num("F1"), 6.0);
  CHECK(b.wb.DeleteCols(b.sheet, 2, 1));
  CHECK_EQ(b.Formula("E1"), std::string("=SUM(A1:C1)"));
  CHECK_EQ(b.Formula("E3"), std::string("=#REF!"));
  CHECK_EQ(b.Num("E1"), 4.0);
  std::string err;
  CHECK(!b.wb.DeleteCols(b.sheet, 0, kMaxCols + 1, &err));
  CHECK(!err.empty());
}

TEST(StructureEditsAndArrays) {
  Book b;
  b.Set("A1", "1");
  b.Set("A2", "2");
  CHECK(b.wb.SetArrayFormula(b.sheet, Area{b.sheet, 0, 2, 1, 2}, "=A1:A2+1"));
  std::string err;
  CHECK(!b.wb.InsertRows(b.sheet, 1, 1, &err));
  CHECK(b.wb.InsertRows(b.sheet, 0, 1, &err));
  CHECK_EQ(b.Num("C2"), 2.0);
  CHECK_EQ(b.Num("C3"), 3.0);
  b.Set("A3", "10");
  CHECK_EQ(b.Num("C3"), 11.0);
}

TEST(HiddenRowsAffectSubtotal) {
  Book b;
  for (int r = 1; r <= 4; ++r) b.Set("A" + std::to_string(r), std::to_string(r * 10));
  b.Set("B1", "=SUBTOTAL(9,A1:A4)");
  b.Set("B2", "=SUBTOTAL(109,A1:A4)");
  b.Set("B3", "=SUM(A1:A4)");
  CHECK_EQ(b.Num("B2"), 100.0);
  b.wb.SetRowHidden(b.sheet, 1, true, false);
  CHECK_EQ(b.Num("B1"), 100.0);
  CHECK_EQ(b.Num("B2"), 80.0);
  b.wb.SetRowHidden(b.sheet, 2, true, true);
  CHECK_EQ(b.Num("B1"), 70.0);
  CHECK_EQ(b.Num("B2"), 50.0);
  CHECK_EQ(b.Num("B3"), 100.0);
  b.wb.SetRowHidden(b.sheet, 1, false);
  b.wb.SetRowHidden(b.sheet, 2, false);
  CHECK_EQ(b.Num("B2"), 100.0);
}

TEST(ParallelRecalcMatchesSequential) {
  Book seq;
  Book par;
  for (Book* b : {&seq, &par}) {
    for (int r = 0; r < 3000; ++r) {
      std::string n = std::to_string(r + 1);
      b->wb.SetNumber(b->sheet, r, 0, (r * 37) % 101);
      b->Set("B" + n, "=A" + n + "*2+1");
      b->Set("C" + n, r == 0 ? "=B1" : "=C" + std::to_string(r) + "+B" + n);
      b->Set("D" + n, "=SUM($A$1:A" + n + ")/(ROW()+1)");
    }
    b->Set("E1", "=SUM(C:C)+MAX(D:D)");
  }
  par.wb.SetThreads(4);
  seq.wb.RecalcAll();
  par.wb.RecalcAll();
  for (int r = 0; r < 3000; ++r) {
    for (int c = 1; c <= 3; ++c)
      CHECK(seq.wb.Get(seq.sheet, r, c).SameAs(par.wb.Get(par.sheet, r, c)));
  }
  CHECK(seq.Get("E1").SameAs(par.Get("E1")));
  seq.Set("A7", "1000");
  par.Set("A7", "1000");
  CHECK(seq.Get("E1").SameAs(par.Get("E1")));
}

TEST_MAIN
