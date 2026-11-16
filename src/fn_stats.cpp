#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "eval.h"

namespace sheet {

namespace {

constexpr double kPi = 3.14159265358979323846;

#define NUM_ARG(i, var) \
  double var;           \
  if (Err e_ = ArgNumber(ctx, a[i], var); e_ != Err::None) return MakeErr(e_)

double Sum(const std::vector<double>& xs) {
  double s = 0;
  for (double x : xs) s += x;
  return s;
}

double Mean(const std::vector<double>& xs) {
  return Sum(xs) / static_cast<double>(xs.size());
}

double SumSqDev(const std::vector<double>& xs) {
  double m = Mean(xs);
  std::vector<double> d;
  d.reserve(xs.size());
  for (double x : xs) d.push_back((x - m) * (x - m));
  return Sum(d);
}

struct Pairs {
  std::vector<double> x;
  std::vector<double> y;
  Err err = Err::None;
};

Pairs CollectPairs(EvalContext& ctx, const Operand& a, const Operand& b) {
  Pairs p;
  Grid ga;
  Grid gb;
  if (!MakeGrid(ctx, a, ga, false) || !MakeGrid(ctx, b, gb, false)) {
    p.err = Err::Value;
    return p;
  }
  if (ga.rows * ga.cols != gb.rows * gb.cols) {
    p.err = Err::NA;
    return p;
  }
  int n = ga.rows * ga.cols;
  for (int i = 0; i < n; ++i) {
    const Value& u = ga.At(i / ga.cols, i % ga.cols);
    const Value& v = gb.At(i / gb.cols, i % gb.cols);
    if (u.IsError()) {
      p.err = u.error();
      return p;
    }
    if (v.IsError()) {
      p.err = v.error();
      return p;
    }
    if (u.IsNumber() && v.IsNumber()) {
      p.x.push_back(u.number());
      p.y.push_back(v.number());
    }
  }
  return p;
}

double LogGamma(double x) {
  static const double g[] = {0.99999999999980993,  676.5203681218851,     -1259.1392167224028,
                             771.32342877765313,   -176.61502916214059,   12.507343278686905,
                             -0.13857109526572012, 9.9843695780195716e-6, 1.5056327351493116e-7};
  if (x < 0.5) return std::log(kPi / std::fabs(std::sin(kPi * x))) - LogGamma(1 - x);
  x -= 1;
  double s = g[0];
  for (int i = 1; i < 9; ++i) s += g[i] / (x + i);
  double t = x + 7.5;
  return 0.5 * std::log(2 * kPi) + (x + 0.5) * std::log(t) - t + std::log(s);
}

double GammaP(double a, double x) {
  if (x <= 0) return 0;
  if (x < a + 1) {
    double sum = 1.0 / a;
    double del = sum;
    double ap = a;
    for (int n = 0; n < 10000; ++n) {
      ap += 1;
      del *= x / ap;
      sum += del;
      if (std::fabs(del) < std::fabs(sum) * 1e-16) break;
    }
    return sum * std::exp(-x + a * std::log(x) - LogGamma(a));
  }
  double b = x + 1 - a;
  double c = 1.0 / 1e-300;
  double d = 1.0 / b;
  double h = d;
  for (int i = 1; i < 10000; ++i) {
    double an = -i * (i - a);
    b += 2;
    d = an * d + b;
    if (std::fabs(d) < 1e-300) d = 1e-300;
    c = b + an / c;
    if (std::fabs(c) < 1e-300) c = 1e-300;
    d = 1.0 / d;
    double del = d * c;
    h *= del;
    if (std::fabs(del - 1) < 1e-16) break;
  }
  return 1.0 - std::exp(-x + a * std::log(x) - LogGamma(a)) * h;
}

double BetaCf(double a, double b, double x) {
  double qab = a + b;
  double qap = a + 1;
  double qam = a - 1;
  double c = 1;
  double d = 1 - qab * x / qap;
  if (std::fabs(d) < 1e-300) d = 1e-300;
  d = 1 / d;
  double h = d;
  for (int m = 1; m < 10000; ++m) {
    int m2 = 2 * m;
    double aa = m * (b - m) * x / ((qam + m2) * (a + m2));
    d = 1 + aa * d;
    if (std::fabs(d) < 1e-300) d = 1e-300;
    c = 1 + aa / c;
    if (std::fabs(c) < 1e-300) c = 1e-300;
    d = 1 / d;
    h *= d * c;
    aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
    d = 1 + aa * d;
    if (std::fabs(d) < 1e-300) d = 1e-300;
    c = 1 + aa / c;
    if (std::fabs(c) < 1e-300) c = 1e-300;
    d = 1 / d;
    double del = d * c;
    h *= del;
    if (std::fabs(del - 1) < 1e-16) break;
  }
  return h;
}

double BetaI(double a, double b, double x) {
  if (x <= 0) return 0;
  if (x >= 1) return 1;
  double bt =
      std::exp(LogGamma(a + b) - LogGamma(a) - LogGamma(b) + a * std::log(x) + b * std::log(1 - x));
  if (x < (a + 1) / (a + b + 2)) return bt * BetaCf(a, b, x) / a;
  return 1 - bt * BetaCf(b, a, 1 - x) / b;
}

double NormCdf(double z) {
  return 0.5 * std::erfc(-z / std::sqrt(2.0));
}
double NormPdf(double z) {
  return std::exp(-0.5 * z * z) / std::sqrt(2 * kPi);
}

double NormInv(double p) {
  static const double a[] = {-3.969683028665376e+01, 2.209460984245205e+02,  -2.759285104469687e+02,
                             1.383577518672690e+02,  -3.066479806614716e+01, 2.506628277459239e+00};
  static const double b[] = {-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
                             6.680131188771972e+01, -1.328068155288572e+01};
  static const double c[] = {-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
                             -2.549732539343734e+00, 4.374664141464968e+00,  2.938163982698783e+00};
  static const double d[] = {7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
                             3.754408661907416e+00};
  double q;
  double x;
  if (p < 0.02425) {
    q = std::sqrt(-2 * std::log(p));
    x = (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
        ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1);
  } else if (p <= 1 - 0.02425) {
    q = p - 0.5;
    double r = q * q;
    x = (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
        (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1);
  } else {
    q = std::sqrt(-2 * std::log(1 - p));
    x = -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
        ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1);
  }
  for (int i = 0; i < 2; ++i) {
    double e = NormCdf(x) - p;
    double u = e * std::sqrt(2 * kPi) * std::exp(x * x / 2);
    x = x - u / (1 + x * u / 2);
  }
  return x;
}

template <typename F>
bool Invert(F cdf, double p, double lo, double hi, bool grow, double& out) {
  if (grow) {
    int guard = 0;
    while (cdf(hi) < p && guard++ < 200) hi *= 2;
  }
  for (int i = 0; i < 300; ++i) {
    double mid = 0.5 * (lo + hi);
    if (cdf(mid) < p) {
      lo = mid;
    } else {
      hi = mid;
    }
    if (hi - lo <= 1e-15 * std::max(1.0, std::fabs(mid))) break;
  }
  out = 0.5 * (lo + hi);
  return std::isfinite(out);
}

double TCdf(double t, double df) {
  double x = df / (df + t * t);
  double tail = 0.5 * BetaI(df / 2, 0.5, x);
  return t > 0 ? 1 - tail : tail;
}

double ChiCdf(double x, double df) {
  return GammaP(df / 2, x / 2);
}

double FCdf(double x, double d1, double d2) {
  if (x <= 0) return 0;
  return BetaI(d1 / 2, d2 / 2, d1 * x / (d1 * x + d2));
}

Operand FnCount(EvalContext& ctx, Operand* a, int n) {
  double count = 0;
  for (int i = 0; i < n; ++i) {
    const Operand& o = a[i];
    if (o.IsMissing()) {
      count += 1;
      continue;
    }
    if (o.IsRef()) {
      auto scan = [&](const Area& ar) {
        ctx.Scan(ar, [&](int, int, const Value& v) {
          if (v.IsNumber()) count += 1;
        });
      };
      if (o.tag == Operand::Tag::Ref) {
        scan(o.area);
      } else {
        for (const Area& ar : *o.list) scan(ar);
      }
      continue;
    }
    const Value& v = o.value;
    if (v.IsArray()) {
      for (uint32_t r = 0; r < v.rows(); ++r) {
        for (uint32_t c = 0; c < v.cols(); ++c) count += v.at(r, c).IsNumber() ? 1 : 0;
      }
      continue;
    }
    double d;
    if (v.IsNumber() || v.IsBool()) {
      count += 1;
    } else if (v.IsText() && ParseNumberText(v.text(), ctx.dates(), d)) {
      count += 1;
    }
  }
  return MakeNum(count);
}

Operand FnCountA(EvalContext& ctx, Operand* a, int n) {
  double count = 0;
  for (int i = 0; i < n; ++i) {
    const Operand& o = a[i];
    if (o.IsMissing()) {
      count += 1;
      continue;
    }
    if (o.IsRef()) {
      auto scan = [&](const Area& ar) {
        ctx.Scan(ar, [&](int, int, const Value&) { count += 1; });
      };
      if (o.tag == Operand::Tag::Ref) {
        scan(o.area);
      } else {
        for (const Area& ar : *o.list) scan(ar);
      }
      continue;
    }
    const Value& v = o.value;
    if (v.IsArray()) {
      for (uint32_t r = 0; r < v.rows(); ++r) {
        for (uint32_t c = 0; c < v.cols(); ++c) count += v.at(r, c).IsBlank() ? 0 : 1;
      }
      continue;
    }
    count += 1;
  }
  return MakeNum(count);
}

Operand FnCountBlank(EvalContext& ctx, Operand* a, int) {
  if (a[0].tag != Operand::Tag::Ref) {
    if (a[0].IsError()) return MakeErr(a[0].value.error());
    return MakeErr(Err::Value);
  }
  const Area& ar = a[0].area;
  double total = static_cast<double>(ar.rows()) * static_cast<double>(ar.cols());
  double filled = 0;
  ctx.Scan(ar, [&](int, int, const Value& v) {
    if (!(v.IsText() && v.text().empty())) filled += 1;
  });
  return MakeNum(total - filled);
}

std::vector<uint32_t> ParsePattern(std::string_view p, std::vector<uint32_t>& kinds) {
  std::vector<uint32_t> chars;
  kinds.clear();
  size_t i = 0;
  while (i < p.size()) {
    char c = p[i];
    if (c == '~' && i + 1 < p.size() && (p[i + 1] == '*' || p[i + 1] == '?' || p[i + 1] == '~')) {
      chars.push_back(static_cast<unsigned char>(p[i + 1]));
      kinds.push_back(0);
      i += 2;
      continue;
    }
    if (c == '*') {
      chars.push_back(0);
      kinds.push_back(1);
      ++i;
      continue;
    }
    if (c == '?') {
      chars.push_back(0);
      kinds.push_back(2);
      ++i;
      continue;
    }
    uint32_t cp = Utf8Decode(p, i);
    chars.push_back(cp);
    kinds.push_back(0);
  }
  return chars;
}

uint32_t Fold(uint32_t c) {
  if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
  std::string s = ToLower(Utf8Encode(c));
  size_t i = 0;
  return Utf8Decode(s, i);
}

}

bool HasWildcards(std::string_view s) {
  return s.find_first_of("*?~") != std::string_view::npos;
}

bool WildcardMatch(std::string_view pattern, std::string_view text) {
  std::vector<uint32_t> kinds;
  std::vector<uint32_t> pc = ParsePattern(pattern, kinds);
  std::vector<uint32_t> tc;
  for (size_t i = 0; i < text.size();) tc.push_back(Fold(Utf8Decode(text, i)));
  for (size_t k = 0; k < pc.size(); ++k) {
    if (kinds[k] == 0) pc[k] = Fold(pc[k]);
  }
  size_t p = 0;
  size_t t = 0;
  size_t star = std::string_view::npos;
  size_t mark = 0;
  while (t < tc.size()) {
    if (p < pc.size() && (kinds[p] == 2 || (kinds[p] == 0 && pc[p] == tc[t]))) {
      ++p;
      ++t;
    } else if (p < pc.size() && kinds[p] == 1) {
      star = p++;
      mark = t;
    } else if (star != std::string_view::npos) {
      p = star + 1;
      t = ++mark;
    } else {
      return false;
    }
  }
  while (p < pc.size() && kinds[p] == 1) ++p;
  return p == pc.size();
}

Criteria ParseCriteria(const EvalContext& ctx, const Value& crit) {
  Criteria c;
  switch (crit.kind()) {
    case Kind::Number:
      c.kind = Criteria::Kind::Number;
      c.num = crit.number();
      return c;
    case Kind::Bool:
      c.kind = Criteria::Kind::Bool;
      c.flag = crit.boolean();
      return c;
    case Kind::Error:
      c.kind = Criteria::Kind::Error;
      c.err = crit.error();
      return c;
    case Kind::Blank:
      c.kind = Criteria::Kind::Number;
      c.num = 0;
      return c;
    case Kind::Array:
      return ParseCriteria(ctx, crit.at(0, 0));
    case Kind::Text:
      break;
  }
  std::string_view s = crit.text();
  bool has_op = true;
  if (s.substr(0, 2) == "<=") {
    c.cmp = Criteria::Cmp::Le;
    s.remove_prefix(2);
  } else if (s.substr(0, 2) == ">=") {
    c.cmp = Criteria::Cmp::Ge;
    s.remove_prefix(2);
  } else if (s.substr(0, 2) == "<>") {
    c.cmp = Criteria::Cmp::Ne;
    s.remove_prefix(2);
  } else if (s.substr(0, 1) == "=") {
    c.cmp = Criteria::Cmp::Eq;
    s.remove_prefix(1);
  } else if (s.substr(0, 1) == "<") {
    c.cmp = Criteria::Cmp::Lt;
    s.remove_prefix(1);
  } else if (s.substr(0, 1) == ">") {
    c.cmp = Criteria::Cmp::Gt;
    s.remove_prefix(1);
  } else {
    has_op = false;
  }
  if (s.empty()) {
    if (!has_op) {
      c.kind = Criteria::Kind::Empty;
    } else if (c.cmp == Criteria::Cmp::Eq) {
      c.kind = Criteria::Kind::Blank;
    } else if (c.cmp == Criteria::Cmp::Ne) {
      c.kind = Criteria::Kind::NonEmpty;
    } else {
      c.kind = Criteria::Kind::Text;
    }
    return c;
  }
  double d;
  if (ParseNumberText(s, ctx.dates(), d)) {
    c.kind = Criteria::Kind::Number;
    c.num = d;
    return c;
  }
  if (EqualsText(s, "TRUE") || EqualsText(s, "FALSE")) {
    c.kind = Criteria::Kind::Bool;
    c.flag = EqualsText(s, "TRUE");
    return c;
  }
  Err e;
  if (ParseErrorText(s, e)) {
    c.kind = Criteria::Kind::Error;
    c.err = e;
    return c;
  }
  c.kind = Criteria::Kind::Text;
  c.text = std::string(s);
  c.wildcard = (c.cmp == Criteria::Cmp::Eq || c.cmp == Criteria::Cmp::Ne) && HasWildcards(s);
  return c;
}

bool MatchCriteria(const Criteria& c, const Value& v, const EvalContext& ctx) {
  using K = Criteria::Kind;
  using C = Criteria::Cmp;
  auto order = [&](int cmp) {
    switch (c.cmp) {
      case C::Eq:
        return cmp == 0;
      case C::Ne:
        return cmp != 0;
      case C::Lt:
        return cmp < 0;
      case C::Le:
        return cmp <= 0;
      case C::Gt:
        return cmp > 0;
      case C::Ge:
        return cmp >= 0;
    }
    return false;
  };
  switch (c.kind) {
    case K::Blank:
      return v.IsBlank();
    case K::Empty:
      return v.IsBlank() || (v.IsText() && v.text().empty());
    case K::NonEmpty:
      return !v.IsBlank();
    case K::Number: {
      double x = 0;
      bool numeric = false;
      if (v.IsNumber()) {
        x = v.number();
        numeric = true;
      } else if (v.IsText() && (c.cmp == C::Eq || c.cmp == C::Ne)) {
        numeric = ParseNumberText(v.text(), ctx.dates(), x);
      }
      if (!numeric) return c.cmp == C::Ne;
      int cmp = ApproxEqual(x, c.num) ? 0 : (x < c.num ? -1 : 1);
      return order(cmp);
    }
    case K::Bool:
      if (!v.IsBool()) return c.cmp == C::Ne;
      return order(v.boolean() == c.flag ? 0 : (v.boolean() ? 1 : -1));
    case K::Error:
      if (!v.IsError()) return c.cmp == C::Ne;
      return order(v.error() == c.err ? 0 : 1);
    case K::Text: {
      if (c.cmp == C::Eq || c.cmp == C::Ne) {
        bool eq = false;
        if (v.IsText()) {
          eq = c.wildcard ? WildcardMatch(c.text, v.text()) : EqualsText(c.text, v.text());
        } else if (v.IsBlank()) {
          eq = c.text.empty();
        }
        return c.cmp == C::Eq ? eq : !eq;
      }
      if (!v.IsText()) return false;
      return order(CompareText(v.text(), c.text));
    }
  }
  return false;
}

bool AreaOf(const Operand& op, Area& out) {
  if (op.tag != Operand::Tag::Ref) return false;
  out = op.area;
  return true;
}

namespace {

struct CondSet {
  std::vector<Area> ranges;
  std::vector<Criteria> crits;
  std::vector<Grid> arrays;
  std::vector<uint8_t> is_array;
};

Err BuildConditions(EvalContext& ctx, Operand* a, int start, int n, CondSet& cs, int& rows,
                    int& cols) {
  rows = -1;
  cols = -1;
  for (int i = start; i + 1 < n; i += 2) {
    Grid g;
    if (a[i].IsError()) return a[i].value.error();
    Area ar;
    bool is_ref = AreaOf(a[i], ar);
    if (!is_ref) {
      if (!MakeGrid(ctx, a[i], g, false)) return Err::Value;
      if (g.kind != 1) return Err::Value;
    }
    int r = is_ref ? ar.rows() : g.rows;
    int c = is_ref ? ar.cols() : g.cols;
    if (rows < 0) {
      rows = r;
      cols = c;
    } else if (r != rows || c != cols) {
      return Err::Value;
    }
    Value crit = ctx.Scalar(a[i + 1]);
    cs.ranges.push_back(ar);
    cs.arrays.push_back(g);
    cs.is_array.push_back(is_ref ? 0 : 1);
    cs.crits.push_back(ParseCriteria(ctx, crit));
  }
  return Err::None;
}

template <typename F>
void ForMatches(EvalContext& ctx, const CondSet& cs, int rows, int cols, F&& fn) {
  int scan_rows = rows;
  int scan_cols = cols;
  for (size_t k = 0; k < cs.ranges.size(); ++k) {
    if (cs.is_array[k]) continue;
    const Area& ar = cs.ranges[k];
    scan_rows = std::min(scan_rows, std::max(0, ctx.UsedRows(ar.sheet) - ar.r1));
    scan_cols = std::min(scan_cols, std::max(0, ctx.UsedCols(ar.sheet) - ar.c1));
  }
  bool all_refs = true;
  for (uint8_t b : cs.is_array) all_refs = all_refs && !b;
  if (!all_refs) {
    scan_rows = rows;
    scan_cols = cols;
  }
  for (int c = 0; c < scan_cols; ++c) {
    for (int r = 0; r < scan_rows; ++r) {
      bool ok = true;
      for (size_t k = 0; k < cs.crits.size() && ok; ++k) {
        const Value& v =
            cs.is_array[k] ? cs.arrays[k].At(r, c)
                           : ctx.Read(cs.ranges[k].sheet, cs.ranges[k].r1 + r, cs.ranges[k].c1 + c);
        ok = MatchCriteria(cs.crits[k], v, ctx);
      }
      if (ok) fn(r, c);
    }
  }
  double outside = static_cast<double>(rows) * cols - static_cast<double>(scan_rows) * scan_cols;
  if (outside > 0) {
    bool blank_ok = true;
    Value blank;
    for (const Criteria& cr : cs.crits) blank_ok = blank_ok && MatchCriteria(cr, blank, ctx);
    if (blank_ok) fn(-1, static_cast<int>(outside));
  }
}

Operand FnCountIfs(EvalContext& ctx, Operand* a, int n) {
  if (n % 2 != 0) return MakeErr(Err::Value);
  CondSet cs;
  int rows;
  int cols;
  if (Err e = BuildConditions(ctx, a, 0, n, cs, rows, cols); e != Err::None) return MakeErr(e);
  double count = 0;
  ForMatches(ctx, cs, rows, cols, [&](int r, int c) { count += r < 0 ? c : 1; });
  return MakeNum(count);
}

Operand FnCountIf(EvalContext& ctx, Operand* a, int n) {
  return FnCountIfs(ctx, a, n);
}

enum class Agg { Sum, Average, Max, Min };

Operand AggregateIfs(EvalContext& ctx, const Operand& target, Operand* a, int start, int n,
                     Agg kind, bool shape_from_criteria) {
  CondSet cs;
  int rows;
  int cols;
  if (Err e = BuildConditions(ctx, a, start, n, cs, rows, cols); e != Err::None) return MakeErr(e);
  Area tgt;
  Grid tg;
  bool tgt_ref = AreaOf(target, tgt);
  if (!tgt_ref) {
    if (target.IsError()) return MakeErr(target.value.error());
    if (!MakeGrid(ctx, target, tg, false) || tg.kind != 1) return MakeErr(Err::Value);
  }
  if (!shape_from_criteria) {
    int tr = tgt_ref ? tgt.rows() : tg.rows;
    int tc = tgt_ref ? tgt.cols() : tg.cols;
    if (tr != rows || tc != cols) return MakeErr(Err::Value);
  }
  std::vector<double> xs;
  Err bad = Err::None;
  ForMatches(ctx, cs, rows, cols, [&](int r, int c) {
    if (r < 0) return;
    const Value& v = tgt_ref ? ctx.Read(tgt.sheet, tgt.r1 + r, tgt.c1 + c) : tg.At(r, c);
    if (v.IsNumber()) {
      xs.push_back(v.number());
    } else if (v.IsError() && bad == Err::None) {
      bad = v.error();
    }
  });
  if (bad != Err::None) return MakeErr(bad);
  switch (kind) {
    case Agg::Sum:
      return MakeNum(Sum(xs));
    case Agg::Average:
      if (xs.empty()) return MakeErr(Err::Div0);
      return MakeNum(Sum(xs) / static_cast<double>(xs.size()));
    case Agg::Max:
      return MakeNum(xs.empty() ? 0 : *std::max_element(xs.begin(), xs.end()));
    case Agg::Min:
      return MakeNum(xs.empty() ? 0 : *std::min_element(xs.begin(), xs.end()));
  }
  return MakeNum(0);
}

Operand IfWithRange(EvalContext& ctx, Operand* a, int n, Agg kind) {
  Operand target = n >= 3 && !a[2].IsMissing() ? a[2] : a[0];
  if (target.tag == Operand::Tag::Ref && a[0].tag == Operand::Tag::Ref) {
    Area& t = target.area;
    t.r2 = t.r1 + a[0].area.rows() - 1;
    t.c2 = t.c1 + a[0].area.cols() - 1;
    if (t.r2 >= kMaxRows || t.c2 >= kMaxCols) return MakeErr(Err::Ref);
  }
  Operand cond[2] = {a[0], a[1]};
  return AggregateIfs(ctx, target, cond, 0, 2, kind, true);
}

Operand FnSumIf(EvalContext& ctx, Operand* a, int n) {
  return IfWithRange(ctx, a, n, Agg::Sum);
}
Operand FnAverageIf(EvalContext& ctx, Operand* a, int n) {
  return IfWithRange(ctx, a, n, Agg::Average);
}
Operand FnSumIfs(EvalContext& ctx, Operand* a, int n) {
  if (n % 2 == 0) return MakeErr(Err::Value);
  return AggregateIfs(ctx, a[0], a, 1, n, Agg::Sum, false);
}
Operand FnAverageIfs(EvalContext& ctx, Operand* a, int n) {
  if (n % 2 == 0) return MakeErr(Err::Value);
  return AggregateIfs(ctx, a[0], a, 1, n, Agg::Average, false);
}
Operand FnMaxIfs(EvalContext& ctx, Operand* a, int n) {
  if (n % 2 == 0) return MakeErr(Err::Value);
  return AggregateIfs(ctx, a[0], a, 1, n, Agg::Max, false);
}
Operand FnMinIfs(EvalContext& ctx, Operand* a, int n) {
  if (n % 2 == 0) return MakeErr(Err::Value);
  return AggregateIfs(ctx, a[0], a, 1, n, Agg::Min, false);
}

Operand FnAverage(EvalContext& ctx, Operand* a, int n) {
  std::vector<double> xs;
  if (Err e = CollectNumbers(ctx, a, n, xs); e != Err::None) return MakeErr(e);
  if (xs.empty()) return MakeErr(Err::Div0);
  return MakeNum(Mean(xs));
}
Operand FnAverageA(EvalContext& ctx, Operand* a, int n) {
  std::vector<double> xs;
  if (Err e = CollectNumbers(ctx, a, n, xs, CollectMode::NumbersA); e != Err::None)
    return MakeErr(e);
  if (xs.empty()) return MakeErr(Err::Div0);
  return MakeNum(Mean(xs));
}

template <bool Max, CollectMode Mode>
Operand FnExtreme(EvalContext& ctx, Operand* a, int n) {
  std::vector<double> xs;
  if (Err e = CollectNumbers(ctx, a, n, xs, Mode); e != Err::None) return MakeErr(e);
  if (xs.empty()) return MakeNum(0);
  return MakeNum(Max ? *std::max_element(xs.begin(), xs.end())
                     : *std::min_element(xs.begin(), xs.end()));
}

Operand FnMedian(EvalContext& ctx, Operand* a, int n) {
  std::vector<double> xs;
  if (Err e = CollectNumbers(ctx, a, n, xs); e != Err::None) return MakeErr(e);
  if (xs.empty()) return MakeErr(Err::Num);
  std::sort(xs.begin(), xs.end());
  size_t m = xs.size() / 2;
  if (xs.size() % 2) return MakeNum(xs[m]);
  return MakeNum((xs[m - 1] + xs[m]) / 2);
}

Err StrictNumbers(EvalContext& ctx, Operand* a, int n, std::vector<double>& xs) {
  for (int i = 0; i < n; ++i) {
    const Operand& o = a[i];
    if (o.tag == Operand::Tag::Ref && (o.area.rows() > 1 || o.area.cols() > 1)) {
      Err bad = Err::None;
      ctx.ScanOrdered(o.area, [&](int, int, const Value& v) {
        if (bad != Err::None) return;
        if (v.IsError()) bad = v.error();
        if (v.IsNumber()) xs.push_back(v.number());
      });
      if (bad != Err::None) return bad;
      continue;
    }
    if (o.IsArray()) {
      for (uint32_t r = 0; r < o.value.rows(); ++r) {
        for (uint32_t c = 0; c < o.value.cols(); ++c) {
          const Value& v = o.value.at(r, c);
          if (v.IsError()) return v.error();
          if (v.IsNumber()) xs.push_back(v.number());
        }
      }
      continue;
    }
    Value v =
        o.tag == Operand::Tag::Ref ? ctx.Read(o.area.sheet, o.area.r1, o.area.c1) : ctx.Scalar(o);
    if (v.IsError()) return v.error();
    if (!v.IsNumber()) return Err::Value;
    xs.push_back(v.number());
  }
  return Err::None;
}

Operand FnMode(EvalContext& ctx, Operand* a, int n) {
  std::vector<double> xs;
  if (Err e = StrictNumbers(ctx, a, n, xs); e != Err::None) return MakeErr(e);
  if (xs.empty()) return MakeErr(Err::NA);
  std::map<double, int> counts;
  for (double x : xs) ++counts[x];
  int best = 1;
  double value = 0;
  bool found = false;
  for (double x : xs) {
    int c = counts[x];
    if (c > best) {
      best = c;
      value = x;
      found = true;
    }
  }
  if (!found) return MakeErr(Err::NA);
  return MakeNum(value);
}

Operand FnModeMult(EvalContext& ctx, Operand* a, int n) {
  std::vector<double> xs;
  if (Err e = CollectNumbers(ctx, a, n, xs); e != Err::None) return MakeErr(e);
  std::map<double, int> counts;
  for (double x : xs) ++counts[x];
  int best = 1;
  for (const auto& kv : counts) best = std::max(best, kv.second);
  if (best < 2) return MakeErr(Err::NA);
  std::vector<double> modes;
  for (double x : xs) {
    if (counts[x] == best && std::find(modes.begin(), modes.end(), x) == modes.end())
      modes.push_back(x);
  }
  Value out = Value::Array(static_cast<uint32_t>(modes.size()), 1);
  for (size_t i = 0; i < modes.size(); ++i)
    out.mut(static_cast<uint32_t>(i), 0) = Value::Number(modes[i]);
  return Operand::Of(std::move(out));
}

template <bool Large>
Operand FnKth(EvalContext& ctx, Operand* a, int) {
  std::vector<double> xs;
  if (Err e = CollectNumbers(ctx, a, 1, xs); e != Err::None) return MakeErr(e);
  NUM_ARG(1, k0);
  double k = std::ceil(Round15(k0));
  if (k < 1 || k > static_cast<double>(xs.size())) return MakeErr(Err::Num);
  size_t idx = static_cast<size_t>(k) - 1;
  if (Large) {
    std::nth_element(xs.begin(), xs.begin() + static_cast<long>(idx), xs.end(),
                     std::greater<double>());
  } else {
    std::nth_element(xs.begin(), xs.begin() + static_cast<long>(idx), xs.end());
  }
  return MakeNum(xs[idx]);
}

template <bool Avg>
Operand FnRank(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, x);
  if (!a[1].IsRef()) return MakeErr(a[1].IsError() ? a[1].value.error() : Err::Value);
  std::vector<double> xs;
  if (Err e = CollectNumbers(ctx, a + 1, 1, xs); e != Err::None) return MakeErr(e);
  double order;
  if (Err e = ArgNumberOr(ctx, a, n, 2, 0, order); e != Err::None) return MakeErr(e);
  double before = 0;
  double same = 0;
  for (double v : xs) {
    if (ApproxEqual(v, x)) {
      same += 1;
    } else if (order == 0 ? v > x : v < x) {
      before += 1;
    }
  }
  if (same == 0) return MakeErr(Err::NA);
  if (Avg) return MakeNum(before + (same + 1) / 2);
  return MakeNum(before + 1);
}

Operand Percentile(std::vector<double>& xs, double k, bool exclusive) {
  if (xs.empty()) return MakeErr(Err::Num);
  std::sort(xs.begin(), xs.end());
  double n = static_cast<double>(xs.size());
  double pos;
  if (exclusive) {
    if (k <= 0 || k >= 1) return MakeErr(Err::Num);
    pos = k * (n + 1) - 1;
    if (pos < 0 || pos > n - 1) return MakeErr(Err::Num);
  } else {
    if (k < 0 || k > 1) return MakeErr(Err::Num);
    pos = k * (n - 1);
  }
  double lo = std::floor(pos);
  double frac = pos - lo;
  size_t i = static_cast<size_t>(lo);
  if (i + 1 >= xs.size()) return MakeNum(xs[i]);
  return MakeNum(xs[i] + frac * (xs[i + 1] - xs[i]));
}

template <bool Exclusive>
Operand FnPercentile(EvalContext& ctx, Operand* a, int) {
  std::vector<double> xs;
  if (Err e = CollectNumbers(ctx, a, 1, xs); e != Err::None) return MakeErr(e);
  NUM_ARG(1, k);
  return Percentile(xs, k, Exclusive);
}

template <bool Exclusive>
Operand FnQuartile(EvalContext& ctx, Operand* a, int) {
  std::vector<double> xs;
  if (Err e = CollectNumbers(ctx, a, 1, xs); e != Err::None) return MakeErr(e);
  NUM_ARG(1, q0);
  double q = std::trunc(q0);
  if (Exclusive ? (q < 1 || q > 3) : (q < 0 || q > 4)) return MakeErr(Err::Num);
  return Percentile(xs, q / 4, Exclusive);
}

template <bool Exclusive>
Operand FnPercentRank(EvalContext& ctx, Operand* a, int n) {
  std::vector<double> xs;
  if (Err e = CollectNumbers(ctx, a, 1, xs); e != Err::None) return MakeErr(e);
  NUM_ARG(1, x);
  double sig0;
  if (Err e = ArgNumberOr(ctx, a, n, 2, 3, sig0); e != Err::None) return MakeErr(e);
  int sig = ToInt(sig0);
  if (sig < 1) return MakeErr(Err::Num);
  if (xs.empty()) return MakeErr(Err::Num);
  std::sort(xs.begin(), xs.end());
  double cnt = static_cast<double>(xs.size());
  if (x < xs.front() || x > xs.back()) return MakeErr(Err::NA);
  double result;
  if (cnt == 1) {
    result = 1;
  } else {
    size_t i = 0;
    while (i < xs.size() && xs[i] < x) ++i;
    double rank;
    if (i < xs.size() && xs[i] == x) {
      rank = static_cast<double>(i);
    } else {
      double lo = xs[i - 1];
      double hi = xs[i];
      rank = static_cast<double>(i - 1) + (x - lo) / (hi - lo);
    }
    result = Exclusive ? (rank + 1) / (cnt + 1) : rank / (cnt - 1);
  }
  double scale = std::pow(10.0, sig);
  return MakeNum(std::floor(Round15(result * scale)) / scale);
}

template <bool Sample, CollectMode Mode, bool Root>
Operand FnVariance(EvalContext& ctx, Operand* a, int n) {
  std::vector<double> xs;
  if (Err e = CollectNumbers(ctx, a, n, xs, Mode); e != Err::None) return MakeErr(e);
  double count = static_cast<double>(xs.size());
  double denom = Sample ? count - 1 : count;
  if (denom <= 0) return MakeErr(Err::Div0);
  double v = SumSqDev(xs) / denom;
  return MakeNum(Root ? std::sqrt(v) : v);
}

Operand FnDevSq(EvalContext& ctx, Operand* a, int n) {
  std::vector<double> xs;
  if (Err e = CollectNumbers(ctx, a, n, xs); e != Err::None) return MakeErr(e);
  if (xs.empty()) return MakeErr(Err::Num);
  return MakeNum(SumSqDev(xs));
}
Operand FnAveDev(EvalContext& ctx, Operand* a, int n) {
  std::vector<double> xs;
  if (Err e = CollectNumbers(ctx, a, n, xs); e != Err::None) return MakeErr(e);
  if (xs.empty()) return MakeErr(Err::Num);
  double m = Mean(xs);
  std::vector<double> d;
  for (double x : xs) d.push_back(std::fabs(x - m));
  return MakeNum(Sum(d) / static_cast<double>(xs.size()));
}
Operand FnGeoMean(EvalContext& ctx, Operand* a, int n) {
  std::vector<double> xs;
  if (Err e = CollectNumbers(ctx, a, n, xs); e != Err::None) return MakeErr(e);
  if (xs.empty()) return MakeErr(Err::Num);
  double s = 0;
  for (double x : xs) {
    if (x <= 0) return MakeErr(Err::Num);
    s += std::log(x);
  }
  return MakeNum(std::exp(s / static_cast<double>(xs.size())));
}
Operand FnHarMean(EvalContext& ctx, Operand* a, int n) {
  std::vector<double> xs;
  if (Err e = CollectNumbers(ctx, a, n, xs); e != Err::None) return MakeErr(e);
  if (xs.empty()) return MakeErr(Err::Num);
  double s = 0;
  for (double x : xs) {
    if (x <= 0) return MakeErr(Err::Num);
    s += 1 / x;
  }
  return MakeNum(static_cast<double>(xs.size()) / s);
}
Operand FnKurt(EvalContext& ctx, Operand* a, int n) {
  std::vector<double> xs;
  if (Err e = CollectNumbers(ctx, a, n, xs); e != Err::None) return MakeErr(e);
  double c = static_cast<double>(xs.size());
  if (c < 4) return MakeErr(Err::Div0);
  double m = Mean(xs);
  double sd = std::sqrt(SumSqDev(xs) / (c - 1));
  if (sd == 0) return MakeErr(Err::Div0);
  double s4 = 0;
  for (double x : xs) s4 += std::pow((x - m) / sd, 4);
  return MakeNum(c * (c + 1) / ((c - 1) * (c - 2) * (c - 3)) * s4 -
                 3 * (c - 1) * (c - 1) / ((c - 2) * (c - 3)));
}
template <bool Population>
Operand FnSkew(EvalContext& ctx, Operand* a, int n) {
  std::vector<double> xs;
  if (Err e = CollectNumbers(ctx, a, n, xs); e != Err::None) return MakeErr(e);
  double c = static_cast<double>(xs.size());
  if (c < (Population ? 1 : 3)) return MakeErr(Err::Div0);
  double m = Mean(xs);
  double sd = std::sqrt(SumSqDev(xs) / (Population ? c : c - 1));
  if (sd == 0) return MakeErr(Err::Div0);
  double s3 = 0;
  for (double x : xs) s3 += std::pow((x - m) / sd, 3);
  if (Population) return MakeNum(s3 / c);
  return MakeNum(c / ((c - 1) * (c - 2)) * s3);
}
Operand FnTrimMean(EvalContext& ctx, Operand* a, int) {
  std::vector<double> xs;
  if (Err e = CollectNumbers(ctx, a, 1, xs); e != Err::None) return MakeErr(e);
  NUM_ARG(1, pct);
  if (pct < 0 || pct >= 1 || xs.empty()) return MakeErr(Err::Num);
  std::sort(xs.begin(), xs.end());
  size_t drop = static_cast<size_t>(std::floor(static_cast<double>(xs.size()) * pct / 2));
  std::vector<double> mid(xs.begin() + static_cast<long>(drop), xs.end() - static_cast<long>(drop));
  return MakeNum(Mean(mid));
}

struct Regression {
  double n = 0;
  double mx = 0;
  double my = 0;
  double sxx = 0;
  double syy = 0;
  double sxy = 0;
};

Err Regress(EvalContext& ctx, const Operand& ys, const Operand& xs, Regression& r) {
  Pairs p = CollectPairs(ctx, xs, ys);
  if (p.err != Err::None) return p.err;
  r.n = static_cast<double>(p.x.size());
  if (r.n == 0) return Err::Div0;
  r.mx = Mean(p.x);
  r.my = Mean(p.y);
  for (size_t i = 0; i < p.x.size(); ++i) {
    double dx = p.x[i] - r.mx;
    double dy = p.y[i] - r.my;
    r.sxx += dx * dx;
    r.syy += dy * dy;
    r.sxy += dx * dy;
  }
  return Err::None;
}

Operand FnCorrel(EvalContext& ctx, Operand* a, int) {
  Regression r;
  if (Err e = Regress(ctx, a[1], a[0], r); e != Err::None) return MakeErr(e);
  if (r.sxx == 0 || r.syy == 0) return MakeErr(Err::Div0);
  return MakeNum(r.sxy / std::sqrt(r.sxx * r.syy));
}
Operand FnRsq(EvalContext& ctx, Operand* a, int) {
  Regression r;
  if (Err e = Regress(ctx, a[0], a[1], r); e != Err::None) return MakeErr(e);
  if (r.sxx == 0 || r.syy == 0) return MakeErr(Err::Div0);
  double c = r.sxy / std::sqrt(r.sxx * r.syy);
  return MakeNum(c * c);
}
Operand FnSlope(EvalContext& ctx, Operand* a, int) {
  Regression r;
  if (Err e = Regress(ctx, a[0], a[1], r); e != Err::None) return MakeErr(e);
  if (r.sxx == 0) return MakeErr(Err::Div0);
  return MakeNum(r.sxy / r.sxx);
}
Operand FnIntercept(EvalContext& ctx, Operand* a, int) {
  Regression r;
  if (Err e = Regress(ctx, a[0], a[1], r); e != Err::None) return MakeErr(e);
  if (r.sxx == 0) return MakeErr(Err::Div0);
  return MakeNum(r.my - r.sxy / r.sxx * r.mx);
}
Operand FnSteyx(EvalContext& ctx, Operand* a, int) {
  Regression r;
  if (Err e = Regress(ctx, a[0], a[1], r); e != Err::None) return MakeErr(e);
  if (r.n < 3 || r.sxx == 0) return MakeErr(Err::Div0);
  return MakeNum(std::sqrt((r.syy - r.sxy * r.sxy / r.sxx) / (r.n - 2)));
}
Operand FnForecast(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  Regression r;
  if (Err e = Regress(ctx, a[1], a[2], r); e != Err::None) return MakeErr(e);
  if (r.sxx == 0) return MakeErr(Err::Div0);
  double b = r.sxy / r.sxx;
  return MakeNum(r.my - b * r.mx + b * x);
}
template <bool Sample>
Operand FnCovar(EvalContext& ctx, Operand* a, int) {
  Regression r;
  if (Err e = Regress(ctx, a[1], a[0], r); e != Err::None) return MakeErr(e);
  double d = Sample ? r.n - 1 : r.n;
  if (d <= 0) return MakeErr(Err::Div0);
  return MakeNum(r.sxy / d);
}

Operand NormDist(double x, double mean, double sd, bool cum) {
  if (sd <= 0) return MakeErr(Err::Num);
  double z = (x - mean) / sd;
  return MakeNum(cum ? NormCdf(z) : NormPdf(z) / sd);
}

Operand FnNormDist(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  NUM_ARG(1, m);
  NUM_ARG(2, sd);
  bool cum;
  if (Err e = ArgBool(ctx, a[3], cum); e != Err::None) return MakeErr(e);
  return NormDist(x, m, sd, cum);
}
Operand FnNormSDist(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, z);
  bool cum;
  if (Err e = ArgBool(ctx, a[1], cum); e != Err::None) return MakeErr(e);
  return NormDist(z, 0, 1, cum);
}
Operand FnNormSDistLegacy(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, z);
  return MakeNum(NormCdf(z));
}
Operand FnNormInv(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, p);
  NUM_ARG(1, m);
  NUM_ARG(2, sd);
  if (p <= 0 || p >= 1 || sd <= 0) return MakeErr(Err::Num);
  return MakeNum(m + sd * NormInv(p));
}
Operand FnNormSInv(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, p);
  if (p <= 0 || p >= 1) return MakeErr(Err::Num);
  return MakeNum(NormInv(p));
}
Operand FnStandardize(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  NUM_ARG(1, m);
  NUM_ARG(2, sd);
  if (sd <= 0) return MakeErr(Err::Num);
  return MakeNum((x - m) / sd);
}
Operand FnPhi(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  return MakeNum(NormPdf(x));
}
Operand FnGauss(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  return MakeNum(NormCdf(x) - 0.5);
}
Operand FnConfidenceNorm(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, alpha);
  NUM_ARG(1, sd);
  NUM_ARG(2, n0);
  double n = std::trunc(n0);
  if (alpha <= 0 || alpha >= 1 || sd <= 0 || n < 1) return MakeErr(Err::Num);
  return MakeNum(NormInv(1 - alpha / 2) * sd / std::sqrt(n));
}
Operand FnConfidenceT(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, alpha);
  NUM_ARG(1, sd);
  NUM_ARG(2, n0);
  double n = std::trunc(n0);
  if (alpha <= 0 || alpha >= 1 || sd <= 0 || n < 1) return MakeErr(Err::Num);
  if (n == 1) return MakeErr(Err::Div0);
  double t;
  double df = n - 1;
  Invert([&](double v) { return TCdf(v, df); }, 1 - alpha / 2, 0, 10, true, t);
  return MakeNum(t * sd / std::sqrt(n));
}

Operand FnExponDist(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  NUM_ARG(1, lambda);
  bool cum;
  if (Err e = ArgBool(ctx, a[2], cum); e != Err::None) return MakeErr(e);
  if (x < 0 || lambda <= 0) return MakeErr(Err::Num);
  return MakeNum(cum ? 1 - std::exp(-lambda * x) : lambda * std::exp(-lambda * x));
}

Operand FnPoisson(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x0);
  NUM_ARG(1, mean);
  bool cum;
  if (Err e = ArgBool(ctx, a[2], cum); e != Err::None) return MakeErr(e);
  double x = std::trunc(x0);
  if (x < 0 || mean < 0) return MakeErr(Err::Num);
  if (mean == 0) return MakeNum(cum || x == 0 ? 1 : 0);
  if (!cum) return MakeNum(std::exp(x * std::log(mean) - mean - LogGamma(x + 1)));
  return MakeNum(1 - GammaP(x + 1, mean));
}

Operand FnBinomDist(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, s0);
  NUM_ARG(1, n0);
  NUM_ARG(2, p);
  bool cum;
  if (Err e = ArgBool(ctx, a[3], cum); e != Err::None) return MakeErr(e);
  double s = std::trunc(s0);
  double n = std::trunc(n0);
  if (s < 0 || s > n || p < 0 || p > 1) return MakeErr(Err::Num);
  auto pmf = [&](double k) {
    if (p == 0) return k == 0 ? 1.0 : 0.0;
    if (p == 1) return k == n ? 1.0 : 0.0;
    return std::exp(LogGamma(n + 1) - LogGamma(k + 1) - LogGamma(n - k + 1) + k * std::log(p) +
                    (n - k) * std::log(1 - p));
  };
  if (!cum) return MakeNum(pmf(s));
  if (s == n) return MakeNum(1);
  if (p == 0) return MakeNum(1);
  if (p == 1) return MakeNum(0);
  return MakeNum(1 - BetaI(s + 1, n - s, p));
}

Operand FnHypGeomDist(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, s0);
  NUM_ARG(1, n0);
  NUM_ARG(2, m0);
  NUM_ARG(3, N0);
  bool cum = false;
  if (n > 4) {
    if (Err e = ArgBool(ctx, a[4], cum); e != Err::None) return MakeErr(e);
  }
  double s = std::trunc(s0);
  double ns = std::trunc(n0);
  double m = std::trunc(m0);
  double pop = std::trunc(N0);
  if (s < 0 || ns <= 0 || m <= 0 || pop <= 0 || ns > pop || m > pop || s > ns || s > m ||
      s < ns - pop + m) {
    return MakeErr(Err::Num);
  }
  auto lc = [](double x, double k) {
    return LogGamma(x + 1) - LogGamma(k + 1) - LogGamma(x - k + 1);
  };
  auto pmf = [&](double k) { return std::exp(lc(m, k) + lc(pop - m, ns - k) - lc(pop, ns)); };
  if (!cum) return MakeNum(pmf(s));
  double from = std::max(0.0, ns - pop + m);
  if (s - from > 1e7) return MakeErr(Err::Num);
  double total = 0;
  for (double k = from; k <= s; k += 1) total += pmf(k);
  return MakeNum(total);
}

Operand FnNegBinomDist(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, f0);
  NUM_ARG(1, s0);
  NUM_ARG(2, p);
  bool cum = false;
  if (n > 3) {
    if (Err e = ArgBool(ctx, a[3], cum); e != Err::None) return MakeErr(e);
  }
  double f = std::trunc(f0);
  double s = std::trunc(s0);
  if (f < 0 || s < 1 || p < 0 || p > 1) return MakeErr(Err::Num);
  if (cum) return MakeNum(BetaI(s, f + 1, p));
  return MakeNum(std::exp(LogGamma(f + s) - LogGamma(f + 1) - LogGamma(s) + s * std::log(p) +
                          f * std::log(1 - p)));
}

Operand FnWeibull(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  NUM_ARG(1, alpha);
  NUM_ARG(2, beta);
  bool cum;
  if (Err e = ArgBool(ctx, a[3], cum); e != Err::None) return MakeErr(e);
  if (x < 0 || alpha <= 0 || beta <= 0) return MakeErr(Err::Num);
  double t = std::pow(x / beta, alpha);
  if (cum) return MakeNum(1 - std::exp(-t));
  return MakeNum(alpha / std::pow(beta, alpha) * std::pow(x, alpha - 1) * std::exp(-t));
}

Operand FnLogNormDist(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, x);
  NUM_ARG(1, m);
  NUM_ARG(2, sd);
  bool cum = true;
  if (n > 3) {
    if (Err e = ArgBool(ctx, a[3], cum); e != Err::None) return MakeErr(e);
  }
  if (x <= 0 || sd <= 0) return MakeErr(Err::Num);
  double z = (std::log(x) - m) / sd;
  if (cum) return MakeNum(NormCdf(z));
  return MakeNum(NormPdf(z) / (x * sd));
}
Operand FnLogNormInv(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, p);
  NUM_ARG(1, m);
  NUM_ARG(2, sd);
  if (p <= 0 || p >= 1 || sd <= 0) return MakeErr(Err::Num);
  return MakeNum(std::exp(m + sd * NormInv(p)));
}

Operand FnGammaLn(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  if (x <= 0) return MakeErr(Err::Num);
  return MakeNum(LogGamma(x));
}
Operand FnGamma(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  if (x <= 0 && x == std::floor(x)) return MakeErr(Err::Num);
  if (x > 171.6) return MakeErr(Err::Num);
  return MakeNum(std::tgamma(x));
}
Operand FnFisher(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  if (x <= -1 || x >= 1) return MakeErr(Err::Num);
  return MakeNum(0.5 * std::log((1 + x) / (1 - x)));
}
Operand FnFisherInv(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, y);
  double e2 = std::exp(2 * y);
  return MakeNum((e2 - 1) / (e2 + 1));
}
Operand FnErf(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, lo);
  if (n > 1 && !a[1].IsMissing()) {
    NUM_ARG(1, hi);
    return MakeNum(std::erf(hi) - std::erf(lo));
  }
  return MakeNum(std::erf(lo));
}
Operand FnErfc(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  return MakeNum(std::erfc(x));
}

Operand FnPermut(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, n0);
  NUM_ARG(1, k0);
  double n = std::trunc(n0);
  double k = std::trunc(k0);
  if (n < 0 || k < 0 || k > n) return MakeErr(Err::Num);
  double r = 1;
  for (double j = 0; j < k && std::isfinite(r); j += 1) r *= n - j;
  return MakeNum(r);
}
Operand FnPermutationA(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, n0);
  NUM_ARG(1, k0);
  double n = std::trunc(n0);
  double k = std::trunc(k0);
  if (n < 0 || k < 0) return MakeErr(Err::Num);
  return MakeNum(std::pow(n, k));
}

Operand FnChiSqDist(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  NUM_ARG(1, df0);
  bool cum;
  if (Err e = ArgBool(ctx, a[2], cum); e != Err::None) return MakeErr(e);
  double df = std::trunc(df0);
  if (x < 0 || df < 1 || df > 1e10) return MakeErr(Err::Num);
  if (cum) return MakeNum(ChiCdf(x, df));
  if (x == 0) return MakeNum(df == 2 ? 0.5 : (df < 2 ? 0 : 0));
  return MakeNum(
      std::exp((df / 2 - 1) * std::log(x) - x / 2 - (df / 2) * std::log(2.0) - LogGamma(df / 2)));
}
Operand FnChiSqDistRt(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  NUM_ARG(1, df0);
  double df = std::trunc(df0);
  if (x < 0 || df < 1 || df > 1e10) return MakeErr(Err::Num);
  return MakeNum(1 - ChiCdf(x, df));
}
template <bool Right>
Operand FnChiSqInv(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, p);
  NUM_ARG(1, df0);
  double df = std::trunc(df0);
  if (p < 0 || p > 1 || df < 1 || df > 1e10) return MakeErr(Err::Num);
  double target = Right ? 1 - p : p;
  if (target <= 0) return MakeNum(0);
  if (target >= 1) return MakeErr(Err::Num);
  double x;
  Invert([&](double v) { return ChiCdf(v, df); }, target, 0, std::max(1.0, df), true, x);
  return MakeNum(x);
}

Operand FnGammaDist(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  NUM_ARG(1, alpha);
  NUM_ARG(2, beta);
  bool cum;
  if (Err e = ArgBool(ctx, a[3], cum); e != Err::None) return MakeErr(e);
  if (x < 0 || alpha <= 0 || beta <= 0) return MakeErr(Err::Num);
  if (cum) return MakeNum(GammaP(alpha, x / beta));
  if (x == 0) return MakeNum(alpha == 1 ? 1 / beta : 0);
  return MakeNum(
      std::exp((alpha - 1) * std::log(x) - x / beta - alpha * std::log(beta) - LogGamma(alpha)));
}
Operand FnGammaInv(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, p);
  NUM_ARG(1, alpha);
  NUM_ARG(2, beta);
  if (p < 0 || p >= 1 || alpha <= 0 || beta <= 0) return MakeErr(Err::Num);
  if (p == 0) return MakeNum(0);
  double x;
  Invert([&](double v) { return GammaP(alpha, v / beta); }, p, 0, std::max(1.0, alpha * beta), true,
         x);
  return MakeNum(x);
}

Operand FnBetaDist(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, x);
  NUM_ARG(1, alpha);
  NUM_ARG(2, beta);
  double lo;
  double hi;
  if (Err e = ArgNumberOr(ctx, a, n, 3, 0, lo); e != Err::None) return MakeErr(e);
  if (Err e = ArgNumberOr(ctx, a, n, 4, 1, hi); e != Err::None) return MakeErr(e);
  if (alpha <= 0 || beta <= 0 || lo >= hi || x < lo || x > hi) return MakeErr(Err::Num);
  return MakeNum(BetaI(alpha, beta, (x - lo) / (hi - lo)));
}
Operand FnBetaDistNew(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, x);
  NUM_ARG(1, alpha);
  NUM_ARG(2, beta);
  bool cum;
  if (Err e = ArgBool(ctx, a[3], cum); e != Err::None) return MakeErr(e);
  double lo;
  double hi;
  if (Err e = ArgNumberOr(ctx, a, n, 4, 0, lo); e != Err::None) return MakeErr(e);
  if (Err e = ArgNumberOr(ctx, a, n, 5, 1, hi); e != Err::None) return MakeErr(e);
  if (alpha <= 0 || beta <= 0 || lo >= hi || x < lo || x > hi) return MakeErr(Err::Num);
  double t = (x - lo) / (hi - lo);
  if (cum) return MakeNum(BetaI(alpha, beta, t));
  if (t == 0 || t == 1) {
    if ((t == 0 && alpha < 1) || (t == 1 && beta < 1)) return MakeErr(Err::Num);
  }
  double logb = LogGamma(alpha) + LogGamma(beta) - LogGamma(alpha + beta);
  return MakeNum(std::exp((alpha - 1) * std::log(t) + (beta - 1) * std::log(1 - t) - logb) /
                 (hi - lo));
}
Operand FnBetaInv(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, p);
  NUM_ARG(1, alpha);
  NUM_ARG(2, beta);
  double lo;
  double hi;
  if (Err e = ArgNumberOr(ctx, a, n, 3, 0, lo); e != Err::None) return MakeErr(e);
  if (Err e = ArgNumberOr(ctx, a, n, 4, 1, hi); e != Err::None) return MakeErr(e);
  if (p <= 0 || p > 1 || alpha <= 0 || beta <= 0 || lo >= hi) return MakeErr(Err::Num);
  double t;
  Invert([&](double v) { return BetaI(alpha, beta, v); }, p, 0, 1, false, t);
  return MakeNum(lo + t * (hi - lo));
}

Operand FnTDist(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  NUM_ARG(1, df0);
  bool cum;
  if (Err e = ArgBool(ctx, a[2], cum); e != Err::None) return MakeErr(e);
  double df = std::trunc(df0);
  if (df < 1) return MakeErr(Err::Div0);
  if (cum) return MakeNum(TCdf(x, df));
  return MakeNum(std::exp(LogGamma((df + 1) / 2) - LogGamma(df / 2)) / std::sqrt(df * kPi) *
                 std::pow(1 + x * x / df, -(df + 1) / 2));
}
Operand FnTDistRt(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  NUM_ARG(1, df0);
  double df = std::trunc(df0);
  if (df < 1) return MakeErr(Err::Div0);
  return MakeNum(1 - TCdf(x, df));
}
Operand FnTDist2T(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  NUM_ARG(1, df0);
  double df = std::trunc(df0);
  if (x < 0 || df < 1) return MakeErr(Err::Num);
  return MakeNum(2 * (1 - TCdf(x, df)));
}
Operand FnTDistLegacy(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  NUM_ARG(1, df0);
  NUM_ARG(2, tails0);
  double df = std::trunc(df0);
  double tails = std::trunc(tails0);
  if (x < 0 || df < 1 || (tails != 1 && tails != 2)) return MakeErr(Err::Num);
  return MakeNum(tails * (1 - TCdf(x, df)));
}
template <bool TwoTailed>
Operand FnTInv(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, p);
  NUM_ARG(1, df0);
  double df = std::trunc(df0);
  if (p <= 0 || p > 1 || df < 1) return MakeErr(Err::Num);
  if (TwoTailed) {
    if (p == 1) return MakeNum(0);
    double t;
    Invert([&](double v) { return TCdf(v, df); }, 1 - p / 2, 0, 10, true, t);
    return MakeNum(t);
  }
  if (p == 1) return MakeErr(Err::Num);
  if (p == 0.5) return MakeNum(0);
  double target = p < 0.5 ? 1 - p : p;
  double t;
  Invert([&](double v) { return TCdf(v, df); }, target, 0, 10, true, t);
  return MakeNum(p < 0.5 ? -t : t);
}

Operand FnFDist(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  NUM_ARG(1, d10);
  NUM_ARG(2, d20);
  bool cum;
  if (Err e = ArgBool(ctx, a[3], cum); e != Err::None) return MakeErr(e);
  double d1 = std::trunc(d10);
  double d2 = std::trunc(d20);
  if (x < 0 || d1 < 1 || d2 < 1) return MakeErr(Err::Num);
  if (cum) return MakeNum(FCdf(x, d1, d2));
  if (x == 0) return MakeNum(0);
  double logb = LogGamma(d1 / 2) + LogGamma(d2 / 2) - LogGamma((d1 + d2) / 2);
  return MakeNum(std::exp(0.5 * (d1 * std::log(d1 * x) + d2 * std::log(d2) -
                                 (d1 + d2) * std::log(d1 * x + d2)) -
                          logb) /
                 x);
}
Operand FnFDistRt(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  NUM_ARG(1, d10);
  NUM_ARG(2, d20);
  double d1 = std::trunc(d10);
  double d2 = std::trunc(d20);
  if (x < 0 || d1 < 1 || d2 < 1) return MakeErr(Err::Num);
  return MakeNum(1 - FCdf(x, d1, d2));
}
template <bool Right>
Operand FnFInv(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, p);
  NUM_ARG(1, d10);
  NUM_ARG(2, d20);
  double d1 = std::trunc(d10);
  double d2 = std::trunc(d20);
  if (p < 0 || p > 1 || d1 < 1 || d2 < 1) return MakeErr(Err::Num);
  double target = Right ? 1 - p : p;
  if (target <= 0) return MakeNum(0);
  if (target >= 1) return MakeErr(Err::Num);
  double x;
  Invert([&](double v) { return FCdf(v, d1, d2); }, target, 0, 10, true, x);
  return MakeNum(x);
}

Operand FnZTest(EvalContext& ctx, Operand* a, int n) {
  std::vector<double> xs;
  if (Err e = CollectNumbers(ctx, a, 1, xs); e != Err::None) return MakeErr(e);
  NUM_ARG(1, x);
  double cnt = static_cast<double>(xs.size());
  if (cnt < 1) return MakeErr(Err::NA);
  double sigma;
  if (n > 2 && !a[2].IsMissing()) {
    if (Err e = ArgNumber(ctx, a[2], sigma); e != Err::None) return MakeErr(e);
  } else {
    if (cnt < 2) return MakeErr(Err::Div0);
    sigma = std::sqrt(SumSqDev(xs) / (cnt - 1));
  }
  if (sigma == 0) return MakeErr(Err::Div0);
  return MakeNum(1 - NormCdf((Mean(xs) - x) / (sigma / std::sqrt(cnt))));
}

Operand FnChiSqTest(EvalContext& ctx, Operand* a, int) {
  Grid ga;
  Grid gb;
  if (!MakeGrid(ctx, a[0], ga, false) || !MakeGrid(ctx, a[1], gb, false))
    return MakeErr(Err::Value);
  if (ga.rows != gb.rows || ga.cols != gb.cols) return MakeErr(Err::NA);
  double chi = 0;
  for (int r = 0; r < ga.rows; ++r) {
    for (int c = 0; c < ga.cols; ++c) {
      const Value& o = ga.At(r, c);
      const Value& e = gb.At(r, c);
      if (o.IsError()) return MakeErr(o.error());
      if (e.IsError()) return MakeErr(e.error());
      if (!o.IsNumber() || !e.IsNumber()) continue;
      if (e.number() == 0) return MakeErr(Err::Div0);
      double d = o.number() - e.number();
      chi += d * d / e.number();
    }
  }
  double df =
      (ga.rows == 1 || ga.cols == 1) ? ga.rows * ga.cols - 1.0 : (ga.rows - 1.0) * (ga.cols - 1.0);
  if (df < 1) return MakeErr(Err::NA);
  return MakeNum(1 - ChiCdf(chi, df));
}

Operand FnTTest(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(2, tails0);
  NUM_ARG(3, type0);
  double tails = std::trunc(tails0);
  double type = std::trunc(type0);
  if ((tails != 1 && tails != 2) || type < 1 || type > 3) return MakeErr(Err::Num);
  double t;
  double df;
  if (type == 1) {
    Pairs p = CollectPairs(ctx, a[0], a[1]);
    if (p.err != Err::None) return MakeErr(p.err);
    std::vector<double> d;
    for (size_t i = 0; i < p.x.size(); ++i) d.push_back(p.x[i] - p.y[i]);
    double n = static_cast<double>(d.size());
    if (n < 2) return MakeErr(Err::Div0);
    double sd = std::sqrt(SumSqDev(d) / (n - 1));
    if (sd == 0) return MakeErr(Err::Div0);
    t = std::fabs(Mean(d)) / (sd / std::sqrt(n));
    df = n - 1;
  } else {
    std::vector<double> x;
    std::vector<double> y;
    if (Err e = CollectNumbers(ctx, a, 1, x); e != Err::None) return MakeErr(e);
    if (Err e = CollectNumbers(ctx, a + 1, 1, y); e != Err::None) return MakeErr(e);
    double n1 = static_cast<double>(x.size());
    double n2 = static_cast<double>(y.size());
    if (n1 < 2 || n2 < 2) return MakeErr(Err::Div0);
    double v1 = SumSqDev(x) / (n1 - 1);
    double v2 = SumSqDev(y) / (n2 - 1);
    if (type == 2) {
      double sp = ((n1 - 1) * v1 + (n2 - 1) * v2) / (n1 + n2 - 2);
      if (sp == 0) return MakeErr(Err::Div0);
      t = std::fabs(Mean(x) - Mean(y)) / std::sqrt(sp * (1 / n1 + 1 / n2));
      df = n1 + n2 - 2;
    } else {
      double s = v1 / n1 + v2 / n2;
      if (s == 0) return MakeErr(Err::Div0);
      t = std::fabs(Mean(x) - Mean(y)) / std::sqrt(s);
      df = s * s / ((v1 / n1) * (v1 / n1) / (n1 - 1) + (v2 / n2) * (v2 / n2) / (n2 - 1));
    }
  }
  return MakeNum(tails * (1 - TCdf(t, df)));
}

Operand FnFTest(EvalContext& ctx, Operand* a, int) {
  std::vector<double> x;
  std::vector<double> y;
  if (Err e = CollectNumbers(ctx, a, 1, x); e != Err::None) return MakeErr(e);
  if (Err e = CollectNumbers(ctx, a + 1, 1, y); e != Err::None) return MakeErr(e);
  double n1 = static_cast<double>(x.size());
  double n2 = static_cast<double>(y.size());
  if (n1 < 2 || n2 < 2) return MakeErr(Err::Div0);
  double v1 = SumSqDev(x) / (n1 - 1);
  double v2 = SumSqDev(y) / (n2 - 1);
  if (v1 == 0 || v2 == 0) return MakeErr(Err::Div0);
  double f = v1 / v2;
  double p = 1 - FCdf(f, n1 - 1, n2 - 1);
  return MakeNum(2 * std::min(p, 1 - p));
}

int FieldIndex(EvalContext& ctx, const Area& db, const Operand& field) {
  Value f = ctx.Scalar(field);
  if (f.IsNumber()) {
    int idx = ToInt(f.number());
    if (idx < 1 || idx > db.cols()) return -1;
    return idx - 1;
  }
  if (f.IsText()) {
    for (int c = 0; c < db.cols(); ++c) {
      const Value& h = ctx.Read(db.sheet, db.r1, db.c1 + c);
      if (EqualsText(ToText(h), f.text())) return c;
    }
  }
  return -1;
}

bool DbRowMatches(EvalContext& ctx, const Area& db, int row, const Area& crit,
                  const std::vector<int>& crit_cols) {
  for (int cr = crit.r1 + 1; cr <= crit.r2; ++cr) {
    bool all = true;
    bool any_cell = false;
    for (int cc = 0; cc < crit.cols() && all; ++cc) {
      const Value& cv = ctx.Read(crit.sheet, cr, crit.c1 + cc);
      if (cv.IsBlank()) continue;
      any_cell = true;
      if (crit_cols[static_cast<size_t>(cc)] < 0) {
        all = false;
        break;
      }
      Value cell = ctx.Read(db.sheet, row, db.c1 + crit_cols[static_cast<size_t>(cc)]);
      Criteria k = ParseCriteria(ctx, cv);
      if (k.kind == Criteria::Kind::Text && k.cmp == Criteria::Cmp::Eq && cv.IsText() &&
          cv.text().substr(0, 1) != "=") {
        k.text += "*";
        k.wildcard = true;
      }
      all = MatchCriteria(k, cell, ctx);
    }
    if (all || !any_cell) return true;
  }
  return false;
}

enum class DbKind { Sum, Average, Count, CountA, Max, Min, Get, Product, StDev, StDevP, Var, VarP };

template <DbKind K>
Operand FnDatabase(EvalContext& ctx, Operand* a, int) {
  Area db;
  Area crit;
  if (!AreaOf(a[0], db) || !AreaOf(a[2], crit)) return MakeErr(Err::Value);
  int field = a[1].IsMissing() ? -1 : FieldIndex(ctx, db, a[1]);
  if (field < 0 && !(K == DbKind::Count || K == DbKind::CountA)) return MakeErr(Err::Value);
  std::vector<int> crit_cols(static_cast<size_t>(crit.cols()), -1);
  for (int cc = 0; cc < crit.cols(); ++cc) {
    std::string label = ToText(ctx.Read(crit.sheet, crit.r1, crit.c1 + cc));
    for (int c = 0; c < db.cols(); ++c) {
      if (EqualsText(ToText(ctx.Read(db.sheet, db.r1, db.c1 + c)), label)) {
        crit_cols[static_cast<size_t>(cc)] = c;
        break;
      }
    }
  }
  std::vector<double> xs;
  double counta = 0;
  int matches = 0;
  Value got;
  int last = std::min(db.r2, ctx.UsedRows(db.sheet) - 1);
  for (int r = db.r1 + 1; r <= last; ++r) {
    if (!DbRowMatches(ctx, db, r, crit, crit_cols)) continue;
    ++matches;
    if (field < 0) {
      counta += 1;
      continue;
    }
    const Value& v = ctx.Read(db.sheet, r, db.c1 + field);
    if (!v.IsBlank()) counta += 1;
    if (v.IsNumber()) xs.push_back(v.number());
    got = v;
  }
  double cnt = static_cast<double>(xs.size());
  switch (K) {
    case DbKind::Sum:
      return MakeNum(Sum(xs));
    case DbKind::Average:
      if (xs.empty()) return MakeErr(Err::Div0);
      return MakeNum(Mean(xs));
    case DbKind::Count:
      return MakeNum(field < 0 ? counta : cnt);
    case DbKind::CountA:
      return MakeNum(counta);
    case DbKind::Max:
      return MakeNum(xs.empty() ? 0 : *std::max_element(xs.begin(), xs.end()));
    case DbKind::Min:
      return MakeNum(xs.empty() ? 0 : *std::min_element(xs.begin(), xs.end()));
    case DbKind::Get:
      if (matches == 0) return MakeErr(Err::Value);
      if (matches > 1) return MakeErr(Err::Num);
      return Operand::Of(got);
    case DbKind::Product: {
      double p = xs.empty() ? 0 : 1;
      for (double x : xs) p *= x;
      return MakeNum(p);
    }
    case DbKind::StDev:
    case DbKind::Var:
    case DbKind::StDevP:
    case DbKind::VarP: {
      bool sample = K == DbKind::StDev || K == DbKind::Var;
      double d = sample ? cnt - 1 : cnt;
      if (d <= 0) return MakeErr(Err::Div0);
      double v = SumSqDev(xs) / d;
      return MakeNum(K == DbKind::StDev || K == DbKind::StDevP ? std::sqrt(v) : v);
    }
  }
  return MakeErr(Err::Value);
}

Operand FnFrequency(EvalContext& ctx, Operand* a, int) {
  std::vector<double> data;
  std::vector<double> bins;
  if (Err e = CollectNumbers(ctx, a, 1, data); e != Err::None) return MakeErr(e);
  if (Err e = CollectNumbers(ctx, a + 1, 1, bins); e != Err::None) return MakeErr(e);
  std::vector<size_t> order(bins.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](size_t x, size_t y) { return bins[x] < bins[y]; });
  std::vector<double> counts(bins.size() + 1, 0);
  for (double d : data) {
    size_t k = 0;
    while (k < order.size() && d > bins[order[k]]) ++k;
    if (k == order.size()) {
      counts[bins.size()] += 1;
    } else {
      counts[order[k]] += 1;
    }
  }
  Value out = Value::Array(static_cast<uint32_t>(counts.size()), 1);
  for (size_t i = 0; i < counts.size(); ++i)
    out.mut(static_cast<uint32_t>(i), 0) = Value::Number(counts[i]);
  return Operand::Of(std::move(out));
}

}

void RegisterStats(std::vector<FuncInfo>& out) {
  const FuncInfo list[] = {
      {"COUNT", 1, 255, "R", 1, 0, FnCount},
      {"COUNTA", 1, 255, "R", 1, 0, FnCountA},
      {"COUNTBLANK", 1, 1, "r", 1, 0, FnCountBlank},
      {"COUNTIF", 2, 2, "rV", 1, kFnLift, FnCountIf},
      {"COUNTIFS", 2, 254, "rV", 2, kFnLift, FnCountIfs},
      {"SUMIF", 2, 3, "rVr", 1, kFnLift, FnSumIf},
      {"SUMIFS", 3, 255, "rrV", 2, kFnLift, FnSumIfs},
      {"AVERAGEIF", 2, 3, "rVr", 1, kFnLift, FnAverageIf},
      {"AVERAGEIFS", 3, 255, "rrV", 2, kFnLift, FnAverageIfs},
      {"MAXIFS", 3, 255, "rrV", 2, kFnLift | kFnXlfn, FnMaxIfs},
      {"MINIFS", 3, 255, "rrV", 2, kFnLift | kFnXlfn, FnMinIfs},
      {"AVERAGE", 1, 255, "R", 1, 0, FnAverage},
      {"AVERAGEA", 1, 255, "R", 1, 0, FnAverageA},
      {"MAX", 1, 255, "R", 1, 0, FnExtreme<true, CollectMode::Numbers>},
      {"MIN", 1, 255, "R", 1, 0, FnExtreme<false, CollectMode::Numbers>},
      {"MAXA", 1, 255, "R", 1, 0, FnExtreme<true, CollectMode::NumbersA>},
      {"MINA", 1, 255, "R", 1, 0, FnExtreme<false, CollectMode::NumbersA>},
      {"MEDIAN", 1, 255, "R", 1, 0, FnMedian},
      {"MODE", 1, 255, "R", 1, 0, FnMode},
      {"MODE.SNGL", 1, 255, "R", 1, kFnXlfn, FnMode},
      {"MODE.MULT", 1, 255, "R", 1, kFnXlfn, FnModeMult},
      {"LARGE", 2, 2, "RV", 1, kFnLift, FnKth<true>},
      {"SMALL", 2, 2, "RV", 1, kFnLift, FnKth<false>},
      {"RANK", 2, 3, "VrV", 1, kFnLift, FnRank<false>},
      {"RANK.EQ", 2, 3, "VrV", 1, kFnLift | kFnXlfn, FnRank<false>},
      {"RANK.AVG", 2, 3, "VrV", 1, kFnLift | kFnXlfn, FnRank<true>},
      {"PERCENTILE", 2, 2, "RV", 1, kFnLift, FnPercentile<false>},
      {"PERCENTILE.INC", 2, 2, "RV", 1, kFnLift | kFnXlfn, FnPercentile<false>},
      {"PERCENTILE.EXC", 2, 2, "RV", 1, kFnLift | kFnXlfn, FnPercentile<true>},
      {"QUARTILE", 2, 2, "RV", 1, kFnLift, FnQuartile<false>},
      {"QUARTILE.INC", 2, 2, "RV", 1, kFnLift | kFnXlfn, FnQuartile<false>},
      {"QUARTILE.EXC", 2, 2, "RV", 1, kFnLift | kFnXlfn, FnQuartile<true>},
      {"PERCENTRANK", 2, 3, "RVV", 1, kFnLift, FnPercentRank<false>},
      {"PERCENTRANK.INC", 2, 3, "RVV", 1, kFnLift | kFnXlfn, FnPercentRank<false>},
      {"PERCENTRANK.EXC", 2, 3, "RVV", 1, kFnLift | kFnXlfn, FnPercentRank<true>},
      {"STDEV", 1, 255, "R", 1, 0, FnVariance<true, CollectMode::Numbers, true>},
      {"STDEV.S", 1, 255, "R", 1, kFnXlfn, FnVariance<true, CollectMode::Numbers, true>},
      {"STDEVP", 1, 255, "R", 1, 0, FnVariance<false, CollectMode::Numbers, true>},
      {"STDEV.P", 1, 255, "R", 1, kFnXlfn, FnVariance<false, CollectMode::Numbers, true>},
      {"STDEVA", 1, 255, "R", 1, 0, FnVariance<true, CollectMode::NumbersA, true>},
      {"STDEVPA", 1, 255, "R", 1, 0, FnVariance<false, CollectMode::NumbersA, true>},
      {"VAR", 1, 255, "R", 1, 0, FnVariance<true, CollectMode::Numbers, false>},
      {"VAR.S", 1, 255, "R", 1, kFnXlfn, FnVariance<true, CollectMode::Numbers, false>},
      {"VARP", 1, 255, "R", 1, 0, FnVariance<false, CollectMode::Numbers, false>},
      {"VAR.P", 1, 255, "R", 1, kFnXlfn, FnVariance<false, CollectMode::Numbers, false>},
      {"VARA", 1, 255, "R", 1, 0, FnVariance<true, CollectMode::NumbersA, false>},
      {"VARPA", 1, 255, "R", 1, 0, FnVariance<false, CollectMode::NumbersA, false>},
      {"DEVSQ", 1, 255, "R", 1, 0, FnDevSq},
      {"AVEDEV", 1, 255, "R", 1, 0, FnAveDev},
      {"GEOMEAN", 1, 255, "R", 1, 0, FnGeoMean},
      {"HARMEAN", 1, 255, "R", 1, 0, FnHarMean},
      {"KURT", 1, 255, "R", 1, 0, FnKurt},
      {"SKEW", 1, 255, "R", 1, 0, FnSkew<false>},
      {"SKEW.P", 1, 255, "R", 1, kFnXlfn, FnSkew<true>},
      {"TRIMMEAN", 2, 2, "RV", 1, 0, FnTrimMean},
      {"CORREL", 2, 2, "AA", 1, 0, FnCorrel},
      {"PEARSON", 2, 2, "AA", 1, 0, FnCorrel},
      {"RSQ", 2, 2, "AA", 1, 0, FnRsq},
      {"SLOPE", 2, 2, "AA", 1, 0, FnSlope},
      {"INTERCEPT", 2, 2, "AA", 1, 0, FnIntercept},
      {"STEYX", 2, 2, "AA", 1, 0, FnSteyx},
      {"FORECAST", 3, 3, "VAA", 1, kFnLift, FnForecast},
      {"FORECAST.LINEAR", 3, 3, "VAA", 1, kFnLift | kFnXlfn, FnForecast},
      {"COVAR", 2, 2, "AA", 1, 0, FnCovar<false>},
      {"COVARIANCE.P", 2, 2, "AA", 1, kFnXlfn, FnCovar<false>},
      {"COVARIANCE.S", 2, 2, "AA", 1, kFnXlfn, FnCovar<true>},
      {"NORMDIST", 4, 4, "VVVV", 1, kFnLift, FnNormDist},
      {"NORM.DIST", 4, 4, "VVVV", 1, kFnLift | kFnXlfn, FnNormDist},
      {"NORMSDIST", 1, 1, "V", 1, kFnLift, FnNormSDistLegacy},
      {"NORM.S.DIST", 2, 2, "VV", 1, kFnLift | kFnXlfn, FnNormSDist},
      {"NORMINV", 3, 3, "VVV", 1, kFnLift, FnNormInv},
      {"NORM.INV", 3, 3, "VVV", 1, kFnLift | kFnXlfn, FnNormInv},
      {"NORMSINV", 1, 1, "V", 1, kFnLift, FnNormSInv},
      {"NORM.S.INV", 1, 1, "V", 1, kFnLift | kFnXlfn, FnNormSInv},
      {"STANDARDIZE", 3, 3, "VVV", 1, kFnLift, FnStandardize},
      {"PHI", 1, 1, "V", 1, kFnLift | kFnXlfn, FnPhi},
      {"GAUSS", 1, 1, "V", 1, kFnLift | kFnXlfn, FnGauss},
      {"CONFIDENCE", 3, 3, "VVV", 1, kFnLift, FnConfidenceNorm},
      {"CONFIDENCE.NORM", 3, 3, "VVV", 1, kFnLift | kFnXlfn, FnConfidenceNorm},
      {"CONFIDENCE.T", 3, 3, "VVV", 1, kFnLift | kFnXlfn, FnConfidenceT},
      {"EXPONDIST", 3, 3, "VVV", 1, kFnLift, FnExponDist},
      {"EXPON.DIST", 3, 3, "VVV", 1, kFnLift | kFnXlfn, FnExponDist},
      {"POISSON", 3, 3, "VVV", 1, kFnLift, FnPoisson},
      {"POISSON.DIST", 3, 3, "VVV", 1, kFnLift | kFnXlfn, FnPoisson},
      {"BINOMDIST", 4, 4, "VVVV", 1, kFnLift, FnBinomDist},
      {"BINOM.DIST", 4, 4, "VVVV", 1, kFnLift | kFnXlfn, FnBinomDist},
      {"HYPGEOMDIST", 4, 4, "VVVV", 1, kFnLift, FnHypGeomDist},
      {"HYPGEOM.DIST", 5, 5, "VVVVV", 1, kFnLift | kFnXlfn, FnHypGeomDist},
      {"NEGBINOMDIST", 3, 3, "VVV", 1, kFnLift, FnNegBinomDist},
      {"NEGBINOM.DIST", 4, 4, "VVVV", 1, kFnLift | kFnXlfn, FnNegBinomDist},
      {"WEIBULL", 4, 4, "VVVV", 1, kFnLift, FnWeibull},
      {"WEIBULL.DIST", 4, 4, "VVVV", 1, kFnLift | kFnXlfn, FnWeibull},
      {"LOGNORMDIST", 3, 3, "VVV", 1, kFnLift, FnLogNormDist},
      {"LOGNORM.DIST", 4, 4, "VVVV", 1, kFnLift | kFnXlfn, FnLogNormDist},
      {"LOGINV", 3, 3, "VVV", 1, kFnLift, FnLogNormInv},
      {"LOGNORM.INV", 3, 3, "VVV", 1, kFnLift | kFnXlfn, FnLogNormInv},
      {"GAMMALN", 1, 1, "V", 1, kFnLift, FnGammaLn},
      {"GAMMALN.PRECISE", 1, 1, "V", 1, kFnLift | kFnXlfn, FnGammaLn},
      {"GAMMA", 1, 1, "V", 1, kFnLift | kFnXlfn, FnGamma},
      {"FISHER", 1, 1, "V", 1, kFnLift, FnFisher},
      {"FISHERINV", 1, 1, "V", 1, kFnLift, FnFisherInv},
      {"ERF", 1, 2, "VV", 1, kFnLift, FnErf},
      {"ERF.PRECISE", 1, 1, "V", 1, kFnLift | kFnXlfn, FnErf},
      {"ERFC", 1, 1, "V", 1, kFnLift, FnErfc},
      {"ERFC.PRECISE", 1, 1, "V", 1, kFnLift | kFnXlfn, FnErfc},
      {"PERMUT", 2, 2, "VV", 1, kFnLift, FnPermut},
      {"PERMUTATIONA", 2, 2, "VV", 1, kFnLift | kFnXlfn, FnPermutationA},
      {"CHISQ.DIST", 3, 3, "VVV", 1, kFnLift | kFnXlfn, FnChiSqDist},
      {"CHISQ.DIST.RT", 2, 2, "VV", 1, kFnLift | kFnXlfn, FnChiSqDistRt},
      {"CHIDIST", 2, 2, "VV", 1, kFnLift, FnChiSqDistRt},
      {"CHISQ.INV", 2, 2, "VV", 1, kFnLift | kFnXlfn, FnChiSqInv<false>},
      {"CHISQ.INV.RT", 2, 2, "VV", 1, kFnLift | kFnXlfn, FnChiSqInv<true>},
      {"CHIINV", 2, 2, "VV", 1, kFnLift, FnChiSqInv<true>},
      {"GAMMA.DIST", 4, 4, "VVVV", 1, kFnLift | kFnXlfn, FnGammaDist},
      {"GAMMADIST", 4, 4, "VVVV", 1, kFnLift, FnGammaDist},
      {"GAMMA.INV", 3, 3, "VVV", 1, kFnLift | kFnXlfn, FnGammaInv},
      {"GAMMAINV", 3, 3, "VVV", 1, kFnLift, FnGammaInv},
      {"BETADIST", 3, 5, "VVVVV", 1, kFnLift, FnBetaDist},
      {"BETA.DIST", 4, 6, "VVVVVV", 1, kFnLift | kFnXlfn, FnBetaDistNew},
      {"BETAINV", 3, 5, "VVVVV", 1, kFnLift, FnBetaInv},
      {"BETA.INV", 3, 5, "VVVVV", 1, kFnLift | kFnXlfn, FnBetaInv},
      {"T.DIST", 3, 3, "VVV", 1, kFnLift | kFnXlfn, FnTDist},
      {"T.DIST.RT", 2, 2, "VV", 1, kFnLift | kFnXlfn, FnTDistRt},
      {"T.DIST.2T", 2, 2, "VV", 1, kFnLift | kFnXlfn, FnTDist2T},
      {"TDIST", 3, 3, "VVV", 1, kFnLift, FnTDistLegacy},
      {"T.INV", 2, 2, "VV", 1, kFnLift | kFnXlfn, FnTInv<false>},
      {"T.INV.2T", 2, 2, "VV", 1, kFnLift | kFnXlfn, FnTInv<true>},
      {"TINV", 2, 2, "VV", 1, kFnLift, FnTInv<true>},
      {"F.DIST", 4, 4, "VVVV", 1, kFnLift | kFnXlfn, FnFDist},
      {"F.DIST.RT", 3, 3, "VVV", 1, kFnLift | kFnXlfn, FnFDistRt},
      {"FDIST", 3, 3, "VVV", 1, kFnLift, FnFDistRt},
      {"F.INV", 3, 3, "VVV", 1, kFnLift | kFnXlfn, FnFInv<false>},
      {"F.INV.RT", 3, 3, "VVV", 1, kFnLift | kFnXlfn, FnFInv<true>},
      {"FINV", 3, 3, "VVV", 1, kFnLift, FnFInv<true>},
      {"Z.TEST", 2, 3, "RVV", 1, kFnXlfn, FnZTest},
      {"ZTEST", 2, 3, "RVV", 1, 0, FnZTest},
      {"CHISQ.TEST", 2, 2, "AA", 1, kFnXlfn, FnChiSqTest},
      {"CHITEST", 2, 2, "AA", 1, 0, FnChiSqTest},
      {"T.TEST", 4, 4, "AAVV", 1, kFnXlfn, FnTTest},
      {"TTEST", 4, 4, "AAVV", 1, 0, FnTTest},
      {"F.TEST", 2, 2, "AA", 1, kFnXlfn, FnFTest},
      {"FTEST", 2, 2, "AA", 1, 0, FnFTest},
      {"FREQUENCY", 2, 2, "AA", 1, 0, FnFrequency},
      {"DSUM", 3, 3, "rVr", 1, 0, FnDatabase<DbKind::Sum>},
      {"DAVERAGE", 3, 3, "rVr", 1, 0, FnDatabase<DbKind::Average>},
      {"DCOUNT", 3, 3, "rVr", 1, 0, FnDatabase<DbKind::Count>},
      {"DCOUNTA", 3, 3, "rVr", 1, 0, FnDatabase<DbKind::CountA>},
      {"DMAX", 3, 3, "rVr", 1, 0, FnDatabase<DbKind::Max>},
      {"DMIN", 3, 3, "rVr", 1, 0, FnDatabase<DbKind::Min>},
      {"DGET", 3, 3, "rVr", 1, 0, FnDatabase<DbKind::Get>},
      {"DPRODUCT", 3, 3, "rVr", 1, 0, FnDatabase<DbKind::Product>},
      {"DSTDEV", 3, 3, "rVr", 1, 0, FnDatabase<DbKind::StDev>},
      {"DSTDEVP", 3, 3, "rVr", 1, 0, FnDatabase<DbKind::StDevP>},
      {"DVAR", 3, 3, "rVr", 1, 0, FnDatabase<DbKind::Var>},
      {"DVARP", 3, 3, "rVr", 1, 0, FnDatabase<DbKind::VarP>},
  };
  out.insert(out.end(), std::begin(list), std::end(list));
}

}
