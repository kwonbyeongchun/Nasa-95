// 스케치 구속 해석기(GEO-34~36, D8 결정: 자체 구현).
// 요소의 좌표를 변수로, 구속·치수를 잔차 식으로 두고 비선형 최소제곱(Levenberg-Marquardt, 수치 야코비안)으로 푼다.
// 상태 판정: 야코비안 계수(rank)로 자유도(미구속)·과구속을 세고, 수렴하지 못하면 충돌 구속을 찾는다.
// 외부 라이브러리를 쓰지 않는다. 해석기를 바꾸려면 이 인터페이스만 다시 구현한다.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "nasa95/model.hpp"

namespace nasa95 {

struct SketchSolveResult {
  bool converged = false;      // 모든 잔차가 허용 오차 안
  double residual = 0;         // 잔차 벡터의 최대 절댓값
  int variables = 0;           // 자유 변수 수(참조 요소 제외)
  int equations = 0;           // 잔차 식 수
  int rank = 0;                // 야코비안 계수
  int dof = 0;                 // variables - rank (남은 자유도. 강체 운동 포함)
  std::string status;          // "unconstrained" | "fully_constrained" | "over_constrained" | "conflict" | "no_constraints"
  std::vector<int> redundant;  // 독립이 아닌(과구속) 구속 번호
  std::vector<int> conflicts;  // 충돌하는 구속 번호(이것 하나를 빼면 풀린다)
  int iterations = 0;
};

// entities: 스케치의 요소 목록(sketch 객체의 entities). 풀린 좌표로 고쳐 돌려준다.
// constraints: 구속 목록 {id, kind, entities, points, value}. 모르는 종류·요소면 Error.
// identify_conflicts: 수렴하지 못했을 때 구속을 하나씩 빼 보며 충돌 구속을 찾는다(구속 수만큼 다시 푼다).
SketchSolveResult solve_sketch(Json& entities, const Json& constraints, bool identify_conflicts = true, double tolerance = 1e-12);

// 구속의 점 지정({entity, point})이 가리키는 지금 좌표 [u, v]. fixed 구속의 at 을 만들 때 쓴다
std::array<double, 2> sketch_point_position(const Json& entities, const Json& spec);

}  // namespace nasa95
