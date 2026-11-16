#include "core.h"

#include <cmath>
#include <string>

#include "format.h"
#include "test.h"

using namespace sheet;

TEST(ValueKindsAndCopies) {
  Value n = Value::Number(2.5);
  Value t = Value::Text("hello");
  Value b = Value::Bool(true);
  Value e = Value::Error(Err::Div0);
  CHECK(n.IsNumber());
  CHECK(t.IsText());
  CHECK(b.IsBool());
  CHECK(e.IsError());
  CHECK(Value().IsBlank());
  Value t2 = t;
  CHECK_EQ(std::string(t2.text()), std::string("hello"));
  Value t3 = std::move(t2);
  CHECK(t2.IsBlank());
  CHECK_EQ(std::string(t3.text()), std::string("hello"));
  t3 = n;
  CHECK(t3.IsNumber());
  CHECK_EQ(std::string(t.text()), std::string("hello"));
  CHECK(n.SameAs(Value::Number(2.5)));
  CHECK(!n.SameAs(Value::Number(2.6)));
  CHECK(e.SameAs(Value::Error(Err::Div0)));
  CHECK(!e.SameAs(Value::Error(Err::NA)));
}

TEST(ValueArrays) {
  Value a = Value::Array(2, 3);
  CHECK_EQ(a.rows(), 2u);
  CHECK_EQ(a.cols(), 3u);
  a.mut(1, 2) = Value::Text("x");
  a.mut(0, 0) = Value::Number(1);
  Value copy = a;
  CHECK_EQ(std::string(copy.at(1, 2).text()), std::string("x"));
  CHECK_EQ(copy.at(0, 0).number(), 1.0);
  CHECK(copy.at(0, 1).IsBlank());
  CHECK(a.SameAs(copy));
}

TEST(ErrorTextRoundTrip) {
  const Err all[] = {Err::Null, Err::Div0, Err::Value, Err::Ref, Err::Name, Err::Num, Err::NA};
  for (Err e : all) {
    Err back = Err::None;
    CHECK(ParseErrorText(ErrorText(e), back));
    CHECK(back == e);
  }
  CHECK_EQ(std::string(ErrorText(Err::Div0)), std::string("#DIV/0!"));
  CHECK_EQ(std::string(ErrorText(Err::NA)), std::string("#N/A"));
  Err e;
  CHECK(!ParseErrorText("#WHAT", e));
}

TEST(FormatGeneralMatchesExcel) {
  CHECK_EQ(FormatGeneral(1), std::string("1"));
  CHECK_EQ(FormatGeneral(-0.5), std::string("-0.5"));
  CHECK_EQ(FormatGeneral(0.1 + 0.2), std::string("0.3"));
  CHECK_EQ(FormatGeneral(1.0 / 3), std::string("0.333333333333333"));
  CHECK_EQ(FormatGeneral(2.0 / 3), std::string("0.666666666666667"));
  CHECK_EQ(FormatGeneral(123456789012345.0), std::string("123456789012345"));
  CHECK_EQ(FormatGeneral(1e21), std::string("1E+21"));
  CHECK_EQ(FormatGeneral(1e-10), std::string("0.0000000001"));
  CHECK_EQ(FormatGeneral(1.23e-20), std::string("1.23E-20"));
  CHECK_EQ(FormatGeneral(0.0001), std::string("0.0001"));
  CHECK_EQ(FormatGeneral(12345.678), std::string("12345.678"));
  CHECK_EQ(FormatGeneral(9007199254740992.0), std::string("9007199254740990"));
  CHECK_EQ(FormatGeneral(1.5e20), std::string("1.5E+20"));
  CHECK_EQ(FormatGeneral(0), std::string("0"));
}

TEST(ApproxArithmetic) {
  CHECK(ApproxEqual(0.1 + 0.2, 0.3));
  CHECK(!ApproxEqual(1.0, 1.0 + 1e-10));
  CHECK_EQ(ApproxSub(0.3, 0.1 + 0.2), 0.0);
  CHECK_EQ(ApproxAdd(0.1 + 0.2, -0.3), 0.0);
  CHECK_EQ(ApproxAdd(1, 2), 3.0);
  CHECK_EQ(Round15(0.1 + 0.2), 0.3);
}

TEST(RoundDigitsHalfAwayFromZero) {
  CHECK_EQ(RoundDigits(2.5, 0), 3.0);
  CHECK_EQ(RoundDigits(-2.5, 0), -3.0);
  CHECK_EQ(RoundDigits(1.005, 2), 1.01);
  CHECK_EQ(RoundDigits(0.285, 2), 0.29);
  CHECK_EQ(RoundDigits(1234.5, -2), 1200.0);
  CHECK_EQ(RoundDigits(-1234.5, -2), -1200.0);
  CHECK_EQ(RoundDigits(5.55, 1), 5.6);
  CHECK_EQ(RoundDigits(0.0, 3), 0.0);
}

TEST(NumberLiterals) {
  double d = 0;
  CHECK(ParseNumberLiteral("42", d));
  CHECK_EQ(d, 42.0);
  CHECK(ParseNumberLiteral("1.5e3", d));
  CHECK_EQ(d, 1500.0);
  CHECK(ParseNumberLiteral(".25", d));
  CHECK_EQ(d, 0.25);
  CHECK(!ParseNumberLiteral("1.2.3", d));
  CHECK(!ParseNumberLiteral("abc", d));
}

TEST(NumberTextLikeValueFunction) {
  DateSystem ds;
  double d = 0;
  CHECK(ParseNumberText("1,234.5", ds, d));
  CHECK_EQ(d, 1234.5);
  CHECK(ParseNumberText("50%", ds, d));
  CHECK_EQ(d, 0.5);
  CHECK(ParseNumberText("$1,000", ds, d));
  CHECK_EQ(d, 1000.0);
  CHECK(ParseNumberText(" 42 ", ds, d));
  CHECK_EQ(d, 42.0);
  CHECK(ParseNumberText("(12)", ds, d));
  CHECK_EQ(d, -12.0);
  CHECK(ParseNumberText("1e3", ds, d));
  CHECK_EQ(d, 1000.0);
  CHECK(ParseNumberText("3/14/2026", ds, d));
  CHECK_EQ(d, 46095.0);
  CHECK(ParseNumberText("2026-03-14", ds, d));
  CHECK_EQ(d, 46095.0);
  CHECK(ParseNumberText("12:30", ds, d));
  CHECK_NEAR(d, 12.5 / 24, 1e-12);
  CHECK(ParseNumberText("6:00 PM", ds, d));
  CHECK_NEAR(d, 0.75, 1e-12);
  CHECK(!ParseNumberText("abc", ds, d));
  CHECK(!ParseNumberText("", ds, d));
  CHECK(!ParseNumberText("1,2,3x", ds, d));
}

TEST(DateSerials1900) {
  DateSystem ds;
  double s = 0;
  CHECK(SerialFromDate(1900, 1, 1, ds, s));
  CHECK_EQ(s, 1.0);
  CHECK(SerialFromDate(1900, 2, 28, ds, s));
  CHECK_EQ(s, 59.0);
  CHECK(SerialFromDate(1900, 3, 1, ds, s));
  CHECK_EQ(s, 61.0);
  CHECK(SerialFromDate(2026, 3, 14, ds, s));
  CHECK_EQ(s, 46095.0);
  CHECK(SerialFromDate(9999, 12, 31, ds, s));
  CHECK_EQ(s, 2958465.0);
  int y = 0;
  int m = 0;
  int d = 0;
  CHECK(DateFromSerial(60, ds, y, m, d));
  CHECK(y == 1900 && m == 2 && d == 29);
  CHECK(DateFromSerial(45351, ds, y, m, d));
  CHECK(y == 2024 && m == 2 && d == 29);
  CHECK(DateFromSerial(0, ds, y, m, d));
  CHECK(y == 1900 && m == 1 && d == 0);
  for (double serial = 61; serial < 80000; serial += 997) {
    CHECK(DateFromSerial(serial, ds, y, m, d));
    double back = 0;
    CHECK(SerialFromDate(y, m, d, ds, back));
    CHECK_EQ(back, serial);
  }
  CHECK_EQ(WeekdayFromSerial(46095, ds), 6);
}

TEST(DateSerials1904) {
  DateSystem ds;
  ds.date1904 = true;
  double s = -1;
  CHECK(SerialFromDate(1904, 1, 1, ds, s));
  CHECK_EQ(s, 0.0);
  CHECK(SerialFromDate(2026, 3, 14, ds, s));
  CHECK_EQ(s, 44633.0);
  int y = 0;
  int m = 0;
  int d = 0;
  CHECK(DateFromSerial(44633, ds, y, m, d));
  CHECK(y == 2026 && m == 3 && d == 14);
}

TEST(CivilDayArithmetic) {
  int y = 0;
  int m = 0;
  int d = 0;
  CivilFromDays(DaysFromCivil(2000, 2, 29), y, m, d);
  CHECK(y == 2000 && m == 2 && d == 29);
  CHECK(IsLeapYear(2000));
  CHECK(!IsLeapYear(1900));
  CHECK(IsLeapYear(2024));
  CHECK_EQ(DaysInMonth(2023, 2), 28);
  CHECK_EQ(DaysInMonth(2024, 2), 29);
  CHECK_EQ(DaysInMonth(2024, 12), 31);
}

TEST(TimeFromSerialRoundsSeconds) {
  int h = 0;
  int mi = 0;
  int s = 0;
  TimeFromSerial(0.5, h, mi, s);
  CHECK(h == 12 && mi == 0 && s == 0);
  TimeFromSerial(46095.75, h, mi, s);
  CHECK(h == 18 && mi == 0 && s == 0);
  TimeFromSerial(1.0 / 86400 * 3661, h, mi, s);
  CHECK(h == 1 && mi == 1 && s == 1);
}

TEST(TextComparisonIgnoresCase) {
  CHECK(EqualsText("abc", "ABC"));
  CHECK(
      EqualsText("Stra\xC3\x9F"
                 "e",
                 "STRA\xC3\x9F"
                 "E"));
  CHECK(!EqualsText("abc", "abd"));
  CHECK(CompareText("a", "B") < 0);
  CHECK(CompareText("Z", "a") > 0);
  CHECK(CompareText("abc", "ABC") == 0);
  CHECK(CompareText("ab", "abc") < 0);
  CHECK(CompareText("_", "a") < 0);
  CHECK(EqualsText("\xC3\x84rger", "\xC3\xA4rger"));
  CHECK(EqualsText("\xCE\xA3\xCE\x9F\xCE\xA6\xCE\x99\xCE\x91",
                   "\xCF\x83\xCE\xBF\xCF\x86\xCE\xB9\xCE\xB1"));
  CHECK(EqualsText("\xD0\x9C\xD0\x9E\xD0\xA1\xD0\x9A\xD0\x92\xD0\x90",
                   "\xD0\xBC\xD0\xBE\xD1\x81\xD0\xBA\xD0\xB2\xD0\xB0"));
}

TEST(CaseMapping) {
  CHECK_EQ(ToUpper("hello world"), std::string("HELLO WORLD"));
  CHECK_EQ(ToLower("MiXeD 123"), std::string("mixed 123"));
  CHECK_EQ(ToUpper("\xC3\xA9\xC3\xA0\xC3\xBC"), std::string("\xC3\x89\xC3\x80\xC3\x9C"));
  CHECK_EQ(ToLower("\xC3\x89\xC3\x80\xC3\x9C"), std::string("\xC3\xA9\xC3\xA0\xC3\xBC"));
  CHECK_EQ(ToUpper("\xCF\x89\xCE\xBC\xCE\xAD\xCE\xB3\xCE\xB1"),
           ToUpper("\xCE\xA9\xCE\x9C\xCE\x88\xCE\x93\xCE\x91"));
}

TEST(Utf8Helpers) {
  CHECK_EQ(Utf8Units("abc"), 3);
  CHECK_EQ(Utf8Units("\xC3\xA9"), 1);
  CHECK_EQ(Utf8Units("\xF0\x9F\x98\x82"), 2);
  CHECK_EQ(Utf8Encode(0x41), std::string("A"));
  CHECK_EQ(Utf8Encode(0xE9), std::string("\xC3\xA9"));
  CHECK_EQ(Utf8Encode(0x20AC), std::string("\xE2\x82\xAC"));
  CHECK_EQ(Utf8Encode(0x1F602), std::string("\xF0\x9F\x98\x82"));
  std::string s =
      "a\xE2\x82\xAC"
      "b";
  size_t i = 0;
  CHECK_EQ(Utf8Decode(s, i), 0x61u);
  CHECK_EQ(Utf8Decode(s, i), 0x20ACu);
  CHECK_EQ(Utf8Decode(s, i), 0x62u);
  CHECK_EQ(i, s.size());
  std::string broken = "\xE2\x82";
  size_t j = 0;
  Utf8Decode(broken, j);
  CHECK(j <= broken.size());
}

TEST(ColumnNames) {
  CHECK_EQ(ColumnName(0), std::string("A"));
  CHECK_EQ(ColumnName(25), std::string("Z"));
  CHECK_EQ(ColumnName(26), std::string("AA"));
  CHECK_EQ(ColumnName(701), std::string("ZZ"));
  CHECK_EQ(ColumnName(702), std::string("AAA"));
  CHECK_EQ(ColumnName(16383), std::string("XFD"));
  int c = -1;
  CHECK(ParseColumn("XFD", c));
  CHECK_EQ(c, 16383);
  CHECK(ParseColumn("aa", c));
  CHECK_EQ(c, 26);
  CHECK(!ParseColumn("XFE", c));
  CHECK(!ParseColumn("", c));
  for (int col = 0; col < kMaxCols; col += 37) {
    int back = -1;
    CHECK(ParseColumn(ColumnName(col), back));
    CHECK_EQ(back, col);
  }
}

TEST(CellAndAreaNames) {
  CHECK_EQ(CellName(0, 0), std::string("A1"));
  CHECK_EQ(CellName(1048575, 16383), std::string("XFD1048576"));
  int r = -1;
  int c = -1;
  CHECK(ParseCellName("B3", r, c));
  CHECK(r == 2 && c == 1);
  CHECK(ParseCellName("xfd1048576", r, c));
  CHECK(r == 1048575 && c == 16383);
  CHECK(!ParseCellName("A0", r, c));
  CHECK(!ParseCellName("A1048577", r, c));
  CHECK(!ParseCellName("1A", r, c));
  int r1 = 0;
  int c1 = 0;
  int r2 = 0;
  int c2 = 0;
  CHECK(ParseAreaName("A1:C3", r1, c1, r2, c2));
  CHECK(r1 == 0 && c1 == 0 && r2 == 2 && c2 == 2);
  CHECK(ParseAreaName("D7", r1, c1, r2, c2));
  CHECK(r1 == 6 && c1 == 3 && r2 == 6 && c2 == 3);
}

TEST(MixAndHashAreStable) {
  CHECK(Mix64(1) != Mix64(2));
  CHECK_EQ(HashBytes("abc", 3), HashBytes("abc", 3));
  CHECK(HashBytes("abc", 3) != HashBytes("abd", 3));
}

TEST_MAIN
