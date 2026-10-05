// 모델 명령: 속성 설정 명령, 스텝 승계·조합, 셋·함수·매개변수, 프로퍼티 할당, 케이스 연결·검사,
// 단위계·물리 상수, 매크로. (`.agent/proj-api-list.md` 2, 5~9절 중 형상·메시·솔버가 필요 없는 것)
#include <algorithm>
#include <any>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>

#include "nasa95/app.hpp"
#include "nasa95/solver.hpp"
#include "nasa95/error.hpp"
#include "nasa95/expr.hpp"

namespace nasa95 {

namespace {

using F = FieldSpec;

const Object& of_kind(const Model& m, Id id, const std::string& kind) {
  const Object& o = m.get(id);
  if (o.kind != kind)
    throw Error("wrong_kind", "id=" + std::to_string(id) + " 는 " + kind + " 가 아닙니다",
                {{"object", id}, {"kind", o.kind}, {"expected", kind}});
  return o;
}

std::string subtype_of(const Object& o) { return o.props.value("type", std::string()); }
bool has(const Json& p, const char* key) { return p.contains(key) && !p[key].is_null(); }

Json issue(const char* severity, const char* code, const Object& o, const std::string& field, const std::string& msg) {
  return Json{{"severity", severity}, {"code", code}, {"object", o.id}, {"kind", o.kind},
              {"name", o.name},       {"field", field}, {"message", msg}};
}

CommandSpec base(const std::string& name, char kind, const std::string& target, const std::string& desc,
                 const std::string& features) {
  CommandSpec c;
  c.name = name, c.kind = kind, c.undoable = (kind == 'C'), c.target = target, c.desc = desc, c.features = features;
  return c;
}

// `<kind>.update` 의 매개변수 정의에서 이름으로 고른다(정의를 한 곳에만 두려고).
Fields pick_params(const App& app, const std::string& kind, const std::vector<std::string>& names) {
  const Fields& all = app.commands().at(kind + ".update").params;
  Fields out;
  for (const std::string& n : names) {
    auto it = std::find_if(all.begin(), all.end(), [&](const FieldSpec& f) { return f.name == n; });
    if (it == all.end()) throw Error("internal", kind + ".update 에 없는 속성: " + n);
    out.push_back(*it);
  }
  return out;
}

// 속성 몇 개만 바꾸는 명령. 실제 처리는 `<kind>.update` 가 한다.
void add_setter(App& app, const std::string& name, const std::string& kind, const std::vector<std::string>& fields,
                const std::string& desc, const std::string& features) {
  CommandSpec c = base(name, 'C', kind, desc, features);
  c.params = {F("id", "ref", "대상 객체").call_req()};
  const Fields picked = pick_params(app, kind, fields);
  c.params.insert(c.params.end(), picked.begin(), picked.end());
  c.fn = [kind](App& a, const Json& p) { return a.invoke(kind + ".update", p); };
  app.register_command(std::move(c));
}

// object_list 속성에 항목 하나를 덧붙이는 명령.
void add_appender(App& app, const std::string& name, const std::string& kind, const std::string& list_field,
                  const std::string& desc, const std::string& features) {
  CommandSpec c = base(name, 'C', kind, desc, features);
  c.params = {F("id", "ref", "대상 객체").call_req()};
  const Fields list = pick_params(app, kind, {list_field});
  c.params.insert(c.params.end(), list.front().items.begin(), list.front().items.end());
  c.fn = [kind, list_field](App& a, const Json& p) {
    const Object& o = of_kind(a.model(), p["id"].get<Id>(), kind);
    Json items = o.props.value(list_field, Json::array());
    Json item = p;
    item.erase("id");
    items.push_back(item);
    return a.invoke(kind + ".update", Json{{"id", o.id}, {list_field, items}});
  };
  app.register_command(std::move(c));
}

// ------------------------------------------------------------------ 스텝 승계
struct Effective {
  struct Entry {
    Id id;
    Id step;
    std::string key;
  };
  std::vector<Entry> loads, bcs;
  std::map<std::string, Entry> outputs;  // 출력 요청은 종류별로 가장 최근 스텝의 것이 유효하다
};

// 같은 종류·같은 대상의 정의는 뒤 스텝의 것이 앞 스텝의 것을 바꾼다. 같은 스텝 안에서는 둘 다 유효하다 —
// 솔버는 같은 스텝의 같은 절점·방향 힘을 더한다(매뉴얼 7.15 *CLOAD; 하중 셋 조합 1.2D + 1.6L 이 이에 기댄다)
std::string entry_key(const Object& o) { return subtype_of(o) + "|" + o.props.value("target", Json()).dump(); }

void put(std::vector<Effective::Entry>& v, const Object& o, Id step) {
  const std::string key = entry_key(o);
  v.erase(std::remove_if(v.begin(), v.end(), [&](const Effective::Entry& e) { return e.key == key && e.step != step; }), v.end());
  v.push_back({o.id, step, key});
}

}  // namespace

// 스텝의 하중·경계조건: 직속 자식 + 참조하는 하중 셋(계수)·구속 셋의 것(D14). 셋의 항목은 사본으로 돌려주고 계수를 크기 값에 곱한다
std::vector<Object> step_entries(const App& a, const Object& step, const std::string& kind) {
  std::vector<Object> out;
  for (const Object* o : a.model().children(step.id, kind))
    if (!o->suppressed) out.push_back(*o);
  const bool loads = kind == "load";
  const Json refs = step.props.value(loads ? "load_sets" : "bc_sets", Json::array());
  const KindSpec& ks = a.schema().get(kind);
  for (const Json& ref : refs) {
    const Id set_id = loads ? ref.value("set", Id(0)) : ref.get<Id>();
    const double factor = loads ? ref.value("factor", 1.0) : 1.0;
    const Object* set = a.model().find(set_id);
    if (!set || set->kind != (loads ? "load_set" : "bc_set") || set->suppressed) continue;
    for (const Object* o : a.model().children(set->id, kind)) {
      if (o->suppressed) continue;
      Object copy = *o;
      if (factor != 1.0) {
        bool scaled = false;
        for (const FieldSpec& f : ks.fields_for(copy.props.value("type", std::string()))) {
          if (!f.scalable || !copy.props.contains(f.name) || copy.props[f.name].is_null()) continue;
          Json& v = copy.props[f.name];
          if (v.is_array())
            for (Json& e : v) e = e.get<double>() * factor;
          else
            v = v.get<double>() * factor;
          scaled = true;
        }
        if (!scaled)
          throw Error("not_scalable", "계수를 곱할 수 없는 하중입니다(크기 값이 없거나 선형이 아님): " + copy.name,
                      {{"object", copy.id}, {"type", copy.props.value("type", std::string())}, {"set", set->id}});
      }
      out.push_back(std::move(copy));
    }
  }
  return out;
}

Id step_own_set(App& a, const std::string& kind, Id step_id) {
  const Object& step = a.model().get(step_id);
  if (step.kind != "step") throw Error("wrong_kind", "스텝이 아닙니다", {{"object", step_id}, {"expected", "step"}});
  const bool loads = kind == "load";
  const std::string set_kind = loads ? "load_set" : "bc_set", list_field = loads ? "load_sets" : "bc_sets";
  for (const Object* s : a.model().by_kind(set_kind))
    if (s->props.contains("step") && s->props["step"].is_number() && s->props["step"].get<Id>() == step_id) return s->id;
  Object set;
  set.kind = set_kind;
  set.props["step"] = step_id;
  set.props["description"] = "스텝 '" + step.name + "' 에 직접 만든 " + (loads ? "하중" : "구속");
  std::set<std::string> used;
  for (const Object* s : a.model().by_kind(set_kind)) used.insert(s->name);
  const std::string base = step.name + (loads ? " 하중" : " 구속");
  set.name = base;
  for (int n = 2; used.count(set.name); ++n) set.name = base + "-" + std::to_string(n);
  std::int64_t order = 0;
  for (const Object* s : a.model().by_kind(set_kind)) order = std::max(order, s->order + 1);
  set.order = order;
  const Id id = a.model().create(set);
  Object copy = step;
  Json refs = copy.props.value(list_field, Json::array());
  refs.push_back(loads ? Json{{"set", id}, {"factor", 1.0}} : Json(id));
  copy.props[list_field] = refs;
  a.model().replace(copy);
  return id;
}

namespace {

// 스텝에서 유효한 하중·경계조건·출력 요청을 계산한다(LOD-38, BC-36, WT-04).
//  - 하중: 앞 스텝의 것이 쌓인다. "new" 면 그 스텝에서 모두 비운다. 섭동 스텝에서는 그 스텝의 것만 유효하고,
//    섭동 스텝을 지나면 그 앞의 하중은 이어지지 않는다(CalculiX *STEP 의 규칙).
//  - 경계조건: 앞 스텝의 것이 쌓인다. "new" 면 비운다.
Effective effective_of(const App& a, const Object& step) {
  Effective e;
  for (const Object* s : a.model().children(step.parent, "step")) {
    if (s->suppressed && s->id != step.id) continue;
    const bool pert = s->props.value("perturbation", false);
    const bool target = s->id == step.id;
    if (pert && !target) {
      e.loads.clear();
    } else {
      if (pert || s->props.value("loads_inheritance", std::string("keep")) == "new") e.loads.clear();
      for (const Object& l : step_entries(a, *s, "load")) put(e.loads, l, s->id);  // 직속 + 하중 셋(D14)
    }
    if (s->props.value("bcs_inheritance", std::string("keep")) == "new") e.bcs.clear();
    for (const Object& b : step_entries(a, *s, "bc")) put(e.bcs, b, s->id);
    std::set<std::string> cleared;
    for (const Object* o : a.model().children(s->id, "output_request")) {
      if (o->suppressed) continue;
      const std::string sub = subtype_of(*o);
      if (cleared.insert(sub).second)  // 이 스텝에서 이 종류를 처음 만나면 앞 스텝의 것을 버린다
        for (auto it = e.outputs.begin(); it != e.outputs.end();)
          it = (it->second.key == sub && it->second.step != s->id) ? e.outputs.erase(it) : std::next(it);
      e.outputs[sub + "#" + std::to_string(o->id)] = {o->id, s->id, sub};
    }
    if (target) break;
  }
  return e;
}

Json entries_json(const std::vector<Effective::Entry>& v, Id step) {
  Json arr = Json::array();
  for (const auto& e : v) arr.push_back(Json{{"id", e.id}, {"step", e.step}, {"inherited", e.step != step}});
  return arr;
}

// ------------------------------------------------------------------ 셋
const char* set_field(const Object& set) {
  const std::string t = subtype_of(set);
  return t == "surface" ? "faces" : t == "geometry" ? "entities" : "ids";
}

// ------------------------------------------------------------------ 함수
double interpolate(const Json& points, double x) {
  if (points.empty()) throw Error("invalid_state", "함수에 점이 없습니다");
  if (x <= points.front()[0].get<double>()) return points.front()[1].get<double>();
  if (x >= points.back()[0].get<double>()) return points.back()[1].get<double>();
  for (std::size_t i = 1; i < points.size(); ++i) {
    const double x1 = points[i][0].get<double>();
    if (x <= x1) {
      const double x0 = points[i - 1][0].get<double>();
      const double t = (x - x0) / (x1 - x0);
      return points[i - 1][1].get<double>() + t * (points[i][1].get<double>() - points[i - 1][1].get<double>());
    }
  }
  return points.back()[1].get<double>();
}

// ------------------------------------------------------------------ 매개변수
// 수식이 있는 매개변수의 값을 다시 계산한다. 서로를 참조하며 값이 수렴하지 않으면 순환이다.
void recompute_parameters(App& a) {
  const auto params = a.model().by_kind("parameter");
  std::map<std::string, double> vars;
  for (const Object* p : params) vars[p->name] = p->props.value("value", 0.0);
  for (std::size_t pass = 0; pass <= params.size() + 1; ++pass) {
    bool changed = false;
    for (const Object* p : params) {
      if (!p->props.contains("expression")) continue;
      const double v = evaluate_expression(p->props["expression"].get<std::string>(), vars);
      if (vars[p->name] != v) vars[p->name] = v, changed = true;
    }
    if (!changed) {
      for (const Object* p : params) {
        if (!p->props.contains("expression")) continue;
        Object o = *p;
        o.props["value"] = vars[p->name];
        a.model().replace(o);
      }
      return;
    }
  }
  throw Error("cyclic_dependency", "매개변수 수식이 서로를 참조합니다");
}

// ------------------------------------------------------------------ 보 단면 상수
Json beam_section_values(const Object& o) {
  if (subtype_of(o) != "beam" || !has(o.props, "section") || !has(o.props, "dimensions"))
    throw Error("invalid_state", "단면 종류와 치수가 있는 보 프로퍼티여야 합니다", {{"object", o.id}});
  const std::string s = o.props["section"].get<std::string>();
  const std::vector<double> d = o.props["dimensions"].get<std::vector<double>>();
  const double pi = 3.14159265358979323846;
  double area = 0, i11 = 0, i22 = 0, j = 0;  // i11: 1축 둘레, i22: 2축 둘레, j: 비틀림 상수(Saint-Venant)
  // 직사각형(긴 변 a, 짧은 변 b)의 비틀림 상수(Roark 근사, 오차 < 1%)
  auto rect_j = [](double a, double b) {
    if (a < b) std::swap(a, b);
    return a * std::pow(b, 3) * (1.0 / 3 - 0.21 * (b / a) * (1 - std::pow(b, 4) / (12 * std::pow(a, 4))));
  };
  if (s == "rect") {  // d = [1방향 두께 a, 2방향 두께 b]
    area = d[0] * d[1], i11 = d[0] * std::pow(d[1], 3) / 12, i22 = d[1] * std::pow(d[0], 3) / 12, j = rect_j(d[0], d[1]);
  } else if (s == "circ") {  // d = [1방향 지름, 2방향 지름] (타원): J = π a³b³/(a²+b²)
    area = pi * d[0] * d[1] / 4, i11 = pi * d[0] * std::pow(d[1], 3) / 64, i22 = pi * d[1] * std::pow(d[0], 3) / 64;
    const double ea = d[0] / 2, eb = d[1] / 2;
    j = pi * std::pow(ea, 3) * std::pow(eb, 3) / (ea * ea + eb * eb);
  } else if (s == "pipe") {  // d = [바깥 반지름, 두께]
    const double r = d[0] - d[1];
    if (r < 0) throw Error("out_of_range", "파이프 두께가 반지름보다 큽니다", {{"object", o.id}});
    area = pi * (d[0] * d[0] - r * r), i11 = i22 = pi * (std::pow(d[0], 4) - std::pow(r, 4)) / 4, j = 2 * i11;
  } else if (s == "box") {  // d = [a, b, t1, t2, t3, t4]: t1·t3 는 ±1방향, t2·t4 는 ±2방향 벽 두께
    const double a = d[0], b = d[1], ai = a - d[2] - d[4], bi = b - d[3] - d[5];
    if (ai <= 0 || bi <= 0) throw Error("out_of_range", "박스 벽 두께가 바깥 치수보다 큽니다", {{"object", o.id}});
    const double e1 = (d[4] - d[2]) / 2, e2 = (d[5] - d[3]) / 2;  // 속 빈 부분 중심의 위치
    const double ao = a * b, ah = ai * bi;
    area = ao - ah;
    const double c1 = -ah * e1 / area, c2 = -ah * e2 / area;  // 단면 도심
    i11 = a * std::pow(b, 3) / 12 + ao * c2 * c2 - (ai * std::pow(bi, 3) / 12 + ah * (e2 - c2) * (e2 - c2));
    i22 = b * std::pow(a, 3) / 12 + ao * c1 * c1 - (bi * std::pow(ai, 3) / 12 + ah * (e1 - c1) * (e1 - c1));
    // 닫힌 얇은 벽(Bredt): J = 4 A_m² / Σ(s/t). 벽 두께 0 인 쪽(열린 단면)이 있으면 열린 단면 식(Σ bt³/3)
    const double am = a - (d[2] + d[4]) / 2, bm = b - (d[3] + d[5]) / 2;
    if (d[2] > 0 && d[3] > 0 && d[4] > 0 && d[5] > 0) j = 4 * std::pow(am * bm, 2) / (bm / d[2] + am / d[3] + bm / d[4] + am / d[5]);
    else for (const BeamRect& r : beam_section_rects(s, d)) j += std::max(r.t1, r.t2) * std::pow(std::min(r.t1, r.t2), 3) / 3;
  } else if (beam_section_composite(s)) {  // 형강(D15): 부분 직사각형의 합, 도심 기준(평행축 정리). J 는 열린 얇은 벽 식 Σ bt³/3
    const std::vector<BeamRect> rects = beam_section_rects(s, d);
    double g1 = 0, g2 = 0;
    for (const BeamRect& r : rects) area += r.t1 * r.t2, g1 += r.t1 * r.t2 * r.c1, g2 += r.t1 * r.t2 * r.c2;
    g1 /= area, g2 /= area;
    for (const BeamRect& r : rects) {
      const double A = r.t1 * r.t2;
      i11 += r.t1 * std::pow(r.t2, 3) / 12 + A * (r.c2 - g2) * (r.c2 - g2);
      i22 += r.t2 * std::pow(r.t1, 3) / 12 + A * (r.c1 - g1) * (r.c1 - g1);
      j += std::max(r.t1, r.t2) * std::pow(std::min(r.t1, r.t2), 3) / 3;
    }
    const auto [H, B] = beam_section_extent(s, d);
    return Json{{"section", s}, {"area", area}, {"i11", i11}, {"i22", i22}, {"j", j}, {"centroid", {g1, g2}}, {"extent", {H, B}}, {"parts", rects.size()},
                {"note", "도심은 외접 상자 중심(기준선) 기준의 1·2축 위치. 덱에는 직사각형 부분 단면의 합성보로 나간다. j 는 열린 얇은 벽 근사(Σbt³/3)"}};
  } else {  // general: [A, I11, I12, I22, 전단 계수]
    return Json{{"section", s}, {"area", d[0]}, {"i11", d[1]}, {"i12", d[2]}, {"i22", d[3]}, {"shear_factor", d[4]}};
  }
  return Json{{"section", s}, {"area", area}, {"i11", i11}, {"i22", i22}, {"j", j}};
}

// ------------------------------------------------------------------ 케이스 검사
const std::set<std::string> kThermalLoads = {"concentrated_flux", "surface_flux", "body_flux", "film",
                                             "forced_convection", "radiation", "cavity_radiation"};
const std::set<std::string> kMechanicalLoads = {"force", "moment", "pressure", "traction", "edge_load", "line_load",
                                                "gravity", "acceleration", "centrifugal", "newton_gravity",
                                                "remote_force", "total_force", "pretension"};
const std::set<std::string> kMechanicalSteps = {"static", "frequency", "complex_frequency", "buckle", "modal_dynamic",
                                                "steady_state_dynamics", "dynamic", "visco"};
const std::set<std::string> kNeedsLoad = {"static", "buckle", "dynamic", "modal_dynamic", "steady_state_dynamics",
                                          "visco"};
const std::set<std::string> kNeedsDensity = {"frequency", "complex_frequency", "modal_dynamic",
                                             "steady_state_dynamics", "dynamic"};
const std::set<std::string> kNeedsEigen = {"modal_dynamic", "steady_state_dynamics", "complex_frequency"};
const std::vector<std::string> kStiffness = {"elastic", "hyperelastic", "hyperfoam", "deformation_plasticity", "user",
                                             "special"};

bool has_behavior(const Object& mat, const std::string& b) {
  auto it = mat.props.find("behaviors");
  return it != mat.props.end() && it->contains(b);
}

// 억제되지 않은 프로퍼티가 쓰는 재료.
std::vector<const Object*> used_materials(const App& a) {
  std::set<Id> ids;
  for (const Object* p : a.model().by_kind("property")) {
    if (p->suppressed) continue;
    if (has(p->props, "material")) ids.insert(p->props["material"].get<Id>());
    if (has(p->props, "layers"))
      for (const Json& layer : p->props["layers"])
        if (has(layer, "material")) ids.insert(layer["material"].get<Id>());
  }
  std::vector<const Object*> out;
  for (Id id : ids)
    if (const Object* m = a.model().find(id)) out.push_back(m);
  return out;
}

Json check_case(const App& a, const Object& cs) {
  Json issues = Json::array();
  // 이 케이스의 객체와 모델 정의(다른 케이스의 것은 제외)를 진단한다.
  std::set<Id> other_cases;
  for (const Object* c : a.model().by_kind("case"))
    if (c->id != cs.id) other_cases.insert(c->id);
  // 하중 셋·구속 셋은 모델 수준이라, 이 케이스의 스텝이 참조하는 셋(과 그 안의 하중·구속)만 진단한다
  std::set<Id> my_sets;
  for (const Object* s : a.model().children(cs.id, "step")) {
    for (const Json& r : s->props.value("load_sets", Json::array()))
      if (r.is_object() && r.contains("set") && r["set"].is_number()) my_sets.insert(r["set"].get<Id>());
    for (const Json& r : s->props.value("bc_sets", Json::array()))
      if (r.is_number()) my_sets.insert(r.get<Id>());
  }
  std::function<bool(const Object&)> in_other = [&](const Object& o) {
    if (o.kind == "load_set" || o.kind == "bc_set") return !my_sets.count(o.id);
    if (o.kind == "load" || o.kind == "bc") return !my_sets.count(o.parent);
    for (Id p = o.parent; p != 0; p = a.model().get(p).parent)
      if (other_cases.count(p)) return true;
    return other_cases.count(o.id) > 0;
  };
  for (const Object* o : a.model().all()) {
    if (o->suppressed || in_other(*o)) continue;
    for (const Json& i : diagnose_object(a, *o)) issues.push_back(i);
  }

  const auto steps = a.model().children(cs.id, "step");
  const auto materials = used_materials(a);
  const bool has_constraints = !a.model().by_kind("constraint").empty();
  bool seen_static = false, seen_eigen = false;
  for (const Object* s : steps) {
    if (s->suppressed) continue;
    const std::string type = subtype_of(*s);
    const Effective e = effective_of(a, *s);
    const bool mechanical = kMechanicalSteps.count(type) > 0;
    const bool thermal = type == "heat_transfer";

    // 하중이 없어도 0 이 아닌 강제 변위·온도(변위 제어)가 있으면 하중이 있는 것이다
    bool prescribed = false;
    for (const Effective::Entry& en : e.bcs) {
      const Object* b = a.model().find(en.id);
      if (!b) continue;
      if (b->props.contains("values"))
        for (const Json& v : b->props["values"]) prescribed = prescribed || (v.is_number() && v.get<double>() != 0.0);
      if (b->props.contains("value") && b->props["value"].is_number() && b->props["value"].get<double>() != 0.0) prescribed = true;
    }
    if (kNeedsLoad.count(type) && e.loads.empty() && !prescribed && type != "modal_dynamic" && type != "steady_state_dynamics")
      issues.push_back(issue("warning", "no_load", *s, "", "이 스텝에는 유효한 하중이 없습니다(0 이 아닌 강제 변위·온도도 없음)"));
    if (mechanical && type != "frequency" && type != "complex_frequency" && e.bcs.empty() && !has_constraints)
      issues.push_back(issue("warning", "unconstrained", *s, "", "이 스텝에는 구속이 없습니다(강체 운동 가능)"));

    for (const auto& le : e.loads) {
      const Object& l = a.model().get(le.id);
      const std::string lt = subtype_of(l);
      if (mechanical && kThermalLoads.count(lt))
        issues.push_back(issue("error", "incompatible_load", l, "", "구조 해석 스텝에서 쓸 수 없는 열 하중입니다"));
      if (thermal && kMechanicalLoads.count(lt))
        issues.push_back(issue("error", "incompatible_load", l, "", "열전달 스텝에서 쓸 수 없는 구조 하중입니다"));
    }

    for (const Object* m : materials) {
      if (mechanical || type == "coupled_temperature_displacement" || type == "uncoupled_temperature_displacement") {
        const bool stiff = std::any_of(kStiffness.begin(), kStiffness.end(),
                                       [&](const std::string& b) { return has_behavior(*m, b); });
        if (!stiff)
          issues.push_back(issue("error", "missing_behavior", *m, "elastic", "구조 해석에 필요한 탄성 정의가 없습니다"));
      }
      if (kNeedsDensity.count(type) && !has_behavior(*m, "density"))
        issues.push_back(issue("error", "missing_behavior", *m, "density", "이 해석 종류에는 밀도가 필요합니다"));
      if (thermal || type == "coupled_temperature_displacement" || type == "uncoupled_temperature_displacement") {
        if (!has_behavior(*m, "conductivity"))
          issues.push_back(issue("error", "missing_behavior", *m, "conductivity", "열해석에 필요한 열전도율이 없습니다"));
        if (!s->props.value("steady_state", false) &&
            (!has_behavior(*m, "specific_heat") || !has_behavior(*m, "density")))
          issues.push_back(issue("error", "missing_behavior", *m, "specific_heat", "과도 열해석에는 비열과 밀도가 필요합니다"));
      }
      if (type == "visco" && !has_behavior(*m, "creep"))
        issues.push_back(issue("warning", "missing_behavior", *m, "creep", "점소성 스텝인데 크리프 정의가 없습니다"));
    }

    if (s->props.value("perturbation", false) && !seen_static)
      issues.push_back(issue("warning", "no_reference_step", *s, "perturbation", "섭동 스텝 앞에 기준이 될 정적 스텝이 없습니다"));
    if (kNeedsEigen.count(type) && !seen_eigen && !has(cs.props, "links"))
      issues.push_back(issue("error", "missing_eigen_data", *s, "",
                             "앞선 고유치 해석(저장 옵션)의 결과가 필요합니다. 같은 케이스에 두거나 케이스를 연결하십시오"));
    if (type == "static") seen_static = true;
    if (type == "frequency" && s->props.value("storage", false)) seen_eigen = true;
  }
  // 사용자 서브루틴이 필요한 정의(CAS-42, BC-37, MAT-19, MAT-22, LOD-37): 전용 실행 파일을 지정하지 않았으면 경고.
  {
    Json needs = Json::array();
    auto need = [&](const Object& o, const std::string& what) { needs.push_back(Json{{"object", o.id}, {"kind", o.kind}, {"name", o.name}, {"what", what}}); };
    for (const Object* m : materials) {
      auto bit = m->props.find("behaviors");
      if (bit == m->props.end()) continue;
      if (bit->contains("user")) need(*m, "*USER MATERIAL (umat)");
      if (bit->contains("creep") && (*bit)["creep"].value("law", std::string("norton")) == "user") need(*m, "*CREEP, LAW=USER (creep)");
      if (bit->contains("plastic") && (*bit)["plastic"].value("hardening", std::string("isotropic")) == "user") need(*m, "*PLASTIC, HARDENING=USER (uhardening)");
    }
    for (const Object* ic : a.model().children(cs.id, "initial_condition"))
      if (!ic->suppressed && ic->props.value("user", false)) need(*ic, "사용자 서브루틴 초기 조건 (sigini/uiniplastic)");
    for (const Object* s : steps) {
      if (s->suppressed) continue;
      const std::vector<Object> step_loads = step_entries(a, *s, "load"), step_bcs = step_entries(a, *s, "bc");  // 직속 + 셋(D14)
      for (const Object& l : step_loads)
        if (subtype_of(l) == "user" || l.props.value("user", false)) need(l, "사용자 서브루틴 하중 (dload/cload/dflux/film)");
      for (const Object& b : step_bcs)
        if (subtype_of(b) == "user") need(b, "*BOUNDARY, USER (uboun)");
      // 프리텐션 단면(매뉴얼 7.106): 단면의 노드를 쓰는 요소는 모두 단면에 면 하나를 통째로 대고 있어야 한다. 변·꼭짓점으로만 닿는 요소(사면체 메시의
      // 임의 절단면에서 흔함)가 있으면 그 요소가 절단을 건너뛰어 체결력이 전달되지 않는다(실측: 1000 N 이 68 N 으로)
      for (const Object& lo : step_loads) {
        const Object* l = &lo;
        if (subtype_of(*l) != "pretension" || !has(l->props, "target")) continue;
        Json faces;
        try {
          faces = resolve_target(a, l->props["target"], "faces");
        } catch (const Error&) {
          continue;
        }
        const Mesh& m = a.mesh();
        std::set<Id> on_section;
        std::set<Id> section_elems;
        for (const Json& fc : faces) {
          const Element e = m.element(fc[0].get<Id>());
          section_elems.insert(e.id);
          for (int k : shape_faces(e.shape)[static_cast<std::size_t>(fc[1].get<int>() - 1)]) on_section.insert(e.nodes[static_cast<std::size_t>(k)]);
        }
        int bridging = 0;
        for (std::size_t i = 0; i < m.element_count(); ++i) {
          const Element e = m.element_at(i);
          if (shape_info(e.shape).dim != 3) continue;
          bool touches = false;
          for (Id n : e.nodes) touches = touches || on_section.count(n);
          if (!touches) continue;
          bool has_face = false;
          for (const auto& f : shape_faces(e.shape)) {
            bool all = true;
            for (int k : f) all = all && on_section.count(e.nodes[static_cast<std::size_t>(k)]);
            has_face = has_face || all;
          }
          if (!has_face) ++bridging;
        }
        if (bridging > 0)
          issues.push_back(issue("warning", "pretension_section_bridged", *l, "target",
                                 "프리텐션 단면에 면이 아닌 변·꼭짓점으로만 닿는 요소가 " + std::to_string(bridging) +
                                     "개 있습니다(매뉴얼 7.106). 그 요소가 절단을 건너뛰어 체결력이 전달되지 않습니다 — 단면이 요소 층 경계가 되게(예: 매핑 육면체) 메싱하십시오"));
      }
      if (subtype_of(*s) == "substructure_generate" && !has(s->props, "retained_dofs"))
        issues.push_back(issue("error", "missing_retained_dofs", *s, "retained_dofs", "부분구조 생성에는 유지 자유도(*RETAINED NODAL DOFS)가 필요합니다"));
      // 고정 증분(direct, 모달 동해석)에서 주기 / 증분 > 최대 증분 수(*STEP, INC — 기본 100)면 솔버가 "max. # of increments reached" 로 멈춘다
      if (has(s->props, "initial_increment") && has(s->props, "period") && s->props["initial_increment"].get<double>() > 0) {
        const bool fixed = subtype_of(*s) == "modal_dynamic" || s->props.value("direct", false);
        const double needed = s->props["period"].get<double>() / s->props["initial_increment"].get<double>();
        const int inc = s->props.value("max_increments", 100);
        if (fixed && needed > inc + 1e-9)
          issues.push_back(issue("warning", "increments_exceed_max", *s, "max_increments",
                                 "주기 / 증분 = " + std::to_string(static_cast<long long>(std::ceil(needed))) + " 이 최대 증분 수 " + std::to_string(inc) +
                                     " 보다 큽니다(솔버가 멈춤). max_increments 를 올리십시오"));
      }
      for (const Object* ch : a.model().children(s->id, "step_change")) {
        // 요소 추가(ADD, 기본 STRAIN FREE)는 비선형(NLGEOM) 스텝에서만 된다(솔버: "a strain-free addition of elements ... is only possible for nonlinear")
        if (!ch->suppressed && subtype_of(*ch) == "model_change_element" && ch->props.value("action", std::string()) == "add" && !s->props.value("nlgeom", false))
          issues.push_back(issue("error", "model_change_add_nonlinear", *ch, "action", "요소 추가(*MODEL CHANGE, ADD)는 NLGEOM 을 켠 스텝에서만 할 수 있습니다"));
        if (ch->suppressed || subtype_of(*ch) != "change_section") continue;
        // *CHANGE SOLID SECTION 은 솔리드 섹션(solid·truss)의 요소에만 쓸 수 있다
        const Json target = ch->props.value("target", Json());
        for (const Object* pr : a.model().by_kind("property")) {
          const std::string pt = subtype_of(*pr);
          if (pr->suppressed || pt == "solid" || pt == "truss" || !has(pr->props, "target")) continue;
          if (pr->props["target"] == target)
            issues.push_back(issue("error", "solid_section_only", *ch, "target", "스텝 중 섹션 변경은 솔리드 섹션의 요소에만 할 수 있습니다(" + pr->name + " 은 " + pt + ")"));
        }
      }
    }
    if (!needs.empty() && !cs.props.value("user_subroutines", false) && !has(cs.props, "solver_executable")) {
      Json i = issue("warning", "user_subroutines", cs, "solver_executable", "사용자 서브루틴이 필요한 정의가 있습니다. 서브루틴을 넣어 빌드한 솔버 실행 파일을 지정하십시오");
      i["needs"] = needs;
      issues.push_back(i);
    }
  }
  // 부분구조(*MATRIX ASSEMBLE)의 행렬 파일: 작업 폴더(없으면 현재 폴더) 기준으로 있어야 한다.
  for (const Object* p : a.model().by_kind("property")) {
    if (p->suppressed || subtype_of(*p) != "substructure") continue;
    const std::filesystem::path base = has(cs.props, "work_directory") ? std::filesystem::path(cs.props["work_directory"].get<std::string>()) : std::filesystem::path();
    for (const char* key : {"stiffness_file", "mass_file"}) {
      if (!has(p->props, key)) continue;
      const std::filesystem::path f = p->props[key].get<std::string>();
      const std::filesystem::path full = f.is_absolute() ? f : base / f;
      if (!std::filesystem::exists(full))
        issues.push_back(issue("error", "missing_file", *p, key, "부분구조 행렬 파일이 없습니다: " + full.string()));
    }
    if (p->name.size() > 4) issues.push_back(issue("error", "name_too_long", *p, "name", "부분구조 이름은 4자 이내여야 합니다"));
  }
  // 보 단면 1축 방향(PRP-07): 주지 않으면 솔버 기본 (0,0,-1) 로 나간다. 요소 축과 나란한 1축(기본값을 쓴 수직 부재가 흔한 경우)은 솔버가
  // 단면을 펼치지 못해 실패하므로 오류로 잡는다. 요소별 방향(mesh.set_beam_direction)이 있으면 그것을 본다
  {
    const Mesh& m = a.mesh();
    const Object* settings = a.find_settings();
    std::map<Id, std::array<double, 3>> per_elem;
    if (settings)
      for (const Json& row : settings->props.value("beam_directions", Json::array()))
        per_elem[row[0].get<Id>()] = {row[1].get<double>(), row[2].get<double>(), row[3].get<double>()};
    for (const Object* p : a.model().by_kind("property")) {
      if (p->suppressed || subtype_of(*p) != "beam" || !has(p->props, "target")) continue;
      const bool given = has(p->props, "direction");
      std::array<double, 3> dir{0, 0, -1};
      if (given) for (int k = 0; k < 3; ++k) dir[k] = p->props["direction"][static_cast<std::size_t>(k)].get<double>();
      Json elems;
      try {
        elems = resolve_target(a, p->props["target"], "elements");
      } catch (const Error&) {
        continue;
      }
      int parallel = 0;
      Id first = 0;
      for (const Json& ej : elems) {
        const Id eid = ej.get<Id>();
        if (!m.has_element(eid)) continue;
        const Element e = m.element(eid);
        if (shape_info(e.shape).dim != 1 || e.nodes.size() < 2) continue;
        std::array<double, 3> d = per_elem.count(eid) ? per_elem[eid] : dir;
        const auto p0 = m.node(e.nodes[0]), p1 = m.node(e.nodes[1]);
        std::array<double, 3> ax{p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
        const double la = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]), ld = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (la <= 0 || ld <= 0) continue;
        const double cross2 = std::pow(ax[1] * d[2] - ax[2] * d[1], 2) + std::pow(ax[2] * d[0] - ax[0] * d[2], 2) + std::pow(ax[0] * d[1] - ax[1] * d[0], 2);
        if (std::sqrt(cross2) / (la * ld) < 1e-6) {
          if (!parallel) first = eid;
          ++parallel;
        }
      }
      if (parallel)
        issues.push_back(issue("error", "beam_direction_parallel", *p, "direction",
                               "단면 1축 방향이 보 요소의 축과 나란합니다(요소 " + std::to_string(parallel) + "개, 예: " + std::to_string(first) + ")" +
                                   (given ? "" : " — 방향을 주지 않아 솔버 기본 (0,0,-1) 이 쓰였습니다(수직 부재)") + ". 보 축에 수직인 방향을 주십시오"));
      else if (!given)
        issues.push_back(issue("warning", "beam_direction_default", *p, "direction", "단면 1축 방향을 주지 않아 솔버 기본 (0,0,-1) 로 나갑니다. 단면이 비대칭이면 방향을 확인하십시오"));
    }
  }
  // 솔버 지원 범위(solver.hpp 기능 표): 이 케이스의 솔버가 지원하지 않는 정의
  for (const Json& i : solver_support_issues(a, cs)) issues.push_back(i);
  if (steps.empty()) issues.push_back(issue("incomplete", "no_step", cs, "", "스텝이 없습니다"));
  return issues;
}

// 스텝에서 유효한 변위 구속의 중복·충돌을 찾는다(BC-14). 메시가 필요한 미구속 강체 운동 판정은 메시 단계에서 더한다.
// 구속이 막지 못하는 강체 운동의 수(0~6). 판정할 수 없으면 0.
// 모델 전체를 한 덩어리로 보고, 구속된 자유도가 강체 운동 6개(병진 3, 회전 3)를 얼마나 막는지 계수(rank)로 센다.
// 구속식·접촉·국부 좌표계가 있으면 이 방법으로는 알 수 없으므로 판정하지 않는다.
int free_rigid_modes(const App& a, const Effective& e) {
  const Mesh& m = a.mesh();
  if (m.node_count() == 0 || !a.model().by_kind("constraint").empty() || !a.model().by_kind("contact_pair").empty()) return 0;
  // 기준점과 길이: 좌표를 모델 크기로 나눠 병진·회전 항의 크기를 맞춘다.
  std::array<double, 3> lo = m.node(m.node_ids().front()), hi = lo;
  for (Id n : m.node_ids()) {
    const auto x = m.node(n);
    for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], x[k]), hi[k] = std::max(hi[k], x[k]);
  }
  const double size = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], 1e-300});
  std::vector<std::array<double, 6>> basis;  // 서로 수직인 단위 벡터
  auto add_row = [&](std::array<double, 6> row) {
    for (int pass = 0; pass < 2; ++pass)
      for (const auto& b : basis) {
        double d = 0;
        for (int k = 0; k < 6; ++k) d += row[k] * b[k];
        for (int k = 0; k < 6; ++k) row[k] -= d * b[k];
      }
    double n = 0;
    for (double v : row) n += v * v;
    if (n < 1e-16) return;
    for (double& v : row) v /= std::sqrt(n);
    basis.push_back(row);
  };
  for (const auto& be : e.bcs) {
    const Object& b = a.model().get(be.id);
    const std::string t = subtype_of(b);
    std::vector<int> dofs;
    if ((t == "displacement" || t == "fixed_current") && has(b.props, "dofs")) {
      for (const Json& d : b.props["dofs"]) dofs.push_back(d.get<int>());
    } else if ((t == "symmetry" || t == "antisymmetry") && has(b.props, "normal")) {
      const int n = b.props["normal"].get<std::string>()[0] - 'x';  // 0, 1, 2
      for (int k = 0; k < 3; ++k) {
        const bool normal_axis = k == n;
        if ((t == "symmetry") == normal_axis) dofs.push_back(k + 1); else dofs.push_back(k + 4);
      }
    } else {
      continue;
    }
    if (has(b.props, "csys") || !has(b.props, "target")) return 0;
    Json nodes;
    try {
      nodes = resolve_target(a, b.props["target"], "nodes");
    } catch (const Error&) {
      return 0;
    }
    for (const Json& nid : nodes) {
      if (!m.has_node(nid.get<Id>())) continue;
      const auto x = m.node(nid.get<Id>());
      const std::array<double, 3> r = {(x[0] - lo[0]) / size, (x[1] - lo[1]) / size, (x[2] - lo[2]) / size};
      for (int d : dofs) {
        std::array<double, 6> row{};
        if (d <= 3) {  // u_d = t_d + (ω × r)_d = t_d + ω · (r × e_d)
          const int i = d - 1, j = (i + 1) % 3, k = (i + 2) % 3;
          row[i] = 1.0, row[3 + j] = r[k], row[3 + k] = -r[j];
        } else {
          row[d - 1] = 1.0;
        }
        add_row(row);
      }
      if (basis.size() == 6) return 0;
    }
  }
  return 6 - static_cast<int>(basis.size());
}

Json check_bcs(const App& a, const Object& step) {
  Json issues = Json::array();
  struct Hit {
    Id bc;
    double value;
  };
  std::map<std::string, std::vector<Hit>> seen;  // "대상|자유도" → 구속
  const Effective e = effective_of(a, step);
  for (const auto& be : e.bcs) {
    const Object& b = a.model().get(be.id);
    const std::string t = subtype_of(b);
    if ((t != "displacement" && t != "fixed_current") || !has(b.props, "target") || !has(b.props, "dofs")) continue;
    const Json& target = b.props["target"];
    const Json values = b.props.value("values", Json::array());
    std::size_t i = 0;
    for (const Json& dof : b.props["dofs"]) {
      const double v = i < values.size() ? values[i].get<double>() : 0.0;
      ++i;
      // 노드를 직접 가리키면 노드별로, 그 밖에는 대상 전체를 하나로 본다.
      std::vector<std::string> keys;
      if (target["type"] == "nodes")
        for (const Json& n : target["ids"]) keys.push_back("n" + n.dump() + "|" + dof.dump());
      else
        keys.push_back(target.dump() + "|" + dof.dump());
      for (const std::string& k : keys) seen[k].push_back({b.id, v});
    }
  }
  std::set<std::pair<Id, std::string>> reported;
  for (const auto& [key, hits] : seen) {
    if (hits.size() < 2) continue;
    const bool conflict = std::any_of(hits.begin(), hits.end(), [&](const Hit& h) { return h.value != hits[0].value; });
    for (const Hit& h : hits) {
      const std::string code = conflict ? "conflicting_constraint" : "duplicate_constraint";
      if (!reported.insert({h.bc, code}).second) continue;
      issues.push_back(issue(conflict ? "error" : "warning", code.c_str(), a.model().get(h.bc), "dofs",
                             conflict ? "같은 자유도에 다른 값의 구속이 겹칩니다" : "같은 자유도를 두 번 구속합니다"));
    }
  }
  if (e.bcs.empty() && a.model().by_kind("constraint").empty())
    issues.push_back(issue("warning", "unconstrained", step, "", "이 스텝에는 구속이 없습니다(강체 운동 가능)"));
  else if (const int free_modes = free_rigid_modes(a, e); free_modes > 0)
    issues.push_back(issue("warning", "rigid_body_motion", step, "",
                           "구속이 강체 운동 " + std::to_string(free_modes) + "개를 막지 못합니다(모델 전체를 한 덩어리로 볼 때)"));
  return issues;
}

// 케이스 연결(선행 케이스)의 실행 순서. 순환이면 Error.
std::vector<Id> case_order(const App& a) {
  std::map<Id, std::vector<Id>> needs;
  for (const Object* c : a.model().by_kind("case")) {
    needs[c->id];
    if (has(c->props, "links"))
      for (const Json& l : c->props["links"]) needs[c->id].push_back(l.get<Id>());
  }
  std::vector<Id> order;
  std::map<Id, int> state;  // 0 미방문, 1 방문 중, 2 완료
  std::function<void(Id)> visit = [&](Id id) {
    if (state[id] == 2) return;
    if (state[id] == 1) throw Error("cyclic_dependency", "케이스 연결이 순환합니다", {{"object", id}});
    state[id] = 1;
    for (Id n : needs[id])
      if (needs.count(n)) visit(n);
    state[id] = 2;
    order.push_back(id);
  };
  for (const auto& [id, list] : needs) visit(id);
  return order;
}

}  // namespace

Json beam_section_properties(const Object& property) { return beam_section_values(property); }

Json beam_section_constants(const Object& beam_property) { return beam_section_values(beam_property); }

void register_model_commands(App& app) {
  // ---------------------------------------------------------------- 하중·경계조건의 설정 명령
  add_setter(app, "load.set_amplitude", "load", {"amplitude", "time_delay"}, "하중에 함수·시간 지연을 연결한다", "LOD-11, LOD-36");
  add_setter(app, "load.set_csys", "load", {"csys"}, "하중 방향의 기준 좌표계를 지정한다", "LOD-13");
  add_setter(app, "load.set_distribution", "load", {"distribution"}, "좌표에 대한 수식으로 하중의 공간 분포를 지정한다", "LOD-12");
  add_setter(app, "load.set_phase", "load", {"phase"}, "실수/허수 성분을 지정한다", "LOD-20");
  add_setter(app, "load.set_sector", "load", {"sector"}, "순환대칭 섹터를 지정한다", "LOD-21");
  add_setter(app, "bc.set_amplitude", "bc", {"amplitude", "time_delay"}, "강제 변위에 함수를 연결한다", "LOD-11");
  add_setter(app, "bc.set_csys", "bc", {"csys"}, "구속의 로컬 좌표계를 지정한다", "BC-04");

  // ---------------------------------------------------------------- 케이스
  add_setter(app, "case.set_physics", "case", {"physics"},
             "케이스의 해석 분야를 지정한다(물성·하중·BC·결과의 이름과 단위 표기가 그 분야로 바뀐다)",
             "CAS-02, MAT-27, LOD-40, BC-39");
  {
    // 열전달 유사 해석(MAT-27, LOD-40, BC-39): 덱은 열전달 카드 그대로이고, 사용자에게 보이는 이름·단위만 분야의 것으로 바꾼다.
    // 대응표는 CalculiX 2.22 매뉴얼 6.9.9~6.9.15 의 표 9~16.
    CommandSpec c = base("case.physics_labels", 'Q', "case",
                         "케이스 해석 분야의 열전달 유사 표기(온도·열유속·전도율·발열·열용량이 그 분야에서 무엇인지)를 조회한다", "CAS-02, MAT-27, LOD-40, BC-39");
    c.params = {F("id", "ref", "케이스").call_req()};
    c.fn = [](App& a, const Json& p) {
      const Object o = of_kind(a.model(), p["id"].get<Id>(), "case");
      const std::string physics = o.props.value("physics", std::string("structural"));
      // {분야, {온도, 열유속, 법선 열유속(경계), 전도율, 발열, 열용량(ρc)}} — 매뉴얼 표의 한 열. 없는 항은 "".
      struct Row { const char* temperature; const char* flux; const char* normal_flux; const char* conductivity; const char* heat_source; const char* capacity; };
      static const std::map<std::string, Row> table = {
          {"thermal", {"온도", "열유속", "법선 열유속", "열전도율", "발열", "열용량 ρc"}},
          {"thermo_mechanical", {"온도", "열유속", "법선 열유속", "열전도율", "발열", "열용량 ρc"}},
          {"acoustic", {"압력 p", "ρ0(a−f)", "ρ0(aₙ−fₙ)", "단위 텐서 I", "−ρ0∇·f", "1/c0²"}},
          {"shallow_water", {"수위 η", "∂(Hv)/∂t", "H ∂vₙ/∂t", "Hg I", "", "1"}},
          {"lubrication", {"압력 p", "ρhv", "ρhvₙ", "h³ρ/(12μ) I", "−((v_b+v_a)/2)·∇(hρ) − ∂(hρ)/∂t − ṁΩ", ""}},
          {"irrotational_flow", {"속도 퍼텐셜 φ", "속도 v", "vₙ", "단위 텐서 I", "0", ""}},
          {"electrostatic", {"전위 V", "전기 변위 D(유전체) / 전기장 E(금속·진공)", "Dₙ / Eₙ = jₙ/σ", "유전율 ε(유전체) / 단위 텐서 I(금속·진공)", "자유 전하 밀도 ρ_f / ρ_e/ε0", ""}},
          {"groundwater", {"총수두 h", "유출 속도 v", "vₙ", "투수계수 k", "0", ""}},
          {"diffusion", {"농도 ρ_A / C_A", "질량 유속 j_A / 몰 유속 J*_A", "j_Aₙ / J*_Aₙ", "확산계수 D_AB", "ṅ_A / Ṅ_A", "1"}}};
      auto it = table.find(physics);
      Json out{{"id", o.id}, {"physics", physics}, {"thermal_analogy", it != table.end()}};
      if (it != table.end()) {
        const Row& r = it->second;
        out["temperature"] = r.temperature, out["flux"] = r.flux, out["normal_flux"] = r.normal_flux;
        out["conductivity"] = r.conductivity, out["heat_source"] = r.heat_source, out["capacity"] = r.capacity;
        out["deck_keywords"] = Json{{"conductivity", "*CONDUCTIVITY"}, {"capacity", "*SPECIFIC HEAT + *DENSITY"},
                                    {"temperature", "*BOUNDARY dof 11"}, {"normal_flux", "*DFLUX"}, {"heat_source", "*DFLUX BF"}};
      }
      return out;
    };
    app.register_command(std::move(c));
  }
  add_setter(app, "case.set_scope", "case", {"scope"}, "케이스에 포함할 요소 범위를 지정한다", "CAS-08");
  add_setter(app, "case.set_environment", "case", {"solver_executable", "threads", "work_directory", "user_subroutines"},
             "솔버 실행 파일·스레드 수·작업 폴더를 지정한다", "CAS-41, CAS-42");
  add_setter(app, "optimization.set_design_variables", "case", {"design_variable_type", "design_nodes"},
             "설계 변수를 지정한다", "CAS-31");
  {
    CommandSpec c = base("contact.set_method", 'C', "case", "케이스의 접촉 방식을 지정한다", "BC-22");
    c.params = {F("id", "ref", "케이스").call_req()};
    FieldSpec method = pick_params(app, "case", {"contact_method"}).front();
    method.name = "method", method.must = true;
    c.params.push_back(method);
    c.fn = [](App& a, const Json& p) {
      return a.invoke("case.update", Json{{"id", p["id"]}, {"contact_method", p["method"]}});
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("damping.set_rayleigh", 'C', "case", "케이스의 레일리 감쇠를 지정한다", "MAT-08");
    c.params = {F("id", "ref", "케이스").call_req(), F("alpha", "number", "질량 비례 계수").call_req().ge(0),
                F("beta", "number", "강성 비례 계수").call_req().ge(0)};
    c.fn = [](App& a, const Json& p) {
      return a.invoke("case.update", Json{{"id", p["id"]}, {"rayleigh_alpha", p["alpha"]}, {"rayleigh_beta", p["beta"]}});
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("case.link", 'C', "case", "다른 케이스의 산출물(.eig, 결과 파일)을 쓰도록 연결한다", "CAS-37");
    c.params = {F("id", "ref", "케이스").call_req(), F("source", "ref", "선행 케이스").ref("case").call_req()};
    c.fn = [](App& a, const Json& p) {
      Object o = of_kind(a.model(), p["id"].get<Id>(), "case");
      const Id src = of_kind(a.model(), p["source"].get<Id>(), "case").id;
      Json links = o.props.value("links", Json::array());
      if (std::find(links.begin(), links.end(), Json(src)) == links.end()) links.push_back(src);
      o.props["links"] = links;
      a.model().replace(o);
      case_order(a);  // 순환이면 여기서 오류 → 명령 전체가 되돌려진다
      return Json{{"id", o.id}, {"links", links}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("case.dependencies", 'Q', "case", "케이스 간 의존 관계와 실행 순서를 조회한다", "CAS-37, WT-09");
    c.fn = [](App& a, const Json&) {
      Json edges = Json::array();
      for (const Object* cs : a.model().by_kind("case"))
        if (has(cs->props, "links"))
          for (const Json& l : cs->props["links"]) edges.push_back(Json{{"case", cs->id}, {"needs", l}});
      return Json{{"edges", edges}, {"order", case_order(a)}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("case.check", 'Q', "case", "사전 검사 결과(오류·경고)를 조회한다", "CAS-09, CAS-43");
    c.params = {F("id", "ref", "케이스").call_req()};
    c.fn = [](App& a, const Json& p) { return check_case(a, of_kind(a.model(), p["id"].get<Id>(), "case")); };
    app.register_command(std::move(c));
  }

  // ---------------------------------------------------------------- 스텝
  add_setter(app, "step.set_options", "step", {"nlgeom", "perturbation", "max_increments", "load_application"},
             "섭동·기하 비선형·증분 수·하중 적용 방식을 지정한다", "CAS-15");
  add_setter(app, "step.set_time", "step",
             {"initial_increment", "period", "min_increment", "max_increment", "direct", "time_reset", "total_time_at_start"},
             "시간 증분을 지정한다", "CAS-16");
  add_setter(app, "step.set_solver", "step", {"solver"}, "방정식 솔버를 지정한다", "CAS-17");
  add_setter(app, "step.set_controls", "step", {"controls", "controls_reset"}, "수렴 제어 값을 지정하거나 초기화한다", "CAS-18");
  add_setter(app, "step.set_modal_damping", "step", {"damping_type", "modal_damping"}, "모드 감쇠를 지정한다", "CAS-22");
  add_setter(app, "step.set_inheritance", "step", {"loads_inheritance", "bcs_inheritance"},
             "앞 스텝 하중·경계조건의 유지/교체를 지정한다", "LOD-38, BC-36");
  add_setter(app, "substructure.retained_dofs", "step", {"retained_dofs"}, "부분구조 유지 자유도를 지정한다", "BC-38");
  add_setter(app, "optimization.set_objective", "step", {"objective", "objective_target"}, "목적 함수를 지정한다", "CAS-31");
  add_setter(app, "optimization.set_filter", "step",
             {"filter_type", "filter_radius", "boundary_weighting", "edge_preservation", "direction_weighting"},
             "민감도 필터를 지정한다", "CAS-31");
  add_setter(app, "robust.set_tolerances", "step", {"tolerances", "correlation_length"}, "기하 공차·상관 길이를 지정한다", "CAS-32");
  add_appender(app, "optimization.add_response", "step", "design_responses", "설계 응답을 추가한다", "CAS-31");
  add_appender(app, "optimization.add_constraint", "step", "constraints", "설계 응답에 대한 제약을 추가한다", "CAS-31");
  add_appender(app, "optimization.add_geometric_constraint", "step", "geometric_constraints", "기하 제약을 추가한다", "CAS-31");
  {  // set_procedure: 그 해석 종류의 설정 전부(= update)
    CommandSpec c = base("step.set_procedure", 'C', "step", "해석 종류별 설정을 지정한다", "CAS-06, CAS-21~36");
    c.params = app.commands().at("step.update").params;
    c.fn = [](App& a, const Json& p) { return a.invoke("step.update", p); };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("step.reorder", 'C', "step", "스텝 순서를 바꾼다", "CAS-04, WT-03");
    c.params = {F("id", "ref", "스텝").call_req(), F("index", "integer", "새 위치(0 부터)").call_req().ge(0)};
    c.fn = [](App& a, const Json& p) { return a.invoke("step.move", Json{{"id", p["id"]}, {"index", p["index"]}}); };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("step.effective", 'Q', "step", "이 스텝에서 유효한 하중·경계조건·출력 요청 전체(승계 포함)를 조회한다", "WT-04");
    c.params = {F("id", "ref", "스텝").call_req()};
    c.fn = [](App& a, const Json& p) {
      const Object& s = of_kind(a.model(), p["id"].get<Id>(), "step");
      const Effective e = effective_of(a, s);
      Json outputs = Json::array();
      for (const auto& [key, o] : e.outputs)
        outputs.push_back(Json{{"id", o.id}, {"step", o.step}, {"inherited", o.step != s.id}});
      Json changes = Json::array();
      for (const Object* ch : a.model().children(s.id, "step_change"))
        if (!ch->suppressed) changes.push_back(ch->id);
      return Json{{"step", s.id},        {"loads", entries_json(e.loads, s.id)}, {"bcs", entries_json(e.bcs, s.id)},
                  {"outputs", outputs}, {"changes", changes}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("step.load_combination", 'C', "step",
                         "다른 스텝의 하중을 계수를 곱해 이 스텝으로 가져와 조합 하중을 만든다", "CAS-05");
    c.params = {F("id", "ref", "조합 하중을 둘 스텝").call_req(),
                F("terms", "object_list", "가져올 스텝과 계수")
                    .call_req()
                    .of({F("step", "ref", "하중을 가져올 스텝").ref("step").call_req(),
                         F("factor", "number", "계수").call_req().ex(1.5)})};
    c.fn = [](App& a, const Json& p) {
      Model& m = a.model();
      const Id target = of_kind(m, p["id"].get<Id>(), "step").id;
      check_value(a.commands().at("step.load_combination").params[1], p["terms"], &m);
      const KindSpec& ks = a.schema().get("load");
      Json created = Json::array();
      for (const Json& term : p["terms"]) {
        const double factor = term["factor"].get<double>();
        // 원본 스텝의 유효 하중(셋 포함) → 계수를 곱한 사본을 대상 스텝의 전용 셋에 넣는다(하중은 셋 안에만)
        const std::vector<Object> src = step_entries(a, of_kind(m, term["step"].get<Id>(), "step"), "load");
        const Id own = step_own_set(a, "load", target);
        for (Object o : src) {
          bool scaled = false;
          for (const FieldSpec& f : ks.fields_for(subtype_of(o))) {
            if (!f.scalable || !has(o.props, f.name.c_str())) continue;
            Json& v = o.props[f.name];
            if (v.is_array())
              for (Json& e : v) e = e.get<double>() * factor;
            else
              v = v.get<double>() * factor;
            scaled = true;
          }
          if (!scaled && factor != 1.0)
            throw Error("not_scalable", "계수를 곱할 수 없는 하중입니다(크기 값이 없거나 선형이 아님)",
                        {{"object", o.id}, {"type", subtype_of(o)}});
          std::set<std::string> used;
          for (const Object* l : m.children(own, "load")) used.insert(l->name);
          std::string name = o.name;
          for (int n = 2; used.count(name); ++n) name = o.name + "-" + std::to_string(n);
          std::int64_t order = 0;
          for (const Object* l : m.children(own, "load")) order = std::max(order, l->order + 1);
          o.id = 0, o.parent = own, o.name = name, o.order = order;
          created.push_back(m.create(o));
        }
      }
      return Json{{"created", created}};
    };
    app.register_command(std::move(c));
  }

  for (const char* kind : {"load", "bc"}) {
    const bool loads = std::string(kind) == "load";
    CommandSpec c = base(loads ? "step.own_load_set" : "step.own_bc_set", 'C', "step",
                         std::string("이 스텝 전용 ") + (loads ? "하중 셋" : "구속 셋") + "을 돌려준다(없으면 만들어 스텝이 참조하게). " +
                             (loads ? "하중" : "구속") + "은 셋 안에만 둘 수 있어서, 스텝에 직접 만들려는 호출은 이 셋으로 간다", "CAS-05");
    c.params = {F("id", "ref", "스텝").call_req().ref("step")};
    const std::string k = kind;
    c.fn = [k](App& a, const Json& p) {
      const Id id = step_own_set(a, k, p["id"].get<Id>());
      return Json{{"id", id}, {"name", a.model().get(id).name}};
    };
    app.register_command(std::move(c));
  }

  // ---------------------------------------------------------------- 경계조건·구속 검사
  {
    CommandSpec c = base("bc.check", 'Q', "bc", "스텝에서 유효한 구속의 중복·충돌·누락을 조회한다", "BC-14");
    c.params = {F("id", "ref", "스텝").ref("step").call_req()};
    c.fn = [](App& a, const Json& p) { return check_bcs(a, of_kind(a.model(), p["id"].get<Id>(), "step")); };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("constraint.check", 'Q', "constraint", "구속식의 종속 자유도 중복을 조회한다", "BC-14");
    c.fn = [](App& a, const Json&) {
      Json issues = Json::array();
      std::map<std::string, Id> dependent;
      for (const Object* o : a.model().by_kind("constraint")) {
        if (o->suppressed || subtype_of(*o) != "equation" || !has(o->props, "terms") || o->props["terms"].empty()) continue;
        const Json& t = o->props["terms"][0];
        const std::string key = t[0].dump() + "|" + t[1].dump();
        if (!dependent.emplace(key, o->id).second)
          issues.push_back(issue("error", "duplicate_dependent", *o, "terms", "같은 자유도가 다른 구속식에서도 종속 항입니다"));
      }
      return issues;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("contact_pair.swap", 'C', "contact_pair", "종속 면과 주 면을 맞바꾼다", "BC-10");
    c.params = {F("id", "ref", "접촉 쌍").call_req()};
    c.fn = [](App& a, const Json& p) {
      const Object& o = of_kind(a.model(), p["id"].get<Id>(), "contact_pair");
      if (!has(o.props, "slave") || !has(o.props, "master"))
        throw Error("invalid_state", "종속 면과 주 면이 모두 지정되어 있어야 합니다", {{"object", o.id}});
      return a.invoke("contact_pair.update", Json{{"id", o.id}, {"slave", o.props["master"]}, {"master", o.props["slave"]}});
    };
    app.register_command(std::move(c));
  }

  // ---------------------------------------------------------------- 프로퍼티
  add_setter(app, "property.set_layup", "property", {"layers"}, "복합재 적층을 지정한다", "PRP-04");
  {
    CommandSpec c = base("property.set_nodal_thickness", 'C', "property", "절점별 두께를 지정한다", "PRP-16");
    c.params = {F("id", "ref", "프로퍼티").call_req()};
    FieldSpec values = pick_params(app, "property", {"nodal_thickness_values"}).front();
    values.name = "values", values.must = true;
    c.params.push_back(values);
    c.fn = [](App& a, const Json& p) {
      for (const Json& row : p["values"])
        if (row[1].get<double>() <= 0) throw Error("out_of_range", "두께는 0 보다 커야 합니다", {{"param", "values"}});
      return a.invoke("property.update",
                      Json{{"id", p["id"]}, {"nodal_thickness_values", p["values"]}, {"nodal_thickness", true}});
    };
    app.register_command(std::move(c));
  }
  for (const bool assign : {true, false}) {
    CommandSpec c = base(assign ? "property.assign" : "property.unassign", 'C', "property",
                         assign ? "프로퍼티를 요소·셋·형상에 할당한다" : "할당을 해제한다", "PRP-12");
    FieldSpec target = pick_params(app, "property", {"target"}).front();
    target.must = true;
    c.params = {F("id", "ref", "프로퍼티").call_req(), target};
    c.fn = [assign](App& a, const Json& p) {
      const Object& o = of_kind(a.model(), p["id"].get<Id>(), "property");
      const Json& t = p["target"];
      Json cur = o.props.value("target", Json());
      if (cur.is_null()) {
        if (!assign) throw Error("invalid_state", "할당된 대상이 없습니다", {{"object", o.id}});
        return a.invoke("property.update", Json{{"id", o.id}, {"target", t}});
      }
      if (cur["type"] != t["type"])
        throw Error("invalid_state", "이미 다른 종류의 대상(" + cur["type"].get<std::string>() + ")에 할당되어 있습니다",
                    {{"object", o.id}, {"param", "target"}});
      Json ids = Json::array();
      for (const Json& e : cur["ids"])
        if (assign || std::find(t["ids"].begin(), t["ids"].end(), e) == t["ids"].end()) ids.push_back(e);
      if (assign)
        for (const Json& e : t["ids"])
          if (std::find(ids.begin(), ids.end(), e) == ids.end()) ids.push_back(e);
      if (ids.empty()) return a.invoke("property.update", Json{{"id", o.id}, {"target", nullptr}});
      cur["ids"] = ids;
      return a.invoke("property.update", Json{{"id", o.id}, {"target", cur}});
    };
    app.register_command(std::move(c));
  }
  // property.assignments 는 메시를 봐야 하므로 commands_mesh_ops.cpp 에 있다.
  {
    CommandSpec c = base("property.check", 'Q', "property", "프로퍼티의 오류·누락을 조회한다", "PRP-14");
    c.params = {F("id", "ref", "프로퍼티").call_req()};
    c.fn = [](App& a, const Json& p) {
      const Object& o = of_kind(a.model(), p["id"].get<Id>(), "property");
      Json issues = diagnose_object(a, o);
      if (!has(o.props, "target") && !o.suppressed)
        issues.push_back(issue("incomplete", "unassigned", o, "target", "어느 요소에도 할당되지 않았습니다"));
      return issues;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("property.section_values", 'Q', "property", "보 단면 상수 계산 결과를 조회한다", "PRP-05");
    c.params = {F("id", "ref", "프로퍼티").call_req()};
    c.fn = [](App& a, const Json& p) { return beam_section_values(of_kind(a.model(), p["id"].get<Id>(), "property")); };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("property.section_shape", 'Q', "property",
                         "보 단면의 모양: 부분 직사각형(1·2축 두께 t1·t2 와 외접 상자 중심 기준 위치 c1·c2)과 외접 상자 크기. 형강(I·T·L·C)은 합성보로 분해한 부분들(D15). "
                         "프로퍼티 없이 종류·치수만으로 조회할 수 있어 창의 단면 미리보기가 쓴다",
                         "PRP-05");
    c.params = {F("section", "string", "단면 종류").call_req().one_of({"rect", "circ", "pipe", "box", "general", "I", "T", "L", "C"}).ex("I"),
                F("dimensions", "number_list", "단면 치수(종류별 개수: rect·circ·pipe 2, box 6, general 5, I·T·L·C 4 = h, b, tw, tf)").call_req().gt(0).ex({100.0, 60.0, 6.0, 8.0})};
    c.fn = [](App& a, const Json& p) {
      const std::string s = p["section"].get<std::string>();
      check_value(a.commands().at("property.section_shape").params[0], p["section"], nullptr);
      const std::vector<double> d = p["dimensions"].get<std::vector<double>>();
      if (static_cast<int>(d.size()) != beam_section_dims(s))
        throw Error("invalid_param_type", "이 단면 종류의 치수는 " + std::to_string(beam_section_dims(s)) + "개여야 합니다", {{"param", "dimensions"}});
      Json rects = Json::array();
      for (const BeamRect& r : beam_section_rects(s, d)) rects.push_back(Json{{"t1", r.t1}, {"t2", r.t2}, {"c1", r.c1}, {"c2", r.c2}});
      const auto [H, B] = beam_section_extent(s, d);
      return Json{{"section", s}, {"composite", beam_section_composite(s)}, {"rects", rects}, {"extent", {H, B}}};
    };
    app.register_command(std::move(c));
  }

  // ---------------------------------------------------------------- 셋
  for (const bool add : {true, false}) {
    CommandSpec c = base(add ? "set.add" : "set.remove", 'C', "set", add ? "셋에 구성원을 넣는다" : "셋에서 구성원을 뺀다", "CMN-04");
    c.params = {F("id", "ref", "셋").call_req(),
                F("members", "array", "구성원(ID 목록. 면 셋은 [요소, 면 번호] 쌍의 목록)").call_req().ex(Json::array({4, 5}))};
    c.fn = [add](App& a, const Json& p) {
      const Object& o = of_kind(a.model(), p["id"].get<Id>(), "set");
      const char* field = set_field(o);
      Json cur = o.props.value(field, Json::array());
      Json out = Json::array();
      for (const Json& e : cur)
        if (add || std::find(p["members"].begin(), p["members"].end(), e) == p["members"].end()) out.push_back(e);
      if (add)
        for (const Json& e : p["members"])
          if (std::find(out.begin(), out.end(), e) == out.end()) out.push_back(e);
      return a.invoke("set.update", Json{{"id", o.id}, {field, out}});  // 형식 검사는 update 가 한다
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("set.members", 'Q', "set", "셋의 구성원을 조회한다", "CMN-04");
    c.params = {F("id", "ref", "셋").call_req()};
    c.fn = [](App& a, const Json& p) {
      const Object& o = of_kind(a.model(), p["id"].get<Id>(), "set");
      const Json members = o.props.value(set_field(o), Json::array());
      return Json{{"type", subtype_of(o)}, {"count", members.size()}, {"members", members}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("set.boolean", 'C', "set", "셋끼리 합·차·교집합으로 새 셋을 만든다", "CMN-04");
    c.params = {F("a", "ref", "첫째 셋").ref("set").call_req(), F("b", "ref", "둘째 셋").ref("set").call_req(),
                F("operation", "string", "연산").call_req().one_of({"union", "difference", "intersection"}).ex("union"),
                F("name", "string", "새 셋의 이름(없으면 자동)")};
    c.fn = [](App& a, const Json& p) {
      const Object& sa = of_kind(a.model(), p["a"].get<Id>(), "set");
      const Object& sb = of_kind(a.model(), p["b"].get<Id>(), "set");
      if (subtype_of(sa) != subtype_of(sb))
        throw Error("invalid_state", "종류가 다른 셋끼리는 연산할 수 없습니다", {{"param", "b"}});
      check_value(a.commands().at("set.boolean").params[2], p["operation"], nullptr);
      const char* field = set_field(sa);
      const Json ma = sa.props.value(field, Json::array()), mb = sb.props.value(field, Json::array());
      const std::string op = p["operation"].get<std::string>();
      auto in = [](const Json& list, const Json& e) { return std::find(list.begin(), list.end(), e) != list.end(); };
      Json out = Json::array();
      for (const Json& e : ma)
        if (op == "union" || (op == "difference" && !in(mb, e)) || (op == "intersection" && in(mb, e))) out.push_back(e);
      if (op == "union")
        for (const Json& e : mb)
          if (!in(out, e)) out.push_back(e);
      Json args{{field, out}};
      if (has(p, "name")) args["name"] = p["name"];
      return a.invoke("set.create_" + subtype_of(sa), args);
    };
    app.register_command(std::move(c));
  }

  // ---------------------------------------------------------------- 함수·매개변수
  {
    CommandSpec c = base("function.evaluate", 'Q', "function", "함수의 값을 계산한다", "CMN-03");
    c.params = {F("id", "ref", "함수").call_req(), F("x", "number_list", "계산할 x(또는 시간) 값").call_req().ex({0.0, 0.5, 1.0}),
                F("y", "number", "수식의 y"), F("z", "number", "수식의 z"), F("t", "number", "수식의 t")};
    c.fn = [](App& a, const Json& p) {
      const Object& f = of_kind(a.model(), p["id"].get<Id>(), "function");
      Json values = Json::array();
      if (subtype_of(f) == "expression") {
        if (!has(f.props, "expression")) throw Error("invalid_state", "수식이 없습니다", {{"object", f.id}});
        std::map<std::string, double> vars{{"y", p.value("y", 0.0)}, {"z", p.value("z", 0.0)}, {"t", p.value("t", 0.0)}};
        for (const Object* par : a.model().by_kind("parameter")) vars[par->name] = par->props.value("value", 0.0);
        for (const Json& x : p["x"]) {
          vars["x"] = x.get<double>();
          values.push_back(evaluate_expression(f.props["expression"].get<std::string>(), vars));
        }
      } else {
        if (!has(f.props, "points")) throw Error("invalid_state", "함수에 점이 없습니다", {{"object", f.id}});
        for (const Json& x : p["x"]) values.push_back(interpolate(f.props["points"], x.get<double>()));
      }
      return Json{{"values", values}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("function.import", 'C', "function", "파일에서 표를 읽어 함수를 만든다", "CMN-03");
    c.params = {F("path", "string", "두 열(x, y)로 된 텍스트·CSV 파일").call_req().ex("curve.csv"),
                F("type", "string", "함수 종류").one_of({"table", "amplitude"}), F("name", "string", "이름(없으면 자동)")};
    c.fn = [](App& a, const Json& p) {
      const std::string path = p["path"].get<std::string>();
      std::ifstream f(path);
      if (!f) throw Error("io_error", "파일을 열 수 없습니다: " + path, {{"path", path}});
      Json points = Json::array();
      std::string line;
      while (std::getline(f, line)) {
        for (char& ch : line)
          if (ch == ',' || ch == ';' || ch == '\t') ch = ' ';
        std::istringstream is(line);
        double x, y;
        if (is >> x >> y) points.push_back(Json::array({x, y}));  // 숫자 두 개로 시작하지 않는 줄(머리글)은 건너뛴다
      }
      if (points.empty()) throw Error("invalid_file", "숫자 두 열을 가진 줄이 없습니다", {{"path", path}});
      Json args{{"points", points}};
      if (has(p, "name")) args["name"] = p["name"];
      const std::string type = p.value("type", std::string("table"));
      check_value(a.commands().at("function.import").params[1], Json(type), nullptr);
      return a.invoke("function.create_" + type, args);
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("parameter.set", 'C', "parameter", "매개변수의 값 또는 수식을 바꾸고 수식이 있는 매개변수를 다시 계산한다", "GEO-43");
    c.params = {F("id", "ref", "매개변수").call_req(), F("value", "number", "값"),
                F("expression", "string", "다른 매개변수의 이름을 쓸 수 있는 수식(null 이면 수식 제거)").ex("H/2")};
    c.fn = [](App& a, const Json& p) {
      a.invoke("parameter.update", p);
      recompute_parameters(a);
      const Object& o = a.model().get(p["id"].get<Id>());
      return Json{{"id", o.id}, {"value", o.props.value("value", 0.0)}};
    };
    app.register_command(std::move(c));
  }

  // ---------------------------------------------------------------- 단위계·물리 상수 (모델 설정 객체)
  {
    CommandSpec c = base("unit.get", 'Q', "", "모델 단위계를 조회한다", "CMN-05");
    c.fn = [](App& a, const Json&) {
      const Object* s = a.find_settings();
      return Json{{"system", s ? s->props.value("unit_system", std::string("mm-t-s")) : std::string("mm-t-s")}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("unit.symbols", 'Q', "", "물리량별 단위 기호를 조회한다(system 이 없으면 모델 단위계). UI 가 입력 상자 끝에 붙인다", "CMN-05, CMN-12");
    c.params = {F("system", "string", "단위계(없으면 모델의 것)").one_of({"mm-t-s", "m-kg-s", "mm-kg-ms", "cm-g-s", "in-lbf-s"}).ex("mm-t-s")};
    c.fn = [](App& a, const Json& p) {
      if (has(p, "system")) check_value(a.commands().at("unit.symbols").params[0], p["system"], nullptr);
      const Object* s = a.find_settings();
      return unit_symbols(has(p, "system") ? p["system"].get<std::string>() : (s ? s->props.value("unit_system", std::string("mm-t-s")) : std::string("mm-t-s")));
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("unit.set", 'C', "", "모델 단위계를 지정한다(값은 환산하지 않는다)", "CMN-05");
    c.params = {F("system", "string", "단위계(길이-질량-시간)").call_req().one_of({"mm-t-s", "m-kg-s", "mm-kg-ms", "cm-g-s", "in-lbf-s"}).ex("mm-t-s")};
    c.fn = [](App& a, const Json& p) {
      check_value(a.commands().at("unit.set").params[0], p["system"], nullptr);
      Object s = a.settings();
      s.props["unit_system"] = p["system"];
      a.model().replace(s);
      return Json{{"system", p["system"]}};
    };
    app.register_command(std::move(c));
  }
  {
    // 단위계 변환(CMN-11)·표시 단위(CMN-12). 계수 표는 unit_factor()(파일 아래) 에 있다.
    auto factor = [](const std::string& from, const std::string& to, const std::string& dim) -> double { return unit_factor(from, to, dim); };
    auto factors_json = [factor](const std::string& from, const std::string& to) {
      Json f = Json::object();
      for (const char* name : {"length", "area", "volume", "mass", "time", "temperature", "force", "moment", "pressure", "stiffness", "acceleration",
                               "velocity", "density", "thermal_expansion", "conductivity", "specific_heat", "energy", "power", "heat_flux",
                               "power_per_volume", "film_coefficient", "force_per_length", "frequency"})
        f[name] = factor(from, to, name);
      return f;
    };
    {
      CommandSpec c = base("unit.set_display", 'V', "", "표시 단위계를 바꾼다(저장된 값은 그대로, UI·범례가 값에 곱할 환산 계수를 돌려준다). 비우면 모델 단위계와 같게", "CMN-12");
      c.params = {F("system", "string", "표시 단위계").one_of({"mm-t-s", "m-kg-s", "mm-kg-ms", "cm-g-s", "in-lbf-s"}).ex("m-kg-s")};
      c.fn = [factors_json](App& a, const Json& p) {
        const Object* s = a.find_settings();
        const std::string model = s ? s->props.value("unit_system", std::string("mm-t-s")) : std::string("mm-t-s");
        std::any& slot = a.runtime("unit.display");
        if (has(p, "system")) {
          check_value(a.commands().at("unit.set_display").params[0], p["system"], nullptr);
          slot = p["system"].get<std::string>();
        } else {
          slot.reset();
        }
        const std::string display = slot.has_value() ? std::any_cast<std::string>(slot) : model;
        return Json{{"model", model}, {"display", display}, {"factors", factors_json(model, display)}};
      };
      app.register_command(std::move(c));
    }
    {
      CommandSpec c = base("unit.convert_model", 'C', "",
                           "모델 전체를 다른 단위계로 환산한다: 차원이 붙은 속성(길이·힘·압력·온도 …), 재료 표의 열(차원을 아는 구성 모델만), 메시 좌표, "
                           "가져온 형상의 배율. 차원을 모르는 재료 구성 모델이 있으면 거부한다(force 로 그대로 두고 진행)",
                           "CMN-11");
      c.params = {F("to", "string", "바꿀 단위계").call_req().one_of({"mm-t-s", "m-kg-s", "mm-kg-ms", "cm-g-s", "in-lbf-s"}).ex("m-kg-s"),
                  F("force", "bool", "환산하지 못하는 값이 있어도 진행한다(그 값은 그대로)")};
      c.fn = [factor, factors_json](App& a, const Json& p) {
        check_value(a.commands().at("unit.convert_model").params[0], p["to"], nullptr);
        const std::string to = p["to"].get<std::string>();
        Object settings = a.settings();
        const std::string from = settings.props.value("unit_system", std::string("mm-t-s"));
        Json unconverted = Json::array();
        // 환산하지 못하는 재료 구성 모델을 먼저 모은다
        for (const Object* m : a.model().by_kind("material")) {
          if (!has(m->props, "behaviors")) continue;
          for (const auto& [name, b] : m->props["behaviors"].items()) {
            if (!b.contains("data") || b["data"].empty()) continue;
            const BehaviorSpec* spec = nullptr;
            for (const BehaviorSpec& s : material_behaviors())
              if (s.name == name) spec = &s;
            if (!spec || !spec->column_dims) unconverted.push_back(Json{{"object", m->id}, {"kind", "material"}, {"behavior", name}});
          }
        }
        if (!unconverted.empty() && !p.value("force", false))
          throw Error("not_available", "차원을 모르는 재료 구성 모델이 있어 환산하지 못합니다(force 로 그대로 두고 진행)", {{"unconverted", unconverted}});
        if (from == to) return Json{{"from", from}, {"to", to}, {"objects", 0}, {"fields", 0}, {"nodes", 0}, {"unconverted", unconverted}};
        auto scale_value = [&](Json& v, double f) {
          if (v.is_number()) v = v.get<double>() * f;
          else if (v.is_array())
            for (Json& x : v) {
              if (x.is_number()) x = x.get<double>() * f;
              else if (x.is_array())
                for (Json& y : x)
                  if (y.is_number()) y = y.get<double>() * f;
            }
        };
        std::size_t objects = 0, fields = 0;
        for (const Object* o : a.model().all()) {
          if (o->kind == "settings") continue;
          const KindSpec* ks = a.schema().find(o->kind);
          if (!ks) continue;
          Object copy = *o;
          bool changed = false;
          for (const FieldSpec& f : ks->fields_for(o->props.value("type", std::string()))) {
            if (f.dim.empty() || !has(copy.props, f.name.c_str())) continue;
            if (f.type == "object_list") continue;
            scale_value(copy.props[f.name], factor(from, to, f.dim)), changed = true, ++fields;
          }
          // object_list 안의 차원 있는 필드(적층 층 두께 등)
          for (const FieldSpec& f : ks->fields_for(o->props.value("type", std::string()))) {
            if (f.type != "object_list" || !has(copy.props, f.name.c_str())) continue;
            for (Json& item : copy.props[f.name])
              for (const FieldSpec& sub : f.items)
                if (!sub.dim.empty() && has(item, sub.name.c_str())) scale_value(item[sub.name], factor(from, to, sub.dim)), changed = true, ++fields;
          }
          // 재료 표: 열마다 차원, 온도 열은 그대로(눈금이 같다)
          if (o->kind == "material" && has(copy.props, "behaviors")) {
            const int n = convert_material_behaviors(copy.props["behaviors"], from, to);
            if (n) changed = true, fields += n;
          }
          // 가져온 형상: 배율에 길이 계수를 곱한다(BREP 글은 그대로)
          if (o->kind == "feature" && o->props.value("type", std::string()) == "import") {
            copy.props["scale"] = copy.props.value("scale", 1.0) * factor(from, to, "length"), changed = true, ++fields;
          }
          if (changed) a.model().replace(copy), ++objects;
        }
        // 메시 좌표
        Mesh& m = a.mesh();
        const std::size_t nodes = m.node_count();
        if (nodes) {
          const double fl = factor(from, to, "length");
          std::vector<double> xyz = m.node_xyz();
          for (double& v : xyz) v *= fl;
          m.move_nodes(m.node_ids(), xyz);
        }
        settings = a.settings();
        settings.props["unit_system"] = to;
        a.model().replace(settings);
        return Json{{"from", from}, {"to", to}, {"objects", objects}, {"fields", fields}, {"nodes", nodes}, {"unconverted", unconverted},
                    {"factors", factors_json(from, to)}};
      };
      app.register_command(std::move(c));
    }
  }
  {
    CommandSpec c = base("physical_constants.set", 'C', "", "절대 영도·스테판-볼츠만 상수·중력 상수를 지정한다", "LOD-29");
    c.params = {F("absolute_zero", "number", "절대 영도(모델의 온도 눈금에서)"),
                F("stefan_boltzmann", "number", "스테판-볼츠만 상수(모델 단위계)").gt(0),
                F("newton_gravity", "number", "만유인력 상수(모델 단위계)").gt(0)};
    c.fn = [](App& a, const Json& p) {
      const Fields& specs = a.commands().at("physical_constants.set").params;
      Object s = a.settings();
      Json pc = s.props.value("physical_constants", Json::object());
      for (const FieldSpec& f : specs) {
        if (!p.contains(f.name)) continue;
        if (p[f.name].is_null()) {
          pc.erase(f.name);
        } else {
          check_value(f, p[f.name], nullptr);
          pc[f.name] = p[f.name];
        }
      }
      s.props["physical_constants"] = pc;
      a.model().replace(s);
      return pc;
    };
    app.register_command(std::move(c));
  }

  // ---------------------------------------------------------------- 프로그램
  {
    CommandSpec c = base("app.commands_for", 'Q', "", "지정 객체에 쓸 수 있는 명령 목록을 조회한다(상황 메뉴용)", "WT-18");
    c.params = {F("id", "ref", "객체(여러 개면 ids)"), F("ids", "ref_list", "객체들 — 모두에 공통인 명령만 돌려준다")};
    c.fn = [](App& a, const Json& p) {
      std::set<std::string> kinds;
      if (has(p, "id")) kinds.insert(a.model().get(p["id"].get<Id>()).kind);
      if (has(p, "ids"))
        for (const Json& id : p["ids"]) kinds.insert(a.model().get(id.get<Id>()).kind);
      if (kinds.empty()) throw Error("missing_param", "id 또는 ids 가 필요합니다", {{"param", "id"}});
      // 종류가 여럿이면 동작 이름(점 뒤)이 모든 종류에 있는 것만 남긴다.
      std::map<std::string, std::size_t> actions;
      for (const auto& [name, spec] : a.commands())
        if (kinds.count(spec.target)) ++actions[name.substr(name.find('.') + 1)];
      Json out = Json::array();
      for (const auto& [name, spec] : a.commands()) {
        if (!kinds.count(spec.target)) continue;
        if (actions[name.substr(name.find('.') + 1)] == kinds.size())
          out.push_back(Json{{"name", name}, {"kind", std::string(1, spec.kind)}, {"desc", spec.desc}});
      }
      return out;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("macro.record_start", 'S', "", "동작을 Python 코드로 기록하기 시작한다", "API-13");
    c.fn = [](App& a, const Json&) {
      a.macro_start();
      return Json::object();
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("macro.record_stop", 'S', "", "기록을 끝내고 Python 코드를 돌려준다", "API-13");
    c.params = {F("path", "string", "코드를 저장할 파일").ex("macro.py")};
    c.fn = [](App& a, const Json& p) {
      if (!a.macro_recording()) throw Error("invalid_state", "매크로 기록 중이 아닙니다");
      const std::string code = a.macro_stop();
      if (has(p, "path")) {
        const std::string path = p["path"].get<std::string>();
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) throw Error("io_error", "파일을 쓸 수 없습니다: " + path, {{"path", path}});
        f << code;
      }
      return Json{{"code", code}};
    };
    app.register_command(std::move(c));
  }
}


// 단위계 = 길이-질량-시간(온도는 모두 같은 눈금으로 본다 — 켈빈/섭씨 차이는 다루지 않는다).
// 물리량 차원 = (길이, 질량, 시간, 온도) 지수. 속성의 차원은 FieldSpec.dim, 재료 표의 열은 BehaviorSpec.column_dims 에서 온다.
namespace {
struct UnitBase {
  double length, mass, time;  // SI 기준 배율
};
const std::map<std::string, UnitBase>& unit_bases() {
  static const std::map<std::string, UnitBase> k = {
      {"mm-t-s", {1e-3, 1e3, 1.0}}, {"m-kg-s", {1.0, 1.0, 1.0}}, {"mm-kg-ms", {1e-3, 1.0, 1e-3}}, {"cm-g-s", {1e-2, 1e-3, 1.0}},
      {"in-lbf-s", {0.0254, 4.4482216152605 / 0.0254, 1.0}}};  // in-lbf-s 의 질량 단위 = lbf·s²/in
  return k;
}
const std::map<std::string, std::array<int, 4>>& unit_dims() {
  static const std::map<std::string, std::array<int, 4>> k = {
      {"length", {1, 0, 0, 0}},  {"area", {2, 0, 0, 0}},   {"volume", {3, 0, 0, 0}},        {"mass", {0, 1, 0, 0}},
      {"time", {0, 0, 1, 0}},    {"temperature", {0, 0, 0, 1}}, {"force", {1, 1, -2, 0}},    {"moment", {2, 1, -2, 0}},
      {"pressure", {-1, 1, -2, 0}}, {"stiffness", {0, 1, -2, 0}}, {"acceleration", {1, 0, -2, 0}}, {"velocity", {1, 0, -1, 0}},
      {"density", {-3, 1, 0, 0}}, {"thermal_expansion", {0, 0, 0, -1}}, {"conductivity", {1, 1, -3, -1}}, {"specific_heat", {2, 0, -2, -1}},
      {"energy", {2, 1, -2, 0}}, {"power", {2, 1, -3, 0}}, {"heat_flux", {0, 1, -3, 0}}, {"power_per_volume", {-1, 1, -3, 0}},
      {"film_coefficient", {0, 1, -3, -1}}, {"force_per_length", {0, 1, -2, 0}}, {"frequency", {0, 0, -1, 0}}, {"none", {0, 0, 0, 0}}};
  return k;
}
}  // namespace

bool known_unit_system(const std::string& system) { return unit_bases().count(system) > 0; }

// 재료 구성 모델 표의 값을 from → to 단위계로 환산한다(열마다 차원, 온도 열은 그대로). 돌려주는 값은 바꾼 값의 수
int convert_material_behaviors(Json& behaviors, const std::string& from, const std::string& to) {
  int fields = 0;
  if (from == to) return 0;
  for (auto& [name, b] : behaviors.items()) {
    const BehaviorSpec* spec = nullptr;
    for (const BehaviorSpec& s : material_behaviors())
      if (s.name == name) spec = &s;
    if (spec && spec->column_dims && b.contains("data")) {
      const std::vector<std::string> dims = spec->column_dims(b);
      for (Json& row : b["data"])
        for (std::size_t c = 0; c < dims.size() && c < row.size(); ++c)
          if (!dims[c].empty() && row[c].is_number()) row[c] = row[c].get<double>() * unit_factor(from, to, dims[c]), ++fields;
    }
    for (const FieldSpec& f : spec ? spec->fields : Fields{})  // 구성 모델의 그 밖의 속성(예: 열팽창 기준 온도)
      if (!f.dim.empty() && f.name != "data" && b.contains(f.name) && b[f.name].is_number()) b[f.name] = b[f.name].get<double>() * unit_factor(from, to, f.dim), ++fields;
  }
  return fields;
}

// 단위계의 단위 기호(UI 의 입력 상자 끝에 붙이는 글). 흔한 조합은 관용 기호(MPa·psi), 그 밖은 기본 단위를 조합한다
Json unit_symbols(const std::string& system) {
  struct Base { const char* L; const char* M; const char* T; const char* F; };
  static const std::map<std::string, Base> base = {{"mm-t-s", {"mm", "t", "s", "N"}}, {"m-kg-s", {"m", "kg", "s", "N"}}, {"mm-kg-ms", {"mm", "kg", "ms", "kN"}},
                                                   {"cm-g-s", {"cm", "g", "s", "dyn"}}, {"in-lbf-s", {"in", "lbf·s²/in", "s", "lbf"}}};
  static const std::map<std::string, std::string> pressure = {{"mm-t-s", "MPa"}, {"m-kg-s", "Pa"}, {"mm-kg-ms", "GPa"}, {"cm-g-s", "dyn/cm²"}, {"in-lbf-s", "psi"}};
  auto it = base.find(system);
  if (it == base.end()) throw Error("out_of_range", "모르는 단위계입니다: " + system, {{"param", "system"}});
  const Base& b = it->second;
  const std::string L = b.L, M = b.M, T = b.T, Fo = b.F, P = pressure.at(system);
  return Json{{"system", system},
              {"length", L}, {"area", L + "²"}, {"volume", L + "³"}, {"mass", M}, {"time", T}, {"temperature", "K"},
              {"force", Fo}, {"moment", Fo + "·" + L}, {"pressure", P}, {"stiffness", Fo + "/" + L}, {"acceleration", L + "/" + T + "²"},
              {"velocity", L + "/" + T}, {"density", M + "/" + L + "³"}, {"thermal_expansion", "1/K"}, {"conductivity", Fo + "/(" + T + "·K)"},
              {"specific_heat", L + "²/(" + T + "²·K)"}, {"energy", Fo + "·" + L}, {"power", Fo + "·" + L + "/" + T},
              {"heat_flux", Fo + "/(" + L + "·" + T + ")"}, {"power_per_volume", Fo + "/(" + L + "²·" + T + ")"},
              {"film_coefficient", Fo + "/(" + L + "·" + T + "·K)"}, {"force_per_length", Fo + "/" + L}, {"frequency", "1/" + T}, {"none", ""}};
}

double unit_factor(const std::string& from, const std::string& to, const std::string& dim) {
  auto d = unit_dims().find(dim);
  if (d == unit_dims().end()) throw Error("not_available", "모르는 물리량 차원입니다: " + dim, {{"dimension", dim}});
  auto a = unit_bases().find(from), b = unit_bases().find(to);
  if (a == unit_bases().end()) throw Error("out_of_range", "모르는 단위계입니다: " + from, {{"param", "unit_system"}});
  if (b == unit_bases().end()) throw Error("out_of_range", "모르는 단위계입니다: " + to, {{"param", "unit_system"}});
  return std::pow(a->second.length / b->second.length, d->second[0]) * std::pow(a->second.mass / b->second.mass, d->second[1]) *
         std::pow(a->second.time / b->second.time, d->second[2]);
}

}  // namespace nasa95
