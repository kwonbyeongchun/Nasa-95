// 객체 종류 정의 파일들이 함께 쓰는 도구(내부 헤더).
#pragma once
#include <string>
#include <vector>

#include "nasa95/schema.hpp"

namespace nasa95::kinds {

using F = FieldSpec;

// 적용 대상 속성. allowed 가 비면 모든 대상 종류를 허용한다.
inline F target(const std::string& name, const std::string& desc, std::vector<std::string> allowed = {}) {
  const std::string first = allowed.empty() ? "nodes" : allowed.front();
  Json ex;
  if (first == "faces")
    ex = Json{{"type", "faces"}, {"ids", Json::array({Json::array({1, 1})})}};
  else
    ex = Json{{"type", first == "set" ? std::string("nodes") : first}, {"ids", Json::array({1})}};
  F f(name, "target", desc);
  f.choices = std::move(allowed);
  f.example = ex;
  return f;
}

// 표(행의 목록) 예시 값. 중괄호 초기화의 모호성을 피하려고 따로 둔다.
inline Json rows(std::initializer_list<std::vector<double>> r) {
  Json j = Json::array();
  for (const auto& row : r) j.push_back(Json(row));
  return j;
}
inline Json ints(std::initializer_list<int> v) {
  Json j = Json::array();
  for (int e : v) j.push_back(e);
  return j;
}

const std::vector<std::string> kNodes ={"nodes", "set", "geometry"};
const std::vector<std::string> kElements = {"elements", "set", "parts", "geometry"};
const std::vector<std::string> kFaces = {"faces", "set", "geometry"};
const std::vector<std::string> kNodesOrFaces = {"nodes", "faces", "set", "geometry"};

void register_geometry(Schema& s);   // 파트·피처
void register_common(Schema& s);     // 좌표계·방향·함수·셋·매개변수·출력 시점·메시 파트
void register_material(Schema& s);   // 재료
void register_property(Schema& s);   // 프로퍼티·구속·접촉
void register_load(Schema& s);       // 초기 조건·하중·경계조건·스텝 중 변경·출력 요청
void register_case(Schema& s);       // 해석 케이스·스텝

}  // namespace nasa95::kinds
