#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace sheet {

constexpr int kMaxRows = 1048576;
constexpr int kMaxCols = 16384;

enum class Err : uint8_t {
  None = 0,
  Null = 1,
  Div0 = 2,
  Value = 3,
  Ref = 4,
  Name = 5,
  Num = 6,
  NA = 7,
  GettingData = 8,
  Spill = 9,
  Calc = 14,
};

std::string_view ErrorText(Err e);
bool ParseErrorText(std::string_view s, Err& out);

enum class Kind : uint8_t { Blank, Number, Bool, Text, Error, Array };

class Value;

struct TextRep {
  std::atomic<uint32_t> refs;
  uint32_t size;
  char* chars() { return reinterpret_cast<char*>(this + 1); }
  const char* chars() const { return reinterpret_cast<const char*>(this + 1); }
};

struct ArrayRep {
  std::atomic<uint32_t> refs;
  uint32_t rows;
  uint32_t cols;
  uint32_t pad;
  Value* cells() { return reinterpret_cast<Value*>(this + 1); }
  const Value* cells() const { return reinterpret_cast<const Value*>(this + 1); }
};

class Value {
 public:
  Value() noexcept : kind_(Kind::Blank), err_(Err::None), flag_(false) { num_ = 0; }
  Value(const Value& o) noexcept : kind_(o.kind_), err_(o.err_), flag_(o.flag_) {
    num_ = o.num_;
    Retain();
  }
  Value(Value&& o) noexcept : kind_(o.kind_), err_(o.err_), flag_(o.flag_) {
    num_ = o.num_;
    o.kind_ = Kind::Blank;
  }
  Value& operator=(const Value& o) noexcept {
    if (this != &o) {
      o.Retain();
      Release();
      kind_ = o.kind_;
      err_ = o.err_;
      flag_ = o.flag_;
      num_ = o.num_;
    }
    return *this;
  }
  Value& operator=(Value&& o) noexcept {
    if (this != &o) {
      Release();
      kind_ = o.kind_;
      err_ = o.err_;
      flag_ = o.flag_;
      num_ = o.num_;
      o.kind_ = Kind::Blank;
    }
    return *this;
  }
  ~Value() { Release(); }

  static Value Number(double d) {
    Value v;
    v.kind_ = Kind::Number;
    v.num_ = d;
    return v;
  }
  static Value Bool(bool b) {
    Value v;
    v.kind_ = Kind::Bool;
    v.flag_ = b;
    return v;
  }
  static Value Error(Err e) {
    Value v;
    v.kind_ = Kind::Error;
    v.err_ = e;
    return v;
  }
  static Value Text(std::string_view s);
  static Value Array(uint32_t rows, uint32_t cols);

  Kind kind() const { return kind_; }
  bool IsBlank() const { return kind_ == Kind::Blank; }
  bool IsNumber() const { return kind_ == Kind::Number; }
  bool IsBool() const { return kind_ == Kind::Bool; }
  bool IsText() const { return kind_ == Kind::Text; }
  bool IsError() const { return kind_ == Kind::Error; }
  bool IsArray() const { return kind_ == Kind::Array; }

  double number() const { return num_; }
  bool boolean() const { return flag_; }
  Err error() const { return err_; }
  std::string_view text() const {
    return kind_ == Kind::Text ? std::string_view(text_->chars(), text_->size) : std::string_view();
  }
  uint32_t rows() const { return kind_ == Kind::Array ? array_->rows : 1; }
  uint32_t cols() const { return kind_ == Kind::Array ? array_->cols : 1; }
  const Value& at(uint32_t r, uint32_t c) const { return array_->cells()[r * array_->cols + c]; }
  Value& mut(uint32_t r, uint32_t c) { return array_->cells()[r * array_->cols + c]; }

  bool SameAs(const Value& o) const;

 private:
  void Retain() const {
    if (kind_ == Kind::Text) {
      text_->refs.fetch_add(1, std::memory_order_relaxed);
    } else if (kind_ == Kind::Array) {
      array_->refs.fetch_add(1, std::memory_order_relaxed);
    }
  }
  void Release() {
    if (kind_ == Kind::Text || kind_ == Kind::Array) ReleaseShared();
    kind_ = Kind::Blank;
  }
  void ReleaseShared();

  Kind kind_;
  Err err_;
  bool flag_;
  union {
    double num_;
    TextRep* text_;
    ArrayRep* array_;
  };
};

static_assert(sizeof(Value) == 16);

bool ApproxEqual(double a, double b);
double ApproxAdd(double a, double b);
double ApproxSub(double a, double b);
double Round15(double x);
double RoundDigits(double x, int digits);

std::string FormatGeneral(double d);
std::string FormatShortest(double d);
bool ParseNumberLiteral(std::string_view s, double& out);

int CompareText(std::string_view a, std::string_view b);
bool EqualsText(std::string_view a, std::string_view b);
std::string ToUpper(std::string_view s);
std::string ToLower(std::string_view s);

int Utf8Units(std::string_view s);
size_t Utf8Offset(std::string_view s, int units);
std::string Utf8Encode(uint32_t cp);
uint32_t Utf8Decode(std::string_view s, size_t& i);

std::string ColumnName(int col);
bool ParseColumn(std::string_view s, int& col);

struct CellPos {
  int sheet = 0;
  int row = 0;
  int col = 0;
  bool operator==(const CellPos&) const = default;
};

struct Area {
  int sheet = 0;
  int r1 = 0;
  int c1 = 0;
  int r2 = 0;
  int c2 = 0;
  int rows() const { return r2 - r1 + 1; }
  int cols() const { return c2 - c1 + 1; }
  bool Contains(int r, int c) const { return r >= r1 && r <= r2 && c >= c1 && c <= c2; }
  bool operator==(const Area&) const = default;
};

std::string CellName(int row, int col);
bool ParseCellName(std::string_view s, int& row, int& col);
bool ParseAreaName(std::string_view s, int& r1, int& c1, int& r2, int& c2);

inline int ToInt(double d) {
  if (!(d == d)) return 0;
  if (d >= 2147483647.0) return 2147483647;
  if (d <= -2147483647.0) return -2147483647;
  return static_cast<int>(d);
}

inline long long ToInt64(double d) {
  if (!(d == d)) return 0;
  if (d >= 9.2e18) return 9200000000000000000LL;
  if (d <= -9.2e18) return -9200000000000000000LL;
  return static_cast<long long>(d);
}

inline uint64_t Mix64(uint64_t x) {
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
}

inline uint64_t HashBytes(const void* data, size_t n, uint64_t seed = 0x9e3779b97f4a7c15ULL) {
  const unsigned char* p = static_cast<const unsigned char*>(data);
  uint64_t h = seed ^ (n * 0x100000001b3ULL);
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 0x100000001b3ULL;
  }
  return Mix64(h);
}

}
