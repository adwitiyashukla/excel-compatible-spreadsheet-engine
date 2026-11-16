#include <algorithm>
#include <cmath>
#include <vector>

#include "eval.h"

namespace sheet {

namespace {

#define NUM_ARG(i, var) \
  double var;           \
  if (Err e_ = ArgNumber(ctx, a[i], var); e_ != Err::None) return MakeErr(e_)

#define OPT_ARG(i, def, var) \
  double var;                \
  if (Err e_ = ArgNumberOr(ctx, a, n, i, def, var); e_ != Err::None) return MakeErr(e_)

double Fv(double r, double n, double pmt, double pv, double type) {
  if (r == 0) return -(pv + pmt * n);
  double f = std::pow(1 + r, n);
  return -(pv * f + pmt * (1 + r * type) * (f - 1) / r);
}

double Pmt(double r, double n, double pv, double fv, double type) {
  if (r == 0) return -(pv + fv) / n;
  double f = std::pow(1 + r, n);
  return -(pv * f + fv) * r / ((1 + r * type) * (f - 1));
}

double Ipmt(double r, double per, double n, double pv, double fv, double type) {
  double pmt = Pmt(r, n, pv, fv, type);
  if (per == 1 && type != 0) return 0;
  double ip = Fv(r, per - 1, pmt, pv, type) * r;
  if (type != 0) ip /= 1 + r;
  return ip;
}

Operand FnPv(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, r);
  NUM_ARG(1, nper);
  NUM_ARG(2, pmt);
  OPT_ARG(3, 0, fv);
  OPT_ARG(4, 0, type);
  type = type != 0 ? 1 : 0;
  if (r == 0) return MakeNum(-(fv + pmt * nper));
  if (r <= -1) return MakeErr(Err::Num);
  double f = std::pow(1 + r, nper);
  return MakeNum(-(fv + pmt * (1 + r * type) * (f - 1) / r) / f);
}
Operand FnFv(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, r);
  NUM_ARG(1, nper);
  NUM_ARG(2, pmt);
  OPT_ARG(3, 0, pv);
  OPT_ARG(4, 0, type);
  return MakeNum(Fv(r, nper, pmt, pv, type != 0 ? 1 : 0));
}
Operand FnPmt(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, r);
  NUM_ARG(1, nper);
  NUM_ARG(2, pv);
  OPT_ARG(3, 0, fv);
  OPT_ARG(4, 0, type);
  if (nper == 0) return MakeErr(Err::Num);
  return MakeNum(Pmt(r, nper, pv, fv, type != 0 ? 1 : 0));
}
Operand FnNper(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, r);
  NUM_ARG(1, pmt);
  NUM_ARG(2, pv);
  OPT_ARG(3, 0, fv);
  OPT_ARG(4, 0, type0);
  double type = type0 != 0 ? 1 : 0;
  if (r == 0) {
    if (pmt == 0) return MakeErr(Err::Num);
    return MakeNum(-(pv + fv) / pmt);
  }
  double num = pmt * (1 + r * type) - fv * r;
  double den = pmt * (1 + r * type) + pv * r;
  if (den == 0 || num / den <= 0 || r <= -1) return MakeErr(Err::Num);
  return MakeNum(std::log(num / den) / std::log(1 + r));
}
bool RateIteration(double nper, double pmt, double pv, double fv, bool due, double& guess) {
  const int kMaxIter = 150;
  const double kTiny = 1e-14;
  const double kStep = 1e-7;
  if (due) {
    fv -= pmt;
    pv += pmt;
  }
  bool valid = true;
  bool found = false;
  int count = 0;
  double x = guess;
  bool integral = nper == std::round(nper);
  if (!integral && x < -1) x = -1;
  while (valid && !found && count < kMaxIter) {
    double pow_n1 = std::pow(1 + x, nper - 1);
    double pow_n = integral ? pow_n1 * (1 + x) : std::pow(1 + x, nper);
    double geo;
    double dgeo;
    if (x == 0) {
      geo = nper;
      dgeo = nper * (nper - 1) / 2;
    } else {
      geo = (pow_n - 1) / x;
      dgeo = nper * pow_n1 / x - geo / x;
    }
    double term = fv + pv * pow_n + pmt * geo;
    double dterm = pv * nper * pow_n1 + pmt * dgeo;
    if (std::fabs(term) < kTiny) {
      found = true;
    } else {
      double next = dterm == 0 ? x + 1.1 * kStep : x - term / dterm;
      ++count;
      found = std::fabs(next - x) < kStep;
      x = next;
      if (!integral) valid = x >= -1;
    }
  }
  if (integral) valid = x > -1;
  guess = x;
  return valid && found && std::isfinite(x);
}

Operand FnRate(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, nper);
  NUM_ARG(1, pmt);
  NUM_ARG(2, pv);
  OPT_ARG(3, 0, fv);
  OPT_ARG(4, 0, type);
  bool default_guess = n < 6 || a[5].IsMissing();
  OPT_ARG(5, 0.1, guess);
  if (nper <= 0) return MakeErr(Err::Num);
  double start = guess;
  double r = guess;
  bool ok = RateIteration(nper, pmt, pv, fv, type != 0, r);
  for (int step = 2; !ok && default_guess && step <= 10; ++step) {
    r = start * step;
    ok = RateIteration(nper, pmt, pv, fv, type != 0, r);
    if (!ok) {
      r = start / step;
      ok = RateIteration(nper, pmt, pv, fv, type != 0, r);
    }
  }
  if (!ok) return MakeErr(Err::Num);
  return MakeNum(r);
}
Operand FnIpmt(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, r);
  NUM_ARG(1, per);
  NUM_ARG(2, nper);
  NUM_ARG(3, pv);
  OPT_ARG(4, 0, fv);
  OPT_ARG(5, 0, type);
  if (per < 1 || per > nper) return MakeErr(Err::Num);
  return MakeNum(Ipmt(r, per, nper, pv, fv, type != 0 ? 1 : 0));
}
Operand FnPpmt(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, r);
  NUM_ARG(1, per);
  NUM_ARG(2, nper);
  NUM_ARG(3, pv);
  OPT_ARG(4, 0, fv);
  OPT_ARG(5, 0, type0);
  if (per < 1 || per > nper) return MakeErr(Err::Num);
  double type = type0 != 0 ? 1 : 0;
  return MakeNum(Pmt(r, nper, pv, fv, type) - Ipmt(r, per, nper, pv, fv, type));
}
template <bool Interest>
Operand FnCumulative(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, r);
  NUM_ARG(1, nper);
  NUM_ARG(2, pv);
  NUM_ARG(3, s0);
  NUM_ARG(4, e0);
  NUM_ARG(5, type);
  double s = std::ceil(s0);
  double e = std::floor(e0);
  if (r <= 0 || nper <= 0 || pv <= 0 || s < 1 || e < s || e > nper || (type != 0 && type != 1)) {
    return MakeErr(Err::Num);
  }
  if (e - s > 1e7) return MakeErr(Err::Num);
  double total = 0;
  double pmt = Pmt(r, nper, pv, 0, type);
  for (double per = s; per <= e; per += 1) {
    double ip = Ipmt(r, per, nper, pv, 0, type);
    total += Interest ? ip : pmt - ip;
  }
  return MakeNum(total);
}

Err Flows(EvalContext& ctx, Operand* a, int count, std::vector<double>& out) {
  for (int i = 0; i < count; ++i) {
    const Operand& o = a[i];
    if (o.IsRef() || o.IsArray()) {
      std::vector<double> xs;
      Operand copy = o;
      if (Err e = CollectNumbers(ctx, &copy, 1, xs); e != Err::None) return e;
      out.insert(out.end(), xs.begin(), xs.end());
      continue;
    }
    double x;
    if (Err e = ArgNumber(ctx, o, x); e != Err::None) return e;
    out.push_back(x);
  }
  return Err::None;
}

Operand FnNpv(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, r);
  std::vector<double> cf;
  if (Err e = Flows(ctx, a + 1, n - 1, cf); e != Err::None) return MakeErr(e);
  if (r == -1) return MakeErr(Err::Div0);
  double total = 0;
  double f = 1;
  for (double v : cf) {
    f *= 1 + r;
    total += v / f;
  }
  return MakeNum(total);
}

bool SolveIrr(const std::vector<double>& cf, double guess, double& out) {
  bool pos = false;
  bool neg = false;
  for (double v : cf) {
    pos = pos || v > 0;
    neg = neg || v < 0;
  }
  if (!pos || !neg) return false;
  double r = guess;
  for (int i = 0; i < 200; ++i) {
    double f = 0;
    double df = 0;
    double g = 1;
    for (size_t k = 0; k < cf.size(); ++k) {
      f += cf[k] / g;
      df -= static_cast<double>(k) * cf[k] / (g * (1 + r));
      g *= 1 + r;
    }
    if (df == 0 || !std::isfinite(f) || !std::isfinite(df)) return false;
    double next = r - f / df;
    if (next <= -1) next = (r - 1) / 2;
    if (std::fabs(next - r) < 1e-12 * std::max(1.0, std::fabs(r))) {
      out = next;
      return true;
    }
    r = next;
  }
  return false;
}

Operand FnIrr(EvalContext& ctx, Operand* a, int n) {
  std::vector<double> cf;
  if (Err e = CollectNumbers(ctx, a, 1, cf); e != Err::None) return MakeErr(e);
  OPT_ARG(1, 0.1, guess);
  double r;
  if (!SolveIrr(cf, guess, r)) return MakeErr(Err::Num);
  return MakeNum(r);
}

Err DatedFlows(EvalContext& ctx, Operand* a, std::vector<double>& v, std::vector<double>& d) {
  Grid gv;
  Grid gd;
  if (!MakeGrid(ctx, a[0], gv, false) || !MakeGrid(ctx, a[1], gd, false)) return Err::Value;
  if (gv.rows * gv.cols != gd.rows * gd.cols) return Err::Num;
  int total = gv.rows * gv.cols;
  for (int i = 0; i < total; ++i) {
    const Value& x = gv.At(i / gv.cols, i % gv.cols);
    const Value& y = gd.At(i / gd.cols, i % gd.cols);
    if (x.IsError()) return x.error();
    if (y.IsError()) return y.error();
    double xv;
    double yv;
    if (Err e = ToNumber(x, ctx, xv); e != Err::None) return Err::Value;
    if (Err e = ToNumber(y, ctx, yv); e != Err::None) return Err::Value;
    v.push_back(xv);
    d.push_back(std::floor(yv));
  }
  if (v.empty()) return Err::Num;
  for (double x : d) {
    if (x < d[0]) return Err::Num;
  }
  return Err::None;
}

double Xnpv(double r, const std::vector<double>& v, const std::vector<double>& d) {
  double total = 0;
  for (size_t i = 0; i < v.size(); ++i) total += v[i] / std::pow(1 + r, (d[i] - d[0]) / 365.0);
  return total;
}

Operand FnXnpv(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, r);
  std::vector<double> v;
  std::vector<double> d;
  if (Err e = DatedFlows(ctx, a + 1, v, d); e != Err::None) return MakeErr(e);
  if (r <= -1) return MakeErr(Err::Num);
  return MakeNum(Xnpv(r, v, d));
}

Operand FnXirr(EvalContext& ctx, Operand* a, int n) {
  std::vector<double> v;
  std::vector<double> d;
  if (Err e = DatedFlows(ctx, a, v, d); e != Err::None) return MakeErr(e);
  OPT_ARG(2, 0.1, guess);
  bool pos = false;
  bool neg = false;
  for (double x : v) {
    pos = pos || x > 0;
    neg = neg || x < 0;
  }
  if (!pos || !neg) return MakeErr(Err::Num);
  double r = guess;
  for (int i = 0; i < 200; ++i) {
    double f = 0;
    double df = 0;
    for (size_t k = 0; k < v.size(); ++k) {
      double t = (d[k] - d[0]) / 365.0;
      double g = std::pow(1 + r, t);
      f += v[k] / g;
      df -= t * v[k] / (g * (1 + r));
    }
    if (df == 0 || !std::isfinite(f)) return MakeErr(Err::Num);
    double next = r - f / df;
    if (next <= -1) next = (r - 1) / 2;
    if (std::fabs(next - r) < 1e-12) return MakeNum(next);
    r = next;
  }
  return MakeErr(Err::Num);
}

Operand FnMirr(EvalContext& ctx, Operand* a, int) {
  std::vector<double> cf;
  if (Err e = CollectNumbers(ctx, a, 1, cf); e != Err::None) return MakeErr(e);
  NUM_ARG(1, fr);
  NUM_ARG(2, rr);
  double n = static_cast<double>(cf.size());
  double npv_pos = 0;
  double npv_neg = 0;
  for (size_t i = 0; i < cf.size(); ++i) {
    double v = cf[i];
    if (v > 0) npv_pos += v / std::pow(1 + rr, static_cast<double>(i));
    if (v < 0) npv_neg += v / std::pow(1 + fr, static_cast<double>(i));
  }
  if (npv_neg == 0 || npv_pos == 0 || n < 2) return MakeErr(Err::Div0);
  return MakeNum(std::pow(-npv_pos * std::pow(1 + rr, n) / (npv_neg * (1 + rr)), 1 / (n - 1)) - 1);
}

Operand FnSln(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, cost);
  NUM_ARG(1, salvage);
  NUM_ARG(2, life);
  if (life == 0) return MakeErr(Err::Div0);
  return MakeNum((cost - salvage) / life);
}
Operand FnSyd(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, cost);
  NUM_ARG(1, salvage);
  NUM_ARG(2, life);
  NUM_ARG(3, per);
  if (life <= 0 || per <= 0 || per > life || salvage < 0) return MakeErr(Err::Num);
  return MakeNum((cost - salvage) * (life - per + 1) * 2 / (life * (life + 1)));
}
Operand FnDdb(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, cost);
  NUM_ARG(1, salvage);
  NUM_ARG(2, life);
  NUM_ARG(3, per);
  OPT_ARG(4, 2, factor);
  if (cost < 0 || salvage < 0 || life <= 0 || per <= 0 || per > life || factor <= 0) {
    return MakeErr(Err::Num);
  }
  double rate = factor / life;
  double old_value;
  if (rate >= 1) {
    rate = 1;
    old_value = per == 1 ? cost : 0;
  } else {
    old_value = cost * std::pow(1 - rate, per - 1);
  }
  double new_value = cost * std::pow(1 - rate, per);
  if (new_value < salvage) {
    double dep = old_value - salvage;
    return MakeNum(dep < 0 ? 0 : dep);
  }
  return MakeNum(old_value - new_value);
}
Operand FnDb(EvalContext& ctx, Operand* a, int n) {
  NUM_ARG(0, cost);
  NUM_ARG(1, salvage);
  NUM_ARG(2, life);
  NUM_ARG(3, per0);
  OPT_ARG(4, 12, month0);
  double per = std::trunc(per0);
  double month = std::trunc(month0);
  if (cost < 0 || salvage < 0 || life <= 0 || per < 1 || month < 1 || month > 12 ||
      per > life + (month < 12 ? 1 : 0)) {
    return MakeErr(Err::Num);
  }
  if (cost == 0) return MakeNum(0);
  if (per > 1e7) return MakeErr(Err::Num);
  double rate = 1 - std::pow(salvage / cost, 1 / life);
  rate = std::floor(rate * 1000 + 0.5) / 1000;
  double total = 0;
  double dep = cost * rate * month / 12;
  if (per == 1) return MakeNum(dep);
  total = dep;
  for (double p = 2; p <= per; p += 1) {
    if (p == life + 1) {
      dep = (cost - total) * rate * (12 - month) / 12;
    } else {
      dep = (cost - total) * rate;
    }
    if (p < per) total += dep;
  }
  return MakeNum(dep);
}
Operand FnEffect(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, rate);
  NUM_ARG(1, np0);
  double np = std::trunc(np0);
  if (rate <= 0 || np < 1) return MakeErr(Err::Num);
  return MakeNum(std::pow(1 + rate / np, np) - 1);
}
Operand FnNominal(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, rate);
  NUM_ARG(1, np0);
  double np = std::trunc(np0);
  if (rate <= 0 || np < 1) return MakeErr(Err::Num);
  return MakeNum((std::pow(1 + rate, 1 / np) - 1) * np);
}
Operand FnRri(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, nper);
  NUM_ARG(1, pv);
  NUM_ARG(2, fv);
  if (nper <= 0 || pv == 0) return MakeErr(Err::Num);
  double r = std::pow(fv / pv, 1 / nper) - 1;
  if (!std::isfinite(r)) return MakeErr(Err::Num);
  return MakeNum(r);
}
Operand FnPduration(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, rate);
  NUM_ARG(1, pv);
  NUM_ARG(2, fv);
  if (rate <= 0 || pv <= 0 || fv <= 0) return MakeErr(Err::Num);
  return MakeNum((std::log(fv) - std::log(pv)) / std::log(1 + rate));
}
Operand FnIspmt(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, rate);
  NUM_ARG(1, per);
  NUM_ARG(2, nper);
  NUM_ARG(3, pv);
  if (nper == 0) return MakeErr(Err::Div0);
  return MakeNum(pv * rate * (per / nper - 1));
}
Operand FnFvSchedule(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, principal);
  std::vector<double> rates;
  if (Err e = CollectNumbers(ctx, a + 1, 1, rates, CollectMode::NumbersA); e != Err::None) {
    return MakeErr(e);
  }
  double v = principal;
  for (double r : rates) v *= 1 + r;
  return MakeNum(v);
}

}

void RegisterFinance(std::vector<FuncInfo>& out) {
  const FuncInfo list[] = {
      {"PV", 3, 5, "VVVVV", 1, kFnLift, FnPv},
      {"FV", 3, 5, "VVVVV", 1, kFnLift, FnFv},
      {"PMT", 3, 5, "VVVVV", 1, kFnLift, FnPmt},
      {"NPER", 3, 5, "VVVVV", 1, kFnLift, FnNper},
      {"RATE", 3, 6, "VVVVVV", 1, kFnLift, FnRate},
      {"IPMT", 4, 6, "VVVVVV", 1, kFnLift, FnIpmt},
      {"PPMT", 4, 6, "VVVVVV", 1, kFnLift, FnPpmt},
      {"CUMIPMT", 6, 6, "VVVVVV", 1, kFnLift, FnCumulative<true>},
      {"CUMPRINC", 6, 6, "VVVVVV", 1, kFnLift, FnCumulative<false>},
      {"NPV", 2, 255, "VR", 1, 0, FnNpv},
      {"IRR", 1, 2, "AV", 1, 0, FnIrr},
      {"XNPV", 3, 3, "VAA", 1, 0, FnXnpv},
      {"XIRR", 2, 3, "AAV", 1, 0, FnXirr},
      {"MIRR", 3, 3, "AVV", 1, 0, FnMirr},
      {"SLN", 3, 3, "VVV", 1, kFnLift, FnSln},
      {"SYD", 4, 4, "VVVV", 1, kFnLift, FnSyd},
      {"DDB", 4, 5, "VVVVV", 1, kFnLift, FnDdb},
      {"DB", 4, 5, "VVVVV", 1, kFnLift, FnDb},
      {"EFFECT", 2, 2, "VV", 1, kFnLift, FnEffect},
      {"NOMINAL", 2, 2, "VV", 1, kFnLift, FnNominal},
      {"RRI", 3, 3, "VVV", 1, kFnLift | kFnXlfn, FnRri},
      {"PDURATION", 3, 3, "VVV", 1, kFnLift | kFnXlfn, FnPduration},
      {"ISPMT", 4, 4, "VVVV", 1, kFnLift, FnIspmt},
      {"FVSCHEDULE", 2, 2, "VA", 1, 0, FnFvSchedule},
  };
  out.insert(out.end(), std::begin(list), std::end(list));
}

}
