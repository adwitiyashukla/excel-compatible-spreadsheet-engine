#pragma once

#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace test {

struct Case {
  const char* name;
  void (*fn)();
};

inline std::vector<Case>& Cases() {
  static std::vector<Case> cases;
  return cases;
}

inline int& Failures() {
  static int failures = 0;
  return failures;
}

struct Registrar {
  Registrar(const char* name, void (*fn)()) { Cases().push_back({name, fn}); }
};

inline void Fail(const char* file, int line, const std::string& what) {
  ++Failures();
  std::printf("  %s:%d: %s\n", file, line, what.c_str());
}

template <typename T>
std::string Show(const T& v) {
  std::ostringstream os;
  os.precision(17);
  os << v;
  return os.str();
}

inline std::string Show(const std::string& v) {
  return "\"" + v + "\"";
}
inline std::string Show(const char* v) {
  return "\"" + std::string(v) + "\"";
}
inline std::string Show(bool v) {
  return v ? "true" : "false";
}

inline bool Near(double a, double b, double tol) {
  if (a == b) return true;
  double scale = std::fmax(1.0, std::fmax(std::fabs(a), std::fabs(b)));
  return std::fabs(a - b) <= tol * scale;
}

inline int Run(int argc, char** argv) {
  std::string filter = argc > 1 ? argv[1] : "";
  int ran = 0;
  int failed = 0;
  for (const Case& c : Cases()) {
    if (!filter.empty() && std::string(c.name).find(filter) == std::string::npos) continue;
    int before = Failures();
    c.fn();
    ++ran;
    if (Failures() != before) {
      ++failed;
      std::printf("FAIL %s\n", c.name);
    }
  }
  std::printf("%d tests, %d failed, %d checks failed\n", ran, failed, Failures());
  return Failures() == 0 ? 0 : 1;
}

}

#define TEST(name)                                      \
  static void name();                                   \
  static test::Registrar name##_registrar(#name, name); \
  static void name()

#define CHECK(cond)                                                  \
  do {                                                               \
    if (!(cond)) test::Fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
  } while (0)

#define CHECK_EQ(a, b)                                                                \
  do {                                                                                \
    auto&& check_a_ = (a);                                                            \
    auto&& check_b_ = (b);                                                            \
    if (!(check_a_ == check_b_)) {                                                    \
      test::Fail(__FILE__, __LINE__,                                                  \
                 std::string(#a " == " #b "  got ") + test::Show(check_a_) + " vs " + \
                     test::Show(check_b_));                                           \
    }                                                                                 \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                                         \
  do {                                                                                \
    double check_a_ = (a);                                                            \
    double check_b_ = (b);                                                            \
    if (!test::Near(check_a_, check_b_, (tol))) {                                     \
      test::Fail(__FILE__, __LINE__,                                                  \
                 std::string(#a " ~= " #b "  got ") + test::Show(check_a_) + " vs " + \
                     test::Show(check_b_));                                           \
    }                                                                                 \
  } while (0)

#define TEST_MAIN                   \
  int main(int argc, char** argv) { \
    return test::Run(argc, argv);   \
  }
