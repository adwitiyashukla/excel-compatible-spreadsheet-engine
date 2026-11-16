#include <cstring>
#include <string>
#include <vector>

#include "eval.h"
#include "formula.h"

namespace sheet {

namespace {

constexpr int kAtom = 100;
constexpr int kUnionList = 1;

int Precedence(Op op) {
  switch (op) {
    case Op::Range:
      return 95;
    case Op::Intersect:
      return 90;
    case Op::Neg:
    case Op::Plus:
      return 80;
    case Op::Percent:
      return 70;
    case Op::Pow:
      return 60;
    case Op::Mul:
    case Op::Div:
      return 50;
    case Op::Add:
    case Op::Sub:
      return 40;
    case Op::Concat:
      return 30;
    case Op::Eq:
    case Op::Ne:
    case Op::Lt:
    case Op::Le:
    case Op::Gt:
    case Op::Ge:
      return 20;
    default:
      return kAtom;
  }
}

const char* OpText(Op op) {
  switch (op) {
    case Op::Add:
      return "+";
    case Op::Sub:
      return "-";
    case Op::Mul:
      return "*";
    case Op::Div:
      return "/";
    case Op::Pow:
      return "^";
    case Op::Concat:
      return "&";
    case Op::Eq:
      return "=";
    case Op::Ne:
      return "<>";
    case Op::Lt:
      return "<";
    case Op::Le:
      return "<=";
    case Op::Gt:
      return ">";
    case Op::Ge:
      return ">=";
    case Op::Range:
      return ":";
    case Op::Intersect:
      return " ";
    default:
      return "";
  }
}

struct Piece {
  std::string text;
  int prec = kAtom;
};

std::string Wrap(const Piece& p, int min_prec) {
  if (p.prec == kUnionList) return "(" + p.text + ")";
  if (p.prec < min_prec) return "(" + p.text + ")";
  return p.text;
}

std::string QuoteString(std::string_view s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"') out.push_back('"');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

std::string ScalarText(const Value& v) {
  switch (v.kind()) {
    case Kind::Number:
      return FormatShortest(v.number());
    case Kind::Text:
      return QuoteString(v.text());
    case Kind::Bool:
      return v.boolean() ? "TRUE" : "FALSE";
    case Kind::Error:
      return std::string(ErrorText(v.error()));
    default:
      return "";
  }
}

std::string CellText(int row, int col, bool row_abs, bool col_abs) {
  std::string out;
  if (col_abs) out.push_back('$');
  out += ColumnName(col);
  if (row_abs) out.push_back('$');
  out += std::to_string(row + 1);
  return out;
}

std::string RefText(const RefSpec& s, int base_row, int base_col, const SheetNamer& sheets) {
  Area a;
  RefSpec abs = s;
  long r1 = s.r1_abs ? s.r1 : static_cast<long>(base_row) + s.r1;
  long r2 = s.r2_abs ? s.r2 : static_cast<long>(base_row) + s.r2;
  long c1 = s.c1_abs ? s.c1 : static_cast<long>(base_col) + s.c1;
  long c2 = s.c2_abs ? s.c2 : static_cast<long>(base_col) + s.c2;
  std::string prefix;
  if (s.explicit_sheet && s.sheet >= 0 && sheets) {
    if (s.sheet2 >= 0) {
      std::string first = sheets(s.sheet);
      std::string last = sheets(s.sheet2);
      bool quote = QuoteSheetName(first) != first || QuoteSheetName(last) != last;
      prefix = quote ? QuoteSheetName(first + ":" + last) + "!" : first + ":" + last + "!";
    } else {
      prefix = QuoteSheetName(sheets(s.sheet)) + "!";
    }
  }
  if (!ResolveRef(abs, base_row, base_col, a)) return prefix + "#REF!";
  if (s.full_cols) {
    std::string out = prefix;
    if (s.c1_abs) out.push_back('$');
    out += ColumnName(static_cast<int>(c1));
    out.push_back(':');
    if (s.c2_abs) out.push_back('$');
    out += ColumnName(static_cast<int>(c2));
    return out;
  }
  if (s.full_rows) {
    std::string out = prefix;
    if (s.r1_abs) out.push_back('$');
    out += std::to_string(r1 + 1);
    out.push_back(':');
    if (s.r2_abs) out.push_back('$');
    out += std::to_string(r2 + 1);
    return out;
  }
  std::string out =
      prefix + CellText(static_cast<int>(r1), static_cast<int>(c1), s.r1_abs, s.c1_abs);
  if (s.area) {
    out.push_back(':');
    out += CellText(static_cast<int>(r2), static_cast<int>(c2), s.r2_abs, s.c2_abs);
  }
  return out;
}

void HashMix(uint64_t& h, uint64_t v) {
  h = Mix64(h ^ (v + 0x9E3779B97F4A7C15ULL + (h << 6)));
}

void HashValue(uint64_t& h, const Value& v) {
  HashMix(h, static_cast<uint64_t>(v.kind()));
  switch (v.kind()) {
    case Kind::Number: {
      double d = v.number();
      uint64_t bits;
      std::memcpy(&bits, &d, 8);
      HashMix(h, bits);
      break;
    }
    case Kind::Text:
      HashMix(h, HashBytes(v.text().data(), v.text().size()));
      break;
    case Kind::Bool:
      HashMix(h, v.boolean() ? 1 : 0);
      break;
    case Kind::Error:
      HashMix(h, static_cast<uint64_t>(v.error()));
      break;
    case Kind::Array:
      HashMix(h, (static_cast<uint64_t>(v.rows()) << 32) | v.cols());
      for (uint32_t r = 0; r < v.rows(); ++r) {
        for (uint32_t c = 0; c < v.cols(); ++c) HashValue(h, v.at(r, c));
      }
      break;
    case Kind::Blank:
      break;
  }
}

bool SameRef(const RefSpec& a, const RefSpec& b) {
  return a.r1 == b.r1 && a.c1 == b.c1 && a.r2 == b.r2 && a.c2 == b.c2 && a.sheet == b.sheet &&
         a.sheet2 == b.sheet2 && a.r1_abs == b.r1_abs && a.c1_abs == b.c1_abs &&
         a.r2_abs == b.r2_abs && a.c2_abs == b.c2_abs && a.full_cols == b.full_cols &&
         a.full_rows == b.full_rows && a.area == b.area && a.explicit_sheet == b.explicit_sheet;
}

}

std::string PrintFormula(const Program& p, int base_row, int base_col, const SheetNamer& sheets,
                         bool xlsx_prefixes) {
  std::vector<Piece> st;
  struct Frame {
    Op kind;
    int argc;
  };
  std::vector<Frame> frames;
  auto pop = [&st]() {
    Piece x = std::move(st.back());
    st.pop_back();
    return x;
  };
  for (size_t pc = 0; pc < p.code.size(); ++pc) {
    const Instr& in = p.code[pc];
    switch (in.op) {
      case Op::PushNum:
        st.push_back({FormatShortest(p.nums[in.a]), kAtom});
        break;
      case Op::PushStr:
        st.push_back({QuoteString(p.consts[in.a].text()), kAtom});
        break;
      case Op::PushArray: {
        const Value& arr = p.consts[in.a];
        std::string out = "{";
        for (uint32_t r = 0; r < arr.rows(); ++r) {
          if (r > 0) out.push_back(';');
          for (uint32_t c = 0; c < arr.cols(); ++c) {
            if (c > 0) out.push_back(',');
            out += ScalarText(arr.at(r, c));
          }
        }
        out.push_back('}');
        st.push_back({out, kAtom});
        break;
      }
      case Op::PushBool:
        st.push_back({in.a ? "TRUE" : "FALSE", kAtom});
        break;
      case Op::PushErr:
        st.push_back({std::string(ErrorText(static_cast<Err>(in.a))), kAtom});
        break;
      case Op::PushMissing:
        st.push_back({"", kAtom});
        break;
      case Op::PushRef:
        st.push_back({RefText(p.refs[in.a], base_row, base_col, sheets), kAtom});
        break;
      case Op::PushRefErr:
        st.push_back({"#REF!", kAtom});
        break;
      case Op::PushName: {
        const std::string& key = p.names[in.a];
        size_t bang = key.find('!');
        std::string text = key;
        if (bang != std::string::npos && sheets) {
          int s = std::atoi(key.substr(0, bang).c_str());
          text = QuoteSheetName(sheets(s)) + "!" + key.substr(bang + 1);
        }
        st.push_back({text, kAtom});
        break;
      }
      case Op::Neg:
      case Op::Plus: {
        Piece x = pop();
        st.push_back({std::string(in.op == Op::Neg ? "-" : "+") + Wrap(x, 80), 80});
        break;
      }
      case Op::Percent: {
        Piece x = pop();
        st.push_back({Wrap(x, 70) + "%", 70});
        break;
      }
      case Op::Paren: {
        Piece x = pop();
        st.push_back({"(" + x.text + ")", kAtom});
        break;
      }
      case Op::Union: {
        Piece r = pop();
        Piece l = pop();
        st.push_back({l.text + "," + r.text, kUnionList});
        break;
      }
      case Op::Call: {
        int argc = in.b;
        std::vector<Piece> args(static_cast<size_t>(argc));
        for (int i = argc - 1; i >= 0; --i) args[static_cast<size_t>(i)] = pop();
        std::string name;
        if (in.a & 0x80000000u) {
          name = p.names[in.a & 0x7FFFFFFFu];
        } else {
          const FuncInfo& f = FunctionAt(static_cast<int>(in.a));
          name = f.name;
          if (xlsx_prefixes && (f.flags & kFnXlfn)) name = "_xlfn." + name;
        }
        std::string out = name + "(";
        for (int i = 0; i < argc; ++i) {
          if (i > 0) out.push_back(',');
          out += Wrap(args[static_cast<size_t>(i)], 0);
        }
        out.push_back(')');
        st.push_back({out, kAtom});
        break;
      }
      case Op::If:
        frames.push_back({Op::If, in.b});
        break;
      case Op::Else:
        break;
      case Op::EndIf: {
        Piece e = pop();
        Piece t = pop();
        Piece c = pop();
        frames.pop_back();
        std::string out = "IF(" + Wrap(c, 0) + "," + Wrap(t, 0);
        if (in.b == 3) out += "," + Wrap(e, 0);
        out.push_back(')');
        st.push_back({out, kAtom});
        break;
      }
      case Op::IfErr:
        frames.push_back({Op::IfErr, in.b});
        break;
      case Op::EndIfErr: {
        Piece alt = pop();
        Piece v = pop();
        frames.pop_back();
        std::string name = in.b == 1 ? "IFNA" : "IFERROR";
        if (xlsx_prefixes && in.b == 1) name = "_xlfn.IFNA";
        st.push_back({name + "(" + Wrap(v, 0) + "," + Wrap(alt, 0) + ")", kAtom});
        break;
      }
      default: {
        Piece r = pop();
        Piece l = pop();
        int prec = Precedence(in.op);
        st.push_back({Wrap(l, prec) + OpText(in.op) + Wrap(r, prec + 1), prec});
        break;
      }
    }
  }
  if (st.empty()) return "";
  Piece top = std::move(st.back());
  if (top.prec == kUnionList) return "(" + top.text + ")";
  return top.text;
}

uint64_t HashProgram(const Program& p) {
  uint64_t h = 0x243F6A8885A308D3ULL;
  HashMix(h, p.array_formula ? 1 : 0);
  for (const Instr& in : p.code) {
    HashMix(h, (static_cast<uint64_t>(in.op) << 56) | (static_cast<uint64_t>(in.mode) << 48) |
                   (static_cast<uint64_t>(in.b) << 32) | in.a);
  }
  for (double d : p.nums) {
    uint64_t bits;
    std::memcpy(&bits, &d, 8);
    HashMix(h, bits);
  }
  for (const Value& v : p.consts) HashValue(h, v);
  for (const RefSpec& r : p.refs) {
    HashMix(h, (static_cast<uint64_t>(static_cast<uint32_t>(r.r1)) << 32) |
                   static_cast<uint32_t>(r.c1));
    HashMix(h, (static_cast<uint64_t>(static_cast<uint32_t>(r.r2)) << 32) |
                   static_cast<uint32_t>(r.c2));
    HashMix(h, (static_cast<uint64_t>(static_cast<uint16_t>(r.sheet2)) << 32) |
                   (static_cast<uint64_t>(static_cast<uint16_t>(r.sheet)) << 16) |
                   (r.r1_abs ? 1u : 0u) | (r.c1_abs ? 2u : 0u) | (r.r2_abs ? 4u : 0u) |
                   (r.c2_abs ? 8u : 0u) | (r.full_cols ? 16u : 0u) | (r.full_rows ? 32u : 0u) |
                   (r.area ? 64u : 0u) | (r.explicit_sheet ? 128u : 0u));
  }
  for (const std::string& n : p.names) HashMix(h, HashBytes(n.data(), n.size()));
  return h;
}

bool SameProgram(const Program& a, const Program& b) {
  if (a.hash != b.hash || a.code.size() != b.code.size() || a.nums.size() != b.nums.size() ||
      a.consts.size() != b.consts.size() || a.refs.size() != b.refs.size() ||
      a.names.size() != b.names.size() || a.array_formula != b.array_formula) {
    return false;
  }
  for (size_t i = 0; i < a.code.size(); ++i) {
    const Instr& x = a.code[i];
    const Instr& y = b.code[i];
    if (x.op != y.op || x.mode != y.mode || x.a != y.a || x.b != y.b) return false;
  }
  for (size_t i = 0; i < a.nums.size(); ++i) {
    if (std::memcmp(&a.nums[i], &b.nums[i], 8) != 0) return false;
  }
  for (size_t i = 0; i < a.consts.size(); ++i) {
    if (!a.consts[i].SameAs(b.consts[i])) return false;
  }
  for (size_t i = 0; i < a.refs.size(); ++i) {
    if (!SameRef(a.refs[i], b.refs[i])) return false;
  }
  return a.names == b.names;
}

std::string TranslateOpenFormula(std::string_view text, bool& ok) {
  ok = true;
  if (text.size() >= 3 && (text.substr(0, 3) == "of:" || text.substr(0, 3) == "oF:")) {
    text.remove_prefix(3);
  }
  if (!text.empty() && text[0] == '=') text.remove_prefix(1);
  std::string out;
  int brace = 0;
  size_t i = 0;
  while (i < text.size()) {
    char c = text[i];
    if (c == '"') {
      size_t j = i + 1;
      while (j < text.size()) {
        if (text[j] == '"') {
          if (j + 1 < text.size() && text[j + 1] == '"') {
            j += 2;
            continue;
          }
          break;
        }
        ++j;
      }
      out.append(text.substr(i, j - i + 1));
      i = j + 1;
      continue;
    }
    if (c == '[') {
      size_t j = text.find(']', i);
      if (j == std::string_view::npos) {
        ok = false;
        return out;
      }
      std::string_view inner = text.substr(i + 1, j - i - 1);
      i = j + 1;
      std::string ref;
      size_t colon = std::string_view::npos;
      bool quote = false;
      for (size_t k = 0; k < inner.size(); ++k) {
        if (inner[k] == '\'') quote = !quote;
        if (inner[k] == ':' && !quote) {
          colon = k;
          break;
        }
      }
      auto part = [&](std::string_view p, bool first) -> bool {
        size_t dot = std::string_view::npos;
        bool q = false;
        for (size_t k = 0; k < p.size(); ++k) {
          if (p[k] == '\'') q = !q;
          if (p[k] == '.' && !q) dot = k;
        }
        if (dot == std::string_view::npos) return false;
        std::string_view sheet = p.substr(0, dot);
        std::string_view cell = p.substr(dot + 1);
        if (!sheet.empty() && sheet[0] == '$') sheet.remove_prefix(1);
        if (first && !sheet.empty()) {
          std::string name;
          if (sheet.size() >= 2 && sheet.front() == '\'' && sheet.back() == '\'') {
            std::string_view body = sheet.substr(1, sheet.size() - 2);
            for (size_t k = 0; k < body.size(); ++k) {
              if (body[k] == '\'' && k + 1 < body.size() && body[k + 1] == '\'') ++k;
              name.push_back(body[k]);
            }
          } else {
            name = std::string(sheet);
          }
          ref += QuoteSheetName(name) + "!";
        }
        ref += std::string(cell);
        return true;
      };
      if (inner.find("#REF!") != std::string_view::npos) {
        out += "#REF!";
        continue;
      }
      if (colon == std::string_view::npos) {
        if (!part(inner, true)) ok = false;
      } else {
        if (!part(inner.substr(0, colon), true)) ok = false;
        ref.push_back(':');
        if (!part(inner.substr(colon + 1), false)) ok = false;
      }
      out += ref;
      continue;
    }
    if (c == '{') ++brace;
    if (c == '}') --brace;
    if (c == ';') {
      out.push_back(',');
      ++i;
      continue;
    }
    if (c == '|' && brace > 0) {
      out.push_back(';');
      ++i;
      continue;
    }
    if (c == '~') {
      out.push_back(',');
      ++i;
      continue;
    }
    if (c == '#') {
      bool matched = false;
      for (const char* e : {"#NULL!", "#DIV/0!", "#VALUE!", "#REF!", "#NAME?", "#NUM!", "#N/A"}) {
        size_t n = std::strlen(e);
        if (text.size() - i >= n && EqualsText(text.substr(i, n), e)) {
          out += e;
          i += n;
          matched = true;
          break;
        }
      }
      if (matched) continue;
    }
    if (c == '!') {
      out.push_back(' ');
      ++i;
      continue;
    }
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_') {
      size_t j = i;
      while (j < text.size() &&
             ((text[j] >= 'A' && text[j] <= 'Z') || (text[j] >= 'a' && text[j] <= 'z') ||
              (text[j] >= '0' && text[j] <= '9') || text[j] == '_' || text[j] == '.')) {
        ++j;
      }
      std::string word(text.substr(i, j - i));
      for (const char* prefix :
           {"ORG.OPENOFFICE.", "COM.MICROSOFT.", "ORG.LIBREOFFICE.", "LEGACY."}) {
        size_t n = std::strlen(prefix);
        if (word.size() > n && word.compare(0, n, prefix) == 0) {
          if (std::strcmp(prefix, "ORG.LIBREOFFICE.") == 0 ||
              std::strcmp(prefix, "ORG.OPENOFFICE.") == 0) {
            word = "_LO." + word.substr(n);
          } else {
            word = word.substr(n);
          }
        }
      }
      out += word;
      i = j;
      continue;
    }
    out.push_back(c);
    ++i;
  }
  return out;
}

}
