#include "formula.h"

#include <string>
#include <vector>

#include "test.h"
#include "util.h"
#include "workbook.h"

using namespace sheet;
using test::Book;

namespace {

std::string Canonical(Book& b, const std::string& input) {
  if (!b.Set("Z100", input)) return "parse error";
  return b.Formula("Z100");
}

}

TEST(PrintsCanonicalText) {
  Book b;
  b.wb.AddSheet("Data");
  b.wb.AddSheet("Q1 Sales");
  b.wb.AddSheet("It's");
  b.wb.AddSheet("Other");
  struct Case {
    const char* in;
    const char* out;
  };
  const Case cases[] = {
      {"=1+2*3", "=1+2*3"},
      {"=sum(a1:b2, c3)", "=SUM(A1:B2,C3)"},
      {"=  SUM( 1 , 2 )", "=SUM(1,2)"},
      {"=$A$1+A$1+$A1", "=$A$1+A$1+$A1"},
      {"=SUM(A:A)", "=SUM(A:A)"},
      {"=SUM(1:1)", "=SUM(1:1)"},
      {"=SUM($B:$D)", "=SUM($B:$D)"},
      {"=SUM(3:5)", "=SUM(3:5)"},
      {"={1,2;3,4}", "={1,2;3,4}"},
      {"={\"a\",TRUE;#N/A,-1.5}", "={\"a\",TRUE;#N/A,-1.5}"},
      {"=IF(,1,2)", "=IF(,1,2)"},
      {"=SUM(A1:B2 B1:C3)", "=SUM(A1:B2 B1:C3)"},
      {"=SUM((A1,B1))", "=SUM((A1,B1))"},
      {"=1e3", "=1000"},
      {"=\"say \"\"hi\"\"\"", "=\"say \"\"hi\"\"\""},
      {"=-(-1)", "=-(-1)"},
      {"=+5", "=+5"},
      {"=5%%", "=5%%"},
      {"=Data!A1", "=Data!A1"},
      {"='Q1 Sales'!B2:C3", "='Q1 Sales'!B2:C3"},
      {"='It''s'!A1", "='It''s'!A1"},
      {"=SUM(Data:Other!A1)", "=SUM(Data:Other!A1)"},
      {"=FOO(1)", "=FOO(1)"},
      {"=_xlfn.XLOOKUP(1,A5:A7,B5:B7)", "=XLOOKUP(1,A5:A7,B5:B7)"},
      {"=#N/A", "=#N/A"},
      {"=1<>2", "=1<>2"},
      {"=A1>=B1", "=A1>=B1"},
  };
  for (const Case& c : cases) CHECK_EQ(Canonical(b, c.in), std::string(c.out));
}

TEST(XlsxPrefixesForNewFunctions) {
  Book b;
  b.Set("Z1", "=XLOOKUP(1,A1:A3,B1:B3)+CONCAT(\"a\",\"b\")");
  int row;
  int col;
  Book::Pos("Z1", row, col);
  std::string text = b.wb.FormulaText(b.sheet, row, col, true);
  CHECK(text.find("_xlfn.XLOOKUP(") != std::string::npos);
  CHECK(text.find("_xlfn.CONCAT(") != std::string::npos);
  CHECK(text.find("_xlfn._xlfn") == std::string::npos);
}

TEST(PrintedFormulasParseBack) {
  Book b;
  b.wb.AddSheet("Data");
  const char* formulas[] = {
      "=IF(A1>0,SUM(B1:B10)/COUNT(B1:B10),\"none\")",
      "=VLOOKUP(\"x\",Data!$A$1:$C$20,3,FALSE)",
      "=-A1^2+(B1-C1)*D1%",
      "=INDEX(A1:C9,MATCH(5,B1:B9,0),2)&\" units\"",
      "=SUMPRODUCT((A1:A9>2)*(B1:B9))",
      "=IFERROR(1/(A1-A1),{1,2,3})",
      "=SUM(Data!A:A,Data!1:1)",
  };
  for (const char* f : formulas) {
    std::string once = Canonical(b, f);
    std::string twice = Canonical(b, once);
    CHECK_EQ(twice, once);
  }
}

TEST(OperatorPrecedence) {
  Book b;
  CHECK_EQ(b.Eval("=-2^2"), std::string("4"));
  CHECK_EQ(b.Eval("=2^3^2"), std::string("64"));
  CHECK_EQ(b.Eval("=2*3^2"), std::string("18"));
  CHECK_EQ(b.Eval("=-3^2+1"), std::string("10"));
  CHECK_EQ(b.Eval("=1-2-3"), std::string("-4"));
  CHECK_EQ(b.Eval("=8/2/2"), std::string("2"));
  CHECK_EQ(b.Eval("=2+3&4"), std::string("54"));
  CHECK_EQ(b.Eval("=1=1=TRUE"), std::string("TRUE"));
  CHECK_EQ(b.Eval("=10%*5"), std::string("0.5"));
  CHECK_EQ(b.Eval("=5%%"), std::string("0.0005"));
  CHECK_EQ(b.Eval("=(1+2)*3"), std::string("9"));
  CHECK_EQ(b.Eval("=1+2=3"), std::string("TRUE"));
}

TEST(ComparisonAndCoercion) {
  Book b;
  CHECK_EQ(b.Eval("=\"a\"<\"B\""), std::string("TRUE"));
  CHECK_EQ(b.Eval("=\"abc\"=\"ABC\""), std::string("TRUE"));
  CHECK_EQ(b.Eval("=1<\"a\""), std::string("TRUE"));
  CHECK_EQ(b.Eval("=\"a\"<TRUE"), std::string("TRUE"));
  CHECK_EQ(b.Eval("=FALSE<TRUE"), std::string("TRUE"));
  CHECK_EQ(b.Eval("=\"3\"+1"), std::string("4"));
  CHECK_EQ(b.Eval("=TRUE+1"), std::string("2"));
  CHECK_EQ(b.Eval("=\"\"&1"), std::string("1"));
  CHECK_EQ(b.Eval("=\"a\"+1"), std::string("#VALUE!"));
  CHECK_EQ(b.Eval("=1/0"), std::string("#DIV/0!"));
  CHECK_EQ(b.Eval("=#N/A+1"), std::string("#N/A"));
  CHECK_EQ(b.Eval("=Z99+1"), std::string("1"));
  CHECK_EQ(b.Eval("=Z99&\"x\""), std::string("x"));
  CHECK_EQ(b.Eval("=Z99=0"), std::string("TRUE"));
  CHECK_EQ(b.Eval("=Z99=\"\""), std::string("TRUE"));
  CHECK_EQ(b.Eval("=0.1+0.2=0.3"), std::string("TRUE"));
}

TEST(RejectsBadSyntax) {
  Book b;
  const char* bad[] = {"=1+",  "=SUM(1",   "=(1",      "=\"abc", "=1 2",    "=)",
                       "=A1:", "=SUM(1,,", "={1,2;3}", "=1++",   "=#BOGUS!"};
  for (const char* f : bad) {
    ParseError pe;
    ProgramRef p = CompileFormula(f, 0, 0, b.wb.Resolver(), false, &pe);
    if (p) test::Fail(__FILE__, __LINE__, std::string("accepted ") + f);
    CHECK(!p || pe.message.empty());
  }
}

TEST(SharedProgramsShiftRelativeRefs) {
  Book b;
  b.Set("B2", "=A1+$A$1+A$1+$A1");
  const Program* p = b.wb.ProgramOf(b.sheet, 1, 1);
  CHECK(p != nullptr);
  ProgramRef copy = std::make_shared<Program>(*p);
  CHECK(b.wb.SetProgram(b.sheet, 4, 2, copy));
  CHECK_EQ(b.Formula("C5"), std::string("=B4+$A$1+B$1+$A4"));
  b.Set("A1", "10");
  b.Set("B4", "5");
  b.Set("A4", "1");
  b.Set("B1", "2");
  CHECK_EQ(b.Num("C5"), 5.0 + 10 + 2 + 1);
  CHECK_EQ(b.wb.ProgramCount() >= 1, true);
}

TEST(OffSheetRelativeRefsBecomeErrors) {
  Book b;
  b.Set("A1", "=Z99");
  const Program* p = b.wb.ProgramOf(b.sheet, 0, 0);
  CHECK(p != nullptr);
  Book c;
  c.Set("C3", "=A1");
  const Program* q = c.wb.ProgramOf(c.sheet, 2, 2);
  ProgramRef moved = std::make_shared<Program>(*q);
  CHECK(c.wb.SetProgram(c.sheet, 0, 0, moved));
  CHECK_EQ(c.Show("A1"), std::string("#REF!"));
}

TEST(TranslatesOpenFormula) {
  struct Case {
    const char* in;
    const char* out;
  };
  const Case cases[] = {
      {"of:=SUM([.A1:.B2];[$Sheet2.C3])", "SUM(A1:B2,Sheet2!C3)"},
      {"of:=IF([.A1]>0;\"pos\";\"neg\")", "IF(A1>0,\"pos\",\"neg\")"},
      {"of:={1;2|3;4}", "{1,2;3,4}"},
      {"of:=[.A1:.C3]![.B2:.D4]", "A1:C3 B2:D4"},
      {"of:=#NUM!", "#NUM!"},
      {"of:=[$'My Sheet'.$A$1]", "'My Sheet'!$A$1"},
      {"of:=ORG.OPENOFFICE.WEEKS(1;2;0)", "_LO.WEEKS(1,2,0)"},
      {"of:=COM.MICROSOFT.CONCAT(\"a\";\"b\")", "CONCAT(\"a\",\"b\")"},
  };
  for (const Case& c : cases) {
    bool ok = false;
    CHECK_EQ(TranslateOpenFormula(c.in, ok), std::string(c.out));
    CHECK(ok);
  }
}

TEST(DefinedNames) {
  Book b;
  b.Set("A1", "0.25");
  CHECK(b.wb.DefineName("Rate", "Sheet1!$A$1"));
  CHECK(b.wb.DefineName("Twice", "Rate*2"));
  b.Set("B1", "=Rate*100");
  b.Set("B2", "=Twice");
  CHECK_EQ(b.Formula("B1"), std::string("=Rate*100"));
  CHECK_EQ(b.Num("B1"), 25.0);
  CHECK_EQ(b.Num("B2"), 0.5);
  b.Set("A1", "0.5");
  CHECK_EQ(b.Num("B1"), 50.0);
  CHECK_EQ(b.Num("B2"), 1.0);
  CHECK(!b.wb.DefineName("A1", "1"));
  CHECK(!b.wb.DefineName("1abc", "1"));
  CHECK(!b.wb.DefineName("TRUE", "1"));
  b.Set("B3", "=NoSuchName+1");
  CHECK_EQ(b.Show("B3"), std::string("#NAME?"));
}

TEST(StructuredReferences) {
  Book b;
  b.Set("A1", "Region");
  b.Set("B1", "Units");
  b.Set("C1", "Price");
  const char* rows[][3] = {{"East", "10", "2.5"}, {"West", "4", "10"}, {"North", "6", "1"}};
  for (int r = 0; r < 3; ++r) {
    std::string n = std::to_string(r + 2);
    b.Set("A" + n, rows[r][0]);
    b.Set("B" + n, rows[r][1]);
    b.Set("C" + n, rows[r][2]);
  }
  Table t;
  t.name = "Sales";
  t.area = Area{b.sheet, 0, 0, 3, 2};
  t.columns = {"Region", "Units", "Price"};
  CHECK(b.wb.AddTable(t));
  CHECK(!b.wb.AddTable(t));
  b.Set("D2", "=Sales[[#This Row],[Units]]*Sales[[#This Row],[Price]]");
  b.Set("D3", "=Sales[@Units]*Sales[@Price]");
  b.Set("F1", "=SUM(Sales[Units])");
  b.Set("F2", "=ROWS(Sales[#All])");
  b.Set("F3", "=ROWS(Sales)");
  b.Set("F4", "=Sales[[#Headers],[Price]]");
  b.Set("F5", "=COUNTA(Sales[[Region]:[Units]])");
  b.Set("F6", "=SUM(Sales[Nope])");
  b.Set("F7", "=SUMPRODUCT(Sales[Units],Sales[Price])");
  b.Set("F8", "=COLUMNS(Sales[[#Headers],[#Data],[Units]:[Price]])");
  b.Set("F9", "=Sales[@Units]");
  CHECK_EQ(b.Num("D2"), 25.0);
  CHECK_EQ(b.Num("D3"), 40.0);
  CHECK_EQ(b.Num("F1"), 20.0);
  CHECK_EQ(b.Num("F2"), 4.0);
  CHECK_EQ(b.Num("F3"), 3.0);
  CHECK_EQ(b.Show("F4"), std::string("Price"));
  CHECK_EQ(b.Num("F5"), 6.0);
  CHECK_EQ(b.Show("F6"), std::string("#REF!"));
  CHECK_EQ(b.Num("F7"), 71.0);
  CHECK_EQ(b.Num("F8"), 2.0);
  CHECK_EQ(b.Show("F9"), std::string("#VALUE!"));
  b.Set("B3", "8");
  CHECK_EQ(b.Num("D3"), 80.0);
  CHECK_EQ(b.Num("F1"), 24.0);
}

TEST(ProgramsAreInterned) {
  Book b;
  for (int r = 1; r <= 50; ++r) {
    std::string n = std::to_string(r);
    b.Set("A" + n, n);
    b.Set("B" + n, "=A" + n + "*2+1");
  }
  CHECK_EQ(b.wb.ProgramCount(), 1u);
  CHECK_EQ(b.Num("B50"), 101.0);
}

TEST_MAIN
