#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "core.h"

namespace sheet {

struct DateSystem {
  bool date1904 = false;
  int current_year = 2026;
};

constexpr double kMaxSerial = 2958465.0;

int64_t DaysFromCivil(int y, int m, int d);
void CivilFromDays(int64_t z, int& y, int& m, int& d);
bool IsLeapYear(int y);
int DaysInMonth(int y, int m);

bool SerialFromDate(int y, int m, int d, const DateSystem& ds, double& serial);
bool DateFromSerial(double serial, const DateSystem& ds, int& y, int& m, int& d);
int WeekdayFromSerial(double serial, const DateSystem& ds);
void TimeFromSerial(double serial, int& h, int& mi, int& s);

bool ParseDateTime(std::string_view s, const DateSystem& ds, double& out);
bool ParseNumberText(std::string_view s, const DateSystem& ds, double& out);

bool FormatValue(const Value& v, std::string_view format, const DateSystem& ds, std::string& out);

}
