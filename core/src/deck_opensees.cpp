// OpenSees 입력(Tcl 스크립트) 쓰기 (D16). 케이스의 solver 가 opensees 이면 write_deck 이 여기로 온다.
// OpenSees 는 링크하지 않고 별도 실행 파일로만 쓴다. 스크립트는 해석을 돌리면서 결과를 frd 형식으로 직접 쓰고
// (<job>.frd — 결과 읽기·표시는 CalculiX 와 같은 경로를 탄다), 증분마다 <job>.sta 에 진행을 적는다.
//
// 지금 쓰는 것(1차: 탄성 골조): 2절점 선 요소의 보(elasticBeamColumn)·트러스(Truss), 점 질량, 변위 구속(fix)·대칭,
// 절점 힘·모멘트, 중력(요소 질량에서 절점 하중으로), 보 등분포 하중, 지반 가속도(base_motion → UniformExcitation),
// 스텝 static·frequency·dynamic, 케이스의 레일리 감쇠. 그 밖의 것은 조용히 빠뜨리지 않고 skipped 에 알린다.
// 평면 모델(쓰는 노드의 z 가 모두 같음)은 ndm 2·ndf 3 으로, 아니면 ndm 3·ndf 6 으로 낸다.
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>

#include "nasa95/app.hpp"
#include "nasa95/deck.hpp"
#include "nasa95/error.hpp"

namespace nasa95 {

namespace {

using V3 = std::array<double, 3>;
const double kPi = 3.14159265358979323846;

bool has(const Json& p, const char* key) { return p.contains(key) && !p[key].is_null(); }
std::string subtype_of(const Object& o) { return o.props.value("type", std::string()); }
std::string num(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.10g", v);
  return buf;
}
V3 sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 cross(const V3& a, const V3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double norm(const V3& a) { return std::sqrt(dot(a, a)); }
V3 unit(const V3& a) {
  const double n = norm(a);
  return {a[0] / n, a[1] / n, a[2] / n};
}

// 결과를 frd 로 쓰는 Tcl 프로시저. 열 위치는 CalculiX 의 frd(ASCII, 긴 형식)와 같다(results_frd.cpp 가 읽는다).
const char* kTclProcs = R"TCL(
# ---- NASA-95: 결과(frd)·진행(sta) 쓰기
set nasa95_frame 0
proc nasa95_vec3 {v} {
  global nasa95_ndm
  if {$nasa95_ndm == 2} { return [list [lindex $v 0] [lindex $v 1] 0.0] }
  return [lrange $v 0 2]
}
proc nasa95_open {job} {
  global nasa95_f nasa95_sta nasa95_nodes nasa95_elems
  set nasa95_f [open "$job.frd" w]
  set nasa95_sta [open "$job.sta" w]
  puts $nasa95_f "    1C$job"
  puts $nasa95_f "    1UUSER                                                              "
  puts $nasa95_f "    1UPGM               OpenSees (NASA-95)"
  puts $nasa95_f [format "    2C                  %12d                                     1" [llength $nasa95_nodes]]
  foreach n $nasa95_nodes {
    set c [nasa95_vec3 [nodeCoord $n]]
    puts $nasa95_f [format " -1%10d%12.5E%12.5E%12.5E" $n [lindex $c 0] [lindex $c 1] [lindex $c 2]]
  }
  puts $nasa95_f " -3"
  puts $nasa95_f [format "    3C                  %12d                                     1" [llength $nasa95_elems]]
  foreach e $nasa95_elems {
    puts $nasa95_f [format " -1%10d%5d%5d%5d" [lindex $e 0] 11 0 1]
    puts $nasa95_f [format " -2%10d%10d" [lindex $e 1] [lindex $e 2]]
  }
  puts $nasa95_f " -3"
}
proc nasa95_begin_frame {} { global nasa95_frame; incr nasa95_frame }
proc nasa95_block {name comps ictype value step inc getter} {
  global nasa95_f nasa95_nodes nasa95_frame
  puts $nasa95_f [format "    1PSTEP%19d%12d%12d" $nasa95_frame $inc $step]
  puts $nasa95_f [format "  100CL  101%12.5E%12d                    %2d%5d          1" $value [llength $nasa95_nodes] $ictype $nasa95_frame]
  puts $nasa95_f [format " -4  %-8s%5d    1" $name [expr {[llength $comps] + 1}]]
  set k 0
  foreach c $comps { incr k; puts $nasa95_f [format " -5  %-8s    1    2%5d    0" $c $k] }
  puts $nasa95_f " -5  ALL         1    2    0    0    1ALL"
  foreach n $nasa95_nodes {
    set v [nasa95_vec3 [$getter $n]]
    puts $nasa95_f [format " -1%10d%12.5E%12.5E%12.5E" $n [lindex $v 0] [lindex $v 1] [lindex $v 2]]
  }
  puts $nasa95_f " -3"
  flush $nasa95_f
}
proc nasa95_mode_shape {n} { global nasa95_mode; return [nodeEigenvector $n $nasa95_mode] }
proc nasa95_progress {step inc iter total steptime dt} {
  global nasa95_sta
  puts $nasa95_sta [format "%6d%11d%7d%7d%15.6E%15.6E%15.6E" $step $inc 1 $iter $total $steptime $dt]
  flush $nasa95_sta
}
proc nasa95_close {} {
  global nasa95_f nasa95_sta
  puts $nasa95_f " 9999"
  close $nasa95_f
  close $nasa95_sta
}
proc nasa95_fail {msg} {
  puts "*ERROR: $msg"
  nasa95_close
  exit 1
}
# 한 증분을 푼다. 수렴하지 못하면 초기 강성 수정 뉴턴으로 한 번 더 해 본다
proc nasa95_advance {args} {
  set ok [eval analyze 1 $args]
  if {$ok != 0} {
    algorithm ModifiedNewton -initial
    test NormDispIncr 1.0e-6 500
    set ok [eval analyze 1 $args]
    algorithm Newton
    test NormDispIncr 1.0e-8 50
  }
  return $ok
}
)TCL";

class Writer {
 public:
  Writer(const App& a, const Object& cs, const DeckOptions& opt) : a_(a), cs_(cs), opt_(opt), m_(a.mesh()) {}

  DeckResult run() {
    collect();
    os_ << "# NASA-95 가 쓴 OpenSees 입력. 케이스: " << cs_.name << "\n";
    os_ << "wipe\nmodel basic -ndm " << ndm_ << " -ndf " << ndf_ << "\nset nasa95_ndm " << ndm_ << "\n";
    write_nodes();
    write_elements();
    os_ << kTclProcs;
    os_ << "set nasa95_nodes {";
    for (Id n : nodes_) os_ << " " << n;
    os_ << " }\nset nasa95_elems {";
    for (const Line& l : lines_) os_ << " {" << l.id << " " << l.n1 << " " << l.n2 << "}";
    os_ << " }\n";
    const std::vector<const Object*> steps = active_steps();
    if (steps.empty()) warn("no_steps", "스텝이 없어 모델만 만든다", cs_.id);
    if (!steps.empty()) apply_bcs(*steps.front(), true);  // 구속은 해석 전에 건다(뒤 스텝에서 늘면 그때 더한다)
    if (opt_.no_analysis) {
      os_ << "puts \"nasa95: model ok (입력 검사만)\"\nexit 0\n";
    } else {
      os_ << "nasa95_open {" << job_ << "}\n";
      if (has(cs_.props, "rayleigh_alpha") || has(cs_.props, "rayleigh_beta"))
        os_ << "rayleigh " << num(cs_.props.value("rayleigh_alpha", 0.0)) << " 0.0 0.0 " << num(cs_.props.value("rayleigh_beta", 0.0)) << "\n";
      int index = 0;
      for (const Object* s : steps) write_step(*s, ++index);
      os_ << "nasa95_close\nputs \"nasa95: done\"\nexit 0\n";
    }
    out_.text = os_.str();
    return std::move(out_);
  }

 private:
  struct Line {
    Id id, n1, n2;
    double mass_per_length;  // 중력 하중·요소 질량
  };

  void warn(const std::string& code, const std::string& message, Id object = 0) {
    Json w{{"code", code}, {"message", message}};
    if (object) w["object"] = object;
    out_.warnings.push_back(w);
  }
  void skip(const Object& o, const std::string& reason) {
    if (!skipped_.insert(o.id).second) return;
    out_.skipped.push_back(Json{{"object", o.id}, {"kind", o.kind}, {"name", o.name}, {"type", subtype_of(o)}, {"reason", reason}});
  }

  // 등방 탄성 재료의 E·ν, 밀도(없으면 0). 탄성이 없거나 등방이 아니면 false
  bool elastic(const Object& mat, double& E, double& nu, double& rho) const {
    const Json b = mat.props.value("behaviors", Json::object());
    if (!b.contains("elastic") || b["elastic"].value("type", std::string("iso")) != "iso" || b["elastic"].value("data", Json::array()).empty()) return false;
    E = b["elastic"]["data"][0][0].get<double>(), nu = b["elastic"]["data"][0][1].get<double>();
    rho = b.contains("density") && !b["density"].value("data", Json::array()).empty() ? b["density"]["data"][0][0].get<double>() : 0.0;
    return true;
  }

  V3 node(Id id) const { return m_.node(id); }

  void collect() {
    for (unsigned char c : cs_.name)
      if (std::isalnum(c) || c == '_' || c == '-') job_ += static_cast<char>(c);
    if (job_.empty()) job_ = "case" + std::to_string(cs_.id);
    std::set<Id> scope;
    const bool scoped = has(cs_.props, "scope");
    if (scoped)
      for (const Json& e : resolve_target(a_, cs_.props["scope"], "elements")) scope.insert(e.get<Id>());
    for (const Object* p : a_.model().by_kind("property")) {
      if (p->suppressed || !has(p->props, "target")) continue;
      const std::string t = subtype_of(*p);
      if (t != "beam" && t != "truss" && t != "mass") {
        skip(*p, "OpenSees 작성기가 아직 쓰지 못하는 프로퍼티 종류입니다(지금은 beam·truss·mass)");
        continue;
      }
      Json ids;
      try {
        ids = resolve_target(a_, p->props["target"], "elements");
      } catch (const Error& e) {
        skip(*p, e.what());
        continue;
      }
      for (const Json& e : ids)
        if (!scoped || scope.count(e.get<Id>())) prop_of_[e.get<Id>()] = p;
    }
    std::size_t orphans = 0;
    for (Id e : m_.element_ids())
      if ((!scoped || scope.count(e)) && !prop_of_.count(e)) ++orphans;
    if (orphans) warn("elements_without_property", "쓸 수 있는 프로퍼티가 없는 요소 " + std::to_string(orphans) + "개는 쓰지 않았다");
    for (const auto& [e, p] : prop_of_) {
      const Element el = m_.element(e);
      const std::string t = subtype_of(*p);
      const bool ok = t == "mass" ? el.shape == Shape::Point1 : el.shape == Shape::Line2;
      if (!ok) {
        skip(*p, t == "mass" ? "질량 프로퍼티는 점 요소에만 쓴다" : "OpenSees 의 보·트러스는 2절점 선 요소여야 한다(3절점 선 요소는 아직 쓰지 못한다)");
        continue;
      }
      for (Id n : el.nodes) nodes_.insert(n);
    }
    if (nodes_.empty()) throw Error("deck_empty", "OpenSees 로 쓸 요소가 없습니다(보·트러스 프로퍼티가 할당된 2절점 선 요소가 필요하다)", {{"object", cs_.id}});
    double zmin = 1e300, zmax = -1e300, size = 0;
    for (Id n : nodes_) {
      const V3 p = node(n);
      zmin = std::min(zmin, p[2]), zmax = std::max(zmax, p[2]);
      size = std::max({size, std::fabs(p[0]), std::fabs(p[1]), std::fabs(p[2])});
    }
    const bool planar = zmax - zmin <= 1e-9 * std::max(size, 1.0);
    ndm_ = planar ? 2 : 3, ndf_ = planar ? 3 : 6;
    for (const Object* s : active_steps()) nlgeom_ = nlgeom_ || s->props.value("nlgeom", false);
  }

  std::vector<const Object*> active_steps() const {
    std::vector<const Object*> out;
    for (const Object* s : a_.model().children(cs_.id, "step"))
      if (!s->suppressed) out.push_back(s);
    return out;
  }

  void write_nodes() {
    for (Id n : nodes_) {
      const V3 p = node(n);
      os_ << "node " << n << " " << num(p[0]) << " " << num(p[1]);
      if (ndm_ == 3) os_ << " " << num(p[2]);
      os_ << "\n";
    }
  }

  // 순수 비틀림 상수(3차원 보). 직사각형·원·파이프는 식으로, 그 밖은 극관성으로 어림한다
  double torsion(const Object& p, const Json& c) {
    const std::string s = p.props["section"].get<std::string>();
    const std::vector<double> d = p.props["dimensions"].get<std::vector<double>>();
    if (s == "rect") {
      const double a = std::max(d[0], d[1]), b = std::min(d[0], d[1]);
      return a * std::pow(b, 3) * (1.0 / 3.0 - 0.21 * (b / a) * (1.0 - std::pow(b, 4) / (12.0 * std::pow(a, 4))));
    }
    if (s == "circ") {
      const double ra = d[0] / 2, rb = d[1] / 2;
      return kPi * std::pow(ra, 3) * std::pow(rb, 3) / (ra * ra + rb * rb);
    }
    if (s == "pipe") return 2.0 * c["i11"].get<double>();
    warn("torsion_estimated", "이 단면의 비틀림 상수는 극관성(I11 + I22)으로 어림했다", p.id);
    return c["i11"].get<double>() + c["i22"].get<double>();
  }

  int transf_tag(const V3& vecxz) {
    const std::string key = ndm_ == 2 ? std::string("2d") : num(vecxz[0]) + "," + num(vecxz[1]) + "," + num(vecxz[2]);
    auto it = transf_.find(key);
    if (it != transf_.end()) return it->second;
    const int tag = static_cast<int>(transf_.size()) + 1;
    os_ << "geomTransf " << (nlgeom_ ? "PDelta" : "Linear") << " " << tag;
    if (ndm_ == 3) os_ << " " << num(vecxz[0]) << " " << num(vecxz[1]) << " " << num(vecxz[2]);
    os_ << "\n";
    return transf_[key] = tag;
  }

  void write_elements() {
    std::map<Id, double> point_mass;  // 노드 → 질량
    std::map<Id, int> truss_material;
    for (const auto& [e, p] : prop_of_) {
      if (skipped_.count(p->id)) continue;
      const Element el = m_.element(e);
      const std::string t = subtype_of(*p);
      if (t == "mass") {
        point_mass[el.nodes[0]] += p->props["mass"].get<double>();
        continue;
      }
      const Object* mat = has(p->props, "material") ? a_.model().find(p->props["material"].get<Id>()) : nullptr;
      double E = 0, nu = 0, rho = 0;
      if (!mat || !elastic(*mat, E, nu, rho)) {
        skip(*p, "등방 선형 탄성 재료가 필요하다(OpenSees 작성기는 아직 탄성 보·트러스만 쓴다)");
        continue;
      }
      const V3 x = sub(node(el.nodes[1]), node(el.nodes[0]));
      if (norm(x) == 0) {
        skip(*p, "길이가 0 인 요소가 있다");
        continue;
      }
      if (t == "truss") {
        const double area = p->props["area"].get<double>();
        if (!truss_material.count(mat->id)) {
          truss_material[mat->id] = ++material_tags_;
          os_ << "uniaxialMaterial Elastic " << material_tags_ << " " << num(E) << "\n";
        }
        os_ << "element Truss " << e << " " << el.nodes[0] << " " << el.nodes[1] << " " << num(area) << " " << truss_material[mat->id];
        if (rho > 0) os_ << " -rho " << num(rho * area);
        os_ << "\n";
        lines_.push_back({e, el.nodes[0], el.nodes[1], rho * area});
        continue;
      }
      // 보: 단면 1축(direction, 기본 (0,0,-1))이 OpenSees 의 국부 z 쪽이 된다. I11 = 1축 둘레, I22 = 2축 둘레
      Json c;
      try {
        c = beam_section_constants(*p);
      } catch (const Error& err) {
        skip(*p, err.what());
        continue;
      }
      const double area = c["area"].get<double>(), i11 = c["i11"].get<double>(), i22 = c["i22"].get<double>();
      V3 axis1{0, 0, -1};
      if (has(p->props, "direction")) axis1 = {p->props["direction"][0].get<double>(), p->props["direction"][1].get<double>(), p->props["direction"][2].get<double>()};
      if (norm(axis1) == 0 || norm(cross(unit(axis1), unit(x))) < 1e-6) {
        skip(*p, "단면 1축 방향(direction)이 요소 축과 나란하다");
        continue;
      }
      axis1 = unit(axis1);
      const int transf = transf_tag(axis1);  // 요소 줄보다 먼저(변환 줄을 따로 쓴다)
      const double torsion_constant = ndm_ == 3 ? torsion(*p, c) : 0.0;
      os_ << "element elasticBeamColumn " << e << " " << el.nodes[0] << " " << el.nodes[1] << " " << num(area) << " " << num(E);
      if (ndm_ == 2) {
        // 평면 골조는 전역 z 둘레로 휜다: 1축이 z 쪽이면 I11, 평면 안이면 I22
        os_ << " " << num(std::fabs(axis1[2]) > 0.7 ? i11 : i22) << " " << transf;
      } else {
        os_ << " " << num(E / (2.0 * (1.0 + nu))) << " " << num(torsion_constant) << " " << num(i22) << " " << num(i11) << " " << transf;
      }
      if (rho > 0) os_ << " -mass " << num(rho * area);
      os_ << "\n";
      lines_.push_back({e, el.nodes[0], el.nodes[1], rho * area});
      axis1_[e] = axis1;
    }
    for (const auto& [n, mass] : point_mass) {
      os_ << "mass " << n;
      for (int k = 0; k < ndf_; ++k) os_ << " " << (k < ndm_ ? num(mass) : std::string("0.0"));
      os_ << "\n";
      point_mass_[n] = mass;
    }
  }

  // 전역 자유도(1~6) → 이 모델의 자유도 위치(0 부터). 평면 모델에 없는 자유도는 -1
  int dof_index(int dof) const {
    if (ndm_ == 3) return dof - 1;
    return dof == 1 ? 0 : dof == 2 ? 1 : dof == 6 ? 2 : -1;
  }

  // 스텝의 구속 가운데 아직 걸지 않은 것을 건다(fix). 지반 운동(base_motion)은 동해석 스텝이 따로 쓴다
  void apply_bcs(const Object& step, bool first) {
    std::map<Id, std::vector<int>> flags;
    for (const Object& bc : step_entries(a_, step, "bc")) {
      const std::string t = subtype_of(bc);
      if (t == "base_motion") continue;
      std::vector<int> dofs;
      if (t == "displacement" && has(bc.props, "dofs")) {
        bool moving = false;
        if (has(bc.props, "values"))
          for (const Json& v : bc.props["values"]) moving = moving || (v.is_number() && v.get<double>() != 0.0);
        if (moving || has(bc.props, "csys")) {
          skip(bc, moving ? "0 이 아닌 변위 구속은 OpenSees 작성기가 아직 쓰지 못한다" : "국부 좌표계의 구속은 아직 쓰지 못한다");
          continue;
        }
        for (const Json& d : bc.props["dofs"]) dofs.push_back(d.get<int>());
      } else if (t == "symmetry" && has(bc.props, "normal") && !has(bc.props, "csys")) {
        const int axis = bc.props["normal"].get<std::string>().at(0) - 'x';
        dofs = {axis + 1, (axis + 1) % 3 + 4, (axis + 2) % 3 + 4};
      } else {
        skip(bc, "OpenSees 작성기가 아직 쓰지 못하는 경계조건입니다(지금은 변위 0 구속·대칭·지반 운동)");
        continue;
      }
      Json ids;
      try {
        ids = resolve_target(a_, bc.props["target"], "nodes");
      } catch (const Error& e) {
        skip(bc, e.what());
        continue;
      }
      for (const Json& n : ids) {
        if (!nodes_.count(n.get<Id>())) continue;
        std::vector<int>& f = flags.try_emplace(n.get<Id>(), std::vector<int>(static_cast<std::size_t>(ndf_), 0)).first->second;
        for (int d : dofs)
          if (d >= 1 && d <= 6 && dof_index(d) >= 0) f[static_cast<std::size_t>(dof_index(d))] = 1;
      }
    }
    for (auto& [n, f] : flags) {
      std::vector<int>& done = fixed_.try_emplace(n, std::vector<int>(static_cast<std::size_t>(ndf_), 0)).first->second;
      bool any = false;
      for (std::size_t k = 0; k < f.size(); ++k) {
        f[k] = f[k] && !done[k];
        any = any || f[k];
        done[k] = done[k] || f[k];
      }
      if (!any) continue;
      os_ << "fix " << n;
      for (int v : f) os_ << " " << v;
      os_ << "\n";
    }
    if (!first && step.props.value("bcs_inheritance", std::string("keep")) == "new")
      warn("bcs_not_removed", "앞 스텝의 구속을 푸는 것(bcs_inheritance = new)은 OpenSees 작성기가 아직 하지 않는다", step.id);
  }

  // 함수(표·시간 함수)를 timeSeries Path 로 쓴다. 쓸 수 없으면 0
  int path_series(Id function_id, const Object& owner) {
    const Object* f = a_.model().find(function_id);
    if (!f || !has(f->props, "points") || f->props["points"].empty()) {
      skip(owner, "시간 함수는 표(points)여야 한다");
      return 0;
    }
    const int tag = ++series_tags_;
    os_ << "timeSeries Path " << tag << " -time {";
    for (const Json& p : f->props["points"]) os_ << " " << num(p[0].get<double>());
    os_ << " } -values {";
    for (const Json& p : f->props["points"]) os_ << " " << num(p[1].get<double>());
    os_ << " }\n";
    return tag;
  }

  // 스텝의 하중을 패턴으로 쓴다. 시간 함수가 같은 것끼리 한 패턴에 넣는다
  void write_loads(const Object& step, bool dynamic) {
    if (step.props.value("loads_inheritance", std::string("keep")) == "new") {
      for (int tag : patterns_) os_ << "remove loadPattern " << tag << "\n";
      patterns_.clear();
    }
    struct Group {
      std::map<Id, std::vector<double>> nodal;
      std::vector<std::string> element_loads;
      const Object* owner = nullptr;
    };
    std::map<Id, Group> groups;  // 시간 함수 ID(0 = 없음) → 하중
    const std::vector<Object> entries = step_entries(a_, step, "load");
    for (const Object& l : entries) {
      const std::string t = subtype_of(l);
      const Json& q = l.props;
      Group& g = groups[has(q, "amplitude") ? q["amplitude"].get<Id>() : Id(0)];
      if (!g.owner) g.owner = &l;
      auto add = [&](Id n, int index, double v) {
        if (index < 0 || !nodes_.count(n)) return;
        std::vector<double>& f = g.nodal.try_emplace(n, std::vector<double>(static_cast<std::size_t>(ndf_), 0.0)).first->second;
        f[static_cast<std::size_t>(index)] += v;
      };
      try {
        if ((t == "force" || t == "moment") && has(q, "components")) {
          if (has(q, "csys")) {
            skip(l, "국부 좌표계의 하중은 아직 쓰지 못한다");
            continue;
          }
          for (const Json& n : resolve_target(a_, q["target"], "nodes"))
            for (int d = 0; d < 3; ++d) add(n.get<Id>(), dof_index(d + (t == "force" ? 1 : 4)), q["components"][static_cast<std::size_t>(d)].get<double>());
        } else if ((t == "gravity" || t == "acceleration") && has(q, "value") && has(q, "direction")) {
          const V3 dir = unit(V3{q["direction"][0].get<double>(), q["direction"][1].get<double>(), q["direction"][2].get<double>()});
          const double g0 = q["value"].get<double>();
          std::set<Id> target;
          for (const Json& e : resolve_target(a_, q["target"], "elements")) target.insert(e.get<Id>());
          for (const Line& line : lines_) {
            if (!target.count(line.id) || line.mass_per_length <= 0) continue;
            const double half = 0.5 * line.mass_per_length * norm(sub(node(line.n2), node(line.n1))) * g0;
            for (int d = 0; d < ndm_; ++d) add(line.n1, d, half * dir[static_cast<std::size_t>(d)]), add(line.n2, d, half * dir[static_cast<std::size_t>(d)]);
          }
          for (const auto& [e, p] : prop_of_) {  // 점 질량
            if (!target.count(e) || subtype_of(*p) != "mass" || skipped_.count(p->id)) continue;
            const Id n = m_.element(e).nodes[0];
            for (int d = 0; d < ndm_; ++d) add(n, d, p->props["mass"].get<double>() * g0 * dir[static_cast<std::size_t>(d)]);
          }
        } else if (t == "line_load" && has(q, "components") && !has(q, "csys")) {
          const V3 w{q["components"][0].get<double>(), q["components"][1].get<double>(), q["components"][2].get<double>()};
          for (const Json& e : resolve_target(a_, q["target"], "elements")) {
            auto ax = axis1_.find(e.get<Id>());
            if (ax == axis1_.end()) continue;  // 보가 아니다
            const Element el = m_.element(e.get<Id>());
            const V3 x = unit(sub(node(el.nodes[1]), node(el.nodes[0])));
            std::ostringstream s;
            s << "  eleLoad -ele " << el.id << " -type -beamUniform ";
            if (ndm_ == 2) {
              const V3 y{-x[1], x[0], 0};
              s << num(dot(w, y)) << " " << num(dot(w, x));
            } else {
              const V3 y = unit(cross(ax->second, x)), z = cross(x, y);
              s << num(dot(w, y)) << " " << num(dot(w, z)) << " " << num(dot(w, x));
            }
            g.element_loads.push_back(s.str());
          }
        } else {
          skip(l, "OpenSees 작성기가 아직 쓰지 못하는 하중입니다(지금은 힘·모멘트·중력·보 등분포 하중)");
        }
      } catch (const Error& e) {
        skip(l, e.what());
      }
    }
    for (auto& [function_id, g] : groups) {
      if (g.nodal.empty() && g.element_loads.empty()) continue;
      int series = 0;
      if (function_id) {
        series = path_series(function_id, *g.owner);
        if (!series) continue;
      } else {
        series = ++series_tags_;
        os_ << "timeSeries " << (dynamic ? "Constant" : "Linear") << " " << series << "\n";
      }
      const int pattern = ++pattern_tags_;
      os_ << "pattern Plain " << pattern << " " << series << " {\n";
      for (const auto& [n, f] : g.nodal) {
        os_ << "  load " << n;
        for (double v : f) os_ << " " << num(v);
        os_ << "\n";
      }
      for (const std::string& line : g.element_loads) os_ << line << "\n";
      os_ << "}\n";
      patterns_.push_back(pattern);
    }
  }

  std::size_t free_dofs() const {
    std::size_t fixed = 0;
    for (const auto& [n, f] : fixed_) fixed += static_cast<std::size_t>(std::count(f.begin(), f.end(), 1));
    return nodes_.size() * static_cast<std::size_t>(ndf_) - fixed;
  }

  void write_step(const Object& s, int index) {
    const std::string t = subtype_of(s);
    const Json& q = s.props;
    os_ << "\n# ---- 스텝 " << index << ": " << s.name << " (" << t << ")\n";
    if (index > 1) apply_bcs(s, false);
    if (t == "frequency") {
      const int modes = q.value("num_modes", 1);
      // 구할 모드가 자유도에 가까우면 Arpack 이 풀지 못한다
      os_ << "set nasa95_lambda [eigen " << (static_cast<std::size_t>(modes) + 2 > free_dofs() ? "-fullGenLapack " : "") << modes << "]\n";
      os_ << "if {[llength $nasa95_lambda] < " << modes << "} { nasa95_fail \"고유치를 구하지 못했다\" }\n";
      os_ << "set nasa95_mode 0\nforeach lam $nasa95_lambda {\n  incr nasa95_mode\n";
      os_ << "  if {$lam <= 0.0} { nasa95_fail \"양수가 아닌 고유치: 모드 $nasa95_mode (구속이 부족하거나 질량이 없다)\" }\n";
      os_ << "  set freq [expr {sqrt($lam) / (2.0 * 3.14159265358979323846)}]\n";
      os_ << "  puts [format \"nasa95: mode %d  frequency %.6E  period %.6E\" $nasa95_mode $freq [expr {1.0 / $freq}]]\n";
      os_ << "  nasa95_begin_frame\n  nasa95_block DISP {D1 D2 D3} 2 $freq " << index << " $nasa95_mode nasa95_mode_shape\n";
      os_ << "  nasa95_progress " << index << " $nasa95_mode 1 $freq $freq 0.0\n}\n";
      return;
    }
    if (t != "static" && t != "dynamic") {
      skip(s, "OpenSees 작성기가 아직 쓰지 못하는 스텝 종류입니다(지금은 static·frequency·dynamic)");
      return;
    }
    const bool dynamic = t == "dynamic";
    write_loads(s, dynamic);
    const double period = q.value("period", 1.0);
    const double dt = q.value("initial_increment", dynamic ? period / 100.0 : period / 10.0);
    const long long count = std::max<long long>(1, std::llround(period / dt));
    long long every = dynamic ? 1 : count;  // 정적 스텝은 마지막 증분만, 동해석은 증분마다(출력 요청의 frequency 로 솎는다)
    if (dynamic) {
      // 지반 운동(BC base_motion): 균일 가진. 함수의 값이 가속도(또는 변위)다
      for (const Object& bc : step_entries(a_, s, "bc")) {
        if (subtype_of(bc) != "base_motion") continue;
        const int direction = bc.props.value("dof", 1);
        if (direction > ndm_ || !has(bc.props, "amplitude")) {
          skip(bc, direction > ndm_ ? "평면 모델에 없는 방향의 지반 운동이다" : "지반 운동에는 시간 함수(amplitude)가 필요하다");
          continue;
        }
        const int series = path_series(bc.props["amplitude"].get<Id>(), bc);
        if (!series) continue;
        const int pattern = ++pattern_tags_;
        os_ << "pattern UniformExcitation " << pattern << " " << direction << " "
            << (bc.props.value("motion", std::string("acceleration")) == "displacement" ? "-disp " : "-accel ") << series << "\n";
        patterns_.push_back(pattern);
      }
      for (const Object* o : a_.model().children(s.id, "output_request"))
        if (!o->suppressed && has(o->props, "frequency")) every = std::max<long long>(1, o->props["frequency"].get<long long>());
    }
    os_ << "wipeAnalysis\nconstraints Plain\nnumberer RCM\nsystem UmfPack\ntest NormDispIncr 1.0e-8 50\nalgorithm Newton\n";
    if (dynamic) {
      const double alpha = q.value("alpha", 0.0);
      if (alpha != 0.0) os_ << "integrator HHT " << num(1.0 + alpha) << "\n";
      else os_ << "integrator Newmark 0.5 0.25\n";
      os_ << "analysis Transient\n";
    } else {
      os_ << "integrator LoadControl " << num(dt) << "\nanalysis Static\n";
    }
    os_ << "set nasa95_t0 [getTime]\n";
    os_ << "for {set nasa95_i 1} {$nasa95_i <= " << count << "} {incr nasa95_i} {\n";
    os_ << "  if {[nasa95_advance" << (dynamic ? " " + num(dt) : "") << "] != 0} { nasa95_fail \"스텝 " << index << " 의 증분 $nasa95_i 에서 수렴하지 못했다\" }\n";
    os_ << "  nasa95_progress " << index << " $nasa95_i 1 [getTime] [expr {[getTime] - $nasa95_t0}] " << num(dt) << "\n";
    os_ << "  if {$nasa95_i % " << every << " == 0 || $nasa95_i == " << count << "} {\n";
    os_ << "    nasa95_begin_frame\n";
    os_ << "    nasa95_block DISP {D1 D2 D3} " << (dynamic ? 1 : 0) << " [getTime] " << index << " $nasa95_i nodeDisp\n";
    if (dynamic) {
      os_ << "    nasa95_block VELO {V1 V2 V3} 1 [getTime] " << index << " $nasa95_i nodeVel\n";
      os_ << "    nasa95_block ACCE {A1 A2 A3} 1 [getTime] " << index << " $nasa95_i nodeAccel\n";
    } else {
      os_ << "    reactions\n    nasa95_block FORC {F1 F2 F3} 0 [getTime] " << index << " $nasa95_i nodeReaction\n";
    }
    os_ << "  }\n}\n";
    // 정적 스텝의 하중은 뒤 스텝에서 그대로 유지된다(시간은 0 으로 되돌린다)
    if (!dynamic) os_ << "loadConst -time 0.0\n";
  }

  const App& a_;
  const Object& cs_;
  const DeckOptions& opt_;
  const Mesh& m_;
  DeckResult out_;
  std::ostringstream os_;
  std::string job_;
  int ndm_ = 3, ndf_ = 6;
  bool nlgeom_ = false;
  std::map<Id, const Object*> prop_of_;  // 요소 → 프로퍼티
  std::set<Id> nodes_, skipped_;
  std::vector<Line> lines_;
  std::map<Id, V3> axis1_;               // 보 요소 → 단면 1축
  std::map<Id, double> point_mass_;
  std::map<std::string, int> transf_;
  std::map<Id, std::vector<int>> fixed_;
  std::vector<int> patterns_;
  int material_tags_ = 0, series_tags_ = 0, pattern_tags_ = 0;
};

}  // namespace

DeckResult write_opensees_deck(const App& app, const Object& analysis_case, const DeckOptions& options) {
  return Writer(app, analysis_case, options).run();
}

}  // namespace nasa95
