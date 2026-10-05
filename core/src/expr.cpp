#include "nasa95/expr.hpp"

#include <cctype>
#include <cmath>
#include <vector>

#include "nasa95/error.hpp"

namespace nasa95 {

namespace {

class Parser {
 public:
  Parser(const std::string& s, const std::map<std::string, double>& vars) : s_(s), vars_(vars) {}

  double parse() {
    const double v = expr();
    skip();
    if (pos_ != s_.size()) fail("해석할 수 없는 부분이 있습니다");
    return v;
  }

 private:
  [[noreturn]] void fail(const std::string& why) const {
    throw Error("invalid_expression", "수식 오류(" + std::to_string(pos_ + 1) + "번째 글자): " + why,
                {{"expression", s_}, {"position", pos_}});
  }
  void skip() {
    while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
  }
  bool eat(char c) {
    skip();
    if (pos_ < s_.size() && s_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  double expr() {
    double v = term();
    for (;;) {
      if (eat('+'))
        v += term();
      else if (eat('-'))
        v -= term();
      else
        return v;
    }
  }
  double term() {
    double v = unary();
    for (;;) {
      if (eat('*'))
        v *= unary();
      else if (eat('/'))
        v /= unary();
      else
        return v;
    }
  }
  double unary() {
    if (eat('-')) return -unary();
    if (eat('+')) return unary();
    return power();
  }
  double power() {
    const double base = primary();
    if (eat('^')) return std::pow(base, unary());  // 오른쪽 결합
    return base;
  }
  double primary() {
    skip();
    if (pos_ >= s_.size()) fail("수식이 중간에 끝났습니다");
    const char c = s_[pos_];
    if (c == '(') {
      ++pos_;
      const double v = expr();
      if (!eat(')')) fail("닫는 괄호가 없습니다");
      return v;
    }
    if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
      std::size_t used = 0;
      double v = 0;
      try {
        v = std::stod(s_.substr(pos_), &used);
      } catch (const std::exception&) {
        fail("숫자를 읽을 수 없습니다");
      }
      pos_ += used;
      return v;
    }
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      const std::size_t start = pos_;
      while (pos_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '_')) ++pos_;
      const std::string name = s_.substr(start, pos_ - start);
      if (eat('(')) return call(name);
      if (auto it = vars_.find(name); it != vars_.end()) return it->second;
      if (name == "pi") return 3.14159265358979323846;
      if (name == "e") return 2.71828182845904523536;
      pos_ = start;
      fail("알 수 없는 이름: " + name);
    }
    fail("예상하지 못한 글자입니다");
  }
  double call(const std::string& name) {
    std::vector<double> a;
    if (!eat(')')) {
      do a.push_back(expr());
      while (eat(','));
      if (!eat(')')) fail("닫는 괄호가 없습니다");
    }
    auto need = [&](std::size_t n) {
      if (a.size() != n) fail(name + " 의 인자 수가 맞지 않습니다");
    };
    if (name == "min" || name == "max" || name == "pow") {
      need(2);
      return name == "min" ? std::fmin(a[0], a[1]) : name == "max" ? std::fmax(a[0], a[1]) : std::pow(a[0], a[1]);
    }
    need(1);
    const double x = a[0];
    if (name == "sin") return std::sin(x);
    if (name == "cos") return std::cos(x);
    if (name == "tan") return std::tan(x);
    if (name == "asin") return std::asin(x);
    if (name == "acos") return std::acos(x);
    if (name == "atan") return std::atan(x);
    if (name == "sinh") return std::sinh(x);
    if (name == "cosh") return std::cosh(x);
    if (name == "tanh") return std::tanh(x);
    if (name == "exp") return std::exp(x);
    if (name == "log") return std::log(x);
    if (name == "log10") return std::log10(x);
    if (name == "sqrt") return std::sqrt(x);
    if (name == "abs") return std::fabs(x);
    fail("알 수 없는 함수: " + name);
  }

  const std::string& s_;
  const std::map<std::string, double>& vars_;
  std::size_t pos_ = 0;
};

}  // namespace

double evaluate_expression(const std::string& text, const std::map<std::string, double>& variables) {
  return Parser(text, variables).parse();
}

}  // namespace nasa95
