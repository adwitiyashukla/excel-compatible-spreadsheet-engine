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

Err DateArg(EvalContext& ctx, const Operand& op, double& out) {
  Err e = ArgNumber(ctx, op, out);
  if (e != Err::None) return e;
  if (out < 0 || out >= kMaxSerial + 1) return Err::Num;
  return Err::None;
}

#define DATE_ARG(i, var) \
  double var;            \
  if (Err e_ = DateArg(ctx, a[i], var); e_ != Err::None) return MakeErr(e_)

Operand FnDate(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, y);
  NUM_ARG(1, m);
  NUM_ARG(2, d);
  double s;
  if (!SerialFromDate(ToInt(y), ToInt(m), ToInt(d), ctx.dates(), s)) {
    return MakeErr(Err::Num);
  }
  return MakeNum(s);
}

Operand FnDateValue(EvalContext& ctx, Operand* a, int) {
  Value v = ctx.Scalar(a[0]);
  if (v.IsError()) return Operand::Of(v);
  if (!v.IsText()) return MakeErr(Err::Value);
  double s;
  if (!ParseDateTime(v.text(), ctx.dates(), s)) return MakeErr(Err::Value);
  return MakeNum(std::floor(s));
}

Operand FnTimeValue(EvalContext& ctx, Operand* a, int) {
  Value v = ctx.Scalar(a[0]);
  if (v.IsError()) return Operand::Of(v);
  if (!v.IsText()) return MakeErr(Err::Value);
  double s;
  if (!ParseDateTime(v.text(), ctx.dates(), s)) return MakeErr(Err::Value);
  return MakeNum(s - std::floor(s));
}

template <int Part>
Operand FnDatePart(EvalContext& ctx, Operand* a, int) {
  DATE_ARG(0, s);
  int y;
  int m;
  int d;
  if (!DateFromSerial(s, ctx.dates(), y, m, d)) return MakeErr(Err::Num);
  if constexpr (Part == 0) {
    return MakeNum(y);
  } else if constexpr (Part == 1) {
    return MakeNum(m);
  } else {
    return MakeNum(d);
  }
}

template <int Part>
Operand FnTimePart(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, s);
  if (s < 0) return MakeErr(Err::Num);
  int h;
  int mi;
  int sec;
  TimeFromSerial(s, h, mi, sec);
  if constexpr (Part == 0) {
    return MakeNum(h);
  } else if constexpr (Part == 1) {
    return MakeNum(mi);
  } else {
    return MakeNum(sec);
  }
}

Operand FnTime(EvalContext& ctx, Operand* a, int) {
  NUM_ARG(0, h);
  NUM_ARG(1, m);
  NUM_ARG(2, s);
  double hh = std::trunc(h);
  double mm = std::trunc(m);
  double ss = std::trunc(s);
  if (hh > 32767 || mm > 32767 || ss > 32767) return MakeErr(Err::Num);
  double total = hh * 3600 + mm * 60 + ss;
  if (total < 0) return MakeErr(Err::Num);
  return MakeNum(std::fmod(total, 86400.0) / 86400.0);
}

Operand FnNow(EvalContext& ctx, Operand*, int) {
  return MakeNum(ctx.Now());
}
Operand FnToday(EvalContext& ctx, Operand*, int) {
  return MakeNum(std::floor(ctx.Now()));
}

Operand FnWeekday(EvalContext& ctx, Operand* a, int n) {
  DATE_ARG(0, s);
  double t0;
  if (Err e = ArgNumberOr(ctx, a, n, 1, 1, t0); e != Err::None) return MakeErr(e);
  int t = ToInt(t0);
  int w = WeekdayFromSerial(s, ctx.dates());
  switch (t) {
    case 1:
      return MakeNum(w + 1);
    case 2:
      return MakeNum(w == 0 ? 7 : w);
    case 3:
      return MakeNum(w == 0 ? 6 : w - 1);
    default:
      if (t >= 11 && t <= 17) {
        int start = t - 10;
        int startw = start % 7;
        return MakeNum((w - startw + 7) % 7 + 1);
      }
      return MakeErr(Err::Num);
  }
}

int IsoWeek(double serial, const DateSystem& ds) {
  int y;
  int m;
  int d;
  DateFromSerial(serial, ds, y, m, d);
  int64_t days = DaysFromCivil(y, m, d);
  int wd = static_cast<int>(((days % 7) + 7 + 3) % 7);
  int64_t thursday = days - wd + 3;
  int ty;
  int tm;
  int td;
  CivilFromDays(thursday, ty, tm, td);
  int64_t jan1 = DaysFromCivil(ty, 1, 1);
  return static_cast<int>((thursday - jan1) / 7 + 1);
}

Operand FnWeekNum(EvalContext& ctx, Operand* a, int n) {
  DATE_ARG(0, s);
  double t0;
  if (Err e = ArgNumberOr(ctx, a, n, 1, 1, t0); e != Err::None) return MakeErr(e);
  int t = ToInt(t0);
  if (t == 21) return MakeNum(IsoWeek(s, ctx.dates()));
  int start;
  if (t == 1) {
    start = 0;
  } else if (t == 2) {
    start = 1;
  } else if (t >= 11 && t <= 17) {
    start = (t - 10) % 7;
  } else {
    return MakeErr(Err::Num);
  }
  int y;
  int m;
  int d;
  if (!DateFromSerial(s, ctx.dates(), y, m, d)) return MakeErr(Err::Num);
  double jan1;
  SerialFromDate(y, 1, 1, ctx.dates(), jan1);
  int w1 = WeekdayFromSerial(jan1, ctx.dates());
  int offset = (w1 - start + 7) % 7;
  double doy = std::floor(s) - jan1;
  return MakeNum(std::floor((doy + offset) / 7) + 1);
}

Operand FnIsoWeekNum(EvalContext& ctx, Operand* a, int) {
  DATE_ARG(0, s);
  return MakeNum(IsoWeek(s, ctx.dates()));
}

bool AddMonths(double s, int months, bool end, const DateSystem& ds, double& out) {
  int y;
  int m;
  int d;
  if (!DateFromSerial(s, ds, y, m, d)) return false;
  long total = static_cast<long>(y) * 12 + (m - 1) + months;
  int ny = static_cast<int>(std::floor(total / 12.0));
  int nm = static_cast<int>(total - static_cast<long>(ny) * 12) + 1;
  if (ny < 1900 || ny > 9999) return false;
  int dim = DaysInMonth(ny, nm);
  int nd = end ? dim : std::min(d, dim);
  return SerialFromDate(ny, nm, nd, ds, out);
}

Operand FnEdate(EvalContext& ctx, Operand* a, int) {
  DATE_ARG(0, s);
  NUM_ARG(1, m);
  double out;
  if (!AddMonths(s, ToInt(m), false, ctx.dates(), out)) return MakeErr(Err::Num);
  return MakeNum(out);
}
Operand FnEomonth(EvalContext& ctx, Operand* a, int) {
  DATE_ARG(0, s);
  NUM_ARG(1, m);
  double out;
  if (!AddMonths(s, ToInt(m), true, ctx.dates(), out)) return MakeErr(Err::Num);
  return MakeNum(out);
}

Operand FnDays(EvalContext& ctx, Operand* a, int) {
  DATE_ARG(0, e);
  DATE_ARG(1, s);
  return MakeNum(std::floor(e) - std::floor(s));
}

bool LastOfFeb(int y, int m, int d) {
  if (m != 2) return false;
  if (d == 29) return true;
  return d == 28 && !IsLeapYear(y) && y != 1900;
}

double Days360(int y1, int m1, int d1, int y2, int m2, int d2, bool european) {
  if (european) {
    if (d1 == 31) d1 = 30;
    if (d2 == 31) d2 = 30;
  } else {
    if (d1 == 31) {
      d1 = 30;
    } else if (LastOfFeb(y1, m1, d1)) {
      d1 = 30;
    }
    if (d2 == 31 && d1 == 30) d2 = 30;
  }
  return (y2 - y1) * 360.0 + (m2 - m1) * 30.0 + (d2 - d1);
}

Operand FnDays360(EvalContext& ctx, Operand* a, int n) {
  DATE_ARG(0, s);
  DATE_ARG(1, e);
  bool eu;
  if (Err er = ArgBoolOr(ctx, a, n, 2, false, eu); er != Err::None) return MakeErr(er);
  int y1;
  int m1;
  int d1;
  int y2;
  int m2;
  int d2;
  DateFromSerial(s, ctx.dates(), y1, m1, d1);
  DateFromSerial(e, ctx.dates(), y2, m2, d2);
  return MakeNum(Days360(y1, m1, d1, y2, m2, d2, eu));
}

Operand FnDateDif(EvalContext& ctx, Operand* a, int) {
  DATE_ARG(0, s);
  DATE_ARG(1, e);
  std::string unit;
  if (Err er = ArgText(ctx, a[2], unit); er != Err::None) return MakeErr(er);
  if (s > e) return MakeErr(Err::Num);
  int y1;
  int m1;
  int d1;
  int y2;
  int m2;
  int d2;
  DateFromSerial(std::floor(s), ctx.dates(), y1, m1, d1);
  DateFromSerial(std::floor(e), ctx.dates(), y2, m2, d2);
  std::string u = ToUpper(unit);
  if (u == "D") return MakeNum(std::floor(e) - std::floor(s));
  int months = (y2 - y1) * 12 + (m2 - m1) - (d2 < d1 ? 1 : 0);
  if (u == "M") return MakeNum(months);
  if (u == "Y") return MakeNum(std::floor(months / 12.0));
  if (u == "YM") return MakeNum(months % 12);
  if (u == "MD") {
    if (d2 >= d1) return MakeNum(d2 - d1);
    int pm = m2 - 1;
    int py = y2;
    if (pm == 0) {
      pm = 12;
      --py;
    }
    return MakeNum(DaysInMonth(py, pm) - d1 + d2);
  }
  if (u == "YD") {
    int ny = y1;
    if (m2 > m1 || (m2 == m1 && d2 >= d1)) {
      ny = y2;
    } else {
      ny = y2 - 1;
    }
    int dd = std::min(d1, DaysInMonth(ny, m1));
    double anchor;
    if (!SerialFromDate(ny, m1, dd, ctx.dates(), anchor)) return MakeErr(Err::Num);
    return MakeNum(std::floor(e) - anchor);
  }
  return MakeErr(Err::Num);
}

Err ParseWeekend(EvalContext& ctx, const Operand& op, bool mask[7]) {
  for (int i = 0; i < 7; ++i) mask[i] = false;
  if (op.IsMissing()) {
    mask[0] = mask[6] = true;
    return Err::None;
  }
  Value v = ctx.Scalar(op);
  if (v.IsError()) return v.error();
  if (v.IsText()) {
    std::string_view s = v.text();
    if (s.size() != 7) return Err::Value;
    int weekend_days = 0;
    for (int i = 0; i < 7; ++i) {
      if (s[i] != '0' && s[i] != '1') return Err::Value;
      bool on = s[i] == '1';
      mask[(i + 1) % 7] = on;
      weekend_days += on ? 1 : 0;
    }
    if (weekend_days == 7) return Err::Value;
    return Err::None;
  }
  double x;
  if (Err e = ToNumber(v, ctx, x); e != Err::None) return e;
  int k = ToInt(x);
  if (k >= 1 && k <= 7) {
    int first = (k + 5) % 7;
    mask[first] = true;
    mask[(first + 1) % 7] = true;
    return Err::None;
  }
  if (k >= 11 && k <= 17) {
    mask[k - 11] = true;
    return Err::None;
  }
  return Err::Num;
}

Err Holidays(EvalContext& ctx, Operand* a, int n, int i, std::vector<double>& out) {
  if (i >= n || a[i].IsMissing()) return Err::None;
  if (a[i].IsError()) return a[i].value.error();
  Grid g;
  if (!MakeGrid(ctx, a[i], g, true)) return Err::Value;
  for (int r = 0; r < g.rows; ++r) {
    for (int c = 0; c < g.cols; ++c) {
      const Value& v = g.At(r, c);
      double x = 0;
      if (v.IsBlank()) continue;
      if (v.IsError()) return v.error();
      if (v.IsNumber()) {
        x = v.number();
      } else if (v.IsText()) {
        if (!ParseNumberText(v.text(), ctx.dates(), x)) return Err::Value;
      } else {
        return Err::Value;
      }
      if (x < 0 || x >= kMaxSerial + 1) return Err::Num;
      out.push_back(std::floor(x));
    }
  }
  std::sort(out.begin(), out.end());
  return Err::None;
}

bool IsWorkday(double day, const bool mask[7], const std::vector<double>& hol,
               const DateSystem& ds) {
  if (mask[WeekdayFromSerial(day, ds)]) return false;
  return !std::binary_search(hol.begin(), hol.end(), day);
}

Operand NetworkDays(EvalContext& ctx, Operand* a, int n, bool intl) {
  DATE_ARG(0, s0);
  DATE_ARG(1, e0);
  bool mask[7];
  Operand none = Operand::OfMissing();
  if (Err e = ParseWeekend(ctx, intl && n > 2 ? a[2] : none, mask); e != Err::None)
    return MakeErr(e);
  std::vector<double> hol;
  if (Err e = Holidays(ctx, a, n, intl ? 3 : 2, hol); e != Err::None) return MakeErr(e);
  double s = std::floor(s0);
  double e = std::floor(e0);
  double sign = 1;
  if (s > e) {
    std::swap(s, e);
    sign = -1;
  }
  double count = 0;
  for (double d = s; d <= e; d += 1) count += IsWorkday(d, mask, hol, ctx.dates()) ? 1 : 0;
  return MakeNum(sign * count);
}
Operand FnNetworkDays(EvalContext& ctx, Operand* a, int n) {
  return NetworkDays(ctx, a, n, false);
}
Operand FnNetworkDaysIntl(EvalContext& ctx, Operand* a, int n) {
  return NetworkDays(ctx, a, n, true);
}

Operand Workday(EvalContext& ctx, Operand* a, int n, bool intl) {
  DATE_ARG(0, s0);
  NUM_ARG(1, k0);
  bool mask[7];
  Operand none = Operand::OfMissing();
  if (Err e = ParseWeekend(ctx, intl && n > 2 ? a[2] : none, mask); e != Err::None)
    return MakeErr(e);
  std::vector<double> hol;
  if (Err e = Holidays(ctx, a, n, intl ? 3 : 2, hol); e != Err::None) return MakeErr(e);
  double d = std::floor(s0);
  if (std::fabs(k0) > 1e7) return MakeErr(Err::Num);
  long long k = ToInt64(k0);
  int step = k >= 0 ? 1 : -1;
  long long left = k >= 0 ? k : -k;
  while (left > 0) {
    d += step;
    if (d < 0 || d > kMaxSerial) return MakeErr(Err::Num);
    if (IsWorkday(d, mask, hol, ctx.dates())) --left;
  }
  return MakeNum(d);
}
Operand FnWorkday(EvalContext& ctx, Operand* a, int n) {
  return Workday(ctx, a, n, false);
}
Operand FnWorkdayIntl(EvalContext& ctx, Operand* a, int n) {
  return Workday(ctx, a, n, true);
}

Operand FnYearFrac(EvalContext& ctx, Operand* a, int n) {
  DATE_ARG(0, s0);
  DATE_ARG(1, e0);
  double b0;
  if (Err e = ArgNumberOr(ctx, a, n, 2, 0, b0); e != Err::None) return MakeErr(e);
  int basis = ToInt(b0);
  if (basis < 0 || basis > 4) return MakeErr(Err::Num);
  double s = std::floor(s0);
  double e = std::floor(e0);
  if (s > e) std::swap(s, e);
  int y1;
  int m1;
  int d1;
  int y2;
  int m2;
  int d2;
  DateFromSerial(s, ctx.dates(), y1, m1, d1);
  DateFromSerial(e, ctx.dates(), y2, m2, d2);
  switch (basis) {
    case 0: {
      bool f1 = LastOfFeb(y1, m1, d1);
      bool f2 = LastOfFeb(y2, m2, d2);
      if (d1 == 31 && d2 == 31) {
        d1 = 30;
        d2 = 30;
      } else if (d1 == 31) {
        d1 = 30;
      } else if (d1 == 30 && d2 == 31) {
        d2 = 30;
      } else if (f1 && f2) {
        d1 = 30;
        d2 = 30;
      } else if (f1) {
        d1 = 30;
      }
      return MakeNum(((y2 - y1) * 360.0 + (m2 - m1) * 30.0 + (d2 - d1)) / 360.0);
    }
    case 1: {
      bool within = y1 == y2 || (y2 == y1 + 1 && (m1 > m2 || (m1 == m2 && d1 >= d2)));
      double denom;
      if (within) {
        bool leap = false;
        if (y1 == y2) {
          leap = IsLeapYear(y1);
        } else {
          if (IsLeapYear(y1) && (m1 < 2 || (m1 == 2 && d1 <= 29))) leap = true;
          if (IsLeapYear(y2) && (m2 > 2 || (m2 == 2 && d2 == 29))) leap = true;
        }
        denom = leap ? 366 : 365;
      } else {
        double first;
        double last;
        SerialFromDate(y1, 1, 1, ctx.dates(), first);
        if (!SerialFromDate(y2 + 1, 1, 1, ctx.dates(), last)) last = first + 365.25 * (y2 - y1 + 1);
        denom = (last - first) / (y2 - y1 + 1);
      }
      return MakeNum((e - s) / denom);
    }
    case 2:
      return MakeNum((e - s) / 360.0);
    case 3:
      return MakeNum((e - s) / 365.0);
    default:
      return MakeNum(Days360(y1, m1, d1, y2, m2, d2, true) / 360.0);
  }
}

}

void RegisterDate(std::vector<FuncInfo>& out) {
  const FuncInfo list[] = {
      {"DATE", 3, 3, "VVV", 1, kFnLift, FnDate},
      {"DATEVALUE", 1, 1, "V", 1, kFnLift, FnDateValue},
      {"TIMEVALUE", 1, 1, "V", 1, kFnLift, FnTimeValue},
      {"YEAR", 1, 1, "V", 1, kFnLift, FnDatePart<0>},
      {"MONTH", 1, 1, "V", 1, kFnLift, FnDatePart<1>},
      {"DAY", 1, 1, "V", 1, kFnLift, FnDatePart<2>},
      {"HOUR", 1, 1, "V", 1, kFnLift, FnTimePart<0>},
      {"MINUTE", 1, 1, "V", 1, kFnLift, FnTimePart<1>},
      {"SECOND", 1, 1, "V", 1, kFnLift, FnTimePart<2>},
      {"TIME", 3, 3, "VVV", 1, kFnLift, FnTime},
      {"NOW", 0, 0, "", 1, kFnVolatile, FnNow},
      {"TODAY", 0, 0, "", 1, kFnVolatile, FnToday},
      {"WEEKDAY", 1, 2, "VV", 1, kFnLift, FnWeekday},
      {"WEEKNUM", 1, 2, "VV", 1, kFnLift, FnWeekNum},
      {"ISOWEEKNUM", 1, 1, "V", 1, kFnLift | kFnXlfn, FnIsoWeekNum},
      {"EDATE", 2, 2, "VV", 1, kFnLift, FnEdate},
      {"EOMONTH", 2, 2, "VV", 1, kFnLift, FnEomonth},
      {"DAYS", 2, 2, "VV", 1, kFnLift | kFnXlfn, FnDays},
      {"DAYS360", 2, 3, "VVV", 1, kFnLift, FnDays360},
      {"DATEDIF", 3, 3, "VVV", 1, kFnLift, FnDateDif},
      {"NETWORKDAYS", 2, 3, "VVR", 1, 0, FnNetworkDays},
      {"NETWORKDAYS.INTL", 2, 4, "VVVR", 1, kFnXlfn, FnNetworkDaysIntl},
      {"WORKDAY", 2, 3, "VVR", 1, 0, FnWorkday},
      {"WORKDAY.INTL", 2, 4, "VVVR", 1, kFnXlfn, FnWorkdayIntl},
      {"YEARFRAC", 2, 3, "VVV", 1, kFnLift, FnYearFrac},
  };
  out.insert(out.end(), std::begin(list), std::end(list));
}

}
