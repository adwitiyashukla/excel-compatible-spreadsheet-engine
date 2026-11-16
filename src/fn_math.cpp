#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "eval.h"

namespace sheet {

namespace {

constexpr double kPi = 3.14159265358979323846;

#define NUM_ARG(i, var) \
  double var;           \
  if (Err e_ = ArgNumber(ctx, a[i], var); e_ != Err::None) return MakeErr(e_)

using Fn1 = double (*)(double, Err&);

Operand Apply1(EvalContext& ctx, Operand* a, Fn1 fn) {
  NUM_ARG(0, x);
  Err e = Err::None;
  double r = fn(x, e);
  if (e != Err::None) return MakeErr(e);
  return MakeNum(r);
}

double ApproxFloor(double x) {
  return std::floor(Round15(x));
}
double ApproxCeil(double x) {
  return std::ceil(Round15(x));
}

double Trig(double x, Err& e, double (*f)(double)) {
  if (std::fabs(x) >= 134217728.0) {
    e = Err::Num;
    return 0;
  }
  return f(x);
}

Operand FnAbs(EvalContext& ctx, Operand* a, int) {
  return Apply1(ctx, a, [](double x, Err&) { return std::fabs(x); });
}
Operand FnSign(EvalContext& ctx, Operand* a, int) {
  return Apply1(ctx, a, [](double x, Err&) { return x > 0 ? 1.0 : (x < 0 ? -1.0 : 0.0); });
}
Operand FnSqrt(EvalContext& ctx, Operand* a, int) {
  return Apply1(ctx, a, [](double x, Err& e) {
    if (x < 0) e = Err::Num;
    return std::sqrt(std::fabs(x));
  });
}
Operand FnSqrtPi(EvalContext& ctx, Operand* a, int) {
  return Apply1(ctx, a, [](double x, Err& e) {
    if (x < 0) e = Err::Num;
    return std::sqrt(std::fabs(x) * kPi);
  });
}
Operand FnExp(EvalContext& ctx, Operand* a, int) {
  return Apply1(ctx, a, [](double x, Err&) { return std::exp(x); });
}
Operand FnLn(EvalContext& ctx, Operand* a, int) {
  return Apply1(ctx, a, [](double x, Err& e) {
    if (x <= 0) {
      e = Err::Num;
      return 0.0;
    }
    return std::log(x);
  });
}
Operand FnLog10(EvalContext& ctx, Operand* a, int) {
  return Apply1(ctx, a, [](double x, Err& e) {
    if (x <= 0) {
      e = Err::Num;
      return 0.0;
    }
    return std::log10(x);
  });
}
Operand FnLog(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, x);
  double base;
  if (Err e = ArgNumberOr(ctx, a, n, 1, 10, base); e != Err::None) return MakeErr(e);
  if (x <= 0 || base <= 0) return MakeErr(Err::Num);
  if (base == 1) return MakeErr(Err::Div0);
  if (base == 10) return MakeNum(std::log10(x));
  return MakeNum(std::log(x) / std::log(base));
}
Operand FnPower(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  NUM_ARG(1, y);
  if (x == 0 && y == 0) return MakeErr(Err::Num);
  if (x == 0 && y < 0) return MakeErr(Err::Div0);
  if (x < 0 && y != std::floor(y)) {
    double inv = 1.0 / y;
    if (inv == std::floor(inv) && std::fmod(std::fabs(inv), 2.0) == 1.0) {
      return MakeNum(-std::pow(-x, y));
    }
    return MakeErr(Err::Num);
  }
  return MakeNum(std::pow(x, y));
}
Operand FnPi(EvalContext&, Operand*, int) {
  return MakeNum(kPi);
}

Operand FnSin(EvalContext& ctx, Operand* a, int) {
  return Apply1(ctx, a,
                [](double x, Err& e) { return Trig(x, e, [](double v) { return std::sin(v); }); });
}
Operand FnCos(EvalContext& ctx, Operand* a, int) {
  return Apply1(ctx, a,
                [](double x, Err& e) { return Trig(x, e, [](double v) { return std::cos(v); }); });
}
Operand FnTan(EvalContext& ctx, Operand* a, int) {
  return Apply1(ctx, a,
                [](double x, Err& e) { return Trig(x, e, [](double v) { return std::tan(v); }); });
}
Operand FnCot(EvalContext& ctx, Operand* a, int) {
  return Apply1(ctx, a, [](double x, Err& e) {
    if (x == 0) {
      e = Err::Div0;
      return 0.0;
    }
    return Trig(x, e, [](double v) { return 1.0 / std::tan(v); });
  });
}
Operand FnCsc(EvalContext& ctx, Operand* a, int) {
  return Apply1(ctx, a, [](double x, Err& e) {
    if (x == 0) {
      e = Err::Div0;
      return 0.0;
    }
    return Trig(x, e, [](double v) { return 1.0 / std::sin(v); });
  });
}
Operand FnSec(EvalContext& ctx, Operand* a, int) {
  return Apply1(ctx, a, [](double x, Err& e) {
    return Trig(x, e, [](double v) { return 1.0 / std::cos(v); });
  });
}
Operand FnAsin(EvalContext& ctx, Operand* a, int) {
  return Apply1(ctx, a, [](double x, Err& e) {
    if (x < -1 || x > 1) e = Err::Num;
    return std::asin(std::max(-1.0, std::min(1.0, x)));
  });
}
Operand FnAcos(EvalContext& ctx, Operand* a, int) {
  return Apply1(ctx, a, [](double x, Err& e) {
    if (x < -1 || x > 1) e = Err::Num;
    return std::acos(std::max(-1.0, std::min(1.0, x)));
  });
}
Operand FnAtan(EvalContext& ctx, Operand* a, int) {
  return Apply1(ctx, a, [](double x, Err&) { return std::atan(x); });
}
Operand FnAcot(EvalContext& ctx, Operand* a, int) {
  return Apply1(ctx, a, [](double x, Err&) { return kPi / 2 - std::atan(x); });
}
Operand FnAtan2(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  NUM_ARG(1, y);
  if (x == 0 && y == 0) return MakeErr(Err::Div0);
  return MakeNum(std::atan2(y, x));
}
Operand FnSinh(EvalContext& ctx, Operand* a, int) {
  return Apply1(ctx, a, [](double x, Err&) { return std::sinh(x); });
}
Operand FnCosh(EvalContext& ctx, Operand* a, int) {
  return Apply1(ctx, a, [](double x, Err&) { return std::cosh(x); });
}
Operand FnTanh(EvalContext& ctx, Operand* a, int) {
  return Apply1(ctx, a, [](double x, Err&) { return std::tanh(x); });
}
Operand FnCoth(EvalContext& ctx, Operand* a, int) {
  return Apply1(ctx, a, [](double x, Err& e) {
    if (x == 0) {
      e = Err::Div0;
      return 0.0;
    }
    return 1.0 / std::tanh(x);
  });
}
Operand FnCsch(EvalContext& ctx, Operand* a, int) {
  return Apply1(ctx, a, [](double x, Err& e) {
    if (x == 0) {
      e = Err::Div0;
      return 0.0;
    }
    return 1.0 / std::sinh(x);
  });
}
Operand FnSech(EvalContext& ctx, Operand* a, int) {
  return Apply1(ctx, a, [](double x, Err&) { return 1.0 / std::cosh(x); });
}
Operand FnAsinh(EvalContext& ctx, Operand* a, int) {
  return Apply1(ctx, a, [](double x, Err&) { return std::asinh(x); });
}
Operand FnAcosh(EvalContext& ctx, Operand* a, int) {
  return Apply1(ctx, a, [](double x, Err& e) {
    if (x < 1) e = Err::Num;
    return std::acosh(std::max(1.0, x));
  });
}
Operand FnAtanh(EvalContext& ctx, Operand* a, int) {
  return Apply1(ctx, a, [](double x, Err& e) {
    if (x <= -1 || x >= 1) {
      e = Err::Num;
      return 0.0;
    }
    return std::atanh(x);
  });
}
Operand FnAcoth(EvalContext& ctx, Operand* a, int) {
  return Apply1(ctx, a, [](double x, Err& e) {
    if (x >= -1 && x <= 1) {
      e = Err::Num;
      return 0.0;
    }
    return 0.5 * std::log((x + 1) / (x - 1));
  });
}
Operand FnDegrees(EvalContext& ctx, Operand* a, int) {
  return Apply1(ctx, a, [](double x, Err&) { return x * 180.0 / kPi; });
}
Operand FnRadians(EvalContext& ctx, Operand* a, int) {
  return Apply1(ctx, a, [](double x, Err&) { return x * kPi / 180.0; });
}

Operand FnRound(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  NUM_ARG(1, d);
  return MakeNum(RoundDigits(x, ToInt(d)));
}

double RoundDir(double x, int digits, bool up) {
  if (x == 0) return 0;
  bool neg = x < 0;
  double ax = std::fabs(x);
  double r;
  if (digits >= 0) {
    double scale = std::pow(10.0, std::min(digits, 308));
    double y = ax * scale;
    if (!std::isfinite(y) || y >= 9007199254740992.0) return x;
    double q = Round15(y);
    r = (up ? std::ceil(q) : std::floor(q)) / scale;
  } else {
    double scale = std::pow(10.0, std::min(-digits, 308));
    double q = Round15(ax / scale);
    r = (up ? std::ceil(q) : std::floor(q)) * scale;
  }
  return neg ? -r : r;
}

Operand FnRoundUp(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  NUM_ARG(1, d);
  return MakeNum(RoundDir(x, ToInt(d), true));
}
Operand FnRoundDown(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  NUM_ARG(1, d);
  return MakeNum(RoundDir(x, ToInt(d), false));
}
Operand FnTrunc(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, x);
  double d;
  if (Err e = ArgNumberOr(ctx, a, n, 1, 0, d); e != Err::None) return MakeErr(e);
  return MakeNum(RoundDir(x, ToInt(d), false));
}
Operand FnInt(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  return MakeNum(ApproxFloor(x));
}
Operand FnEven(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  double ax = ApproxCeil(std::fabs(x));
  if (std::fmod(ax, 2.0) != 0) ax += 1;
  return MakeNum(x < 0 ? -ax : ax);
}
Operand FnOdd(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  double ax = ApproxCeil(std::fabs(x));
  if (std::fmod(ax, 2.0) == 0) ax += 1;
  return MakeNum(x < 0 ? -ax : ax);
}
Operand FnMround(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  NUM_ARG(1, m);
  if (m == 0 || x == 0) return MakeNum(0);
  if ((x > 0) != (m > 0)) return MakeErr(Err::Num);
  double q = Round15(x / m);
  double r = std::floor(std::fabs(q) + 0.5);
  return MakeNum(r * m * (q < 0 ? -1 : 1));
}

Operand FnCeiling(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, x);
  double sig;
  if (Err e = ArgNumberOr(ctx, a, n, 1, x < 0 ? -1 : 1, sig); e != Err::None) return MakeErr(e);
  if (x == 0 || sig == 0) return MakeNum(0);
  if (x > 0 && sig < 0) return MakeErr(Err::Num);
  return MakeNum(ApproxCeil(x / sig) * sig);
}
Operand FnFloor(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, x);
  double sig;
  if (Err e = ArgNumberOr(ctx, a, n, 1, x < 0 ? -1 : 1, sig); e != Err::None) return MakeErr(e);
  if (x == 0) return MakeNum(0);
  if (sig == 0) return MakeErr(Err::Div0);
  if (x > 0 && sig < 0) return MakeErr(Err::Num);
  return MakeNum(ApproxFloor(x / sig) * sig);
}
Operand FnCeilingMath(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, x);
  double sig;
  double mode;
  if (Err e = ArgNumberOr(ctx, a, n, 1, 1, sig); e != Err::None) return MakeErr(e);
  if (Err e = ArgNumberOr(ctx, a, n, 2, 0, mode); e != Err::None) return MakeErr(e);
  sig = std::fabs(sig);
  if (sig == 0 || x == 0) return MakeNum(0);
  if (x < 0 && mode != 0) return MakeNum(-ApproxCeil(-x / sig) * sig);
  return MakeNum(ApproxCeil(x / sig) * sig);
}
Operand FnFloorMath(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, x);
  double sig;
  double mode;
  if (Err e = ArgNumberOr(ctx, a, n, 1, 1, sig); e != Err::None) return MakeErr(e);
  if (Err e = ArgNumberOr(ctx, a, n, 2, 0, mode); e != Err::None) return MakeErr(e);
  sig = std::fabs(sig);
  if (sig == 0 || x == 0) return MakeNum(0);
  if (x < 0 && mode != 0) return MakeNum(-ApproxFloor(-x / sig) * sig);
  return MakeNum(ApproxFloor(x / sig) * sig);
}
Operand FnCeilingPrecise(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, x);
  double sig;
  if (Err e = ArgNumberOr(ctx, a, n, 1, 1, sig); e != Err::None) return MakeErr(e);
  sig = std::fabs(sig);
  if (sig == 0 || x == 0) return MakeNum(0);
  return MakeNum(ApproxCeil(x / sig) * sig);
}
Operand FnFloorPrecise(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, x);
  double sig;
  if (Err e = ArgNumberOr(ctx, a, n, 1, 1, sig); e != Err::None) return MakeErr(e);
  sig = std::fabs(sig);
  if (sig == 0 || x == 0) return MakeNum(0);
  return MakeNum(ApproxFloor(x / sig) * sig);
}

Operand FnMod(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  NUM_ARG(1, d);
  if (d == 0) return MakeErr(Err::Div0);
  double q = x / d;
  if (std::fabs(q) > 4503599627370496.0) return MakeErr(Err::Num);
  double r = ApproxSub(x, ApproxFloor(q) * d);
  if (r != 0 && ((r < 0) != (d < 0))) r += d;
  if (ApproxEqual(r, d)) r = 0;
  return MakeNum(r);
}
Operand FnQuotient(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  NUM_ARG(1, d);
  if (d == 0) return MakeErr(Err::Div0);
  return MakeNum(std::trunc(Round15(x / d)));
}

Operand FnFact(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  if (x < 0) return MakeErr(Err::Num);
  double n = std::floor(x);
  if (n > 170) return MakeErr(Err::Num);
  double r = 1;
  for (int i = 2; i <= static_cast<int>(n); ++i) r *= i;
  return MakeNum(r);
}
Operand FnFactDouble(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  double n = std::floor(x);
  if (n < -1) return MakeErr(Err::Num);
  if (n > 300) return MakeErr(Err::Num);
  double r = 1;
  for (int i = static_cast<int>(n); i > 1; i -= 2) r *= i;
  return MakeNum(r);
}

double Combin(double n, double k) {
  if (k > n - k) k = n - k;
  if (k > 1030) return HUGE_VAL;
  double r = 1;
  for (int i = 1; i <= static_cast<int>(k) && std::isfinite(r); ++i) r = r * (n - k + i) / i;
  return std::floor(r + 0.5);
}

Operand FnCombin(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, nn);
  NUM_ARG(1, kk);
  double n = std::trunc(nn);
  double k = std::trunc(kk);
  if (n < 0 || k < 0 || k > n) return MakeErr(Err::Num);
  return MakeNum(Combin(n, k));
}
Operand FnCombinA(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, nn);
  NUM_ARG(1, kk);
  double n = std::trunc(nn);
  double k = std::trunc(kk);
  if (n < 0 || k < 0 || (n == 0 && k > 0)) return MakeErr(Err::Num);
  if (k == 0) return MakeNum(1);
  return MakeNum(Combin(n + k - 1, k));
}
Operand FnMultinomial(EvalContext& ctx, Operand* a, int n) {
  std::vector<double> xs;
  if (Err e = CollectNumbers(ctx, a, n, xs); e != Err::None) return MakeErr(e);
  double sum = 0;
  double r = 1;
  for (double x0 : xs) {
    double x = std::trunc(x0);
    if (x < 0) return MakeErr(Err::Num);
    sum += x;
    r *= Combin(sum, x);
    if (!std::isfinite(r)) return MakeErr(Err::Num);
  }
  return MakeNum(std::floor(r + 0.5));
}

double Gcd(double a, double b) {
  while (b > 0.5) {
    double t = std::fmod(a, b);
    a = b;
    b = t;
  }
  return a;
}

Operand FnGcd(EvalContext& ctx, Operand* a, int n) {
  std::vector<double> xs;
  if (Err e = CollectNumbers(ctx, a, n, xs); e != Err::None) return MakeErr(e);
  double g = 0;
  for (double x0 : xs) {
    double x = std::trunc(x0);
    if (x < 0 || x >= 9007199254740992.0) return MakeErr(Err::Num);
    g = Gcd(std::max(g, x), std::min(g, x));
  }
  return MakeNum(g);
}
Operand FnLcm(EvalContext& ctx, Operand* a, int n) {
  std::vector<double> xs;
  if (Err e = CollectNumbers(ctx, a, n, xs); e != Err::None) return MakeErr(e);
  double l = 1;
  for (double x0 : xs) {
    double x = std::trunc(x0);
    if (x < 0 || x >= 9007199254740992.0) return MakeErr(Err::Num);
    if (x == 0) return MakeNum(0);
    l = l / Gcd(std::max(l, x), std::min(l, x)) * x;
  }
  return MakeNum(l);
}

double PlainSum(const std::vector<double>& xs) {
  double sum = 0;
  for (double x : xs) sum += x;
  return sum;
}

Operand FnSum(EvalContext& ctx, Operand* a, int n) {
  if (n == 1 && a[0].tag == Operand::Tag::Ref && a[0].area.cols() == 1) {
    double sum = 0;
    Err bad = Err::None;
    ctx.Scan(a[0].area, [&](int, int, const Value& v) {
      if (v.IsNumber()) {
        sum += v.number();
      } else if (v.IsError() && bad == Err::None) {
        bad = v.error();
      }
    });
    if (bad != Err::None) return MakeErr(bad);
    return MakeNum(sum);
  }
  std::vector<double> xs;
  if (Err e = CollectNumbers(ctx, a, n, xs); e != Err::None) return MakeErr(e);
  return MakeNum(PlainSum(xs));
}
Operand FnProduct(EvalContext& ctx, Operand* a, int n) {
  std::vector<double> xs;
  if (Err e = CollectNumbers(ctx, a, n, xs); e != Err::None) return MakeErr(e);
  if (xs.empty()) return MakeNum(0);
  double r = 1;
  for (double x : xs) r *= x;
  return MakeNum(r);
}
Operand FnSumSq(EvalContext& ctx, Operand* a, int n) {
  std::vector<double> xs;
  if (Err e = CollectNumbers(ctx, a, n, xs); e != Err::None) return MakeErr(e);
  for (double& x : xs) x *= x;
  return MakeNum(PlainSum(xs));
}

Operand FnSumProduct(EvalContext& ctx, Operand* a, int n) {
  std::vector<Grid> grids(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    if (a[i].IsError()) return MakeErr(a[i].value.error());
    if (!MakeGrid(ctx, a[i], grids[static_cast<size_t>(i)], false)) return MakeErr(Err::Value);
  }
  int rows = grids[0].rows;
  int cols = grids[0].cols;
  for (const Grid& g : grids) {
    if (g.rows != rows || g.cols != cols) return MakeErr(Err::Value);
  }
  if (rows == 1 && cols == 1) {
    double p = 1;
    for (const Grid& g : grids) {
      const Value& v = g.At(0, 0);
      if (v.IsError()) return MakeErr(v.error());
      if (v.IsBlank() || v.IsText()) return MakeErr(Err::Value);
      p *= v.IsBool() ? (v.boolean() ? 1.0 : 0.0) : v.number();
    }
    return MakeNum(p);
  }
  int r2 = rows;
  bool all_refs = true;
  for (const Grid& g : grids) all_refs = all_refs && g.kind == 0;
  if (all_refs) {
    for (const Grid& g : grids)
      r2 = std::min(r2, std::max(0, ctx.UsedRows(g.area.sheet) - g.area.r1));
  }
  double sum = 0;
  for (int r = 0; r < r2; ++r) {
    for (int c = 0; c < cols; ++c) {
      double p = 1;
      for (const Grid& g : grids) {
        const Value& v = g.At(r, c);
        if (v.IsError()) return MakeErr(v.error());
        p *= v.IsNumber() ? v.number() : 0.0;
      }
      sum += p;
    }
  }
  return MakeNum(sum);
}

Operand SumPairs(EvalContext& ctx, Operand* a, int mode) {
  Grid x;
  Grid y;
  if (!MakeGrid(ctx, a[0], x, false) || !MakeGrid(ctx, a[1], y, false)) return MakeErr(Err::Value);
  if (x.rows * x.cols != y.rows * y.cols) return MakeErr(Err::NA);
  std::vector<double> terms;
  int total = x.rows * x.cols;
  size_t pairs = 0;
  for (int i = 0; i < total; ++i) {
    const Value& u = x.At(i / x.cols, i % x.cols);
    const Value& v = y.At(i / y.cols, i % y.cols);
    if (u.IsError()) return MakeErr(u.error());
    if (v.IsError()) return MakeErr(v.error());
    if (!u.IsNumber() || !v.IsNumber()) continue;
    ++pairs;
    double p = u.number();
    double q = v.number();
    if (mode == 0) terms.push_back(p * p - q * q);
    if (mode == 1) terms.push_back(p * p + q * q);
    if (mode == 2) terms.push_back((p - q) * (p - q));
  }
  if (pairs == 0) return MakeErr(Err::Div0);
  return MakeNum(PlainSum(terms));
}
Operand FnSumX2MY2(EvalContext& ctx, Operand* a, int) {
  return SumPairs(ctx, a, 0);
}
Operand FnSumX2PY2(EvalContext& ctx, Operand* a, int) {
  return SumPairs(ctx, a, 1);
}
Operand FnSumXMY2(EvalContext& ctx, Operand* a, int) {
  return SumPairs(ctx, a, 2);
}

Operand FnRand(EvalContext& ctx, Operand*, int) {
  return MakeNum(ctx.Random(1));
}
Operand FnRandBetween(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, lo0);
  NUM_ARG(1, hi0);
  double lo = std::ceil(lo0);
  double hi = std::floor(hi0);
  if (lo > hi) return MakeErr(Err::Num);
  return MakeNum(lo + std::floor(ctx.Random(2) * (hi - lo + 1)));
}

bool NumericMatrix(EvalContext& ctx, const Operand& op, std::vector<double>& m, int& rows,
                   int& cols) {
  Grid g;
  if (!MakeGrid(ctx, op, g, false)) return false;
  rows = g.rows;
  cols = g.cols;
  if (static_cast<long long>(rows) * cols > 1000000) return false;
  m.resize(static_cast<size_t>(rows) * static_cast<size_t>(cols));
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      const Value& v = g.At(r, c);
      if (!v.IsNumber()) return false;
      m[static_cast<size_t>(r) * static_cast<size_t>(cols) + static_cast<size_t>(c)] = v.number();
    }
  }
  return true;
}

Operand FnMmult(EvalContext& ctx, Operand* a, int) {
  std::vector<double> x;
  std::vector<double> y;
  int xr;
  int xc;
  int yr;
  int yc;
  if (!NumericMatrix(ctx, a[0], x, xr, xc) || !NumericMatrix(ctx, a[1], y, yr, yc)) {
    return MakeErr(Err::Value);
  }
  if (xc != yr) return MakeErr(Err::Value);
  Value out = Value::Array(static_cast<uint32_t>(xr), static_cast<uint32_t>(yc));
  for (int i = 0; i < xr; ++i) {
    for (int j = 0; j < yc; ++j) {
      double s = 0;
      for (int k = 0; k < xc; ++k)
        s += x[static_cast<size_t>(i * xc + k)] * y[static_cast<size_t>(k * yc + j)];
      out.mut(static_cast<uint32_t>(i), static_cast<uint32_t>(j)) = Value::Number(s);
    }
  }
  return Operand::Of(std::move(out));
}

Operand FnMdeterm(EvalContext& ctx, Operand* a, int) {
  std::vector<double> m;
  int r;
  int c;
  if (!NumericMatrix(ctx, a[0], m, r, c)) return MakeErr(Err::Value);
  if (r != c) return MakeErr(Err::Value);
  int n = r;
  double det = 1;
  for (int k = 0; k < n; ++k) {
    int piv = k;
    for (int i = k + 1; i < n; ++i) {
      if (std::fabs(m[static_cast<size_t>(i * n + k)]) >
          std::fabs(m[static_cast<size_t>(piv * n + k)]))
        piv = i;
    }
    if (m[static_cast<size_t>(piv * n + k)] == 0) return MakeNum(0);
    if (piv != k) {
      for (int j = 0; j < n; ++j)
        std::swap(m[static_cast<size_t>(k * n + j)], m[static_cast<size_t>(piv * n + j)]);
      det = -det;
    }
    double p = m[static_cast<size_t>(k * n + k)];
    det *= p;
    for (int i = k + 1; i < n; ++i) {
      double f = m[static_cast<size_t>(i * n + k)] / p;
      for (int j = k; j < n; ++j)
        m[static_cast<size_t>(i * n + j)] -= f * m[static_cast<size_t>(k * n + j)];
    }
  }
  return MakeNum(det);
}

Operand FnMinverse(EvalContext& ctx, Operand* a, int) {
  std::vector<double> m;
  int r;
  int c;
  if (!NumericMatrix(ctx, a[0], m, r, c)) return MakeErr(Err::Value);
  if (r != c) return MakeErr(Err::Value);
  int n = r;
  std::vector<double> inv(static_cast<size_t>(n * n), 0.0);
  for (int i = 0; i < n; ++i) inv[static_cast<size_t>(i * n + i)] = 1;
  for (int k = 0; k < n; ++k) {
    int piv = k;
    for (int i = k + 1; i < n; ++i) {
      if (std::fabs(m[static_cast<size_t>(i * n + k)]) >
          std::fabs(m[static_cast<size_t>(piv * n + k)]))
        piv = i;
    }
    double p = m[static_cast<size_t>(piv * n + k)];
    if (std::fabs(p) < 1e-300) return MakeErr(Err::Num);
    if (piv != k) {
      for (int j = 0; j < n; ++j) {
        std::swap(m[static_cast<size_t>(k * n + j)], m[static_cast<size_t>(piv * n + j)]);
        std::swap(inv[static_cast<size_t>(k * n + j)], inv[static_cast<size_t>(piv * n + j)]);
      }
    }
    for (int j = 0; j < n; ++j) {
      m[static_cast<size_t>(k * n + j)] /= p;
      inv[static_cast<size_t>(k * n + j)] /= p;
    }
    for (int i = 0; i < n; ++i) {
      if (i == k) continue;
      double f = m[static_cast<size_t>(i * n + k)];
      if (f == 0) continue;
      for (int j = 0; j < n; ++j) {
        m[static_cast<size_t>(i * n + j)] -= f * m[static_cast<size_t>(k * n + j)];
        inv[static_cast<size_t>(i * n + j)] -= f * inv[static_cast<size_t>(k * n + j)];
      }
    }
  }
  Value out = Value::Array(static_cast<uint32_t>(n), static_cast<uint32_t>(n));
  for (int i = 0; i < n; ++i) {
    for (int j = 0; j < n; ++j) {
      out.mut(static_cast<uint32_t>(i), static_cast<uint32_t>(j)) =
          Value::Number(inv[static_cast<size_t>(i * n + j)]);
    }
  }
  return Operand::Of(std::move(out));
}

Operand FnMunit(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, x);
  long long n = ToInt64(x);
  if (n < 1 || n > 4096) return MakeErr(Err::Value);
  Value out = Value::Array(static_cast<uint32_t>(n), static_cast<uint32_t>(n));
  for (long long i = 0; i < n; ++i) {
    for (long long j = 0; j < n; ++j) {
      out.mut(static_cast<uint32_t>(i), static_cast<uint32_t>(j)) = Value::Number(i == j ? 1 : 0);
    }
  }
  return Operand::Of(std::move(out));
}

bool IsSubtotalCell(const EvalContext& ctx, int s, int r, int c) {
  static const int sub = FindFunction("SUBTOTAL");
  static const int agg = FindFunction("AGGREGATE");
  const Program* p = ctx.ProgramAt(s, r, c);
  if (!p) return false;
  for (const Instr& in : p->code) {
    if (in.op != Op::Call || (in.a & 0x80000000u)) continue;
    int id = static_cast<int>(in.a);
    if (id == sub || id == agg) return true;
  }
  return false;
}

Operand FnSubtotal(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, f0);
  int f = ToInt(f0);
  uint8_t skip = f > 100 ? static_cast<uint8_t>(kRowHidden | kRowFiltered) : kRowFiltered;
  if (f > 100) f -= 100;
  if (f < 1 || f > 11) return MakeErr(Err::Value);
  std::vector<double> xs;
  long long counta = 0;
  for (int i = 1; i < n; ++i) {
    if (!a[i].IsRef()) return MakeErr(Err::Value);
    std::vector<Area> areas;
    if (a[i].tag == Operand::Tag::Ref) {
      areas.push_back(a[i].area);
    } else {
      areas = *a[i].list;
    }
    Err bad = Err::None;
    for (const Area& ar : areas) {
      auto visit = [&](int r, int c, const Value& v) {
        if (ctx.wb().RowFlags(ar.sheet, r) & skip) return;
        if (IsSubtotalCell(ctx, ar.sheet, r, c)) return;
        ++counta;
        if (v.IsNumber()) xs.push_back(v.number());
        if (v.IsError() && bad == Err::None) bad = v.error();
      };
      if (ar.cols() == 1) {
        ctx.Scan(ar, visit);
      } else {
        ctx.ScanOrdered(ar, visit);
      }
    }
    if (bad != Err::None && f != 2 && f != 3) return MakeErr(bad);
  }
  double count = static_cast<double>(xs.size());
  double sum = PlainSum(xs);
  auto var = [&](bool sample) -> Operand {
    double denom = sample ? count - 1 : count;
    if (denom <= 0) return MakeErr(Err::Div0);
    double mean = sum / count;
    double ss = 0;
    for (double x : xs) ss += (x - mean) * (x - mean);
    return MakeNum(ss / denom);
  };
  switch (f) {
    case 1:
      if (count == 0) return MakeErr(Err::Div0);
      return MakeNum(sum / count);
    case 2:
      return MakeNum(count);
    case 3:
      return MakeNum(static_cast<double>(counta));
    case 4:
      return MakeNum(xs.empty() ? 0 : *std::max_element(xs.begin(), xs.end()));
    case 5:
      return MakeNum(xs.empty() ? 0 : *std::min_element(xs.begin(), xs.end()));
    case 6: {
      double p = xs.empty() ? 0 : 1;
      for (double x : xs) p *= x;
      return MakeNum(p);
    }
    case 7: {
      Operand v = var(true);
      if (v.IsError()) return v;
      return MakeNum(std::sqrt(v.value.number()));
    }
    case 8: {
      Operand v = var(false);
      if (v.IsError()) return v;
      return MakeNum(std::sqrt(v.value.number()));
    }
    case 9:
      return MakeNum(sum);
    case 10:
      return var(true);
    default:
      return var(false);
  }
}

Operand FnBase(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, x);
  NUM_ARG(1, radix0);
  double len0;
  if (Err e = ArgNumberOr(ctx, a, n, 2, 0, len0); e != Err::None) return MakeErr(e);
  double v = std::trunc(x);
  int radix = ToInt(radix0);
  int len = ToInt(len0);
  if (v < 0 || v >= 9007199254740992.0 || radix < 2 || radix > 36 || len < 0 || len > 255) {
    return MakeErr(Err::Num);
  }
  std::string out;
  uint64_t u = static_cast<uint64_t>(v);
  do {
    int d = static_cast<int>(u % static_cast<uint64_t>(radix));
    out.insert(out.begin(), static_cast<char>(d < 10 ? '0' + d : 'A' + d - 10));
    u /= static_cast<uint64_t>(radix);
  } while (u > 0);
  while (static_cast<int>(out.size()) < len) out.insert(out.begin(), '0');
  return MakeText(out);
}
Operand FnDecimal(EvalContext& ctx, Operand* a, int) {
  std::string text;
  if (Err e = ArgText(ctx, a[0], text); e != Err::None) return MakeErr(e);
  NUM_ARG(1, radix0);
  int radix = ToInt(radix0);
  if (radix < 2 || radix > 36) return MakeErr(Err::Num);
  if (text.size() > 255) return MakeErr(Err::Value);
  double v = 0;
  for (char ch : text) {
    int d;
    if (ch >= '0' && ch <= '9') {
      d = ch - '0';
    } else if (ch >= 'a' && ch <= 'z') {
      d = ch - 'a' + 10;
    } else if (ch >= 'A' && ch <= 'Z') {
      d = ch - 'A' + 10;
    } else {
      return MakeErr(Err::Num);
    }
    if (d >= radix) return MakeErr(Err::Num);
    v = v * radix + d;
  }
  return MakeNum(v);
}

Operand ToBaseText(double v, int radix, int bits, int places, bool has_places) {
  double limit = std::pow(2.0, bits - 1);
  if (v < -limit || v >= limit) return MakeErr(Err::Num);
  long long iv = static_cast<long long>(v);
  std::string out;
  if (iv < 0) {
    unsigned long long u = static_cast<unsigned long long>(iv) & ((1ULL << bits) - 1);
    do {
      int d = static_cast<int>(u % static_cast<unsigned long long>(radix));
      out.insert(out.begin(), static_cast<char>(d < 10 ? '0' + d : 'A' + d - 10));
      u /= static_cast<unsigned long long>(radix);
    } while (u > 0);
    return MakeText(out);
  }
  unsigned long long u = static_cast<unsigned long long>(iv);
  do {
    int d = static_cast<int>(u % static_cast<unsigned long long>(radix));
    out.insert(out.begin(), static_cast<char>(d < 10 ? '0' + d : 'A' + d - 10));
    u /= static_cast<unsigned long long>(radix);
  } while (u > 0);
  if (has_places) {
    if (places < static_cast<int>(out.size()) || places > 10) return MakeErr(Err::Num);
    while (static_cast<int>(out.size()) < places) out.insert(out.begin(), '0');
  }
  return MakeText(out);
}

bool FromBaseText(const std::string& s, int radix, int bits, double& out) {
  if (s.size() > 10) return false;
  if (s.empty()) {
    out = 0;
    return true;
  }
  unsigned long long u = 0;
  for (char ch : s) {
    int d;
    if (ch >= '0' && ch <= '9') {
      d = ch - '0';
    } else if (ch >= 'a' && ch <= 'f') {
      d = ch - 'a' + 10;
    } else if (ch >= 'A' && ch <= 'F') {
      d = ch - 'A' + 10;
    } else {
      return false;
    }
    if (d >= radix) return false;
    u = u * static_cast<unsigned long long>(radix) + static_cast<unsigned long long>(d);
  }
  if (s.size() == 10 && (u >> (bits - 1)) & 1ULL) {
    out = static_cast<double>(static_cast<long long>(u) - (1LL << bits));
  } else {
    out = static_cast<double>(u);
  }
  return true;
}

template <int From, int To>
Operand BaseConvert(EvalContext& ctx, Operand* a, int n) {
  constexpr int bits = 40;
  int from_bits = From == 2 ? 10 : (From == 8 ? 30 : bits);
  double v;
  if constexpr (From == 10) {
    NUM_ARG(0, x);
    v = std::trunc(x);
  } else {
    Value raw = ctx.Scalar(a[0]);
    if (raw.IsError()) return MakeErr(raw.error());
    std::string s = raw.IsNumber() ? FormatGeneral(raw.number()) : ToText(raw);
    if (raw.IsBool()) return MakeErr(Err::Value);
    if (!FromBaseText(s, From, from_bits, v)) return MakeErr(Err::Num);
  }
  if constexpr (To == 10) {
    return MakeNum(v);
  } else {
    int to_bits = To == 2 ? 10 : (To == 8 ? 30 : bits);
    double places = 0;
    bool has_places = n > 1 && !a[1].IsMissing();
    if (has_places) {
      if (Err e = ArgNumber(ctx, a[1], places); e != Err::None) return MakeErr(e);
      if (places < 0) return MakeErr(Err::Num);
    }
    return ToBaseText(v, To, to_bits, ToInt(places), has_places);
  }
}

Operand FnDelta(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, x);
  double y;
  if (Err e = ArgNumberOr(ctx, a, n, 1, 0, y); e != Err::None) return MakeErr(e);
  return MakeNum(x == y ? 1 : 0);
}
Operand FnGestep(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, x);
  double y;
  if (Err e = ArgNumberOr(ctx, a, n, 1, 0, y); e != Err::None) return MakeErr(e);
  return MakeNum(x >= y ? 1 : 0);
}

bool BitArg(EvalContext& ctx, const Operand& op, uint64_t& out, Err& err) {
  double x;
  err = ArgNumber(ctx, op, x);
  if (err != Err::None) return false;
  if (x < 0 || x != std::floor(x) || x >= 281474976710656.0) {
    err = Err::Num;
    return false;
  }
  out = static_cast<uint64_t>(x);
  return true;
}

template <int Kind>
Operand BitOp(EvalContext& ctx, Operand* a, int) {
  uint64_t x;
  Err e;
  if (!BitArg(ctx, a[0], x, e)) return MakeErr(e);
  if constexpr (Kind <= 2) {
    uint64_t y;
    if (!BitArg(ctx, a[1], y, e)) return MakeErr(e);
    if constexpr (Kind == 0) {
      return MakeNum(static_cast<double>(x & y));
    } else if constexpr (Kind == 1) {
      return MakeNum(static_cast<double>(x | y));
    } else {
      return MakeNum(static_cast<double>(x ^ y));
    }
  } else {
    NUM_ARG(1, sh0);
    double sh = std::trunc(sh0);
    if (std::fabs(sh) > 53) return MakeErr(Err::Num);
    bool left = (Kind == 3) == (sh >= 0);
    int s = static_cast<int>(std::fabs(sh));
    double r = left ? static_cast<double>(x) * std::pow(2.0, s) : static_cast<double>(x >> s);
    if (r >= 281474976710656.0) return MakeErr(Err::Num);
    return MakeNum(r);
  }
}

}

void RegisterMath(std::vector<FuncInfo>& out) {
  const FuncInfo list[] = {
      {"ABS", 1, 1, "V", 1, kFnLift, FnAbs},
      {"SIGN", 1, 1, "V", 1, kFnLift, FnSign},
      {"SQRT", 1, 1, "V", 1, kFnLift, FnSqrt},
      {"SQRTPI", 1, 1, "V", 1, kFnLift, FnSqrtPi},
      {"EXP", 1, 1, "V", 1, kFnLift, FnExp},
      {"LN", 1, 1, "V", 1, kFnLift, FnLn},
      {"LOG10", 1, 1, "V", 1, kFnLift, FnLog10},
      {"LOG", 1, 2, "VV", 1, kFnLift, FnLog},
      {"POWER", 2, 2, "VV", 1, kFnLift, FnPower},
      {"PI", 0, 0, "", 1, 0, FnPi},
      {"SIN", 1, 1, "V", 1, kFnLift, FnSin},
      {"COS", 1, 1, "V", 1, kFnLift, FnCos},
      {"TAN", 1, 1, "V", 1, kFnLift, FnTan},
      {"COT", 1, 1, "V", 1, kFnLift | kFnXlfn, FnCot},
      {"CSC", 1, 1, "V", 1, kFnLift | kFnXlfn, FnCsc},
      {"SEC", 1, 1, "V", 1, kFnLift | kFnXlfn, FnSec},
      {"ASIN", 1, 1, "V", 1, kFnLift, FnAsin},
      {"ACOS", 1, 1, "V", 1, kFnLift, FnAcos},
      {"ATAN", 1, 1, "V", 1, kFnLift, FnAtan},
      {"ACOT", 1, 1, "V", 1, kFnLift | kFnXlfn, FnAcot},
      {"ATAN2", 2, 2, "VV", 1, kFnLift, FnAtan2},
      {"SINH", 1, 1, "V", 1, kFnLift, FnSinh},
      {"COSH", 1, 1, "V", 1, kFnLift, FnCosh},
      {"TANH", 1, 1, "V", 1, kFnLift, FnTanh},
      {"COTH", 1, 1, "V", 1, kFnLift | kFnXlfn, FnCoth},
      {"CSCH", 1, 1, "V", 1, kFnLift | kFnXlfn, FnCsch},
      {"SECH", 1, 1, "V", 1, kFnLift | kFnXlfn, FnSech},
      {"ASINH", 1, 1, "V", 1, kFnLift, FnAsinh},
      {"ACOSH", 1, 1, "V", 1, kFnLift, FnAcosh},
      {"ATANH", 1, 1, "V", 1, kFnLift, FnAtanh},
      {"ACOTH", 1, 1, "V", 1, kFnLift | kFnXlfn, FnAcoth},
      {"DEGREES", 1, 1, "V", 1, kFnLift, FnDegrees},
      {"RADIANS", 1, 1, "V", 1, kFnLift, FnRadians},
      {"ROUND", 2, 2, "VV", 1, kFnLift, FnRound},
      {"ROUNDUP", 2, 2, "VV", 1, kFnLift, FnRoundUp},
      {"ROUNDDOWN", 2, 2, "VV", 1, kFnLift, FnRoundDown},
      {"TRUNC", 1, 2, "VV", 1, kFnLift, FnTrunc},
      {"INT", 1, 1, "V", 1, kFnLift, FnInt},
      {"EVEN", 1, 1, "V", 1, kFnLift, FnEven},
      {"ODD", 1, 1, "V", 1, kFnLift, FnOdd},
      {"MROUND", 2, 2, "VV", 1, kFnLift, FnMround},
      {"CEILING", 1, 2, "VV", 1, kFnLift, FnCeiling},
      {"FLOOR", 1, 2, "VV", 1, kFnLift, FnFloor},
      {"CEILING.MATH", 1, 3, "VVV", 1, kFnLift | kFnXlfn, FnCeilingMath},
      {"FLOOR.MATH", 1, 3, "VVV", 1, kFnLift | kFnXlfn, FnFloorMath},
      {"CEILING.PRECISE", 1, 2, "VV", 1, kFnLift | kFnXlfn, FnCeilingPrecise},
      {"ISO.CEILING", 1, 2, "VV", 1, kFnLift | kFnXlfn, FnCeilingPrecise},
      {"FLOOR.PRECISE", 1, 2, "VV", 1, kFnLift | kFnXlfn, FnFloorPrecise},
      {"MOD", 2, 2, "VV", 1, kFnLift, FnMod},
      {"QUOTIENT", 2, 2, "VV", 1, kFnLift, FnQuotient},
      {"FACT", 1, 1, "V", 1, kFnLift, FnFact},
      {"FACTDOUBLE", 1, 1, "V", 1, kFnLift, FnFactDouble},
      {"COMBIN", 2, 2, "VV", 1, kFnLift, FnCombin},
      {"COMBINA", 2, 2, "VV", 1, kFnLift | kFnXlfn, FnCombinA},
      {"MULTINOMIAL", 1, 255, "R", 1, 0, FnMultinomial},
      {"GCD", 1, 255, "R", 1, 0, FnGcd},
      {"LCM", 1, 255, "R", 1, 0, FnLcm},
      {"SUM", 1, 255, "R", 1, 0, FnSum},
      {"PRODUCT", 1, 255, "R", 1, 0, FnProduct},
      {"SUMSQ", 1, 255, "R", 1, 0, FnSumSq},
      {"SUMPRODUCT", 1, 255, "A", 1, 0, FnSumProduct},
      {"SUMX2MY2", 2, 2, "AA", 1, 0, FnSumX2MY2},
      {"SUMX2PY2", 2, 2, "AA", 1, 0, FnSumX2PY2},
      {"SUMXMY2", 2, 2, "AA", 1, 0, FnSumXMY2},
      {"RAND", 0, 0, "", 1, kFnVolatile, FnRand},
      {"RANDBETWEEN", 2, 2, "VV", 1, kFnVolatile | kFnLift, FnRandBetween},
      {"MMULT", 2, 2, "AA", 1, 0, FnMmult},
      {"MDETERM", 1, 1, "A", 1, 0, FnMdeterm},
      {"MINVERSE", 1, 1, "A", 1, 0, FnMinverse},
      {"MUNIT", 1, 1, "V", 1, kFnXlfn, FnMunit},
      {"SUBTOTAL", 2, 255, "Vr", 1, 0, FnSubtotal},
      {"BASE", 2, 3, "VVV", 1, kFnLift | kFnXlfn, FnBase},
      {"DECIMAL", 2, 2, "VV", 1, kFnLift | kFnXlfn, FnDecimal},
      {"BIN2DEC", 1, 1, "V", 1, kFnLift, BaseConvert<2, 10>},
      {"BIN2HEX", 1, 2, "VV", 1, kFnLift, BaseConvert<2, 16>},
      {"BIN2OCT", 1, 2, "VV", 1, kFnLift, BaseConvert<2, 8>},
      {"DEC2BIN", 1, 2, "VV", 1, kFnLift, BaseConvert<10, 2>},
      {"DEC2HEX", 1, 2, "VV", 1, kFnLift, BaseConvert<10, 16>},
      {"DEC2OCT", 1, 2, "VV", 1, kFnLift, BaseConvert<10, 8>},
      {"HEX2BIN", 1, 2, "VV", 1, kFnLift, BaseConvert<16, 2>},
      {"HEX2DEC", 1, 1, "V", 1, kFnLift, BaseConvert<16, 10>},
      {"HEX2OCT", 1, 2, "VV", 1, kFnLift, BaseConvert<16, 8>},
      {"OCT2BIN", 1, 2, "VV", 1, kFnLift, BaseConvert<8, 2>},
      {"OCT2DEC", 1, 1, "V", 1, kFnLift, BaseConvert<8, 10>},
      {"OCT2HEX", 1, 2, "VV", 1, kFnLift, BaseConvert<8, 16>},
      {"DELTA", 1, 2, "VV", 1, kFnLift, FnDelta},
      {"GESTEP", 1, 2, "VV", 1, kFnLift, FnGestep},
      {"BITAND", 2, 2, "VV", 1, kFnLift | kFnXlfn, BitOp<0>},
      {"BITOR", 2, 2, "VV", 1, kFnLift | kFnXlfn, BitOp<1>},
      {"BITXOR", 2, 2, "VV", 1, kFnLift | kFnXlfn, BitOp<2>},
      {"BITLSHIFT", 2, 2, "VV", 1, kFnLift | kFnXlfn, BitOp<3>},
      {"BITRSHIFT", 2, 2, "VV", 1, kFnLift | kFnXlfn, BitOp<4>},
  };
  out.insert(out.end(), std::begin(list), std::end(list));
}

}
