#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "eval.h"

namespace sheet {

namespace {

const std::vector<FuncInfo>& Registry() {
  static const std::vector<FuncInfo> table = [] {
    std::vector<FuncInfo> t;
    RegisterMath(t);
    RegisterStats(t);
    RegisterText(t);
    RegisterLookup(t);
    RegisterDate(t);
    RegisterInfo(t);
    RegisterFinance(t);
    return t;
  }();
  return table;
}

const std::unordered_map<std::string, int>& Index() {
  static const std::unordered_map<std::string, int> index = [] {
    std::unordered_map<std::string, int> m;
    const std::vector<FuncInfo>& t = Registry();
    for (size_t i = 0; i < t.size(); ++i) m.emplace(t[i].name, static_cast<int>(i));
    return m;
  }();
  return index;
}

class Stack {
 public:
  explicit Stack(size_t hint) {
    if (hint > kInline) heap_.reserve(hint);
  }
  ~Stack() {
    for (size_t i = 0; i < size_ && !use_heap_; ++i) Slot(i)->~Operand();
  }
  Stack(const Stack&) = delete;
  Stack& operator=(const Stack&) = delete;

  void Push(Operand&& o) {
    if (!use_heap_ && size_ == kInline) Spill();
    if (use_heap_) {
      heap_.push_back(std::move(o));
    } else {
      new (Slot(size_)) Operand(std::move(o));
    }
    ++size_;
  }
  Operand Pop() {
    --size_;
    if (use_heap_) {
      Operand o = std::move(heap_.back());
      heap_.pop_back();
      return o;
    }
    Operand* p = Slot(size_);
    Operand o = std::move(*p);
    p->~Operand();
    return o;
  }
  Operand& Top() { return use_heap_ ? heap_.back() : *Slot(size_ - 1); }
  Operand* From(size_t n) { return use_heap_ ? heap_.data() + (size_ - n) : Slot(size_ - n); }
  void Drop(size_t n) {
    for (size_t i = 0; i < n; ++i) Pop();
  }
  size_t size() const { return size_; }

 private:
  static constexpr size_t kInline = 24;
  Operand* Slot(size_t i) { return reinterpret_cast<Operand*>(storage_) + i; }
  void Spill() {
    heap_.reserve(kInline * 2);
    for (size_t i = 0; i < size_; ++i) {
      heap_.push_back(std::move(*Slot(i)));
      Slot(i)->~Operand();
    }
    use_heap_ = true;
  }
  alignas(Operand) unsigned char storage_[kInline * sizeof(Operand)];
  std::vector<Operand> heap_;
  size_t size_ = 0;
  bool use_heap_ = false;
};

Value Prepare(const Operand& o, bool array_mode, const EvalContext& ctx) {
  if (o.tag == Operand::Tag::Ref) {
    if (array_mode) {
      if (o.area.rows() == 1 && o.area.cols() == 1) {
        return ctx.Read(o.area.sheet, o.area.r1, o.area.c1);
      }
      return ctx.Materialize(o.area);
    }
    return ctx.Intersect(o.area);
  }
  if (o.tag == Operand::Tag::Refs) return Value::Error(Err::Value);
  if (o.tag == Operand::Tag::Missing) return Value();
  return o.value;
}

const Value& Element(const Value& v, uint32_t r, uint32_t c, bool& na) {
  na = false;
  if (!v.IsArray()) return v;
  uint32_t rr = v.rows() == 1 ? 0 : r;
  uint32_t cc = v.cols() == 1 ? 0 : c;
  if (rr >= v.rows() || cc >= v.cols()) {
    na = true;
    return v;
  }
  return v.at(rr, cc);
}

double Clean(double d) {
  return d == 0.0 ? 0.0 : d;
}

Value Arith(Op op, const Value& a, const Value& b, const EvalContext& ctx) {
  if (a.IsError()) return a;
  double x;
  Err e = ToNumber(a, ctx, x);
  if (e != Err::None) return Value::Error(e);
  if (b.IsError()) return b;
  double y;
  e = ToNumber(b, ctx, y);
  if (e != Err::None) return Value::Error(e);
  double r = 0;
  switch (op) {
    case Op::Add:
      r = ApproxAdd(x, y);
      break;
    case Op::Sub:
      r = ApproxSub(x, y);
      break;
    case Op::Mul:
      r = x * y;
      break;
    case Op::Div:
      if (y == 0) return Value::Error(Err::Div0);
      r = x / y;
      break;
    case Op::Pow:
      if (x == 0 && y == 0) return Value::Error(Err::Num);
      if (x == 0 && y < 0) return Value::Error(Err::Div0);
      if (x < 0 && y != std::floor(y)) {
        double inv = 1.0 / y;
        if (inv == std::floor(inv) && std::fmod(std::fabs(inv), 2.0) == 1.0) {
          r = -std::pow(-x, y);
          break;
        }
        return Value::Error(Err::Num);
      }
      r = std::pow(x, y);
      break;
    default:
      break;
  }
  if (!std::isfinite(r)) return Value::Error(Err::Num);
  return Value::Number(Clean(r));
}

Value CompareOp(Op op, const Value& a, const Value& b) {
  if (a.IsError()) return a;
  if (b.IsError()) return b;
  int c = CompareValues(a, b);
  bool r = false;
  switch (op) {
    case Op::Eq:
      r = c == 0;
      break;
    case Op::Ne:
      r = c != 0;
      break;
    case Op::Lt:
      r = c < 0;
      break;
    case Op::Le:
      r = c <= 0;
      break;
    case Op::Gt:
      r = c > 0;
      break;
    case Op::Ge:
      r = c >= 0;
      break;
    default:
      break;
  }
  return Value::Bool(r);
}

Value ScalarBinary(Op op, const Value& a, const Value& b, const EvalContext& ctx) {
  switch (op) {
    case Op::Add:
    case Op::Sub:
    case Op::Mul:
    case Op::Div:
    case Op::Pow:
      return Arith(op, a, b, ctx);
    case Op::Concat: {
      if (a.IsError()) return a;
      if (b.IsError()) return b;
      std::string s = ToText(a);
      s += ToText(b);
      if (s.size() > 32767) return Value::Error(Err::Value);
      return Value::Text(s);
    }
    default:
      return CompareOp(op, a, b);
  }
}

Value Binary(Op op, const Value& a, const Value& b, const EvalContext& ctx) {
  if (!a.IsArray() && !b.IsArray()) return ScalarBinary(op, a, b, ctx);
  uint32_t rows = std::max(a.rows(), b.rows());
  uint32_t cols = std::max(a.cols(), b.cols());
  Value out = Value::Array(rows, cols);
  for (uint32_t r = 0; r < rows; ++r) {
    for (uint32_t c = 0; c < cols; ++c) {
      bool na1;
      bool na2;
      const Value& x = Element(a, r, c, na1);
      const Value& y = Element(b, r, c, na2);
      if (na1 || na2) {
        out.mut(r, c) = Value::Error(Err::NA);
      } else {
        out.mut(r, c) = ScalarBinary(op, x, y, ctx);
      }
    }
  }
  return out;
}

Value Unary(Op op, const Value& v, const EvalContext& ctx) {
  if (v.IsArray()) {
    Value out = Value::Array(v.rows(), v.cols());
    for (uint32_t r = 0; r < v.rows(); ++r) {
      for (uint32_t c = 0; c < v.cols(); ++c) out.mut(r, c) = Unary(op, v.at(r, c), ctx);
    }
    return out;
  }
  if (op == Op::Plus) return v;
  if (v.IsError()) return v;
  double x;
  Err e = ToNumber(v, ctx, x);
  if (e != Err::None) return Value::Error(e);
  if (op == Op::Neg) return Value::Number(Clean(-x));
  return Value::Number(x / 100.0);
}

bool CondValue(const Value& v, bool& out, Err& err) {
  err = ToBool(v, out);
  return err == Err::None;
}

Value SelectIf(const Value& cond, const Value& t, const Value& e) {
  uint32_t rows = std::max({cond.rows(), t.rows(), e.rows()});
  uint32_t cols = std::max({cond.cols(), t.cols(), e.cols()});
  Value out = Value::Array(rows, cols);
  for (uint32_t r = 0; r < rows; ++r) {
    for (uint32_t c = 0; c < cols; ++c) {
      bool na;
      const Value& cv = Element(cond, r, c, na);
      if (na) {
        out.mut(r, c) = Value::Error(Err::NA);
        continue;
      }
      bool b;
      Err err;
      if (!CondValue(cv, b, err)) {
        out.mut(r, c) = Value::Error(err);
        continue;
      }
      const Value& pick = Element(b ? t : e, r, c, na);
      out.mut(r, c) = na ? Value::Error(Err::NA) : pick;
    }
  }
  return out;
}

Value SelectIfErr(const Value& v, const Value& alt, bool na_only) {
  uint32_t rows = std::max(v.rows(), alt.rows());
  uint32_t cols = std::max(v.cols(), alt.cols());
  Value out = Value::Array(rows, cols);
  for (uint32_t r = 0; r < rows; ++r) {
    for (uint32_t c = 0; c < cols; ++c) {
      bool na;
      const Value& x = Element(v, r, c, na);
      if (na) {
        out.mut(r, c) = Value::Error(Err::NA);
        continue;
      }
      bool hit = x.IsError() && (!na_only || x.error() == Err::NA);
      if (!hit) {
        out.mut(r, c) = x;
        continue;
      }
      const Value& y = Element(alt, r, c, na);
      out.mut(r, c) = na ? Value::Error(Err::NA) : y;
    }
  }
  return out;
}

Value ResultScalar(const Operand& o, const EvalContext& ctx) {
  Value v = ctx.Scalar(o);
  return v;
}

Operand CallLifted(const FuncInfo& f, EvalContext& ctx, Operand* args, int argc,
                   const std::vector<int>& lifted) {
  uint32_t rows = 1;
  uint32_t cols = 1;
  for (int i : lifted) {
    rows = std::max(rows, args[i].value.rows());
    cols = std::max(cols, args[i].value.cols());
  }
  Value out = Value::Array(rows, cols);
  std::vector<Operand> call(static_cast<size_t>(argc));
  for (uint32_t r = 0; r < rows; ++r) {
    for (uint32_t c = 0; c < cols; ++c) {
      bool na_any = false;
      for (int i = 0; i < argc; ++i) {
        if (std::find(lifted.begin(), lifted.end(), i) != lifted.end()) {
          bool na;
          const Value& x = Element(args[i].value, r, c, na);
          if (na) na_any = true;
          call[static_cast<size_t>(i)] = Operand::Of(x);
        } else {
          call[static_cast<size_t>(i)] = args[i];
        }
      }
      if (na_any) {
        out.mut(r, c) = Value::Error(Err::NA);
        continue;
      }
      Operand res = f.impl(ctx, call.data(), argc);
      out.mut(r, c) = ResultScalar(res, ctx);
    }
  }
  return Operand::Of(std::move(out));
}

Operand CallFunction(const FuncInfo& f, EvalContext& ctx, Operand* args, int argc,
                     bool array_mode) {
  std::vector<int> lifted;
  for (int i = 0; i < argc; ++i) {
    char cls = ParamClass(f, i);
    if (cls != 'V') continue;
    Operand& o = args[i];
    if (o.tag == Operand::Tag::Ref) {
      if (array_mode && (o.area.rows() > 1 || o.area.cols() > 1)) {
        o = Operand::Of(ctx.Materialize(o.area));
        lifted.push_back(i);
      } else {
        o = Operand::Of(Prepare(o, false, ctx));
      }
    } else if (o.tag == Operand::Tag::Refs) {
      o = Operand::Of(Value::Error(Err::Value));
    } else if (o.tag == Operand::Tag::Val && o.value.IsArray()) {
      if (o.value.rows() == 1 && o.value.cols() == 1) {
        Value inner = o.value.at(0, 0);
        o.value = std::move(inner);
      } else {
        lifted.push_back(i);
      }
    }
  }
  ctx.array_call = array_mode;
  if (!lifted.empty() && (f.flags & kFnLift)) return CallLifted(f, ctx, args, argc, lifted);
  if (!lifted.empty()) {
    for (int i : lifted) {
      Value first = args[i].value.at(0, 0);
      args[i] = Operand::Of(std::move(first));
    }
  }
  return f.impl(ctx, args, argc);
}

}

int FindFunction(std::string_view name) {
  const auto& idx = Index();
  auto it = idx.find(std::string(name));
  if (it != idx.end()) return it->second;
  std::string upper = ToUpper(name);
  it = idx.find(upper);
  return it == idx.end() ? -1 : it->second;
}

const FuncInfo& FunctionAt(int id) {
  return Registry()[static_cast<size_t>(id)];
}

int FunctionCount() {
  return static_cast<int>(Registry().size());
}

char ParamClass(const FuncInfo& f, int index) {
  int n = static_cast<int>(std::strlen(f.params));
  if (n == 0) return 'V';
  if (index < n) return f.params[index];
  int rep = f.repeat > 0 ? f.repeat : 1;
  if (rep > n) rep = n;
  return f.params[n - rep + (index - n) % rep];
}

Err ToNumber(const Value& v, const EvalContext& ctx, double& out) {
  switch (v.kind()) {
    case Kind::Number:
      out = v.number();
      return Err::None;
    case Kind::Bool:
      out = v.boolean() ? 1 : 0;
      return Err::None;
    case Kind::Blank:
      out = 0;
      return Err::None;
    case Kind::Text:
      if (ParseNumberText(v.text(), ctx.dates(), out)) return Err::None;
      return Err::Value;
    case Kind::Error:
      return v.error();
    case Kind::Array:
      if (v.rows() > 0 && v.cols() > 0) return ToNumber(v.at(0, 0), ctx, out);
      return Err::Value;
  }
  return Err::Value;
}

std::string ToText(const Value& v) {
  switch (v.kind()) {
    case Kind::Number:
      return FormatGeneral(v.number());
    case Kind::Bool:
      return v.boolean() ? "TRUE" : "FALSE";
    case Kind::Text:
      return std::string(v.text());
    case Kind::Error:
      return std::string(ErrorText(v.error()));
    case Kind::Array:
      if (v.rows() > 0 && v.cols() > 0) return ToText(v.at(0, 0));
      return "";
    case Kind::Blank:
      return "";
  }
  return "";
}

Err ToBool(const Value& v, bool& out) {
  switch (v.kind()) {
    case Kind::Number:
      out = v.number() != 0;
      return Err::None;
    case Kind::Bool:
      out = v.boolean();
      return Err::None;
    case Kind::Blank:
      out = false;
      return Err::None;
    case Kind::Text:
      if (EqualsText(v.text(), "TRUE")) {
        out = true;
        return Err::None;
      }
      if (EqualsText(v.text(), "FALSE")) {
        out = false;
        return Err::None;
      }
      return Err::Value;
    case Kind::Error:
      return v.error();
    case Kind::Array:
      if (v.rows() > 0 && v.cols() > 0) return ToBool(v.at(0, 0), out);
      return Err::Value;
  }
  return Err::Value;
}

int CompareValues(const Value& a, const Value& b) {
  if (a.IsBlank() && b.IsBlank()) return 0;
  if (a.IsBlank() || b.IsBlank()) {
    const Value& v = a.IsBlank() ? b : a;
    int c = 0;
    if (v.IsNumber()) {
      double y = v.number();
      c = ApproxEqual(0.0, y) ? 0 : (0.0 < y ? -1 : 1);
    } else if (v.IsText()) {
      c = v.text().empty() ? 0 : -1;
    } else if (v.IsBool()) {
      c = v.boolean() ? -1 : 0;
    }
    return a.IsBlank() ? c : -c;
  }
  auto rank = [](const Value& v) { return v.IsNumber() ? 0 : v.IsText() ? 1 : v.IsBool() ? 2 : 3; };
  int ra = rank(a);
  int rb = rank(b);
  if (ra != rb) return ra < rb ? -1 : 1;
  if (a.IsNumber()) {
    double x = a.number();
    double y = b.number();
    if (ApproxEqual(x, y)) return 0;
    return x < y ? -1 : 1;
  }
  if (a.IsText()) {
    int c = CompareText(a.text(), b.text());
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
  }
  if (a.IsBool()) {
    if (a.boolean() == b.boolean()) return 0;
    return a.boolean() ? 1 : -1;
  }
  return 0;
}

Value EvalContext::Intersect(const Area& a) const {
  if (a.r1 == a.r2 && a.c1 == a.c2) return Read(a.sheet, a.r1, a.c1);
  if (a.c1 == a.c2 && row >= a.r1 && row <= a.r2) return Read(a.sheet, row, a.c1);
  if (a.r1 == a.r2 && col >= a.c1 && col <= a.c2) return Read(a.sheet, a.r1, col);
  return Value::Error(Err::Value);
}

Value EvalContext::Materialize(const Area& a0) const {
  Area a = a0;
  if (a.sheet < 0 || a.sheet >= wb_.SheetCount()) return Value::Error(Err::Ref);
  uint64_t cells = static_cast<uint64_t>(a.rows()) * static_cast<uint64_t>(a.cols());
  if (cells > 4000000) {
    a.r2 = std::max(a.r1, std::min(a.r2, UsedRows(a.sheet) - 1));
    a.c2 = std::max(a.c1, std::min(a.c2, UsedCols(a.sheet) - 1));
    cells = static_cast<uint64_t>(a.rows()) * static_cast<uint64_t>(a.cols());
    if (cells > 4000000) return Value::Error(Err::Num);
  }
  Value out = Value::Array(static_cast<uint32_t>(a.rows()), static_cast<uint32_t>(a.cols()));
  Scan(a, [&](int r, int c, const Value& v) {
    out.mut(static_cast<uint32_t>(r - a.r1), static_cast<uint32_t>(c - a.c1)) = v;
  });
  return out;
}

Value EvalContext::Deref(const Operand& op, bool array_mode) const {
  return Prepare(op, array_mode, *this);
}

Value EvalContext::Scalar(const Operand& op) const {
  Value v = Prepare(op, false, *this);
  if (v.IsArray()) {
    if (v.rows() == 0 || v.cols() == 0) return Value::Error(Err::Value);
    Value first = v.at(0, 0);
    return first;
  }
  return v;
}

double EvalContext::Random(uint64_t salt) const {
  uint64_t h = Mix64(wb_.seed_ ^ Mix64(wb_.recalc_serial_ * 0x9E3779B97F4A7C15ULL));
  h = Mix64(h ^ CellKey(sheet, row, col));
  h = Mix64(h ^ (salt + random_calls++));
  return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0);
}

Operand EvalContext::EvalName(std::string_view key) const {
  int scope = -1;
  std::string_view name = key;
  size_t bang = key.find('!');
  if (bang != std::string_view::npos) {
    scope = std::atoi(std::string(key.substr(0, bang)).c_str());
    name = key.substr(bang + 1);
  }
  const DefinedName* dn = wb_.FindName(name, scope >= 0 ? scope : sheet);
  if (!dn || !dn->prog) return MakeErr(Err::Name);
  if (depth > 32) return MakeErr(Err::Name);
  ++depth;
  Operand r;
  try {
    r = Execute(*dn->prog, const_cast<EvalContext&>(*this));
  } catch (...) {
    --depth;
    throw;
  }
  --depth;
  if (r.tag == Operand::Tag::Ref && dn->prog->refs.size() == 1 && dn->prog->refs[0].sheet < 0) {
    r.area.sheet = sheet;
  }
  return r;
}

bool EvalContext::ParseRef(std::string_view text, bool a1, Area& out) const {
  while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
  while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
  if (text.empty() || text.size() > 512) return false;
  if (!a1) {
    std::string_view rest = text;
    int target = sheet;
    size_t bang = rest.rfind('!');
    if (bang != std::string_view::npos) {
      std::string_view sname = rest.substr(0, bang);
      if (sname.size() >= 2 && sname.front() == '\'' && sname.back() == '\'') {
        sname = sname.substr(1, sname.size() - 2);
      }
      target = wb_.FindSheet(sname);
      if (target < 0) return false;
      rest = rest.substr(bang + 1);
    }
    auto parse_part = [&](std::string_view& s, int& r, int& c) -> bool {
      auto parse_axis = [&](char letter, int base, int& v) -> bool {
        if (s.empty() || (s[0] != letter && s[0] != letter + 32)) return false;
        s.remove_prefix(1);
        if (!s.empty() && s[0] == '[') {
          size_t close = s.find(']');
          if (close == std::string_view::npos) return false;
          long off = std::strtol(std::string(s.substr(1, close - 1)).c_str(), nullptr, 10);
          v = base + static_cast<int>(off);
          s.remove_prefix(close + 1);
          return true;
        }
        size_t k = 0;
        while (k < s.size() && s[k] >= '0' && s[k] <= '9') ++k;
        if (k == 0) {
          v = base;
          return true;
        }
        v = std::atoi(std::string(s.substr(0, k)).c_str()) - 1;
        s.remove_prefix(k);
        return true;
      };
      if (!parse_axis('R', row, r)) return false;
      if (!parse_axis('C', col, c)) return false;
      return true;
    };
    int r1;
    int c1;
    if (!parse_part(rest, r1, c1)) return false;
    int r2 = r1;
    int c2 = c1;
    if (!rest.empty() && rest[0] == ':') {
      rest.remove_prefix(1);
      if (!parse_part(rest, r2, c2)) return false;
    }
    if (!rest.empty()) return false;
    if (r1 > r2) std::swap(r1, r2);
    if (c1 > c2) std::swap(c1, c2);
    if (r1 < 0 || c1 < 0 || r2 >= kMaxRows || c2 >= kMaxCols) return false;
    out = Area{target, r1, c1, r2, c2};
    return true;
  }
  ParseError err;
  ProgramRef p = CompileFormula(text, row, col, wb_.Resolver(), false, &err);
  if (!p) return false;
  bool simple =
      (p->code.size() == 1 && (p->code[0].op == Op::PushRef || p->code[0].op == Op::PushName)) ||
      (p->code.size() == 3 && p->code[2].op == Op::Range);
  if (!simple) return false;
  EvalContext sub(wb_, sheet, row, col);
  sub.depth = depth + 1;
  Operand r = Execute(*p, sub);
  if (r.tag != Operand::Tag::Ref) return false;
  out = r.area;
  return true;
}

Operand Execute(const Program& p, EvalContext& ctx) {
  Stack st(static_cast<size_t>(p.max_stack) * 2 + 8);
  uint8_t frames[64];
  int fp = 0;
  const size_t n = p.code.size();
  for (size_t pc = 0; pc < n; ++pc) {
    const Instr& in = p.code[pc];
    bool am = in.mode != 0;
    switch (in.op) {
      case Op::PushNum:
        st.Push(Operand::Of(Value::Number(p.nums[in.a])));
        break;
      case Op::PushStr:
      case Op::PushArray:
        st.Push(Operand::Of(p.consts[in.a]));
        break;
      case Op::PushBool:
        st.Push(Operand::Of(Value::Bool(in.a != 0)));
        break;
      case Op::PushErr:
        st.Push(Operand::Of(Value::Error(static_cast<Err>(in.a))));
        break;
      case Op::PushMissing:
        st.Push(Operand::OfMissing());
        break;
      case Op::PushRefErr:
        st.Push(Operand::Of(Value::Error(Err::Ref)));
        break;
      case Op::PushRef: {
        const RefSpec& s = p.refs[in.a];
        Area a;
        if (!ResolveRef(s, ctx.row, ctx.col, a)) {
          st.Push(Operand::Of(Value::Error(Err::Ref)));
          break;
        }
        a.sheet = s.sheet >= 0 ? s.sheet : ctx.sheet;
        if (s.sheet2 > s.sheet) {
          auto list = std::make_shared<std::vector<Area>>();
          for (int sh = s.sheet; sh <= s.sheet2; ++sh) {
            Area x = a;
            x.sheet = sh;
            list->push_back(x);
          }
          Operand u;
          u.tag = Operand::Tag::Refs;
          u.list = std::move(list);
          st.Push(std::move(u));
          break;
        }
        st.Push(Operand::OfRef(a));
        break;
      }
      case Op::PushName:
        st.Push(ctx.EvalName(p.names[in.a]));
        break;
      case Op::Neg:
      case Op::Plus:
      case Op::Percent: {
        Operand x = st.Pop();
        if (in.op == Op::Plus && x.IsRef() && !am) {
          st.Push(Operand::Of(Prepare(x, false, ctx)));
          break;
        }
        st.Push(Operand::Of(Unary(in.op, Prepare(x, am, ctx), ctx)));
        break;
      }
      case Op::Paren:
        break;
      case Op::Range:
      case Op::Union:
      case Op::Intersect: {
        Operand b = st.Pop();
        Operand a = st.Pop();
        if (a.IsError()) {
          st.Push(std::move(a));
          break;
        }
        if (b.IsError()) {
          st.Push(std::move(b));
          break;
        }
        if (!a.IsRef() || !b.IsRef()) {
          st.Push(MakeErr(Err::Value));
          break;
        }
        if (in.op == Op::Union) {
          auto list = std::make_shared<std::vector<Area>>();
          for (const Operand* o : {&a, &b}) {
            if (o->tag == Operand::Tag::Ref) {
              list->push_back(o->area);
            } else {
              list->insert(list->end(), o->list->begin(), o->list->end());
            }
          }
          Operand u;
          u.tag = Operand::Tag::Refs;
          u.list = std::move(list);
          st.Push(std::move(u));
          break;
        }
        if (a.tag != Operand::Tag::Ref || b.tag != Operand::Tag::Ref ||
            a.area.sheet != b.area.sheet) {
          st.Push(MakeErr(Err::Value));
          break;
        }
        Area r;
        r.sheet = a.area.sheet;
        if (in.op == Op::Range) {
          r.r1 = std::min(a.area.r1, b.area.r1);
          r.c1 = std::min(a.area.c1, b.area.c1);
          r.r2 = std::max(a.area.r2, b.area.r2);
          r.c2 = std::max(a.area.c2, b.area.c2);
        } else {
          r.r1 = std::max(a.area.r1, b.area.r1);
          r.c1 = std::max(a.area.c1, b.area.c1);
          r.r2 = std::min(a.area.r2, b.area.r2);
          r.c2 = std::min(a.area.c2, b.area.c2);
          if (r.r1 > r.r2 || r.c1 > r.c2) {
            st.Push(MakeErr(Err::Null));
            break;
          }
        }
        st.Push(Operand::OfRef(r));
        break;
      }
      case Op::Call: {
        int argc = in.b;
        Operand* args = st.From(static_cast<size_t>(argc));
        Operand result;
        if (in.a & 0x80000000u) {
          result = MakeErr(Err::Name);
        } else {
          const FuncInfo& f = FunctionAt(static_cast<int>(in.a));
          result = CallFunction(f, ctx, args, argc, am);
        }
        st.Drop(static_cast<size_t>(argc));
        st.Push(std::move(result));
        break;
      }
      case Op::If: {
        Operand c = st.Pop();
        Value v = Prepare(c, am, ctx);
        if (fp >= 64) {
          return MakeErr(Err::Value);
        }
        if (v.IsArray()) {
          st.Push(Operand::Of(std::move(v)));
          frames[fp++] = 1;
          break;
        }
        bool b;
        Err e;
        if (!CondValue(v, b, e)) {
          st.Push(MakeErr(e));
          frames[fp++] = 2;
          pc = p.code[in.a - 1].a - 1;
          break;
        }
        frames[fp++] = 0;
        if (!b) pc = in.a - 1;
        break;
      }
      case Op::Else:
        if (frames[fp - 1] == 0) pc = in.a - 1;
        break;
      case Op::EndIf: {
        uint8_t kind = frames[--fp];
        if (kind == 1) {
          Operand e = st.Pop();
          Operand t = st.Pop();
          Operand c = st.Pop();
          Value ev = Prepare(e, true, ctx);
          Value tv = Prepare(t, true, ctx);
          st.Push(Operand::Of(SelectIf(c.value, tv, ev)));
        }
        break;
      }
      case Op::IfErr: {
        Operand x = st.Pop();
        Value v = Prepare(x, am, ctx);
        bool na_only = in.b == 1;
        if (fp >= 64) return MakeErr(Err::Value);
        if (v.IsArray()) {
          bool any = false;
          for (uint32_t r = 0; r < v.rows() && !any; ++r) {
            for (uint32_t c = 0; c < v.cols(); ++c) {
              const Value& e = v.at(r, c);
              if (e.IsError() && (!na_only || e.error() == Err::NA)) {
                any = true;
                break;
              }
            }
          }
          st.Push(Operand::Of(std::move(v)));
          if (any) {
            frames[fp++] = 1;
          } else {
            frames[fp++] = 0;
            pc = in.a - 1;
          }
          break;
        }
        bool hit = v.IsError() && (!na_only || v.error() == Err::NA);
        frames[fp++] = 0;
        if (!hit) {
          st.Push(Operand::Of(std::move(v)));
          pc = in.a - 1;
        }
        break;
      }
      case Op::EndIfErr: {
        uint8_t kind = frames[--fp];
        if (kind == 1) {
          Operand alt = st.Pop();
          Operand v = st.Pop();
          Value av = Prepare(alt, true, ctx);
          st.Push(Operand::Of(SelectIfErr(v.value, av, in.b == 1)));
        }
        break;
      }
      default: {
        Operand b = st.Pop();
        Operand a = st.Pop();
        Value av = Prepare(a, am, ctx);
        Value bv = Prepare(b, am, ctx);
        st.Push(Operand::Of(Binary(in.op, av, bv, ctx)));
        break;
      }
    }
  }
  if (st.size() == 0) return MakeErr(Err::Value);
  return st.Pop();
}

Value EvaluateFormula(const Program& p, EvalContext& ctx, bool array_mode) {
  Operand r = Execute(p, ctx);
  Value v;
  if (r.tag == Operand::Tag::Ref) {
    if (array_mode) {
      v = (r.area.rows() == 1 && r.area.cols() == 1) ? ctx.Read(r.area.sheet, r.area.r1, r.area.c1)
                                                     : ctx.Materialize(r.area);
    } else {
      v = ctx.Intersect(r.area);
    }
  } else if (r.tag == Operand::Tag::Refs) {
    v = Value::Error(Err::Value);
  } else if (r.tag == Operand::Tag::Missing) {
    v = Value::Number(0);
  } else {
    v = std::move(r.value);
  }
  if (v.IsArray()) {
    if (!array_mode) {
      Value first = v.rows() > 0 && v.cols() > 0 ? v.at(0, 0) : Value::Error(Err::Value);
      v = std::move(first);
    } else {
      for (uint32_t i = 0; i < v.rows(); ++i) {
        for (uint32_t j = 0; j < v.cols(); ++j) {
          if (v.at(i, j).IsBlank()) v.mut(i, j) = Value::Number(0);
        }
      }
    }
  }
  if (v.IsBlank()) v = Value::Number(0);
  return v;
}

Err ArgNumber(EvalContext& ctx, const Operand& a, double& out) {
  if (a.IsMissing()) {
    out = 0;
    return Err::None;
  }
  Value v = ctx.Scalar(a);
  return ToNumber(v, ctx, out);
}

Err ArgNumberOr(EvalContext& ctx, Operand* args, int argc, int i, double def, double& out) {
  if (i >= argc || args[i].IsMissing()) {
    out = def;
    return Err::None;
  }
  return ArgNumber(ctx, args[i], out);
}

Err ArgInt(EvalContext& ctx, const Operand& a, long long& out) {
  double d;
  Err e = ArgNumber(ctx, a, d);
  if (e != Err::None) return e;
  if (!(std::fabs(d) < 9.2e18)) return Err::Num;
  out = static_cast<long long>(d);
  return Err::None;
}

Err ArgText(EvalContext& ctx, const Operand& a, std::string& out) {
  if (a.IsMissing()) {
    out.clear();
    return Err::None;
  }
  Value v = ctx.Scalar(a);
  if (v.IsError()) return v.error();
  out = ToText(v);
  return Err::None;
}

Err ArgBool(EvalContext& ctx, const Operand& a, bool& out) {
  if (a.IsMissing()) {
    out = false;
    return Err::None;
  }
  Value v = ctx.Scalar(a);
  return ToBool(v, out);
}

Err ArgBoolOr(EvalContext& ctx, Operand* args, int argc, int i, bool def, bool& out) {
  if (i >= argc || args[i].IsMissing()) {
    out = def;
    return Err::None;
  }
  return ArgBool(ctx, args[i], out);
}

bool MakeGrid(EvalContext& ctx, const Operand& op, Grid& g, bool clamp) {
  g.ctx = &ctx;
  if (op.tag == Operand::Tag::Ref) {
    g.kind = 0;
    g.area = op.area;
    g.rows = op.area.rows();
    g.cols = op.area.cols();
    if (clamp && op.area.sheet >= 0 && op.area.sheet < ctx.wb().SheetCount()) {
      int used_r = ctx.UsedRows(op.area.sheet) - op.area.r1;
      int used_c = ctx.UsedCols(op.area.sheet) - op.area.c1;
      g.rows = std::max(0, std::min(g.rows, used_r));
      g.cols = std::max(0, std::min(g.cols, used_c));
    }
    return true;
  }
  if (op.tag == Operand::Tag::Val) {
    if (op.value.IsArray()) {
      g.kind = 1;
      g.array = op.value;
      g.rows = static_cast<int>(op.value.rows());
      g.cols = static_cast<int>(op.value.cols());
      return true;
    }
    g.kind = 2;
    g.scalar = op.value;
    g.rows = 1;
    g.cols = 1;
    return true;
  }
  return false;
}

Err CollectNumbers(EvalContext& ctx, Operand* args, int argc, std::vector<double>& out,
                   CollectMode mode, bool skip_errors) {
  for (int i = 0; i < argc; ++i) {
    Operand& a = args[i];
    if (a.IsMissing()) {
      out.push_back(0);
      continue;
    }
    auto take_element = [&](const Value& v) -> Err {
      switch (v.kind()) {
        case Kind::Number:
          out.push_back(v.number());
          return Err::None;
        case Kind::Error:
          return skip_errors ? Err::None : v.error();
        case Kind::Bool:
          if (mode != CollectMode::Numbers) out.push_back(v.boolean() ? 1 : 0);
          return Err::None;
        case Kind::Text:
          if (mode != CollectMode::Numbers) out.push_back(0);
          return Err::None;
        default:
          return Err::None;
      }
    };
    if (a.tag == Operand::Tag::Ref || a.tag == Operand::Tag::Refs) {
      Err bad = Err::None;
      auto scan = [&](const Area& area) {
        if (area.sheet >= 0 && area.sheet < ctx.wb().SheetCount()) {
          long long r = std::min(area.r2, ctx.UsedRows(area.sheet) - 1) - area.r1 + 1;
          long long c = std::min(area.c2, ctx.UsedCols(area.sheet) - 1) - area.c1 + 1;
          if (r > 0 && c > 0) out.reserve(out.size() + static_cast<size_t>(r * c));
        }
        auto visit = [&](int, int, const Value& v) {
          if (bad != Err::None) return;
          Err e = take_element(v);
          if (e != Err::None) bad = e;
        };
        if (area.cols() == 1) {
          ctx.Scan(area, visit);
        } else {
          ctx.ScanOrdered(area, visit);
        }
      };
      if (a.tag == Operand::Tag::Ref) {
        scan(a.area);
      } else {
        for (const Area& area : *a.list) scan(area);
      }
      if (bad != Err::None) return bad;
      continue;
    }
    const Value& v = a.value;
    if (v.IsArray()) {
      for (uint32_t r = 0; r < v.rows(); ++r) {
        for (uint32_t c = 0; c < v.cols(); ++c) {
          Err e = take_element(v.at(r, c));
          if (e != Err::None) return e;
        }
      }
      continue;
    }
    switch (v.kind()) {
      case Kind::Number:
        out.push_back(v.number());
        break;
      case Kind::Bool:
        out.push_back(v.boolean() ? 1 : 0);
        break;
      case Kind::Blank:
        out.push_back(0);
        break;
      case Kind::Text: {
        double d;
        if (!ParseNumberText(v.text(), ctx.dates(), d)) {
          if (mode == CollectMode::All) {
            out.push_back(0);
            break;
          }
          return Err::Value;
        }
        out.push_back(d);
        break;
      }
      case Kind::Error:
        if (!skip_errors) return v.error();
        break;
      default:
        break;
    }
  }
  return Err::None;
}

}
