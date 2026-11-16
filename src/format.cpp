#include "format.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace sheet {

namespace {

constexpr const char* kMonthNames[] = {"January",   "February", "March",    "April",
                                       "May",       "June",     "July",     "August",
                                       "September", "October",  "November", "December"};
constexpr const char* kDayNames[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                     "Thursday", "Friday", "Saturday"};

bool IsDigit(char c) {
  return c >= '0' && c <= '9';
}
bool IsAlpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
char Lower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

std::string_view Trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

int MonthFromName(std::string_view w) {
  if (w.size() < 3) return 0;
  for (int i = 0; i < 12; ++i) {
    std::string_view full = kMonthNames[i];
    if (w.size() > full.size()) continue;
    bool match = true;
    for (size_t k = 0; k < w.size(); ++k) {
      if (Lower(w[k]) != Lower(full[k])) {
        match = false;
        break;
      }
    }
    if (match && (w.size() == 3 || w.size() == full.size() || (i == 8 && w.size() == 4))) {
      return i + 1;
    }
  }
  return 0;
}

struct DtToken {
  enum Kind { Num, Word, Sep } kind;
  std::string_view text;
  char sep = 0;
  long value = 0;
  int digits = 0;
};

bool TokenizeDateTime(std::string_view s, std::vector<DtToken>& out) {
  size_t i = 0;
  while (i < s.size()) {
    char c = s[i];
    if (IsDigit(c)) {
      size_t j = i;
      long v = 0;
      while (j < s.size() && IsDigit(s[j])) {
        if (j - i > 9) return false;
        v = v * 10 + (s[j] - '0');
        ++j;
      }
      DtToken t{DtToken::Num, s.substr(i, j - i)};
      t.value = v;
      t.digits = static_cast<int>(j - i);
      out.push_back(t);
      i = j;
    } else if (IsAlpha(c)) {
      size_t j = i;
      while (j < s.size() && IsAlpha(s[j])) ++j;
      out.push_back(DtToken{DtToken::Word, s.substr(i, j - i)});
      i = j;
    } else if (c == '/' || c == '-' || c == ':' || c == '.' || c == ',' || c == ' ') {
      if (c == ' ' && !out.empty() && out.back().kind == DtToken::Sep && out.back().sep == ' ') {
        ++i;
        continue;
      }
      DtToken t{DtToken::Sep, s.substr(i, 1)};
      t.sep = c;
      out.push_back(t);
      ++i;
    } else {
      return false;
    }
  }
  return true;
}

int ExpandYear(const DtToken& t) {
  if (t.digits <= 2)
    return t.value < 30 ? 2000 + static_cast<int>(t.value) : 1900 + static_cast<int>(t.value);
  return static_cast<int>(t.value);
}

bool ValidDate(int y, int m, int d) {
  if (y < 1900 || y > 9999 || m < 1 || m > 12 || d < 1) return false;
  if (y == 1900 && m == 2 && d == 29) return true;
  return d <= DaysInMonth(y, m);
}

bool ParseTimePart(const std::vector<DtToken>& t, size_t i, size_t end, bool with_date,
                   double& out) {
  if (i >= end) return false;
  if (t[i].kind != DtToken::Num) return false;
  long parts[3] = {0, 0, 0};
  int nparts = 0;
  double frac_seconds = 0;
  size_t k = i;
  while (k < end && nparts < 3) {
    if (t[k].kind != DtToken::Num) return false;
    parts[nparts++] = t[k].value;
    ++k;
    if (k < end && t[k].kind == DtToken::Sep && t[k].sep == ':') {
      ++k;
      continue;
    }
    break;
  }
  if (nparts == 3 && k + 1 < end && t[k].kind == DtToken::Sep && t[k].sep == '.' &&
      t[k + 1].kind == DtToken::Num) {
    frac_seconds = static_cast<double>(t[k + 1].value) / std::pow(10.0, t[k + 1].digits);
    k += 2;
  }
  if (k < end && t[k].kind == DtToken::Sep && t[k].sep == ' ') ++k;
  int ampm = 0;
  if (k < end && t[k].kind == DtToken::Word) {
    std::string w;
    for (char c : t[k].text) w.push_back(Lower(c));
    if (w == "am" || w == "a") {
      ampm = 1;
    } else if (w == "pm" || w == "p") {
      ampm = 2;
    } else {
      return false;
    }
    ++k;
  }
  while (k < end && t[k].kind == DtToken::Sep && t[k].sep == ' ') ++k;
  if (k != end) return false;
  if (nparts == 1 && ampm == 0) return false;
  long h = parts[0];
  long mi = parts[1];
  long sec = parts[2];
  if (mi > 59 || sec > 59) return false;
  if (ampm != 0) {
    if (h > 12) return false;
    if (h == 12) h = 0;
    if (ampm == 2) h += 12;
  } else if (with_date && h > 23) {
    return false;
  } else if (h > 9999) {
    return false;
  }
  out = (static_cast<double>(h) * 3600.0 + static_cast<double>(mi) * 60.0 +
         static_cast<double>(sec) + frac_seconds) /
        86400.0;
  return true;
}

bool ParseDatePart(const std::vector<DtToken>& t, size_t end, const DateSystem& ds, int& y, int& m,
                   int& d) {
  std::vector<const DtToken*> items;
  std::vector<char> seps;
  for (size_t i = 0; i < end; ++i) {
    if (t[i].kind == DtToken::Sep) {
      if (!items.empty()) seps.push_back(t[i].sep);
      continue;
    }
    if (items.size() == seps.size() + 1) seps.push_back(' ');
    items.push_back(&t[i]);
  }
  if (items.size() < 2 || items.size() > 3) return false;
  for (char s : seps) {
    if (s == ':') return false;
  }
  int word_index = -1;
  for (size_t i = 0; i < items.size(); ++i) {
    if (items[i]->kind == DtToken::Word) {
      if (word_index >= 0) return false;
      word_index = static_cast<int>(i);
    }
  }
  if (word_index < 0) {
    for (char s : seps) {
      if (s != '/' && s != '-' && s != '.') return false;
    }
    if (seps.size() == 2 && seps[0] != seps[1]) return false;
    if (items.size() == 3) {
      if (items[0]->digits == 4) {
        y = static_cast<int>(items[0]->value);
        m = static_cast<int>(items[1]->value);
        d = static_cast<int>(items[2]->value);
      } else {
        m = static_cast<int>(items[0]->value);
        d = static_cast<int>(items[1]->value);
        y = ExpandYear(*items[2]);
      }
      return ValidDate(y, m, d);
    }
    if (seps[0] == '.') return false;
    if (items[1]->digits == 4 || items[1]->value > 31) {
      m = static_cast<int>(items[0]->value);
      y = ExpandYear(*items[1]);
      d = 1;
      return ValidDate(y, m, d);
    }
    if (items[0]->digits == 4) {
      y = static_cast<int>(items[0]->value);
      m = static_cast<int>(items[1]->value);
      d = 1;
      return ValidDate(y, m, d);
    }
    m = static_cast<int>(items[0]->value);
    d = static_cast<int>(items[1]->value);
    y = ds.current_year;
    return ValidDate(y, m, d);
  }
  m = MonthFromName(items[word_index]->text);
  if (m == 0) return false;
  std::vector<const DtToken*> nums;
  for (size_t i = 0; i < items.size(); ++i) {
    if (static_cast<int>(i) != word_index) nums.push_back(items[i]);
  }
  if (items.size() == 2) {
    const DtToken* n = nums[0];
    if (word_index == 0 && (n->digits == 4 || n->value > 31)) {
      y = ExpandYear(*n);
      d = 1;
    } else if (word_index == 1 && n->digits == 4) {
      y = static_cast<int>(n->value);
      d = 1;
    } else {
      d = static_cast<int>(n->value);
      y = ds.current_year;
    }
    return ValidDate(y, m, d);
  }
  if (word_index == 0 || word_index == 1) {
    d = static_cast<int>(nums[0]->value);
    y = ExpandYear(*nums[1]);
    if (word_index == 0 && nums[0]->digits == 4) {
      y = static_cast<int>(nums[0]->value);
      d = static_cast<int>(nums[1]->value);
    }
    return ValidDate(y, m, d);
  }
  return false;
}

}

bool IsLeapYear(int y) {
  return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

int DaysInMonth(int y, int m) {
  static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 2 && IsLeapYear(y)) return 29;
  return kDays[m - 1];
}

int64_t DaysFromCivil(int y, int m, int d) {
  int64_t yy = y - (m <= 2 ? 1 : 0);
  int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
  int64_t yoe = yy - era * 400;
  int64_t mp = (m + 9) % 12;
  int64_t doy = (153 * mp + 2) / 5 + d - 1;
  int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

void CivilFromDays(int64_t z, int& y, int& m, int& d) {
  z += 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  int64_t doe = z - era * 146097;
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t yy = yoe + era * 400;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int64_t mp = (5 * doy + 2) / 153;
  d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  y = static_cast<int>(yy + (m <= 2 ? 1 : 0));
}

bool SerialFromDate(int y, int m, int d, const DateSystem& ds, double& serial) {
  if (y < 0 || y > 9999) return false;
  if (y < 1900) y += 1900;
  int64_t months = static_cast<int64_t>(y) * 12 + (static_cast<int64_t>(m) - 1);
  int64_t ny = months >= 0 ? months / 12 : (months - 11) / 12;
  int nm = static_cast<int>(months - ny * 12) + 1;
  if (ny < 0 || ny > 10000) return false;
  int64_t base;
  if (ds.date1904) {
    base = DaysFromCivil(static_cast<int>(ny), nm, 1) - DaysFromCivil(1904, 1, 1);
  } else if (ny < 1900 || (ny == 1900 && nm <= 2)) {
    base = DaysFromCivil(static_cast<int>(ny), nm, 1) - DaysFromCivil(1899, 12, 31);
  } else {
    base = DaysFromCivil(static_cast<int>(ny), nm, 1) - DaysFromCivil(1899, 12, 30);
  }
  double s = static_cast<double>(base) + static_cast<double>(d) - 1.0;
  if (s < 0 || s > kMaxSerial) return false;
  serial = s;
  return true;
}

bool DateFromSerial(double serial, const DateSystem& ds, int& y, int& m, int& d) {
  if (!(serial >= 0) || serial >= kMaxSerial + 1) return false;
  int64_t days = static_cast<int64_t>(std::floor(serial));
  if (ds.date1904) {
    CivilFromDays(days + DaysFromCivil(1904, 1, 1), y, m, d);
    return true;
  }
  if (days == 0) {
    y = 1900;
    m = 1;
    d = 0;
    return true;
  }
  if (days == 60) {
    y = 1900;
    m = 2;
    d = 29;
    return true;
  }
  if (days < 60) {
    CivilFromDays(days + DaysFromCivil(1899, 12, 31), y, m, d);
  } else {
    CivilFromDays(days + DaysFromCivil(1899, 12, 30), y, m, d);
  }
  return true;
}

int WeekdayFromSerial(double serial, const DateSystem& ds) {
  int64_t days = static_cast<int64_t>(std::floor(serial));
  if (ds.date1904) days += 1462;
  int64_t w = (days + 6) % 7;
  if (w < 0) w += 7;
  return static_cast<int>(w);
}

void TimeFromSerial(double serial, int& h, int& mi, int& s) {
  double frac = serial - std::floor(serial);
  long long total = static_cast<long long>(std::floor(frac * 86400.0 + 0.5));
  if (total >= 86400) total -= 86400;
  h = static_cast<int>(total / 3600);
  mi = static_cast<int>((total / 60) % 60);
  s = static_cast<int>(total % 60);
}

bool ParseDateTime(std::string_view text, const DateSystem& ds, double& out) {
  std::string_view s = Trim(text);
  if (s.empty() || s.size() > 64) return false;
  std::vector<DtToken> t;
  if (!TokenizeDateTime(s, t)) return false;
  if (t.empty()) return false;
  size_t colon = t.size();
  for (size_t i = 0; i < t.size(); ++i) {
    if (t[i].kind == DtToken::Sep && t[i].sep == ':') {
      colon = i;
      break;
    }
  }
  size_t time_start = t.size();
  if (colon < t.size()) {
    if (colon == 0) return false;
    time_start = colon - 1;
  } else {
    for (size_t i = 0; i + 1 < t.size(); ++i) {
      if (t[i].kind == DtToken::Num && t[i + 1].kind == DtToken::Word) {
        std::string w;
        for (char c : t[i + 1].text) w.push_back(Lower(c));
        if (w == "am" || w == "pm") {
          time_start = i;
          break;
        }
      }
      if (t[i].kind == DtToken::Num && t[i + 1].kind == DtToken::Sep && t[i + 1].sep == ' ' &&
          i + 2 < t.size() && t[i + 2].kind == DtToken::Word) {
        std::string w;
        for (char c : t[i + 2].text) w.push_back(Lower(c));
        if (w == "am" || w == "pm") {
          time_start = i;
          break;
        }
      }
    }
  }
  double time = 0;
  bool has_time = false;
  size_t date_end = time_start;
  if (time_start < t.size()) {
    if (!ParseTimePart(t, time_start, t.size(), time_start > 0, time)) return false;
    has_time = true;
    while (date_end > 0 && t[date_end - 1].kind == DtToken::Sep && t[date_end - 1].sep == ' ') {
      --date_end;
    }
  }
  if (date_end == 0) {
    if (!has_time) return false;
    out = time;
    return true;
  }
  int y = 0;
  int m = 0;
  int d = 0;
  if (!ParseDatePart(t, date_end, ds, y, m, d)) return false;
  double serial;
  if (!SerialFromDate(y, m, d, ds, serial)) return false;
  out = serial + time;
  return true;
}

bool ParseNumberText(std::string_view text, const DateSystem& ds, double& out) {
  std::string_view s = Trim(text);
  if (s.empty()) return false;
  bool negative = false;
  bool paren = false;
  std::string_view body = s;
  if (body.front() == '(' && body.back() == ')') {
    paren = true;
    body = Trim(body.substr(1, body.size() - 2));
  }
  size_t i = 0;
  bool currency = false;
  bool sign_seen = false;
  while (i < body.size()) {
    char c = body[i];
    if ((c == '-' || c == '+') && !sign_seen) {
      sign_seen = true;
      negative = c == '-';
      ++i;
    } else if (c == '$' && !currency) {
      currency = true;
      ++i;
    } else if (c == ' ' && i > 0) {
      ++i;
    } else {
      break;
    }
  }
  size_t start = i;
  std::string digits;
  bool any = false;
  bool point = false;
  int group_len = -1;
  bool grouped = false;
  while (i < body.size()) {
    char c = body[i];
    if (IsDigit(c)) {
      digits.push_back(c);
      any = true;
      if (group_len >= 0) ++group_len;
      ++i;
    } else if (c == ',' && !point && any) {
      if (group_len >= 0 && group_len != 3) {
        digits.clear();
        any = false;
        break;
      }
      if (group_len < 0 && digits.size() > 3) {
        any = false;
        break;
      }
      group_len = 0;
      grouped = true;
      ++i;
    } else if (c == '.' && !point) {
      if (grouped && group_len != 3) {
        any = false;
        break;
      }
      group_len = -1;
      point = true;
      digits.push_back('.');
      ++i;
    } else {
      break;
    }
  }
  if (grouped && !point && group_len != 3) any = false;
  bool plain = any && i > start;
  if (plain && i < body.size() && (body[i] == 'e' || body[i] == 'E')) {
    size_t j = i + 1;
    std::string ex = "e";
    if (j < body.size() && (body[j] == '+' || body[j] == '-')) ex.push_back(body[j++]);
    size_t ds_start = j;
    while (j < body.size() && IsDigit(body[j])) ex.push_back(body[j++]);
    if (j == ds_start) {
      plain = false;
    } else {
      digits += ex;
      i = j;
    }
  }
  int percent = 0;
  if (plain) {
    while (i < body.size() && (body[i] == '%' || body[i] == ' ')) {
      if (body[i] == '%') ++percent;
      ++i;
    }
    if (i == body.size() && !(paren && sign_seen)) {
      double v = std::strtod(digits.c_str(), nullptr);
      if (!std::isfinite(v)) return false;
      for (int p = 0; p < percent; ++p) v /= 100.0;
      if (negative) v = -v;
      if (paren) v = -v;
      out = v;
      return true;
    }
  }
  if (paren || currency) return false;
  size_t space = s.find(' ');
  if (space != std::string_view::npos) {
    std::string_view whole = Trim(s.substr(0, space));
    std::string_view frac = Trim(s.substr(space + 1));
    size_t slash = frac.find('/');
    if (slash != std::string_view::npos && !whole.empty()) {
      bool neg = false;
      if (whole.front() == '-') {
        neg = true;
        whole.remove_prefix(1);
      }
      std::string_view num = frac.substr(0, slash);
      std::string_view den = frac.substr(slash + 1);
      auto all_digits = [](std::string_view x) {
        if (x.empty() || x.size() > 9) return false;
        for (char c : x) {
          if (!IsDigit(c)) return false;
        }
        return true;
      };
      if (all_digits(whole) && all_digits(num) && all_digits(den)) {
        double dn = std::strtod(std::string(den).c_str(), nullptr);
        double nn = std::strtod(std::string(num).c_str(), nullptr);
        if (dn > 0 && nn < dn) {
          double v = std::strtod(std::string(whole).c_str(), nullptr) + nn / dn;
          out = neg ? -v : v;
          return true;
        }
      }
    }
  }
  return ParseDateTime(s, ds, out);
}

namespace {

enum class Tok : uint8_t {
  Literal,
  Zero,
  Hash,
  Question,
  Point,
  Comma,
  Percent,
  Exp,
  Slash,
  At,
  Year,
  Month,
  Day,
  Hour,
  Minute,
  Second,
  AmPm,
  ElapsedH,
  ElapsedM,
  ElapsedS,
  SubSecond,
  General,
};

struct FmtTok {
  Tok kind;
  std::string text;
  int len = 1;
  bool exp_plus = false;
};

struct Section {
  std::vector<FmtTok> toks;
  bool has_cond = false;
  std::string cond_op;
  double cond_value = 0;
  bool is_date = false;
  bool has_ampm = false;
  bool has_at = false;
  bool has_digits = false;
  bool empty = true;
};

bool IsDigitTok(Tok k) {
  return k == Tok::Zero || k == Tok::Hash || k == Tok::Question;
}

void AddLiteral(Section& sec, std::string_view text) {
  if (!sec.toks.empty() && sec.toks.back().kind == Tok::Literal) {
    sec.toks.back().text += text;
  } else {
    sec.toks.push_back(FmtTok{Tok::Literal, std::string(text)});
  }
}

std::vector<std::string> SplitSections(std::string_view f) {
  std::vector<std::string> parts(1);
  bool quote = false;
  bool bracket = false;
  for (size_t i = 0; i < f.size(); ++i) {
    char c = f[i];
    if (quote) {
      parts.back().push_back(c);
      if (c == '"') quote = false;
      continue;
    }
    if (c == '\\' && i + 1 < f.size()) {
      parts.back().push_back(c);
      parts.back().push_back(f[++i]);
      continue;
    }
    if (c == '"') quote = true;
    if (c == '[') bracket = true;
    if (c == ']') bracket = false;
    if (c == ';' && !bracket) {
      parts.emplace_back();
      continue;
    }
    parts.back().push_back(c);
  }
  return parts;
}

bool ParseSection(std::string_view f, Section& sec) {
  size_t i = 0;
  sec.empty = f.empty();
  while (i < f.size()) {
    char c = f[i];
    char lc = Lower(c);
    if (c == '"') {
      size_t j = f.find('"', i + 1);
      if (j == std::string_view::npos) j = f.size();
      AddLiteral(sec, f.substr(i + 1, j - i - 1));
      i = j + 1;
    } else if (c == '\\') {
      if (i + 1 < f.size()) AddLiteral(sec, f.substr(i + 1, 1));
      i += 2;
    } else if (c == '[') {
      size_t j = f.find(']', i);
      if (j == std::string_view::npos) return false;
      std::string_view in = f.substr(i + 1, j - i - 1);
      i = j + 1;
      if (in.empty()) continue;
      if (in[0] == '<' || in[0] == '>' || in[0] == '=') {
        size_t k = 0;
        while (k < in.size() && (in[k] == '<' || in[k] == '>' || in[k] == '=')) ++k;
        sec.has_cond = true;
        sec.cond_op = std::string(in.substr(0, k));
        sec.cond_value = std::strtod(std::string(in.substr(k)).c_str(), nullptr);
        continue;
      }
      if (in[0] == '$') {
        size_t dash = in.find('-');
        AddLiteral(sec, in.substr(1, dash == std::string_view::npos ? in.size() - 1 : dash - 1));
        continue;
      }
      char e = Lower(in[0]);
      bool same = true;
      for (char x : in) same = same && Lower(x) == e;
      if (same && (e == 'h' || e == 'm' || e == 's')) {
        Tok k = e == 'h' ? Tok::ElapsedH : (e == 'm' ? Tok::ElapsedM : Tok::ElapsedS);
        FmtTok t{k, ""};
        t.len = static_cast<int>(in.size());
        sec.toks.push_back(t);
        sec.is_date = true;
      }
    } else if (c == '_') {
      AddLiteral(sec, " ");
      i += 2;
    } else if (c == '*') {
      i += 2;
    } else if (c == '0' || c == '#' || c == '?') {
      sec.toks.push_back(FmtTok{c == '0' ? Tok::Zero : (c == '#' ? Tok::Hash : Tok::Question), ""});
      sec.has_digits = true;
      ++i;
    } else if (c == '.') {
      if (!sec.toks.empty() &&
          (sec.toks.back().kind == Tok::Second || sec.toks.back().kind == Tok::ElapsedS) &&
          i + 1 < f.size() && f[i + 1] == '0') {
        size_t j = i + 1;
        while (j < f.size() && f[j] == '0') ++j;
        FmtTok t{Tok::SubSecond, ""};
        t.len = static_cast<int>(j - i - 1);
        sec.toks.push_back(t);
        i = j;
      } else {
        sec.toks.push_back(FmtTok{Tok::Point, "."});
        ++i;
      }
    } else if (c == ',') {
      sec.toks.push_back(FmtTok{Tok::Comma, ","});
      ++i;
    } else if (c == '%') {
      sec.toks.push_back(FmtTok{Tok::Percent, "%"});
      ++i;
    } else if (c == '@') {
      sec.toks.push_back(FmtTok{Tok::At, ""});
      sec.has_at = true;
      ++i;
    } else if ((c == 'E' || c == 'e') && i + 1 < f.size() && (f[i + 1] == '+' || f[i + 1] == '-') &&
               sec.has_digits) {
      FmtTok t{Tok::Exp, std::string(1, c)};
      t.exp_plus = f[i + 1] == '+';
      sec.toks.push_back(t);
      i += 2;
    } else if (c == '/') {
      sec.toks.push_back(FmtTok{Tok::Slash, "/"});
      ++i;
    } else if (lc == 'g' && f.size() - i >= 7 && Lower(f[i + 1]) == 'e' && Lower(f[i + 2]) == 'n' &&
               Lower(f[i + 3]) == 'e' && Lower(f[i + 4]) == 'r' && Lower(f[i + 5]) == 'a' &&
               Lower(f[i + 6]) == 'l') {
      sec.toks.push_back(FmtTok{Tok::General, ""});
      i += 7;
    } else if (lc == 'a' && f.size() - i >= 5 && Lower(f[i + 1]) == 'm' && f[i + 2] == '/' &&
               Lower(f[i + 3]) == 'p' && Lower(f[i + 4]) == 'm') {
      FmtTok t{Tok::AmPm, std::string(f.substr(i, 5))};
      t.len = 5;
      sec.toks.push_back(t);
      sec.has_ampm = true;
      sec.is_date = true;
      i += 5;
    } else if (lc == 'a' && f.size() - i >= 3 && f[i + 1] == '/' && Lower(f[i + 2]) == 'p') {
      FmtTok t{Tok::AmPm, std::string(f.substr(i, 3))};
      t.len = 3;
      sec.toks.push_back(t);
      sec.has_ampm = true;
      sec.is_date = true;
      i += 3;
    } else if (lc == 'y' || lc == 'm' || lc == 'd' || lc == 'h' || lc == 's' || lc == 'e') {
      size_t j = i;
      while (j < f.size() && Lower(f[j]) == lc) ++j;
      Tok k = lc == 'y' || lc == 'e' ? Tok::Year
              : lc == 'm'            ? Tok::Month
              : lc == 'd'            ? Tok::Day
              : lc == 'h'            ? Tok::Hour
                                     : Tok::Second;
      FmtTok t{k, ""};
      t.len = static_cast<int>(j - i);
      if (lc == 'e') t.len = 4;
      sec.toks.push_back(t);
      sec.is_date = true;
      i = j;
    } else if (lc == 'b' && i + 1 < f.size() && (f[i + 1] == '1' || f[i + 1] == '2')) {
      i += 2;
    } else {
      size_t len = 1;
      unsigned char uc = static_cast<unsigned char>(c);
      if (uc >= 0xC0) {
        while (i + len < f.size() && (static_cast<unsigned char>(f[i + len]) & 0xC0) == 0x80) ++len;
      }
      AddLiteral(sec, f.substr(i, len));
      i += len;
    }
  }
  if (!sec.is_date) {
    for (FmtTok& t : sec.toks) {
      if (IsDigitTok(t.kind)) break;
      if (t.kind == Tok::Comma) t.kind = Tok::Literal;
    }
  }
  if (sec.is_date) {
    for (size_t k = 0; k < sec.toks.size(); ++k) {
      if (sec.toks[k].kind != Tok::Month) continue;
      bool minute = false;
      for (size_t p = k; p-- > 0;) {
        Tok pk = sec.toks[p].kind;
        if (pk == Tok::Literal) continue;
        minute = pk == Tok::Hour || pk == Tok::ElapsedH || pk == Tok::Second || pk == Tok::ElapsedS;
        break;
      }
      if (!minute) {
        for (size_t n = k + 1; n < sec.toks.size(); ++n) {
          Tok nk = sec.toks[n].kind;
          if (nk == Tok::Literal) continue;
          minute = nk == Tok::Second || nk == Tok::ElapsedS;
          break;
        }
      }
      if (minute && sec.toks[k].len <= 2) sec.toks[k].kind = Tok::Minute;
    }
  }
  return true;
}

void DecimalParts(double r, int frac_digits, std::string& ip, std::string& fp) {
  ip.clear();
  fp.assign(static_cast<size_t>(frac_digits), '0');
  if (r == 0) return;
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.14e", r);
  char digits[15];
  digits[0] = buf[0];
  for (int k = 1; k < 15; ++k) digits[k] = buf[k + 1];
  int e = std::atoi(std::strchr(buf, 'e') + 1);
  for (int pos = 0; pos < 15; ++pos) {
    int place = e - pos;
    char d = digits[pos];
    if (place >= 0) {
      ip.push_back(d);
    } else {
      int fi = -place - 1;
      if (fi < frac_digits) fp[fi] = d;
    }
  }
  if (e >= 15) ip.append(static_cast<size_t>(e - 14), '0');
  if (e < 0) ip.clear();
  size_t nz = ip.find_first_not_of('0');
  ip = nz == std::string::npos ? std::string() : ip.substr(nz);
}

void BestFraction(double x, long max_den, long& num, long& den) {
  long p0 = 0, q0 = 1, p1 = 1, q1 = 0;
  double v = x;
  for (int iter = 0; iter < 64; ++iter) {
    double a = std::floor(v);
    long ai = static_cast<long>(a);
    long p2 = ai * p1 + p0;
    long q2 = ai * q1 + q0;
    if (q2 > max_den) {
      long k = (max_den - q0) / q1;
      long pa = k * p1 + p0;
      long qa = k * q1 + q0;
      if (std::fabs(x - static_cast<double>(pa) / qa) <
          std::fabs(x - static_cast<double>(p1) / q1)) {
        num = pa;
        den = qa;
      } else {
        num = p1;
        den = q1;
      }
      return;
    }
    p0 = p1;
    q0 = q1;
    p1 = p2;
    q1 = q2;
    double f = v - a;
    if (f < 1e-12) break;
    v = 1.0 / f;
  }
  num = p1;
  den = q1;
}

int DecimalExponent(double a) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.14e", a);
  return std::atoi(std::strchr(buf, 'e') + 1);
}

bool IsTextTok(Tok k) {
  return k == Tok::Literal || k == Tok::Percent || k == Tok::Slash;
}

std::string Literals(const std::vector<FmtTok>& toks, size_t from, size_t to) {
  std::string out;
  for (size_t k = from; k < to && k < toks.size(); ++k) {
    if (IsTextTok(toks[k].kind)) out += toks[k].text;
  }
  return out;
}

size_t DisplayWidth(const std::vector<FmtTok>& toks, size_t from, size_t to) {
  size_t w = 0;
  for (size_t k = from; k < to && k < toks.size(); ++k) {
    if (IsDigitTok(toks[k].kind)) {
      ++w;
    } else if (IsTextTok(toks[k].kind)) {
      for (char c : toks[k].text) w += (static_cast<unsigned char>(c) & 0xC0) != 0x80 ? 1 : 0;
    }
  }
  return w;
}

bool HasKind(const std::vector<FmtTok>& toks, size_t from, size_t to, Tok kind) {
  for (size_t k = from; k < to && k < toks.size(); ++k) {
    if (toks[k].kind == kind) return true;
  }
  return false;
}

std::string RenderInt(const std::vector<FmtTok>& toks, size_t from, size_t to,
                      const std::string& digits, bool grouping) {
  std::vector<Tok> kinds;
  for (size_t k = from; k < to; ++k) {
    if (IsDigitTok(toks[k].kind)) kinds.push_back(toks[k].kind);
  }
  size_t n = kinds.size();
  size_t nd = digits.size();
  size_t total = std::max(n, nd);
  auto at = [&](size_t i) -> char {
    if (i < nd) return digits[nd - 1 - i];
    Tok kind = kinds[n - 1 - i];
    return kind == Tok::Zero ? '0' : (kind == Tok::Question ? ' ' : '\0');
  };
  std::string out;
  auto put = [&](size_t i) {
    char c = at(i);
    if (c == '\0') return;
    out.push_back(c);
    if (grouping && i > 0 && i % 3 == 0) out.push_back(c == ' ' ? ' ' : ',');
  };
  size_t idx = 0;
  for (size_t k = from; k < to; ++k) {
    const FmtTok& t = toks[k];
    if (!IsDigitTok(t.kind)) {
      if (IsTextTok(t.kind)) out += t.text;
      continue;
    }
    if (idx == 0) {
      for (size_t i = total; i-- > n;) put(i);
    }
    put(n - 1 - idx);
    ++idx;
  }
  if (n == 0) {
    for (size_t i = nd; i-- > 0;) put(i);
  }
  return out;
}

std::string RenderFrac(const std::vector<FmtTok>& toks, size_t from, size_t to,
                       const std::string& fp) {
  std::vector<Tok> kinds;
  for (size_t k = from; k < to; ++k) {
    if (IsDigitTok(toks[k].kind)) kinds.push_back(toks[k].kind);
  }
  size_t keep = std::min(fp.size(), kinds.size());
  while (keep > 0 && fp[keep - 1] == '0' && kinds[keep - 1] != Tok::Zero) --keep;
  std::string out;
  size_t idx = 0;
  for (size_t k = from; k < to; ++k) {
    const FmtTok& t = toks[k];
    if (IsDigitTok(t.kind)) {
      if (idx < keep) {
        out.push_back(fp[idx]);
      } else if (t.kind == Tok::Question) {
        out.push_back(' ');
      } else if (t.kind == Tok::Zero) {
        out.push_back('0');
      }
      ++idx;
    } else if (IsTextTok(t.kind)) {
      out += t.text;
    }
  }
  return out;
}

std::string RenderDenominator(const std::vector<FmtTok>& toks, size_t from, size_t to,
                              const std::string& digits) {
  if (HasKind(toks, from, to, Tok::Zero)) return RenderInt(toks, from, to, digits, false);
  std::string out;
  size_t idx = 0;
  for (size_t k = from; k < to; ++k) {
    const FmtTok& t = toks[k];
    if (IsDigitTok(t.kind)) {
      if (idx == 0) out += digits;
      if (idx >= digits.size() && t.kind == Tok::Question) out.push_back(' ');
      ++idx;
    } else if (IsTextTok(t.kind)) {
      out += t.text;
    }
  }
  return out;
}

bool FormatFraction(const Section& sec, double x, std::string& out) {
  const std::vector<FmtTok>& toks = sec.toks;
  size_t end = toks.size();
  size_t slash = 0;
  while (slash < end && toks[slash].kind != Tok::Slash) ++slash;
  if (slash == end) return false;
  size_t n2 = slash;
  while (n2 > 0 && !IsDigitTok(toks[n2 - 1].kind)) --n2;
  size_t n1 = n2;
  while (n1 > 0 && IsDigitTok(toks[n1 - 1].kind)) --n1;
  if (n1 == n2) return false;
  size_t l1 = n1;
  while (l1 > 0 && !IsDigitTok(toks[l1 - 1].kind)) --l1;
  bool has_whole = l1 > 0;
  size_t sep = has_whole ? l1 : n1;
  std::string fixed;
  size_t d1 = slash + 1;
  size_t d2 = d1;
  if (d1 < end && toks[d1].kind == Tok::Literal && !toks[d1].text.empty() &&
      toks[d1].text[0] >= '1' && toks[d1].text[0] <= '9') {
    const std::string& t = toks[d1].text;
    size_t q = 0;
    while (q < t.size() && IsDigit(t[q])) fixed.push_back(t[q++]);
    d2 = d1 + 1;
    if (q == t.size()) {
      while (d2 < end && toks[d2].kind == Tok::Zero) {
        fixed.push_back('0');
        ++d2;
      }
    }
  } else {
    while (d1 < end && !IsDigitTok(toks[d1].kind)) ++d1;
    d2 = d1;
    while (d2 < end && IsDigitTok(toks[d2].kind)) ++d2;
  }
  double whole = has_whole ? std::floor(x) : 0.0;
  double frac = x - whole;
  long num = 0;
  long den = 1;
  if (!fixed.empty()) {
    den = std::atol(fixed.c_str());
    if (den <= 0) den = 1;
    num = static_cast<long>(std::floor(frac * static_cast<double>(den) + 0.5));
  } else {
    long max_den = 1;
    for (size_t k = d1; k < d2 && max_den < 100000; ++k) max_den *= 10;
    BestFraction(frac, std::max(1L, max_den - 1), num, den);
  }
  if (has_whole && num != 0 && num == den) {
    whole += 1;
    num = 0;
  }
  size_t tail = d2;
  std::string fixed_rest;
  if (!fixed.empty() && toks[d1].text.size() > fixed.size())
    fixed_rest = toks[d1].text.substr(fixed.size());
  bool w_q = HasKind(toks, 0, sep, Tok::Question);
  bool n_q = HasKind(toks, n1, n2, Tok::Question);
  bool any_q = w_q || n_q || HasKind(toks, d1, d2, Tok::Question);
  bool n_zero = HasKind(toks, n1, n2, Tok::Zero);
  std::string wd = whole > 0 ? std::to_string(static_cast<long long>(whole)) : std::string();
  if (has_whole && num == 0 && !n_zero) {
    out += RenderInt(toks, 0, sep, wd.empty() ? "0" : wd, false);
    if (any_q) {
      size_t width =
          fixed.empty() ? DisplayWidth(toks, sep, d2) : DisplayWidth(toks, sep, d1) + fixed.size();
      out.append(width, ' ');
    }
    out += fixed_rest;
    out += Literals(toks, tail, end);
    return true;
  }
  if (has_whole) {
    if (wd.empty()) {
      size_t last = sep;
      for (size_t k = 0; k < sep; ++k) {
        if (IsDigitTok(toks[k].kind)) last = k;
      }
      if (num == 0 && toks[last].kind != Tok::Hash) wd = "0";
      out += RenderInt(toks, 0, sep, wd, false);
      if (!wd.empty() || HasKind(toks, 0, sep, Tok::Zero)) {
        out += Literals(toks, sep, n1);
      } else if (w_q || n_q) {
        out.append(DisplayWidth(toks, sep, n1), ' ');
      }
    } else {
      out += RenderInt(toks, 0, sep, wd, false);
      out += Literals(toks, sep, n1);
    }
  } else {
    out += Literals(toks, 0, n1);
  }
  out += RenderInt(toks, n1, n2, std::to_string(num), false);
  out += Literals(toks, n2, slash);
  out += "/";
  if (!fixed.empty()) {
    out += Literals(toks, slash + 1, d1);
    out += fixed;
    out += fixed_rest;
  } else {
    out += Literals(toks, slash + 1, d1);
    out += RenderDenominator(toks, d1, d2, std::to_string(den));
  }
  out += Literals(toks, tail, end);
  return true;
}

bool FormatNumber(const Section& sec, double v, bool add_minus, std::string& out) {
  const std::vector<FmtTok>& toks = sec.toks;
  int percent = 0;
  for (const FmtTok& t : toks) percent += t.kind == Tok::Percent ? 1 : 0;
  size_t point = toks.size();
  size_t exp = toks.size();
  for (size_t k = 0; k < toks.size(); ++k) {
    if (toks[k].kind == Tok::Point && point == toks.size() && exp == toks.size()) point = k;
    if (toks[k].kind == Tok::Exp && exp == toks.size()) exp = k;
  }
  size_t num_end = std::min(point, exp);
  bool grouping = false;
  int scale = 0;
  for (size_t k = 0; k < toks.size(); ++k) {
    if (toks[k].kind != Tok::Comma) continue;
    bool before = false;
    for (size_t p = k; p-- > 0;) {
      if (IsDigitTok(toks[p].kind)) {
        before = true;
        break;
      }
      if (toks[p].kind != Tok::Comma) break;
    }
    bool after = false;
    for (size_t n = k + 1; n < toks.size(); ++n) {
      if (IsDigitTok(toks[n].kind)) {
        after = true;
        break;
      }
      if (toks[n].kind != Tok::Comma) break;
    }
    if (before && after && k < num_end) {
      grouping = true;
    } else if (before && !after) {
      ++scale;
    }
  }
  std::string body;
  if (add_minus) body.push_back('-');
  if (exp < toks.size()) {
    double a = std::fabs(v);
    int int_ph = 0;
    for (size_t k = 0; k < num_end; ++k) int_ph += IsDigitTok(toks[k].kind) ? 1 : 0;
    int frac_ph = 0;
    for (size_t k = point; k < exp; ++k) frac_ph += IsDigitTok(toks[k].kind) ? 1 : 0;
    int step = std::max(1, int_ph);
    int e = 0;
    std::string ip;
    std::string fp;
    if (a == 0) {
      ip.assign(static_cast<size_t>(int_ph), '0');
      fp.assign(static_cast<size_t>(frac_ph), '0');
    } else {
      int e10 = DecimalExponent(a);
      e = (e10 >= 0 ? e10 / step : -((-e10 + step - 1) / step)) * step;
      double mant = RoundDigits(a / std::pow(10.0, e), frac_ph);
      if (mant >= std::pow(10.0, step)) {
        e += step;
        mant = RoundDigits(a / std::pow(10.0, e), frac_ph);
      }
      DecimalParts(mant, frac_ph, ip, fp);
    }
    body += RenderInt(toks, 0, num_end, ip, grouping);
    if (point < exp) {
      body += ".";
      body += RenderFrac(toks, point + 1, exp, fp);
    }
    body += toks[exp].text.empty() ? std::string("E") : toks[exp].text;
    size_t d1 = exp + 1;
    while (d1 < toks.size() && !IsDigitTok(toks[d1].kind)) ++d1;
    size_t d2 = d1;
    while (d2 < toks.size() && (IsDigitTok(toks[d2].kind) || toks[d2].kind == Tok::Comma)) ++d2;
    body += Literals(toks, exp + 1, d1);
    if (e < 0) {
      body += "-";
    } else if (toks[exp].exp_plus) {
      body += "+";
    }
    body += RenderInt(toks, d1, d2, std::to_string(std::abs(e)), false);
    body += Literals(toks, d2, toks.size());
    out = body;
    return true;
  }
  double x = std::fabs(v);
  for (int p = 0; p < percent; ++p) x *= 100.0;
  for (int s = 0; s < scale; ++s) x /= 1000.0;
  if (HasKind(toks, 0, toks.size(), Tok::Slash)) {
    std::string f;
    if (FormatFraction(sec, x, f)) {
      out = body + f;
      return true;
    }
  }
  int frac_ph = 0;
  for (size_t k = point; k < toks.size(); ++k) frac_ph += IsDigitTok(toks[k].kind) ? 1 : 0;
  double r = RoundDigits(x, frac_ph);
  std::string ip;
  std::string fp;
  DecimalParts(r, frac_ph, ip, fp);
  body += RenderInt(toks, 0, num_end, ip, grouping);
  if (point < toks.size()) {
    body += ".";
    body += RenderFrac(toks, point + 1, toks.size(), fp);
  }
  out = body;
  return true;
}

bool FormatDate(const Section& sec, double v0, const DateSystem& ds, std::string& out) {
  bool elapsed_h = false;
  bool elapsed_m = false;
  bool elapsed_s = false;
  for (const FmtTok& t : sec.toks) {
    elapsed_h = elapsed_h || t.kind == Tok::ElapsedH;
    elapsed_m = elapsed_m || t.kind == Tok::ElapsedM;
    elapsed_s = elapsed_s || t.kind == Tok::ElapsedS;
  }
  bool elapsed = elapsed_h || elapsed_m || elapsed_s;
  double v = v0;
  if (v < 0) {
    if (!elapsed) return false;
    out += "-";
    v = -v;
  }
  if (v >= kMaxSerial + 1) return false;
  int sub = 0;
  for (const FmtTok& t : sec.toks) {
    if (t.kind == Tok::SubSecond) sub = std::max(sub, t.len);
  }
  double unit = 86400.0 * std::pow(10.0, sub);
  double total = std::floor(v * unit + 0.5);
  double day = std::floor(total / unit);
  double in_day = total - day * unit;
  long long sub_units = static_cast<long long>(in_day);
  long long secs_total = sub_units / static_cast<long long>(std::pow(10.0, sub));
  long long sub_value = sub_units % static_cast<long long>(std::pow(10.0, sub));
  int h = static_cast<int>(secs_total / 3600);
  int mi = static_cast<int>((secs_total / 60) % 60);
  int s = static_cast<int>(secs_total % 60);
  int y = 1900;
  int m = 1;
  int d = 0;
  if (!DateFromSerial(day, ds, y, m, d)) return false;
  int wd = WeekdayFromSerial(day, ds);
  double elapsed_seconds = std::floor(total / std::pow(10.0, sub));
  long long eh = static_cast<long long>(std::floor(elapsed_seconds / 3600.0));
  long long em = static_cast<long long>(std::floor(elapsed_seconds / 60.0));
  long long es = static_cast<long long>(elapsed_seconds);
  auto pad = [](long long value, int width) {
    std::string x = std::to_string(value);
    while (static_cast<int>(x.size()) < width) x.insert(0, "0");
    return x;
  };
  for (const FmtTok& t : sec.toks) {
    char buf[32];
    switch (t.kind) {
      case Tok::Literal:
        out += t.text;
        break;
      case Tok::Year:
        if (t.len <= 2) {
          std::snprintf(buf, sizeof buf, "%02d", y % 100);
        } else {
          std::snprintf(buf, sizeof buf, "%04d", y);
        }
        out += buf;
        break;
      case Tok::Month:
        if (t.len == 1) {
          out += std::to_string(m);
        } else if (t.len == 2) {
          std::snprintf(buf, sizeof buf, "%02d", m);
          out += buf;
        } else if (t.len == 3) {
          out += std::string(kMonthNames[m - 1]).substr(0, 3);
        } else if (t.len == 5) {
          out += kMonthNames[m - 1][0];
        } else {
          out += kMonthNames[m - 1];
        }
        break;
      case Tok::Day:
        if (t.len == 1) {
          out += std::to_string(d);
        } else if (t.len == 2) {
          std::snprintf(buf, sizeof buf, "%02d", d);
          out += buf;
        } else if (t.len == 3) {
          out += std::string(kDayNames[wd]).substr(0, 3);
        } else {
          out += kDayNames[wd];
        }
        break;
      case Tok::Hour: {
        if (elapsed_h) {
          out += pad(eh, std::min(t.len, 2));
          break;
        }
        int hh = h;
        if (sec.has_ampm) {
          hh = h % 12;
          if (hh == 0) hh = 12;
        }
        out += pad(hh, std::min(t.len, 2));
        break;
      }
      case Tok::Minute:
        out += pad(elapsed_m ? em : mi, std::min(t.len, 2));
        break;
      case Tok::Second:
        out += pad(elapsed_s ? es : s, std::min(t.len, 2));
        break;
      case Tok::SubSecond: {
        std::string digits = std::to_string(sub_value);
        while (static_cast<int>(digits.size()) < sub) digits.insert(0, "0");
        out += ".";
        out += digits.substr(0, static_cast<size_t>(t.len));
        break;
      }
      case Tok::AmPm: {
        bool pm = h >= 12;
        if (t.len == 5) {
          out += pm ? "PM" : "AM";
        } else {
          bool lower_case = t.text[0] == 'a';
          out += pm ? (lower_case ? "p" : "P") : (lower_case ? "a" : "A");
        }
        break;
      }
      case Tok::ElapsedH:
        out += pad(eh, t.len);
        break;
      case Tok::ElapsedM:
        out += pad(em, t.len);
        break;
      case Tok::ElapsedS:
        out += pad(es, t.len);
        break;
      case Tok::Point:
        out += ".";
        break;
      case Tok::Comma:
        out += ",";
        break;
      case Tok::Percent:
        out += "%";
        break;
      case Tok::Slash:
        out += "/";
        break;
      case Tok::Zero:
        out += "0";
        break;
      default:
        break;
    }
  }
  return true;
}

int SignificantDigits(const std::string& text) {
  std::string d;
  for (char c : text) {
    if (IsDigit(c)) d.push_back(c);
  }
  size_t lead = d.find_first_not_of('0');
  if (lead == std::string::npos) return 0;
  d = d.substr(lead);
  if (text.find('.') == std::string::npos) {
    while (d.size() > 1 && d.back() == '0') d.pop_back();
  }
  return static_cast<int>(d.size());
}

std::string GeneralDisplay(double x) {
  if (x == 0 || !std::isfinite(x)) return "0";
  double a = std::fabs(x);
  char buf[64];
  int e10 = DecimalExponent(a);
  std::snprintf(buf, sizeof buf, "%.*e", std::abs(e10) >= 99 ? 4 : 5, a);
  std::string mant(buf, std::strchr(buf, 'e'));
  int ex = std::atoi(std::strchr(buf, 'e') + 1);
  while (mant.back() == '0') mant.pop_back();
  if (mant.back() == '.') mant.pop_back();
  std::string exps = std::to_string(std::abs(ex));
  if (exps.size() < 2) exps.insert(0, "0");
  std::string sci = mant + (ex < 0 ? "E-" : "E+") + exps;
  std::string fixed;
  if (a < 1e11) {
    int k = a >= 1 ? e10 + 1 : 1;
    int decimals = std::max(0, 10 - k);
    std::snprintf(buf, sizeof buf, "%.*f", decimals, RoundDigits(a, decimals));
    fixed = buf;
    if (fixed.find('.') != std::string::npos) {
      while (fixed.back() == '0') fixed.pop_back();
      if (fixed.back() == '.') fixed.pop_back();
    }
    if (fixed.size() > 11) fixed.clear();
  }
  bool use_fixed =
      !fixed.empty() && fixed != "0" &&
      (a >= 1 || SignificantDigits(fixed) >= SignificantDigits(sci.substr(0, sci.find('E'))));
  std::string out = use_fixed ? fixed : sci;
  return x < 0 ? "-" + out : out;
}

bool CondMatches(const Section& s, double v) {
  const std::string& op = s.cond_op;
  if (op == "<") return v < s.cond_value;
  if (op == "<=") return v <= s.cond_value;
  if (op == ">") return v > s.cond_value;
  if (op == ">=") return v >= s.cond_value;
  if (op == "=") return v == s.cond_value;
  if (op == "<>") return v != s.cond_value;
  return false;
}

}

bool FormatValue(const Value& v, std::string_view format, const DateSystem& ds, std::string& out) {
  out.clear();
  std::vector<std::string> parts = SplitSections(format);
  std::vector<Section> secs(parts.size());
  for (size_t i = 0; i < parts.size(); ++i) {
    if (!ParseSection(parts[i], secs[i])) return false;
  }
  if (v.IsText() || v.IsBool()) {
    std::string text = v.IsText() ? std::string(v.text()) : (v.boolean() ? "TRUE" : "FALSE");
    const Section* use = nullptr;
    if (secs.size() >= 4) {
      use = &secs[3];
    } else {
      for (const Section& s : secs) {
        if (s.has_at) {
          use = &s;
          break;
        }
      }
    }
    if (!use) {
      out = text;
      return true;
    }
    for (const FmtTok& t : use->toks) {
      if (t.kind == Tok::At) {
        out += text;
      } else if (t.kind == Tok::Literal) {
        out += t.text;
      }
    }
    return true;
  }
  double x = v.IsNumber() ? v.number() : 0.0;
  const Section* use = &secs[0];
  bool add_minus = false;
  bool any_cond = false;
  for (const Section& s : secs) any_cond = any_cond || s.has_cond;
  if (any_cond && secs.size() == 1) {
    if (!CondMatches(secs[0], x)) {
      out = GeneralDisplay(x);
      return true;
    }
    add_minus = x < 0;
  } else if (any_cond) {
    use = nullptr;
    for (size_t i = 0; i < 2 && i < secs.size(); ++i) {
      if (!secs[i].has_cond || CondMatches(secs[i], x)) {
        use = &secs[i];
        break;
      }
    }
    if (!use && secs.size() >= 3) {
      use = &secs[2];
      add_minus = x < 0;
    }
    if (!use) {
      out = GeneralDisplay(x);
      return true;
    }
  } else if (secs.size() == 1 || (secs.size() >= 2 && x > 0) || (secs.size() == 2 && x == 0)) {
    use = &secs[0];
    add_minus = x < 0;
  } else if (x < 0) {
    use = &secs[1];
  } else {
    use = secs.size() >= 3 ? &secs[2] : &secs[0];
  }
  if (use->toks.empty() && use->empty) return true;
  bool general = false;
  for (const FmtTok& t : use->toks) general = general || t.kind == Tok::General;
  if (general) {
    for (const FmtTok& t : use->toks) {
      if (t.kind == Tok::General) {
        out += GeneralDisplay(add_minus ? -std::fabs(x) : std::fabs(x));
      } else if (t.kind == Tok::Literal) {
        out += t.text;
      }
    }
    return true;
  }
  if (use->is_date) return FormatDate(*use, add_minus ? x : std::fabs(x), ds, out);
  if (!use->has_digits) {
    if (add_minus) out.push_back('-');
    for (const FmtTok& t : use->toks) {
      if (t.kind == Tok::Literal || t.kind == Tok::Percent || t.kind == Tok::Comma ||
          t.kind == Tok::Point || t.kind == Tok::Slash) {
        out += t.text;
      } else if (t.kind == Tok::At) {
        out += FormatGeneral(x);
      }
    }
    return true;
  }
  return FormatNumber(*use, x, add_minus, out);
}

}
