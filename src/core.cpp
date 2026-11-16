#include "core.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace sheet {

namespace {

struct ErrName {
  Err code;
  const char* text;
};

constexpr ErrName kErrNames[] = {
    {Err::Null, "#NULL!"},   {Err::Div0, "#DIV/0!"},
    {Err::Value, "#VALUE!"}, {Err::Ref, "#REF!"},
    {Err::Name, "#NAME?"},   {Err::Num, "#NUM!"},
    {Err::NA, "#N/A"},       {Err::GettingData, "#GETTING_DATA"},
    {Err::Spill, "#SPILL!"}, {Err::Calc, "#CALC!"},
};

uint32_t FoldCase(uint32_t c, bool upper) {
  if (c < 0x80) {
    if (upper && c >= 'a' && c <= 'z') return c - 32;
    if (!upper && c >= 'A' && c <= 'Z') return c + 32;
    return c;
  }
  if (c >= 0xC0 && c <= 0xFE && c != 0xD7 && c != 0xF7) {
    if (upper && c >= 0xE0) return c - 32;
    if (!upper && c <= 0xDE) return c + 32;
    return c;
  }
  if (c == 0x130) return c;
  if (c == 0x131) return upper ? 'I' : 'i';
  if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) {
    bool lower = (c & 1) != 0;
    if (upper && lower) return c - 1;
    if (!upper && !lower) return c + 1;
    return c;
  }
  if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) {
    bool lower = (c & 1) == 0;
    if (upper && lower) return c - 1;
    if (!upper && !lower) return c + 1;
    return c;
  }
  if (c == 0x178) return upper ? c : 0xFF;
  if (c == 0xFF) return upper ? 0x178 : c;
  if (c == 0x3C2) return upper ? 0x3A3 : c;
  if (c >= 0x391 && c <= 0x3AB && c != 0x3A2) return upper ? c : c + 32;
  if (c >= 0x3B1 && c <= 0x3CB && c != 0x3C2) return upper ? c - 32 : c;
  if (c == 0x386) return upper ? c : 0x3AC;
  if (c == 0x3AC) return upper ? 0x386 : c;
  if (c >= 0x388 && c <= 0x38A) return upper ? c : c + 37;
  if (c >= 0x3AD && c <= 0x3AF) return upper ? c - 37 : c;
  if (c == 0x38C) return upper ? c : 0x3CC;
  if (c == 0x3CC) return upper ? 0x38C : c;
  if (c == 0x38E || c == 0x38F) return upper ? c : c + 63;
  if (c == 0x3CD || c == 0x3CE) return upper ? c - 63 : c;
  if (c >= 0x410 && c <= 0x42F) return upper ? c : c + 32;
  if (c >= 0x430 && c <= 0x44F) return upper ? c - 32 : c;
  if (c >= 0x400 && c <= 0x40F) return upper ? c : c + 80;
  if (c >= 0x450 && c <= 0x45F) return upper ? c - 80 : c;
  return c;
}

std::string MapCase(std::string_view s, bool upper) {
  std::string out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    unsigned char b = static_cast<unsigned char>(s[i]);
    if (b < 0x80) {
      char c = s[i];
      if (upper && c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
      if (!upper && c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
      out.push_back(c);
      ++i;
      continue;
    }
    out += Utf8Encode(FoldCase(Utf8Decode(s, i), upper));
  }
  return out;
}

bool IsRepresentableInteger(double x) {
  if (x < 0) x = -x;
  if (x > 9007199254740992.0) return false;
  return x == std::floor(x);
}

void AppendExponent(std::string& out, int e) {
  if (e < 10) out.push_back('0');
  out += std::to_string(e);
}

int DecimalDigits(double d, char* digits, int keep) {
  char buf[64];
  auto res = std::to_chars(buf, buf + sizeof buf - 1, d, std::chars_format::scientific, 25);
  *res.ptr = '\0';
  char raw[32];
  int n = 0;
  raw[n++] = buf[0];
  const char* p = buf + 2;
  while (*p != 'e' && n < 26) raw[n++] = *p++;
  while (*p != 'e') ++p;
  int exponent = std::atoi(p + 1);
  for (int i = 0; i < keep; ++i) digits[i] = raw[i];
  if (raw[keep] >= '5') {
    int i = keep - 1;
    while (i >= 0 && digits[i] == '9') {
      digits[i] = '0';
      --i;
    }
    if (i < 0) {
      digits[0] = '1';
      for (int j = 1; j < keep; ++j) digits[j] = '0';
      ++exponent;
    } else {
      ++digits[i];
    }
  }
  return exponent;
}

std::string LayoutDecimal(bool negative, const char* digits, int count, int exponent) {
  std::string out;
  if (negative) out.push_back('-');
  if (exponent < 0) {
    int zeros = -exponent - 1;
    if (2 + zeros + count > 20) {
      out.push_back(digits[0]);
      if (count > 1) {
        out.push_back('.');
        out.append(digits + 1, count - 1);
      }
      out += "E-";
      AppendExponent(out, -exponent);
      return out;
    }
    out += "0.";
    out.append(zeros, '0');
    out.append(digits, count);
    return out;
  }
  if (exponent > 19) {
    out.push_back(digits[0]);
    if (count > 1) {
      out.push_back('.');
      out.append(digits + 1, count - 1);
    }
    out += "E+";
    AppendExponent(out, exponent);
    return out;
  }
  if (count > exponent + 1) {
    out.append(digits, exponent + 1);
    out.push_back('.');
    out.append(digits + exponent + 1, count - exponent - 1);
    return out;
  }
  out.append(digits, count);
  out.append(exponent + 1 - count, '0');
  return out;
}

}

std::string_view ErrorText(Err e) {
  for (const ErrName& n : kErrNames) {
    if (n.code == e) return n.text;
  }
  return "#VALUE!";
}

bool ParseErrorText(std::string_view s, Err& out) {
  for (const ErrName& n : kErrNames) {
    if (EqualsText(s, n.text)) {
      out = n.code;
      return true;
    }
  }
  return false;
}

Value Value::Text(std::string_view s) {
  void* mem = ::operator new(sizeof(TextRep) + s.size() + 1);
  TextRep* rep = new (mem) TextRep();
  rep->refs.store(1, std::memory_order_relaxed);
  rep->size = static_cast<uint32_t>(s.size());
  if (!s.empty()) std::memcpy(rep->chars(), s.data(), s.size());
  rep->chars()[s.size()] = '\0';
  Value v;
  v.kind_ = Kind::Text;
  v.text_ = rep;
  return v;
}

Value Value::Array(uint32_t rows, uint32_t cols) {
  size_t n = static_cast<size_t>(rows) * cols;
  void* mem = ::operator new(sizeof(ArrayRep) + n * sizeof(Value));
  ArrayRep* rep = new (mem) ArrayRep();
  rep->refs.store(1, std::memory_order_relaxed);
  rep->rows = rows;
  rep->cols = cols;
  rep->pad = 0;
  Value* cells = rep->cells();
  for (size_t i = 0; i < n; ++i) new (cells + i) Value();
  Value v;
  v.kind_ = Kind::Array;
  v.array_ = rep;
  return v;
}

void Value::ReleaseShared() {
  if (kind_ == Kind::Text) {
    if (text_->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      text_->~TextRep();
      ::operator delete(text_);
    }
  } else if (kind_ == Kind::Array) {
    if (array_->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      size_t n = static_cast<size_t>(array_->rows) * array_->cols;
      Value* cells = array_->cells();
      for (size_t i = 0; i < n; ++i) cells[i].~Value();
      array_->~ArrayRep();
      ::operator delete(array_);
    }
  }
  kind_ = Kind::Blank;
}

bool Value::SameAs(const Value& o) const {
  if (kind_ != o.kind_) return false;
  switch (kind_) {
    case Kind::Blank:
      return true;
    case Kind::Number:
      return num_ == o.num_;
    case Kind::Bool:
      return flag_ == o.flag_;
    case Kind::Error:
      return err_ == o.err_;
    case Kind::Text:
      return text() == o.text();
    case Kind::Array:
      if (rows() != o.rows() || cols() != o.cols()) return false;
      for (uint32_t r = 0; r < rows(); ++r) {
        for (uint32_t c = 0; c < cols(); ++c) {
          if (!at(r, c).SameAs(o.at(r, c))) return false;
        }
      }
      return true;
  }
  return false;
}

bool ApproxEqual(double a, double b) {
  constexpr double e48 = 1.0 / (16777216.0 * 16777216.0);
  constexpr double e44 = e48 * 16.0;
  if (a == b) return true;
  if (a == 0.0 || b == 0.0) return false;
  double d = std::fabs(a - b);
  if (!std::isfinite(d)) return false;
  double fa = std::fabs(a);
  double fb = std::fabs(b);
  if (d > fa * e44 || d > fb * e44) return false;
  if (IsRepresentableInteger(d) && IsRepresentableInteger(fa) && IsRepresentableInteger(fb)) {
    return false;
  }
  return d < fa * e48 && d < fb * e48;
}

double ApproxAdd(double a, double b) {
  if (((a < 0.0 && b > 0.0) || (b < 0.0 && a > 0.0)) && ApproxEqual(a, -b)) return 0.0;
  return a + b;
}

double ApproxSub(double a, double b) {
  if (((a < 0.0 && b < 0.0) || (a > 0.0 && b > 0.0)) && ApproxEqual(a, b)) return 0.0;
  return a - b;
}

double Round15(double x) {
  if (x == 0.0 || !std::isfinite(x)) return x;
  static const double kPow10[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
                                  1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
                                  1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
  double a = std::fabs(x);
  if (a >= 1e-8 && a < 1e15) {
    int e = static_cast<int>(std::floor(std::log10(a)));
    for (int attempt = 0; attempt < 3; ++attempt) {
      int k = 14 - e;
      if (k < 0 || k > 22) break;
      double scale = kPow10[k];
      double p = a * scale;
      double err = std::fma(a, scale, -p);
      if (p > 1e15 || (p == 1e15 && err >= 0)) {
        ++e;
        continue;
      }
      if (p < 1e14 || (p == 1e14 && err < 0)) {
        --e;
        continue;
      }
      double q = std::nearbyint(p);
      double half = std::floor(p) + 0.5;
      if (p == half && err != 0) q = err > 0 ? half + 0.5 : half - 0.5;
      double r = q / scale;
      return x < 0 ? -r : r;
    }
  }
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.14e", x);
  return std::strtod(buf, nullptr);
}

double RoundDigits(double x, int digits) {
  if (!std::isfinite(x) || x == 0.0) return x;
  if (digits > 308) digits = 308;
  if (digits < -308) return 0.0;
  bool negative = x < 0;
  double a = std::fabs(x);
  double result;
  if (digits >= 0) {
    double scale = std::pow(10.0, digits);
    double y = a * scale;
    if (!std::isfinite(y) || y >= 9007199254740992.0) return x;
    result = std::floor(Round15(y) + 0.5) / scale;
  } else {
    double scale = std::pow(10.0, -digits);
    double y = Round15(a / scale);
    result = std::floor(y + 0.5) * scale;
  }
  return negative ? -result : result;
}

std::string FormatGeneral(double d) {
  if (d == 0.0 || std::isnan(d)) return "0";
  if (std::isinf(d)) return "1.7976931348623E+308";
  bool negative = d < 0;
  double a = std::fabs(d);
  if (a < 2.2250738585072014e-308) return "0";
  char digits[16];
  int exponent = DecimalDigits(a, digits, 15);
  int count = 15;
  while (count > 1 && digits[count - 1] == '0') --count;
  if (std::abs(exponent) > 98) {
    bool up = digits[14] >= '5';
    if (up) {
      int i = 13;
      while (i >= 0 && digits[i] == '9') {
        digits[i] = '0';
        --i;
      }
      if (i < 0) {
        digits[0] = '1';
        ++exponent;
      } else {
        ++digits[i];
      }
    }
    count = 14;
    while (count > 1 && digits[count - 1] == '0') --count;
  }
  return LayoutDecimal(negative, digits, count, exponent);
}

std::string FormatShortest(double d) {
  if (d == 0.0 || !std::isfinite(d)) return "0";
  bool negative = d < 0;
  char buf[40];
  auto res = std::to_chars(buf, buf + sizeof buf - 1, std::fabs(d), std::chars_format::scientific);
  *res.ptr = '\0';
  char digits[20];
  int count = 0;
  const char* p = buf;
  while (*p != 'e' && count < 17) {
    if (*p != '.') digits[count++] = *p;
    ++p;
  }
  while (*p != 'e') ++p;
  int exponent = std::atoi(p + 1);
  while (count > 1 && digits[count - 1] == '0') --count;
  return LayoutDecimal(negative, digits, count, exponent);
}

bool ParseNumberLiteral(std::string_view s, double& out) {
  if (s.empty() || s.size() > 400) return false;
  size_t i = 0;
  bool digits = false;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
    ++i;
    digits = true;
  }
  if (i < s.size() && s[i] == '.') {
    ++i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      ++i;
      digits = true;
    }
  }
  if (!digits) return false;
  if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    ++i;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
    bool exp_digits = false;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      ++i;
      exp_digits = true;
    }
    if (!exp_digits) return false;
  }
  if (i != s.size()) return false;
  std::string copy(s);
  out = std::strtod(copy.c_str(), nullptr);
  return std::isfinite(out);
}

int CompareText(std::string_view a, std::string_view b) {
  size_t i = 0;
  size_t j = 0;
  while (i < a.size() && j < b.size()) {
    uint32_t ca;
    uint32_t cb;
    unsigned char ba = static_cast<unsigned char>(a[i]);
    unsigned char bb = static_cast<unsigned char>(b[j]);
    if (ba < 0x80 && bb < 0x80) {
      ca = (ba >= 'A' && ba <= 'Z') ? ba + 32u : ba;
      cb = (bb >= 'A' && bb <= 'Z') ? bb + 32u : bb;
      ++i;
      ++j;
    } else {
      ca = FoldCase(Utf8Decode(a, i), false);
      cb = FoldCase(Utf8Decode(b, j), false);
    }
    if (ca != cb) return ca < cb ? -1 : 1;
  }
  if (i < a.size()) return 1;
  if (j < b.size()) return -1;
  return 0;
}

bool EqualsText(std::string_view a, std::string_view b) {
  size_t n = std::min(a.size(), b.size());
  size_t start = 0;
  for (size_t i = 0; i < n; ++i) {
    unsigned char x = static_cast<unsigned char>(a[i]);
    unsigned char y = static_cast<unsigned char>(b[i]);
    if (x == y) {
      if ((x & 0xC0) != 0x80) start = i;
      continue;
    }
    if (x >= 0x80 || y >= 0x80) return CompareText(a.substr(start), b.substr(start)) == 0;
    if ((x | 0x20) != (y | 0x20) || (x | 0x20) < 'a' || (x | 0x20) > 'z') return false;
  }
  return a.size() == b.size();
}

std::string ToUpper(std::string_view s) {
  return MapCase(s, true);
}
std::string ToLower(std::string_view s) {
  return MapCase(s, false);
}

uint32_t Utf8Decode(std::string_view s, size_t& i) {
  unsigned char b0 = static_cast<unsigned char>(s[i]);
  if (b0 < 0x80) {
    ++i;
    return b0;
  }
  int extra = 0;
  uint32_t cp = 0;
  if ((b0 & 0xE0) == 0xC0) {
    extra = 1;
    cp = b0 & 0x1F;
  } else if ((b0 & 0xF0) == 0xE0) {
    extra = 2;
    cp = b0 & 0x0F;
  } else if ((b0 & 0xF8) == 0xF0) {
    extra = 3;
    cp = b0 & 0x07;
  } else {
    ++i;
    return b0;
  }
  if (i + extra >= s.size()) {
    ++i;
    return b0;
  }
  for (int k = 1; k <= extra; ++k) {
    unsigned char b = static_cast<unsigned char>(s[i + k]);
    if ((b & 0xC0) != 0x80) {
      ++i;
      return b0;
    }
    cp = (cp << 6) | (b & 0x3F);
  }
  i += extra + 1;
  return cp;
}

std::string Utf8Encode(uint32_t cp) {
  std::string out;
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
  return out;
}

int Utf8Units(std::string_view s) {
  int units = 0;
  size_t i = 0;
  while (i < s.size()) {
    unsigned char b = static_cast<unsigned char>(s[i]);
    if (b < 0x80) {
      ++i;
      ++units;
      continue;
    }
    uint32_t cp = Utf8Decode(s, i);
    units += cp >= 0x10000 ? 2 : 1;
  }
  return units;
}

size_t Utf8Offset(std::string_view s, int units) {
  size_t i = 0;
  int seen = 0;
  while (i < s.size() && seen < units) {
    unsigned char b = static_cast<unsigned char>(s[i]);
    if (b < 0x80) {
      ++i;
      ++seen;
      continue;
    }
    uint32_t cp = Utf8Decode(s, i);
    seen += cp >= 0x10000 ? 2 : 1;
  }
  return i;
}

std::string ColumnName(int col) {
  std::string out;
  int n = col + 1;
  while (n > 0) {
    int rem = (n - 1) % 26;
    out.insert(out.begin(), static_cast<char>('A' + rem));
    n = (n - 1) / 26;
  }
  return out;
}

bool ParseColumn(std::string_view s, int& col) {
  if (s.empty() || s.size() > 3) return false;
  int n = 0;
  for (char ch : s) {
    char c = ch;
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    if (c < 'A' || c > 'Z') return false;
    n = n * 26 + (c - 'A' + 1);
  }
  if (n > kMaxCols) return false;
  col = n - 1;
  return true;
}

std::string CellName(int row, int col) {
  return ColumnName(col) + std::to_string(row + 1);
}

bool ParseCellName(std::string_view s, int& row, int& col) {
  size_t i = 0;
  if (i < s.size() && s[i] == '$') ++i;
  size_t letters = i;
  while (i < s.size() && ((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z'))) ++i;
  if (i == letters) return false;
  if (!ParseColumn(s.substr(letters, i - letters), col)) return false;
  if (i < s.size() && s[i] == '$') ++i;
  size_t digits = i;
  long n = 0;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
    n = n * 10 + (s[i] - '0');
    if (n > kMaxRows) return false;
    ++i;
  }
  if (i == digits || i != s.size() || n < 1) return false;
  row = static_cast<int>(n - 1);
  return true;
}

bool ParseAreaName(std::string_view s, int& r1, int& c1, int& r2, int& c2) {
  size_t colon = s.find(':');
  if (colon == std::string_view::npos) {
    if (!ParseCellName(s, r1, c1)) return false;
    r2 = r1;
    c2 = c1;
    return true;
  }
  if (!ParseCellName(s.substr(0, colon), r1, c1)) return false;
  if (!ParseCellName(s.substr(colon + 1), r2, c2)) return false;
  if (r1 > r2) std::swap(r1, r2);
  if (c1 > c2) std::swap(c1, c2);
  return true;
}

}
