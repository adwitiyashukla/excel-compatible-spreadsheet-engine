#include "xlsx.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <unordered_map>

#include "eval.h"
#include "formula.h"
#include "xml.h"
#include "zip.h"

namespace sheet {

namespace {

constexpr size_t kMaxFailures = 50;

std::string Unescape(std::string_view s) {
  if (s.find("_x") == std::string_view::npos) return std::string(s);
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '_' && i + 6 < s.size() && s[i + 1] == 'x' && s[i + 6] == '_') {
      bool hex = true;
      for (size_t k = 2; k < 6; ++k) {
        char c = s[i + k];
        hex = hex && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'));
      }
      if (hex) {
        uint32_t cp = static_cast<uint32_t>(
            std::strtoul(std::string(s.substr(i + 2, 4)).c_str(), nullptr, 16));
        out += Utf8Encode(cp);
        i += 6;
        continue;
      }
    }
    out.push_back(s[i]);
  }
  return out;
}

std::string JoinPath(const std::string& base_dir, std::string target) {
  if (!target.empty() && target[0] == '/') return target.substr(1);
  std::string dir = base_dir;
  while (target.compare(0, 3, "../") == 0) {
    target = target.substr(3);
    if (!dir.empty()) dir.pop_back();
    size_t slash = dir.rfind('/');
    dir = slash == std::string::npos ? "" : dir.substr(0, slash + 1);
  }
  return dir + target;
}

bool EndsWith(const std::string& s, std::string_view tail) {
  return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

std::map<std::string, std::string> ReadRels(const ZipReader& zip, const std::string& part,
                                            std::map<std::string, std::string>* types) {
  std::map<std::string, std::string> out;
  size_t slash = part.rfind('/');
  std::string dir = slash == std::string::npos ? "" : part.substr(0, slash + 1);
  std::string file = slash == std::string::npos ? part : part.substr(slash + 1);
  std::string rels_path = dir + "_rels/" + file + ".rels";
  std::string doc;
  if (!zip.Read(rels_path, doc, nullptr)) return out;
  XmlReader x(doc);
  while (true) {
    XmlReader::Event e = x.Next();
    if (e == XmlReader::kEof || e == XmlReader::kError) break;
    if (e != XmlReader::kStart || x.Name() != "Relationship") continue;
    std::string id;
    std::string target;
    std::string type;
    std::string mode;
    x.Attr("Id", id);
    x.Attr("Target", target);
    x.Attr("Type", type);
    x.Attr("TargetMode", mode);
    if (mode == "External") continue;
    out[id] = JoinPath(dir, target);
    if (types) (*types)[id] = type;
  }
  return out;
}

bool ParseIsoDate(std::string_view s, const DateSystem& ds, double& out) {
  int y = 0;
  int m = 0;
  int d = 0;
  int hh = 0;
  int mm = 0;
  double ss = 0;
  if (s.size() < 10) return false;
  y = std::atoi(std::string(s.substr(0, 4)).c_str());
  m = std::atoi(std::string(s.substr(5, 2)).c_str());
  d = std::atoi(std::string(s.substr(8, 2)).c_str());
  if (s.size() >= 16 && s[10] == 'T') {
    hh = std::atoi(std::string(s.substr(11, 2)).c_str());
    mm = std::atoi(std::string(s.substr(14, 2)).c_str());
    if (s.size() >= 19) ss = std::strtod(std::string(s.substr(17)).c_str(), nullptr);
  }
  double serial;
  if (!SerialFromDate(y, m, d, ds, serial)) return false;
  out = serial + (hh * 3600 + mm * 60 + ss) / 86400.0;
  return true;
}

void NoteFailure(LoadReport& rep, const Workbook& wb, int sheet, int row, int col,
                 const std::string& text, const std::string& why) {
  ++rep.failed;
  if (rep.failures.size() < kMaxFailures) {
    rep.failures.push_back(wb.SheetName(sheet) + "!" + CellName(row, col) + ": " + text + "  (" +
                           why + ")");
  }
}

struct SheetLoader {
  SheetLoader(Workbook& w, LoadReport& r, int s, const std::vector<Value>& str)
      : wb(w), rep(r), sheet(s), strings(str), tables(w.Tables(s)) {}

  Workbook& wb;
  LoadReport& rep;
  int sheet;
  const std::vector<Value>& strings;

  struct Shared {
    ProgramRef prog;
  };
  std::unordered_map<std::string, Shared> shared;
  std::vector<Area> arrays;
  TableResolver tables;
  std::vector<int> hidden_rows;
  std::vector<Area> filters;
  bool pending_filter = false;
  Area filter_area;

  void ApplyHiddenRows() {
    for (const Table& t : wb.tables()) {
      if (t.filter && t.area.sheet == sheet) filters.push_back(t.area);
    }
    for (int r : hidden_rows) {
      bool filtered = false;
      for (const Area& f : filters) filtered = filtered || (r > f.r1 && r <= f.r2);
      wb.SetRowHidden(sheet, r, true, filtered);
    }
  }

  bool InArray(int row, int col) const {
    for (const Area& a : arrays) {
      if (a.Contains(row, col)) return true;
    }
    return false;
  }

  void Finish(int row, int col, const std::string& type, const std::string& value, bool has_value,
              bool has_f, const std::string& ftype, const std::string& ftext,
              const std::string& fref, const std::string& si) {
    Value v;
    if (type == "s") {
      size_t idx = static_cast<size_t>(std::strtoul(value.c_str(), nullptr, 10));
      if (has_value && idx < strings.size()) v = strings[idx];
    } else if (type == "str" || type == "inlineStr") {
      if (has_value || type == "inlineStr") v = Value::Text(Unescape(value));
    } else if (type == "b") {
      if (has_value) v = Value::Bool(value == "1" || value == "true");
    } else if (type == "e") {
      Err e;
      v = ParseErrorText(value, e) ? Value::Error(e) : Value::Error(Err::Value);
    } else if (type == "d") {
      double serial;
      if (has_value && ParseIsoDate(value, wb.dates(), serial)) v = Value::Number(serial);
    } else if (has_value && !value.empty()) {
      v = Value::Number(std::strtod(value.c_str(), nullptr));
    }
    ++rep.cells;
    if (!has_f) {
      if (InArray(row, col)) {
        wb.SetCachedValue(sheet, row, col, v);
        rep.cached.push_back({CellPos{sheet, row, col}, v});
        return;
      }
      if (!v.IsBlank()) wb.SetValue(sheet, row, col, v);
      return;
    }
    ++rep.formulas;
    if (ftype == "array" && !fref.empty()) {
      int r1;
      int c1;
      int r2;
      int c2;
      std::string err;
      if (ParseAreaName(fref, r1, c1, r2, c2) &&
          wb.SetArrayFormula(sheet, Area{sheet, r1, c1, r2, c2}, ftext, &err)) {
        arrays.push_back(Area{sheet, r1, c1, r2, c2});
        ++rep.arrays;
        wb.SetCachedValue(sheet, row, col, v);
        rep.cached.push_back({CellPos{sheet, row, col}, v});
        return;
      }
      NoteFailure(rep, wb, sheet, row, col, ftext, err);
      if (!v.IsBlank()) wb.SetValue(sheet, row, col, v);
      return;
    }
    if (ftype == "dataTable") {
      NoteFailure(rep, wb, sheet, row, col, "data table", "unsupported");
      if (!v.IsBlank()) wb.SetValue(sheet, row, col, v);
      return;
    }
    ProgramRef prog;
    std::string why;
    if (ftype == "shared" && ftext.empty()) {
      auto it = shared.find(si);
      if (it != shared.end() && it->second.prog) {
        prog = it->second.prog;
        ++rep.shared;
      } else {
        why = "shared formula master missing";
      }
    } else {
      ParseError pe;
      prog = CompileFormula(ftext, row, col, wb.Resolver(), false, &pe, &tables);
      if (!prog) why = pe.message;
      if (ftype == "shared") shared[si] = Shared{prog};
    }
    if (prog && wb.SetProgram(sheet, row, col, prog)) {
      wb.SetCachedValue(sheet, row, col, v);
      rep.cached.push_back({CellPos{sheet, row, col}, v});
      return;
    }
    NoteFailure(rep, wb, sheet, row, col, ftext.empty() ? "(shared " + si + ")" : ftext, why);
    if (!v.IsBlank()) wb.SetValue(sheet, row, col, v);
  }

  bool Load(const std::string& doc) {
    XmlReader x(doc);
    int cur_row = -1;
    int cur_col = -1;
    while (true) {
      XmlReader::Event e = x.Next();
      if (e == XmlReader::kEof) return true;
      if (e == XmlReader::kError) return false;
      if (e != XmlReader::kStart) continue;
      std::string_view name = x.Name();
      if (name == "row") {
        std::string r;
        if (x.Attr("r", r)) {
          cur_row = std::atoi(r.c_str()) - 1;
        } else {
          ++cur_row;
        }
        cur_col = -1;
        std::string hidden;
        if (x.Attr("hidden", hidden) && (hidden == "1" || hidden == "true"))
          hidden_rows.push_back(cur_row);
        continue;
      }
      if (name == "autoFilter") {
        std::string ref;
        int r1 = 0;
        int c1 = 0;
        int r2 = 0;
        int c2 = 0;
        pending_filter = x.Attr("ref", ref) && ParseAreaName(ref, r1, c1, r2, c2);
        if (pending_filter) filter_area = Area{sheet, r1, c1, r2, c2};
        continue;
      }
      if (name == "filterColumn" && pending_filter) {
        filters.push_back(filter_area);
        pending_filter = false;
        continue;
      }
      if (name != "c") continue;
      std::string ref;
      int row = cur_row;
      int col = cur_col + 1;
      if (x.Attr("r", ref)) {
        int rr;
        int cc;
        if (ParseCellName(ref, rr, cc)) {
          row = rr;
          col = cc;
        }
      }
      cur_col = col;
      if (row < 0) row = 0;
      std::string type;
      x.Attr("t", type);
      std::string value;
      bool has_value = false;
      bool has_f = false;
      std::string ftype;
      std::string ftext;
      std::string fref;
      std::string si;
      int depth = x.depth();
      std::string* sink = nullptr;
      bool in_is = false;
      bool in_rph = false;
      while (true) {
        XmlReader::Event ce = x.Next();
        if (ce == XmlReader::kEof || ce == XmlReader::kError) return false;
        if (ce == XmlReader::kEnd && x.depth() < depth) break;
        if (ce == XmlReader::kStart) {
          std::string_view cn = x.Name();
          if (cn == "v") {
            sink = &value;
            has_value = true;
          } else if (cn == "f") {
            has_f = true;
            x.Attr("t", ftype);
            x.Attr("ref", fref);
            x.Attr("si", si);
            sink = &ftext;
          } else if (cn == "is") {
            in_is = true;
          } else if (cn == "rPh") {
            in_rph = true;
          } else if (cn == "t" && in_is && !in_rph) {
            sink = &value;
            has_value = true;
          }
        } else if (ce == XmlReader::kEnd) {
          std::string_view cn = x.Name();
          if (cn == "v" || cn == "f" || cn == "t") sink = nullptr;
          if (cn == "rPh") in_rph = false;
          if (cn == "is") in_is = false;
        } else if (ce == XmlReader::kText && sink) {
          *sink += x.Text();
        }
      }
      Finish(row, col, type, value, has_value, has_f, ftype, ftext, fref, si);
    }
  }
};

void LoadTable(Workbook& wb, int sheet, const std::string& doc) {
  XmlReader x(doc);
  Table t;
  bool have = false;
  while (true) {
    XmlReader::Event e = x.Next();
    if (e == XmlReader::kEof || e == XmlReader::kError) break;
    if (e != XmlReader::kStart) continue;
    if (x.Name() == "table" && !have) {
      std::string ref;
      std::string v;
      if (!x.Attr("displayName", t.name)) x.Attr("name", t.name);
      x.Attr("ref", ref);
      int r1;
      int c1;
      int r2;
      int c2;
      if (!ParseAreaName(ref, r1, c1, r2, c2)) return;
      t.area = Area{sheet, r1, c1, r2, c2};
      t.header_rows = x.Attr("headerRowCount", v) ? std::atoi(v.c_str()) : 1;
      t.totals_rows = x.Attr("totalsRowCount", v) ? std::atoi(v.c_str()) : 0;
      have = true;
    } else if (x.Name() == "filterColumn" && have) {
      t.filter = true;
    } else if (x.Name() == "tableColumn" && have) {
      std::string name;
      x.Attr("name", name);
      t.columns.push_back(Unescape(name));
    }
  }
  if (have) wb.AddTable(t, nullptr);
}

std::vector<Value> ReadSharedStrings(const std::string& doc) {
  std::vector<Value> out;
  XmlReader x(doc);
  std::string cur;
  bool in_si = false;
  bool in_t = false;
  int rph = 0;
  while (true) {
    XmlReader::Event e = x.Next();
    if (e == XmlReader::kEof || e == XmlReader::kError) break;
    if (e == XmlReader::kStart) {
      if (x.Name() == "si") {
        in_si = true;
        cur.clear();
      } else if (x.Name() == "t" && in_si) {
        in_t = true;
      } else if (x.Name() == "rPh") {
        ++rph;
      }
    } else if (e == XmlReader::kEnd) {
      if (x.Name() == "si") {
        out.push_back(Value::Text(Unescape(cur)));
        in_si = false;
      } else if (x.Name() == "t") {
        in_t = false;
      } else if (x.Name() == "rPh") {
        --rph;
      }
    } else if (e == XmlReader::kText && in_t && rph == 0) {
      cur += x.Text();
    }
  }
  return out;
}

}

bool LoadXlsx(const std::string& path, Workbook& wb, LoadReport& rep, std::string* error) {
  auto t0 = std::chrono::steady_clock::now();
  std::string data;
  if (!ReadFile(path, data)) {
    if (error) *error = "cannot read " + path;
    return false;
  }
  ZipReader zip;
  if (!zip.Open(std::move(data), error)) return false;
  std::string wb_part = "xl/workbook.xml";
  std::map<std::string, std::string> root_types;
  auto root = ReadRels(zip, "", &root_types);
  for (const auto& kv : root) {
    if (EndsWith(root_types[kv.first], "/officeDocument")) wb_part = kv.second;
  }
  std::string doc;
  if (!zip.Read(wb_part, doc, error)) return false;
  std::map<std::string, std::string> types;
  auto rels = ReadRels(zip, wb_part, &types);
  struct SheetRef {
    std::string name;
    std::string part;
  };
  std::vector<SheetRef> sheets;
  struct NameDef {
    std::string name;
    std::string text;
    int scope;
  };
  std::vector<NameDef> names;
  {
    XmlReader x(doc);
    std::string* sink = nullptr;
    NameDef cur;
    while (true) {
      XmlReader::Event e = x.Next();
      if (e == XmlReader::kEof || e == XmlReader::kError) break;
      if (e == XmlReader::kStart) {
        if (x.Name() == "sheet") {
          SheetRef s;
          x.Attr("name", s.name);
          std::string rid;
          x.Attr("id", rid);
          s.part = rels.count(rid) ? rels[rid] : "";
          sheets.push_back(s);
        } else if (x.Name() == "workbookPr") {
          std::string v;
          if (x.Attr("date1904", v)) wb.SetDate1904(v == "1" || v == "true");
        } else if (x.Name() == "calcPr") {
          std::string it;
          if (x.Attr("iterate", it) && (it == "1" || it == "true")) {
            std::string cnt = "100";
            std::string delta = "0.001";
            x.Attr("iterateCount", cnt);
            x.Attr("iterateDelta", delta);
            wb.SetIteration(true, std::atoi(cnt.c_str()), std::strtod(delta.c_str(), nullptr));
          }
        } else if (x.Name() == "definedName") {
          cur = NameDef();
          x.Attr("name", cur.name);
          std::string ls;
          cur.scope = x.Attr("localSheetId", ls) ? std::atoi(ls.c_str()) : -1;
          sink = &cur.text;
        }
      } else if (e == XmlReader::kEnd && x.Name() == "definedName") {
        names.push_back(cur);
        sink = nullptr;
      } else if (e == XmlReader::kText && sink) {
        *sink += x.Text();
      }
    }
  }
  std::vector<Value> strings;
  for (const auto& kv : rels) {
    if (EndsWith(types[kv.first], "/sharedStrings")) {
      std::string ss;
      if (zip.Read(kv.second, ss, error)) strings = ReadSharedStrings(ss);
    }
  }
  wb.SuspendDependencies();
  std::vector<int> ids;
  for (const SheetRef& s : sheets) {
    int id = wb.AddSheet(s.name);
    if (id < 0) id = wb.AddSheet("");
    ids.push_back(id);
  }
  for (size_t i = 0; i < sheets.size(); ++i) {
    if (sheets[i].part.empty()) continue;
    std::map<std::string, std::string> sheet_types;
    auto sheet_rels = ReadRels(zip, sheets[i].part, &sheet_types);
    for (const auto& kv : sheet_rels) {
      if (!EndsWith(sheet_types[kv.first], "/table")) continue;
      std::string tdoc;
      if (zip.Read(kv.second, tdoc, nullptr)) LoadTable(wb, ids[i], tdoc);
    }
  }
  for (const NameDef& n : names) {
    int scope = n.scope >= 0 && n.scope < static_cast<int>(ids.size())
                    ? ids[static_cast<size_t>(n.scope)]
                    : -1;
    wb.DefineName(n.name, n.text, scope, nullptr);
  }
  for (size_t i = 0; i < sheets.size(); ++i) {
    if (sheets[i].part.empty()) continue;
    std::string sdoc;
    if (!zip.Read(sheets[i].part, sdoc, error)) return false;
    SheetLoader loader(wb, rep, ids[i], strings);
    if (!loader.Load(sdoc)) {
      if (error) *error = "malformed sheet xml in " + sheets[i].part;
      return false;
    }
    loader.ApplyHiddenRows();
    ++rep.sheets;
  }
  rep.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return true;
}

namespace {

double FodsDate(const std::string& v, const DateSystem& ds) {
  double serial = 0;
  ParseIsoDate(v, ds, serial);
  return serial;
}

double FodsTime(const std::string& v) {
  double h = 0;
  double m = 0;
  double s = 0;
  size_t t = v.find('T');
  if (t == std::string::npos) return 0;
  size_t i = t + 1;
  double days = 0;
  if (v.size() > 1 && v[0] == 'P') {
    size_t dpos = v.find('D');
    if (dpos != std::string::npos && dpos < t)
      days = std::strtod(v.substr(1, dpos - 1).c_str(), nullptr);
  }
  while (i < v.size()) {
    size_t j = i;
    while (j < v.size() && ((v[j] >= '0' && v[j] <= '9') || v[j] == '.')) ++j;
    double num = std::strtod(v.substr(i, j - i).c_str(), nullptr);
    if (j >= v.size()) break;
    if (v[j] == 'H') h = num;
    if (v[j] == 'M') m = num;
    if (v[j] == 'S') s = num;
    i = j + 1;
  }
  return days + (h * 3600 + m * 60 + s) / 86400.0;
}

Err LibreError(const std::string& text) {
  Err e;
  if (ParseErrorText(text, e)) return e;
  if (text.compare(0, 4, "Err:") == 0) {
    int code = std::atoi(text.c_str() + 4);
    switch (code) {
      case 502:
      case 503:
      case 523:
        return Err::Num;
      case 532:
        return Err::Div0;
      case 522:
        return Err::Ref;
      case 525:
        return Err::Name;
      default:
        return Err::Value;
    }
  }
  return Err::Value;
}

}

bool LoadFods(const std::string& path, Workbook& wb, LoadReport& rep, std::string* error) {
  auto t0 = std::chrono::steady_clock::now();
  std::string doc;
  if (!ReadFile(path, doc)) {
    if (error) *error = "cannot read " + path;
    return false;
  }
  std::vector<std::string> names;
  struct FodsName {
    std::string name;
    std::string text;
    int table = -1;
  };
  std::vector<FodsName> fods_names;
  {
    XmlReader x(doc);
    int open = -1;
    while (true) {
      XmlReader::Event e = x.Next();
      if (e == XmlReader::kEof || e == XmlReader::kError) break;
      if (e == XmlReader::kEnd && x.Name() == "table") open = -1;
      if (e != XmlReader::kStart) continue;
      if (x.Name() == "table") {
        std::string n;
        x.Attr("name", n);
        names.push_back(n);
        open = static_cast<int>(names.size()) - 1;
      } else if (x.Name() == "named-range" || x.Name() == "named-expression") {
        FodsName n;
        x.Attr("name", n.name);
        if (x.Name() == "named-range") {
          std::string body;
          x.Attr("cell-range-address", body);
          n.text = "[" + body + "]";
        } else {
          x.Attr("expression", n.text);
        }
        n.table = open;
        fods_names.push_back(n);
      }
    }
  }
  wb.SuspendDependencies();
  std::vector<int> ids;
  for (const std::string& n : names) {
    int id = wb.AddSheet(n);
    if (id < 0) id = wb.AddSheet("");
    ids.push_back(id);
  }
  for (const FodsName& n : fods_names) {
    bool ok = false;
    std::string text = TranslateOpenFormula(n.text, ok);
    int scope = n.table >= 0 ? ids[static_cast<size_t>(n.table)] : -1;
    if (ok) wb.DefineName(n.name, text, scope, nullptr);
  }
  XmlReader x(doc);
  int table = -1;
  bool in_table = false;
  std::vector<Area> fods_arrays;
  int row = 0;
  int col = 0;
  struct Pending {
    int col;
    int repeat;
    std::string formula;
    Value value;
    int mrows;
    int mcols;
  };
  std::vector<Pending> row_cells;
  int row_repeat = 1;
  auto flush_row = [&]() {
    int reps = std::min(row_repeat, row_cells.empty() ? row_repeat : 64);
    for (int rr = 0; rr < reps; ++rr) {
      int r = row + rr;
      if (r >= kMaxRows) break;
      for (const Pending& p : row_cells) {
        for (int k = 0; k < p.repeat && p.col + k < kMaxCols; ++k) {
          int c = p.col + k;
          ++rep.cells;
          int sid = ids[static_cast<size_t>(table)];
          if (p.formula.empty()) {
            bool covered = false;
            for (const Area& a : fods_arrays)
              covered = covered || (a.sheet == sid && a.Contains(r, c));
            if (covered) {
              wb.SetCachedValue(sid, r, c, p.value);
              rep.cached.push_back({CellPos{sid, r, c}, p.value});
            } else if (!p.value.IsBlank()) {
              wb.SetValue(sid, r, c, p.value);
            }
            continue;
          }
          ++rep.formulas;
          bool ok = false;
          std::string text = TranslateOpenFormula(p.formula, ok);
          int s = ids[static_cast<size_t>(table)];
          std::string err;
          bool set = false;
          if (ok) {
            if (p.mrows > 0 && p.mcols > 0) {
              Area area{s, r, c, r + p.mrows - 1, c + p.mcols - 1};
              set = wb.SetArrayFormula(s, area, text, &err);
              if (set) {
                ++rep.arrays;
                fods_arrays.push_back(area);
              }
            } else {
              set = wb.SetFormula(s, r, c, text, &err);
            }
          } else {
            err = "untranslatable";
          }
          if (set) {
            wb.SetCachedValue(s, r, c, p.value);
            rep.cached.push_back({CellPos{s, r, c}, p.value});
          } else {
            NoteFailure(rep, wb, s, r, c, p.formula, err);
            if (!p.value.IsBlank()) wb.SetValue(s, r, c, p.value);
          }
        }
      }
    }
    row += row_repeat;
    row_cells.clear();
    row_repeat = 1;
  };
  while (true) {
    XmlReader::Event e = x.Next();
    if (e == XmlReader::kEof) break;
    if (e == XmlReader::kError) {
      if (error) *error = "malformed xml";
      return false;
    }
    if (e == XmlReader::kEnd && x.Name() == "table-row" && table >= 0) {
      flush_row();
      continue;
    }
    if (e == XmlReader::kEnd && x.Name() == "table") {
      in_table = false;
      continue;
    }
    if (e != XmlReader::kStart) continue;
    std::string_view name = x.Name();
    if (name == "table") {
      ++table;
      row = 0;
      in_table = true;
      continue;
    }
    if (table < 0 || !in_table) continue;
    if (name == "table-row") {
      std::string rr;
      row_repeat = x.Attr("number-rows-repeated", rr) ? std::max(1, std::atoi(rr.c_str())) : 1;
      std::string vis;
      if (x.Attr("visibility", vis) && (vis == "collapse" || vis == "filter") &&
          row_repeat <= 4096) {
        for (int k = 0; k < row_repeat && row + k < kMaxRows; ++k) {
          wb.SetRowHidden(ids[static_cast<size_t>(table)], row + k, true, vis == "filter");
        }
      }
      col = 0;
      row_cells.clear();
      continue;
    }
    if (name != "table-cell" && name != "covered-table-cell") continue;
    std::string cr;
    int repeat = x.Attr("number-columns-repeated", cr) ? std::max(1, std::atoi(cr.c_str())) : 1;
    std::string formula;
    x.Attr("formula", formula);
    std::string type;
    std::string calc_type;
    x.QAttr("office:value-type", type);
    x.QAttr("calcext:value-type", calc_type);
    std::string mr;
    std::string mc;
    int mrows = x.Attr("number-matrix-rows-spanned", mr) ? std::atoi(mr.c_str()) : 0;
    int mcols = x.Attr("number-matrix-columns-spanned", mc) ? std::atoi(mc.c_str()) : 0;
    Value v;
    std::string date_value;
    std::string time_value;
    bool has_date = x.Attr("date-value", date_value);
    bool has_time = x.Attr("time-value", time_value);
    bool has_string_attr = x.Has("string-value");
    std::string string_attr;
    x.Attr("string-value", string_attr);
    std::string bool_attr;
    bool has_bool = x.QAttr("office:boolean-value", bool_attr);
    std::string num_attr;
    bool has_num = x.QAttr("office:value", num_attr);
    std::string text;
    int depth = x.depth();
    int paragraphs = 0;
    int p_depth = -1;
    bool soft_space = false;
    auto end_paragraph = [&]() {
      soft_space = false;
      p_depth = -1;
    };
    while (true) {
      XmlReader::Event ce = x.Next();
      if (ce == XmlReader::kEof || ce == XmlReader::kError) break;
      if (ce == XmlReader::kEnd && p_depth >= 0 && x.depth() < p_depth) end_paragraph();
      if (ce == XmlReader::kEnd && x.depth() < depth) break;
      if (ce == XmlReader::kStart) {
        std::string_view cn = x.Name();
        if (cn == "p" && p_depth < 0) {
          if (paragraphs++ > 0) text.push_back('\n');
          p_depth = x.depth();
          soft_space = true;
        } else if (cn == "s" && p_depth >= 0) {
          std::string c;
          int count = x.Attr("c", c) ? std::max(1, std::atoi(c.c_str())) : 1;
          text.append(static_cast<size_t>(count), ' ');
          soft_space = false;
        } else if (cn == "tab" && p_depth >= 0) {
          text.push_back('\t');
          soft_space = false;
        } else if (cn == "line-break" && p_depth >= 0) {
          text.push_back('\n');
          soft_space = true;
        } else if (cn == "annotation") {
          x.Skip();
        }
      } else if (ce == XmlReader::kText && p_depth >= 0) {
        for (char ch : x.Text()) {
          if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
            if (!soft_space) text.push_back(' ');
            soft_space = true;
          } else {
            text.push_back(ch);
            soft_space = false;
          }
        }
      }
    }
    if (type == "float" || type == "percentage" || type == "currency") {
      if (has_num) v = Value::Number(std::strtod(num_attr.c_str(), nullptr));
    } else if (type == "date") {
      if (has_date) v = Value::Number(FodsDate(date_value, wb.dates()));
    } else if (type == "time") {
      if (has_time) v = Value::Number(FodsTime(time_value));
    } else if (type == "boolean") {
      if (has_bool) v = Value::Bool(bool_attr == "true" || bool_attr == "1");
    } else if (type == "string") {
      v = Value::Text(has_string_attr ? string_attr : text);
    }
    if (calc_type == "error") v = Value::Error(LibreError(text));
    row_cells.push_back(Pending{col, repeat, formula, v, mrows, mcols});
    col += repeat;
  }
  for (size_t i = 0; i < ids.size(); ++i) ++rep.sheets;
  rep.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return true;
}

bool LoadWorkbook(const std::string& path, Workbook& wb, LoadReport& rep, std::string* error) {
  std::string lower = ToLower(path);
  if (lower.size() >= 5 && lower.compare(lower.size() - 5, 5, ".fods") == 0) {
    return LoadFods(path, wb, rep, error);
  }
  return LoadXlsx(path, wb, rep, error);
}

namespace {

std::string NumberText(double d) {
  if (d == 0) return "0";
  return FormatShortest(d);
}

std::string CellXml(const std::string& ref, const Value& v, const std::string& formula,
                    const std::string& array_ref, std::unordered_map<std::string, size_t>& sst,
                    std::vector<std::string>& sst_list) {
  std::string out = "<c r=\"" + ref + "\"";
  std::string f;
  if (!formula.empty()) {
    if (!array_ref.empty()) {
      f = "<f t=\"array\" ref=\"" + array_ref + "\">" + XmlEscape(formula) + "</f>";
    } else {
      f = "<f>" + XmlEscape(formula) + "</f>";
    }
  }
  bool has_formula = !formula.empty();
  switch (v.kind()) {
    case Kind::Number:
      out += ">" + f + "<v>" + NumberText(v.number()) + "</v></c>";
      break;
    case Kind::Bool:
      out += " t=\"b\">" + f + "<v>" + (v.boolean() ? "1" : "0") + "</v></c>";
      break;
    case Kind::Error:
      out += " t=\"e\">" + f + "<v>" + XmlEscape(ErrorText(v.error())) + "</v></c>";
      break;
    case Kind::Text:
      if (has_formula) {
        out += " t=\"str\">" + f + "<v>" + XmlEscape(v.text()) + "</v></c>";
      } else {
        std::string key(v.text());
        auto it = sst.find(key);
        size_t idx;
        if (it == sst.end()) {
          idx = sst_list.size();
          sst.emplace(key, idx);
          sst_list.push_back(key);
        } else {
          idx = it->second;
        }
        out += " t=\"s\"><v>" + std::to_string(idx) + "</v></c>";
      }
      break;
    default:
      out += ">" + f + "</c>";
      break;
  }
  return out;
}

}

bool SaveXlsx(const Workbook& wb, const std::string& path, std::string* error) {
  const char* kMain = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";
  const char* kRel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
  std::unordered_map<std::string, size_t> sst;
  std::vector<std::string> sst_list;
  ZipWriter zip;
  int n = wb.SheetCount();
  std::vector<std::string> sheet_docs;
  for (int s = 0; s < n; ++s) {
    struct Item {
      int row;
      int col;
      const Cell* cell;
    };
    std::vector<Item> items;
    wb.ForEachCell(s, [&](int r, int c, const Cell& cell) { items.push_back(Item{r, c, &cell}); });
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
      return a.row != b.row ? a.row < b.row : a.col < b.col;
    });
    std::string doc =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<worksheet xmlns=\"";
    doc += kMain;
    doc += "\" xmlns:r=\"";
    doc += kRel;
    doc += "\">";
    if (!items.empty()) {
      doc += "<dimension ref=\"A1:" + CellName(wb.MaxRow(s), wb.MaxCol(s)) + "\"/>";
    }
    doc += "<sheetData>";
    int open_row = -1;
    for (const Item& it : items) {
      if (it.row != open_row) {
        if (open_row >= 0) doc += "</row>";
        doc += "<row r=\"" + std::to_string(it.row + 1) + "\">";
        open_row = it.row;
      }
      std::string formula;
      std::string array_ref;
      if (it.cell->formula != 0) {
        Area g;
        if (wb.ArrayGroup(s, it.row, it.col, g)) {
          if (g.r1 == it.row && g.c1 == it.col) {
            formula = wb.FormulaText(s, it.row, it.col, true).substr(1);
            array_ref = CellName(g.r1, g.c1) +
                        (g.r1 == g.r2 && g.c1 == g.c2 ? "" : ":" + CellName(g.r2, g.c2));
          }
        } else {
          formula = wb.FormulaText(s, it.row, it.col, true).substr(1);
        }
      }
      doc += CellXml(CellName(it.row, it.col), it.cell->value, formula, array_ref, sst, sst_list);
    }
    if (open_row >= 0) doc += "</row>";
    doc += "</sheetData></worksheet>";
    sheet_docs.push_back(std::move(doc));
  }
  std::string ct = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n";
  ct += "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">";
  ct +=
      "<Default Extension=\"rels\" "
      "ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>";
  ct += "<Default Extension=\"xml\" ContentType=\"application/xml\"/>";
  ct +=
      "<Override PartName=\"/xl/workbook.xml\" "
      "ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/"
      ">";
  for (int s = 0; s < n; ++s) {
    ct += "<Override PartName=\"/xl/worksheets/sheet" + std::to_string(s + 1) +
          ".xml\" "
          "ContentType=\"application/"
          "vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>";
  }
  ct +=
      "<Override PartName=\"/xl/styles.xml\" "
      "ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>";
  ct +=
      "<Override PartName=\"/xl/sharedStrings.xml\" "
      "ContentType=\"application/"
      "vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml\"/>";
  ct += "</Types>";
  zip.Add("[Content_Types].xml", ct);
  std::string root = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n";
  root += "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
  root +=
      "<Relationship Id=\"rId1\" "
      "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" "
      "Target=\"xl/workbook.xml\"/>";
  root += "</Relationships>";
  zip.Add("_rels/.rels", root);
  std::string book =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<workbook xmlns=\"";
  book += kMain;
  book += "\" xmlns:r=\"";
  book += kRel;
  book += "\">";
  if (wb.dates().date1904) book += "<workbookPr date1904=\"1\"/>";
  book += "<sheets>";
  for (int s = 0; s < n; ++s) {
    book += "<sheet name=\"" + XmlEscape(wb.SheetName(s)) + "\" sheetId=\"" +
            std::to_string(s + 1) + "\" r:id=\"rId" + std::to_string(s + 1) + "\"/>";
  }
  book += "</sheets>";
  if (!wb.names().empty()) {
    book += "<definedNames>";
    for (const DefinedName& dn : wb.names()) {
      if (!dn.prog) continue;
      book += "<definedName name=\"" + XmlEscape(dn.name) + "\"";
      if (dn.scope >= 0) book += " localSheetId=\"" + std::to_string(dn.scope) + "\"";
      book += ">" + XmlEscape(PrintFormula(*dn.prog, 0, 0, wb.Namer(), true)) + "</definedName>";
    }
    book += "</definedNames>";
  }
  book += "<calcPr calcId=\"191029\" fullCalcOnLoad=\"1\"/></workbook>";
  zip.Add("xl/workbook.xml", book);
  std::string brels = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n";
  brels += "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
  for (int s = 0; s < n; ++s) {
    brels +=
        "<Relationship Id=\"rId" + std::to_string(s + 1) +
        "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" "
        "Target=\"worksheets/sheet" +
        std::to_string(s + 1) + ".xml\"/>";
  }
  brels += "<Relationship Id=\"rId" + std::to_string(n + 1) +
           "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" "
           "Target=\"styles.xml\"/>";
  brels += "<Relationship Id=\"rId" + std::to_string(n + 2) +
           "\" "
           "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
           "sharedStrings\" Target=\"sharedStrings.xml\"/>";
  brels += "</Relationships>";
  zip.Add("xl/_rels/workbook.xml.rels", brels);
  std::string styles =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<styleSheet xmlns=\"";
  styles += kMain;
  styles += "\"><fonts count=\"1\"><font><sz val=\"11\"/><name val=\"Calibri\"/></font></fonts>";
  styles +=
      "<fills count=\"2\"><fill><patternFill patternType=\"none\"/></fill><fill><patternFill "
      "patternType=\"gray125\"/></fill></fills>";
  styles +=
      "<borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders>";
  styles +=
      "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" "
      "borderId=\"0\"/></cellStyleXfs>";
  styles +=
      "<cellXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" "
      "xfId=\"0\"/></cellXfs>";
  styles +=
      "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" "
      "builtinId=\"0\"/></cellStyles></styleSheet>";
  zip.Add("xl/styles.xml", styles);
  for (int s = 0; s < n; ++s) {
    zip.Add("xl/worksheets/sheet" + std::to_string(s + 1) + ".xml",
            sheet_docs[static_cast<size_t>(s)]);
  }
  std::string sdoc = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<sst xmlns=\"";
  sdoc += kMain;
  sdoc += "\" count=\"" + std::to_string(sst_list.size()) + "\" uniqueCount=\"" +
          std::to_string(sst_list.size()) + "\">";
  for (const std::string& t : sst_list) {
    bool preserve =
        !t.empty() && (t.front() == ' ' || t.back() == ' ' || t.find('\n') != std::string::npos);
    sdoc += preserve ? "<si><t xml:space=\"preserve\">" : "<si><t>";
    sdoc += XmlEscape(t) + "</t></si>";
  }
  sdoc += "</sst>";
  zip.Add("xl/sharedStrings.xml", sdoc);
  if (!WriteFile(path, zip.Finish())) {
    if (error) *error = "cannot write " + path;
    return false;
  }
  return true;
}

std::string SheetCsv(const Workbook& wb, int sheet) {
  std::string out;
  int rows = wb.MaxRow(sheet) + 1;
  int cols = wb.MaxCol(sheet) + 1;
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      if (c > 0) out.push_back(',');
      Value v = wb.Get(sheet, r, c);
      std::string t;
      switch (v.kind()) {
        case Kind::Number:
          t = FormatGeneral(v.number());
          break;
        case Kind::Bool:
          t = v.boolean() ? "TRUE" : "FALSE";
          break;
        case Kind::Error:
          t = std::string(ErrorText(v.error()));
          break;
        case Kind::Text:
          t = std::string(v.text());
          break;
        default:
          break;
      }
      if (t.find_first_of(",\"\n\r") != std::string::npos) {
        std::string q = "\"";
        for (char ch : t) {
          if (ch == '"') q.push_back('"');
          q.push_back(ch);
        }
        q.push_back('"');
        t = q;
      }
      out += t;
    }
    out += "\n";
  }
  return out;
}

}
