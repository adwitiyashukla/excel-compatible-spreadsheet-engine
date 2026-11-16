#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "eval.h"
#include "formula.h"

namespace sheet {

namespace {

enum class T : uint8_t {
  End,
  Num,
  Str,
  Bool,
  ErrLit,
  Ref,
  RefErr,
  Name,
  Func,
  Op,
  Colon,
  LParen,
  RParen,
  Comma,
  Semi,
  LBrace,
  RBrace,
};

struct Token {
  T t = T::End;
  bool space = false;
  size_t pos = 0;
  double num = 0;
  std::string text;
  RefSpec ref;
  Err err = Err::None;
  bool b = false;
  int scope = -1;
};

bool IsWordStart(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '\\' || c == '$' ||
         static_cast<unsigned char>(c) >= 0x80;
}

bool IsWordChar(char c) {
  return IsWordStart(c) || (c >= '0' && c <= '9') || c == '.' || c == '?';
}

bool ParseCellPart(std::string_view w, int& row, int& col, bool& row_abs, bool& col_abs) {
  size_t i = 0;
  col_abs = false;
  row_abs = false;
  if (i < w.size() && w[i] == '$') {
    col_abs = true;
    ++i;
  }
  size_t letters = i;
  while (i < w.size() && ((w[i] >= 'A' && w[i] <= 'Z') || (w[i] >= 'a' && w[i] <= 'z'))) ++i;
  if (i == letters || i - letters > 3) return false;
  if (!ParseColumn(w.substr(letters, i - letters), col)) return false;
  if (i < w.size() && w[i] == '$') {
    row_abs = true;
    ++i;
  }
  size_t digits = i;
  long n = 0;
  while (i < w.size() && w[i] >= '0' && w[i] <= '9') {
    n = n * 10 + (w[i] - '0');
    if (n > kMaxRows) return false;
    ++i;
  }
  if (i == digits || i != w.size() || n < 1) return false;
  row = static_cast<int>(n - 1);
  return true;
}

bool ParseColPart(std::string_view w, int& col, bool& abs) {
  size_t i = 0;
  abs = false;
  if (i < w.size() && w[i] == '$') {
    abs = true;
    ++i;
  }
  if (i >= w.size()) return false;
  return ParseColumn(w.substr(i), col);
}

bool ParseRowPart(std::string_view w, int& row, bool& abs) {
  size_t i = 0;
  abs = false;
  if (i < w.size() && w[i] == '$') {
    abs = true;
    ++i;
  }
  if (i >= w.size()) return false;
  long n = 0;
  for (; i < w.size(); ++i) {
    if (w[i] < '0' || w[i] > '9') return false;
    n = n * 10 + (w[i] - '0');
    if (n > kMaxRows) return false;
  }
  if (n < 1) return false;
  row = static_cast<int>(n - 1);
  return true;
}

class Lexer {
 public:
  Lexer(std::string_view s, const SheetResolver& sheets, const TableResolver* tables, int row,
        int col)
      : s_(s), sheets_(sheets), tables_(tables), row_(row), col_(col) {}

  bool Next(Token& tok, std::string& error) {
    tok = Token();
    bool space = false;
    while (i_ < s_.size() &&
           (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) {
      ++i_;
      space = true;
    }
    tok.space = space;
    tok.pos = i_;
    if (i_ >= s_.size()) {
      tok.t = T::End;
      return true;
    }
    char c = s_[i_];
    if (c == '"') return LexString(tok, error);
    if (c == '#') return LexError(tok, error);
    if (c == '\'') return LexQuotedSheet(tok, error);
    if ((c >= '0' && c <= '9') ||
        (c == '.' && i_ + 1 < s_.size() && s_[i_ + 1] >= '0' && s_[i_ + 1] <= '9')) {
      return LexNumberOrRows(tok, error);
    }
    if (c == '$' && i_ + 1 < s_.size() && s_[i_ + 1] >= '0' && s_[i_ + 1] <= '9') {
      return LexNumberOrRows(tok, error);
    }
    if (IsWordStart(c)) return LexWord(tok, error);
    if (c == '[' && HasTables()) return LexStructured(tok, "", error);
    ++i_;
    switch (c) {
      case '(':
        tok.t = T::LParen;
        return true;
      case ')':
        tok.t = T::RParen;
        return true;
      case ',':
        tok.t = T::Comma;
        return true;
      case ';':
        tok.t = T::Semi;
        return true;
      case '{':
        tok.t = T::LBrace;
        return true;
      case '}':
        tok.t = T::RBrace;
        return true;
      case ':':
        tok.t = T::Colon;
        return true;
      case '+':
      case '-':
      case '*':
      case '/':
      case '^':
      case '&':
      case '=':
      case '%':
      case '@':
        tok.t = T::Op;
        tok.text = std::string(1, c);
        return true;
      case '<':
        tok.t = T::Op;
        if (i_ < s_.size() && (s_[i_] == '=' || s_[i_] == '>')) {
          tok.text = std::string("<") + s_[i_];
          ++i_;
        } else {
          tok.text = "<";
        }
        return true;
      case '>':
        tok.t = T::Op;
        if (i_ < s_.size() && s_[i_] == '=') {
          tok.text = ">=";
          ++i_;
        } else {
          tok.text = ">";
        }
        return true;
      default:
        error = std::string("unexpected character '") + c + "'";
        return false;
    }
  }

  size_t pos() const { return i_; }

 private:
  bool LexString(Token& tok, std::string& error) {
    ++i_;
    std::string out;
    while (true) {
      if (i_ >= s_.size()) {
        error = "unterminated string";
        return false;
      }
      char c = s_[i_++];
      if (c == '"') {
        if (i_ < s_.size() && s_[i_] == '"') {
          out.push_back('"');
          ++i_;
          continue;
        }
        break;
      }
      out.push_back(c);
    }
    tok.t = T::Str;
    tok.text = std::move(out);
    return true;
  }

  bool LexError(Token& tok, std::string& error) {
    static const char* kTexts[] = {"#NULL!", "#DIV/0!", "#VALUE!",       "#REF!",   "#NAME?",
                                   "#NUM!",  "#N/A",    "#GETTING_DATA", "#SPILL!", "#CALC!"};
    for (const char* t : kTexts) {
      size_t n = std::strlen(t);
      if (i_ + n <= s_.size() && EqualsText(s_.substr(i_, n), t)) {
        Err e;
        ParseErrorText(t, e);
        i_ += n;
        if (e == Err::Ref) {
          tok.t = T::RefErr;
          return true;
        }
        tok.t = T::ErrLit;
        tok.err = e;
        return true;
      }
    }
    error = "unknown error literal";
    return false;
  }

  bool LexNumberOrRows(Token& tok, std::string& error) {
    size_t start = i_;
    size_t j = i_;
    if (s_[j] == '$') ++j;
    size_t dstart = j;
    while (j < s_.size() && s_[j] >= '0' && s_[j] <= '9') ++j;
    if (j < s_.size() && s_[j] == ':' && j > dstart) {
      size_t k = j + 1;
      if (k < s_.size() && s_[k] == '$') ++k;
      size_t d2 = k;
      while (k < s_.size() && s_[k] >= '0' && s_[k] <= '9') ++k;
      if (k > d2 && (k >= s_.size() || !IsWordChar(s_[k]))) {
        int r1;
        int r2;
        bool a1;
        bool a2;
        if (ParseRowPart(s_.substr(start, j - start), r1, a1) &&
            ParseRowPart(s_.substr(j + 1, k - j - 1), r2, a2)) {
          tok.t = T::Ref;
          tok.ref.area = true;
          tok.ref.full_rows = true;
          tok.ref.r1 = r1;
          tok.ref.r2 = r2;
          tok.ref.r1_abs = a1;
          tok.ref.r2_abs = a2;
          tok.ref.c1 = 0;
          tok.ref.c2 = kMaxCols - 1;
          tok.ref.c1_abs = true;
          tok.ref.c2_abs = true;
          i_ = k;
          return true;
        }
      }
    }
    if (s_[start] == '$') {
      error = "bad row reference";
      return false;
    }
    j = start;
    while (j < s_.size() && s_[j] >= '0' && s_[j] <= '9') ++j;
    if (j < s_.size() && s_[j] == '.') {
      ++j;
      while (j < s_.size() && s_[j] >= '0' && s_[j] <= '9') ++j;
    }
    if (j < s_.size() && (s_[j] == 'e' || s_[j] == 'E')) {
      size_t k = j + 1;
      if (k < s_.size() && (s_[k] == '+' || s_[k] == '-')) ++k;
      size_t ed = k;
      while (k < s_.size() && s_[k] >= '0' && s_[k] <= '9') ++k;
      if (k > ed) j = k;
    }
    double v;
    if (!ParseNumberLiteral(s_.substr(start, j - start), v)) {
      error = "bad number";
      return false;
    }
    tok.t = T::Num;
    tok.num = v;
    i_ = j;
    return true;
  }

  bool LexQuotedSheet(Token& tok, std::string& error) {
    ++i_;
    std::string name;
    while (true) {
      if (i_ >= s_.size()) {
        error = "unterminated sheet name";
        return false;
      }
      char c = s_[i_++];
      if (c == '\'') {
        if (i_ < s_.size() && s_[i_] == '\'') {
          name.push_back('\'');
          ++i_;
          continue;
        }
        break;
      }
      name.push_back(c);
    }
    if (i_ >= s_.size() || s_[i_] != '!') {
      error = "expected ! after sheet name";
      return false;
    }
    ++i_;
    size_t colon = name.find(':');
    if (colon != std::string::npos && sheets_ && sheets_(name) < 0) {
      return LexAfterSheet(tok, name.substr(0, colon), name.substr(colon + 1), error);
    }
    return LexAfterSheet(tok, name, "", error);
  }

  bool LexAfterSheet(Token& tok, const std::string& sheet_name, const std::string& last_sheet,
                     std::string& error) {
    int sheet = sheets_ ? sheets_(sheet_name) : -1;
    int sheet2 = -1;
    if (!last_sheet.empty()) {
      sheet2 = sheets_ ? sheets_(last_sheet) : -1;
      if (sheet2 < 0) sheet = -1;
    }
    if (i_ < s_.size() && s_[i_] == '#') {
      if (!LexError(tok, error)) return false;
      if (tok.t != T::RefErr) {
        error = "bad reference";
        return false;
      }
      return true;
    }
    size_t start = i_;
    if (i_ < s_.size() && (s_[i_] == '$' || (s_[i_] >= '0' && s_[i_] <= '9'))) {
      size_t save = i_;
      if (LexNumberOrRows(tok, error) && tok.t == T::Ref) {
        ApplySheet(tok, sheet, sheet2);
        return true;
      }
      i_ = save;
      error.clear();
    }
    while (i_ < s_.size() && IsWordChar(s_[i_])) ++i_;
    std::string_view word = s_.substr(start, i_ - start);
    if (word.empty()) {
      error = "expected reference after sheet name";
      return false;
    }
    if (!LexRefWord(tok, word)) {
      tok.t = T::Name;
      tok.text = std::string(word);
      tok.scope = sheet;
      if (sheet < 0 || sheet2 >= 0) {
        error = "unknown sheet";
        return false;
      }
      return true;
    }
    ApplySheet(tok, sheet, sheet2);
    return true;
  }

  void ApplySheet(Token& tok, int sheet, int sheet2) {
    if (sheet < 0) {
      tok = Token();
      tok.t = T::RefErr;
      return;
    }
    if (sheet2 >= 0 && sheet2 < sheet) std::swap(sheet, sheet2);
    tok.ref.sheet = static_cast<int16_t>(sheet);
    tok.ref.sheet2 = static_cast<int16_t>(sheet2 > sheet ? sheet2 : -1);
    tok.ref.explicit_sheet = true;
  }

  bool LexRefWord(Token& tok, std::string_view word) {
    int row;
    int col;
    bool ra;
    bool ca;
    if (ParseCellPart(word, row, col, ra, ca)) {
      tok.t = T::Ref;
      tok.ref.r1 = tok.ref.r2 = row;
      tok.ref.c1 = tok.ref.c2 = col;
      tok.ref.r1_abs = tok.ref.r2_abs = ra;
      tok.ref.c1_abs = tok.ref.c2_abs = ca;
      if (i_ < s_.size() && s_[i_] == ':') {
        size_t k = i_ + 1;
        while (k < s_.size() && IsWordChar(s_[k])) ++k;
        int row2;
        int col2;
        bool ra2;
        bool ca2;
        if (ParseCellPart(s_.substr(i_ + 1, k - i_ - 1), row2, col2, ra2, ca2)) {
          tok.ref.area = true;
          tok.ref.r2 = row2;
          tok.ref.c2 = col2;
          tok.ref.r2_abs = ra2;
          tok.ref.c2_abs = ca2;
          i_ = k;
          if (tok.ref.r1 > tok.ref.r2) {
            std::swap(tok.ref.r1, tok.ref.r2);
            std::swap(tok.ref.r1_abs, tok.ref.r2_abs);
          }
          if (tok.ref.c1 > tok.ref.c2) {
            std::swap(tok.ref.c1, tok.ref.c2);
            std::swap(tok.ref.c1_abs, tok.ref.c2_abs);
          }
        }
      }
      return true;
    }
    int c1;
    bool a1;
    if (ParseColPart(word, c1, a1) && i_ < s_.size() && s_[i_] == ':') {
      size_t k = i_ + 1;
      while (k < s_.size() && IsWordChar(s_[k])) ++k;
      int c2;
      bool a2;
      if (ParseColPart(s_.substr(i_ + 1, k - i_ - 1), c2, a2)) {
        tok.t = T::Ref;
        tok.ref.area = true;
        tok.ref.full_cols = true;
        if (c1 > c2) {
          std::swap(c1, c2);
          std::swap(a1, a2);
        }
        tok.ref.c1 = c1;
        tok.ref.c2 = c2;
        tok.ref.c1_abs = a1;
        tok.ref.c2_abs = a2;
        tok.ref.r1 = 0;
        tok.ref.r2 = kMaxRows - 1;
        tok.ref.r1_abs = tok.ref.r2_abs = true;
        i_ = k;
        return true;
      }
    }
    return false;
  }

  bool HasTables() const { return tables_ && *tables_; }

  static std::string Unescape(std::string_view in) {
    std::string out;
    for (size_t k = 0; k < in.size(); ++k) {
      if (in[k] == '\'' && k + 1 < in.size()) ++k;
      out.push_back(in[k]);
    }
    return out;
  }

  static std::string_view TrimSpaces(std::string_view v) {
    while (!v.empty() && v.front() == ' ') v.remove_prefix(1);
    while (!v.empty() && v.back() == ' ') v.remove_suffix(1);
    return v;
  }

  struct TableSpec {
    bool all = false;
    bool data = false;
    bool headers = false;
    bool totals = false;
    bool this_row = false;
    std::string first;
    std::string last;
  };

  static bool Keyword(std::string_view item, TableSpec& spec) {
    std::string k;
    for (char ch : item) {
      if (ch != ' ') k.push_back(static_cast<char>(ch >= 'a' && ch <= 'z' ? ch - 32 : ch));
    }
    if (k == "#ALL") {
      spec.all = true;
    } else if (k == "#DATA") {
      spec.data = true;
    } else if (k == "#HEADERS") {
      spec.headers = true;
    } else if (k == "#TOTALS") {
      spec.totals = true;
    } else if (k == "#THISROW") {
      spec.this_row = true;
    } else {
      return false;
    }
    return true;
  }

  static bool ParseItems(std::string_view in, TableSpec& spec, bool columns_only) {
    size_t k = 0;
    bool range = false;
    while (true) {
      while (k < in.size() && in[k] == ' ') ++k;
      if (k >= in.size() || in[k] != '[') return false;
      size_t j = k + 1;
      while (j < in.size() && in[j] != ']') j += in[j] == '\'' ? 2 : 1;
      if (j >= in.size()) return false;
      std::string_view item = in.substr(k + 1, j - k - 1);
      k = j + 1;
      std::string_view trimmed = TrimSpaces(item);
      if (!trimmed.empty() && trimmed[0] == '#') {
        if (columns_only || range || !Keyword(trimmed, spec)) return false;
      } else if (range) {
        spec.last = Unescape(item);
        range = false;
      } else {
        if (!spec.first.empty()) return false;
        spec.first = Unescape(item);
        spec.last = spec.first;
      }
      while (k < in.size() && in[k] == ' ') ++k;
      if (k >= in.size()) return !range;
      if (in[k] == ':' && !range && !spec.first.empty() && spec.first == spec.last) {
        range = true;
        ++k;
        continue;
      }
      if (in[k] != ',') return false;
      ++k;
    }
  }

  static bool ParseTableSpec(std::string_view inner, TableSpec& spec) {
    inner = TrimSpaces(inner);
    if (inner.empty()) return true;
    if (inner[0] == '@') {
      spec.this_row = true;
      std::string_view rest = TrimSpaces(inner.substr(1));
      if (rest.empty()) return true;
      if (rest[0] == '[') return ParseItems(rest, spec, true);
      spec.first = spec.last = Unescape(rest);
      return true;
    }
    if (inner[0] == '#') return Keyword(inner, spec);
    if (inner[0] == '[') return ParseItems(inner, spec, false);
    spec.first = spec.last = Unescape(inner);
    return true;
  }

  bool LexStructured(Token& tok, std::string_view table, std::string& error) {
    size_t start = i_;
    int depth = 0;
    size_t k = i_;
    for (; k < s_.size(); ++k) {
      char ch = s_[k];
      if (ch == '\'' && k + 1 < s_.size()) {
        ++k;
        continue;
      }
      if (ch == '[') {
        ++depth;
      } else if (ch == ']' && --depth == 0) {
        break;
      }
    }
    if (k >= s_.size()) {
      error = "unterminated structured reference";
      return false;
    }
    std::string_view inner = s_.substr(start + 1, k - start - 1);
    i_ = k + 1;
    TableSpec spec;
    if (!ParseTableSpec(inner, spec)) {
      error = "bad structured reference";
      return false;
    }
    return ResolveTable(tok, table, spec);
  }

  bool ResolveTable(Token& tok, std::string_view table, const TableSpec& spec) {
    TableRef t;
    if (!HasTables() || !(*tables_)(table, row_, col_, t)) {
      tok.t = T::RefErr;
      return true;
    }
    int data_r1 = t.r1 + t.header_rows;
    int data_r2 = t.r2 - t.totals_rows;
    int r1 = data_r1;
    int r2 = data_r2;
    bool relative = false;
    if (spec.this_row) {
      if (row_ < data_r1 || row_ > data_r2) {
        tok.t = T::ErrLit;
        tok.err = Err::Value;
        return true;
      }
      r1 = r2 = row_;
      relative = true;
    } else if (spec.all) {
      r1 = t.r1;
      r2 = t.r2;
    } else if (spec.headers || spec.totals) {
      if ((spec.headers && t.header_rows == 0) || (spec.totals && t.totals_rows == 0)) {
        tok.t = T::RefErr;
        return true;
      }
      r1 = spec.headers ? t.r1 : (spec.data ? data_r1 : t.r2);
      r2 = spec.totals ? t.r2 : (spec.data ? data_r2 : t.r1);
    }
    int c1 = t.c1;
    int c2 = t.c2;
    if (!spec.first.empty()) {
      int a = -1;
      int b = -1;
      auto find = [&](const std::string& want) {
        for (size_t i = 0; i < t.columns.size(); ++i) {
          if (EqualsText(t.columns[i], want)) return static_cast<int>(i);
        }
        for (size_t i = 0; i < t.columns.size(); ++i) {
          if (EqualsText(TrimSpaces(t.columns[i]), TrimSpaces(want))) return static_cast<int>(i);
        }
        return -1;
      };
      a = find(spec.first);
      b = find(spec.last);
      if (a < 0 || b < 0) {
        tok.t = T::RefErr;
        return true;
      }
      if (a > b) std::swap(a, b);
      c1 = t.c1 + a;
      c2 = t.c1 + b;
    }
    tok.t = T::Ref;
    tok.ref.r1 = r1;
    tok.ref.r2 = r2;
    tok.ref.c1 = c1;
    tok.ref.c2 = c2;
    tok.ref.r1_abs = tok.ref.r2_abs = !relative;
    tok.ref.c1_abs = tok.ref.c2_abs = true;
    tok.ref.area = r1 != r2 || c1 != c2;
    tok.ref.sheet = static_cast<int16_t>(t.sheet);
    tok.ref.explicit_sheet = true;
    return true;
  }

  bool LexWord(Token& tok, std::string& error) {
    size_t start = i_;
    while (i_ < s_.size() && IsWordChar(s_[i_])) ++i_;
    std::string_view word = s_.substr(start, i_ - start);
    if (i_ < s_.size() && s_[i_] == '[' && HasTables()) return LexStructured(tok, word, error);
    if (i_ < s_.size() && s_[i_] == '!') {
      ++i_;
      return LexAfterSheet(tok, std::string(word), "", error);
    }
    if (i_ < s_.size() && s_[i_] == ':' && sheets_ && sheets_(word) >= 0) {
      size_t k = i_ + 1;
      while (k < s_.size() && IsWordChar(s_[k])) ++k;
      if (k < s_.size() && s_[k] == '!' && k > i_ + 1) {
        std::string last(s_.substr(i_ + 1, k - i_ - 1));
        i_ = k + 1;
        return LexAfterSheet(tok, std::string(word), last, error);
      }
    }
    if (i_ < s_.size() && s_[i_] == '(') {
      std::string name = ToUpper(word);
      for (const char* prefix : {"_XLFN._XLWS.", "_XLFN.", "_XLWS.", "COM.MICROSOFT."}) {
        size_t n = std::strlen(prefix);
        if (name.size() > n && name.compare(0, n, prefix) == 0) name = name.substr(n);
      }
      tok.t = T::Func;
      tok.text = std::move(name);
      return true;
    }
    if (EqualsText(word, "TRUE") || EqualsText(word, "FALSE")) {
      tok.t = T::Bool;
      tok.b = EqualsText(word, "TRUE");
      return true;
    }
    if (LexRefWord(tok, word)) return true;
    if (word.find('$') != std::string_view::npos) {
      error = "bad reference";
      return false;
    }
    TableRef t;
    if (HasTables() && (*tables_)(word, row_, col_, t)) return ResolveTable(tok, word, TableSpec{});
    tok.t = T::Name;
    tok.text = std::string(word);
    return true;
  }

  std::string_view s_;
  const SheetResolver& sheets_;
  const TableResolver* tables_;
  int row_;
  int col_;
  size_t i_ = 0;
};

struct Node {
  enum Kind : uint8_t {
    Num,
    Str,
    Bool,
    ErrLit,
    Missing,
    Array,
    Ref,
    RefErr,
    Name,
    Unary,
    Binary,
    Percent,
    Paren,
    Union,
    Call,
  };
  Kind kind = Num;
  Op op = Op::Add;
  double num = 0;
  Value val;
  RefSpec ref;
  std::string name;
  int func = -1;
  int scope = -1;
  std::vector<int> kids;
};

class Parser {
 public:
  Parser(std::string_view text, const SheetResolver& sheets, const TableResolver* tables, int row,
         int col)
      : lex_(text, sheets, tables, row, col) {}

  bool Parse(std::vector<Node>& nodes, int& root, std::string& error) {
    nodes_ = &nodes;
    if (!Advance(error)) return false;
    if (!ParseExpr(root, error)) return false;
    if (tok_.t != T::End) {
      error = "unexpected token after expression";
      return false;
    }
    return true;
  }

  size_t pos() const { return tok_.pos; }

 private:
  bool Advance(std::string& error) { return lex_.Next(tok_, error); }

  int Add(Node n) {
    nodes_->push_back(std::move(n));
    return static_cast<int>(nodes_->size() - 1);
  }

  bool IsOp(const char* s) const { return tok_.t == T::Op && tok_.text == s; }

  bool ParseExpr(int& out, std::string& error) {
    if (++depth_ > 200) {
      error = "formula nested too deeply";
      return false;
    }
    bool ok = ParseCompare(out, error);
    --depth_;
    return ok;
  }

  bool ParseCompare(int& out, std::string& error) {
    if (!ParseConcat(out, error)) return false;
    while (tok_.t == T::Op && (tok_.text == "=" || tok_.text == "<>" || tok_.text == "<" ||
                               tok_.text == "<=" || tok_.text == ">" || tok_.text == ">=")) {
      Op op = tok_.text == "="    ? Op::Eq
              : tok_.text == "<>" ? Op::Ne
              : tok_.text == "<"  ? Op::Lt
              : tok_.text == "<=" ? Op::Le
              : tok_.text == ">"  ? Op::Gt
                                  : Op::Ge;
      if (!Advance(error)) return false;
      int rhs;
      if (!ParseConcat(rhs, error)) return false;
      out = Binary(op, out, rhs);
    }
    return true;
  }

  bool ParseConcat(int& out, std::string& error) {
    if (!ParseAdd(out, error)) return false;
    while (IsOp("&")) {
      if (!Advance(error)) return false;
      int rhs;
      if (!ParseAdd(rhs, error)) return false;
      out = Binary(Op::Concat, out, rhs);
    }
    return true;
  }

  bool ParseAdd(int& out, std::string& error) {
    if (!ParseMul(out, error)) return false;
    while (IsOp("+") || IsOp("-")) {
      Op op = tok_.text == "+" ? Op::Add : Op::Sub;
      if (!Advance(error)) return false;
      int rhs;
      if (!ParseMul(rhs, error)) return false;
      out = Binary(op, out, rhs);
    }
    return true;
  }

  bool ParseMul(int& out, std::string& error) {
    if (!ParsePow(out, error)) return false;
    while (IsOp("*") || IsOp("/")) {
      Op op = tok_.text == "*" ? Op::Mul : Op::Div;
      if (!Advance(error)) return false;
      int rhs;
      if (!ParsePow(rhs, error)) return false;
      out = Binary(op, out, rhs);
    }
    return true;
  }

  bool ParsePow(int& out, std::string& error) {
    if (!ParsePercent(out, error)) return false;
    while (IsOp("^")) {
      if (!Advance(error)) return false;
      int rhs;
      if (!ParsePercent(rhs, error)) return false;
      out = Binary(Op::Pow, out, rhs);
    }
    return true;
  }

  bool ParsePercent(int& out, std::string& error) {
    if (!ParseUnary(out, error)) return false;
    while (IsOp("%")) {
      if (!Advance(error)) return false;
      Node n;
      n.kind = Node::Percent;
      n.kids = {out};
      out = Add(std::move(n));
    }
    return true;
  }

  bool ParseUnary(int& out, std::string& error) {
    if (IsOp("-") || IsOp("+") || IsOp("@")) {
      Op op = tok_.text == "-" ? Op::Neg : Op::Plus;
      if (++depth_ > 200) {
        error = "formula nested too deeply";
        return false;
      }
      if (!Advance(error)) return false;
      int inner;
      bool ok = ParseUnary(inner, error);
      --depth_;
      if (!ok) return false;
      Node n;
      n.kind = Node::Unary;
      n.op = op;
      n.kids = {inner};
      out = Add(std::move(n));
      return true;
    }
    return ParseIntersect(out, error);
  }

  bool StartsOperand() const {
    return tok_.t == T::Ref || tok_.t == T::Name || tok_.t == T::Func || tok_.t == T::LParen ||
           tok_.t == T::RefErr;
  }

  bool RefLike(int n) const {
    Node::Kind k = (*nodes_)[n].kind;
    return k == Node::Ref || k == Node::Name || k == Node::Call || k == Node::Paren ||
           k == Node::Union || k == Node::RefErr ||
           (k == Node::Binary &&
            ((*nodes_)[n].op == Op::Range || (*nodes_)[n].op == Op::Intersect));
  }

  bool ParseIntersect(int& out, std::string& error) {
    if (!ParseRange(out, error)) return false;
    while (tok_.space && StartsOperand() && RefLike(out)) {
      int rhs;
      if (!ParseRange(rhs, error)) return false;
      out = Binary(Op::Intersect, out, rhs);
    }
    return true;
  }

  bool ParseRange(int& out, std::string& error) {
    if (!ParsePrimary(out, error)) return false;
    while (tok_.t == T::Colon) {
      if (!Advance(error)) return false;
      int rhs;
      if (!ParsePrimary(rhs, error)) return false;
      out = Binary(Op::Range, out, rhs);
    }
    return true;
  }

  int Binary(Op op, int l, int r) {
    Node n;
    n.kind = Node::Binary;
    n.op = op;
    n.kids = {l, r};
    return Add(std::move(n));
  }

  bool ParsePrimary(int& out, std::string& error) {
    Node n;
    switch (tok_.t) {
      case T::Num:
        n.kind = Node::Num;
        n.num = tok_.num;
        out = Add(std::move(n));
        return Advance(error);
      case T::Str:
        n.kind = Node::Str;
        n.val = Value::Text(tok_.text);
        out = Add(std::move(n));
        return Advance(error);
      case T::Bool:
        n.kind = Node::Bool;
        n.num = tok_.b ? 1 : 0;
        out = Add(std::move(n));
        return Advance(error);
      case T::ErrLit:
        n.kind = Node::ErrLit;
        n.val = Value::Error(tok_.err);
        out = Add(std::move(n));
        return Advance(error);
      case T::Ref:
        n.kind = Node::Ref;
        n.ref = tok_.ref;
        out = Add(std::move(n));
        return Advance(error);
      case T::RefErr:
        n.kind = Node::RefErr;
        out = Add(std::move(n));
        return Advance(error);
      case T::Name:
        n.kind = Node::Name;
        n.name = tok_.text;
        n.scope = tok_.scope;
        out = Add(std::move(n));
        return Advance(error);
      case T::Func:
        return ParseCall(out, error);
      case T::LParen: {
        if (!Advance(error)) return false;
        std::vector<int> items;
        int first;
        if (!ParseExpr(first, error)) return false;
        items.push_back(first);
        while (tok_.t == T::Comma) {
          if (!Advance(error)) return false;
          int next;
          if (!ParseExpr(next, error)) return false;
          items.push_back(next);
        }
        if (tok_.t != T::RParen) {
          error = "expected )";
          return false;
        }
        n.kind = items.size() == 1 ? Node::Paren : Node::Union;
        n.kids = std::move(items);
        out = Add(std::move(n));
        return Advance(error);
      }
      case T::LBrace:
        return ParseArray(out, error);
      case T::End:
        error = "unexpected end of formula";
        return false;
      default:
        error = "unexpected token";
        return false;
    }
  }

  bool ParseCall(int& out, std::string& error) {
    Node n;
    n.kind = Node::Call;
    n.name = tok_.text;
    n.func = FindFunction(n.name);
    if (!Advance(error)) return false;
    if (tok_.t != T::LParen) {
      error = "expected ( after function name";
      return false;
    }
    if (!Advance(error)) return false;
    if (tok_.t == T::RParen) {
      out = Add(std::move(n));
      return Advance(error);
    }
    while (true) {
      if (tok_.t == T::Comma || tok_.t == T::RParen) {
        Node m;
        m.kind = Node::Missing;
        n.kids.push_back(Add(std::move(m)));
      } else {
        int arg;
        if (!ParseExpr(arg, error)) return false;
        n.kids.push_back(arg);
      }
      if (tok_.t == T::Comma) {
        if (!Advance(error)) return false;
        continue;
      }
      if (tok_.t == T::RParen) break;
      error = "expected , or ) in argument list";
      return false;
    }
    if (n.kids.size() > 255) {
      error = "too many arguments";
      return false;
    }
    out = Add(std::move(n));
    return Advance(error);
  }

  bool ParseArray(int& out, std::string& error) {
    if (!Advance(error)) return false;
    std::vector<std::vector<Value>> rows(1);
    while (true) {
      Value v;
      bool neg = false;
      while (IsOp("-") || IsOp("+")) {
        if (tok_.text == "-") neg = !neg;
        if (!Advance(error)) return false;
      }
      if (tok_.t == T::Num) {
        v = Value::Number(neg ? -tok_.num : tok_.num);
      } else if (tok_.t == T::Str && !neg) {
        v = Value::Text(tok_.text);
      } else if (tok_.t == T::Bool && !neg) {
        v = Value::Bool(tok_.b);
      } else if (tok_.t == T::ErrLit && !neg) {
        v = Value::Error(tok_.err);
      } else if (tok_.t == T::RefErr && !neg) {
        v = Value::Error(Err::Ref);
      } else {
        error = "bad array constant";
        return false;
      }
      rows.back().push_back(std::move(v));
      if (!Advance(error)) return false;
      if (tok_.t == T::Comma) {
        if (!Advance(error)) return false;
        continue;
      }
      if (tok_.t == T::Semi) {
        rows.emplace_back();
        if (!Advance(error)) return false;
        continue;
      }
      if (tok_.t == T::RBrace) break;
      error = "expected , ; or } in array constant";
      return false;
    }
    size_t cols = rows[0].size();
    for (const auto& r : rows) {
      if (r.size() != cols) {
        error = "array rows have different lengths";
        return false;
      }
    }
    Value arr = Value::Array(static_cast<uint32_t>(rows.size()), static_cast<uint32_t>(cols));
    for (size_t r = 0; r < rows.size(); ++r) {
      for (size_t c = 0; c < cols; ++c) {
        arr.mut(static_cast<uint32_t>(r), static_cast<uint32_t>(c)) = rows[r][c];
      }
    }
    Node n;
    n.kind = Node::Array;
    n.val = std::move(arr);
    out = Add(std::move(n));
    return Advance(error);
  }

  Lexer lex_;
  Token tok_;
  std::vector<Node>* nodes_ = nullptr;
  int depth_ = 0;
};

class Compiler {
 public:
  Compiler(const std::vector<Node>& nodes, int base_row, int base_col, Program& p)
      : nodes_(nodes), base_row_(base_row), base_col_(base_col), p_(p) {}

  bool Compile(int n, uint8_t mode, std::string& error) {
    const Node& node = nodes_[n];
    switch (node.kind) {
      case Node::Num:
        p_.nums.push_back(node.num);
        Emit(Op::PushNum, mode, 0, static_cast<uint32_t>(p_.nums.size() - 1), 1);
        return true;
      case Node::Str:
      case Node::Array:
        p_.consts.push_back(node.val);
        Emit(node.kind == Node::Str ? Op::PushStr : Op::PushArray, mode, 0,
             static_cast<uint32_t>(p_.consts.size() - 1), 1);
        return true;
      case Node::Bool:
        Emit(Op::PushBool, mode, 0, node.num != 0 ? 1 : 0, 1);
        return true;
      case Node::ErrLit:
        Emit(Op::PushErr, mode, 0, static_cast<uint32_t>(node.val.error()), 1);
        return true;
      case Node::Missing:
        Emit(Op::PushMissing, mode, 0, 0, 1);
        return true;
      case Node::RefErr:
        Emit(Op::PushRefErr, mode, 0, 0, 1);
        return true;
      case Node::Ref: {
        RefSpec r = node.ref;
        if (!r.r1_abs) r.r1 -= base_row_;
        if (!r.r2_abs) r.r2 -= base_row_;
        if (!r.c1_abs) r.c1 -= base_col_;
        if (!r.c2_abs) r.c2 -= base_col_;
        p_.refs.push_back(r);
        Emit(Op::PushRef, mode, 0, static_cast<uint32_t>(p_.refs.size() - 1), 1);
        return true;
      }
      case Node::Name: {
        std::string key = node.name;
        if (node.scope >= 0) key = std::to_string(node.scope) + "!" + key;
        p_.names.push_back(key);
        p_.uses_names = true;
        Emit(Op::PushName, mode, 0, static_cast<uint32_t>(p_.names.size() - 1), 1);
        return true;
      }
      case Node::Unary:
        if (!Compile(node.kids[0], mode, error)) return false;
        Emit(node.op, mode, 0, 0, 0);
        return true;
      case Node::Percent:
        if (!Compile(node.kids[0], mode, error)) return false;
        Emit(Op::Percent, mode, 0, 0, 0);
        return true;
      case Node::Paren:
        if (!Compile(node.kids[0], mode, error)) return false;
        Emit(Op::Paren, mode, 0, 0, 0);
        return true;
      case Node::Binary:
        if (!Compile(node.kids[0], mode, error)) return false;
        if (!Compile(node.kids[1], mode, error)) return false;
        Emit(node.op, mode, 0, 0, -1);
        return true;
      case Node::Union:
        for (size_t i = 0; i < node.kids.size(); ++i) {
          if (!Compile(node.kids[i], mode, error)) return false;
          if (i > 0) Emit(Op::Union, mode, 0, 0, -1);
        }
        return true;
      case Node::Call:
        return CompileCall(node, mode, error);
    }
    return false;
  }

  void Finish() { p_.max_stack = max_depth_; }

 private:
  void Emit(Op op, uint8_t mode, uint16_t b, uint32_t a, int delta) {
    p_.code.push_back(Instr{op, mode, b, a});
    depth_ += delta;
    if (depth_ > max_depth_) max_depth_ = depth_;
  }

  bool CompileCall(const Node& node, uint8_t mode, std::string& error) {
    int argc = static_cast<int>(node.kids.size());
    const std::string& name = node.name;
    if (name == "IF") {
      if (argc < 2 || argc > 3) {
        error = "IF takes 2 or 3 arguments";
        return false;
      }
      if (!Compile(node.kids[0], mode, error)) return false;
      size_t at_if = p_.code.size();
      Emit(Op::If, mode, static_cast<uint16_t>(argc), 0, -1);
      if (!Compile(node.kids[1], mode, error)) return false;
      size_t at_else = p_.code.size();
      Emit(Op::Else, mode, 0, 0, -1);
      p_.code[at_if].a = static_cast<uint32_t>(p_.code.size());
      if (argc == 3) {
        if (!Compile(node.kids[2], mode, error)) return false;
      } else {
        Emit(Op::PushBool, mode, 0, 0, 1);
      }
      p_.code[at_else].a = static_cast<uint32_t>(p_.code.size());
      Emit(Op::EndIf, mode, static_cast<uint16_t>(argc), 0, 0);
      return true;
    }
    if (name == "IFERROR" || name == "IFNA") {
      if (argc != 2) {
        error = name + " takes 2 arguments";
        return false;
      }
      if (!Compile(node.kids[0], mode, error)) return false;
      size_t at = p_.code.size();
      Emit(Op::IfErr, mode, name == "IFNA" ? 1 : 0, 0, -1);
      if (!Compile(node.kids[1], mode, error)) return false;
      p_.code[at].a = static_cast<uint32_t>(p_.code.size());
      Emit(Op::EndIfErr, mode, name == "IFNA" ? 1 : 0, 0, 0);
      return true;
    }
    if (node.func < 0) {
      for (int i = 0; i < argc; ++i) {
        if (!Compile(node.kids[i], mode, error)) return false;
      }
      p_.names.push_back(name);
      Emit(Op::Call, mode, static_cast<uint16_t>(argc),
           0x80000000u | static_cast<uint32_t>(p_.names.size() - 1), 1 - argc);
      return true;
    }
    const FuncInfo& f = FunctionAt(node.func);
    if (argc < f.min_args || argc > f.max_args) {
      error = "wrong number of arguments to " + name;
      return false;
    }
    for (int i = 0; i < argc; ++i) {
      char cls = ParamClass(f, i);
      uint8_t m = cls == 'A' ? 1 : mode;
      if (!Compile(node.kids[i], m, error)) return false;
    }
    if (f.flags & kFnVolatile) p_.is_volatile = true;
    if (f.flags & kFnDynamic) p_.dynamic = true;
    Emit(Op::Call, mode, static_cast<uint16_t>(argc), static_cast<uint32_t>(node.func), 1 - argc);
    return true;
  }

  const std::vector<Node>& nodes_;
  int base_row_;
  int base_col_;
  Program& p_;
  int depth_ = 0;
  int max_depth_ = 0;
};

}

ProgramRef CompileFormula(std::string_view text, int base_row, int base_col,
                          const SheetResolver& sheets, bool array_mode, ParseError* err,
                          const TableResolver* tables) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\n' ||
                           text.front() == '\r')) {
    text.remove_prefix(1);
  }
  if (!text.empty() && text.front() == '=') text.remove_prefix(1);
  std::vector<Node> nodes;
  nodes.reserve(16);
  int root = -1;
  std::string error;
  Parser parser(text, sheets, tables, base_row, base_col);
  if (!parser.Parse(nodes, root, error)) {
    if (err) {
      err->message = error;
      err->pos = parser.pos();
    }
    return nullptr;
  }
  auto p = std::make_shared<Program>();
  Compiler c(nodes, base_row, base_col, *p);
  if (!c.Compile(root, array_mode ? 1 : 0, error)) {
    if (err) {
      err->message = error;
      err->pos = 0;
    }
    return nullptr;
  }
  c.Finish();
  MarkAddressRefs(*p);
  p->array_formula = array_mode;
  p->hash = HashProgram(*p);
  return p;
}

void MarkAddressRefs(Program& p) {
  static const int kIds[] = {FindFunction("ROW"), FindFunction("COLUMN"), FindFunction("ROWS"),
                             FindFunction("COLUMNS"), FindFunction("AREAS")};
  std::vector<uint8_t> use(p.refs.size(), 0);
  for (size_t k = 0; k < p.code.size(); ++k) {
    const Instr& in = p.code[k];
    if (in.op != Op::PushRef || in.a >= p.refs.size()) continue;
    bool address = false;
    if (k + 1 < p.code.size() && p.code[k + 1].op == Op::Call && p.code[k + 1].b == 1) {
      for (int id : kIds)
        address = address || (id >= 0 && p.code[k + 1].a == static_cast<uint32_t>(id));
    }
    use[in.a] |= address ? 1 : 2;
  }
  for (size_t i = 0; i < p.refs.size(); ++i) p.refs[i].address_only = use[i] == 1;
}

std::string QuoteSheetName(std::string_view name) {
  bool quote = name.empty();
  if (!quote && name[0] >= '0' && name[0] <= '9') quote = true;
  for (char c : name) {
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '.' || c == '[' || c == ']' || static_cast<unsigned char>(c) >= 0x80;
    if (!ok) quote = true;
  }
  if (!quote) {
    int r;
    int c;
    bool ra;
    bool ca;
    if (ParseCellPart(name, r, c, ra, ca)) quote = true;
    if (EqualsText(name, "TRUE") || EqualsText(name, "FALSE")) quote = true;
    if (name.size() >= 2 &&
        (name[0] == 'R' || name[0] == 'r' || name[0] == 'C' || name[0] == 'c')) {
      bool rc = true;
      for (size_t i = 1; i < name.size(); ++i) {
        char ch = name[i];
        if (!((ch >= '0' && ch <= '9') || ch == 'C' || ch == 'c')) rc = false;
      }
      if (rc) quote = true;
    }
  }
  if (!quote) return std::string(name);
  std::string out = "'";
  for (char c : name) {
    if (c == '\'') out.push_back('\'');
    out.push_back(c);
  }
  out.push_back('\'');
  return out;
}

}
