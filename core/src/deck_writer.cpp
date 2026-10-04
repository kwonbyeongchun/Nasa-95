// CalculiX 입력 파일(덱) 쓰기 (CAS-10).
//
// 구성: 메시 → 셋 → 방향·재료·섹션 → 함수(*AMPLITUDE)·출력 시점 → 구속·접촉 → 초기 조건 → 스텝.
// 적용 대상(노드·요소·면의 목록)은 이름 붙은 셋으로 바꿔 쓴다. 사용자가 만든 셋 하나를 그대로 가리키면 그 이름을 쓰고,
// 그 밖에는 내부 셋(OFEP_N1, OFEP_E1, OFEP_S1 …)을 만든다. 내부 셋은 쓰는 곳보다 앞에 모아 적는다.
// 덱에 쓰지 못한 객체는 조용히 빠뜨리지 않고 skipped 에 알리고, 덱에도 주석으로 남긴다.
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <sstream>

#include "ofep/deck.hpp"
#include "ofep/error.hpp"
#include "ofep/expr.hpp"

namespace ofep {

namespace {

bool has(const Json& p, const char* key) { return p.contains(key) && !p[key].is_null(); }
std::string subtype_of(const Object& o) { return o.props.value("type", std::string()); }

// 실수 하나. CalculiX 는 한 칸을 20자까지만 읽으므로 그 안에 들어가게 쓴다.
std::string num(double v) {
  char buf[400];
  // 보통 크기의 값은 지수 없이 쓴다(읽기 쉽게). 그 밖에는 가장 짧은 표기.
  const double mag = std::fabs(v);
  const bool plain = v == 0.0 || (mag >= 1e-4 && mag < 1e15);
  const auto r = plain ? std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed) : std::to_chars(buf, buf + sizeof buf, v);
  std::string s(buf, r.ptr);
  if (s.size() > 19) {
    std::snprintf(buf, sizeof buf, "%.12g", v);
    s = buf;
  }
  // 실수에는 항상 소수점을 붙인다. CalculiX 는 소수점이 없으면 정수(자유도 번호 등)로 읽는 카드가 있다(*SPRING).
  if (s.find_first_of(".eEn") == std::string::npos) s += '.';
  return s;
}
std::string num(const Json& v) { return num(v.get<double>()); }
std::string integer(const Json& v) { return std::to_string(static_cast<long long>(std::llround(v.get<double>()))); }

bool is_shell_type(const std::string& t) { return t.rfind("S", 0) == 0 || t.rfind("M3D", 0) == 0; }

class Deck {
 public:
  Deck(const App& app, const Object* cs, const DeckOptions& options = {}) : a_(app), m_(app.mesh()), case_(cs), options_(options) {}

  DeckResult run() {
    extra_node_ = m_.max_node_id();
    select_scope();
    write_mesh();
    write_user_sets();
    if (case_) {
      write_normals();
      write_orientations();
      write_materials();
      write_sections();
      write_functions();
      write_transforms();
      write_constraints();
      write_contact();
      write_initial_conditions();
      write_model_settings();
      write_steps();
    }
    DeckResult r;
    std::ostringstream out;
    out << "** open-fep " << App::version() << " - CalculiX input deck\n";
    if (case_ && options_.restart_from_step <= 0) {  // 재시작 덱은 *RESTART, READ 가 첫 카드여야 한다
      out << "*HEADING\n";
      const bool ascii = std::all_of(case_->name.begin(), case_->name.end(), [](unsigned char c) { return c >= 32 && c < 127; });
      out << (ascii && !case_->name.empty() ? case_->name : "case " + std::to_string(case_->id)) << "\n";
    }
    if (options_.restart_from_step > 0) {
      // 재시작 덱: 모델 정의(셋 포함)는 재시작 파일에 있다. 솔버로 확인: 재시작 덱에 *NSET/*ELSET 을 두면
      // "should be placed before all step definitions" 오류가 난다 → 셋도 쓰지 않는다. 새 스텝이 쓰는 셋은 앞선 실행의 덱에 있던 것이어야 한다
      // (셋 이름은 같은 모델이면 같게 만들어진다).
      out << "*RESTART, READ, STEP=" << options_.restart_from_step << "\n";
      warn("restart_sets", "재시작 덱에는 셋을 쓸 수 없다: 새 스텝이 쓰는 셋(노드·요소·면)은 앞선 실행의 덱에 있던 것이어야 한다");
      r.mesh_text.clear();
      r.model_text = steps_.str();
    } else {
      r.mesh_text = mesh_.str() + sets_.str();
      r.model_text = model_.str() + steps_.str();
    }
    out << r.mesh_text << r.model_text;
    r.text = out.str();
    r.warnings = warnings_, r.skipped = skipped_;
    return r;
  }

 private:
  // ------------------------------------------------------------ 알림
  bool contact_method_warned_ = false;
  void warn(const char* code, const std::string& message, Id object = 0) {
    Json w{{"code", code}, {"message", message}};
    if (object) w["object"] = object;
    warnings_.push_back(std::move(w));
  }
  void skip(std::ostream& os, const Object& o, const std::string& reason) {
    skipped_.push_back(Json{{"object", o.id}, {"kind", o.kind}, {"name", o.name}, {"type", subtype_of(o)}, {"reason", reason}});
    os << "** skipped: " << o.kind << " " << o.id << " (" << subtype_of(o) << "): " << reason << "\n";
  }

  // ------------------------------------------------------------ 범위
  void select_scope() {
    if (case_ && has(case_->props, "scope")) {
      scoped_ = true;
      for (const Json& e : resolve_target(a_, case_->props["scope"], "elements")) elems_.insert(e.get<Id>());
      for (Id e : elems_)
        for (Id n : m_.element(e).nodes)
          if (n) nodes_.insert(n);
    }
  }
  bool in_scope_elem(Id e) const { return scoped_ ? elems_.count(e) > 0 : m_.has_element(e); }
  bool in_scope_node(Id n) const { return scoped_ ? nodes_.count(n) > 0 : m_.has_node(n); }

  // ------------------------------------------------------------ 목록 쓰기
  template <class Seq>
  static void write_ids(std::ostream& os, const Seq& ids, int per_line = 10) {
    int k = 0;
    for (const auto& id : ids) {
      if (k) os << (k % per_line == 0 ? ",\n" : ", ");
      os << id;
      ++k;
    }
    os << "\n";
  }
  // 표의 한 행: 한 줄에 값 8개까지.
  static void write_row(std::ostream& os, const Json& row) {
    for (std::size_t i = 0; i < row.size(); ++i) {
      os << num(row[i]);
      if (i + 1 < row.size()) os << ((i + 1) % 8 == 0 ? ",\n" : ", ");
    }
    os << "\n";
  }
  static void write_rows(std::ostream& os, const Json& rows) {
    for (const Json& row : rows) write_row(os, row);
  }
  // 뒤쪽의 빈 칸을 뺀 데이터 줄. 모두 비었으면 쓰지 않는다.
  static void write_optional(std::ostream& os, std::vector<std::string> cells) {
    while (!cells.empty() && cells.back().empty()) cells.pop_back();
    if (cells.empty()) return;
    for (std::size_t i = 0; i < cells.size(); ++i) os << cells[i] << (i + 1 < cells.size() ? ", " : "\n");
  }
  static std::string opt(const Json& p, const char* key) { return has(p, key) ? num(p[key]) : std::string(); }

  // ------------------------------------------------------------ 메시
  void write_mesh() {
    std::ostream& os = mesh_;
    if (m_.node_count() == 0) {
      warn("empty_mesh", "메시가 없습니다");
      return;
    }
    os << "*NODE, NSET=NALL\n";
    const auto& ids = m_.node_ids();
    const auto& xyz = m_.node_xyz();
    for (std::size_t i = 0; i < ids.size(); ++i) {
      if (scoped_ && !nodes_.count(ids[i])) continue;
      os << ids[i] << ", " << num(xyz[3 * i]) << ", " << num(xyz[3 * i + 1]) << ", " << num(xyz[3 * i + 2]) << "\n";
    }
    // 요소: (솔버 타입, 메시 파트)별로 묶는다(처음 나온 순서). 파트는 그 카드의 ELSET 이 된다.
    std::vector<std::pair<std::string, Id>> order;
    std::map<std::pair<std::string, Id>, std::vector<std::size_t>> groups;
    for (std::size_t i = 0; i < m_.element_count(); ++i) {
      const Id id = m_.element_ids()[i];
      if (scoped_ && !elems_.count(id)) continue;
      const Element e = m_.element_at(i);
      const std::string type = e.type.empty() ? shape_info(e.shape).default_type : e.type;
      if (type.empty())
        throw Error("unsupported", std::string(shape_info(e.shape).name) + " 요소는 CalculiX 에 없습니다(요소 " + std::to_string(id) + ")",
                    {{"element", id}});
      const std::pair<std::string, Id> key{type, a_.model().find(e.part) ? e.part : 0};
      if (!groups.count(key)) order.push_back(key);
      groups[key].push_back(i);
      type_of_[id] = type;
      const int dim = shape_info(e.shape).dim;
      if ((dim == 2 && is_shell_type(type)) || (dim == 1 && type.rfind("B3", 0) == 0)) has_rotations_ = true;
    }
    // 사용자 요소는 *ELEMENT 앞에 *USER ELEMENT 정의가 있어야 한다(매뉴얼 7.136). U1(Timoshenko 보)은 매뉴얼의 예 그대로.
    // US3 의 적분점 수·자유도는 매뉴얼에 없어 쓰지 않는다(사용자가 덱에 더해야 한다).
    std::set<std::string> user_defined;
    for (const auto& key : order) {
      if (key.first.rfind("U", 0) == 0 && user_defined.insert(key.first).second) {
        if (key.first == "U1") os << "*USER ELEMENT, TYPE=U1, INTEGRATION POINTS=0, MAXDOF=6, NODES=2\n";
        else warn("user_element_definition", "사용자 요소 " + key.first + " 의 *USER ELEMENT 카드는 매뉴얼에 정의가 없어 쓰지 않습니다. 덱에 직접 더하십시오");
      }
      os << "*ELEMENT, TYPE=" << key.first;
      if (key.second) {
        const Object& part = a_.model().get(key.second);
        os << ", ELSET=" << part.name;
        if (part_written_.insert(part.id).second) claim("elset", part.name, part);
      }
      os << "\n";
      for (std::size_t i : groups[key]) {
        os << m_.element_ids()[i];
        const Id* n = m_.nodes_at(i);
        const std::size_t count = m_.node_count_at(i);
        for (std::size_t k = 0; k < count; ++k) os << ((k + 1) % 16 == 0 ? ",\n" : ", ") << n[k];
        os << "\n";
      }
    }
  }

  // 같은 이름이 같은 종류의 셋으로 두 번 나가면 솔버가 합쳐 버린다.
  void claim(const std::string& space, const std::string& name, const Object& o) {
    std::string key = space + "|" + name;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (!names_.emplace(key, o.id).second)
      warn("name_conflict", "같은 이름의 셋이 두 번 나갑니다: " + name, o.id);
  }

  void write_user_sets() {
    std::ostream& os = sets_;
    for (const Object* s : a_.model().by_kind("set")) {
      if (s->suppressed) continue;
      const std::string t = subtype_of(*s);
      if (t == "geometry") {
        if (case_) skip(os, *s, "geometry_set");
        continue;
      }
      if (t == "node" || t == "node_surface") {
        std::vector<Id> ids;
        for (const Json& n : s->props.value("ids", Json::array()))
          if (in_scope_node(n.get<Id>())) ids.push_back(n.get<Id>());
        if (ids.empty()) continue;
        if (t == "node") {
          os << "*NSET, NSET=" << s->name << "\n";
          write_ids(os, ids);
          claim("nset", s->name, *s);
        } else {
          os << "*SURFACE, NAME=" << s->name << ", TYPE=NODE\n";
          for (Id n : ids) os << n << "\n";
          claim("surface", s->name, *s);
        }
        // 같은 내용의 내부 셋이 필요해지면 이 셋을 쓴다.
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        internal_.emplace((t == "node" ? "N" : "SN") + Json(ids).dump(), s->name);
      } else if (t == "element") {
        std::vector<Id> ids;
        for (const Json& e : s->props.value("ids", Json::array()))
          if (in_scope_elem(e.get<Id>())) ids.push_back(e.get<Id>());
        if (ids.empty()) continue;
        os << "*ELSET, ELSET=" << s->name << "\n";
        write_ids(os, ids);
        claim("elset", s->name, *s);
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        internal_.emplace("E" + Json(ids).dump(), s->name);
      } else {  // surface
        std::vector<std::pair<Id, int>> faces;
        for (const Json& f : s->props.value("faces", Json::array()))
          if (in_scope_elem(f[0].get<Id>())) faces.push_back({f[0].get<Id>(), f[1].get<int>()});
        if (faces.empty()) continue;
        os << "*SURFACE, NAME=" << s->name << ", TYPE=ELEMENT\n";
        for (const auto& [e, f] : faces) os << e << ", S" << face_label(e, f) << "\n";
        claim("surface", s->name, *s);
        std::sort(faces.begin(), faces.end());
        faces.erase(std::unique(faces.begin(), faces.end()), faces.end());
        Json key = Json::array();
        for (const auto& [e, f] : faces) key.push_back(Json::array({e, f}));
        internal_.emplace("SF" + key.dump(), s->name);
      }
      set_written_.insert(s->id);
    }
  }

  // 면 번호 → 솔버의 면 라벨 번호. 쉘·멤브레인은 1, 2 가 아랫면·윗면이라 변은 3 부터다.
  int face_label(Id element, int face) const {
    auto it = type_of_.find(element);
    return it != type_of_.end() && is_shell_type(it->second) ? face + 2 : face;
  }

  // ------------------------------------------------------------ 적용 대상 → 셋 이름
  const Object* single_set(const Json& target, std::initializer_list<const char*> subtypes) const {
    if (target.value("type", std::string()) != "set" || target["ids"].size() != 1) return nullptr;
    const Object* s = a_.model().find(target["ids"][0].get<Id>());
    if (!s || !set_written_.count(s->id)) return nullptr;
    for (const char* t : subtypes)
      if (subtype_of(*s) == t) return s;
    return nullptr;
  }
  std::string internal(const char* prefix, const std::string& key, const std::function<void(std::ostream&, const std::string&)>& write) {
    auto it = internal_.find(key);
    if (it != internal_.end()) return it->second;
    // 사용자 셋과 겹치지 않는 이름을 고른다(가져온 덱에는 OFEP_ 로 시작하는 셋이 있을 수 있다).
    const std::string space = std::string(prefix) == "OFEP_N" ? "NSET|" : std::string(prefix) == "OFEP_E" ? "ELSET|" : "SURFACE|";
    std::string name;
    do name = std::string(prefix) + std::to_string(++counter_[prefix]);
    while (names_.count(space + name));
    names_.emplace(space + name, 0);
    write(sets_, name);
    internal_[key] = name;
    return name;
  }
  std::string nset_of(const std::vector<Id>& ids) {
    if (ids.empty()) throw Error("invalid_state", "대상에 노드가 없습니다");
    return internal("OFEP_N", "N" + Json(ids).dump(), [&](std::ostream& os, const std::string& name) {
      os << "*NSET, NSET=" << name << "\n";
      write_ids(os, ids);
    });
  }
  std::string elset_of(const std::vector<Id>& ids) {
    if (ids.empty()) throw Error("invalid_state", "대상에 요소가 없습니다");
    return internal("OFEP_E", "E" + Json(ids).dump(), [&](std::ostream& os, const std::string& name) {
      os << "*ELSET, ELSET=" << name << "\n";
      write_ids(os, ids);
    });
  }
  std::vector<Id> nodes_of(const Json& target) {
    std::vector<Id> ids;
    for (const Json& n : resolve_target(a_, target, "nodes"))
      if (in_scope_node(n.get<Id>())) ids.push_back(n.get<Id>());
    return ids;
  }
  std::vector<Id> elems_of(const Json& target) {
    std::vector<Id> ids;
    for (const Json& e : resolve_target(a_, target, "elements"))
      if (in_scope_elem(e.get<Id>())) ids.push_back(e.get<Id>());
    return ids;
  }
  std::vector<std::pair<Id, int>> faces_of(const Json& target) {
    std::vector<std::pair<Id, int>> faces;
    for (const Json& f : resolve_target(a_, target, "faces"))
      if (in_scope_elem(f[0].get<Id>())) faces.push_back({f[0].get<Id>(), f[1].get<int>()});
    return faces;
  }
  std::string nset(const Json& target) {
    if (const Object* s = single_set(target, {"node"})) return s->name;
    return nset_of(nodes_of(target));
  }
  std::string elset(const Json& target) {
    if (const Object* s = single_set(target, {"element"})) return s->name;
    if (target.value("type", std::string()) == "parts" && target["ids"].size() == 1 && part_written_.count(target["ids"][0].get<Id>()))
      return a_.model().get(target["ids"][0].get<Id>()).name;
    return elset_of(elems_of(target));
  }
  // 면(요소면) 또는 노드로 된 면. nodes_ok 면 노드 대상은 노드 면으로 만든다.
  std::string surface(const Json& target, bool nodes_ok = false) {
    if (const Object* s = single_set(target, {"surface"})) return s->name;
    if (nodes_ok) {
      if (const Object* s = single_set(target, {"node_surface"})) return s->name;
      const std::string type = target.value("type", std::string());
      const Object* s = type == "set" && target["ids"].size() == 1 ? a_.model().find(target["ids"][0].get<Id>()) : nullptr;
      if (type == "nodes" || (s && (subtype_of(*s) == "node" || subtype_of(*s) == "node_surface"))) {
        const std::vector<Id> ids = nodes_of(target);
        if (ids.empty()) throw Error("invalid_state", "대상에 노드가 없습니다");
        return internal("OFEP_S", "SN" + Json(ids).dump(), [&](std::ostream& os, const std::string& name) {
          os << "*SURFACE, NAME=" << name << ", TYPE=NODE\n";
          for (Id n : ids) os << n << "\n";
        });
      }
    }
    const auto faces = faces_of(target);
    if (faces.empty()) throw Error("invalid_state", "대상에 면이 없습니다");
    Json key = Json::array();
    for (const auto& [e, f] : faces) key.push_back(Json::array({e, f}));
    return internal("OFEP_S", "SF" + key.dump(), [&](std::ostream& os, const std::string& name) {
      os << "*SURFACE, NAME=" << name << ", TYPE=ELEMENT\n";
      for (const auto& [e, f] : faces) os << e << ", S" << face_label(e, f) << "\n";
    });
  }
  // 요소면을 라벨 번호별 요소 셋으로 나눈다(분포 하중은 "요소 셋, 라벨" 로 쓴다).
  std::vector<std::pair<int, std::string>> face_groups(const Json& target) {
    std::map<int, std::vector<Id>> groups;
    for (const auto& [e, f] : faces_of(target)) groups[face_label(e, f)].push_back(e);
    if (groups.empty()) throw Error("invalid_state", "대상에 면이 없습니다");
    std::vector<std::pair<int, std::string>> out;
    for (auto& [label, ids] : groups) out.push_back({label, elset_of(ids)});
    return out;
  }
  // 대상이 요소(쉘의 면 전체)인지: 요소·파트·요소 셋, 또는 면 메시(쉘)의 형상 면처럼 전개하면 요소가 되는 형상 대상.
  bool is_element_target(const Json& target) const {
    const std::string type = target.value("type", std::string());
    if (type == "elements" || type == "parts") return true;
    if (type == "geometry") {
      try {
        resolve_target(a_, target, "faces");
        return false;  // 솔리드 메시의 면: 요소면 쌍으로 풀린다
      } catch (const Error&) {
      }
      try {
        return !resolve_target(a_, target, "elements").empty();
      } catch (const Error&) {
        return false;
      }
    }
    if (type != "set" || target["ids"].empty()) return false;
    const Object* s = a_.model().find(target["ids"][0].get<Id>());
    return s && subtype_of(*s) == "element";
  }

  std::string name_of(const Json& ref) const { return a_.model().get(ref.get<Id>()).name; }

  // ------------------------------------------------------------ 모델 정의
  void write_normals() {
    const Object* s = a_.find_settings();
    if (!s || !has(s->props, "normals") || s->props["normals"].empty()) return;
    std::ostringstream body;
    for (const Json& row : s->props["normals"])
      if (in_scope_elem(row[0].get<Id>()))
        body << row[0] << ", " << row[1] << ", " << num(row[2]) << ", " << num(row[3]) << ", " << num(row[4]) << "\n";
    if (!body.str().empty()) model_ << "*NORMAL\n" << body.str();
  }

  void write_orientations() {
    for (const Object* o : a_.model().by_kind("orientation")) {
      if (o->suppressed) continue;
      const std::string t = subtype_of(*o);
      if (t == "distribution") {
        // 분포는 *SOLID SECTION 에서만 쓸 수 있다(매뉴얼 7.42). 요소마다 한 줄, 같은 요소가 두 분포에 들어가면 안 된다.
        const std::string dist = o->name + "_D";
        std::ostringstream body;
        std::set<Id> seen;
        try {
          for (const Json& e : o->props.value("entries", Json::array())) {
            Json row = Json::array();
            for (const char* k : {"a", "b"})
              for (const Json& v : e[k]) row.push_back(v);
            for (Id id : elems_of(e["elements"])) {
              if (!seen.insert(id).second) throw Error("duplicate", "요소 " + std::to_string(id) + " 가 분포에 두 번 나옵니다");
              body << id;
              for (const Json& v : row) body << ", " << num(v);
              body << "\n";
            }
          }
        } catch (const Error& e) {
          skip(model_, *o, e.code());
          continue;
        }
        model_ << "*DISTRIBUTION, NAME=" << dist << "\n";
        for (const char* k : {"default_a", "default_b"})
          for (const Json& v : o->props[k]) model_ << ", " << num(v);
        model_ << "\n" << body.str();
        model_ << "*ORIENTATION, NAME=" << o->name << (o->props.value("system", std::string("rectangular")) == "cylindrical" ? ", SYSTEM=CYLINDRICAL" : "") << "\n"
               << dist << "\n";
        orientation_written_.insert(o->id);
        distribution_ids_.insert(o->id);
        continue;
      }
      model_ << "*ORIENTATION, NAME=" << o->name << (t == "cylindrical" ? ", SYSTEM=CYLINDRICAL" : "") << "\n";
      Json row = Json::array();
      for (const char* k : {"a", "b"})
        for (const Json& v : o->props[k]) row.push_back(v);
      write_row(model_, row);
      if (has(o->props, "rotation_axis"))
        model_ << o->props["rotation_axis"] << ", " << num(o->props.value("rotation_angle", 0.0)) << "\n";
      orientation_written_.insert(o->id);
    }
  }
  std::string orientation_param(const Object& o, const Object* material = nullptr, bool solid_section = true) {
    Id id = has(o.props, "orientation") ? o.props["orientation"].get<Id>() : 0;
    if (!id && material && has(material->props, "orientation")) id = material->props["orientation"].get<Id>();
    if (!id) return "";
    if (!orientation_written_.count(id)) {
      warn("missing_reference", "덱에 쓰지 못한 방향을 가리킵니다", o.id);
      return "";
    }
    if (!solid_section && distribution_ids_.count(id))
      warn("distribution_solid_only", "요소별 방향 분포는 *SOLID SECTION 에서만 쓸 수 있습니다(매뉴얼 7.42): " + a_.model().get(id).name, o.id);
    return ", ORIENTATION=" + a_.model().get(id).name;
  }

  void write_materials() {
    static const std::map<std::string, const char*> hyper = {
        {"arruda_boyce", "ARRUDA-BOYCE"}, {"mooney_rivlin", "MOONEY-RIVLIN"}, {"neo_hooke", "NEO HOOKE"}, {"ogden", "OGDEN"},
        {"polynomial", "POLYNOMIAL"}, {"reduced_polynomial", "REDUCED POLYNOMIAL"}, {"yeoh", "YEOH"}};
    static const std::map<std::string, const char*> elastic = {
        {"iso", "ISO"}, {"ortho", "ORTHO"}, {"engineering_constants", "ENGINEERING CONSTANTS"}, {"aniso", "ANISO"}};
    static const std::map<std::string, const char*> hardening = {
        {"kinematic", "KINEMATIC"}, {"combined", "COMBINED"}, {"user", "USER"}, {"johnson_cook", "JOHNSON COOK"}};
    std::ostream& os = model_;
    for (const Object* mat : a_.model().by_kind("material")) {
      if (mat->suppressed) continue;
      material_written_.insert(mat->id);
      os << "*MATERIAL, NAME=" << deck_material_name(*mat) << "\n";
      auto bit = mat->props.find("behaviors");
      if (bit == mat->props.end()) continue;
      auto upper = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return s;
      };
      for (const BehaviorSpec& spec : material_behaviors()) {
        auto it = bit->find(spec.name);
        if (it == bit->end()) continue;
        const Json& b = *it;
        const std::string& n = spec.name;
        const Json data = b.value("data", Json::array());
        if (n == "elastic") {
          const std::string t = b.value("type", std::string("iso"));
          os << "*ELASTIC" << (t == "iso" ? "" : std::string(", TYPE=") + elastic.at(t)) << "\n";
          write_rows(os, data);
        } else if (n == "density") {
          os << "*DENSITY\n", write_rows(os, data);
        } else if (n == "plastic") {
          const std::string h = b.value("hardening", std::string("isotropic"));
          os << "*PLASTIC" << (h == "isotropic" ? "" : std::string(", HARDENING=") + hardening.at(h)) << "\n";
          write_rows(os, data);
        } else if (n == "cyclic_hardening") {
          os << "*CYCLIC HARDENING\n", write_rows(os, data);
        } else if (n == "rate_dependent") {
          os << "*RATE DEPENDENT, TYPE=JOHNSON COOK\n" << num(b["c"]) << ", " << num(b["eps0"]) << "\n";
        } else if (n == "deformation_plasticity") {
          os << "*DEFORMATION PLASTICITY\n", write_rows(os, data);
        } else if (n == "creep") {
          os << "*CREEP" << (b.value("law", std::string("norton")) == "user" ? ", LAW=USER" : "") << "\n";
          write_rows(os, data);
        } else if (n == "hyperelastic") {
          const std::string model = b["model"].get<std::string>();
          os << "*HYPERELASTIC, " << hyper.at(model);
          if (has(b, "n") && (model == "ogden" || model == "polynomial" || model == "reduced_polynomial")) os << ", N=" << b["n"];
          os << "\n", write_rows(os, data);
        } else if (n == "hyperfoam") {
          os << "*HYPERFOAM" << (has(b, "n") ? ", N=" + b["n"].dump() : "") << "\n", write_rows(os, data);
        } else if (n == "mohr_coulomb") {
          os << "*MOHR COULOMB\n", write_rows(os, data);
        } else if (n == "mohr_coulomb_hardening") {
          os << "*MOHR COULOMB HARDENING\n", write_rows(os, data);
        } else if (n == "user") {
          os << "*USER MATERIAL, CONSTANTS=" << b["constants"] << (b.value("type", std::string()) == "thermal" ? ", TYPE=THERMAL" : "") << "\n";
          write_rows(os, data);
        } else if (n == "depvar") {
          os << "*DEPVAR\n" << b["count"] << "\n";
        } else if (n == "expansion" || n == "conductivity") {
          const std::string t = b.value("type", std::string("iso"));
          os << (n == "expansion" ? "*EXPANSION" : "*CONDUCTIVITY") << (t == "iso" ? "" : ", TYPE=" + upper(t));
          if (n == "expansion" && has(b, "zero")) os << ", ZERO=" << num(b["zero"]);
          os << "\n", write_rows(os, data);
        } else if (n == "specific_heat") {
          os << "*SPECIFIC HEAT\n", write_rows(os, data);
        } else if (n == "structural_damping") {
          os << "*DAMPING, STRUCTURAL=" << num(b["value"]) << "\n";
        } else if (n == "electrical_conductivity") {
          os << "*ELECTRICAL CONDUCTIVITY\n", write_rows(os, data);
        } else if (n == "magnetic_permeability") {
          os << "*MAGNETIC PERMEABILITY\n";
          for (const Json& row : data) {  // 둘째 값(영역 번호)은 정수
            os << num(row[0]) << ", " << integer(row[1]);
            for (std::size_t i = 2; i < row.size(); ++i) os << ", " << num(row[i]);
            os << "\n";
          }
        } else if (n == "fluid_constants") {
          os << "*FLUID CONSTANTS\n", write_rows(os, data);
        } else if (n == "specific_gas_constant") {
          os << "*SPECIFIC GAS CONSTANT\n" << num(b["value"]) << "\n";
        } else if (n == "special") {
          write_special(os, *mat, b, bit->contains("depvar"));
        } else if (n == "allowable") {
          // 결과 평가용. 솔버로는 나가지 않는다.
        } else {
          skipped_.push_back(Json{{"object", mat->id}, {"kind", "material"}, {"name", mat->name}, {"type", n}, {"reason", "material_behavior"}});
          os << "** skipped: material behavior " << n << "\n";
        }
      }
    }
  }
  // 재료 이름. 덱에 없는 재료면 경고한다.
  std::string material_name(const Object& user, const Json& ref) {
    const Object& mat = a_.model().get(ref.get<Id>());
    if (!material_written_.count(mat.id)) warn("missing_reference", "억제된 재료를 가리킵니다: " + mat.name, user.id);
    return deck_material_name(mat);
  }

  // 내장 특수 재료(매뉴얼 6.8.3·6.8.4·6.8.12~6.8.15): 이름의 접두어로 고르는 사용자 재료.
  // {접두어, CONSTANTS(0 = 행 길이로 정함), DEPVAR(0 = 없음)}
  struct SpecialSpec { const char* prefix; int constants; int depvar; };
  static const SpecialSpec* special_spec(const std::string& model) {
    static const std::map<std::string, SpecialSpec> table = {
        {"tension_only", {"TENSION_ONLY", 2, 0}},
        {"compression_only", {"COMPRESSION_ONLY", 2, 0}},
        {"fiber", {"ELASTIC_FIBER", 0, 0}},
        {"single_crystal", {"SINGLE_CRYSTAL", 21, 60}},
        {"single_crystal_creep", {"SINGLE_CRYSTAL_CREEP", 7, 24}},
        {"ideal_gas", {"IDEAL_GAS", 1, 0}},
        {"ciarlet", {"CIARLET_EL", 2, 0}},
        {"small_strain_rotation_insensitive", {"UNDO_NLGEOM_LIN_ISO_EL", 2, 0}}};
    auto it = table.find(model);
    return it == table.end() ? nullptr : &it->second;
  }
  static std::string special_model(const Object& mat) {
    auto bit = mat.props.find("behaviors");
    if (bit == mat.props.end() || !bit->contains("special")) return "";
    return (*bit)["special"].value("model", std::string());
  }
  // 덱에 쓰는 재료 이름. 특수 재료는 매뉴얼이 요구하는 접두어가 앞에 오게 한다(이미 붙어 있으면 그대로).
  std::string deck_material_name(const Object& mat) const {
    const SpecialSpec* sp = special_spec(special_model(mat));
    if (!sp) return mat.name;
    std::string up = mat.name;
    std::transform(up.begin(), up.end(), up.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (up.rfind(sp->prefix, 0) == 0) return mat.name;
    return std::string(sp->prefix) + "_" + mat.name;
  }
  void write_special(std::ostream& os, const Object& mat, const Json& b, bool has_depvar) {
    const std::string model = b.value("model", std::string());
    const Json rows = b.value("constants", Json::array());
    if (model == "aniso_plasticity" || model == "aniso_creep") {
      // 매뉴얼 6.8.16·6.8.17: *ELASTIC, TYPE=ORTHO 뒤에 *PLASTIC / *CREEP 이 오면 켜진다. 상수는 직교 이방성 탄성 9개(+온도).
      os << "*ELASTIC, TYPE=ORTHO\n";
      write_rows(os, rows);
      return;
    }
    const SpecialSpec* sp = special_spec(model);
    if (!sp) {
      skipped_.push_back(Json{{"object", mat.id}, {"kind", "material"}, {"name", mat.name}, {"type", "special:" + model}, {"reason", "material_behavior"}});
      os << "** skipped: special material " << model << "\n";
      return;
    }
    int constants = sp->constants;
    if (constants == 0 && !rows.empty()) {  // ELASTIC_FIBER: 2 + 4n (온도 열은 뺀다)
      const int len = static_cast<int>(rows[0].size());
      constants = (len - 2) % 4 == 0 ? len : len - 1;
    }
    os << "*USER MATERIAL, CONSTANTS=" << constants << "\n";
    write_rows(os, rows);
    if (sp->depvar && !has_depvar) os << "*DEPVAR\n" << sp->depvar << "\n";
  }

  void write_sections() {
    std::ostream& os = model_;
    const Object* settings = a_.find_settings();
    std::map<Id, Json> beam_dir;
    if (settings)
      for (const Json& row : settings->props.value("beam_directions", Json::array()))
        beam_dir[row[0].get<Id>()] = Json::array({row[1], row[2], row[3]});
    std::set<Id> covered;
    for (const Object* p : a_.model().by_kind("property")) {
      if (p->suppressed) continue;
      const Json& q = p->props;
      const std::string t = subtype_of(*p);
      if (t == "substructure") {
        // 이름은 4자 이내(앞에 U 가 붙어 사용자 요소 종류가 된다). 행렬 파일은 작업 폴더 기준 상대 경로.
        if (p->name.size() > 4) warn("name_too_long", "부분구조 이름은 4자 이내여야 합니다: " + p->name, p->id);
        os << "*MATRIX ASSEMBLE, NAME=" << p->name << ", STIFFNESS FILE=" << q["stiffness_file"].get<std::string>();
        if (has(q, "mass_file")) os << ", MASS FILE=" << q["mass_file"].get<std::string>();
        os << "\n";
        continue;
      }
      if (!has(q, "target")) {
        warn("unassigned_property", "요소에 할당되지 않은 프로퍼티는 덱에 쓰지 않습니다: " + p->name, p->id);
        continue;
      }
      try {
        const std::vector<Id> ids = elems_of(q["target"]);
        if (ids.empty()) continue;  // 이 케이스의 범위 밖
        const std::string set = elset(q["target"]);
        const Object* mat = has(q, "material") ? &a_.model().get(q["material"].get<Id>()) : nullptr;
        const std::string material = mat ? ", MATERIAL=" + material_name(*p, q["material"]) : "";
        if (t == "solid" || t == "truss") {
          os << "*SOLID SECTION, ELSET=" << set << material << orientation_param(*p, mat) << "\n";
          if (t == "truss") os << num(q["area"]) << "\n";
          else if (has(q, "thickness")) os << num(q["thickness"]) << "\n";
        } else if (t == "shell" || t == "membrane") {
          os << (t == "shell" ? "*SHELL SECTION" : "*MEMBRANE SECTION") << ", ELSET=" << set << material << orientation_param(*p, mat, false);
          if (has(q, "offset")) os << ", OFFSET=" << num(q["offset"]);
          if (q.value("nodal_thickness", false)) os << ", NODAL THICKNESS";
          os << "\n" << num(q["thickness"]) << "\n";
        } else if (t == "composite") {
          os << "*SHELL SECTION, COMPOSITE, ELSET=" << set << orientation_param(*p, nullptr, false);
          if (has(q, "offset")) os << ", OFFSET=" << num(q["offset"]);
          os << "\n";
          for (const Json& layer : q["layers"]) {
            os << num(layer["thickness"]) << ", , " << material_name(*p, layer["material"]);
            if (has(layer, "orientation")) os << ", " << name_of(layer["orientation"]);
            os << "\n";
          }
        } else if (t == "beam") {
          // 요소별로 지정한 단면 방향이 있으면 방향별로 섹션을 나눈다.
          std::map<std::string, std::pair<Json, std::vector<Id>>> groups;
          for (Id e : ids) {
            auto it = beam_dir.find(e);
            const Json d = it != beam_dir.end() ? it->second : q.value("direction", Json());
            auto& g = groups[d.dump()];
            g.first = d, g.second.push_back(e);
          }
          static const std::map<std::string, const char*> section = {
              {"rect", "RECT"}, {"circ", "CIRC"}, {"pipe", "PIPE"}, {"box", "BOX"}, {"general", "GENERAL"}};
          for (const auto& [key, g] : groups) {
            os << "*BEAM SECTION, ELSET=" << (groups.size() == 1 ? set : elset_of(g.second)) << material
               << ", SECTION=" << section.at(q["section"].get<std::string>()) << orientation_param(*p, mat, false);
            if (has(q, "offset1")) os << ", OFFSET1=" << num(q["offset1"]);
            if (has(q, "offset2")) os << ", OFFSET2=" << num(q["offset2"]);
            os << "\n";
            write_row(os, q["dimensions"]);
            if (!g.first.is_null()) write_row(os, g.first);
          }
        } else if (t == "spring" || t == "dashpot") {
          const bool table = has(q, "table");
          os << (t == "spring" ? "*SPRING" : "*DASHPOT") << ", ELSET=" << set;
          if (t == "spring" && table) os << ", NONLINEAR";
          if (t == "spring") os << orientation_param(*p, nullptr, false);
          os << "\n";
          if (has(q, "dof1")) os << q["dof1"] << (has(q, "dof2") ? ", " + q["dof2"].dump() : "");
          os << "\n";  // 자유도 줄(SPRINGA·DASHPOTA 는 빈 줄)
          const char* single = t == "spring" ? "stiffness" : "coefficient";
          if (table) write_rows(os, q["table"]);
          else if (has(q, single)) os << num(q[single]) << "\n";
          else warn("incomplete", "강성(감쇠) 값이 없습니다: " + p->name, p->id);
        } else if (t == "gap") {
          os << "*GAP, ELSET=" << set << "\n" << num(q["clearance"]);
          for (const Json& v : q["direction"]) os << ", " << num(v);
          if (has(q, "stiffness") || has(q, "tension_force")) os << ", , " << opt(q, "stiffness");
          if (has(q, "tension_force")) os << ", " << num(q["tension_force"]);
          os << "\n";
        } else if (t == "mass") {
          os << "*MASS, ELSET=" << set << "\n" << num(q["mass"]) << "\n";
        } else if (t == "fluid") {
          os << "*FLUID SECTION, ELSET=" << set << ", TYPE=" << q["section_type"].get<std::string>() << material;
          if (has(q, "oil")) os << ", OIL=" << q["oil"].get<std::string>();
          os << "\n";
          if (has(q, "constants")) write_row(os, q["constants"]);
        } else if (t == "user") {
          os << "*USER SECTION, ELSET=" << set << material << ", CONSTANTS=" << q["constants"].size() << "\n";
          write_row(os, q["constants"]);
        } else {
          skip(os, *p, "property_type");
          continue;
        }
        if (has(q, "nodal_thickness_values") && q.value("nodal_thickness", false)) {
          os << "*NODAL THICKNESS\n";
          for (const Json& row : q["nodal_thickness_values"])
            if (in_scope_node(static_cast<Id>(row[0].get<double>()))) os << integer(row[0]) << ", " << num(row[1]) << "\n";
        }
        covered.insert(ids.begin(), ids.end());
      } catch (const Error& e) {
        skip(os, *p, e.code());
      }
    }
    std::size_t total = 0, missing = 0;
    for (Id e : m_.element_ids()) {
      if (!in_scope_elem(e)) continue;
      ++total;
      if (!covered.count(e)) ++missing;
    }
    if (missing)
      warn("unassigned_elements", "프로퍼티가 없는 요소가 " + std::to_string(missing) + "개 있습니다(전체 " + std::to_string(total) + "개)");
  }

  void write_functions() {
    for (const Object* f : a_.model().by_kind("function")) {
      const std::string t = subtype_of(*f);
      if (f->suppressed || t == "expression" || !has(f->props, "points")) continue;
      model_ << "*AMPLITUDE, NAME=" << f->name << (f->props.value("time", std::string("step")) == "total" ? ", TIME=TOTAL TIME" : "") << "\n";
      const Json& pts = f->props["points"];
      for (std::size_t i = 0; i < pts.size(); ++i)
        model_ << num(pts[i][0]) << ", " << num(pts[i][1]) << ((i + 1) % 4 == 0 || i + 1 == pts.size() ? "\n" : ", ");
      amplitude_written_.insert(f->id);
    }
    for (const Object* tp : a_.model().by_kind("time_points")) {
      model_ << "*TIME POINTS, NAME=" << tp->name << (tp->props.value("time", std::string("step")) == "total" ? ", TIME=TOTAL TIME" : "") << "\n";
      write_row(model_, tp->props["times"]);
    }
  }
  // 하중·경계조건 카드에 붙는 시간 함수 매개변수. 쓸 수 없는 함수면 Error.
  std::string amplitude_param(const Object& o) {
    std::string s;
    if (has(o.props, "amplitude")) {
      const Id id = o.props["amplitude"].get<Id>();
      if (!amplitude_written_.count(id)) throw Error("amplitude_function", "수식 함수는 시간 함수로 쓸 수 없습니다");
      s += ", AMPLITUDE=" + a_.model().get(id).name;
    }
    if (has(o.props, "time_delay")) s += ", TIME DELAY=" + num(o.props["time_delay"]);
    return s;
  }

  void write_constraints() {
    std::ostream& os = model_;
    for (const Object* c : a_.model().by_kind("constraint")) {
      if (c->suppressed) continue;
      const Json& q = c->props;
      const std::string t = subtype_of(*c);
      const std::string name = "C" + std::to_string(c->id);
      try {
        std::ostringstream card;  // 실패하면 반쯤 쓴 카드가 남지 않게 따로 모은다
        if (t == "equation") {
          const Json& terms = q["terms"];
          card << "*EQUATION\n" << terms.size() << "\n";
          for (std::size_t i = 0; i < terms.size(); ++i)
            card << integer(terms[i][0]) << ", " << integer(terms[i][1]) << ", " << num(terms[i][2])
                 << ((i + 1) % 4 == 0 || i + 1 == terms.size() ? "\n" : ", ");
        } else if (t.rfind("mpc_", 0) == 0) {
          static const std::map<std::string, const char*> mpc = {
              {"mpc_plane", "PLANE"}, {"mpc_straight", "STRAIGHT"}, {"mpc_beam", "BEAM"}, {"mpc_meanrot", "MEANROT"}, {"mpc_dist", "DIST"}};
          std::vector<std::string> cells = {mpc.at(t)};
          for (const Json& n : q["nodes"]) cells.push_back(n.dump());
          if (has(q, "pilot_node")) cells.push_back(q["pilot_node"].dump());
          card << "*MPC\n";
          for (std::size_t i = 0; i < cells.size(); ++i)
            card << cells[i] << (i + 1 == cells.size() ? "\n" : (i + 1) % 16 == 0 ? ",\n" : ", ");
        } else if (t == "rigid_body") {
          const bool elements = is_element_target(q["target"]);
          card << "*RIGID BODY, " << (elements ? "ELSET=" + elset(q["target"]) : "NSET=" + nset(q["target"]));
          if (has(q, "ref_node")) card << ", REF NODE=" << q["ref_node"];
          if (has(q, "rot_node")) card << ", ROT NODE=" << q["rot_node"];
          card << "\n";
        } else if (t == "coupling_kinematic" || t == "coupling_distributing") {
          card << "*COUPLING, REF NODE=" << q["ref_node"] << ", SURFACE=" << surface(q["surface"]) << ", CONSTRAINT NAME=" << name
               << orientation_param(*c) << "\n" << (t == "coupling_kinematic" ? "*KINEMATIC\n" : "*DISTRIBUTING\n");
          for (const Json& d : q["dofs"]) card << d << ", " << d << "\n";
        } else if (t == "tie" || t == "multistage" || t == "cyclic_symmetry") {
          card << "*TIE, NAME=" << name;
          if (t == "multistage") card << ", MULTISTAGE";
          if (t == "cyclic_symmetry") card << ", CYCLIC SYMMETRY";
          if (has(q, "position_tolerance")) card << ", POSITION TOLERANCE=" << num(q["position_tolerance"]);
          if (t == "tie" && has(q, "adjust") && !q["adjust"].get<bool>()) card << ", ADJUST=NO";
          const bool node_sets = t == "multistage";
          card << "\n" << (node_sets ? nset(q["slave"]) : surface(q["slave"], true)) << ", "
               << (node_sets ? nset(q["master"]) : surface(q["master"], t == "cyclic_symmetry")) << "\n";
          if (t == "cyclic_symmetry") {
            card << "*CYCLIC SYMMETRY MODEL, N=" << q["sectors"] << ", TIE=" << name;
            if (has(q, "ngraph")) card << ", NGRAPH=" << q["ngraph"];
            if (has(q, "elements")) card << ", ELSET=" << elset(q["elements"]);
            card << "\n";
            Json row = Json::array();
            for (const char* k : {"axis_point_a", "axis_point_b"})
              for (const Json& v : q[k]) row.push_back(v);
            write_row(card, row);
          }
        } else {
          skip(os, *c, "constraint_type");
          continue;
        }
        os << card.str();
      } catch (const Error& e) {
        skip(os, *c, e.code());
      }
    }
  }

  void write_surface_behavior(std::ostream& os, const Json& q) {
    static const std::map<std::string, const char*> po = {
        {"hard", "HARD"}, {"linear", "LINEAR"}, {"exponential", "EXPONENTIAL"}, {"tabular", "TABULAR"}, {"tied", "TIED"}};
    if (!has(q, "pressure_overclosure")) return;
    const std::string t = q["pressure_overclosure"].get<std::string>();
    os << "*SURFACE BEHAVIOR, PRESSURE-OVERCLOSURE=" << po.at(t) << "\n";
    if (t == "linear") write_optional(os, {opt(q, "slope"), opt(q, "sigma_inf"), opt(q, "c0")});
    else if (t == "exponential") write_optional(os, {opt(q, "c0"), opt(q, "p0")});
    else if (t == "tied") write_optional(os, {opt(q, "slope")});
    else if (t == "tabular" && has(q, "table")) write_rows(os, q["table"]);
  }
  void write_friction(std::ostream& os, const Json& q) {
    if (!has(q, "friction_coefficient")) return;
    os << "*FRICTION\n";
    write_optional(os, {num(q["friction_coefficient"]), opt(q, "stick_slope")});
  }

  void write_contact() {
    static const std::map<std::string, const char*> method = {
        {"node_to_surface", "NODE TO SURFACE"}, {"surface_to_surface", "SURFACE TO SURFACE"}, {"mortar", "MORTAR"}, {"massless", "MASSLESS"}};
    std::ostream& os = model_;
    for (const Object* ci : a_.model().by_kind("contact_property")) {
      if (ci->suppressed) continue;
      const Json& q = ci->props;
      os << "*SURFACE INTERACTION, NAME=" << ci->name << "\n";
      write_surface_behavior(os, q);
      write_friction(os, q);
      if (has(q, "damping")) {
        os << "*CONTACT DAMPING" << (has(q, "damping_tangent_fraction") ? ", TANGENT FRACTION=" + num(q["damping_tangent_fraction"]) : "")
           << "\n" << num(q["damping"]) << "\n";
      }
      if (has(q, "conductance")) os << "*GAP CONDUCTANCE\n", write_rows(os, q["conductance"]);
      if (has(q, "heat_conversion")) {
        os << "*GAP HEAT GENERATION\n";
        write_optional(os, {num(q["heat_conversion"]), opt(q, "heat_slave_fraction"), opt(q, "sliding_velocity")});
      }
      interaction_written_.insert(ci->id);
    }
    for (const Object* cp : a_.model().by_kind("contact_pair")) {
      if (cp->suppressed) continue;
      const Json& q = cp->props;
      try {
        if (!interaction_written_.count(q["interaction"].get<Id>())) throw Error("missing_reference", "접촉 속성이 억제되어 있습니다");
        const std::string slave = surface(q["slave"], true), master = surface(q["master"]);
        os << "*CONTACT PAIR, INTERACTION=" << name_of(q["interaction"]);
        // TYPE 은 필수 매개변수다(매뉴얼 7.25). 케이스에 접촉 방식이 없으면 SURFACE TO SURFACE 로 쓰고 경고한다
        if (has(case_->props, "contact_method")) {
          os << ", TYPE=" << method.at(case_->props["contact_method"].get<std::string>());
        } else {
          os << ", TYPE=SURFACE TO SURFACE";
          if (!contact_method_warned_) warn("contact_method_default", "케이스에 접촉 방식(contact_method)이 없어 SURFACE TO SURFACE 로 씁니다", case_->id);
          contact_method_warned_ = true;
        }
        if (q.value("small_sliding", false)) os << ", SMALL SLIDING";
        if (has(q, "adjust")) os << ", ADJUST=" << num(q["adjust"]);
        os << "\n" << slave << ", " << master << "\n";
        if (has(q, "clearance")) os << "*CLEARANCE, SLAVE=" << slave << ", MASTER=" << master << ", VALUE=" << num(q["clearance"]) << "\n";
        pair_surfaces_[cp->id] = {slave, master};
      } catch (const Error& e) {
        skip(os, *cp, e.code());
      }
    }
  }

  // *TRANSFORM: 국부 좌표계(csys)를 쓰는 경계조건·집중 하중의 노드에 좌표 변환을 건다. 솔버는 노드마다 좌표계를
  // 하나만 두고, 그 노드에 적힌 *BOUNDARY·*CLOAD 의 자유도를 모두 국부 축으로 읽는다. 그래서
  //   - 한 노드가 두 좌표계를 받으면 둘 다 쓰지 못하고(transform_conflict)
  //   - 변환된 노드에 전역 좌표계로 적힌 경계조건·집중 하중도 쓰지 못한다(transform_conflict).
  void write_transforms() {
    std::map<Id, std::vector<Id>> nodes_by_csys;     // 좌표계 → 노드(순서대로)
    std::map<Id, Id> csys_of_node;
    std::map<Id, std::vector<Id>> objects_by_node;   // 노드 → 좌표계를 쓰는 객체
    std::vector<const Object*> local, global;        // 노드 자유도에 걸리는 객체(좌표계 있음 / 없음)
    transform_entries_.clear();                      // 셋에서 온 사본은 여기 담아 포인터를 유지한다
    for (const Object* step : a_.model().children(case_->id, "step")) {
      if (step->suppressed) continue;
      for (const Object& o : step_entries(a_, *step, "bc")) transform_entries_.push_back(o);
      for (const Object& o : step_entries(a_, *step, "load")) transform_entries_.push_back(o);
    }
    for (const Object& o : transform_entries_) {
      const std::string t = subtype_of(o);
      if (o.kind == "bc") {
        if (t == "temperature" || t == "fluid" || t == "electromagnetic" || t == "network") continue;  // 스칼라 자유도(8·11 등)는 좌표 변환과 무관하다
        if (has(o.props, "target")) (has(o.props, "csys") ? local : global).push_back(&o);
      } else if ((t == "force" || t == "moment") && has(o.props, "target")) {
        (has(o.props, "csys") ? local : global).push_back(&o);
      }
    }
    if (local.empty()) return;
    // 1) 좌표계 자체를 쓸 수 없는 객체, 2) 한 노드에 두 좌표계를 거는 객체를 뺀다.
    std::map<Id, std::vector<Id>> nodes_of_object;
    for (const Object* o : local) {
      const Object* c = a_.model().find(o->props["csys"].get<Id>());
      if (!c) transform_skip_[o->id] = "missing_reference";
      else if (subtype_of(*c) == "spherical") transform_skip_[o->id] = "transform_type";  // 솔버의 *TRANSFORM 은 직교(R)·원통(C)뿐이다
      else if (!has(c->props, "origin") || !has(c->props, "axis1_point") || !has(c->props, "plane12_point")) transform_skip_[o->id] = "incomplete_csys";
      else {
        try {
          nodes_of_object[o->id] = nodes_of(o->props["target"]);
        } catch (const Error& e) {
          transform_skip_[o->id] = e.code();
        }
      }
    }
    std::set<Id> conflict;
    for (const Object* o : local) {
      if (transform_skip_.count(o->id)) continue;
      const Id cs = o->props["csys"].get<Id>();
      for (Id n : nodes_of_object[o->id]) {
        auto it = csys_of_node.find(n);
        if (it == csys_of_node.end()) csys_of_node[n] = cs;
        else if (it->second != cs) conflict.insert(n);
        objects_by_node[n].push_back(o->id);
      }
    }
    for (Id n : conflict) {
      for (Id id : objects_by_node[n]) transform_skip_[id] = "transform_conflict";
      warn("transform_conflict", "노드 " + std::to_string(n) + " 에 서로 다른 좌표계가 걸려 있습니다");
    }
    // 남은 객체의 노드만 변환한다(뺀 객체 때문에 변환이 남아 다른 카드의 뜻이 바뀌지 않게).
    csys_of_node.clear();
    for (const Object* o : local) {
      if (transform_skip_.count(o->id)) continue;
      const Id cs = o->props["csys"].get<Id>();
      for (Id n : nodes_of_object[o->id])
        if (csys_of_node.emplace(n, cs).second) nodes_by_csys[cs].push_back(n);
    }
    for (const Object* o : global) {
      try {
        for (Id n : nodes_of(o->props["target"]))
          if (csys_of_node.count(n)) {
            transform_skip_[o->id] = "transform_conflict";
            warn("transform_conflict", "좌표 변환이 걸린 노드 " + std::to_string(n) + " 에 전역 좌표계의 경계조건(하중)이 있어 쓰지 못합니다", o->id);
            break;
          }
      } catch (const Error&) {
      }
    }
    std::ostream& os = model_;
    for (auto& [cs, ids] : nodes_by_csys) {
      const Object* c = a_.model().find(cs);
      const std::string t = subtype_of(*c);
      const Json& q = c->props;
      auto vec = [&](const char* key) {
        const Json& v = q[key];
        return std::array<double, 3>{v[0].get<double>(), v[1].get<double>(), v[2].get<double>()};
      };
      const auto o = vec("origin"), p1 = vec("axis1_point"), p2 = vec("plane12_point");
      std::array<double, 3> a, b;
      if (t == "rectangular") {  // a: 국부 1축 방향, b: 1-2 평면 위(원점에서 본 벡터)
        for (int k = 0; k < 3; ++k) a[k] = p1[k] - o[k], b[k] = p2[k] - o[k];
      } else {  // 원통: a, b 는 축 위의 두 점. 축 = (1축 점 - 원점) × (평면 점 - 원점)
        std::array<double, 3> u, v;
        for (int k = 0; k < 3; ++k) u[k] = p1[k] - o[k], v[k] = p2[k] - o[k];
        a = o;
        b = {o[0] + (u[1] * v[2] - u[2] * v[1]), o[1] + (u[2] * v[0] - u[0] * v[2]), o[2] + (u[0] * v[1] - u[1] * v[0])};
      }
      os << "*TRANSFORM, NSET=" << nset_of(ids) << ", TYPE=" << (t == "rectangular" ? "R" : "C") << "\n"
         << num(a[0]) << ", " << num(a[1]) << ", " << num(a[2]) << ", " << num(b[0]) << ", " << num(b[1]) << ", " << num(b[2]) << "\n";
    }
  }

  void write_initial_conditions() {
    std::ostream& os = model_;
    for (const Object* ic : a_.model().by_kind("initial_condition")) {
      if (ic->suppressed) continue;
      const Json& q = ic->props;
      const std::string t = subtype_of(*ic);
      try {
        std::ostringstream card;
        if (t == "temperature") {
          card << "*INITIAL CONDITIONS, TYPE=TEMPERATURE\n" << nset(q["target"]) << ", " << num(q["value"]);
          if (has(q, "gradient")) card << ", " << num(q["gradient"]);
          card << "\n";
        } else if (t == "velocity" || t == "displacement" || t == "fluid_velocity") {
          card << "*INITIAL CONDITIONS, TYPE=" << (t == "velocity" ? "VELOCITY" : t == "displacement" ? "DISPLACEMENT" : "FLUID VELOCITY") << "\n";
          const std::string set = nset(q["target"]);
          for (int d = 0; d < 3; ++d) card << set << ", " << d + 1 << ", " << num(q["components"][static_cast<std::size_t>(d)]) << "\n";
        } else if (t == "mass_flow" || t == "total_pressure" || t == "pressure") {
          card << "*INITIAL CONDITIONS, TYPE=" << (t == "mass_flow" ? "MASS FLOW" : t == "total_pressure" ? "TOTAL PRESSURE" : "PRESSURE") << "\n"
               << nset(q["target"]) << ", " << num(q["value"]) << "\n";
        } else {
          skip(os, *ic, "initial_condition_type");
          continue;
        }
        os << card.str();
      } catch (const Error& e) {
        skip(os, *ic, e.code());
      }
    }
  }

  void write_model_settings() {
    std::ostream& os = model_;
    if (const Object* s = a_.find_settings(); s && has(s->props, "physical_constants")) {
      const Json& pc = s->props["physical_constants"];
      std::string params;
      if (has(pc, "absolute_zero")) params += ", ABSOLUTE ZERO=" + num(pc["absolute_zero"]);
      if (has(pc, "stefan_boltzmann")) params += ", STEFAN BOLTZMANN=" + num(pc["stefan_boltzmann"]);
      if (has(pc, "newton_gravity")) params += ", NEWTON GRAVITY=" + num(pc["newton_gravity"]);
      if (!params.empty()) os << "*PHYSICAL CONSTANTS" << params << "\n";
    }
    const Json& q = case_->props;
    if (has(q, "rayleigh_alpha") || has(q, "rayleigh_beta"))
      os << "*DAMPING, ALPHA=" << num(q.value("rayleigh_alpha", 0.0)) << ", BETA=" << num(q.value("rayleigh_beta", 0.0)) << "\n";
    if (has(q, "values_at_infinity")) {  // 3D 유체(매뉴얼 7.139): 정온도, 속도 크기, 정압, 밀도, 계산 영역 길이
      if (q["values_at_infinity"].size() != 5)
        throw Error("invalid_param", "values_at_infinity 는 값 5개여야 합니다", {{"param", "values_at_infinity"}, {"object", case_->id}});
      os << "*VALUES AT INFINITY\n";
      write_row(os, q["values_at_infinity"]);
    }
    if (has(q, "design_variable_type")) {  // 민감도 해석의 설계 변수(*DESIGN VARIABLES, TYPE=COORDINATE|ORIENTATION). 좌표면 노드 셋 줄
      const std::string t = q["design_variable_type"].get<std::string>();
      os << "*DESIGN VARIABLES, TYPE=" << (t == "orientation" ? "ORIENTATION" : "COORDINATE") << "\n";
      if (t == "coordinate") {
        if (!has(q, "design_nodes"))
          throw Error("missing_param", "좌표 설계 변수에는 design_nodes(표면 노드)가 필요합니다", {{"param", "design_nodes"}, {"object", case_->id}});
        os << nset(q["design_nodes"]) << "\n";
      }
    }
    write_blocks(os, case_->id, "model", 0);
  }

  // 보존한 덱 내용을 그대로 쓴다(CAS-12). 케이스 아래의 블록은 자리(place, after)가 맞는 것만.
  void write_blocks(std::ostream& os, Id parent, const std::string& place = "", Id after = 0) {
    for (const Object* b : a_.model().children(parent, "deck_block")) {
      if (b->suppressed) continue;
      if (!place.empty()) {
        if (b->props.value("place", std::string("model")) != place) continue;
        if (place == "steps" && (has(b->props, "after") ? b->props["after"].get<Id>() : 0) != after) continue;
      }
      const std::string text = b->props.value("text", std::string());
      if (text.empty()) continue;
      if (!std::all_of(text.begin(), text.end(), [](unsigned char c) { return c < 128; })) {
        skip(os, *b, "non_ascii");
        continue;
      }
      os << text << (text.back() == '\n' ? "" : "\n");
    }
  }

  // ------------------------------------------------------------ 스텝
  static std::string solver_param(const Json& q) {
    static const std::map<std::string, const char*> names = {
        {"pastix", "PASTIX"}, {"pardiso", "PARDISO"}, {"spooles", "SPOOLES"}, {"taucs", "TAUCS"},
        {"iterative_scaling", "ITERATIVE SCALING"}, {"iterative_cholesky", "ITERATIVE CHOLESKY"}};
    auto it = names.find(q.value("solver", std::string("default")));
    return it == names.end() ? "" : std::string(", SOLVER=") + it->second;
  }
  static std::string time_params(const Json& q) {
    std::string s;
    if (q.value("direct", false)) s += ", DIRECT";
    if (q.value("time_reset", false)) s += ", TIME RESET";
    if (has(q, "total_time_at_start")) s += ", TOTAL TIME AT START=" + num(q["total_time_at_start"]);
    return s;
  }
  static void time_line(std::ostream& os, const Json& q) {
    write_optional(os, {opt(q, "initial_increment"), opt(q, "period"), opt(q, "min_increment"), opt(q, "max_increment")});
  }
  static void modal_damping(std::ostream& os, const Json& q) {
    if (!has(q, "modal_damping")) return;
    const bool rayleigh = q.value("damping_type", std::string("direct")) == "rayleigh";
    os << "*MODAL DAMPING" << (rayleigh ? ", RAYLEIGH" : "") << "\n";
    for (const Json& row : q["modal_damping"]) {
      if (rayleigh) os << ", , " << num(row[0]) << ", " << num(row[1]) << "\n";
      else os << integer(row[0]) << ", " << integer(row[1]) << ", " << num(row[2]) << "\n";
    }
  }

  // 절차 키워드. 쓸 수 없는 절차면 false.
  bool write_procedure(std::ostream& os, const Object& step) {
    const Json& q = step.props;
    const std::string t = subtype_of(step);
    const std::string solver = solver_param(q);
    auto flag = [&](const char* key, const char* text) { return q.value(key, false) ? std::string(", ") + text : std::string(); };
    auto val = [&](const char* key, const char* text) { return has(q, key) ? std::string(", ") + text + "=" + num(q[key]) : std::string(); };
    if (t == "static") {
      os << "*STATIC" << solver << time_params(q) << "\n", time_line(os, q);
    } else if (t == "frequency") {
      os << "*FREQUENCY" << solver << (q.value("storage", false) ? ", STORAGE=YES" : "");
      if (has(q, "global") && !q["global"].get<bool>()) os << ", GLOBAL=NO";
      if (q.value("cycmpc", std::string("active")) == "inactive") os << ", CYCMPC=INACTIVE";
      os << "\n";
      write_optional(os, {q["num_modes"].dump(), opt(q, "freq_min"), opt(q, "freq_max")});
      if (has(q, "cyclic_mode_min") || has(q, "cyclic_mode_max")) {  // 순환대칭 고유치(매뉴얼 7.115)
        os << "*SELECT CYCLIC SYMMETRY MODES";
        if (has(q, "cyclic_mode_min")) os << ", NMIN=" << q["cyclic_mode_min"];
        if (has(q, "cyclic_mode_max")) os << ", NMAX=" << q["cyclic_mode_max"];
        os << "\n";
      }
    } else if (t == "complex_frequency") {
      os << "*COMPLEX FREQUENCY" << flag("coriolis", "CORIOLIS") << "\n" << q["num_modes"] << "\n";
    } else if (t == "buckle") {
      os << "*BUCKLE" << solver << "\n";
      write_optional(os, {q["num_modes"].dump(), opt(q, "accuracy")});
    } else if (t == "green") {
      os << "*GREEN" << solver << (q.value("storage", false) ? ", STORAGE=YES" : "") << "\n" << q["num_modes"] << "\n";
    } else if (t == "modal_dynamic") {
      os << "*MODAL DYNAMIC" << solver << (q.value("direct", false) ? ", DIRECT" : "") << flag("steady_state", "STEADY STATE") << "\n";
      time_line(os, q), modal_damping(os, q);
    } else if (t == "steady_state_dynamics") {
      os << "*STEADY STATE DYNAMICS" << solver;
      if (has(q, "harmonic") && !q["harmonic"].get<bool>()) os << ", HARMONIC=NO";
      os << "\n";
      write_optional(os, {num(q["freq_min"]), num(q["freq_max"]), has(q, "points") ? q["points"].dump() : "", opt(q, "bias"),
                          has(q, "fourier_terms") ? q["fourier_terms"].dump() : "", opt(q, "period_start"), opt(q, "period_end")});
      modal_damping(os, q);
    } else if (t == "dynamic") {
      os << "*DYNAMIC" << solver << time_params(q) << val("alpha", "ALPHA")
         << (q.value("explicit_scheme", std::string("implicit")) == "explicit" ? ", EXPLICIT" : "")
         << flag("relative_to_absolute", "RELATIVE TO ABSOLUTE") << "\n";
      time_line(os, q);
    } else if (t == "heat_transfer") {
      os << "*HEAT TRANSFER" << solver << flag("steady_state", "STEADY STATE") << time_params(q) << val("deltmx", "DELTMX")
         << flag("eigenmodes", "FREQUENCY") << flag("modal", "MODAL DYNAMIC") << (q.value("storage", false) ? ", STORAGE=YES" : "") << "\n";
      if (q.value("eigenmodes", false)) os << q.value("num_modes", 10) << "\n";
      else time_line(os, q);
    } else if (t == "coupled_temperature_displacement" || t == "uncoupled_temperature_displacement") {
      os << (t[0] == 'c' ? "*COUPLED TEMPERATURE-DISPLACEMENT" : "*UNCOUPLED TEMPERATURE-DISPLACEMENT") << solver
         << flag("steady_state", "STEADY STATE") << time_params(q) << val("alpha", "ALPHA") << val("deltmx", "DELTMX") << "\n";
      time_line(os, q);
    } else if (t == "visco") {
      os << "*VISCO" << solver << ", CETOL=" << num(q["cetol"]) << time_params(q) << "\n", time_line(os, q);
    } else if (t == "electromagnetics") {
      os << "*ELECTROMAGNETICS" << solver << flag("magnetostatics", "MAGNETOSTATICS") << val("omega", "OMEGA")
         << flag("no_heat_transfer", "NO HEAT TRANSFER") << time_params(q) << val("deltmx", "DELTMX") << "\n";
      time_line(os, q);
    } else if (t == "cfd") {
      static const std::map<std::string, const char*> turbulence = {
          {"none", "NONE"}, {"k-epsilon", "K-EPSILON"}, {"k-omega", "K-OMEGA"}, {"sst", "SST"}};
      os << "*CFD" << flag("steady_state", "STEADY STATE") << flag("compressible", "COMPRESSIBLE") << flag("shallow_water", "SHALLOW WATER");
      if (has(q, "turbulence_model")) os << ", TURBULENCE MODEL=" << turbulence.at(q["turbulence_model"].get<std::string>());
      os << time_params(q) << "\n", time_line(os, q);
    } else if (t == "substructure_generate") {
      os << "*SUBSTRUCTURE GENERATE" << solver << "\n";
      if (has(q, "retained_dofs")) {
        os << "*RETAINED NODAL DOFS\n";
        for (const Json& r : q["retained_dofs"])
          os << nset(r["target"]) << ", " << r["first_dof"] << (has(r, "last_dof") ? ", " + r["last_dof"].dump() : "") << "\n";
      }
      os << "*SUBSTRUCTURE MATRIX OUTPUT, STIFFNESS=" << (q.value("stiffness", true) ? "YES" : "NO")
         << ", MASS=" << (q.value("mass", false) ? "YES" : "NO");
      if (has(q, "output_file")) os << ", OUTPUT FILE=" << q["output_file"].get<std::string>();
      os << "\n";
    } else if (t == "sensitivity") {  // *SENSITIVITY(매뉴얼 7.116) + *DESIGN RESPONSE(7.33) + *FILTER(7.57)
      os << "*SENSITIVITY" << (q.value("nlgeom", false) ? ", NLGEOM" : "") << "\n";
      for (const Json& r : q.value("design_responses", Json::array())) {
        os << "*DESIGN RESPONSE, NAME=" << r["name"].get<std::string>() << "\n" << r["type"].get<std::string>();
        if (has(r, "target")) {
          const std::string tt = r["target"].value("type", std::string());
          os << ", " << (tt == "elements" || tt == "parts" || (tt == "set" && is_element_target(r["target"])) ? elset(r["target"]) : nset(r["target"]));
        }
        for (const Json& v : r.value("values", Json::array())) os << ", " << num(v);
        os << "\n";
      }
      if (has(q, "filter_type") || has(q, "filter_radius")) {
        os << "*FILTER";
        if (has(q, "filter_type")) os << ", TYPE=" << (q["filter_type"] == "explicit" ? "EXPLICIT" : "IMPLICIT");
        os << flag("boundary_weighting", "BOUNDARY WEIGHTING=YES") << flag("edge_preservation", "EDGE PRESERVATION=YES")
           << flag("direction_weighting", "DIRECTION WEIGHTING=YES") << "\n";
        if (has(q, "filter_radius")) os << num(q["filter_radius"]) << "\n";
      }
    } else if (t == "crack_propagation") {  // *CRACK PROPAGATION(매뉴얼 7.26) + *HCF(7.70)
      os << "*CRACK PROPAGATION, INPUT=" << q["input_file"].get<std::string>() << ", MATERIAL=" << name_of(q["material"]);
      if (has(q, "length_method")) os << ", LENGTH=" << (q["length_method"] == "cumulative" ? "CUMULATIVE" : "INTERSECTION");
      os << "\n";
      if (has(q, "max_increment") || has(q, "max_angle")) write_optional(os, {opt(q, "max_increment"), opt(q, "max_angle")});
      if (has(q, "hcf_input_file")) {
        os << "*HCF, INPUT=" << q["hcf_input_file"].get<std::string>();
        if (has(q, "hcf_mode")) os << ", MODE=" << q["hcf_mode"];
        if (has(q, "hcf_mission_step")) os << ", MISSION STEP=" << q["hcf_mission_step"];
        if (has(q, "hcf_max_cycle")) os << ", MAX CYCLE=" << num(q["hcf_max_cycle"]);
        if (has(q, "hcf_scaling")) os << ", SCALING=" << num(q["hcf_scaling"]);
        os << "\n";
      }
    } else if (t == "feasible_direction") {  // *FEASIBLE DIRECTION + *OBJECTIVE + *CONSTRAINT + *GEOMETRIC CONSTRAINT
      os << "*FEASIBLE DIRECTION";
      if (has(q, "method")) os << ", METHOD=" << (q["method"] == "gradient_projection" ? "GRADIENT PROJECTION" : "GRADIENT DESCENT");
      os << "\n";
      if (has(q, "step_size")) os << num(q["step_size"]) << "\n";
      os << "*OBJECTIVE";
      if (has(q, "objective_target")) os << ", TARGET=" << (q["objective_target"] == "max" ? "MAX" : "MIN");
      os << "\n" << q["objective"].get<std::string>() << "\n";
      if (has(q, "constraints") && !q["constraints"].empty()) {
        os << "*CONSTRAINT\n";
        for (const Json& c : q["constraints"])
          os << c["response"].get<std::string>() << ", " << (c["relation"] == "ge" ? "GE" : "LE") << ", " << opt(c, "relative_value") << ", "
             << opt(c, "absolute_value") << "\n";
      }
      if (has(q, "geometric_constraints") && !q["geometric_constraints"].empty()) {
        os << "*GEOMETRIC CONSTRAINT\n";
        for (const Json& g : q["geometric_constraints"]) {
          os << g["type"].get<std::string>() << ", " << nset(g["target"]);
          if (has(g, "other_target")) os << ", " << nset(g["other_target"]);
          if (has(g, "value")) os << (has(g, "other_target") ? ", " : ", , ") << num(g["value"]);
          os << "\n";
        }
      }
    } else if (t == "robust_design") {  // *ROBUST DESIGN + *CORRELATION LENGTH + *GEOMETRIC TOLERANCES
      os << "*ROBUST DESIGN" << (q.value("random_field_only", true) ? ", RANDOM FIELD ONLY" : "") << "\n" << num(q["accuracy"]) << "\n";
      os << "*CORRELATION LENGTH\n" << num(q["correlation_length"]) << "\n";
      os << "*GEOMETRIC TOLERANCES, TYPE=NORMAL" << flag("constrained", "CONSTRAINED") << "\n";
      for (const Json& tol : q["tolerances"]) os << nset(tol["target"]) << ", " << num(tol["mean"]) << ", " << num(tol["deviation"]) << "\n";
    } else {
      return false;
    }
    return true;
  }

  void write_steps() {
    if (options_.restart_from_step <= 0) write_blocks(steps_, case_->id, "steps", 0);
    int index = 0;  // 덱에 쓰이는 스텝의 순번(억제된 것 제외)
    for (const Object* step : a_.model().children(case_->id, "step")) {
      if (!step->suppressed) ++index;
      if (!step->suppressed && index <= options_.restart_from_step) continue;  // 재시작: 이미 계산된 스텝은 빼고 쓴다
      write_step(*step);
      write_blocks(steps_, case_->id, "steps", step->id);  // 이 스텝 뒤에 두기로 한 보존 블록(스텝이 억제되어도 나간다)
    }
    if (options_.restart_from_step > 0 && index <= options_.restart_from_step)
      throw Error("invalid_param", "재시작할 스텝 뒤에 쓸 스텝이 없습니다(스텝 " + std::to_string(index) + "개)", {{"param", "step"}, {"steps", index}});
    if (steps_.str().empty()) warn("no_steps", "케이스에 스텝이 없습니다", case_->id);
  }

  void write_step(const Object& step_object) {
    {
      const Object* step = &step_object;
      if (step->suppressed) return;
      const Json& q = step->props;
      std::ostringstream os;
      os << "** step " << step->id << " (" << subtype_of(*step) << ")\n*STEP";
      if (q.value("nlgeom", false)) os << ", NLGEOM";
      if (q.value("perturbation", false)) os << ", PERTURBATION";
      if (has(q, "max_increments")) os << ", INC=" << q["max_increments"];
      if (has(q, "max_fluid_increments")) os << ", INCF=" << q["max_fluid_increments"];
      if (has(q, "load_application")) os << ", AMPLITUDE=" << (q["load_application"] == "step" ? "STEP" : "RAMP");
      os << "\n";
      if (options_.no_analysis) os << "*NO ANALYSIS\n";  // 계산 없이 입력만 검사한다(CAS-39)
      if (q.value("restart_write", false) || has(q, "restart_frequency")) {  // *RESTART, WRITE 는 스텝 안에 둔다(CAS-38)
        os << "*RESTART, WRITE";
        if (has(q, "restart_frequency")) os << ", FREQUENCY=" << q["restart_frequency"].get<int>();
        if (q.value("restart_overlay", false)) os << ", OVERLAY";
        os << "\n";
      }
      try {
        if (!write_procedure(os, *step)) {
          skip(steps_, *step, "step_type");
          return;
        }
      } catch (const Error& e) {
        skip(steps_, *step, e.code());
        return;
      }
      if (q.value("controls_reset", false)) os << "*CONTROLS, RESET\n";
      for (const Json& c : q.value("controls", Json::array())) {
        std::string name = c["parameters"].get<std::string>();
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return ch == '_' ? ' ' : static_cast<char>(std::toupper(ch)); });
        os << "*CONTROLS, PARAMETERS=" << name << "\n";
        write_rows(os, c["values"]);
      }
      for (const Object* ch : a_.model().children(step->id, "step_change"))
        if (!ch->suppressed) write_change(os, *ch);

      // "new" 면 그 종류의 첫 카드에 OP=NEW 를 붙여 앞 스텝의 것을 지운다. 이 스텝에 그 종류가 없어도 빈 카드를 쓴다.
      const std::string bc_op = q.value("bcs_inheritance", std::string("keep")) == "new" ? ", OP=NEW" : "";
      const std::string load_op = q.value("loads_inheritance", std::string("keep")) == "new" ? ", OP=NEW" : "";
      bool any_boundary = false;
      for (const Object& bc : step_entries(a_, *step, "bc")) any_boundary |= write_bc(os, bc, bc_op);  // 스텝 직속 + 구속 셋(D14)
      // 빈 OP=NEW 카드는 앞 스텝에서 실제로 쓴 종류에만 낸다(쓴 적 없는 종류의 카드는 솔버가 오류로 본다:
      // 예) 초기 온도 없는 *TEMPERATURE, 물리 상수 없는 *RADIATE).
      if (!any_boundary && !bc_op.empty() && boundary_seen_) os << "*BOUNDARY, OP=NEW\n";
      boundary_seen_ |= any_boundary;
      std::set<std::string> used;
      for (const Object& load : step_entries(a_, *step, "load")) write_load(os, load, load_op, used);  // 스텝 직속 + 하중 셋(계수 적용, D14)
      if (!load_op.empty())
        for (const std::string& k : loads_seen_)
          if (!used.count(k)) os << k << ", OP=NEW\n";
      loads_seen_.insert(used.begin(), used.end());
      write_blocks(os, step->id);
      for (const Object* out : a_.model().children(step->id, "output_request"))
        if (!out->suppressed) write_output(os, *out);
      os << "*END STEP\n";
      steps_ << os.str();
    }
  }

  bool write_bc(std::ostream& os, const Object& bc, const std::string& op) {
    const Json& q = bc.props;
    const std::string t = subtype_of(bc);
    try {
      std::ostringstream card;
      if (auto it = transform_skip_.find(bc.id); it != transform_skip_.end()) throw Error(it->second, "좌표 변환을 쓰지 못합니다");
      if (t == "base_motion") {
        if (!has(q, "amplitude")) throw Error("missing_amplitude", "가진에는 시간 함수가 필요합니다");
        card << "*BASE MOTION, DOF=" << q["dof"] << amplitude_param(bc);
        if (has(q, "motion")) card << ", TYPE=" << (q["motion"] == "acceleration" ? "ACCELERATION" : "DISPLACEMENT");
        card << "\n";
        os << card.str();
        return false;
      }
      const std::string set = nset(q["target"]);
      card << "*BOUNDARY" << op << amplitude_param(bc);
      if (t == "displacement") {
        card << "\n";
        const Json values = q.value("values", Json::array());
        std::size_t i = 0;
        for (const Json& d : q["dofs"]) {
          const double v = i < values.size() ? values[i].get<double>() : 0.0;
          ++i;
          card << set << ", " << d << ", " << d;
          if (v != 0.0) card << ", " << num(v);
          card << "\n";
        }
      } else if (t == "symmetry" || t == "antisymmetry") {
        // 대칭: 법선 방향 병진과 면 안의 두 축 둘레 회전을 막는다. 반대칭은 그 나머지.
        card << "\n";
        const int n = q["normal"].get<std::string>()[0] - 'x';
        for (int k = 0; k < 3; ++k) {
          const int dof = ((t == "symmetry") == (k == n)) ? k + 1 : k + 4;
          if (dof > 3 && !has_rotations_) continue;  // 회전 자유도가 없는 모델
          card << set << ", " << dof << ", " << dof << "\n";
        }
      } else if (t == "fixed_current" || t == "user") {
        card << (t == "user" ? ", USER" : ", FIXED") << "\n";
        for (const Json& d : q["dofs"]) card << set << ", " << d << ", " << d << "\n";
      } else if (t == "temperature") {
        card << "\n" << set << ", 11, 11, " << num(q["value"]) << "\n";
      } else if (t == "network") {
        // 네트워크(ccx 2.22 매뉴얼 7.4): 1 = 질량유량, 2 = 전압력(기체)·정압(액체)·수심(수로), 11 = (전)온도
        const std::string qn = q["quantity"].get<std::string>();
        const int dof = qn == "mass_flow" ? 1 : qn == "temperature" ? 11 : 2;
        card << "\n" << set << ", " << dof << ", " << dof << ", " << num(q["value"]) << "\n";
      } else if (t == "electromagnetic") {
        card << "\n" << set << ", 8, 8, " << num(q["value"]) << "\n";  // 전위 V 는 자유도 8(매뉴얼 6.9 전자기)
      } else if (t == "fluid") {
        // 3D 유체(매뉴얼 6.9 CFD): 속도 1~3, 압력 8, 온도 11. 난류량의 자유도 번호는 매뉴얼에서 확인하지 못해 쓰지 않는다
        const std::string qn = q["quantity"].get<std::string>();
        const Json vals = q["values"];
        card << "\n";
        if (qn == "velocity") {
          for (std::size_t k = 0; k < 3 && k < vals.size(); ++k) card << set << ", " << k + 1 << ", " << k + 1 << ", " << num(vals[k]) << "\n";
        } else if (qn == "pressure" || qn == "temperature") {
          const int dof = qn == "pressure" ? 8 : 11;
          card << set << ", " << dof << ", " << dof << ", " << num(vals[0]) << "\n";
        } else {
          throw Error("fluid_turbulence", "난류량 경계조건의 자유도 번호를 확인하지 못해 아직 쓰지 못합니다");
        }
      } else if (t == "submodel") {
        // 서브모델(매뉴얼 7.4.3): *SUBMODEL, TYPE=NODE, INPUT=<전역 결과 frd> + *BOUNDARY, SUBMODEL, STEP=n
        if (!has(q, "global_file")) throw Error("missing_file", "전역 모델 결과 파일이 없습니다");
        std::ostringstream sub;
        sub << "*SUBMODEL, TYPE=NODE, INPUT=" << q["global_file"].get<std::string>();
        if (has(q, "global_elements")) sub << ", GLOBAL ELSET=" << elset(q["global_elements"]);
        sub << "\n" << set << "\n";
        card.str(std::string());
        card << sub.str() << "*BOUNDARY, SUBMODEL, STEP=" << q.value("global_step", 1) << op << "\n";
        for (const Json& d : q["dofs"]) card << set << ", " << d << ", " << d << "\n";
      } else {
        throw Error("bc_type", "이 경계조건은 아직 쓰지 못합니다");
      }
      os << card.str();
      return true;
    } catch (const Error& e) {
      skip(os, bc, e.code());
      return false;
    }
  }

  void write_load(std::ostream& os, const Object& load, const std::string& op, std::set<std::string>& used) {
    const Json& q = load.props;
    const std::string t = subtype_of(load);
    try {
      std::ostringstream card;
      std::string keyword;
      // 카드 머리. 같은 스텝의 같은 키워드는 모두 같은 OP 를 쓴다.
      auto head = [&](const char* kw, const std::string& extra = "") {
        keyword = kw;
        card << kw << op << amplitude_param(load);
        if (has(q, "phase")) card << ", LOAD CASE=" << (q["phase"] == "imaginary" ? 2 : 1);
        if (has(q, "sector")) card << ", SECTOR=" << q["sector"];
        card << extra << "\n";
      };
      if (auto it = transform_skip_.find(load.id); it != transform_skip_.end()) throw Error(it->second, "좌표 변환을 쓰지 못합니다");
      if (has(q, "csys") && t != "force" && t != "moment") throw Error("local_csys", "집중 하중이 아닌 하중의 국부 좌표계는 아직 쓰지 못합니다");
      if (has(q, "distribution") && t != "pressure") throw Error("distribution", "공간 분포 함수는 압력에서만 쓸 수 있습니다");
      if (t == "pressure" && has(q, "distribution")) {
        // 공간 분포(LOD-01, 정수압 등): 요소면마다 면 중심 (x, y, z) 로 수식을 계산해 값 × 분포를 한 줄씩 쓴다.
        // *DLOAD 는 요소 번호를 직접 받으므로(매뉴얼 7.37) 셋 없이 "요소, P면, 값" 으로 쓴다. 쉘(요소 전체)은 요소 중심
        const Object& fn = a_.model().get(q["distribution"].get<Id>());
        if (fn.kind != "function" || subtype_of(fn) != "expression")
          throw Error("distribution", "공간 분포는 변수 x, y, z 의 수식 함수여야 합니다: " + fn.name);
        const std::string text = fn.props.value("expression", std::string());
        const Mesh& m = a_.mesh();
        auto centroid = [&](const std::vector<Id>& nodes) {
          std::map<std::string, double> v{{"x", 0.0}, {"y", 0.0}, {"z", 0.0}, {"t", 0.0}};
          for (Id n : nodes) {
            const Vec3 p = m.node(n);
            v["x"] += p[0] / nodes.size(), v["y"] += p[1] / nodes.size(), v["z"] += p[2] / nodes.size();
          }
          return v;
        };
        const double scale = q.value("value", 1.0);
        head("*DLOAD");
        int lines = 0;
        if (is_element_target(q["target"])) {
          for (const Json& e : resolve_target(a_, q["target"], "elements")) {
            const Element el = m.element(e.get<Id>());
            if (!in_scope_elem(el.id)) continue;
            card << el.id << ", P, " << num(scale * evaluate_expression(text, centroid(el.nodes))) << "\n", ++lines;
          }
        } else {
          for (const auto& [e, f] : faces_of(q["target"])) {
            const Element el = m.element(e);
            const auto& faces = shape_faces(el.shape);
            if (f < 1 || f > static_cast<int>(faces.size())) continue;
            std::vector<Id> nodes;
            for (int k : faces[static_cast<std::size_t>(f - 1)]) nodes.push_back(el.nodes[static_cast<std::size_t>(k)]);
            card << el.id << ", P" << face_label(e, f) << ", " << num(scale * evaluate_expression(text, centroid(nodes))) << "\n", ++lines;
          }
        }
        if (!lines) throw Error("invalid_state", "대상에 면이 없습니다");
      } else if (t == "force" || t == "moment") {
        const std::string set = nset(q["target"]);
        head("*CLOAD");
        // 0 인 성분도 쓴다: 솔버는 (노드, 자유도)마다 앞 스텝의 값을 이어 가므로, 쓰지 않으면 앞 스텝에서
        // 같은 대상에 준 성분이 남는다. 모델의 규칙(같은 종류·같은 대상은 뒤의 것이 통째로 바꾼다)과 맞춘다.
        for (int d = 0; d < 3; ++d)
          card << set << ", " << d + (t == "force" ? 1 : 4) << ", " << num(q["components"][static_cast<std::size_t>(d)]) << "\n";
      } else if (t == "pressure") {
        if (is_element_target(q["target"])) {  // 쉘 요소의 면 전체: 요소 법선 방향으로 미는 쪽이 양(솔버로 확인)
          const std::string set = elset(q["target"]);
          head("*DLOAD");
          card << set << ", P, " << num(q["value"]) << "\n";
        } else {
          const auto groups = face_groups(q["target"]);
          head("*DLOAD");
          for (const auto& [label, set] : groups) card << set << ", P" << label << ", " << num(q["value"]) << "\n";
        }
      } else if (t == "edge_load") {
        const auto groups = face_groups(q["target"]);
        head("*DLOAD");
        for (const auto& [label, set] : groups) card << set << ", EDNOR" << label - 2 << ", " << num(q["value"]) << "\n";
      } else if (t == "gravity" || t == "acceleration") {
        const std::string set = elset(q["target"]);
        const Json& d = q["direction"];
        const double len = std::sqrt(d[0].get<double>() * d[0].get<double>() + d[1].get<double>() * d[1].get<double>() + d[2].get<double>() * d[2].get<double>());
        if (len == 0.0) throw Error("zero_direction", "방향의 길이가 0 입니다");
        head("*DLOAD");
        card << set << ", GRAV, " << num(q["value"]);
        for (const Json& v : d) card << ", " << num(v.get<double>() / len);
        card << "\n";
      } else if (t == "centrifugal") {
        const std::string set = elset(q["target"]);
        const double w = q["omega"].get<double>();
        head("*DLOAD");
        card << set << ", CENTRIF, " << num(w * w);
        for (const char* k : {"axis_point", "axis_direction"})
          for (const Json& v : q[k]) card << ", " << num(v);
        card << "\n";
      } else if (t == "newton_gravity") {
        const std::string set = elset(q["target"]);
        head("*DLOAD");
        card << set << ", NEWTON\n";
      } else if (t == "temperature" || t == "temperature_gradient") {
        const std::string set = nset(q["target"]);
        head("*TEMPERATURE");
        card << set << ", " << num(q["value"]);
        if (t == "temperature_gradient") card << ", " << num(q["gradient"]);
        card << "\n";
      } else if (t == "mapped_field") {
        // 외부 필드 매핑(LOD-14): 점 자료를 노드(온도) 또는 면 중심(압력·면 열유속)으로 보간해 노드·면마다 값을 쓴다
        const std::string quantity = q["quantity"].get<std::string>();
        std::vector<Vec3> pts;
        std::vector<double> vals;
        for (const Json& row : q["points"]) pts.push_back({row[0].get<double>(), row[1].get<double>(), row[2].get<double>()}), vals.push_back(row[3].get<double>());
        const std::string method = q.value("method", std::string("idw"));
        const double power = q.value("power", 2.0), radius = q.value("radius", 0.0);
        if (quantity == "temperature") {
          const std::vector<Id> nodes = nodes_of(q["target"]);
          if (nodes.empty()) throw Error("empty_target", "대상에 노드가 없습니다");
          std::vector<Vec3> at;
          for (Id n : nodes) at.push_back(m_.node(n));
          const std::vector<double> v = interpolate_points(pts, vals, at, method, power, radius);
          head("*TEMPERATURE");
          for (std::size_t i = 0; i < nodes.size(); ++i) card << nodes[i] << ", " << num(v[i]) << "\n";
        } else {
          const auto faces = faces_of(q["target"]);
          if (faces.empty()) throw Error("empty_target", "대상에 면이 없습니다");
          std::vector<Vec3> at;
          for (const auto& [e, f] : faces) {
            const Element el = m_.element(e);
            const auto& fn = shape_faces(el.shape)[static_cast<std::size_t>(f - 1)];
            Vec3 c{0, 0, 0};
            for (int k : fn) {
              const Vec3 p = m_.node(el.nodes[static_cast<std::size_t>(k)]);
              c = {c[0] + p[0], c[1] + p[1], c[2] + p[2]};
            }
            at.push_back({c[0] / fn.size(), c[1] / fn.size(), c[2] / fn.size()});
          }
          const std::vector<double> v = interpolate_points(pts, vals, at, method, power, radius);
          head(quantity == "pressure" ? "*DLOAD" : "*DFLUX");
          for (std::size_t i = 0; i < faces.size(); ++i)
            card << faces[i].first << ", " << (quantity == "pressure" ? "P" : "S") << faces[i].second << ", " << num(v[i]) << "\n";
        }
      } else if (t == "traction" || t == "total_force" || t == "line_load") {
        // 솔버에는 면의 접선 분포력·보의 선하중 카드가 없다 → 등가 집중 하중(*CLOAD)으로 환산해 쓴다(합은 보존).
        // 면: 면적 × 분포력을 꼭짓점에 나눈다(1차: 균등, 2차 사각형: 꼭짓점 −1/12·중간 1/3, 2차 삼각형: 꼭짓점 0·중간 1/3 — 일관 하중).
        // total_force: 면적 가중으로 합력을 나눈다. 보 선하중: 요소 길이 × 분포력을 끝 절점에(B31 1/2씩, B32 끝 1/6·가운데 2/3).
        std::map<Id, std::array<double, 3>> nodal;
        auto add = [&](Id n, const std::array<double, 3>& f, double w) {
          auto& acc = nodal[n];
          for (int k = 0; k < 3; ++k) acc[k] += f[k] * w;
        };
        if (t == "line_load") {
          const std::vector<Id> elems = elems_of(q["target"]);
          if (elems.empty()) throw Error("empty_target", "대상에 요소가 없습니다");
          const std::array<double, 3> qv{q["components"][0].get<double>(), q["components"][1].get<double>(), q["components"][2].get<double>()};
          for (Id e : elems) {
            const Element el = m_.element(e);
            if (shape_info(el.shape).dim != 1) throw Error("invalid_target", "선하중은 보·트러스 요소에만 둔다");
            // 2차 선 요소의 절점 순서는 (끝, 가운데, 끝)이다(mesh.hpp) — 길이는 양 끝 사이, 가운데 절점이 2/3 를 받는다
            const bool quadratic = el.nodes.size() >= 3 && el.nodes[2];
            const Vec3 a = m_.node(el.nodes[0]), b = m_.node(el.nodes[quadratic ? 2 : 1]);
            const double L = std::sqrt((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]) + (b[2] - a[2]) * (b[2] - a[2]));
            if (quadratic) add(el.nodes[0], qv, L / 6), add(el.nodes[2], qv, L / 6), add(el.nodes[1], qv, 2 * L / 3);
            else add(el.nodes[0], qv, L / 2), add(el.nodes[1], qv, L / 2);
          }
        } else {
          const auto faces = faces_of(q["target"]);
          if (faces.empty()) throw Error("empty_target", "대상에 면이 없습니다");
          double total_area = 0;
          std::vector<std::pair<std::vector<Id>, double>> face_nodes;  // (면 절점, 넓이)
          for (const auto& [e, f] : faces) {
            const Element el = m_.element(e);
            const auto& fn = shape_faces(el.shape)[static_cast<std::size_t>(f - 1)];
            std::vector<Id> ids;
            std::vector<Vec3> pts;
            for (int k : fn) ids.push_back(el.nodes[static_cast<std::size_t>(k)]);
            const std::size_t nc = ids.size() >= 6 ? (ids.size() == 6 ? 3 : 4) : ids.size();
            for (std::size_t k = 0; k < nc; ++k) pts.push_back(m_.node(ids[k]));
            const Vec3 na = face_normal_area(pts);
            const double area = std::sqrt(na[0] * na[0] + na[1] * na[1] + na[2] * na[2]);
            total_area += area;
            face_nodes.push_back({ids, area});
          }
          const std::array<double, 3> comp{q["components"][0].get<double>(), q["components"][1].get<double>(), q["components"][2].get<double>()};
          for (const auto& [ids, area] : face_nodes) {
            // 면의 힘 = 분포력 × 넓이, 또는 합력 × (넓이 / 전체 넓이)
            const double scale = t == "traction" ? area : (total_area > 0 ? area / total_area : 0.0);
            const std::size_t n = ids.size();
            if (n == 8) {
              for (std::size_t k = 0; k < 4; ++k) add(ids[k], comp, -scale / 12.0);
              for (std::size_t k = 4; k < 8; ++k) add(ids[k], comp, scale / 3.0);
            } else if (n == 6) {
              for (std::size_t k = 3; k < 6; ++k) add(ids[k], comp, scale / 3.0);
            } else {
              for (Id id : ids) add(id, comp, scale / static_cast<double>(n));
            }
          }
        }
        head("*CLOAD");
        for (const auto& [n, f] : nodal)
          for (int d = 0; d < 3; ++d)
            if (f[static_cast<std::size_t>(d)] != 0.0) card << n << ", " << d + 1 << ", " << num(f[static_cast<std::size_t>(d)]) << "\n";
        warn("equivalent_nodal_load", "분포력·선하중·합력은 등가 집중 하중(*CLOAD)으로 썼습니다", load.id);
      } else if (t == "remote_force") {
        // 원격 하중(LOD-06): 기준 노드를 새로 만들어 분배 커플링(*COUPLING + *DISTRIBUTING)으로 면에 잇고 그 노드에 *CLOAD
        const std::string surf = surface(q["target"]);
        const Id ref = ++extra_node_;
        mesh_ << "*NODE\n" << ref << ", " << num(q["point"][0]) << ", " << num(q["point"][1]) << ", " << num(q["point"][2]) << "\n";
        model_ << "*COUPLING, REF NODE=" << ref << ", SURFACE=" << surf << ", CONSTRAINT NAME=OFEP_RF" << load.id << "\n*DISTRIBUTING\n1, 6\n";
        head("*CLOAD");
        for (int d = 0; d < 3; ++d) card << ref << ", " << d + 1 << ", " << num(q["force"][static_cast<std::size_t>(d)]) << "\n";
        if (has(q, "moment"))
          for (int d = 0; d < 3; ++d) card << ref << ", " << d + 4 << ", " << num(q["moment"][static_cast<std::size_t>(d)]) << "\n";
      } else if (t == "pretension") {
        // 볼트 프리텐션(LOD-08, 매뉴얼 7.106): 단면(면)과 기준 노드 → 기준 노드의 자유도 1 에 힘(*CLOAD) 또는 길이 고정(*BOUNDARY)
        const std::string surf = surface(q["target"]);
        const Id node = q["node"].get<Id>();
        if (!m_.has_node(node)) throw Error("not_found", "기준 노드가 메시에 없습니다");
        model_ << "*PRE-TENSION SECTION, SURFACE=" << surf << ", NODE=" << node << "\n";
        if (has(q, "direction")) model_ << num(q["direction"][0]) << ", " << num(q["direction"][1]) << ", " << num(q["direction"][2]) << "\n";
        if (q.value("fixed", false)) {
          keyword = "*BOUNDARY";
          card << "*BOUNDARY" << op << "\n" << node << ", 1, 1\n";
        } else {
          if (!has(q, "force")) throw Error("missing_param", "체결력(force) 또는 fixed 가 필요합니다");
          head("*CLOAD");
          card << node << ", 1, " << num(q["force"]) << "\n";
        }
      } else if (t == "network_pressure") {
        // 네트워크 압력 연동(LOD-19): PxNP 라벨에 값 대신 유체 노드 번호
        const auto groups = face_groups(q["target"]);
        head("*DLOAD");
        for (const auto& [label, set] : groups) card << set << ", P" << label << "NP, " << q["fluid_node"] << "\n";
      } else if (t == "coil_current") {
        // 전자기 구동 전류(LOD-35): 열전달 유사로 전류는 열유속 경계조건(*CFLUX, 자유도 11)으로 준다(매뉴얼 6.9 전자기)
        const std::string set = nset(q["target"]);
        head("*CFLUX");
        card << set << ", 11, " << num(q["value"]) << "\n";
      } else if (t == "submodel_traction" || t == "submodel_force") {
        // 서브모델 하중(LOD-34): *SUBMODEL(모델 정의) + *DSLOAD/*CLOAD, SUBMODEL, STEP=n
        if (!has(q, "global_file")) throw Error("missing_file", "전역 모델 결과 파일이 없습니다");
        if (t == "submodel_traction") {
          const std::string surf = surface(q["target"]);
          model_ << "*SUBMODEL, TYPE=SURFACE, INPUT=" << q["global_file"].get<std::string>();
          if (has(q, "global_elements")) model_ << ", GLOBAL ELSET=" << elset(q["global_elements"]);
          model_ << "\n" << surf << "\n";
          keyword = "*DSLOAD";
          card << "*DSLOAD, SUBMODEL, STEP=" << q.value("global_step", 1) << op << "\n";
          for (const auto& [label, set] : face_groups(q["target"])) card << set << ", P" << label << "\n";
        } else {
          const std::string set = nset(q["target"]);
          model_ << "*SUBMODEL, TYPE=NODE, INPUT=" << q["global_file"].get<std::string>() << "\n" << set << "\n";
          keyword = "*CLOAD";
          card << "*CLOAD, SUBMODEL, STEP=" << q.value("global_step", 1) << op << "\n";
          for (int d = 1; d <= 3; ++d) card << set << ", " << d << "\n";
        }
      } else if (t == "user") {
        // 사용자 서브루틴 하중(LOD-37, 매뉴얼 7.15/7.7/7.43/7.42/7.58/7.107): 비균일 라벨 PxNUy·SxNUy·BFNU·FxNUy·RxNUy, 집중 하중·열유속은 USER 매개변수
        const std::string base = q["base"].get<std::string>(), label = q.value("label", std::string());
        if (base == "force") {
          const std::string set = nset(q["target"]);
          keyword = "*CLOAD";
          card << "*CLOAD, USER" << op << "\n";
          for (int d = 1; d <= 3; ++d) card << set << ", " << d << ", 0.\n";
        } else if (base == "temperature") {
          const std::string set = nset(q["target"]);
          keyword = "*CFLUX";
          card << "*CFLUX, USER" << op << "\n" << set << ", 11, 0.\n";
        } else if (base == "body_flux") {
          head("*DFLUX");
          card << elset(q["target"]) << ", BFNU, 0.\n";
        } else {
          const auto groups = face_groups(q["target"]);
          const char* kw = base == "pressure" ? "*DLOAD" : base == "surface_flux" ? "*DFLUX" : base == "film" ? "*FILM" : "*RADIATE";
          const char* prefix = base == "pressure" ? "P" : base == "surface_flux" ? "S" : base == "film" ? "F" : "R";
          head(kw);
          for (const auto& [lab, set] : groups) card << set << ", " << prefix << lab << "NU" << label << "\n";
        }
      } else if (t == "temperature_from_file") {
        head("*TEMPERATURE", ", FILE=" + q["file"].get<std::string>() + (has(q, "begin_step") ? ", BSTEP=" + q["begin_step"].dump() : ""));
      } else if (t == "concentrated_flux") {
        const std::string set = nset(q["target"]);
        head("*CFLUX");
        card << set << ", 11, " << num(q["value"]) << "\n";
      } else if (t == "surface_flux") {
        const auto groups = face_groups(q["target"]);
        head("*DFLUX");
        for (const auto& [label, set] : groups) card << set << ", S" << label << ", " << num(q["value"]) << "\n";
      } else if (t == "body_flux") {
        const std::string set = elset(q["target"]);
        head("*DFLUX");
        card << set << ", BF, " << num(q["value"]) << "\n";
      } else if (t == "film" || t == "forced_convection") {
        const auto groups = face_groups(q["target"]);
        std::string extra;
        if (has(q, "coefficient_amplitude")) {
          const Id id = q["coefficient_amplitude"].get<Id>();
          if (!amplitude_written_.count(id)) throw Error("amplitude_function", "수식 함수는 시간 함수로 쓸 수 없습니다");
          extra = ", FILM AMPLITUDE=" + a_.model().get(id).name;
        }
        head("*FILM", extra);
        for (const auto& [label, set] : groups) {
          if (t == "film") card << set << ", F" << label << ", " << num(q["sink_temperature"]) << ", " << num(q["coefficient"]) << "\n";
          else card << set << ", F" << label << "FC, " << q["fluid_node"] << ", " << num(q["coefficient"]) << "\n";
        }
      } else if (t == "radiation" || t == "cavity_radiation") {
        const auto groups = face_groups(q["target"]);
        if (t == "radiation" && has(q, "sink_node")) throw Error("radiation_sink_node", "노드로 주는 주변 온도는 아직 쓰지 못합니다");
        if (!has(q, "sink_temperature")) throw Error("missing_sink_temperature", "주변 온도가 없습니다");
        head("*RADIATE", t == "cavity_radiation" && has(q, "cavity") ? ", CAVITY=" + q["cavity"].get<std::string>() : "");
        for (const auto& [label, set] : groups)
          card << set << ", R" << label << (t == "cavity_radiation" ? "CR" : "") << ", " << num(q["sink_temperature"]) << ", "
               << num(q["emissivity"]) << "\n";
      } else {
        throw Error("load_type", "이 하중은 아직 쓰지 못합니다");
      }
      os << card.str();
      used.insert(keyword);
    } catch (const Error& e) {
      skip(os, load, e.code());
    }
  }

  void write_change(std::ostream& os, const Object& ch) {
    const Json& q = ch.props;
    const std::string t = subtype_of(ch);
    try {
      std::ostringstream card;
      if (t == "model_change_element") {
        card << "*MODEL CHANGE, TYPE=ELEMENT, " << (q["action"] == "add" ? "ADD" : "REMOVE") << "\n" << elset(q["target"]) << "\n";
      } else if (t == "model_change_contact") {
        auto it = pair_surfaces_.find(q["pair"].get<Id>());
        if (it == pair_surfaces_.end()) throw Error("missing_reference", "덱에 없는 접촉 쌍입니다");
        card << "*MODEL CHANGE, TYPE=CONTACT PAIR, " << (q["action"] == "add" ? "ADD" : "REMOVE") << "\n"
             << it->second.first << ", " << it->second.second << "\n";
      } else if (t == "change_friction") {
        card << "*CHANGE FRICTION, INTERACTION=" << name_of(q["interaction"]) << "\n";
        write_friction(card, q);
      } else if (t == "change_surface_behavior") {
        card << "*CHANGE SURFACE BEHAVIOR, INTERACTION=" << name_of(q["interaction"]) << "\n";
        write_surface_behavior(card, q);
      } else if (t == "change_material") {
        card << "*CHANGE MATERIAL, NAME=" << name_of(q["material"]) << "\n*CHANGE PLASTIC"
             << (q.value("hardening", std::string("isotropic")) == "kinematic" ? ", HARDENING=KINEMATIC" : "") << "\n";
        write_rows(card, q["data"]);
      } else if (t == "change_section") {
        // 솔리드 섹션(solid·truss)의 요소에만 쓸 수 있다. 쉘·보·멤브레인 타입 요소가 들어 있으면 쓰지 않는다.
        for (Id e : elems_of(q["target"])) {
          const std::string& ty = type_of_[e];
          if (is_shell_type(ty) || ty.rfind("B", 0) == 0) throw Error("solid_section_only", "스텝 중 섹션 변경은 솔리드 섹션의 요소에만 할 수 있습니다(요소 " + std::to_string(e) + " 은 " + ty + ")");
        }
        card << "*CHANGE SOLID SECTION, ELSET=" << elset(q["target"]) << ", MATERIAL=" << deck_material_name(a_.model().get(q["material"].get<Id>())) << orientation_param(ch) << "\n";
      } else if (t == "change_contact_type") {
        // *CHANGE CONTACT TYPE(매뉴얼 7.8): 동해석 스텝 시작에서 TO NODE TO SURFACE 또는 TO MASSLESS 만 된다
        const std::string m = q["method"].get<std::string>();
        if (m == "node_to_surface") card << "*CHANGE CONTACT TYPE, TO NODE TO SURFACE\n";
        else if (m == "massless") card << "*CHANGE CONTACT TYPE, TO MASSLESS\n";
        else throw Error("change_type", "솔버는 접촉 방식을 NODE TO SURFACE 또는 MASSLESS 로만 바꿀 수 있습니다");
      } else {
        throw Error("change_type", "이 변경은 아직 쓰지 못합니다");
      }
      os << card.str();
    } catch (const Error& e) {
      skip(os, ch, e.code());
    }
  }

  void write_output(std::ostream& os, const Object& out) {
    const Json& q = out.props;
    const std::string t = subtype_of(out);
    try {
      std::ostringstream card;
      std::string common;
      if (has(q, "frequency")) common += ", FREQUENCY=" + q["frequency"].dump();
      if (has(q, "time_points")) common += ", TIME POINTS=" + name_of(q["time_points"]);
      auto totals = [&] {
        const std::string v = q.value("totals", std::string("no"));
        return v == "yes" ? std::string(", TOTALS=YES") : v == "only" ? std::string(", TOTALS=ONLY") : std::string();
      };
      if (q.value("binary", false)) warn("ignored_option", "이진 출력은 솔버 실행 옵션으로 정합니다(덱에는 쓰지 않음)", out.id);
      if (t == "node_file" || t == "element_file") {
        card << (t == "node_file" ? "*NODE FILE" : "*EL FILE");
        if (has(q, "target")) card << ", NSET=" << nset(q["target"]);
        card << common;
        if (has(q, "frequency_f")) card << ", FREQUENCYF=" << q["frequency_f"];
        if (has(q, "global") && !q["global"].get<bool>()) card << ", GLOBAL=NO";
        // SECTION FORCES 와 OUTPUT=3D 는 함께 쓸 수 없다(매뉴얼 *EL FILE). 보·쉘의 기본이 3D 라 단면력을 요청하면 2D 를 명시한다
        const bool section_forces = t == "element_file" && q.value("section_forces", false);
        if (section_forces && has(q, "expand") && q["expand"] != "2d")
          warn("section_forces_output_2d", "단면력(SECTION FORCES)은 OUTPUT=3D 와 함께 쓸 수 없어 OUTPUT=2D 로 씁니다", out.id);
        if (section_forces) card << ", OUTPUT=2D";
        else if (has(q, "expand")) card << ", OUTPUT=" << (q["expand"] == "2d" ? "2D" : "3D");
        if (section_forces) card << ", SECTION FORCES";
        if (q.value("last_iterations", false)) card << ", LAST ITERATIONS";
        if (q.value("contact_elements", false)) card << ", CONTACT ELEMENTS";
      } else if (t == "contact_file") {
        card << "*CONTACT FILE" << common;
      } else if (t == "node_print" || t == "element_print") {
        card << (t == "node_print" ? "*NODE PRINT, NSET=" + nset(q["target"]) : "*EL PRINT, ELSET=" + elset(q["target"])) << common;
        if (has(q, "frequency_f")) card << ", FREQUENCYF=" << q["frequency_f"];
        if (q.value("global", false)) card << ", GLOBAL=YES";
        card << totals();
      } else if (t == "contact_print") {
        card << "*CONTACT PRINT";
        if (has(q, "pair")) {
          auto it = pair_surfaces_.find(q["pair"].get<Id>());
          if (it == pair_surfaces_.end()) throw Error("missing_reference", "덱에 없는 접촉 쌍입니다");
          card << ", SLAVE=" << it->second.first << ", MASTER=" << it->second.second;
        }
        card << common << totals();
      } else if (t == "section_print") {
        card << "*SECTION PRINT, SURFACE=" << surface(q["target"]) << ", NAME=" << q["label"].get<std::string>() << common;
      } else {
        throw Error("output_type", "이 출력 요청은 아직 쓰지 못합니다");
      }
      card << "\n";
      const Json& vars = q["variables"];
      for (std::size_t i = 0; i < vars.size(); ++i) card << vars[i].get<std::string>() << (i + 1 < vars.size() ? ", " : "\n");
      os << card.str();
    } catch (const Error& e) {
      skip(os, out, e.code());
    }
  }

  const App& a_;
  const Mesh& m_;
  const Object* case_;
  DeckOptions options_;
  std::ostringstream mesh_, sets_, model_, steps_;
  Id extra_node_ = 0;  // 덱에만 더하는 노드(원격 하중의 기준 노드): 메시의 가장 큰 번호 다음부터
  Json warnings_ = Json::array(), skipped_ = Json::array();

  bool scoped_ = false;
  std::set<Id> elems_, nodes_;
  std::map<Id, std::string> type_of_;  // 요소 → 솔버 타입
  bool has_rotations_ = false;         // 쉘·보가 있는 모델(회전 자유도)

  std::map<std::string, std::string> internal_;  // 내용 → 내부 셋 이름
  std::map<std::string, int> counter_;
  std::map<std::string, Id> names_;
  std::set<Id> distribution_ids_;
  std::vector<Object> transform_entries_;  // write_transforms 가 보는 하중·경계조건(셋에서 온 사본 포함)
  std::set<Id> set_written_, part_written_, orientation_written_, material_written_, amplitude_written_, interaction_written_;
  std::map<Id, std::pair<std::string, std::string>> pair_surfaces_;
  std::map<Id, std::string> transform_skip_;  // 좌표 변환 때문에 쓰지 못하는 경계조건·하중 → 사유
  bool boundary_seen_ = false;          // 앞 스텝에서 *BOUNDARY 를 썼다
  std::set<std::string> loads_seen_;    // 앞 스텝에서 쓴 하중 키워드
};

}  // namespace

DeckResult write_deck(const App& app, const Object& analysis_case, const DeckOptions& options) {
  return Deck(app, &analysis_case, options).run();
}
DeckResult write_mesh_deck(const App& app) { return Deck(app, nullptr).run(); }

}  // namespace ofep
