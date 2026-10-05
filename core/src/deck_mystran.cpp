// MyStran(오픈소스 Nastran 호환, MIT) 입력 덱 작성(D17, 2026-10-05). 선형 정적(SOL 101)·고유치(SOL 103)·좌굴(SOL 105)만.
// 모델은 솔버 중립이고 이 파일이 Nastran 벌크 데이터로 옮긴다. 카드는 모두 큰 필드 형식(이름 8자 + 16자 필드 4개, 이어지는 줄은 '*')으로 쓴다.
// 솔버로 확인한 것(MyStran 19.0.0): PLOAD1 은 읽기만 하고 하중으로 쓰지 않는다 → 보 등분포 하중은 등가 절점 하중(힘 + 모멘트)으로;
// 이어지는 줄 표시는 10번째 필드에 있어야 한다; PSOLID 는 적분 방식 필드(TWO, GRID)가 있어야 HEXA8 을 받는다.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>

#include "nasa95/app.hpp"
#include "nasa95/deck.hpp"
#include "nasa95/error.hpp"
#include "nasa95/mesh.hpp"
#include "nasa95/solver.hpp"

namespace nasa95 {

namespace {

bool has(const Json& p, const char* key) { return p.contains(key) && !p[key].is_null(); }
std::string subtype_of(const Object& o) { return o.props.value("type", std::string()); }

// 16자 필드의 실수(Nastran 큰 필드): 유효 숫자 9자리
std::string real16(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%16.9G", v);
  std::string s(buf);
  if (s.find_first_of(".Ee") == std::string::npos) {
    s.erase(0, s.find_first_not_of(' '));
    s += '.';
    s = std::string(16 - s.size(), ' ') + s;
  }
  return s;
}
std::string int16(long long v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%16lld", v);
  return buf;
}
std::string str16(const std::string& s) {
  std::string t = s.substr(0, 16);
  return t + std::string(16 - t.size(), ' ');
}

struct Field {
  std::string text;
  Field() : text(16, ' ') {}
  Field(double v) : text(real16(v)) {}
  Field(int v) : text(int16(v)) {}
  Field(long long v) : text(int16(v)) {}
  Field(unsigned long long v) : text(int16(static_cast<long long>(v))) {}
  Field(const char* s) : text(str16(s)) {}
  Field(const std::string& s) : text(str16(s)) {}
};

class Bdf {
 public:
  Bdf(const App& app, const Object& cs, const DeckOptions& options) : a_(app), m_(app.mesh()), cs_(cs), options_(options) {}

  DeckResult run() {
    select_scope();
    const std::vector<const Object*> steps = active_steps();
    if (steps.empty()) throw Error("invalid_state", "케이스에 스텝이 없습니다", {{"object", cs_.id}});
    // 해석 종류: 모든 스텝이 static 이거나, frequency 하나, buckle 하나(앞에 static 이 있어도 된다 — 좌굴은 그 스텝의 하중으로)
    std::string sol;
    for (const Object* s : steps) {
      const std::string t = subtype_of(*s);
      if (t == "static") continue;
      if (t == "frequency" || t == "buckle") {
        if (!sol.empty()) throw Error("not_supported", "MyStran 케이스에는 고유치·좌굴 스텝을 하나만 둘 수 있습니다", {{"object", s->id}});
        sol = t;
      } else {
        throw Error("not_supported", "MyStran 이 지원하지 않는 스텝 종류입니다: " + t, {{"object", s->id}, {"step_type", t}});
      }
    }
    if (sol.empty()) sol = "static";
    // 기준 온도: 초기 조건 온도(균일하면 그 값, 없으면 0) → MAT1 TREF·TEMPD
    for (const Object* ic : a_.model().by_kind("initial_condition"))
      if (!ic->suppressed && subtype_of(*ic) == "temperature" && has(ic->props, "value")) tref_ = ic->props["value"].get<double>();
    node_csys_ = mystran_node_csys(a_, cs_);
    write_csys();
    write_materials();
    write_properties();
    write_mesh();
    // 스텝 → 서브케이스
    std::ostringstream cc;
    cc << "ID NASA-95,case" << cs_.id << "\n";
    cc << "SOL " << (sol == "static" ? "101" : sol == "frequency" ? "103" : "105") << "\n";
    cc << "CEND\n";
    cc << "TITLE = " << ascii(cs_.name.empty() ? "case " + std::to_string(cs_.id) : cs_.name) << "\n";
    cc << "DISPLACEMENT = ALL\nSPCFORCES = ALL\nSTRESS = ALL\nFORCE = ALL\n";
    // 구속: MyStran 은 한 번의 실행에 SPC 셋 하나만 허용한다(솔버 오류 1830) → 모든 스텝의 구속이 같아야 하고 SID 1 로 한 번 쓴다
    {
      std::vector<Id> first;
      for (std::size_t k = 0; k < steps.size(); ++k) {
        std::vector<Id> ids;
        for (const Object& b : step_entries(a_, *steps[k], "bc")) ids.push_back(b.id);
        if (k == 0) first = ids;
        else if (ids != first)
          throw Error("not_supported", "MyStran 은 한 번의 실행에 구속 셋 하나만 허용합니다. 모든 스텝이 같은 구속 셋을 써야 합니다", {{"object", steps[k]->id}});
      }
      write_step_bcs(*steps[0], 1);
    }
    int sub = 0;
    for (const Object* s : steps) {
      const std::string t = subtype_of(*s);
      const int sid = static_cast<int>(s->id);
      write_step_loads(*s, sid);
      if (t == "static" && sol == "buckle") continue;  // 좌굴 덱에서 앞선 정적 스텝은 쓰지 않는다(좌굴 스텝의 하중이 기준 하중)
      cc << "SUBCASE " << ++sub << "\n";
      cc << "  LABEL = " << ascii(s->name) << "\n";
      if (spc_written_.count(1)) cc << "  SPC = 1\n";
      if (t == "static") {
        if (load_written_.count(sid)) cc << "  LOAD = " << sid << "\n";
        if (temp_written_.count(sid)) cc << "  TEMP(LOAD) = " << sid << "\n";
      } else if (t == "frequency") {
        cc << "  METHOD = " << sid << "\n";
        bulk_ << card("EIGRL", {sid, Field(), Field(), std::max(1, s->props.value("num_modes", 10))});
      } else {  // buckle: 같은 서브케이스에 정적 하중, 다음 서브케이스에 고유치
        if (load_written_.count(sid)) cc << "  LOAD = " << sid << "\n";
        cc << "SUBCASE " << ++sub << "\n";
        cc << "  LABEL = " << ascii(s->name) << " buckling\n";
        if (spc_written_.count(1)) cc << "  SPC = 1\n";
        cc << "  METHOD = " << sid << "\n";
        bulk_ << card("EIGRL", {sid, Field(), Field(), std::max(1, s->props.value("num_modes", 5))});
      }
    }
    DeckResult r;
    std::ostringstream out;
    out << "$ NASA-95 " << App::version() << " - MyStran (Nastran format) input deck\n";
    out << cc.str();
    out << "BEGIN BULK\n";
    if (options_.no_analysis) out << "PARAM   CHKPNT  YES\n";
    out << "PARAM   AUTOSPC YES\n";
    out << bulk_.str();
    out << "ENDDATA\n";
    r.text = out.str();
    r.model_text = r.text;
    r.warnings = warnings_, r.skipped = skipped_;
    return r;
  }

 private:
  // ------------------------------------------------------------ 카드
  std::string card(const std::string& name, const std::vector<Field>& fields) {
    // 큰 필드: 물리 줄 = 이름(8) + 필드 4개(64) + 이어짐 표시(8). 논리 카드 한 줄(필드 2~9)은 물리 줄 둘이다.
    // 이어짐 표시는 MSC 규약대로 앞 줄 끝(73~80열)의 "*C###" 와 다음 줄 머리(1~8열)의 같은 글자로 잇는다
    std::string out;
    std::string line = name + "*";
    line += std::string(8 - line.size(), ' ');
    std::size_t k = 0;
    while (true) {
      for (int c = 0; c < 4; ++c) line += (k < fields.size() ? fields[k].text : std::string(16, ' ')), ++k;
      const bool more = k < fields.size() || (k % 8 != 0);  // 논리 카드는 8필드 단위로 끝난다(둘째 물리 줄까지 쓴다)
      if (more) {
        char mark[16];
        std::snprintf(mark, sizeof mark, "*C%-6zu", ++cont_);
        out += line + mark + "\n";
        line = mark;
      } else {
        out += line + "\n";
        break;
      }
    }
    return out;
  }
  static std::string ascii(const std::string& s) {
    std::string out;
    for (unsigned char c : s) out += (c >= 32 && c < 127) ? static_cast<char>(c) : '_';
    return out.substr(0, 64);
  }
  void warn(const std::string& code, const std::string& msg, Id object = 0) {
    Json w{{"code", code}, {"message", msg}};
    if (object) w["object"] = object;
    warnings_.push_back(w);
  }
  void skip(const Object& o, const std::string& reason) {
    skipped_.push_back(Json{{"object", o.id}, {"kind", o.kind}, {"name", o.name}, {"type", subtype_of(o)}, {"reason", reason}});
    bulk_ << "$ skipped: " << o.kind << " " << o.name << " (" << reason << ")\n";
  }

  // ------------------------------------------------------------ 범위
  void select_scope() {
    if (has(cs_.props, "scope")) {
      scoped_ = true;
      for (const Json& e : resolve_target(a_, cs_.props["scope"], "elements")) elems_.insert(e.get<Id>());
      for (std::size_t i = 0; i < m_.element_count(); ++i)
        if (elems_.count(m_.element_ids()[i]))
          for (std::size_t k = 0; k < m_.node_count_at(i); ++k) nodes_.insert(m_.nodes_at(i)[k]);
    }
  }
  bool in_scope_elem(Id e) const { return scoped_ ? elems_.count(e) > 0 : m_.has_element(e); }
  bool in_scope_node(Id n) const { return scoped_ ? nodes_.count(n) > 0 : m_.has_node(n); }
  std::vector<const Object*> active_steps() const {
    std::vector<const Object*> out;
    for (const Object* s : a_.model().children(cs_.id, "step"))
      if (!s->suppressed) out.push_back(s);
    return out;
  }

  // ------------------------------------------------------------ 국부 좌표계(구속의 csys): CORD2R/CORD2C + GRID CD
  void write_csys() {
    std::set<Id> used;
    for (const auto& [n, c] : node_csys_) used.insert(c);
    for (Id cid : used) {
      const Object* cs = a_.model().find(cid);
      if (!cs) continue;
      Vec3 ax[3];
      csys_axes_at(*cs, Vec3{0, 0, 0}, ax);  // 직교: 고정 축. 원통: 기준 축(e1 반경 기준, e3 축)
      const Vec3 o = {cs->props["origin"][0].get<double>(), cs->props["origin"][1].get<double>(), cs->props["origin"][2].get<double>()};
      const std::string t = subtype_of(*cs);
      if (t == "spherical") warn("unsupported_by_solver", "구면 좌표계는 MyStran 덱에서 직교 좌표계로 씁니다: " + cs->name, cs->id);
      // A = 원점, B = z 축 위의 점(e3), C = x-z 평면 위의 점(e1)
      bulk_ << card(t == "cylindrical" ? "CORD2C" : "CORD2R", {static_cast<long long>(cid), 0LL, o[0], o[1], o[2], o[0] + ax[2][0], o[1] + ax[2][1], o[2] + ax[2][2],
                                                               o[0] + ax[0][0], o[1] + ax[0][1], o[2] + ax[0][2]});
    }
  }

  // ------------------------------------------------------------ 재료
  void write_materials() {
    for (const Object* mat : a_.model().by_kind("material")) {
      if (mat->suppressed) continue;
      auto bit = mat->props.find("behaviors");
      if (bit == mat->props.end() || !bit->contains("elastic")) {
        warn("missing_behavior", "탄성 정의가 없는 재료는 MAT1 로 쓰지 못합니다: " + mat->name, mat->id);
        continue;
      }
      const Json& el = (*bit)["elastic"];
      const std::string t = el.value("type", std::string("iso"));
      if (t != "iso" || !el.contains("data") || el["data"].empty()) {
        skip(*mat, "material_behavior:elastic:" + t);
        continue;
      }
      const double E = el["data"][0][0].get<double>(), nu = el["data"][0][1].get<double>();
      double rho = 0, alpha = 0;
      if (bit->contains("density") && !(*bit)["density"]["data"].empty()) rho = (*bit)["density"]["data"][0][0].get<double>();
      if (bit->contains("expansion") && (*bit)["expansion"].contains("data") && !(*bit)["expansion"]["data"].empty())
        alpha = (*bit)["expansion"]["data"][0][0].get<double>();
      for (auto it = bit->begin(); it != bit->end(); ++it)
        if (it.key() != "elastic" && it.key() != "density" && it.key() != "expansion" && it.key() != "allowable")
          warn("unsupported_by_solver", "MyStran 은 선형 탄성만 풉니다. 재료 " + mat->name + " 의 " + it.key() + " 은(는) 쓰지 않습니다", mat->id);
      // MAT1: MID, E, G(빈칸 = E/2(1+ν)), NU, RHO, A, TREF
      bulk_ << card("MAT1", {static_cast<long long>(mat->id), E, Field(), nu, rho, alpha, tref_});  // TREF = 초기 온도
      material_ids_.insert(mat->id);
    }
  }

  // ------------------------------------------------------------ 프로퍼티
  // 요소 → 프로퍼티. MyStran 은 요소마다 PID 가 있어야 한다
  void write_properties() {
    const Object* settings = a_.find_settings();
    if (settings)
      for (const Json& row : settings->props.value("beam_directions", Json::array()))
        beam_dir_[row[0].get<Id>()] = {row[1].get<double>(), row[2].get<double>(), row[3].get<double>()};
    for (const Object* p : a_.model().by_kind("property")) {
      if (p->suppressed || !has(p->props, "target")) continue;
      const std::string t = subtype_of(*p);
      std::vector<Id> ids;
      try {
        for (const Json& e : resolve_target(a_, p->props["target"], "elements"))
          if (in_scope_elem(e.get<Id>())) ids.push_back(e.get<Id>());
      } catch (const Error& e) {
        skip(*p, e.code());
        continue;
      }
      if (ids.empty()) continue;
      const Id mid = has(p->props, "material") ? p->props["material"].get<Id>() : 0;
      if (t != "mass" && t != "spring" && !material_ids_.count(mid)) {
        warn("missing_reference", "덱에 없는 재료를 가리킵니다: " + p->name, p->id);
        continue;
      }
      const long long pid = static_cast<long long>(p->id);
      bool ok = true;
      if (t == "beam") {
        ok = write_beam_property(*p, pid, static_cast<long long>(mid));
      } else if (t == "truss") {
        bulk_ << card("PROD", {pid, static_cast<long long>(mid), p->props.value("area", 0.0), 0.0});
      } else if (t == "shell" || t == "membrane") {
        const double th = p->props.value("thickness", 0.0);
        // PSHELL: PID, MID1, T, MID2(굽힘), 12I/T³, MID3(횡전단)
        bulk_ << card("PSHELL", {pid, static_cast<long long>(mid), th, static_cast<long long>(mid), 1.0, static_cast<long long>(mid)});
        // 오프셋: CalculiX 규약(두께 단위, +0.5 = 기준면이 윗면 → 중립면 = 기준면 − offset·t·n) → Nastran ZOFFS(길이, 중립면 = 기준면 + ZOFFS·n)
        if (has(p->props, "offset")) zoffs_[p->id] = -p->props["offset"].get<double>() * th;
      } else if (t == "solid") {
        bulk_ << card("PSOLID", {pid, static_cast<long long>(mid), Field(), "TWO", "GRID"});
      } else if (t == "mass") {
        mass_props_[p->id] = p->props.value("mass", 0.0);
      } else {
        skip(*p, "property_type:" + t);
        ok = false;
      }
      if (ok)
        for (Id e : ids) prop_of_[e] = p;
    }
  }

  bool write_beam_property(const Object& p, long long pid, long long mid) {
    const Json& q = p.props;
    const std::string sec = q.value("section", std::string("rect"));
    const std::vector<double> d = q.value("dimensions", std::vector<double>());
    if (has(q, "offset1") || has(q, "offset2")) warn("unsupported_by_solver", "보 오프셋은 MyStran 덱에 쓰지 않습니다: " + p.name, p.id);
    // 직사각형·원형·파이프는 PBARL(응력 회복점이 자동), 형강·박스·타원은 PBAR(A, I1, I2, J — 솔버로 확인: PBARL I 의 치수 해석이 달라 쓰지 않는다).
    // Nastran 평면 1 = 요소 y 축 = 우리 1축 → I1 = 1축을 따라 휘는 굽힘 = i22
    if (sec == "rect" && d.size() >= 2) {
      // 솔버로 확인(MyStran 19.0.0): BAR 의 DIM1 은 z(평면 2) 폭, DIM2 는 y(평면 1 = 우리 1축) 높이 → 평면 1 굽힘에 DIM2³ 이 쓰인다
      bulk_ << card("PBARL", {pid, mid, Field(), "BAR", Field(), Field(), Field(), Field(), d[1], d[0]});
    } else if (sec == "circ" && d.size() >= 2 && std::fabs(d[0] - d[1]) < 1e-9 * std::max(d[0], 1.0)) {
      bulk_ << card("PBARL", {pid, mid, Field(), "ROD", Field(), Field(), Field(), Field(), d[0] / 2});
    } else if (sec == "pipe" && d.size() >= 2) {
      bulk_ << card("PBARL", {pid, mid, Field(), "TUBE", Field(), Field(), Field(), Field(), d[0], d[0] - d[1]});
    } else if (sec == "general" && d.size() >= 5) {
      bulk_ << card("PBAR", {pid, mid, d[0], d[3], d[1], 0.0});  // A, I1(=우리 I22), I2(=I11), J 미상 → 0
      warn("incomplete", "일반 단면의 비틀림 상수 J 를 모릅니다(0 으로 씀): " + p.name, p.id);
    } else {
      Json v;
      try {
        v = beam_section_properties(p);
      } catch (const Error& e) {
        skip(p, e.code());
        return false;
      }
      bulk_ << card("PBAR", {pid, mid, v["area"].get<double>(), v["i22"].get<double>(), v["i11"].get<double>(), v.value("j", 0.0)});
    }
    return true;
  }

  // ------------------------------------------------------------ 메시
  void write_mesh() {
    const auto& ids = m_.node_ids();
    const auto& xyz = m_.node_xyz();
    for (std::size_t i = 0; i < ids.size(); ++i) {
      if (!in_scope_node(ids[i])) continue;
      std::vector<Field> g{static_cast<long long>(ids[i]), Field(), xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2]};
      if (auto it = node_csys_.find(ids[i]); it != node_csys_.end()) g.push_back(static_cast<long long>(it->second));  // CD: 변위·구속의 국부 좌표계
      bulk_ << card("GRID", g);
    }
    std::size_t unassigned = 0;
    for (std::size_t i = 0; i < m_.element_count(); ++i) {
      const Element e = m_.element_at(i);
      if (!in_scope_elem(e.id)) continue;
      const ShapeInfo& info = shape_info(e.shape);
      const std::string shape = info.name;
      auto pit = prop_of_.find(e.id);
      if (shape == "point1") {
        // 점 질량: CONM2(EID, G, CID, M)
        if (pit != prop_of_.end() && mass_props_.count(pit->second->id))
          bulk_ << card("CONM2", {static_cast<long long>(e.id), static_cast<long long>(e.nodes[0]), 0LL, mass_props_[pit->second->id]});
        continue;
      }
      if (pit == prop_of_.end()) {
        ++unassigned;
        continue;
      }
      const long long eid = static_cast<long long>(e.id), pid = static_cast<long long>(pit->second->id);
      const std::string pt = subtype_of(*pit->second);
      std::vector<Field> f{eid, pid};
      if (shape == "line2") {
        if (pt == "truss") {
          f.push_back(static_cast<long long>(e.nodes[0])), f.push_back(static_cast<long long>(e.nodes[1]));
          bulk_ << card("CROD", f);
        } else {
          Vec3 dir{0, 0, -1};
          if (auto it = beam_dir_.find(e.id); it != beam_dir_.end()) dir = it->second;
          else if (has(pit->second->props, "direction")) dir = {pit->second->props["direction"][0].get<double>(), pit->second->props["direction"][1].get<double>(), pit->second->props["direction"][2].get<double>()};
          f.push_back(static_cast<long long>(e.nodes[0])), f.push_back(static_cast<long long>(e.nodes[1]));
          f.push_back(dir[0]), f.push_back(dir[1]), f.push_back(dir[2]);
          bulk_ << card("CBAR", f);
        }
        element_count_++;
        continue;
      }
      static const std::map<std::string, const char*> names = {{"tri3", "CTRIA3"}, {"quad4", "CQUAD4"}, {"tet4", "CTETRA"}, {"tet10", "CTETRA"},
                                                               {"hex8", "CHEXA"}, {"hex20", "CHEXA"}, {"wedge6", "CPENTA"}};
      auto nit = names.find(shape);
      if (nit == names.end()) {
        warn("unsupported_by_solver", "MyStran 에 없는 요소 형상입니다: " + shape + " (요소 " + std::to_string(e.id) + ")");
        continue;
      }
      // 절점 순서: 1차는 같고, tet10 은 모서리 절점 순서가 같다(1-2, 2-3, 3-1, 1-4, 2-4, 3-4). hex20 도 같은 순서(모서리 12개)
      for (Id n : e.nodes) f.push_back(static_cast<long long>(n));
      if (auto z = zoffs_.find(pit->second->id); z != zoffs_.end() && info.dim == 2) {  // CQUAD4: THETA(7), ZOFFS(8) / CTRIA3: THETA(6), ZOFFS(7)
        f.push_back(Field());
        f.push_back(z->second);
      }
      bulk_ << card(nit->second, f);
      element_count_++;
    }
    if (unassigned) warn("unassigned_elements", "프로퍼티가 없는 요소 " + std::to_string(unassigned) + "개는 덱에 쓰지 않았습니다(MyStran 은 요소마다 프로퍼티가 필요)");
  }

  // ------------------------------------------------------------ 하중
  void write_step_loads(const Object& step, int sid) {
    for (const Object& l : step_entries(a_, step, "load")) {
      const std::string t = subtype_of(l);
      try {
        if (t == "force" || t == "moment") {
          const Vec3 c = vec(l.props["components"]);
          const double mag = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
          if (mag <= 0) continue;
          for (const Json& n : resolve_target(a_, l.props["target"], "nodes")) {
            if (!in_scope_node(n.get<Id>())) continue;
            bulk_ << card(t == "force" ? "FORCE" : "MOMENT", {static_cast<long long>(sid), static_cast<long long>(n.get<Id>()), 0LL, mag, c[0] / mag, c[1] / mag, c[2] / mag});
            load_written_.insert(sid);
          }
        } else if (t == "gravity" || t == "acceleration") {
          const Vec3 dir = vec(l.props["direction"]);
          const double g = l.props.value("value", 0.0);
          bulk_ << card("GRAV", {static_cast<long long>(sid), 0LL, g, dir[0], dir[1], dir[2]});
          load_written_.insert(sid);
          if (!whole_model(l.props["target"])) warn("approximation", "MyStran 의 중력(GRAV)은 모델 전체에 걸립니다. 일부 요소만 고른 중력도 전체에 적용됩니다: " + l.name, l.id);
        } else if (t == "pressure") {
          write_pressure(l, sid);
        } else if (t == "line_load") {
          write_line_load(l, sid);
        } else if (t == "temperature") {
          const double T = l.props.value("value", 0.0);
          for (const Json& n : resolve_target(a_, l.props["target"], "nodes"))
            if (in_scope_node(n.get<Id>())) bulk_ << card("TEMP", {static_cast<long long>(sid), static_cast<long long>(n.get<Id>()), T});
          if (!temp_written_.count(sid)) bulk_ << card("TEMPD", {static_cast<long long>(sid), tref_});  // 온도를 주지 않은 절점은 기준 온도
          temp_written_.insert(sid);
        } else {
          skip(l, "load_type:" + t);
        }
      } catch (const Error& e) {
        skip(l, e.code());
      }
    }
  }
  bool whole_model(const Json& target) const {
    try {
      std::size_t n = 0;
      for (const Json& e : resolve_target(a_, target, "elements"))
        if (in_scope_elem(e.get<Id>())) ++n;
      return n >= element_count_;
    } catch (const Error&) {
      return false;
    }
  }
  static Vec3 vec(const Json& j) { return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()}; }

  // 압력: 쉘 요소는 PLOAD4(요소 법선 방향으로 미는 쪽이 양 — 우리 규약과 같다). 솔리드 요소면은 MyStran 이 PLOAD4 의 G1/G3 를 아직
  // 지원하지 않으므로(솔버 메시지 "CODE NOT WRITTEN YET") 등가 절점 하중(일관 하중: 1차 면은 균등, 2차 면은 꼭짓점 0 또는 −A/12, 변 중앙 A/3)으로 쓴다.
  // 면 꼭짓점 순서의 외적(CalculiX 면 표 기준)은 요소 안쪽을 향한다 — 양의 압력은 그 방향으로 누른다(CalculiX 결과와 대조해 확인)
  void write_pressure(const Object& l, int sid) {
    const double p = l.props.value("value", 0.0);
    if (has(l.props, "distribution")) warn("unsupported_by_solver", "공간 분포 압력은 MyStran 덱에서 상수로 씁니다: " + l.name, l.id);
    std::map<Id, Vec3> nodal;
    bool solid = false;
    for (const Json& fc : resolve_target(a_, l.props["target"], "faces")) {
      const Id eid = fc[0].get<Id>();
      if (!in_scope_elem(eid)) continue;
      const Element e = m_.element(eid);
      const ShapeInfo& info = shape_info(e.shape);
      if (info.dim == 2) {
        bulk_ << card("PLOAD4", {static_cast<long long>(sid), static_cast<long long>(eid), p});
        load_written_.insert(sid);
      } else if (info.dim == 3) {
        const int face = fc[1].get<int>();
        const auto& faces = shape_faces(e.shape);
        if (face < 1 || face > static_cast<int>(faces.size())) continue;
        const std::vector<int>& fn = faces[static_cast<std::size_t>(face - 1)];
        std::vector<Vec3> pts;
        for (int k : fn) pts.push_back(m_.node(e.nodes[static_cast<std::size_t>(k)]));
        const std::size_t corners = fn.size() >= 6 ? fn.size() / 2 : fn.size();  // 2차 면: 앞 절반이 꼭짓점
        // 바깥 법선·넓이(꼭짓점 다각형을 삼각형으로 나눠 합한다)
        Vec3 nsum{0, 0, 0};
        for (std::size_t k = 1; k + 1 < corners; ++k) {
          const Vec3 u{pts[k][0] - pts[0][0], pts[k][1] - pts[0][1], pts[k][2] - pts[0][2]}, v{pts[k + 1][0] - pts[0][0], pts[k + 1][1] - pts[0][1], pts[k + 1][2] - pts[0][2]};
          nsum[0] += 0.5 * (u[1] * v[2] - u[2] * v[1]), nsum[1] += 0.5 * (u[2] * v[0] - u[0] * v[2]), nsum[2] += 0.5 * (u[0] * v[1] - u[1] * v[0]);
        }
        const double area = std::sqrt(nsum[0] * nsum[0] + nsum[1] * nsum[1] + nsum[2] * nsum[2]);
        if (area <= 0) continue;
        const Vec3 nrm{nsum[0] / area, nsum[1] / area, nsum[2] / area};
        std::vector<double> w(fn.size(), 0.0);
        if (fn.size() == 3) w = {1.0 / 3, 1.0 / 3, 1.0 / 3};
        else if (fn.size() == 4) w = {0.25, 0.25, 0.25, 0.25};
        else if (fn.size() == 6) w = {0, 0, 0, 1.0 / 3, 1.0 / 3, 1.0 / 3};
        else if (fn.size() == 8) w = {-1.0 / 12, -1.0 / 12, -1.0 / 12, -1.0 / 12, 1.0 / 3, 1.0 / 3, 1.0 / 3, 1.0 / 3};
        for (std::size_t k = 0; k < fn.size(); ++k) {
          Vec3& f = nodal[e.nodes[static_cast<std::size_t>(fn[k])]];
          for (int c = 0; c < 3; ++c) f[static_cast<std::size_t>(c)] += p * area * w[k] * nrm[static_cast<std::size_t>(c)];  // 솔버로 확인: 면 꼭짓점 순서의 외적이 안쪽을 향한다(CalculiX 결과와 같은 부호)
        }
        solid = true;
      }
    }
    for (const auto& [n, f] : nodal) {
      const double mag = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
      if (mag <= 0) continue;
      bulk_ << card("FORCE", {static_cast<long long>(sid), static_cast<long long>(n), 0LL, mag, f[0] / mag, f[1] / mag, f[2] / mag});
      load_written_.insert(sid);
    }
    if (solid) warn("approximation", "솔리드 면 압력을 등가 절점 하중으로 썼습니다(MyStran 은 솔리드 PLOAD4 를 아직 지원하지 않는다): " + l.name, l.id);
  }

  // 보 등분포 하중 → 등가 절점 하중(고정단 등가: 힘 qL/2, 모멘트 ±qL²/12). MyStran 은 PLOAD1 을 쓰지 않는다
  void write_line_load(const Object& l, int sid) {
    const Vec3 q = vec(l.props["components"]);
    std::map<Id, Vec3> force, moment;
    for (const Json& ej : resolve_target(a_, l.props["target"], "elements")) {
      const Id eid = ej.get<Id>();
      if (!in_scope_elem(eid)) continue;
      const Element e = m_.element(eid);
      if (shape_info(e.shape).dim != 1 || e.nodes.size() < 2) continue;
      const Vec3 p0 = m_.node(e.nodes[0]), p1 = m_.node(e.nodes[1]);
      const Vec3 d{p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
      const double L = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
      if (L <= 0) continue;
      // 축에 수직한 하중 성분만 모멘트를 만든다: M = (L/12) · (축 × q_perp)·L → 끝 A 에 +, 끝 B 에 −
      const Vec3 ax{d[0] / L, d[1] / L, d[2] / L};
      const double qa = q[0] * ax[0] + q[1] * ax[1] + q[2] * ax[2];
      const Vec3 qp{q[0] - qa * ax[0], q[1] - qa * ax[1], q[2] - qa * ax[2]};
      const Vec3 mvec{(ax[1] * qp[2] - ax[2] * qp[1]) * L * L / 12, (ax[2] * qp[0] - ax[0] * qp[2]) * L * L / 12, (ax[0] * qp[1] - ax[1] * qp[0]) * L * L / 12};
      for (int end = 0; end < 2; ++end) {
        Vec3& f = force[e.nodes[static_cast<std::size_t>(end)]];
        Vec3& mm = moment[e.nodes[static_cast<std::size_t>(end)]];
        for (int k = 0; k < 3; ++k) f[k] += q[k] * L / 2, mm[k] += (end == 0 ? 1.0 : -1.0) * mvec[k];
      }
    }
    for (const auto& [n, f] : force) {
      const double mag = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
      if (mag > 0) bulk_ << card("FORCE", {static_cast<long long>(sid), static_cast<long long>(n), 0LL, mag, f[0] / mag, f[1] / mag, f[2] / mag}), load_written_.insert(sid);
      const Vec3& mm = moment[n];
      const double mm_mag = std::sqrt(mm[0] * mm[0] + mm[1] * mm[1] + mm[2] * mm[2]);
      if (mm_mag > 1e-12 * std::max(mag, 1.0)) bulk_ << card("MOMENT", {static_cast<long long>(sid), static_cast<long long>(n), 0LL, mm_mag, mm[0] / mm_mag, mm[1] / mm_mag, mm[2] / mm_mag});
    }
    warn("approximation", "보 등분포 하중을 등가 절점 하중(힘 + 고정단 모멘트)으로 썼습니다(MyStran 은 PLOAD1 을 쓰지 않는다): " + l.name, l.id);
  }

  // ------------------------------------------------------------ 경계조건
  void write_step_bcs(const Object& step, int sid) {
    for (const Object& b : step_entries(a_, step, "bc")) {
      const std::string t = subtype_of(b);
      try {
        if (t == "displacement") {
          const std::vector<int> dofs = b.props["dofs"].get<std::vector<int>>();
          const Json values = b.props.value("values", Json::array());
          for (const Json& n : resolve_target(a_, b.props["target"], "nodes")) {
            if (!in_scope_node(n.get<Id>())) continue;
            for (std::size_t k = 0; k < dofs.size(); ++k) {
              const double v = k < values.size() && values[k].is_number() ? values[k].get<double>() : 0.0;
              // SPC: SID, G, C, D(값. 0 이 아니면 강제 변위)
              bulk_ << card("SPC", {static_cast<long long>(sid), static_cast<long long>(n.get<Id>()), static_cast<long long>(dofs[k]), v});
            }
            spc_written_.insert(sid);
          }
        } else if (t == "symmetry" || t == "antisymmetry") {
          const std::string axis = b.props.value("normal", std::string("x"));
          const int k = axis == "x" ? 1 : axis == "y" ? 2 : 3;
          // 대칭: 법선 병진 + 면 안 회전 둘(회전 자유도는 보·쉘에만 있다 — AUTOSPC 가 나머지를 처리). 반대칭: 면 안 병진 둘 + 법선 회전
          std::vector<int> dofs = t == "symmetry" ? std::vector<int>{k, (k % 3) + 4, ((k + 1) % 3) + 4} : std::vector<int>{(k % 3) + 1, ((k + 1) % 3) + 1, k + 3};
          for (const Json& n : resolve_target(a_, b.props["target"], "nodes")) {
            if (!in_scope_node(n.get<Id>())) continue;
            for (int dof : dofs) bulk_ << card("SPC", {static_cast<long long>(sid), static_cast<long long>(n.get<Id>()), static_cast<long long>(dof), 0.0});
            spc_written_.insert(sid);
          }
        } else {
          skip(b, "bc_type:" + t);
        }
      } catch (const Error& e) {
        skip(b, e.code());
      }
    }
  }

  const App& a_;
  const Mesh& m_;
  const Object& cs_;
  DeckOptions options_;
  bool scoped_ = false;
  std::set<Id> elems_, nodes_;
  std::ostringstream bulk_;
  Json warnings_ = Json::array(), skipped_ = Json::array();
  std::set<Id> material_ids_;
  std::map<Id, const Object*> prop_of_;
  std::map<Id, double> mass_props_;
  std::map<Id, Vec3> beam_dir_;
  std::set<int> load_written_, spc_written_, temp_written_;
  double tref_ = 0.0;                 // 기준 온도(초기 조건)
  std::map<Id, Id> node_csys_;        // 절점 → 구속 좌표계(GRID CD)
  std::map<Id, double> zoffs_;        // 쉘 프로퍼티 → ZOFFS
  std::size_t element_count_ = 0;
  std::size_t cont_ = 0;  // 이어짐 표시 번호
};

}  // namespace

// 구속(변위)에 csys 가 있는 절점 → 좌표계(GRID CD). 한 절점에 좌표계가 둘이면 먼저 나온 것. 덱 작성기와 결과 변환기가 같이 쓴다
std::map<Id, Id> mystran_node_csys(const App& a, const Object& cs) {
  std::map<Id, Id> out;
  for (const Object* st : a.model().children(cs.id, "step")) {
    if (st->suppressed) continue;
    for (const Object& b : step_entries(a, *st, "bc")) {
      if (!has(b.props, "csys") || !b.props["csys"].is_number() || !has(b.props, "target")) continue;
      try {
        for (const Json& n : resolve_target(a, b.props["target"], "nodes")) out.emplace(n.get<Id>(), b.props["csys"].get<Id>());
      } catch (const Error&) {
      }
    }
  }
  return out;
}

DeckResult write_mystran_deck(const App& app, const Object& analysis_case, const DeckOptions& options) {
  return Bdf(app, analysis_case, options).run();
}

}  // namespace nasa95
