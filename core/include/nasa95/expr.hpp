#pragma once
#include <map>
#include <string>

namespace nasa95 {

// 수식 계산기. 함수의 수식, 매개변수, 공간 분포, 사용자 정의 결과가 함께 쓴다.
//   연산: + - * / ^, 괄호, 단항 -
//   함수: sin cos tan asin acos atan sinh cosh tanh exp log log10 sqrt abs min max pow
//   상수: pi, e
// 문법 오류·알 수 없는 이름은 Error("invalid_expression") 를 던진다.
double evaluate_expression(const std::string& text, const std::map<std::string, double>& variables);

}  // namespace nasa95
