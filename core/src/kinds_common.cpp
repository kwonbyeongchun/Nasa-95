// 공통 정의: 좌표계, 방향, 함수, 셋, 매개변수, 출력 시점
#include <array>
#include <cmath>

#include "kinds.hpp"
#include "nasa95/error.hpp"

namespace nasa95::kinds {

namespace {

std::array<double, 3> vec3(const Json& v) { return {v[0].get<double>(), v[1].get<double>(), v[2].get<double>()}; }

// 세 점(원점 a, 1축 위의 점 b, 1-2 평면 위의 점 c)이 좌표계를 정의할 수 있는지.
void check_three_points(const Object& o, const char* ka, const char* kb, const char* kc) {
  const Json& p = o.props;
  if (!p.contains(ka) || !p.contains(kb) || !p.contains(kc)) return;
  const auto a = vec3(p[ka]), b = vec3(p[kb]), c = vec3(p[kc]);
  const double u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
  const double v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
  const double n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
  const double nn = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
  const double uu = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
  const double vv = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (uu == 0.0 || vv == 0.0 || nn <= 1e-12 * uu * vv)
    throw Error("invalid_geometry", "좌표계의 두 축 방향이 평행하거나 길이가 0 입니다", {{"object", o.id}});
}

Fields csys_fields() {
  return {F("origin", "vector3", "원점").req().unit("length").ex({0.0, 0.0, 0.0}),
          F("axis1_point", "vector3", "1축(x 또는 반경) 위의 점").req().unit("length").ex({1.0, 0.0, 0.0}),
          F("plane12_point", "vector3", "1-2 평면 위의 점").req().unit("length").ex({0.0, 1.0, 0.0})};
}

// 솔버의 방향 정의(*ORIENTATION): 점 a, b 와 선택적 추가 회전.
Fields orientation_fields() {
  return {F("a", "vector3", "점 a (직교: 1축 위, 원통: 축 위의 한 점)").req().unit("length").ex({1.0, 0.0, 0.0}),
          F("b", "vector3", "점 b (직교: 1-2 평면 위, 원통: 축 위의 다른 점)").req().unit("length").ex({0.0, 1.0, 0.0}),
          F("rotation_axis", "integer", "추가 회전의 축(1~3)").ge(1).le(3),
          F("rotation_angle", "number", "추가 회전각(도)")};
}

void check_function(const Model&, const Object& o) {
  // 표의 x(또는 시간)는 오름차순이어야 한다.
  auto it = o.props.find("points");
  if (it == o.props.end()) return;
  for (std::size_t i = 1; i < it->size(); ++i)
    if ((*it)[i][0].get<double>() <= (*it)[i - 1][0].get<double>())
      throw Error("invalid_order", "함수 표의 첫 열은 오름차순이어야 합니다",
                  {{"object", o.id}, {"param", "points"}, {"row", i}});
}

}  // namespace

void register_common(Schema& s) {
  {
    KindSpec k;
    k.kind = "csys", k.label = "좌표계", k.collection = "csys", k.features = "CMN-02";
    k.subtypes = {{"rectangular", csys_fields()}, {"cylindrical", csys_fields()}, {"spherical", csys_fields()}};
    k.check = [](const Model&, const Object& o) { check_three_points(o, "origin", "axis1_point", "plane12_point"); };
    s.add(std::move(k));
  }
  {
    KindSpec k;
    k.kind = "orientation", k.label = "방향", k.collection = "orientations", k.features = "PRP-23, MAT-10";
    k.solver_name = true;
    Fields dist = {F("system", "string", "좌표계 종류").one_of({"rectangular", "cylindrical"}).req(),
                   F("default_a", "vector3", "기본 방향의 점 a").req().ex({1.0, 0.0, 0.0}),
                   F("default_b", "vector3", "기본 방향의 점 b").req().ex({0.0, 1.0, 0.0}),
                   F("entries", "object_list", "요소별 방향")
                       .of({target("elements", "대상 요소", kElements).call_req(),
                            F("a", "vector3", "점 a").call_req().ex({1.0, 0.0, 0.0}),
                            F("b", "vector3", "점 b").call_req().ex({0.0, 0.0, 1.0})})};
    k.subtypes = {{"rectangular", orientation_fields()}, {"cylindrical", orientation_fields()}, {"distribution", dist}};
    s.add(std::move(k));
  }
  {
    KindSpec k;
    k.kind = "function", k.label = "함수", k.collection = "functions", k.features = "CMN-03";
    k.solver_name = true;
    k.subtypes = {
        {"table", {F("points", "table", "[x, y] 쌍의 목록(x 오름차순)").columns(2).req().ex({{0.0, 0.0}, {1.0, 1.0}})}},
        {"expression", {F("expression", "string", "변수 x, y, z, t 의 수식").req().ex("2*x")}},
        {"amplitude",
         {F("points", "table", "[시간, 값] 쌍의 목록(시간 오름차순)").columns(2).req().ex({{0.0, 0.0}, {1.0, 1.0}}),
          F("time", "string", "기준 시간").one_of({"step", "total"})}},
    };
    k.check = check_function;
    s.add(std::move(k));
  }
  {
    KindSpec k;
    k.kind = "set", k.label = "셋", k.collection = "sets", k.features = "CMN-04";
    k.solver_name = true;
    k.subtypes = {
        {"node", {F("ids", "integer_list", "노드 ID").req().ge(1).nodes().ex({1, 2, 3})}},
        {"element", {F("ids", "integer_list", "요소 ID").req().ge(1).elems().ex({1, 2})}},
        {"surface", {F("faces", "table", "[요소, 면 번호] 쌍").columns(2).req().elem_col(0).ex({{1, 1}, {2, 1}})}},
        {"node_surface", {F("ids", "integer_list", "면을 이루는 노드 ID").req().ge(1).nodes().ex({1, 2, 3})}},
        {"geometry", {F("entities", "integer_list", "형상 엔티티 ID").req().ge(1).ex(Json::array({1}))}},
    };
    s.add(std::move(k));
  }
  {
    KindSpec k;  // 요소의 묶음. 요소가 자기 파트의 ID 를 갖는다
    k.kind = "mesh_part", k.label = "메시 파트", k.collection = "mesh_parts", k.features = "MSH-01";
    k.solver_name = true;
    k.copyable = false;  // 객체만 복제하면 요소 없는 파트가 된다. 요소 복제는 mesh.transform(copy) 로 한다
    k.fields = {F("description", "string", "설명").ex("브래킷"),
                // 형상 파트를 메싱해 만든 메시 파트(mesh.generate 가 채운다)
                F("geometry", "ref", "이 메시를 만든 형상 파트").ref("part"),
                F("mesh_params", "any", "메싱에 쓴 설정(다시 메싱할 때 쓴다)").ex(Json::object()),
                F("geometry_digest", "string", "메싱할 때의 형상 요약값(형상이 바뀌었는지 판단)").ex(""),
                F("association", "any", "형상-메시 연관: 형상의 솔리드·면·모서리·꼭짓점에 놓인 요소·요소면·노드").ex(Json::object())};
    s.add(std::move(k));
  }
  {
    KindSpec k;  // 메시 제어(MSH-04): 형상 파트·엔티티에 크기·분할·곡률·편향을 둔다. mesh.generate 가 파트에 해당하는 것을 모아 쓴다
    k.kind = "mesh_control", k.label = "메시 제어", k.collection = "mesh_controls", k.features = "MSH-04";
    const F parts = F("parts", "ref_list", "적용할 형상 파트(없으면 전부)").ref("part");
    const F edges = F("target", "target", "대상 모서리(형상)").req().ex(Json{{"type", "geometry"}, {"ids", Json::array({Json::array({1, "edge", 1})})}});
    k.subtypes = {
        {"global_size",  // 전역 크기: 상한·하한·크기 변화율
         {parts, F("size", "number", "요소 크기 상한").req().gt(0).unit("length").ex(5.0), F("min_size", "number", "하한").ge(0).unit("length"),
          F("grading", "number", "크기가 변하는 빠르기(0~1)").gt(0).le(1)}},
        {"edge_division",  // 모서리 분할 수(+편향). 사면체 메셔는 길이/분할수를 그 모서리의 크기로 쓴다
         {edges, F("divisions", "integer", "분할 수").req().ge(1).ex(10),
          F("bias", "number", "편향 비(마지막 간격 / 첫 간격, 1 = 고르게)").gt(0).ex(1.0)}},
        {"local_size",  // 면·모서리·꼭짓점의 크기
         {F("target", "target", "대상 면·모서리·꼭짓점(형상)").req().ex(Json{{"type", "geometry"}, {"ids", Json::array({Json::array({1, "face", 1})})}}),
          F("size", "number", "요소 크기").req().gt(0).unit("length").ex(2.0)}},
        {"curvature",  // 곡률 기반: 곡률 반지름당 요소 수(메셔의 curvature safety)
         {parts, F("safety", "number", "곡률 반지름당 요소 수(클수록 곡면이 촘촘)").req().ge(0.1).ex(2.0),
          F("min_size", "number", "하한").ge(0).unit("length")}},
        {"bias",  // 모서리 편향(매핑 메싱용. 사면체 메셔는 무시한다)
         {edges, F("ratio", "number", "편향 비(마지막 간격 / 첫 간격)").req().gt(0).ex(4.0)}},
    };
    s.add(std::move(k));
  }
  {
    KindSpec k;
    k.kind = "parameter", k.label = "매개변수", k.collection = "parameters", k.features = "GEO-43";
    k.suppressible = false;
    k.fields = {F("value", "number", "값"), F("expression", "string", "수식").ex("H/2")};
    s.add(std::move(k));
  }
  {
    KindSpec k;
    k.kind = "time_points", k.label = "출력 시점", k.collection = "time_points", k.features = "CAS-19";
    k.solver_name = true, k.suppressible = false;
    k.fields = {F("times", "number_list", "출력할 시각(오름차순)").req().ex({0.25, 0.5, 1.0}),
                F("time", "string", "기준 시간").one_of({"step", "total"})};
    s.add(std::move(k));
  }
}

}  // namespace nasa95::kinds
