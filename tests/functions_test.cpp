#include <cstdlib>
#include <string>

#include "eval.h"
#include "test.h"
#include "util.h"

using namespace sheet;
using test::Book;

namespace {

struct Case {
  const char* formula;
  const char* expected;
};

void Fill(Book& b) {
  for (int i = 1; i <= 10; ++i) b.Set("A" + std::to_string(i), std::to_string(i));
  const char* fruit[] = {"apple", "Banana", "cherry", "date", "Elder"};
  for (int i = 0; i < 5; ++i) b.Set("B" + std::to_string(i + 1), fruit[i]);
  const char* stats[] = {"2", "4", "4", "4", "5", "5", "7", "9"};
  for (int i = 0; i < 8; ++i) b.Set("C" + std::to_string(i + 1), stats[i]);
  b.Set("D1", "TRUE");
  b.Set("D2", "text");
  b.Set("D4", "=NA()");
  b.Set("D5", "3.5");
}

void RunCases(const Case* cases, size_t n) {
  Book b;
  b.wb.AddSheet("Data");
  Fill(b);
  for (size_t i = 0; i < n; ++i) {
    std::string got = b.Eval(cases[i].formula);
    std::string want = cases[i].expected;
    char* end_want = nullptr;
    char* end_got = nullptr;
    double w = std::strtod(want.c_str(), &end_want);
    double g = std::strtod(got.c_str(), &end_got);
    bool numeric = !want.empty() && *end_want == '\0' && !got.empty() && *end_got == '\0';
    bool ok = numeric ? test::Near(g, w, 1e-9) : got == want;
    if (!ok) {
      test::Fail(__FILE__, __LINE__,
                 std::string(cases[i].formula) + "  expected " + want + "  got " + got);
    }
  }
}

}

TEST(MathFunctions) {
  const Case cases[] = {
      {"SUM(A1:A10)", "55"},
      {"SUM(1,2,\"3\")", "6"},
      {"SUM(A1:A3,TRUE)", "7"},
      {"SUM(D1:D2)", "0"},
      {"SUM(D4,1)", "#N/A"},
      {"SUMSQ(1,2,3)", "14"},
      {"PRODUCT(A1:A5)", "120"},
      {"ROUND(2.345,2)", "2.35"},
      {"ROUND(-2.5,0)", "-3"},
      {"ROUND(1234.567,-2)", "1200"},
      {"ROUNDUP(3.2,0)", "4"},
      {"ROUNDDOWN(-3.7,0)", "-3"},
      {"INT(-3.5)", "-4"},
      {"TRUNC(-3.5)", "-3"},
      {"MOD(10,3)", "1"},
      {"MOD(-10,3)", "2"},
      {"MOD(10,-3)", "-2"},
      {"MOD(5,0)", "#DIV/0!"},
      {"ABS(-4)", "4"},
      {"SIGN(-2)", "-1"},
      {"SQRT(16)", "4"},
      {"SQRT(-1)", "#NUM!"},
      {"POWER(2,10)", "1024"},
      {"EXP(0)", "1"},
      {"LN(1)", "0"},
      {"LOG(100)", "2"},
      {"LOG(8,2)", "3"},
      {"LOG10(1000)", "3"},
      {"LN(0)", "#NUM!"},
      {"PI()", "3.14159265358979"},
      {"DEGREES(PI())", "180"},
      {"RADIANS(180)", "3.14159265358979"},
      {"COS(0)", "1"},
      {"TAN(PI()/4)", "1"},
      {"ATAN2(1,1)", "0.785398163397448"},
      {"FACT(5)", "120"},
      {"FACT(0)", "1"},
      {"FACT(-1)", "#NUM!"},
      {"COMBIN(5,2)", "10"},
      {"PERMUT(5,2)", "20"},
      {"GCD(12,18)", "6"},
      {"LCM(4,6)", "12"},
      {"CEILING(2.5,1)", "3"},
      {"FLOOR(2.5,1)", "2"},
      {"FLOOR(-2.5,-2)", "-2"},
      {"MROUND(10,3)", "9"},
      {"EVEN(3)", "4"},
      {"ODD(2)", "3"},
      {"QUOTIENT(7,2)", "3"},
      {"SUMIF(A1:A10,\">5\")", "40"},
      {"SUMIF(B1:B5,\"*e*\",A1:A5)", "13"},
      {"SUMIFS(A1:A10,A1:A10,\">2\",A1:A10,\"<6\")", "12"},
      {"SUMPRODUCT(A1:A3,A4:A6)", "32"},
      {"SUMPRODUCT((A1:A10>5)*1)", "5"},
      {"BASE(255,16)", "FF"},
      {"DECIMAL(\"FF\",16)", "255"},
      {"DEC2BIN(10)", "1010"},
      {"HEX2DEC(\"FF\")", "255"},
      {"BIN2DEC(\"1010\")", "10"},
      {"MDETERM({1,2;3,4})", "-2"},
      {"INDEX(MMULT({1,2;3,4},{1;1}),2,1)", "7"},
      {"SUBTOTAL(9,A1:A10)", "55"},
      {"SUBTOTAL(1,A1:A10)", "5.5"},
  };
  RunCases(cases, std::size(cases));
}

TEST(StatisticalFunctions) {
  const Case cases[] = {
      {"AVERAGE(A1:A10)", "5.5"},
      {"AVERAGE(C1:C8)", "5"},
      {"AVERAGEA(D1:D2)", "0.5"},
      {"MEDIAN(C1:C8)", "4.5"},
      {"MODE(C1:C8)", "4"},
      {"STDEV.P(C1:C8)", "2"},
      {"STDEV(C1:C8)", "2.13808993529939"},
      {"STDEV.S(A1:A10)", "3.02765035409749"},
      {"VAR.P(C1:C8)", "4"},
      {"VAR(C1:C8)", "4.57142857142857"},
      {"MAX(A1:A10)", "10"},
      {"MIN(C1:C8)", "2"},
      {"MAX(D1:D2)", "0"},
      {"LARGE(A1:A10,2)", "9"},
      {"SMALL(A1:A10,3)", "3"},
      {"COUNT(A1:A10,D1:D5)", "11"},
      {"COUNTA(D1:D5)", "4"},
      {"COUNTBLANK(D1:D5)", "1"},
      {"COUNTIF(C1:C8,4)", "3"},
      {"COUNTIF(B1:B5,\"?a*\")", "2"},
      {"COUNTIF(C1:C8,\">=5\")", "4"},
      {"COUNTIF(B1:B5,\"<c\")", "2"},
      {"COUNTIFS(A1:A10,\">3\",A1:A10,\"<=7\")", "4"},
      {"AVERAGEIF(A1:A10,\">5\")", "8"},
      {"MAXIFS(A1:A10,A1:A10,\"<5\")", "4"},
      {"RANK(4,C1:C8)", "5"},
      {"RANK.AVG(4,C1:C8)", "6"},
      {"RANK(4,C1:C8,1)", "2"},
      {"PERCENTILE(A1:A10,0.25)", "3.25"},
      {"QUARTILE(A1:A10,3)", "7.75"},
      {"PERCENTRANK(A1:A10,4)", "0.333"},
      {"CORREL(A1:A5,A6:A10)", "1"},
      {"SLOPE(A6:A10,A1:A5)", "1"},
      {"INTERCEPT(A6:A10,A1:A5)", "5"},
      {"GEOMEAN(1,2,4)", "2"},
      {"HARMEAN(1,2,4)", "1.71428571428571"},
      {"NORM.S.DIST(0,TRUE)", "0.5"},
      {"NORM.S.INV(0.975)", "1.95996398454005"},
      {"NORM.DIST(1,0,1,TRUE)", "0.841344746068543"},
      {"BINOM.DIST(3,10,0.5,FALSE)", "0.1171875"},
      {"POISSON.DIST(2,3,FALSE)", "0.224041807655388"},
      {"T.INV.2T(0.05,10)", "2.22813885198627"},
      {"EXPON.DIST(1,1,TRUE)", "0.632120558828558"},
      {"CHISQ.DIST.RT(3.84,1)", "0.0500435212487051"},
      {"GAMMALN(10)", "12.8018274800815"},
      {"ERF(1)", "0.842700792949715"},
      {"FISHER(0.5)", "0.549306144334055"},
      {"INDEX(FREQUENCY(A1:A10,{3;6}),2)", "3"},
      {"AVERAGE(D2)", "#DIV/0!"},
  };
  RunCases(cases, std::size(cases));
}

TEST(TextFunctions) {
  const Case cases[] = {
      {"LEN(\"hello\")", "5"},
      {"LEFT(\"hello\",2)", "he"},
      {"LEFT(\"abc\")", "a"},
      {"RIGHT(\"hello\",3)", "llo"},
      {"MID(\"hello\",2,3)", "ell"},
      {"MID(\"hello\",0,1)", "#VALUE!"},
      {"UPPER(\"abc\")", "ABC"},
      {"LOWER(\"ABC\")", "abc"},
      {"PROPER(\"hello WORLD\")", "Hello World"},
      {"TRIM(\"  a   b  \")", "a b"},
      {"CONCATENATE(\"a\",\"b\",1)", "ab1"},
      {"CONCAT(B1:B2)", "appleBanana"},
      {"TEXTJOIN(\", \",TRUE,\"a\",\"\",\"b\")", "a, b"},
      {"TEXTJOIN(\"-\",FALSE,\"a\",\"\",\"b\")", "a--b"},
      {"FIND(\"l\",\"hello\")", "3"},
      {"FIND(\"L\",\"hello\")", "#VALUE!"},
      {"SEARCH(\"L\",\"hello\")", "3"},
      {"SEARCH(\"l?o\",\"hello\")", "3"},
      {"SUBSTITUTE(\"aaa\",\"a\",\"b\")", "bbb"},
      {"SUBSTITUTE(\"aaa\",\"a\",\"b\",2)", "aba"},
      {"REPLACE(\"abcdef\",2,3,\"X\")", "aXef"},
      {"REPT(\"ab\",3)", "ababab"},
      {"EXACT(\"a\",\"A\")", "FALSE"},
      {"CHAR(65)", "A"},
      {"CODE(\"A\")", "65"},
      {"UNICODE(\"\xE2\x82\xAC\")", "8364"},
      {"UNICHAR(8364)", "\xE2\x82\xAC"},
      {"TEXT(1234.567,\"#,##0.00\")", "1,234.57"},
      {"TEXT(0.5,\"0%\")", "50%"},
      {"TEXT(46095,\"yyyy-mm-dd\")", "2026-03-14"},
      {"TEXT(46095,\"dddd, mmmm d\")", "Saturday, March 14"},
      {"TEXT(0.75,\"h:mm AM/PM\")", "6:00 PM"},
      {"TEXT(-5,\"0;(0)\")", "(5)"},
      {"TEXT(1.5,\"# ?/?\")", "1 1/2"},
      {"TEXT(12345.678,\"0.00E+00\")", "1.23E+04"},
      {"TEXT(1/3,\"General\")", "0.333333333"},
      {"VALUE(\"1,234\")", "1234"},
      {"VALUE(\"abc\")", "#VALUE!"},
      {"NUMBERVALUE(\"1.234,5\",\",\",\".\")", "1234.5"},
      {"FIXED(1234.567,1)", "1,234.6"},
      {"DOLLAR(1234.567,2)", "$1,234.57"},
      {"T(1)", ""},
      {"T(\"a\")", "a"},
      {"N(TRUE)", "1"},
      {"N(\"a\")", "0"},
      {"CLEAN(CHAR(7)&\"a\")", "a"},
      {"TEXTBEFORE(\"a-b-c\",\"-\")", "a"},
      {"TEXTAFTER(\"a-b-c\",\"-\",-1)", "c"},
      {"TEXTAFTER(\"a-b:c\",{\"-\",\":\"},2)", "c"},
      {"ASC(\"\xEF\xBC\xA1\xEF\xBC\xA2\")", "AB"},
      {"LEN(\"\xF0\x9F\x98\x82\")", "2"},
  };
  RunCases(cases, std::size(cases));
}

TEST(LogicalAndInfoFunctions) {
  const Case cases[] = {
      {"IF(TRUE,1,2)", "1"},
      {"IF(FALSE,1)", "FALSE"},
      {"IF(\"x\",1,2)", "#VALUE!"},
      {"AND(TRUE,1)", "TRUE"},
      {"AND(A1:A3)", "TRUE"},
      {"OR(FALSE,0)", "FALSE"},
      {"XOR(TRUE,TRUE)", "FALSE"},
      {"NOT(0)", "TRUE"},
      {"IFERROR(1/0,\"bad\")", "bad"},
      {"IFNA(NA(),\"na\")", "na"},
      {"IFNA(1/0,\"na\")", "#DIV/0!"},
      {"IFS(1>2,\"a\",TRUE,\"b\")", "b"},
      {"IFS(1>2,\"a\")", "#N/A"},
      {"SWITCH(2,1,\"one\",2,\"two\",\"other\")", "two"},
      {"SWITCH(9,1,\"one\",\"other\")", "other"},
      {"CHOOSE(2,\"a\",\"b\",\"c\")", "b"},
      {"CHOOSE(4,\"a\",\"b\",\"c\")", "#VALUE!"},
      {"ISNUMBER(1)", "TRUE"},
      {"ISNUMBER(\"1\")", "FALSE"},
      {"ISTEXT(\"a\")", "TRUE"},
      {"ISBLANK(D3)", "TRUE"},
      {"ISERROR(D4)", "TRUE"},
      {"ISNA(D4)", "TRUE"},
      {"ISERR(D4)", "FALSE"},
      {"ISLOGICAL(D1)", "TRUE"},
      {"ISREF(A1)", "TRUE"},
      {"ISREF(1)", "FALSE"},
      {"ISEVEN(4)", "TRUE"},
      {"ISODD(3)", "TRUE"},
      {"ISFORMULA(D4)", "TRUE"},
      {"TYPE(1)", "1"},
      {"TYPE(\"a\")", "2"},
      {"TYPE(TRUE)", "4"},
      {"TYPE(NA())", "16"},
      {"TYPE({1,2})", "64"},
      {"ERROR.TYPE(NA())", "7"},
      {"ERROR.TYPE(1/0)", "2"},
      {"ERROR.TYPE(1)", "#N/A"},
      {"CELL(\"address\",A1)", "$A$1"},
      {"ROW(A5)", "5"},
      {"COLUMN(C1)", "3"},
      {"ROWS(A1:A10)", "10"},
      {"COLUMNS(A1:C1)", "3"},
      {"SHEETS()", "2"},
      {"FORMULATEXT(D4)", "=NA()"},
  };
  RunCases(cases, std::size(cases));
}

TEST(LookupFunctions) {
  const Case cases[] = {
      {"VLOOKUP(3,A1:A10,1,FALSE)", "3"},
      {"VLOOKUP(\"CHERRY\",B1:B5,1,FALSE)", "cherry"},
      {"VLOOKUP(11,A1:A10,1,FALSE)", "#N/A"},
      {"VLOOKUP(5.5,A1:A10,1,TRUE)", "5"},
      {"VLOOKUP(0,A1:A10,1,TRUE)", "#N/A"},
      {"VLOOKUP(2,A1:B5,3,FALSE)", "#REF!"},
      {"VLOOKUP(\"b*\",B1:B5,1,FALSE)", "Banana"},
      {"HLOOKUP(2,{1,2,3;\"a\",\"b\",\"c\"},2,FALSE)", "b"},
      {"MATCH(4,A1:A10,0)", "4"},
      {"MATCH(4.5,A1:A10,1)", "4"},
      {"MATCH(\"b*\",B1:B5,0)", "2"},
      {"MATCH(\"zzz\",B1:B5,0)", "#N/A"},
      {"INDEX(A1:A10,3)", "3"},
      {"INDEX({1,2;3,4},2,2)", "4"},
      {"INDEX(B1:B5,MATCH(\"date\",B1:B5,0))", "date"},
      {"SUM(INDEX(A1:C10,0,1))", "55"},
      {"XLOOKUP(\"date\",B1:B5,A1:A5)", "4"},
      {"XLOOKUP(\"zzz\",B1:B5,A1:A5,\"none\")", "none"},
      {"XLOOKUP(5.5,A1:A10,A1:A10,,1)", "6"},
      {"XLOOKUP(5.5,A1:A10,A1:A10,,-1)", "5"},
      {"XLOOKUP(4,C1:C8,A1:A8,,0,-1)", "4"},
      {"XMATCH(7,A1:A10)", "7"},
      {"LOOKUP(4.5,A1:A10)", "4"},
      {"LOOKUP(3,A1:A10,B1:B10)", "cherry"},
      {"OFFSET(A1,2,0)", "3"},
      {"SUM(OFFSET(A1,0,0,3,1))", "6"},
      {"INDIRECT(\"A4\")", "4"},
      {"INDIRECT(\"R5C1\",FALSE)", "5"},
      {"INDIRECT(\"nonsense\")", "#REF!"},
      {"ADDRESS(2,3)", "$C$2"},
      {"ADDRESS(2,3,4)", "C2"},
      {"ADDRESS(1,1,1,TRUE,\"Data\")", "Data!$A$1"},
      {"INDEX(TRANSPOSE({1,2,3}),2,1)", "2"},
      {"SUM(CHOOSE(2,A1:A2,A3:A4))", "7"},
      {"AREAS((A1:A2,C1:C3))", "2"},
      {"INDEX(SEQUENCE(3,2,10,5),3,2)", "35"},
  };
  RunCases(cases, std::size(cases));
}

TEST(DateFunctions) {
  const Case cases[] = {
      {"DATE(2026,3,14)", "46095"},
      {"DATE(2026,13,1)", "46388"},
      {"DATE(2026,3,0)", "46081"},
      {"YEAR(46095)", "2026"},
      {"MONTH(46095)", "3"},
      {"DAY(46095)", "14"},
      {"WEEKDAY(46095)", "7"},
      {"WEEKDAY(46095,2)", "6"},
      {"EDATE(DATE(2026,1,31),1)", "46081"},
      {"EOMONTH(DATE(2026,2,10),0)", "46081"},
      {"DATEDIF(DATE(2020,1,15),DATE(2026,3,14),\"y\")", "6"},
      {"DATEDIF(DATE(2020,1,15),DATE(2026,3,14),\"m\")", "73"},
      {"DATEDIF(DATE(2020,1,15),DATE(2026,3,14),\"d\")", "2250"},
      {"DATEDIF(DATE(2026,3,14),DATE(2020,1,15),\"d\")", "#NUM!"},
      {"DAYS(DATE(2026,3,14),DATE(2026,1,1))", "72"},
      {"NETWORKDAYS(DATE(2026,3,2),DATE(2026,3,13))", "10"},
      {"NETWORKDAYS(DATE(2026,3,2),DATE(2026,3,13),DATE(2026,3,4))", "9"},
      {"NETWORKDAYS.INTL(DATE(2026,3,2),DATE(2026,3,13),11)", "11"},
      {"WORKDAY(DATE(2026,3,13),1)", "46097"},
      {"WORKDAY.INTL(DATE(2026,3,13),1,\"0000011\")", "46097"},
      {"TIME(12,30,0)", "0.520833333333333"},
      {"HOUR(0.75)", "18"},
      {"MINUTE(TIME(1,2,3))", "2"},
      {"SECOND(TIME(1,2,3))", "3"},
      {"DATEVALUE(\"2026-03-14\")", "46095"},
      {"DATEVALUE(\"14-Mar-2026\")", "46095"},
      {"TIMEVALUE(\"6:00 PM\")", "0.75"},
      {"YEARFRAC(DATE(2026,1,1),DATE(2026,7,1))", "0.5"},
      {"ISOWEEKNUM(DATE(2026,1,1))", "1"},
      {"WEEKNUM(DATE(2026,3,14))", "11"},
      {"DAYS360(DATE(2026,1,31),DATE(2026,3,31))", "60"},
      {"TODAY()", "46000"},
      {"NOW()", "46000.5"},
      {"DATE(2026,2,29)", "46082"},
      {"YEAR(60)", "1900"},
  };
  RunCases(cases, std::size(cases));
}

TEST(FinancialFunctions) {
  const Case cases[] = {
      {"PMT(0.08/12,10,10000)", "-1037.03208935916"},
      {"FV(0.06/12,10,-200,-500,1)", "2581.40337406014"},
      {"PV(0.08/12,12*20,500,,0)", "-59777.1458511878"},
      {"NPER(0.12/12,-100,-1000,10000,1)", "59.6738656742946"},
      {"RATE(4*12,-200,8000)", "0.00770147248820165"},
      {"NPV(0.1,-10000,3000,4200,6800)", "1188.44341233522"},
      {"IRR({-70000,12000,15000,18000,21000,26000})", "0.0866309480365316"},
      {"IPMT(0.1/12,1,3*12,8000)", "-66.6666666666667"},
      {"PPMT(0.1/12,1,2*12,2000)", "-75.6231860083666"},
      {"SLN(30000,7500,10)", "2250"},
      {"SYD(30000,7500,10,1)", "4090.90909090909"},
      {"DDB(2400,300,10*365,1)", "1.31506849315068"},
      {"DB(1000000,100000,6,1,7)", "186083.333333333"},
      {"EFFECT(0.0525,4)", "0.0535426673707582"},
      {"XNPV(0.09,{-10000,2750,4250,3250,2750},{39448,39508,39751,39859,39904})",
       "2086.64760203154"},
      {"XIRR({-10000,2750,4250,3250,2750},{39448,39508,39751,39859,39904})", "0.373362533518831"},
      {"CUMIPMT(0.09/12,30*12,125000,13,24,0)", "-11135.2321307508"},
      {"CUMPRINC(0.09/12,30*12,125000,13,24,0)", "-934.107123420876"},
      {"MIRR({-120000,39000,30000,21000,37000,46000},0.1,0.12)", "0.126094130365905"},
      {"PMT(0,10,1000)", "-100"},
      {"RATE(3,-10,-900)", "#NUM!"},
  };
  RunCases(cases, std::size(cases));
}

std::string EvalArray(Book& b, const char* formula) {
  if (!b.wb.SetArrayFormula(b.sheet, Area{b.sheet, 899, 701, 899, 701}, formula))
    return "parse error";
  std::string out = b.Show("ZZ900");
  b.wb.Clear(b.sheet, 899, 701);
  return out;
}

TEST(ArrayFormulasAndBroadcasting) {
  Book b;
  Fill(b);
  CHECK_EQ(b.Eval("=SUM(A1:A3*A4:A6)"), std::string("#VALUE!"));
  CHECK_EQ(EvalArray(b, "=SUM(A1:A3*A4:A6)"), std::string("32"));
  CHECK_EQ(EvalArray(b, "=SUM((A1:A10>3)*(A1:A10<8))"), std::string("4"));
  CHECK_EQ(b.Eval("=INDEX({1,2,3}*{10;100},2,3)"), std::string("300"));
  CHECK_EQ(EvalArray(b, "=SUM(LEN(B1:B5))"), std::string("26"));
  CHECK_EQ(EvalArray(b, "=MAX(IF(A1:A10<5,A1:A10))"), std::string("4"));
  CHECK_EQ(b.Eval("=SUMPRODUCT(LEN(B1:B5))"), std::string("26"));
  CHECK(b.wb.SetArrayFormula(b.sheet, Area{b.sheet, 0, 5, 2, 5}, "=A1:A3*10"));
  CHECK_EQ(b.Num("F1"), 10.0);
  CHECK_EQ(b.Num("F3"), 30.0);
  CHECK(b.wb.SetArrayFormula(b.sheet, Area{b.sheet, 0, 6, 0, 8}, "=COLUMN()"));
  CHECK_EQ(b.Num("G1"), 7.0);
  CHECK_EQ(b.Num("I1"), 9.0);
  CHECK(b.wb.SetArrayFormula(b.sheet, Area{b.sheet, 4, 5, 5, 5}, "={1,2,3}"));
  CHECK_EQ(b.Num("F5"), 1.0);
  CHECK_EQ(b.Num("F6"), 1.0);
  CHECK(b.wb.SetArrayFormula(b.sheet, Area{b.sheet, 7, 5, 8, 7}, "={1,2}"));
  CHECK_EQ(b.Show("H8"), std::string("#N/A"));
}

TEST(ExtremeArgumentsAreHandled) {
  Book b;
  Fill(b);
  const char* values[] = {"1E+300", "-1E+300",   "1E+15",   "-1",   "0",
                          "0.5",    "2",         "\"abc\"", "TRUE", "A1:A10",
                          "B1:B5",  "{1,2;3,4}", "D4",      "\"\"", "999999999"};
  uint64_t state = 12345;
  auto next = [&]() {
    state += 0x9E3779B97F4A7C15ULL;
    return Mix64(state);
  };
  int calls = 0;
  for (int id = 0; id < FunctionCount(); ++id) {
    const FuncInfo& f = FunctionAt(id);
    if (f.flags & kFnVolatile) continue;
    for (int trial = 0; trial < 40; ++trial) {
      int span = std::min(f.max_args, f.min_args + 2) - f.min_args + 1;
      int argc = f.min_args + static_cast<int>(next() % static_cast<uint64_t>(span));
      std::string call = std::string(f.name) + "(";
      for (int i = 0; i < argc; ++i) {
        if (i > 0) call += ",";
        call += values[next() % std::size(values)];
      }
      call += ")";
      std::string out = b.Eval(call);
      ++calls;
    }
  }
  CHECK(calls > 10000);
}

TEST_MAIN
