#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "nasa95/json.hpp"
#include "nasa95/model.hpp"

namespace nasa95 {

// 요소의 형상(절점 배치). 솔버 요소 타입(C3D8R 등)과는 별개 속성이다(MSH-26).
enum class Shape : std::uint8_t {
  Point1, Line2, Line3, Tri3, Tri6, Quad4, Quad8, Tet4, Tet10, Hex8, Hex20, Wedge6, Wedge15, Pyramid5
};

struct ShapeInfo {
  Shape shape;
  const char* name;                 // "hex8"
  int nodes;                        // 절점 수
  int corners;                      // 꼭짓점 절점 수
  int dim;                          // 0 점, 1 선, 2 면, 3 체적
  int order;                        // 1차 / 2차
  const char* default_type;         // 기본 솔버 요소 타입("" = 솔버에 없음)
  std::vector<const char*> types;   // 쓸 수 있는 솔버 요소 타입
};

const std::vector<ShapeInfo>& all_shapes();
const ShapeInfo& shape_info(Shape s);
const ShapeInfo* find_shape(const std::string& name);  // 없으면 nullptr
// 형상의 면(3D)·변(2D)을 이루는 꼭짓점의 위치(0 부터). 번호 순서는 CalculiX 의 면 번호와 같다.
const std::vector<std::vector<int>>& shape_faces(Shape s);
// 꼭짓점 절점의 위치(요소 절점 목록 안에서). 선 요소의 2차형은 (끝, 가운데, 끝) 순이다.
const std::vector<int>& corner_positions(Shape s);
// 꼭짓점 사이의 변.
const std::vector<std::array<int, 2>>& shape_edges(Shape s);

using Vec3 = std::array<double, 3>;

struct Element {
  Id id = 0;
  Shape shape = Shape::Hex8;
  Id part = 0;             // 메시 파트(mesh_part 객체)의 ID. 0 = 없음
  std::string type;        // 솔버 요소 타입. 비면 형상의 기본 타입
  std::vector<Id> nodes;   // 절점 ID
};

// 메시 변경 1건(묶음 단위). Undo 는 기록의 역순으로 되돌린다.
struct MeshOp {
  enum Kind { AddNodes, RemoveNodes, MoveNodes, AddElements, RemoveElements, ReplaceElements } kind;
  std::vector<Id> ids;                // 노드 ID (노드 연산)
  std::vector<double> xyz, old_xyz;   // 노드 좌표(새 값 / 이전 값)
  std::vector<Element> elems, old_elems;
};
using MeshLog = std::vector<MeshOp>;

// 배열 기반 메시 저장소. 노드와 요소는 각각 ID 오름차순으로 둔다(조회는 이진 탐색, 배열 보기는 항상 같은 순서).
class Mesh {
 public:
  // --- 노드
  std::size_t node_count() const { return node_ids_.size(); }
  const std::vector<Id>& node_ids() const { return node_ids_; }
  const std::vector<double>& node_xyz() const { return node_xyz_; }  // 3 × 노드 수
  std::ptrdiff_t find_node(Id id) const;  // 위치. 없으면 -1
  bool has_node(Id id) const { return find_node(id) >= 0; }
  Vec3 node(Id id) const;                 // 없으면 Error("not_found")
  Id max_node_id() const { return node_ids_.empty() ? 0 : node_ids_.back(); }

  void add_nodes(const std::vector<Id>& ids, const std::vector<double>& xyz);  // 이미 있는 ID 면 Error
  void remove_nodes(const std::vector<Id>& ids);                              // 요소가 쓰는 노드면 Error
  void move_nodes(const std::vector<Id>& ids, const std::vector<double>& xyz);

  // --- 요소
  std::size_t element_count() const { return elem_ids_.size(); }
  const std::vector<Id>& element_ids() const { return elem_ids_; }
  std::ptrdiff_t find_element(Id id) const;
  bool has_element(Id id) const { return find_element(id) >= 0; }
  Element element(Id id) const;                 // 없으면 Error("not_found")
  Element element_at(std::size_t index) const;
  Shape shape_at(std::size_t index) const { return elem_shape_[index]; }
  Id part_at(std::size_t index) const { return elem_part_[index]; }
  const Id* nodes_at(std::size_t index) const { return conn_.data() + elem_offset_[index]; }
  std::size_t node_count_at(std::size_t index) const { return elem_offset_[index + 1] - elem_offset_[index]; }
  Id max_element_id() const { return elem_ids_.empty() ? 0 : elem_ids_.back(); }

  void add_elements(const std::vector<Element>& elems);     // 절점이 없거나 ID 가 겹치면 Error
  void remove_elements(const std::vector<Id>& ids);
  void replace_elements(const std::vector<Element>& elems); // 같은 ID 의 요소를 바꾼다

  // 요소가 쓰는 노드인지(노드 삭제 검사용).
  std::vector<Id> used_nodes() const;  // 오름차순, 중복 없음

  // --- 변경 기록
  void begin_record();
  MeshLog end_record();
  bool recording() const { return recording_; }
  void apply_inverse(const MeshLog& log);
  void apply_forward(const MeshLog& log);

  void clear();
  bool empty() const { return node_ids_.empty() && elem_ids_.empty(); }
  Json to_json() const;
  void load_json(const Json& j);
  std::string digest() const;

 private:
  void raw_add_nodes(const std::vector<Id>& ids, const std::vector<double>& xyz);
  void raw_remove_nodes(const std::vector<Id>& ids);
  void raw_move_nodes(const std::vector<Id>& ids, const std::vector<double>& xyz);
  void raw_add_elements(const std::vector<Element>& elems);
  void raw_remove_elements(const std::vector<Id>& ids);
  void rebuild_elements(std::vector<Element> all);
  std::vector<Element> all_elements() const;
  std::uint16_t intern(const std::string& type);

  std::vector<Id> node_ids_;
  std::vector<double> node_xyz_;

  std::vector<Id> elem_ids_;
  std::vector<Shape> elem_shape_;
  std::vector<Id> elem_part_;
  std::vector<std::uint16_t> elem_type_;     // types_ 의 위치
  std::vector<std::size_t> elem_offset_{0};  // 요소 수 + 1
  std::vector<Id> conn_;
  std::vector<std::string> types_{""};

  bool recording_ = false;
  MeshLog log_;
};

// --- 형상 계산(꼭짓점 기준)
double element_size(const ShapeInfo& info, const std::vector<Vec3>& p);  // 길이 / 넓이 / 부피(부호 있음)
Vec3 face_normal_area(const std::vector<Vec3>& p);                       // 다각형 면의 넓이 벡터(법선 × 넓이)

// 점 자료(위치·값)를 지정 위치들로 보간한다(LOD-14 외부 필드 매핑). method: "nearest"(가장 가까운 점) 또는
// "idw"(역거리 가중, power 승; radius > 0 이면 그 안의 점만, 없으면 가장 가까운 점). 점이 없으면 Error.
std::vector<double> interpolate_points(const std::vector<Vec3>& points, const std::vector<double>& values, const std::vector<Vec3>& at,
                                       const std::string& method, double power = 2.0, double radius = 0.0);

}  // namespace nasa95
