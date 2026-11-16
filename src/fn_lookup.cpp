#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "eval.h"

namespace sheet {

namespace {

#define NUM_ARG(i, var) \
  double var;           \
  if (Err e_ = ArgNumber(ctx, a[i], var); e_ != Err::None) return MakeErr(e_)

Err LogicalFold(EvalContext& ctx, Operand* a, int n, int& trues, int& total) {
  trues = 0;
  total = 0;
  for (int i = 0; i < n; ++i) {
    const Operand& o = a[i];
    if (o.IsMissing()) {
      ++total;
      continue;
    }
    Err bad = Err::None;
    auto element = [&](const Value& v) {
      if (bad != Err::None) return;
      if (v.IsError()) {
        bad = v.error();
      } else if (v.IsBool()) {
        ++total;
        trues += v.boolean() ? 1 : 0;
      } else if (v.IsNumber()) {
        ++total;
        trues += v.number() != 0 ? 1 : 0;
      }
    };
    if (o.tag == Operand::Tag::Ref) {
      ctx.Scan(o.area, [&](int, int, const Value& v) { element(v); });
    } else if (o.tag == Operand::Tag::Refs) {
      for (const Area& ar : *o.list) ctx.Scan(ar, [&](int, int, const Value& v) { element(v); });
    } else if (o.value.IsArray()) {
      for (uint32_t r = 0; r < o.value.rows(); ++r) {
        for (uint32_t c = 0; c < o.value.cols(); ++c) element(o.value.at(r, c));
      }
    } else {
      const Value& v = o.value;
      if (v.IsText()) {
        bool b;
        if (ToBool(v, b) != Err::None) return Err::Value;
        ++total;
        trues += b ? 1 : 0;
      } else if (v.IsBlank()) {
        continue;
      } else {
        element(v);
      }
    }
    if (bad != Err::None) return bad;
  }
  if (total == 0) return Err::Value;
  return Err::None;
}

Operand FnAnd(EvalContext& ctx, Operand* a, int n) {
  int t;
  int total;
  if (Err e = LogicalFold(ctx, a, n, t, total); e != Err::None) return MakeErr(e);
  return MakeBool(t == total);
}
Operand FnOr(EvalContext& ctx, Operand* a, int n) {
  int t;
  int total;
  if (Err e = LogicalFold(ctx, a, n, t, total); e != Err::None) return MakeErr(e);
  return MakeBool(t > 0);
}
Operand FnXor(EvalContext& ctx, Operand* a, int n) {
  int t;
  int total;
  if (Err e = LogicalFold(ctx, a, n, t, total); e != Err::None) return MakeErr(e);
  return MakeBool(t % 2 == 1);
}
Operand FnNot(EvalContext& ctx, Operand* a, int) {
  bool b;
  if (Err e = ArgBool(ctx, a[0], b); e != Err::None) return MakeErr(e);
  return MakeBool(!b);
}
Operand FnTrue(EvalContext&, Operand*, int) {
  return MakeBool(true);
}
Operand FnFalse(EvalContext&, Operand*, int) {
  return MakeBool(false);
}

Operand FnIfs(EvalContext& ctx, Operand* a, int n) {
  if (n % 2 != 0) return MakeErr(Err::Value);
  for (int i = 0; i < n; i += 2) {
    bool b;
    if (Err e = ArgBool(ctx, a[i], b); e != Err::None) return MakeErr(e);
    if (b) return a[i + 1].IsMissing() ? MakeNum(0) : a[i + 1];
  }
  return MakeErr(Err::NA);
}

Operand FnSwitch(EvalContext& ctx, Operand* a, int n) {
  Value x = ctx.Scalar(a[0]);
  if (x.IsError()) return Operand::Of(x);
  int i = 1;
  for (; i + 1 < n; i += 2) {
    Value v = ctx.Scalar(a[i]);
    if (v.IsError()) return Operand::Of(v);
    if (x.kind() == v.kind() && CompareValues(x, v) == 0) return a[i + 1];
  }
  if (i < n) return a[i];
  return MakeErr(Err::NA);
}

Operand FnChoose(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, x);
  int idx = ToInt(x);
  if (idx < 1 || idx >= n) return MakeErr(Err::Value);
  if (a[idx].IsMissing()) return MakeNum(0);
  return a[idx];
}

bool SameKind(const Value& a, const Value& b) {
  if (a.IsNumber() && b.IsNumber()) return true;
  if (a.IsText() && b.IsText()) return true;
  if (a.IsBool() && b.IsBool()) return true;
  return false;
}

bool ExactEqual(const Value& lookup, const Value& v, bool wildcard) {
  if (!SameKind(lookup, v)) return false;
  if (lookup.IsText() && wildcard && HasWildcards(lookup.text())) {
    return WildcardMatch(lookup.text(), v.text());
  }
  return CompareValues(lookup, v) == 0;
}

template <typename Get>
int LinearFind(Get get, int n, const Value& lookup, bool wildcard, bool reverse) {
  for (int k = 0; k < n; ++k) {
    int i = reverse ? n - 1 - k : k;
    if (ExactEqual(lookup, get(i), wildcard)) return i;
  }
  return -1;
}

template <typename Get>
int BinaryFind(Get get, int n, const Value& lookup, bool descending) {
  auto cmp = [&](int i) -> int {
    const Value& v = get(i);
    if (!SameKind(v, lookup)) return -2;
    return CompareValues(lookup, v);
  };
  if (descending) {
    for (int i = 0; i < n; ++i) {
      int c = cmp(i);
      if (c == 0) return i;
      if (c == 1) return i - 1;
    }
    return n - 1;
  }
  int low = -1;
  int high = n;
  while (true) {
    if (high - low < 2) return low;
    int mid = low + (high - low) / 2;
    int c = cmp(mid);
    if (c == -2) {
      int probe = mid;
      bool narrowed = false;
      while (true) {
        ++probe;
        if (probe == high) {
          high = mid;
          narrowed = true;
          break;
        }
        int c2 = cmp(probe);
        if (c2 == -1 && probe == high - 1) {
          high = mid;
          narrowed = true;
          break;
        }
        if (c2 == -2) continue;
        mid = probe;
        c = c2;
        break;
      }
      if (narrowed) continue;
    }
    if (c == 0) {
      int i = mid;
      while (i + 1 < high && cmp(i + 1) == 0) ++i;
      return i;
    }
    if (c == -1) {
      high = mid;
    } else {
      low = mid;
    }
  }
}

Operand Lookup1D(const Grid& g, bool by_rows, const Value& lookup, int mode) {
  int n = by_rows ? g.rows : g.cols;
  auto get = [&](int i) -> const Value& { return by_rows ? g.At(i, 0) : g.At(0, i); };
  int idx;
  if (mode == 0) {
    idx = LinearFind(get, n, lookup, true, false);
  } else {
    idx = BinaryFind(get, n, lookup, mode < 0);
  }
  if (idx < 0) return MakeErr(Err::NA);
  return MakeNum(idx + 1);
}

Operand FnMatch(EvalContext& ctx, Operand* a, int n) {
  Value lookup = ctx.Scalar(a[0]);
  if (lookup.IsError()) return Operand::Of(lookup);
  if (lookup.IsBlank()) return MakeErr(Err::NA);
  double type;
  if (Err e = ArgNumberOr(ctx, a, n, 2, 1, type); e != Err::None) return MakeErr(e);
  Grid g;
  if (a[1].IsError()) return MakeErr(a[1].value.error());
  if (!MakeGrid(ctx, a[1], g, true)) return MakeErr(Err::NA);
  Area full;
  bool is_ref = AreaOf(a[1], full);
  int rows = is_ref ? full.rows() : g.rows;
  int cols = is_ref ? full.cols() : g.cols;
  if (rows != 1 && cols != 1) return MakeErr(Err::NA);
  bool by_rows = cols == 1 && rows >= 1 && !(rows == 1 && cols > 1);
  int mode = type > 0 ? 1 : (type < 0 ? -1 : 0);
  return Lookup1D(g, by_rows, lookup, mode);
}

Operand TableLookup(EvalContext& ctx, Operand* a, int n, bool vertical) {
  Value lookup = ctx.Scalar(a[0]);
  if (lookup.IsError()) return Operand::Of(lookup);
  NUM_ARG(2, idx0);
  bool approx;
  if (Err e = ArgBoolOr(ctx, a, n, 3, true, approx); e != Err::None) return MakeErr(e);
  if (a[1].IsError()) return MakeErr(a[1].value.error());
  Grid g;
  if (!MakeGrid(ctx, a[1], g, true)) return MakeErr(Err::Value);
  Area full;
  bool is_ref = AreaOf(a[1], full);
  int span = vertical ? (is_ref ? full.cols() : g.cols) : (is_ref ? full.rows() : g.rows);
  int idx = ToInt(idx0);
  if (idx < 1) return MakeErr(Err::Value);
  if (idx > span) return MakeErr(Err::Ref);
  int count = vertical ? g.rows : g.cols;
  auto key = [&](int i) -> const Value& { return vertical ? g.At(i, 0) : g.At(0, i); };
  int hit;
  if (lookup.IsBlank()) return MakeErr(Err::NA);
  if (approx) {
    hit = BinaryFind(key, count, lookup, false);
  } else {
    hit = LinearFind(key, count, lookup, true, false);
  }
  if (hit < 0) return MakeErr(Err::NA);
  if (is_ref) {
    int r = vertical ? full.r1 + hit : full.r1 + idx - 1;
    int c = vertical ? full.c1 + idx - 1 : full.c1 + hit;
    return Operand::OfRef(Area{full.sheet, r, c, r, c});
  }
  return Operand::Of(vertical ? g.At(hit, idx - 1) : g.At(idx - 1, hit));
}

Operand FnVlookup(EvalContext& ctx, Operand* a, int n) {
  return TableLookup(ctx, a, n, true);
}
Operand FnHlookup(EvalContext& ctx, Operand* a, int n) {
  return TableLookup(ctx, a, n, false);
}

Operand FnLookup(EvalContext& ctx, Operand* a, int n) {
  Value lookup = ctx.Scalar(a[0]);
  if (lookup.IsError()) return Operand::Of(lookup);
  if (a[1].IsError()) return MakeErr(a[1].value.error());
  Grid g;
  if (!MakeGrid(ctx, a[1], g, true)) return MakeErr(Err::NA);
  Area full;
  bool is_ref = AreaOf(a[1], full);
  int rows = is_ref ? full.rows() : g.rows;
  int cols = is_ref ? full.cols() : g.cols;
  if (lookup.IsBlank()) return MakeErr(Err::NA);
  if (n < 3 || a[2].IsMissing()) {
    bool by_rows = rows >= cols;
    int count = by_rows ? g.rows : g.cols;
    auto key = [&](int i) -> const Value& { return by_rows ? g.At(i, 0) : g.At(0, i); };
    int hit = BinaryFind(key, count, lookup, false);
    if (hit < 0) return MakeErr(Err::NA);
    if (is_ref) {
      int r = by_rows ? full.r1 + hit : full.r2;
      int c = by_rows ? full.c2 : full.c1 + hit;
      return Operand::OfRef(Area{full.sheet, r, c, r, c});
    }
    return Operand::Of(by_rows ? g.At(hit, g.cols - 1) : g.At(g.rows - 1, hit));
  }
  if (a[2].IsError()) return MakeErr(a[2].value.error());
  Area res;
  bool res_ref = AreaOf(a[2], res);
  if (res_ref && res.rows() > 1 && res.cols() > 1) return MakeErr(Err::Value);
  bool by_rows = rows >= cols;
  int count = by_rows ? g.rows : g.cols;
  auto key = [&](int i) -> const Value& { return by_rows ? g.At(i, 0) : g.At(0, i); };
  int hit = BinaryFind(key, count, lookup, false);
  if (hit < 0) return MakeErr(Err::NA);
  if (res_ref) {
    bool res_rows = res.cols() == 1 && res.rows() >= 1 && !(res.rows() == 1 && res.cols() > 1);
    if (res.rows() == 1 && res.cols() == 1) res_rows = by_rows;
    int r = res_rows ? res.r1 + hit : res.r1;
    int c = res_rows ? res.c1 : res.c1 + hit;
    if (r >= kMaxRows || c >= kMaxCols) return MakeErr(Err::Ref);
    return Operand::OfRef(Area{res.sheet, r, c, r, c});
  }
  Grid rg;
  if (!MakeGrid(ctx, a[2], rg, false)) return MakeErr(Err::NA);
  if (rg.rows > 1 && rg.cols > 1) return MakeErr(Err::Value);
  bool res_rows = rg.cols == 1;
  int lim = res_rows ? rg.rows : rg.cols;
  if (hit >= lim) return MakeErr(Err::NA);
  return Operand::Of(res_rows ? rg.At(hit, 0) : rg.At(0, hit));
}

int XFind(const Grid& g, bool by_rows, const Value& lookup, int match_mode, int search_mode) {
  int n = by_rows ? g.rows : g.cols;
  auto get = [&](int i) -> const Value& { return by_rows ? g.At(i, 0) : g.At(0, i); };
  if (search_mode == 2 || search_mode == -2) {
    bool desc = search_mode == -2;
    int lo = 0;
    int hi = n - 1;
    int exact = -1;
    int below = -1;
    int above = -1;
    while (lo <= hi) {
      int mid = lo + (hi - lo) / 2;
      const Value& v = get(mid);
      int c = SameKind(v, lookup) ? CompareValues(v, lookup) : (v.IsBlank() ? -1 : 1);
      if (desc) c = -c;
      if (c == 0) {
        exact = mid;
        break;
      }
      if (c < 0) {
        if (desc) {
          above = mid;
        } else {
          below = mid;
        }
        lo = mid + 1;
      } else {
        if (desc) {
          below = mid;
        } else {
          above = mid;
        }
        hi = mid - 1;
      }
    }
    if (exact >= 0) return exact;
    if (match_mode == -1) return below;
    if (match_mode == 1) return above;
    return -1;
  }
  bool reverse = search_mode == -1;
  int best = -1;
  for (int k = 0; k < n; ++k) {
    int i = reverse ? n - 1 - k : k;
    const Value& v = get(i);
    if (match_mode == 2) {
      if (ExactEqual(lookup, v, true)) return i;
      continue;
    }
    if (ExactEqual(lookup, v, false)) return i;
    if (match_mode == 0 || !SameKind(v, lookup)) continue;
    int c = CompareValues(v, lookup);
    if (match_mode == -1 && c < 0) {
      if (best < 0 || CompareValues(v, get(best)) > 0) best = i;
    }
    if (match_mode == 1 && c > 0) {
      if (best < 0 || CompareValues(v, get(best)) < 0) best = i;
    }
  }
  return best;
}

Operand FnXlookup(EvalContext& ctx, Operand* a, int n) {
  Value lookup = ctx.Scalar(a[0]);
  if (lookup.IsError()) return Operand::Of(lookup);
  double mm;
  double sm;
  if (Err e = ArgNumberOr(ctx, a, n, 4, 0, mm); e != Err::None) return MakeErr(e);
  if (Err e = ArgNumberOr(ctx, a, n, 5, 1, sm); e != Err::None) return MakeErr(e);
  int match_mode = ToInt(mm);
  int search_mode = ToInt(sm);
  if (match_mode < -1 || match_mode > 2) return MakeErr(Err::Value);
  if (search_mode != 1 && search_mode != -1 && search_mode != 2 && search_mode != -2) {
    return MakeErr(Err::Value);
  }
  if (a[1].IsError()) return MakeErr(a[1].value.error());
  if (a[2].IsError()) return MakeErr(a[2].value.error());
  Grid g;
  if (!MakeGrid(ctx, a[1], g, false)) return MakeErr(Err::Value);
  if (g.rows != 1 && g.cols != 1) return MakeErr(Err::Value);
  bool by_rows = g.cols == 1 && g.rows >= 1;
  if (g.rows == 1 && g.cols > 1) by_rows = false;
  Grid rg;
  if (!MakeGrid(ctx, a[2], rg, false)) return MakeErr(Err::Value);
  int len = by_rows ? g.rows : g.cols;
  int rlen = by_rows ? rg.rows : rg.cols;
  if (len != rlen) return MakeErr(Err::Value);
  int hit = XFind(g, by_rows, lookup, match_mode, search_mode);
  if (hit < 0) {
    if (n > 3 && !a[3].IsMissing()) return a[3];
    return MakeErr(Err::NA);
  }
  Area ra;
  if (AreaOf(a[2], ra)) {
    if (by_rows) return Operand::OfRef(Area{ra.sheet, ra.r1 + hit, ra.c1, ra.r1 + hit, ra.c2});
    return Operand::OfRef(Area{ra.sheet, ra.r1, ra.c1 + hit, ra.r2, ra.c1 + hit});
  }
  if (by_rows) {
    if (rg.cols == 1) return Operand::Of(rg.At(hit, 0));
    Value out = Value::Array(1, static_cast<uint32_t>(rg.cols));
    for (int c = 0; c < rg.cols; ++c) out.mut(0, static_cast<uint32_t>(c)) = rg.At(hit, c);
    return Operand::Of(std::move(out));
  }
  if (rg.rows == 1) return Operand::Of(rg.At(0, hit));
  Value out = Value::Array(static_cast<uint32_t>(rg.rows), 1);
  for (int r = 0; r < rg.rows; ++r) out.mut(static_cast<uint32_t>(r), 0) = rg.At(r, hit);
  return Operand::Of(std::move(out));
}

Operand FnXmatch(EvalContext& ctx, Operand* a, int n) {
  Value lookup = ctx.Scalar(a[0]);
  if (lookup.IsError()) return Operand::Of(lookup);
  double mm;
  double sm;
  if (Err e = ArgNumberOr(ctx, a, n, 2, 0, mm); e != Err::None) return MakeErr(e);
  if (Err e = ArgNumberOr(ctx, a, n, 3, 1, sm); e != Err::None) return MakeErr(e);
  int match_mode = ToInt(mm);
  int search_mode = ToInt(sm);
  if (match_mode < -1 || match_mode > 2) return MakeErr(Err::Value);
  if (a[1].IsError()) return MakeErr(a[1].value.error());
  Grid g;
  if (!MakeGrid(ctx, a[1], g, false)) return MakeErr(Err::Value);
  if (g.rows != 1 && g.cols != 1) return MakeErr(Err::Value);
  bool by_rows = g.cols == 1;
  if (g.rows == 1 && g.cols > 1) by_rows = false;
  int hit = XFind(g, by_rows, lookup, match_mode, search_mode);
  if (hit < 0) return MakeErr(Err::NA);
  return MakeNum(hit + 1);
}

Operand FnIndex(EvalContext& ctx, Operand* a, int n) {
  double r0;
  double c0 = 0;
  double area0;
  if (Err e = ArgNumberOr(ctx, a, n, 1, 0, r0); e != Err::None) return MakeErr(e);
  bool has_col = n > 2 && !a[2].IsMissing();
  if (has_col) {
    if (Err e = ArgNumber(ctx, a[2], c0); e != Err::None) return MakeErr(e);
  }
  if (Err e = ArgNumberOr(ctx, a, n, 3, 1, area0); e != Err::None) return MakeErr(e);
  int r = ToInt(r0);
  int c = ToInt(c0);
  int area_no = ToInt(area0);
  if (r < 0 || c < 0) return MakeErr(Err::Value);
  const Operand& src = a[0];
  if (src.IsError()) return MakeErr(src.value.error());
  if (src.IsRef()) {
    Area ar;
    if (src.tag == Operand::Tag::Ref) {
      if (area_no != 1) return MakeErr(Err::Ref);
      ar = src.area;
    } else {
      if (area_no < 1 || area_no > static_cast<int>(src.list->size())) return MakeErr(Err::Ref);
      ar = (*src.list)[static_cast<size_t>(area_no - 1)];
    }
    int rows = ar.rows();
    int cols = ar.cols();
    if (!has_col && rows == 1 && cols > 1) {
      c = r;
      r = 1;
    }
    if (!has_col && cols == 1) c = 1;
    if (r > rows || c > cols) return MakeErr(Err::Ref);
    Area out = ar;
    if (r > 0) out.r1 = out.r2 = ar.r1 + r - 1;
    if (c > 0) out.c1 = out.c2 = ar.c1 + c - 1;
    return Operand::OfRef(out);
  }
  const Value& v = src.value;
  if (!v.IsArray()) {
    if ((r == 0 || r == 1) && (c == 0 || c == 1)) return Operand::Of(v);
    return MakeErr(Err::Ref);
  }
  int rows = static_cast<int>(v.rows());
  int cols = static_cast<int>(v.cols());
  if (!has_col && rows == 1 && cols > 1) {
    c = r;
    r = 1;
  }
  if (!has_col && cols == 1) c = 1;
  if (r > rows || c > cols) return MakeErr(Err::Ref);
  if (r > 0 && c > 0)
    return Operand::Of(v.at(static_cast<uint32_t>(r - 1), static_cast<uint32_t>(c - 1)));
  if (r > 0) {
    Value out = Value::Array(1, static_cast<uint32_t>(cols));
    for (int k = 0; k < cols; ++k)
      out.mut(0, static_cast<uint32_t>(k)) =
          v.at(static_cast<uint32_t>(r - 1), static_cast<uint32_t>(k));
    return Operand::Of(std::move(out));
  }
  if (c > 0) {
    Value out = Value::Array(static_cast<uint32_t>(rows), 1);
    for (int k = 0; k < rows; ++k)
      out.mut(static_cast<uint32_t>(k), 0) =
          v.at(static_cast<uint32_t>(k), static_cast<uint32_t>(c - 1));
    return Operand::Of(std::move(out));
  }
  return Operand::Of(v);
}

Operand FnOffset(EvalContext& ctx, Operand* a, int n) {
  if (a[0].IsError()) return MakeErr(a[0].value.error());
  if (a[0].tag != Operand::Tag::Ref) return MakeErr(Err::Value);
  NUM_ARG(1, dr0);
  NUM_ARG(2, dc0);
  const Area& base = a[0].area;
  double h0;
  double w0;
  if (Err e = ArgNumberOr(ctx, a, n, 3, base.rows(), h0); e != Err::None) return MakeErr(e);
  if (Err e = ArgNumberOr(ctx, a, n, 4, base.cols(), w0); e != Err::None) return MakeErr(e);
  long long dr = ToInt64(dr0);
  long long dc = ToInt64(dc0);
  long long h = ToInt64(h0);
  long long w = ToInt64(w0);
  if (h == 0 || w == 0) return MakeErr(Err::Ref);
  if (std::llabs(dr) > kMaxRows || std::llabs(dc) > kMaxCols || std::llabs(h) > kMaxRows ||
      std::llabs(w) > kMaxCols) {
    return MakeErr(Err::Ref);
  }
  long long r1 = base.r1 + dr;
  long long c1 = base.c1 + dc;
  long long r2 = h > 0 ? r1 + h - 1 : r1;
  long long c2 = w > 0 ? c1 + w - 1 : c1;
  if (h < 0) r1 = r1 + h + 1;
  if (w < 0) c1 = c1 + w + 1;
  if (r1 < 0 || c1 < 0 || r2 >= kMaxRows || c2 >= kMaxCols) return MakeErr(Err::Ref);
  return Operand::OfRef(Area{base.sheet, static_cast<int>(r1), static_cast<int>(c1),
                             static_cast<int>(r2), static_cast<int>(c2)});
}

Operand FnIndirect(EvalContext& ctx, Operand* a, int n) {
  std::string text;
  if (Err e = ArgText(ctx, a[0], text); e != Err::None) return MakeErr(e);
  bool a1;
  if (Err e = ArgBoolOr(ctx, a, n, 1, true, a1); e != Err::None) return MakeErr(e);
  Area out;
  if (!ctx.ParseRef(text, a1, out)) return MakeErr(Err::Ref);
  return Operand::OfRef(out);
}

Operand Position(EvalContext& ctx, Operand* a, int n, bool row) {
  if (n == 0 || a[0].IsMissing()) {
    int first = (row ? ctx.row : ctx.col) + 1;
    int count = row ? ctx.array_rows : ctx.array_cols;
    if (!ctx.array_call || count <= 1) return MakeNum(first);
    Value out = row ? Value::Array(static_cast<uint32_t>(count), 1)
                    : Value::Array(1, static_cast<uint32_t>(count));
    for (int i = 0; i < count; ++i) {
      Value& slot =
          row ? out.mut(static_cast<uint32_t>(i), 0) : out.mut(0, static_cast<uint32_t>(i));
      slot = Value::Number(first + i);
    }
    return Operand::Of(std::move(out));
  }
  if (a[0].IsError()) return MakeErr(a[0].value.error());
  if (a[0].tag != Operand::Tag::Ref) return MakeErr(Err::Value);
  const Area& ar = a[0].area;
  int first = row ? ar.r1 : ar.c1;
  int count = row ? ar.rows() : ar.cols();
  if (count == 1 || !ctx.array_call) return MakeNum(first + 1);
  Value out = row ? Value::Array(static_cast<uint32_t>(count), 1)
                  : Value::Array(1, static_cast<uint32_t>(count));
  for (int i = 0; i < count; ++i) {
    if (row) {
      out.mut(static_cast<uint32_t>(i), 0) = Value::Number(first + i + 1);
    } else {
      out.mut(0, static_cast<uint32_t>(i)) = Value::Number(first + i + 1);
    }
  }
  return Operand::Of(std::move(out));
}
Operand FnRow(EvalContext& ctx, Operand* a, int n) {
  return Position(ctx, a, n, true);
}
Operand FnColumn(EvalContext& ctx, Operand* a, int n) {
  return Position(ctx, a, n, false);
}

Operand Extent(EvalContext&, Operand* a, int, bool rows) {
  const Operand& o = a[0];
  if (o.IsError()) return MakeErr(o.value.error());
  if (o.tag == Operand::Tag::Ref) return MakeNum(rows ? o.area.rows() : o.area.cols());
  if (o.tag == Operand::Tag::Refs) return MakeErr(Err::Ref);
  if (o.IsMissing()) return MakeErr(Err::Value);
  return MakeNum(rows ? o.value.rows() : o.value.cols());
}
Operand FnRows(EvalContext& ctx, Operand* a, int n) {
  return Extent(ctx, a, n, true);
}
Operand FnColumns(EvalContext& ctx, Operand* a, int n) {
  return Extent(ctx, a, n, false);
}

Operand FnAreas(EvalContext&, Operand* a, int) {
  if (a[0].tag == Operand::Tag::Ref) return MakeNum(1);
  if (a[0].tag == Operand::Tag::Refs) return MakeNum(static_cast<double>(a[0].list->size()));
  if (a[0].IsError()) return MakeErr(a[0].value.error());
  return MakeErr(Err::Value);
}

Operand FnAddress(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, r0);
  NUM_ARG(1, c0);
  double abs0;
  if (Err e = ArgNumberOr(ctx, a, n, 2, 1, abs0); e != Err::None) return MakeErr(e);
  bool a1;
  if (Err e = ArgBoolOr(ctx, a, n, 3, true, a1); e != Err::None) return MakeErr(e);
  int r = ToInt(r0);
  int c = ToInt(c0);
  int ab = ToInt(abs0);
  if (ab < 1 || ab > 4) return MakeErr(Err::Value);
  if (a1 && (r < 1 || c < 1 || r > kMaxRows || c > kMaxCols)) return MakeErr(Err::Value);
  bool row_abs = ab == 1 || ab == 2;
  bool col_abs = ab == 1 || ab == 3;
  std::string out;
  if (n > 4 && !a[4].IsMissing()) {
    std::string sheet;
    if (Err e = ArgText(ctx, a[4], sheet); e != Err::None) return MakeErr(e);
    if (!sheet.empty()) out = QuoteSheetName(sheet) + "!";
  }
  if (a1) {
    if (col_abs) out += "$";
    out += ColumnName(c - 1);
    if (row_abs) out += "$";
    out += std::to_string(r);
  } else {
    out += row_abs ? "R" + std::to_string(r) : "R[" + std::to_string(r) + "]";
    out += col_abs ? "C" + std::to_string(c) : "C[" + std::to_string(c) + "]";
  }
  return MakeText(out);
}

Operand FnTranspose(EvalContext& ctx, Operand* a, int) {
  Grid g;
  if (a[0].IsError()) return MakeErr(a[0].value.error());
  if (!MakeGrid(ctx, a[0], g, false)) return MakeErr(Err::Value);
  if (static_cast<long long>(g.rows) * g.cols > 4000000) return MakeErr(Err::Num);
  Value out = Value::Array(static_cast<uint32_t>(g.cols), static_cast<uint32_t>(g.rows));
  for (int r = 0; r < g.rows; ++r) {
    for (int c = 0; c < g.cols; ++c)
      out.mut(static_cast<uint32_t>(c), static_cast<uint32_t>(r)) = g.At(r, c);
  }
  return Operand::Of(std::move(out));
}

Operand FnFormulaText(EvalContext& ctx, Operand* a, int) {
  if (a[0].IsError()) return MakeErr(a[0].value.error());
  if (a[0].tag != Operand::Tag::Ref) return MakeErr(Err::NA);
  const Area& ar = a[0].area;
  if (!ctx.CellHasFormula(ar.sheet, ar.r1, ar.c1)) return MakeErr(Err::NA);
  return MakeText(ctx.FormulaTextAt(ar.sheet, ar.r1, ar.c1));
}

Operand FnSingle(EvalContext& ctx, Operand* a, int) {
  return Operand::Of(ctx.Scalar(a[0]));
}

Operand FnSequence(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, r0);
  double c0;
  double s0;
  double st0;
  if (Err e = ArgNumberOr(ctx, a, n, 1, 1, c0); e != Err::None) return MakeErr(e);
  if (Err e = ArgNumberOr(ctx, a, n, 2, 1, s0); e != Err::None) return MakeErr(e);
  if (Err e = ArgNumberOr(ctx, a, n, 3, 1, st0); e != Err::None) return MakeErr(e);
  long long rows = ToInt64(r0);
  long long cols = ToInt64(c0);
  if (rows < 1 || cols < 1) return MakeErr(Err::Calc);
  if (rows > kMaxRows || cols > kMaxCols || rows * cols > 4000000) return MakeErr(Err::Num);
  Value out = Value::Array(static_cast<uint32_t>(rows), static_cast<uint32_t>(cols));
  for (long long r = 0; r < rows; ++r) {
    for (long long c = 0; c < cols; ++c) {
      out.mut(static_cast<uint32_t>(r), static_cast<uint32_t>(c)) =
          Value::Number(s0 + st0 * static_cast<double>(r * cols + c));
    }
  }
  return Operand::Of(std::move(out));
}

Operand FnIsBlank(EvalContext& ctx, Operand* a, int) {
  return MakeBool(ctx.Scalar(a[0]).IsBlank());
}
Operand FnIsErr(EvalContext& ctx, Operand* a, int) {
  Value v = ctx.Scalar(a[0]);
  return MakeBool(v.IsError() && v.error() != Err::NA);
}
Operand FnIsError(EvalContext& ctx, Operand* a, int) {
  return MakeBool(ctx.Scalar(a[0]).IsError());
}
Operand FnIsNa(EvalContext& ctx, Operand* a, int) {
  Value v = ctx.Scalar(a[0]);
  return MakeBool(v.IsError() && v.error() == Err::NA);
}
Operand FnIsText(EvalContext& ctx, Operand* a, int) {
  return MakeBool(ctx.Scalar(a[0]).IsText());
}
Operand FnIsNonText(EvalContext& ctx, Operand* a, int) {
  return MakeBool(!ctx.Scalar(a[0]).IsText());
}
Operand FnIsNumber(EvalContext& ctx, Operand* a, int) {
  return MakeBool(ctx.Scalar(a[0]).IsNumber());
}
Operand FnIsLogical(EvalContext& ctx, Operand* a, int) {
  return MakeBool(ctx.Scalar(a[0]).IsBool());
}
Operand FnIsRef(EvalContext&, Operand* a, int) {
  return MakeBool(a[0].IsRef());
}
Operand Parity(EvalContext& ctx, Operand* a, bool even) {
  Value v = ctx.Scalar(a[0]);
  if (v.IsError()) return Operand::Of(v);
  if (v.IsBool()) return MakeErr(Err::Value);
  double x;
  if (Err e = ToNumber(v, ctx, x); e != Err::None) return MakeErr(e);
  double t = std::trunc(x);
  bool is_even = std::fmod(std::fabs(t), 2.0) == 0;
  return MakeBool(even ? is_even : !is_even);
}
Operand FnIsEven(EvalContext& ctx, Operand* a, int) {
  return Parity(ctx, a, true);
}
Operand FnIsOdd(EvalContext& ctx, Operand* a, int) {
  return Parity(ctx, a, false);
}
Operand FnIsFormula(EvalContext& ctx, Operand* a, int) {
  if (a[0].IsError()) return MakeErr(a[0].value.error());
  if (a[0].tag != Operand::Tag::Ref) return MakeErr(Err::Value);
  return MakeBool(ctx.CellHasFormula(a[0].area.sheet, a[0].area.r1, a[0].area.c1));
}
Operand FnN(EvalContext& ctx, Operand* a, int) {
  Value v = ctx.Scalar(a[0]);
  if (v.IsError()) return Operand::Of(v);
  if (v.IsNumber()) return Operand::Of(v);
  if (v.IsBool()) return MakeNum(v.boolean() ? 1 : 0);
  return MakeNum(0);
}
Operand FnNa(EvalContext&, Operand*, int) {
  return MakeErr(Err::NA);
}
Operand FnErrorType(EvalContext& ctx, Operand* a, int) {
  Value v = ctx.Scalar(a[0]);
  if (!v.IsError()) return MakeErr(Err::NA);
  return MakeNum(static_cast<int>(v.error()));
}
Operand FnType(EvalContext& ctx, Operand* a, int) {
  const Operand& o = a[0];
  if (o.tag == Operand::Tag::Refs) return MakeNum(16);
  if (o.tag == Operand::Tag::Ref && (o.area.rows() > 1 || o.area.cols() > 1)) return MakeNum(16);
  if (o.IsArray()) return MakeNum(64);
  Value v = ctx.Scalar(o);
  switch (v.kind()) {
    case Kind::Text:
      return MakeNum(2);
    case Kind::Bool:
      return MakeNum(4);
    case Kind::Error:
      return MakeNum(16);
    default:
      return MakeNum(1);
  }
}
Operand FnSheet(EvalContext& ctx, Operand* a, int n) {
  if (n == 0 || a[0].IsMissing()) return MakeNum(ctx.sheet + 1);
  if (a[0].tag == Operand::Tag::Ref) return MakeNum(a[0].area.sheet + 1);
  std::string name;
  if (Err e = ArgText(ctx, a[0], name); e != Err::None) return MakeErr(e);
  int s = ctx.wb().FindSheet(name);
  if (s < 0) return MakeErr(Err::NA);
  return MakeNum(s + 1);
}
Operand FnSheets(EvalContext& ctx, Operand* a, int n) {
  if (n == 0 || a[0].IsMissing()) return MakeNum(ctx.wb().SheetCount());
  if (a[0].IsRef()) return MakeNum(1);
  return MakeErr(Err::Value);
}
Operand FnCell(EvalContext& ctx, Operand* a, int n) {
  std::string info;
  if (Err e = ArgText(ctx, a[0], info); e != Err::None) return MakeErr(e);
  Area ar{ctx.sheet, ctx.row, ctx.col, ctx.row, ctx.col};
  if (n > 1 && !a[1].IsMissing()) {
    if (a[1].tag != Operand::Tag::Ref) return MakeErr(Err::Value);
    ar = a[1].area;
  }
  std::string k = ToLower(info);
  if (k == "row") return MakeNum(ar.r1 + 1);
  if (k == "col") return MakeNum(ar.c1 + 1);
  if (k == "address") return MakeText("$" + ColumnName(ar.c1) + "$" + std::to_string(ar.r1 + 1));
  if (k == "contents") return Operand::Of(ctx.Read(ar.sheet, ar.r1, ar.c1));
  if (k == "type") {
    const Value& v = ctx.Read(ar.sheet, ar.r1, ar.c1);
    return MakeText(v.IsBlank() ? "b" : (v.IsText() ? "l" : "v"));
  }
  return MakeErr(Err::Value);
}

}

void RegisterLookup(std::vector<FuncInfo>& out) {
  const FuncInfo list[] = {
      {"AND", 1, 255, "R", 1, 0, FnAnd},
      {"OR", 1, 255, "R", 1, 0, FnOr},
      {"XOR", 1, 255, "R", 1, kFnXlfn, FnXor},
      {"NOT", 1, 1, "V", 1, kFnLift, FnNot},
      {"TRUE", 0, 0, "", 1, 0, FnTrue},
      {"FALSE", 0, 0, "", 1, 0, FnFalse},
      {"IFS", 2, 254, "VP", 2, kFnXlfn, FnIfs},
      {"SWITCH", 3, 254, "VVP", 2, kFnXlfn, FnSwitch},
      {"CHOOSE", 2, 255, "VP", 1, kFnLift, FnChoose},
      {"MATCH", 2, 3, "VAV", 1, kFnLift, FnMatch},
      {"VLOOKUP", 3, 4, "VAVV", 1, kFnLift, FnVlookup},
      {"HLOOKUP", 3, 4, "VAVV", 1, kFnLift, FnHlookup},
      {"LOOKUP", 2, 3, "VAA", 1, kFnLift, FnLookup},
      {"XLOOKUP", 3, 6, "VAAPVV", 1, kFnXlfn | kFnLift, FnXlookup},
      {"XMATCH", 2, 4, "VAVV", 1, kFnXlfn | kFnLift, FnXmatch},
      {"INDEX", 2, 4, "AVVV", 1, kFnLift, FnIndex},
      {"OFFSET", 3, 5, "rVVVV", 1, kFnDynamic | kFnVolatile, FnOffset},
      {"INDIRECT", 1, 2, "VV", 1, kFnDynamic | kFnVolatile, FnIndirect},
      {"ROW", 0, 1, "r", 1, 0, FnRow},
      {"COLUMN", 0, 1, "r", 1, 0, FnColumn},
      {"ROWS", 1, 1, "R", 1, 0, FnRows},
      {"COLUMNS", 1, 1, "R", 1, 0, FnColumns},
      {"AREAS", 1, 1, "r", 1, 0, FnAreas},
      {"ADDRESS", 2, 5, "VVVVV", 1, kFnLift, FnAddress},
      {"TRANSPOSE", 1, 1, "A", 1, 0, FnTranspose},
      {"FORMULATEXT", 1, 1, "r", 1, kFnXlfn, FnFormulaText},
      {"SINGLE", 1, 1, "P", 1, kFnXlfn, FnSingle},
      {"SEQUENCE", 1, 4, "VVVV", 1, kFnXlfn, FnSequence},
  };
  out.insert(out.end(), std::begin(list), std::end(list));
}

void RegisterInfo(std::vector<FuncInfo>& out) {
  const FuncInfo list[] = {
      {"ISBLANK", 1, 1, "V", 1, kFnLift, FnIsBlank},
      {"ISERR", 1, 1, "V", 1, kFnLift, FnIsErr},
      {"ISERROR", 1, 1, "V", 1, kFnLift, FnIsError},
      {"ISNA", 1, 1, "V", 1, kFnLift, FnIsNa},
      {"ISTEXT", 1, 1, "V", 1, kFnLift, FnIsText},
      {"ISNONTEXT", 1, 1, "V", 1, kFnLift, FnIsNonText},
      {"ISNUMBER", 1, 1, "V", 1, kFnLift, FnIsNumber},
      {"ISLOGICAL", 1, 1, "V", 1, kFnLift, FnIsLogical},
      {"ISREF", 1, 1, "P", 1, 0, FnIsRef},
      {"ISEVEN", 1, 1, "V", 1, kFnLift, FnIsEven},
      {"ISODD", 1, 1, "V", 1, kFnLift, FnIsOdd},
      {"ISFORMULA", 1, 1, "r", 1, kFnXlfn, FnIsFormula},
      {"N", 1, 1, "V", 1, kFnLift, FnN},
      {"NA", 0, 0, "", 1, 0, FnNa},
      {"ERROR.TYPE", 1, 1, "V", 1, kFnLift, FnErrorType},
      {"TYPE", 1, 1, "P", 1, 0, FnType},
      {"SHEET", 0, 1, "P", 1, kFnXlfn, FnSheet},
      {"SHEETS", 0, 1, "P", 1, kFnXlfn, FnSheets},
      {"CELL", 1, 2, "Vr", 1, kFnVolatile, FnCell},
  };
  out.insert(out.end(), std::begin(list), std::end(list));
}

}
