// 요소 형상 표와 형상 계산.
// 솔버 요소 타입 이름과 면 번호는 CalculiX 2.22 매뉴얼(6.2 Element Types, *DLOAD 의 면 번호)을 따른다.
#include <cmath>

#include "nasa95/error.hpp"
#include "nasa95/mesh.hpp"

namespace nasa95 {

const std::vector<ShapeInfo>& all_shapes() {
  static const std::vector<ShapeInfo> v = {
      {Shape::Point1, "point1", 1, 1, 0, 1, "MASS", {"MASS", "SPRING1", "DCOUP3D"}},
      {Shape::Line2, "line2", 2, 2, 1, 1, "B31",
       {"B31", "B31R", "B21", "T3D2", "T2D2", "SPRINGA", "SPRING2", "DASHPOTA", "GAPUNI", "U1"}},
      {Shape::Line3, "line3", 3, 2, 1, 2, "B32", {"B32", "B32R", "T3D3", "D"}},
      {Shape::Tri3, "tri3", 3, 3, 2, 1, "S3", {"S3", "M3D3", "CPS3", "CPE3", "CAX3", "US3"}},
      {Shape::Tri6, "tri6", 6, 3, 2, 2, "S6", {"S6", "M3D6", "CPS6", "CPE6", "CAX6"}},
      {Shape::Quad4, "quad4", 4, 4, 2, 1, "S4",
       {"S4", "S4R", "M3D4", "M3D4R", "CPS4", "CPS4R", "CPE4", "CPE4R", "CAX4", "CAX4R"}},
      {Shape::Quad8, "quad8", 8, 4, 2, 2, "S8",
       {"S8", "S8R", "M3D8", "M3D8R", "CPS8", "CPS8R", "CPE8", "CPE8R", "CAX8", "CAX8R"}},
      {Shape::Tet4, "tet4", 4, 4, 3, 1, "C3D4", {"C3D4", "F3D4"}},
      {Shape::Tet10, "tet10", 10, 4, 3, 2, "C3D10", {"C3D10", "C3D10T"}},
      {Shape::Hex8, "hex8", 8, 8, 3, 1, "C3D8", {"C3D8", "C3D8R", "C3D8I", "F3D8"}},
      {Shape::Hex20, "hex20", 20, 8, 3, 2, "C3D20", {"C3D20", "C3D20R"}},
      {Shape::Wedge6, "wedge6", 6, 6, 3, 1, "C3D6", {"C3D6", "F3D6"}},
      {Shape::Wedge15, "wedge15", 15, 6, 3, 2, "C3D15", {"C3D15"}},
      {Shape::Pyramid5, "pyramid5", 5, 5, 3, 1, "", {}},  // CalculiX 에는 피라미드 요소가 없다(MSH-38)
  };
  return v;
}

const ShapeInfo& shape_info(Shape s) { return all_shapes()[static_cast<std::size_t>(s)]; }

const ShapeInfo* find_shape(const std::string& name) {
  for (const ShapeInfo& i : all_shapes())
    if (name == i.name) return &i;
  return nullptr;
}

const std::vector<int>& corner_positions(Shape s) {
  static const std::vector<int> line3 = {0, 2};
  static std::vector<std::vector<int>> first;  // 앞에서부터 corners 개
  if (s == Shape::Line3) return line3;
  if (first.empty()) {
    for (const ShapeInfo& i : all_shapes()) {
      std::vector<int> v;
      for (int k = 0; k < i.corners; ++k) v.push_back(k);
      first.push_back(v);
    }
  }
  return first[static_cast<std::size_t>(s)];
}

const std::vector<std::vector<int>>& shape_faces(Shape s) {
  static const std::vector<std::vector<int>> none;
  static const std::vector<std::vector<int>> tri = {{0, 1}, {1, 2}, {2, 0}};
  static const std::vector<std::vector<int>> quad = {{0, 1}, {1, 2}, {2, 3}, {3, 0}};
  static const std::vector<std::vector<int>> tet = {{0, 1, 2}, {0, 3, 1}, {1, 3, 2}, {2, 3, 0}};
  static const std::vector<std::vector<int>> hex = {{0, 1, 2, 3}, {4, 7, 6, 5}, {0, 4, 5, 1},
                                                    {1, 5, 6, 2}, {2, 6, 7, 3}, {3, 7, 4, 0}};
  static const std::vector<std::vector<int>> wedge = {{0, 1, 2}, {3, 4, 5}, {0, 1, 4, 3}, {1, 2, 5, 4}, {2, 0, 3, 5}};
  static const std::vector<std::vector<int>> pyramid = {{0, 1, 2, 3}, {0, 4, 1}, {1, 4, 2}, {2, 4, 3}, {3, 4, 0}};
  switch (s) {
    case Shape::Tri3: case Shape::Tri6: return tri;
    case Shape::Quad4: case Shape::Quad8: return quad;
    case Shape::Tet4: case Shape::Tet10: return tet;
    case Shape::Hex8: case Shape::Hex20: return hex;
    case Shape::Wedge6: case Shape::Wedge15: return wedge;
    case Shape::Pyramid5: return pyramid;
    default: return none;
  }
}

const std::vector<std::array<int, 2>>& shape_edges(Shape s) {
  static const std::vector<std::array<int, 2>> none;
  static const std::vector<std::array<int, 2>> line = {{0, 1}};
  static const std::vector<std::array<int, 2>> tri = {{0, 1}, {1, 2}, {2, 0}};
  static const std::vector<std::array<int, 2>> quad = {{0, 1}, {1, 2}, {2, 3}, {3, 0}};
  static const std::vector<std::array<int, 2>> tet = {{0, 1}, {1, 2}, {2, 0}, {0, 3}, {1, 3}, {2, 3}};
  static const std::vector<std::array<int, 2>> hex = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                                                      {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
  static const std::vector<std::array<int, 2>> wedge = {{0, 1}, {1, 2}, {2, 0}, {3, 4}, {4, 5},
                                                        {5, 3}, {0, 3}, {1, 4}, {2, 5}};
  static const std::vector<std::array<int, 2>> pyramid = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {0, 4}, {1, 4}, {2, 4}, {3, 4}};
  switch (s) {
    case Shape::Line2: case Shape::Line3: return line;
    case Shape::Tri3: case Shape::Tri6: return tri;
    case Shape::Quad4: case Shape::Quad8: return quad;
    case Shape::Tet4: case Shape::Tet10: return tet;
    case Shape::Hex8: case Shape::Hex20: return hex;
    case Shape::Wedge6: case Shape::Wedge15: return wedge;
    case Shape::Pyramid5: return pyramid;
    default: return none;
  }
}

namespace {

Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 cross(const Vec3& a, const Vec3& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double norm(const Vec3& a) { return std::sqrt(dot(a, a)); }

// 사면체의 부호 있는 부피. 표준 절점 순서에서 양수.
double tet_volume(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d) {
  return dot(sub(b, a), cross(sub(c, a), sub(d, a))) / 6.0;
}

}  // namespace

Vec3 face_normal_area(const std::vector<Vec3>& p) {
  Vec3 n{0, 0, 0};
  for (std::size_t i = 1; i + 1 < p.size(); ++i) {
    const Vec3 c = cross(sub(p[i], p[0]), sub(p[i + 1], p[0]));
    n[0] += c[0] / 2, n[1] += c[1] / 2, n[2] += c[2] / 2;
  }
  return n;
}

// 꼭짓점만으로 계산한다. 2차 요소의 굽은 변은 반영하지 않는다(곡면 요소에서는 근사값).
double element_size(const ShapeInfo& info, const std::vector<Vec3>& p) {
  switch (info.shape) {
    case Shape::Point1: return 0.0;
    case Shape::Line2: case Shape::Line3: return norm(sub(p[1], p[0]));
    case Shape::Tri3: case Shape::Tri6: return norm(face_normal_area({p[0], p[1], p[2]}));
    case Shape::Quad4: case Shape::Quad8: return norm(face_normal_area({p[0], p[1], p[2], p[3]}));
    case Shape::Tet4: case Shape::Tet10: return tet_volume(p[0], p[1], p[2], p[3]);
    case Shape::Hex8: case Shape::Hex20:
      return tet_volume(p[0], p[1], p[2], p[6]) + tet_volume(p[0], p[2], p[3], p[6]) + tet_volume(p[0], p[3], p[7], p[6]) +
             tet_volume(p[0], p[7], p[4], p[6]) + tet_volume(p[0], p[4], p[5], p[6]) + tet_volume(p[0], p[5], p[1], p[6]);
    case Shape::Wedge6: case Shape::Wedge15:
      return tet_volume(p[0], p[1], p[2], p[3]) + tet_volume(p[1], p[2], p[3], p[4]) + tet_volume(p[2], p[3], p[4], p[5]);
    case Shape::Pyramid5: return tet_volume(p[0], p[1], p[2], p[4]) + tet_volume(p[0], p[2], p[3], p[4]);
  }
  return 0.0;
}

}  // namespace nasa95

namespace nasa95 {

std::vector<double> interpolate_points(const std::vector<Vec3>& points, const std::vector<double>& values, const std::vector<Vec3>& at,
                                       const std::string& method, double power, double radius) {
  if (points.empty() || points.size() != values.size()) throw Error("invalid_param", "점 자료가 비었거나 값의 수가 맞지 않습니다", {{"param", "points"}});
  if (method != "nearest" && method != "idw") throw Error("invalid_param", "보간 방법은 nearest 또는 idw 입니다", {{"param", "method"}});
  std::vector<double> out;
  out.reserve(at.size());
  for (const Vec3& q : at) {
    double best = 1e300;
    std::size_t nearest = 0;
    for (std::size_t i = 0; i < points.size(); ++i) {
      const double dx = points[i][0] - q[0], dy = points[i][1] - q[1], dz = points[i][2] - q[2];
      const double d2 = dx * dx + dy * dy + dz * dz;
      if (d2 < best) best = d2, nearest = i;
    }
    if (method == "nearest" || best == 0.0) {
      out.push_back(values[nearest]);
      continue;
    }
    double wsum = 0, vsum = 0;
    for (std::size_t i = 0; i < points.size(); ++i) {
      const double dx = points[i][0] - q[0], dy = points[i][1] - q[1], dz = points[i][2] - q[2];
      const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
      if (radius > 0 && d > radius) continue;
      const double w = 1.0 / std::pow(d, power);
      wsum += w, vsum += w * values[i];
    }
    out.push_back(wsum > 0 ? vsum / wsum : values[nearest]);  // 반지름 안에 점이 없으면 가장 가까운 점
  }
  return out;
}

}  // namespace nasa95
