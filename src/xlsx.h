#pragma once

#include <string>
#include <utility>
#include <vector>

#include "core.h"
#include "workbook.h"

namespace sheet {

struct LoadReport {
  size_t sheets = 0;
  size_t cells = 0;
  size_t formulas = 0;
  size_t shared = 0;
  size_t arrays = 0;
  size_t failed = 0;
  std::vector<std::string> failures;
  std::vector<std::pair<CellPos, Value>> cached;
  double seconds = 0;
};

bool LoadXlsx(const std::string& path, Workbook& wb, LoadReport& rep, std::string* error);
bool LoadFods(const std::string& path, Workbook& wb, LoadReport& rep, std::string* error);
bool LoadWorkbook(const std::string& path, Workbook& wb, LoadReport& rep, std::string* error);
bool SaveXlsx(const Workbook& wb, const std::string& path, std::string* error);
std::string SheetCsv(const Workbook& wb, int sheet);

}
