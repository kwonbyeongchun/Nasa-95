#pragma once
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ofep/json.hpp"
#include "ofep/model.hpp"

namespace ofep {

// 객체 속성(= 명령 매개변수) 하나의 정의. 명령 등록부·입력 검증·문서가 모두 이것에서 나온다.
//
// type
//   number | integer | string | bool
//   vector3        숫자 3개
//   number_list | integer_list | string_list
//   table          숫자 행의 목록. cols 가 0 이 아니면 행마다 그 개수여야 한다
//   target         적용 대상. {"type": nodes|elements|faces|set|geometry, "ids": [...]}
//                  faces 의 ids 는 [요소, 면 번호] 쌍, set 의 ids 는 셋 객체의 ID
//   ref | ref_list 다른 객체의 ID
//   object_list    객체의 목록. 항목의 속성은 items 로 정의한다(안에 ref 를 둘 수 있다)
//   array | object 형식을 정하지 않은 값(되도록 쓰지 않는다)
//   any            명령 매개변수 전용: 호출 수준에서는 타입을 보지 않는다(update 에서 하위 종류마다 타입이 다른 속성)
struct FieldSpec {
  std::string name;
  std::string type;
  std::string desc;
  std::string ref_kind;  // ref / ref_list 가 가리키는 객체 종류(비면 아무 종류)
  std::string dim;       // 물리량 차원(단위 환산용). 예: "length", "pressure"
  bool required = false; // 없으면 "미완성"(validate 에서 보고). 생성 자체는 막지 않는다
  bool must = false;     // 호출 시 반드시 줘야 한다(없으면 missing_param)
  // 범위. hard 는 입력 거부, soft 는 저장은 하되 validate 에서 오류로 보고(물리적 범위 위반)
  std::optional<double> min, max, soft_min, soft_max;
  bool min_excl = false, max_excl = false, soft_min_excl = false, soft_max_excl = false;
  std::vector<std::string> choices;  // string: 허용 값. target: 허용되는 대상 종류
  int cols = 0;                      // table 의 열 수(0 = 제한 없음)
  bool scalable = false;             // 하중의 크기를 나타내는 값(계수를 곱해 조합할 수 있다)
  // 값이 메시의 노드·요소 번호일 때: "node" / "element". 표이면 mesh_col 번째 열이 번호다(-1 = 값 전체).
  // 재번호·노드 병합 때 함께 바뀌고, 없는 번호를 가리키면 검사에서 지목된다.
  std::string mesh_ref;
  int mesh_col = -1;
  std::vector<FieldSpec> items;      // object_list 항목의 속성
  Json example;                      // 유효한 값의 예(문서·테스트용)

  FieldSpec(std::string n, std::string t, std::string d) : name(std::move(n)), type(std::move(t)), desc(std::move(d)) {}
  FieldSpec& req() { required = true; return *this; }
  FieldSpec& call_req() { must = true; return *this; }
  FieldSpec& ref(std::string k) { ref_kind = std::move(k); return *this; }
  FieldSpec& unit(std::string d) { dim = std::move(d); return *this; }
  FieldSpec& ge(double v) { min = v; min_excl = false; return *this; }
  FieldSpec& gt(double v) { min = v; min_excl = true; return *this; }
  FieldSpec& le(double v) { max = v; max_excl = false; return *this; }
  FieldSpec& lt(double v) { max = v; max_excl = true; return *this; }
  FieldSpec& soft_gt(double v) { soft_min = v; soft_min_excl = true; return *this; }
  FieldSpec& soft_ge(double v) { soft_min = v; soft_min_excl = false; return *this; }
  FieldSpec& soft_lt(double v) { soft_max = v; soft_max_excl = true; return *this; }
  FieldSpec& soft_le(double v) { soft_max = v; soft_max_excl = false; return *this; }
  FieldSpec& one_of(std::vector<std::string> c) { choices = std::move(c); return *this; }
  FieldSpec& columns(int n) { cols = n; return *this; }
  FieldSpec& scales() { scalable = true; return *this; }
  FieldSpec& nodes() { mesh_ref = "node"; return *this; }
  FieldSpec& elems() { mesh_ref = "element"; return *this; }
  FieldSpec& node_col(int c) { mesh_ref = "node"; mesh_col = c; return *this; }
  FieldSpec& elem_col(int c) { mesh_ref = "element"; mesh_col = c; return *this; }
  FieldSpec& of(std::vector<FieldSpec> f) { items = std::move(f); return *this; }
  FieldSpec& ex(Json e) { example = std::move(e); return *this; }

  Json to_json() const;
};

using Fields = std::vector<FieldSpec>;

// 객체 종류의 정의.
struct KindSpec {
  std::string kind;        // 예: "material"
  std::string label;       // 한글 이름(자동 이름·문서용)
  std::string collection;  // Python 컬렉션 이름. 예: "materials"
  std::vector<std::string> parents;  // 허용되는 상위 객체 종류. 비면 최상위에만 둔다
  bool solver_name = false;          // 솔버 이름 규칙을 적용한다(덱에 이름이 그대로 나가는 객체)
  bool suppressible = true;          // 억제(suppress) 동작이 있는가
  bool copyable = true;              // 복제(copy) 동작이 있는가
  Fields fields;                     // 모든 하위 종류 공통 속성
  std::vector<std::pair<std::string, Fields>> subtypes;  // 하위 종류별 속성. 비면 하위 종류 없음
  std::string features;              // 대응 기능 ID(문서용)
  // 생성·수정 직후의 추가 검사. 위반이면 Error 를 던진다.
  std::function<void(const Model&, const Object&)> check;
  // validate 용 추가 진단. issues 배열에 덧붙인다.
  std::function<void(const Model&, const Object&, Json& issues)> diagnose;

  bool has_subtype(const std::string& s) const;
  Fields fields_for(const std::string& subtype) const;  // 공통 + 하위 종류
  Json to_json() const;
};

class Schema {
 public:
  void add(KindSpec k);
  const KindSpec* find(const std::string& kind) const;
  const KindSpec& get(const std::string& kind) const;
  const std::vector<KindSpec>& all() const { return kinds_; }

 private:
  std::vector<KindSpec> kinds_;
};

// 값이 타입에 맞는지(모델 조회 없이).
bool type_matches(const std::string& type, const Json& v);
// 타입·선택지·hard 범위·형식·참조 대상을 검사한다. 위반이면 Error. model 이 nullptr 이면 참조 검사는 건너뛴다.
void check_value(const FieldSpec& f, const Json& v, const Model* model);
// 객체가 가리키는 다른 객체의 ID 를 훑는다(ref, ref_list, target 의 셋, object_list 안의 참조).
void for_each_ref(const Fields& fields, const Json& props, const std::function<void(const FieldSpec&, Id)>& fn);
// 참조 ID 를 바꾼다(복제 시). map 에 없는 ID 는 그대로 둔다.
void remap_refs(const Fields& fields, Json& props, const std::map<Id, Id>& map);
// 객체가 가리키는 메시 번호(노드·요소)를 훑는다. fn("node"|"element", 번호 값) — 값을 바꿀 수 있다.
// 표시된 속성(mesh_ref)과 적용 대상(target 의 nodes·elements·faces)을 본다.
void for_each_mesh_ref(const Fields& fields, Json& props, const std::function<void(const std::string&, Json&)>& fn);
// 속성 값에서 물리적 범위(soft)를 벗어난 곳을 찾는다. fn(속성 경로)
void for_each_soft_violation(const Fields& fields, const Json& props, const std::string& prefix,
                             const std::function<void(const std::string&)>& fn);

// 내장 객체 종류를 등록한다(kinds_*.cpp).
void register_builtin_kinds(Schema& schema);

// 재료의 구성 모델(탄성·밀도 …). 재료는 구성 모델의 묶음이며 props.behaviors 에 둔다.
struct BehaviorSpec {
  using Report = std::function<void(const std::string& field, const std::string& message)>;

  std::string name;   // 예: "elastic"
  std::string label;
  Fields fields;
  std::string features;
  std::vector<std::string> requires_all;  // 함께 있어야 하는 구성 모델(없으면 진단). 예: plastic → elastic
  // `data` 표의 행당 상수 개수(온도 열 제외). 0 이면 표를 주지 않는다. 비어 있으면 형식을 검사하지 않는다.
  std::function<int(const Json& params)> row_size;
  bool curve = false;  // 온도별 곡선: [값, x, (온도)] — 같은 온도 안에서 x 가 오름차순
  // 물리적 범위 진단(저장은 하되 validate 에서 오류로 보고).
  std::function<void(const Json& params, const Report& report)> diagnose;
  // 단위계 변환(CMN-11)용: `data` 표의 열마다 물리량 차원(온도 열 제외, "" = 무차원). 비어 있으면 변환하지 못한다
  std::function<std::vector<std::string>(const Json& params)> column_dims;
};
const std::vector<BehaviorSpec>& material_behaviors();

}  // namespace ofep
