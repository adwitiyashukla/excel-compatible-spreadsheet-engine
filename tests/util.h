#pragma once

#include <string>
#include <string_view>

#include "core.h"
#include "workbook.h"

namespace test {

inline std::string Display(const sheet::Value& v) {
  switch (v.kind()) {
    case sheet::Kind::Blank:
      return "";
    case sheet::Kind::Number:
      return sheet::FormatGeneral(v.number());
    case sheet::Kind::Bool:
      return v.boolean() ? "TRUE" : "FALSE";
    case sheet::Kind::Text:
      return std::string(v.text());
    case sheet::Kind::Error:
      return std::string(sheet::ErrorText(v.error()));
    case sheet::Kind::Array:
      return "{array}";
  }
  return "";
}

struct Book {
  sheet::Workbook wb;
  int sheet = 0;

  Book() {
    sheet = wb.AddSheet("Sheet1");
    wb.SetNow(46000.5);
  }

  static void Pos(std::string_view cell, int& row, int& col) {
    if (!sheet::ParseCellName(cell, row, col)) {
      row = -1;
      col = -1;
    }
  }

  bool Set(std::string_view cell, std::string_view input, int s = -1) {
    int row;
    int col;
    Pos(cell, row, col);
    std::string error;
    return wb.SetInput(s < 0 ? sheet : s, row, col, input, &error);
  }

  sheet::Value Get(std::string_view cell, int s = -1) {
    wb.Recalc();
    int row;
    int col;
    Pos(cell, row, col);
    return wb.Get(s < 0 ? sheet : s, row, col);
  }

  std::string Show(std::string_view cell, int s = -1) { return Display(Get(cell, s)); }

  double Num(std::string_view cell, int s = -1) {
    sheet::Value v = Get(cell, s);
    return v.IsNumber() ? v.number() : -999999.0;
  }

  std::string Formula(std::string_view cell, int s = -1) {
    int row;
    int col;
    Pos(cell, row, col);
    return wb.FormulaText(s < 0 ? sheet : s, row, col);
  }

  std::string Eval(std::string_view formula) {
    std::string text =
        formula.empty() || formula[0] != '=' ? "=" + std::string(formula) : std::string(formula);
    if (!Set("ZZ900", text)) return "parse error";
    std::string out = Show("ZZ900");
    wb.Clear(sheet, 899, 701);
    return out;
  }
};

}
