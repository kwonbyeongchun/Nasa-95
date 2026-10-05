// 솔버 백엔드 등록부(solver.hpp): CalculiX·OpenSees·MyStran 의 실행 방법과 지원 범위.
#include "nasa95/solver.hpp"

#include <algorithm>

#include "nasa95/app.hpp"
#include "nasa95/error.hpp"
#include "nasa95/mesh.hpp"

namespace nasa95 {

namespace {

std::string subtype_of(const Object& o) { return o.props.value("type", std::string()); }

std::vector<SolverSpec> make_specs() {
  std::vector<SolverSpec> v;
  {
    SolverSpec s;
    s.name = "calculix", s.label = "CalculiX", s.env_var = "NASA95_CCX", s.setting_key = "solver_executable", s.deck_extension = "inp";
    s.license = "GPL — 별도 실행 파일로만 실행(링크·동봉 배포 안 함)";
    s.restart = true, s.all = true, s.contact = true, s.nlgeom = true;
    s.run_args = [](const std::string& job) { return std::vector<std::string>{"-i", job}; };
    s.write = [](const App& a, const Object& cs, const DeckOptions& o) { return write_calculix_deck(a, cs, o); };
    v.push_back(std::move(s));
  }
  {
    SolverSpec s;
    s.name = "opensees", s.label = "OpenSees", s.env_var = "NASA95_OPENSEES", s.setting_key = "opensees_executable", s.deck_extension = "tcl";
    s.license = "오픈소스 아님(비상업·내부 목적만) — 사용자가 설치한 실행 파일을 실행만 하고 동봉 배포하지 않음(D16)";
    s.step_types = {"static", "frequency", "dynamic"};
    s.element_shapes = {"point1", "line2"};
    s.load_types = {"force", "moment", "gravity", "line_load", "base_motion"};
    s.bc_types = {"displacement", "symmetry", "base_motion"};  // deck_opensees.cpp 가 쓰는 것과 같게 유지
    s.property_types = {"beam", "truss", "mass"};
    s.material_behaviors = {"elastic", "density"};
    s.nlgeom = true;
    s.run_args = [](const std::string& job) { return std::vector<std::string>{job + ".tcl"}; };
    s.write = [](const App& a, const Object& cs, const DeckOptions& o) { return write_opensees_deck(a, cs, o); };
    v.push_back(std::move(s));
  }
  {
    SolverSpec s;
    s.name = "mystran", s.label = "MyStran", s.env_var = "NASA95_MYSTRAN", s.setting_key = "mystran_executable", s.deck_extension = "bdf";
    s.license = "MIT(GitHub MYSTRANsolver/MYSTRAN, 2026-10-05 확인) — 별도 실행 파일. 선형 정적·고유치·좌굴만";
    s.step_types = {"static", "frequency", "buckle"};
    s.element_shapes = {"point1", "line2", "tri3", "quad4", "tet4", "tet10", "hex8", "hex20", "wedge6"};
    s.load_types = {"force", "moment", "pressure", "gravity", "line_load", "temperature"};
    s.bc_types = {"displacement", "symmetry", "antisymmetry"};
    s.property_types = {"beam", "truss", "shell", "solid", "mass", "spring"};
    s.material_behaviors = {"elastic", "density", "expansion"};
    s.constraint_types = {"rigid_body", "equation"};
    s.run_args = [](const std::string& job) { return std::vector<std::string>{job + ".bdf"}; };
    s.write = [](const App& a, const Object& cs, const DeckOptions& o) { return write_mystran_deck(a, cs, o); };
    s.after_run = [](const App& a, const Object& cs, const std::string& work, const std::string& job) { convert_f06_to_frd(a, cs, work, job); };
    v.push_back(std::move(s));
  }
  return v;
}

}  // namespace

const std::vector<SolverSpec>& solver_specs() {
  static const std::vector<SolverSpec> specs = make_specs();
  return specs;
}

const SolverSpec& solver_spec(const std::string& name) {
  for (const SolverSpec& s : solver_specs())
    if (s.name == name) return s;
  Json known = Json::array();
  for (const SolverSpec& s : solver_specs()) known.push_back(s.name);
  throw Error("not_supported", "모르는 솔버입니다: " + name, {{"solver", name}, {"known", known}});
}

Json solver_capabilities(const SolverSpec& s) {
  auto list = [](const std::set<std::string>& v) { return Json(std::vector<std::string>(v.begin(), v.end())); };
  return Json{{"name", s.name},
              {"label", s.label},
              {"deck_extension", s.deck_extension},
              {"env_var", s.env_var},
              {"setting_key", s.setting_key},
              {"license", s.license},
              {"restart", s.restart},
              {"all", s.all},
              {"contact", s.contact},
              {"nlgeom", s.nlgeom},
              {"step_types", list(s.step_types)},
              {"element_shapes", list(s.element_shapes)},
              {"load_types", list(s.load_types)},
              {"bc_types", list(s.bc_types)},
              {"property_types", list(s.property_types)},
              {"material_behaviors", list(s.material_behaviors)},
              {"constraint_types", list(s.constraint_types)}};
}

Json solver_support_issues(const App& a, const Object& cs) {
  Json issues = Json::array();
  const SolverSpec& s = solver_spec(cs.props.value("solver", std::string("calculix")));
  if (s.all) return issues;
  auto issue = [&](const char* code, const Object& o, const std::string& what) {
    issues.push_back(Json{{"severity", "error"}, {"code", code}, {"object", o.id}, {"kind", o.kind}, {"name", o.name}, {"field", ""},
                          {"message", s.label + " 솔버가 지원하지 않습니다: " + what}, {"solver", s.name}});
  };
  const char* code = "unsupported_by_solver";
  for (const Object* st : a.model().children(cs.id, "step")) {
    if (st->suppressed) continue;
    const std::string t = subtype_of(*st);
    if (!s.step_types.count(t)) issue(code, *st, "스텝 종류 " + t);
    if (st->props.value("nlgeom", false) && !s.nlgeom) issue(code, *st, "기하 비선형(nlgeom)");
    for (const Object& l : step_entries(a, *st, "load"))
      if (!s.load_types.count(subtype_of(l))) issue(code, l, "하중 종류 " + subtype_of(l));
    for (const Object& b : step_entries(a, *st, "bc"))
      if (!s.bc_types.count(subtype_of(b))) issue(code, b, "경계조건 종류 " + subtype_of(b));
  }
  // 모델 정의: 케이스 범위 안의 요소 형상, 프로퍼티·재료·구속·접촉
  std::set<std::string> shapes;
  const Mesh& m = a.mesh();
  for (std::size_t i = 0; i < m.element_count(); ++i) shapes.insert(shape_info(m.shape_at(i)).name);
  for (const std::string& sh : shapes)
    if (!s.element_shapes.count(sh)) issue(code, cs, "요소 형상 " + sh);
  for (const Object* p : a.model().by_kind("property"))
    if (!p->suppressed && !s.property_types.count(subtype_of(*p))) issue(code, *p, "프로퍼티 종류 " + subtype_of(*p));
  for (const Object* mt : a.model().by_kind("material")) {
    if (mt->suppressed) continue;
    auto bit = mt->props.find("behaviors");
    if (bit == mt->props.end()) continue;
    // 구조 해석의 강성·질량을 바꾸는 구성 모델만 본다. 열·전기·평가용 항목은 그 솔버가 안 써도 결과가 달라지지 않는다
    static const std::set<std::string> harmless = {"allowable", "expansion", "conductivity", "specific_heat", "structural_damping", "electrical_conductivity",
                                                   "magnetic_permeability", "fluid_constants", "specific_gas_constant", "depvar"};
    for (auto it = bit->begin(); it != bit->end(); ++it)
      if (!harmless.count(it.key()) && !s.material_behaviors.count(it.key())) issue(code, *mt, "재료 구성 모델 " + it.key());
  }
  for (const Object* c : a.model().by_kind("constraint"))
    if (!c->suppressed && !s.constraint_types.count(subtype_of(*c))) issue(code, *c, "구속 종류 " + subtype_of(*c));
  if (!s.contact)
    for (const Object* c : a.model().by_kind("contact_pair"))
      if (!c->suppressed) issue(code, *c, "접촉");
  return issues;
}

}  // namespace nasa95
