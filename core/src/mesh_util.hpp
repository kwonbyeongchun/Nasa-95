// 메시 명령 파일들이 함께 쓰는 도구(내부 헤더).
#pragma once
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "ofep/app.hpp"
#include "ofep/error.hpp"

namespace ofep::meshutil {

using F = FieldSpec;

inline bool has(const Json& p, const char* key) { return p.contains(key) && !p[key].is_null(); }

inline Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
inline Vec3 mul(const Vec3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline double norm(const Vec3& a) { return std::sqrt(dot(a, a)); }
inline Vec3 vec3(const Json& v) { return {v[0].get<double>(), v[1].get<double>(), v[2].get<double>()}; }
inline Vec3 unit(const Vec3& v, const char* param) {
  const double n = norm(v);
  if (n == 0.0) throw Error("out_of_range", std::string("'") + param + "' 의 길이가 0 입니다", {{"param", param}});
  return mul(v, 1.0 / n);
}

// 실수 배열 매개변수: 따로 넘긴 배열이 있으면 그것을, 없으면 JSON(표 또는 목록)을 편다.
inline std::vector<double> doubles(const App& a, const Json& p, const char* key) {
  if (a.arrays() && a.arrays()->doubles.count(key)) return a.arrays()->doubles.at(key);
  std::vector<double> out;
  if (!has(p, key)) return out;
  for (const Json& e : p[key]) {
    if (e.is_array())
      for (const Json& x : e) out.push_back(x.get<double>());
    else
      out.push_back(e.get<double>());
  }
  return out;
}

inline std::vector<Id> id_list(const App& a, const Json& p, const char* key) {
  std::vector<Id> out;
  auto put = [&](std::int64_t v) {
    if (v < 0) throw Error("out_of_range", std::string("'") + key + "' 에 음수 ID 가 있습니다", {{"param", key}});
    out.push_back(static_cast<Id>(v));
  };
  if (a.arrays() && a.arrays()->ints.count(key)) {
    for (std::int64_t v : a.arrays()->ints.at(key)) put(v);
    return out;
  }
  if (!has(p, key)) return out;
  for (const Json& e : p[key]) {
    if (e.is_array())
      for (const Json& x : e) put(x.get<std::int64_t>());
    else
      put(e.get<std::int64_t>());
  }
  return out;
}

inline bool given(const App& a, const Json& p, const char* key) {
  return has(p, key) || (a.arrays() && (a.arrays()->doubles.count(key) || a.arrays()->ints.count(key)));
}

inline std::vector<Vec3> corner_points(const Mesh& m, const Element& e) {
  std::vector<Vec3> pts;
  for (int k : corner_positions(e.shape)) pts.push_back(m.node(e.nodes[static_cast<std::size_t>(k)]));
  return pts;
}

// 대상 요소: ids 가 있으면 그것, part 가 있으면 그 파트, 둘 다 없으면 전부.
inline std::vector<Id> select_elements(const App& a, const Json& p) {
  const Mesh& m = a.mesh();
  if (given(a, p, "ids")) {
    std::vector<Id> ids = id_list(a, p, "ids");
    for (Id id : ids)
      if (!m.has_element(id)) throw Error("not_found", "요소가 없습니다: " + std::to_string(id), {{"element", id}});
    return ids;
  }
  std::vector<Id> ids;
  const Id part = has(p, "part") ? a.model().get(p["part"].get<Id>()).id : 0;
  for (std::size_t i = 0; i < m.element_count(); ++i)
    if (!part || m.part_at(i) == part) ids.push_back(m.element_ids()[i]);
  return ids;
}

inline Json element_json(const Element& e) {
  const ShapeInfo& info = shape_info(e.shape);
  return Json{{"id", e.id}, {"shape", info.name}, {"type", e.type.empty() ? std::string(info.default_type) : e.type},
              {"part", e.part}, {"nodes", e.nodes}};
}

inline Fields select_params() {
  return {F("ids", "integer_list", "대상 요소(없으면 전체 또는 part)").ex(Json::array({1})),
          F("part", "ref", "이 메시 파트의 요소").ref("mesh_part")};
}

// 절점 순서를 바꿔 방향(쉘 법선, 솔리드 부피의 부호)을 뒤집는다. 2차 요소의 중간 절점도 맞춰 옮긴다.
inline const std::vector<int>& flip_order(Shape s) {
  static const std::map<Shape, std::vector<int>> t = {
      {Shape::Point1, {0}},
      {Shape::Line2, {1, 0}},
      {Shape::Line3, {2, 1, 0}},
      {Shape::Tri3, {0, 2, 1}},
      {Shape::Tri6, {0, 2, 1, 5, 4, 3}},
      {Shape::Quad4, {0, 3, 2, 1}},
      {Shape::Quad8, {0, 3, 2, 1, 7, 6, 5, 4}},
      {Shape::Tet4, {0, 2, 1, 3}},
      {Shape::Tet10, {0, 2, 1, 3, 6, 5, 4, 7, 9, 8}},
      {Shape::Hex8, {0, 3, 2, 1, 4, 7, 6, 5}},
      {Shape::Hex20, {0, 3, 2, 1, 4, 7, 6, 5, 11, 10, 9, 8, 15, 14, 13, 12, 16, 19, 18, 17}},
      {Shape::Wedge6, {0, 2, 1, 3, 5, 4}},
      {Shape::Wedge15, {0, 2, 1, 3, 5, 4, 8, 7, 6, 11, 10, 9, 12, 14, 13}},
      {Shape::Pyramid5, {0, 3, 2, 1, 4}},
  };
  return t.at(s);
}

inline void flip(Element& e) {
  const std::vector<int>& order = flip_order(e.shape);
  std::vector<Id> n(e.nodes.size());
  for (std::size_t i = 0; i < n.size(); ++i) n[i] = e.nodes[static_cast<std::size_t>(order[i])];
  e.nodes = std::move(n);
}

}  // namespace ofep::meshutil
