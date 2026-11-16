#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

#include "eval.h"

namespace sheet {

namespace {

#define NUM_ARG(i, var) \
  double var;           \
  if (Err e_ = ArgNumber(ctx, a[i], var); e_ != Err::None) return MakeErr(e_)

#define TEXT_ARG(i, var) \
  std::string var;       \
  if (Err e_ = ArgText(ctx, a[i], var); e_ != Err::None) return MakeErr(e_)

constexpr size_t kMaxText = 32767;

const uint16_t kCp1252[32] = {0x20AC, 0x81,   0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                              0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D,   0x017D, 0x8F,
                              0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                              0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D,   0x017E, 0x0178};

Operand Text(std::string s) {
  if (s.size() > kMaxText) return MakeErr(Err::Value);
  return MakeText(s);
}

std::string Substr(const std::string& s, int start_unit, int count) {
  size_t b = Utf8Offset(s, start_unit);
  size_t e = Utf8Offset(s, start_unit + count);
  if (b > s.size()) b = s.size();
  if (e > s.size()) e = s.size();
  return s.substr(b, e - b);
}

Operand FnLen(EvalContext& ctx, Operand* a, int) {
  TEXT_ARG(0, s);
  return MakeNum(Utf8Units(s));
}
Operand FnLeft(EvalContext& ctx, Operand* a, int n) {
  TEXT_ARG(0, s);
  double k;
  if (Err e = ArgNumberOr(ctx, a, n, 1, 1, k); e != Err::None) return MakeErr(e);
  if (k < 0) return MakeErr(Err::Value);
  int count = static_cast<int>(std::min(k, 1e9));
  return MakeText(Substr(s, 0, count));
}
Operand FnRight(EvalContext& ctx, Operand* a, int n) {
  TEXT_ARG(0, s);
  double k;
  if (Err e = ArgNumberOr(ctx, a, n, 1, 1, k); e != Err::None) return MakeErr(e);
  if (k < 0) return MakeErr(Err::Value);
  int len = Utf8Units(s);
  int count = static_cast<int>(std::min(k, 1e9));
  if (count >= len) return MakeText(s);
  return MakeText(Substr(s, len - count, count));
}
Operand FnMid(EvalContext& ctx, Operand* a, int) {
  TEXT_ARG(0, s);
  NUM_ARG(1, start);
  NUM_ARG(2, k);
  if (start < 1 || k < 0) return MakeErr(Err::Value);
  int st = static_cast<int>(std::min(start, 1e9));
  int count = static_cast<int>(std::min(k, 1e9));
  return MakeText(Substr(s, st - 1, count));
}
Operand FnUpper(EvalContext& ctx, Operand* a, int) {
  TEXT_ARG(0, s);
  return MakeText(ToUpper(s));
}
Operand FnLower(EvalContext& ctx, Operand* a, int) {
  TEXT_ARG(0, s);
  return MakeText(ToLower(s));
}
Operand FnProper(EvalContext& ctx, Operand* a, int) {
  TEXT_ARG(0, s);
  std::string out;
  bool prev_letter = false;
  size_t i = 0;
  while (i < s.size()) {
    size_t start = i;
    Utf8Decode(s, i);
    std::string ch = s.substr(start, i - start);
    std::string up = ToUpper(ch);
    std::string lo = ToLower(ch);
    bool letter = up != lo;
    out += letter ? (prev_letter ? lo : up) : ch;
    prev_letter = letter;
  }
  return MakeText(out);
}
Operand FnTrim(EvalContext& ctx, Operand* a, int) {
  TEXT_ARG(0, s);
  std::string out;
  bool pending = false;
  for (char c : s) {
    if (c == ' ') {
      pending = !out.empty();
      continue;
    }
    if (pending) out.push_back(' ');
    pending = false;
    out.push_back(c);
  }
  return MakeText(out);
}
Operand FnClean(EvalContext& ctx, Operand* a, int) {
  TEXT_ARG(0, s);
  std::string out;
  for (char c : s) {
    if (static_cast<unsigned char>(c) >= 32) out.push_back(c);
  }
  return MakeText(out);
}
Operand FnConcatenate(EvalContext& ctx, Operand* a, int n) {
  std::string out;
  for (int i = 0; i < n; ++i) {
    TEXT_ARG(i, s);
    out += s;
    if (out.size() > kMaxText) return MakeErr(Err::Value);
  }
  return Text(out);
}

Err AppendAll(EvalContext& ctx, const Operand& op, std::vector<Value>& out) {
  if (op.IsMissing()) {
    out.push_back(Value::Text(""));
    return Err::None;
  }
  if (op.tag == Operand::Tag::Ref) {
    const Area& ar = op.area;
    int r2 = std::min(ar.r2, ctx.UsedRows(ar.sheet) - 1);
    int c2 = std::min(ar.c2, ctx.UsedCols(ar.sheet) - 1);
    for (int r = ar.r1; r <= r2; ++r) {
      for (int c = ar.c1; c <= c2; ++c) out.push_back(ctx.Read(ar.sheet, r, c));
    }
    return Err::None;
  }
  if (op.tag == Operand::Tag::Refs) {
    for (const Area& ar : *op.list) {
      Operand one = Operand::OfRef(ar);
      if (Err e = AppendAll(ctx, one, out); e != Err::None) return e;
    }
    return Err::None;
  }
  const Value& v = op.value;
  if (v.IsArray()) {
    for (uint32_t r = 0; r < v.rows(); ++r) {
      for (uint32_t c = 0; c < v.cols(); ++c) out.push_back(v.at(r, c));
    }
    return Err::None;
  }
  out.push_back(v);
  return Err::None;
}

Operand FnConcat(EvalContext& ctx, Operand* a, int n) {
  std::string out;
  for (int i = 0; i < n; ++i) {
    std::vector<Value> vals;
    AppendAll(ctx, a[i], vals);
    for (const Value& v : vals) {
      if (v.IsError()) return MakeErr(v.error());
      out += ToText(v);
      if (out.size() > kMaxText) return MakeErr(Err::Value);
    }
  }
  return Text(out);
}

Operand FnTextJoin(EvalContext& ctx, Operand* a, int n) {
  std::vector<Value> delims;
  AppendAll(ctx, a[0], delims);
  bool ignore;
  if (Err e = ArgBool(ctx, a[1], ignore); e != Err::None) return MakeErr(e);
  std::vector<std::string> parts;
  for (int i = 2; i < n; ++i) {
    std::vector<Value> vals;
    AppendAll(ctx, a[i], vals);
    for (const Value& v : vals) {
      if (v.IsError()) return MakeErr(v.error());
      std::string t = ToText(v);
      if (ignore && t.empty()) continue;
      parts.push_back(std::move(t));
    }
  }
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0 && !delims.empty()) {
      const Value& d = delims[(i - 1) % delims.size()];
      if (d.IsError()) return MakeErr(d.error());
      out += ToText(d);
    }
    out += parts[i];
    if (out.size() > kMaxText) return MakeErr(Err::Value);
  }
  return Text(out);
}

Operand FnRept(EvalContext& ctx, Operand* a, int) {
  TEXT_ARG(0, s);
  NUM_ARG(1, k0);
  if (k0 < 0) return MakeErr(Err::Value);
  double k = std::floor(k0);
  if (s.empty() || k == 0) return MakeText("");
  if (static_cast<double>(s.size()) * k > static_cast<double>(kMaxText)) return MakeErr(Err::Value);
  std::string out;
  for (int i = 0; i < static_cast<int>(k); ++i) out += s;
  return MakeText(out);
}

Operand FnSubstitute(EvalContext& ctx, Operand* a, int n) {
  TEXT_ARG(0, s);
  TEXT_ARG(1, from);
  TEXT_ARG(2, to);
  if (from.empty()) return MakeText(s);
  bool has_instance = n > 3 && !a[3].IsMissing();
  double instance = 0;
  if (has_instance) {
    if (Err e = ArgNumber(ctx, a[3], instance); e != Err::None) return MakeErr(e);
    if (instance < 1) return MakeErr(Err::Value);
  }
  std::string out;
  size_t pos = 0;
  int seen = 0;
  while (true) {
    size_t hit = s.find(from, pos);
    if (hit == std::string::npos) break;
    ++seen;
    if (!has_instance || seen == ToInt(instance)) {
      out += s.substr(pos, hit - pos);
      out += to;
      pos = hit + from.size();
      if (has_instance) break;
    } else {
      out += s.substr(pos, hit + from.size() - pos);
      pos = hit + from.size();
    }
  }
  out += s.substr(pos);
  return Text(out);
}

Operand FnReplace(EvalContext& ctx, Operand* a, int) {
  TEXT_ARG(0, s);
  NUM_ARG(1, start);
  NUM_ARG(2, k);
  TEXT_ARG(3, with);
  if (start < 1 || k < 0) return MakeErr(Err::Value);
  int st = static_cast<int>(std::min(start, 1e9)) - 1;
  int count = static_cast<int>(std::min(k, 1e9));
  size_t b = Utf8Offset(s, st);
  size_t e = Utf8Offset(s, st + count);
  std::string out = s.substr(0, b) + with + s.substr(std::min(e, s.size()));
  return Text(out);
}

Operand FnFind(EvalContext& ctx, Operand* a, int n) {
  TEXT_ARG(0, needle);
  TEXT_ARG(1, hay);
  double start;
  if (Err e = ArgNumberOr(ctx, a, n, 2, 1, start); e != Err::None) return MakeErr(e);
  int len = Utf8Units(hay);
  if (start < 1 || start > len + 1) return MakeErr(Err::Value);
  int st = static_cast<int>(start) - 1;
  size_t from = Utf8Offset(hay, st);
  size_t hit = hay.find(needle, from);
  if (hit == std::string::npos) return MakeErr(Err::Value);
  return MakeNum(Utf8Units(std::string_view(hay).substr(0, hit)) + 1);
}

Operand FnSearch(EvalContext& ctx, Operand* a, int n) {
  TEXT_ARG(0, needle);
  TEXT_ARG(1, hay);
  double start;
  if (Err e = ArgNumberOr(ctx, a, n, 2, 1, start); e != Err::None) return MakeErr(e);
  int len = Utf8Units(hay);
  if (start < 1 || start > len + 1) return MakeErr(Err::Value);
  int st = static_cast<int>(start) - 1;
  if (needle.empty()) return MakeNum(st + 1);
  bool wild = HasWildcards(needle);
  std::string pattern = wild ? needle + "*" : needle;
  std::string lower_hay = ToLower(hay);
  std::string lower_needle = ToLower(needle);
  for (int u = st; u < len; ++u) {
    size_t off = Utf8Offset(hay, u);
    if (wild) {
      if (WildcardMatch(pattern, std::string_view(hay).substr(off))) return MakeNum(u + 1);
    } else {
      size_t loff = Utf8Offset(lower_hay, u);
      if (lower_hay.compare(loff, lower_needle.size(), lower_needle) == 0) return MakeNum(u + 1);
    }
  }
  return MakeErr(Err::Value);
}

Operand FnExact(EvalContext& ctx, Operand* a, int) {
  TEXT_ARG(0, x);
  TEXT_ARG(1, y);
  return MakeBool(x == y);
}

Operand FnChar(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  int c = ToInt(x);
  if (c < 1 || c > 255) return MakeErr(Err::Value);
  uint32_t cp = static_cast<uint32_t>(c);
  if (c >= 0x80 && c <= 0x9F) cp = kCp1252[c - 0x80];
  return MakeText(Utf8Encode(cp));
}
Operand FnCode(EvalContext& ctx, Operand* a, int) {
  TEXT_ARG(0, s);
  if (s.empty()) return MakeErr(Err::Value);
  size_t i = 0;
  uint32_t cp = Utf8Decode(s, i);
  if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF)) return MakeNum(cp);
  for (int k = 0; k < 32; ++k) {
    if (kCp1252[k] == cp) return MakeNum(0x80 + k);
  }
  return MakeNum(63);
}
Operand FnUniChar(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  double c = std::trunc(x);
  if (c < 1 || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return MakeErr(Err::Value);
  return MakeText(Utf8Encode(static_cast<uint32_t>(c)));
}
Operand FnUnicode(EvalContext& ctx, Operand* a, int) {
  TEXT_ARG(0, s);
  if (s.empty()) return MakeErr(Err::Value);
  size_t i = 0;
  return MakeNum(Utf8Decode(s, i));
}
Operand FnT(EvalContext& ctx, Operand* a, int) {
  Value v;
  if (a[0].tag == Operand::Tag::Ref) {
    v = ctx.Read(a[0].area.sheet, a[0].area.r1, a[0].area.c1);
  } else {
    v = ctx.Scalar(a[0]);
  }
  if (v.IsError()) return Operand::Of(v);
  if (v.IsText()) return Operand::Of(v);
  return MakeText("");
}
Operand FnValue(EvalContext& ctx, Operand* a, int) {
  Value v = ctx.Scalar(a[0]);
  if (v.IsError()) return Operand::Of(v);
  if (v.IsNumber()) return Operand::Of(v);
  if (v.IsBlank()) return MakeNum(0);
  if (v.IsBool()) return MakeErr(Err::Value);
  double d;
  if (!ParseNumberText(v.text(), ctx.dates(), d)) return MakeErr(Err::Value);
  return MakeNum(d);
}
Operand FnNumberValue(EvalContext& ctx, Operand* a, int n) {
  TEXT_ARG(0, s);
  std::string dec = ".";
  std::string grp = ",";
  if (n > 1 && !a[1].IsMissing()) {
    if (Err e = ArgText(ctx, a[1], dec); e != Err::None) return MakeErr(e);
    if (dec.empty()) return MakeErr(Err::Value);
    dec = dec.substr(0, 1);
  }
  if (n > 2 && !a[2].IsMissing()) {
    if (Err e = ArgText(ctx, a[2], grp); e != Err::None) return MakeErr(e);
    grp = grp.substr(0, 1);
  }
  if (!grp.empty() && grp == dec) return MakeErr(Err::Value);
  std::string clean;
  bool seen_dec = false;
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
    if (!dec.empty() && c == dec[0]) {
      if (seen_dec) return MakeErr(Err::Value);
      seen_dec = true;
      clean.push_back('.');
      continue;
    }
    if (!grp.empty() && c == grp[0]) {
      if (seen_dec) return MakeErr(Err::Value);
      continue;
    }
    clean.push_back(c);
  }
  int pct = 0;
  while (!clean.empty() && clean.back() == '%') {
    ++pct;
    clean.pop_back();
  }
  if (clean.empty()) return MakeNum(0);
  double v;
  bool neg = false;
  std::string body = clean;
  if (!body.empty() && (body[0] == '-' || body[0] == '+')) {
    neg = body[0] == '-';
    body = body.substr(1);
  }
  if (!ParseNumberLiteral(body, v)) return MakeErr(Err::Value);
  for (int i = 0; i < pct; ++i) v /= 100;
  return MakeNum(neg ? -v : v);
}
Operand FnText(EvalContext& ctx, Operand* a, int) {
  Value v = ctx.Scalar(a[0]);
  if (v.IsError()) return Operand::Of(v);
  TEXT_ARG(1, fmt);
  if (v.IsText()) {
    double d;
    if (ParseNumberText(v.text(), ctx.dates(), d)) v = Value::Number(d);
  }
  std::string out;
  if (!FormatValue(v, fmt, ctx.dates(), out)) return MakeErr(Err::Value);
  return Text(out);
}
Operand FnFixed(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, x);
  double d0;
  if (Err e = ArgNumberOr(ctx, a, n, 1, 2, d0); e != Err::None) return MakeErr(e);
  bool no_commas;
  if (Err e = ArgBoolOr(ctx, a, n, 2, false, no_commas); e != Err::None) return MakeErr(e);
  int d = ToInt(d0);
  if (d > 127) return MakeErr(Err::Value);
  double r = RoundDigits(x, d);
  std::string fmt = no_commas ? "0" : "#,##0";
  if (d > 0) fmt += "." + std::string(static_cast<size_t>(d), '0');
  std::string out;
  if (!FormatValue(Value::Number(r), fmt, ctx.dates(), out)) return MakeErr(Err::Value);
  return MakeText(out);
}
Operand FnDollar(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, x);
  double d0;
  if (Err e = ArgNumberOr(ctx, a, n, 1, 2, d0); e != Err::None) return MakeErr(e);
  int d = ToInt(d0);
  if (d > 127) return MakeErr(Err::Value);
  double r = RoundDigits(x, d);
  std::string body = "#,##0";
  if (d > 0) body += "." + std::string(static_cast<size_t>(d), '0');
  std::string fmt = "$" + body + ";($" + body + ")";
  std::string out;
  if (!FormatValue(Value::Number(r), fmt, ctx.dates(), out)) return MakeErr(Err::Value);
  return MakeText(out);
}

Err Delimiters(EvalContext& ctx, const Operand& op, std::vector<std::string>& out) {
  if (op.IsError()) return op.value.error();
  Grid g;
  if (op.tag == Operand::Tag::Ref || (op.tag == Operand::Tag::Val && op.value.IsArray())) {
    if (!MakeGrid(ctx, op, g, false)) return Err::Value;
    for (int r = 0; r < g.rows; ++r) {
      for (int c = 0; c < g.cols; ++c) {
        const Value& v = g.At(r, c);
        if (v.IsError()) return v.error();
        out.push_back(ToText(v));
      }
    }
    return Err::None;
  }
  std::string d;
  if (Err e = ArgText(ctx, op, d); e != Err::None) return e;
  out.push_back(d);
  return Err::None;
}

Operand TextAround(EvalContext& ctx, Operand* a, int n, bool before) {
  TEXT_ARG(0, s);
  std::vector<std::string> delims;
  if (Err e = Delimiters(ctx, a[1], delims); e != Err::None) return MakeErr(e);
  double inst;
  if (Err e = ArgNumberOr(ctx, a, n, 2, 1, inst); e != Err::None) return MakeErr(e);
  double mode;
  if (Err e = ArgNumberOr(ctx, a, n, 3, 0, mode); e != Err::None) return MakeErr(e);
  double match_end;
  if (Err e = ArgNumberOr(ctx, a, n, 4, 0, match_end); e != Err::None) return MakeErr(e);
  int k = ToInt(inst);
  if (k == 0 || std::fabs(inst) > static_cast<double>(s.size()) + 1) return MakeErr(Err::Value);
  std::string hay = mode != 0 ? ToLower(s) : s;
  std::vector<std::string> needles;
  for (const std::string& d : delims) {
    if (!d.empty()) needles.push_back(mode != 0 ? ToLower(d) : d);
  }
  struct Hit {
    size_t pos;
    size_t len;
  };
  std::vector<Hit> hits;
  if (needles.empty()) {
    hits.push_back(Hit{k > 0 ? 0 : s.size(), 0});
  } else {
    size_t pos = 0;
    while (pos <= hay.size()) {
      size_t best = std::string::npos;
      size_t best_len = 0;
      for (const std::string& nd : needles) {
        size_t h = hay.find(nd, pos);
        if (h != std::string::npos && (best == std::string::npos || h < best)) {
          best = h;
          best_len = nd.size();
        }
      }
      if (best == std::string::npos) break;
      hits.push_back(Hit{best, best_len});
      pos = best + best_len;
    }
    if (match_end != 0) {
      if (k > 0) hits.push_back(Hit{s.size(), 0});
      if (k < 0) hits.insert(hits.begin(), Hit{0, 0});
    }
  }
  size_t want = static_cast<size_t>(std::abs(k));
  if (want > hits.size()) {
    if (n > 5 && !a[5].IsMissing()) return a[5];
    return MakeErr(Err::NA);
  }
  const Hit& h = k > 0 ? hits[want - 1] : hits[hits.size() - want];
  if (before) return MakeText(s.substr(0, h.pos));
  return MakeText(s.substr(std::min(s.size(), h.pos + h.len)));
}
Operand FnTextBefore(EvalContext& ctx, Operand* a, int n) {
  return TextAround(ctx, a, n, true);
}
Operand FnTextAfter(EvalContext& ctx, Operand* a, int n) {
  return TextAround(ctx, a, n, false);
}

constexpr uint16_t kHalfKana[] = {
    0x3002, 0x300C, 0x300D, 0x3001, 0x30FB, 0x30F2, 0x30A1, 0x30A3, 0x30A5, 0x30A7, 0x30A9,
    0x30E3, 0x30E5, 0x30E7, 0x30C3, 0x30FC, 0x30A2, 0x30A4, 0x30A6, 0x30A8, 0x30AA, 0x30AB,
    0x30AD, 0x30AF, 0x30B1, 0x30B3, 0x30B5, 0x30B7, 0x30B9, 0x30BB, 0x30BD, 0x30BF, 0x30C1,
    0x30C4, 0x30C6, 0x30C8, 0x30CA, 0x30CB, 0x30CC, 0x30CD, 0x30CE, 0x30CF, 0x30D2, 0x30D5,
    0x30D8, 0x30DB, 0x30DE, 0x30DF, 0x30E0, 0x30E1, 0x30E2, 0x30E4, 0x30E6, 0x30E8, 0x30E9,
    0x30EA, 0x30EB, 0x30EC, 0x30ED, 0x30EF, 0x30F3, 0x309B, 0x309C,
};

constexpr uint32_t kWideSigns[][2] = {
    {0x22, 0x201D}, {0x27, 0x2019}, {0x5C, 0xFFE5},   {0x60, 0x2018},
    {0x20, 0x3000}, {0xA2, 0xFFE0}, {0xA3, 0xFFE1},   {0xAC, 0xFFE2},
    {0xAF, 0xFFE3}, {0xA6, 0xFFE4}, {0x20A9, 0xFFE6},
};

uint32_t VoicedKana(uint32_t base, bool semi) {
  if (semi) {
    return (base == 0x30CF || base == 0x30D2 || base == 0x30D5 || base == 0x30D8 || base == 0x30DB)
               ? base + 2
               : 0;
  }
  if (base == 0x30A6) return 0x30F4;
  if (base == 0x30EF) return 0x30F7;
  if (base == 0x30F2) return 0x30FA;
  if (base >= 0x30AB && base <= 0x30C1) return (base - 0x30AB) % 2 == 0 ? base + 1 : 0;
  if (base == 0x30C4 || base == 0x30C6 || base == 0x30C8) return base + 1;
  if (base >= 0x30CF && base <= 0x30DB) return (base - 0x30CF) % 3 == 0 ? base + 1 : 0;
  return 0;
}

int HalfKanaIndex(uint32_t full) {
  for (size_t i = 0; i < std::size(kHalfKana); ++i) {
    if (kHalfKana[i] == full) return static_cast<int>(i);
  }
  return -1;
}

std::string ToHalfWidth(std::string_view s) {
  std::string out;
  size_t i = 0;
  while (i < s.size()) {
    uint32_t c = Utf8Decode(s, i);
    if (c >= 0xFF01 && c <= 0xFF5E) {
      out.push_back(static_cast<char>(c - 0xFEE0));
      continue;
    }
    bool done = false;
    for (const auto& w : kWideSigns) {
      if (w[1] == c) {
        out += Utf8Encode(w[0]);
        done = true;
        break;
      }
    }
    if (done) continue;
    if (c >= 0x30A1 && c <= 0x30FA) {
      for (uint32_t base = 0x30A1; base <= 0x30F3 && !done; ++base) {
        for (int semi = 0; semi < 2 && !done; ++semi) {
          if (VoicedKana(base, semi != 0) == c) {
            out += Utf8Encode(0xFF61 + static_cast<uint32_t>(HalfKanaIndex(base)));
            out += Utf8Encode(semi != 0 ? 0xFF9F : 0xFF9E);
            done = true;
          }
        }
      }
      if (done) continue;
    }
    int k = HalfKanaIndex(c);
    out += Utf8Encode(k >= 0 ? 0xFF61 + static_cast<uint32_t>(k) : c);
  }
  return out;
}

std::string ToFullWidth(std::string_view s) {
  std::string out;
  size_t i = 0;
  while (i < s.size()) {
    uint32_t c = Utf8Decode(s, i);
    bool done = false;
    for (const auto& w : kWideSigns) {
      if (w[0] == c) {
        out += Utf8Encode(w[1]);
        done = true;
        break;
      }
    }
    if (done) continue;
    if (c >= 0x21 && c <= 0x7E) {
      out += Utf8Encode(c + 0xFEE0);
      continue;
    }
    if (c >= 0xFF61 && c <= 0xFF9F) {
      uint32_t full = kHalfKana[c - 0xFF61];
      size_t j = i;
      if (j < s.size()) {
        uint32_t next = Utf8Decode(s, j);
        if (next == 0xFF9E || next == 0xFF9F) {
          uint32_t voiced = VoicedKana(full, next == 0xFF9F);
          if (voiced != 0 && voiced < 0x30F4) {
            out += Utf8Encode(voiced);
            i = j;
            continue;
          }
        }
      }
      out += Utf8Encode(full);
      continue;
    }
    out += Utf8Encode(c);
  }
  return out;
}

Operand FnAsc(EvalContext& ctx, Operand* a, int) {
  TEXT_ARG(0, s);
  return MakeText(ToHalfWidth(s));
}

Operand FnJis(EvalContext& ctx, Operand* a, int) {
  TEXT_ARG(0, s);
  return MakeText(ToFullWidth(s));
}

}

void RegisterText(std::vector<FuncInfo>& out) {
  const FuncInfo list[] = {
      {"LEN", 1, 1, "V", 1, kFnLift, FnLen},
      {"LENB", 1, 1, "V", 1, kFnLift, FnLen},
      {"LEFT", 1, 2, "VV", 1, kFnLift, FnLeft},
      {"LEFTB", 1, 2, "VV", 1, kFnLift, FnLeft},
      {"RIGHT", 1, 2, "VV", 1, kFnLift, FnRight},
      {"RIGHTB", 1, 2, "VV", 1, kFnLift, FnRight},
      {"MID", 3, 3, "VVV", 1, kFnLift, FnMid},
      {"MIDB", 3, 3, "VVV", 1, kFnLift, FnMid},
      {"UPPER", 1, 1, "V", 1, kFnLift, FnUpper},
      {"LOWER", 1, 1, "V", 1, kFnLift, FnLower},
      {"PROPER", 1, 1, "V", 1, kFnLift, FnProper},
      {"TRIM", 1, 1, "V", 1, kFnLift, FnTrim},
      {"CLEAN", 1, 1, "V", 1, kFnLift, FnClean},
      {"CONCATENATE", 1, 255, "V", 1, kFnLift, FnConcatenate},
      {"CONCAT", 1, 255, "R", 1, kFnXlfn, FnConcat},
      {"TEXTJOIN", 3, 255, "RVR", 1, kFnXlfn, FnTextJoin},
      {"REPT", 2, 2, "VV", 1, kFnLift, FnRept},
      {"SUBSTITUTE", 3, 4, "VVVV", 1, kFnLift, FnSubstitute},
      {"REPLACE", 4, 4, "VVVV", 1, kFnLift, FnReplace},
      {"REPLACEB", 4, 4, "VVVV", 1, kFnLift, FnReplace},
      {"FIND", 2, 3, "VVV", 1, kFnLift, FnFind},
      {"FINDB", 2, 3, "VVV", 1, kFnLift, FnFind},
      {"SEARCH", 2, 3, "VVV", 1, kFnLift, FnSearch},
      {"SEARCHB", 2, 3, "VVV", 1, kFnLift, FnSearch},
      {"EXACT", 2, 2, "VV", 1, kFnLift, FnExact},
      {"CHAR", 1, 1, "V", 1, kFnLift, FnChar},
      {"CODE", 1, 1, "V", 1, kFnLift, FnCode},
      {"UNICHAR", 1, 1, "V", 1, kFnLift | kFnXlfn, FnUniChar},
      {"UNICODE", 1, 1, "V", 1, kFnLift | kFnXlfn, FnUnicode},
      {"T", 1, 1, "P", 1, 0, FnT},
      {"VALUE", 1, 1, "V", 1, kFnLift, FnValue},
      {"NUMBERVALUE", 1, 3, "VVV", 1, kFnLift | kFnXlfn, FnNumberValue},
      {"TEXT", 2, 2, "VV", 1, kFnLift, FnText},
      {"FIXED", 1, 3, "VVV", 1, kFnLift, FnFixed},
      {"DOLLAR", 1, 2, "VV", 1, kFnLift, FnDollar},
      {"TEXTBEFORE", 2, 6, "VRVVVV", 1, kFnLift | kFnXlfn, FnTextBefore},
      {"TEXTAFTER", 2, 6, "VRVVVV", 1, kFnLift | kFnXlfn, FnTextAfter},
      {"ASC", 1, 1, "V", 1, kFnLift, FnAsc},
      {"JIS", 1, 1, "V", 1, kFnLift, FnJis},
  };
  out.insert(out.end(), std::begin(list), std::end(list));
}

}
