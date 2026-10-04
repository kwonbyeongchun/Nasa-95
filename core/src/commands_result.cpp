// 결과 조회 명령(4단계): 결과 파일 열기, 프레임·필드 목록, 값·파생량·최대최소·반력 합·이력, dat 의 표(모드·좌굴), 수렴 이력.
// 결과는 실행 중 상태로만 둔다(Undo 대상이 아니다).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

#include "ofep/error.hpp"
#include "ofep/expr.hpp"
#include "ofep/mesh.hpp"
#include "ofep/results.hpp"

namespace ofep {

namespace {

using F = FieldSpec;
namespace fs = std::filesystem;

fs::path path_of(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }
std::string utf8(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}
bool has(const Json& p, const char* key) { return p.contains(key) && !p[key].is_null(); }

CommandSpec base(const std::string& name, char kind, const std::string& desc, const std::string& features) {
  CommandSpec c;
  c.name = name, c.kind = kind, c.undoable = false, c.desc = desc, c.features = features;
  return c;
}

F id_param() { return F("result", "integer", "결과(result.open 이 돌려준 번호)").call_req().ge(1).ex(1); }
F frame_param() { return F("frame", "integer", "프레임 번호(1 부터)").call_req().ge(1).ex(1); }
F field_param() { return F("field", "string", "결과 종류(솔버의 이름. 예: DISP, STRESS)").call_req().ex("DISP"); }
F nodes_param() { return F("nodes", "integer_list", "이 노드만(없으면 전부)").ex(Json::array({1, 2})); }

ResultFile& file_of(App& a, const Json& p) { return results(a).get(p["result"].get<Id>()); }
ResultField& field_of(App& a, const Json& p, ResultFile** file_out = nullptr) {
  ResultFile& file = file_of(a, p);
  if (file_out) *file_out = &file;
  return file.field(file.frame(p["frame"].get<int>()), p["field"].get<std::string>());
}

// 성분 값: 파일의 성분 이름 또는 파생량 이름.
std::vector<double> component_values(const ResultField& f, const std::string& name) {
  const std::size_t nc = f.components.size();
  for (std::size_t c = 0; c < nc; ++c) {
    if (f.components[c] != name) continue;
    std::vector<double> out(f.count);
    for (std::size_t i = 0; i < f.count; ++i) out[i] = f.data[i * nc + c];
    return out;
  }
  const auto derived = derived_names(f);
  if (std::find(derived.begin(), derived.end(), name) == derived.end()) {
    Json available = f.components;
    for (const std::string& d : derived) available.push_back(d);
    throw Error("not_found", f.name + " 에 없는 성분입니다: " + name, {{"param", "component"}, {"available", available}});
  }
  return derived_values(f, name);
}

}  // namespace

// ------------------------------------------------------------------ 전개된 쉘·보 결과(RES-53)
ShellExpansion shell_expansion(const App& a, const ResultFile& f) {
  ShellExpansion ex;
  const Mesh& m = a.mesh();
  if (m.node_count() == 0) return ex;
  // 모델의 2D 요소 노드와 그 법선(이웃 요소 법선의 평균), 1D 요소 노드
  std::map<Id, Vec3> normal;
  std::set<Id> line_nodes;
  std::map<Id, std::vector<Vec3>> axes;  // 보 절점 → 이어진 요소의 축 방향들
  std::map<Id, double> reach;            // 보 절점 → 이어진 요소 반 길이의 최소(단면 절점이 있을 수 있는 거리)
  for (std::size_t i = 0; i < m.element_count(); ++i) {
    const ShapeInfo& info = shape_info(m.shape_at(i));
    const Id* n = m.nodes_at(i);
    if (info.dim == 2) {
      const std::vector<int>& c = corner_positions(info.shape);
      const Vec3 p0 = m.node(n[c[0]]), p1 = m.node(n[c[1]]), p2 = m.node(n[c[2]]);
      Vec3 nn{(p1[1] - p0[1]) * (p2[2] - p0[2]) - (p1[2] - p0[2]) * (p2[1] - p0[1]), (p1[2] - p0[2]) * (p2[0] - p0[0]) - (p1[0] - p0[0]) * (p2[2] - p0[2]),
              (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p1[1] - p0[1]) * (p2[0] - p0[0])};
      const double len = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
      if (len <= 0) continue;
      for (int k = 0; k < 3; ++k) nn[k] /= len;
      for (std::size_t k = 0; k < m.node_count_at(i); ++k) {
        Vec3& acc = normal[n[k]];
        for (int q = 0; q < 3; ++q) acc[q] += nn[q];
      }
    } else if (info.dim == 1) {
      // 보·트러스: 절점마다 이어진 요소의 축 방향과 반 길이(단면 절점은 축에 수직인 면 안, 그 요소 길이의 반 안에 있다)
      const std::vector<int>& c = corner_positions(info.shape);
      const Vec3 p0 = m.node(n[c[0]]), p1 = m.node(n[c[1]]);
      Vec3 d{p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
      const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
      if (len <= 0) continue;
      for (int k = 0; k < 3; ++k) d[k] /= len;
      for (std::size_t k = 0; k < m.node_count_at(i); ++k) {
        line_nodes.insert(n[k]);
        axes[n[k]].push_back(d);
        reach[n[k]] = reach.count(n[k]) ? std::min(reach[n[k]], 0.5 * len) : 0.5 * len;
      }
    }
  }
  if (normal.empty() && line_nodes.empty()) return ex;
  for (auto& [id, nn] : normal) {
    const double len = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
    if (len > 0) for (int k = 0; k < 3; ++k) nn[k] /= len;
  }
  // 모델 크기 기준 허용 오차
  double scale = 0;
  for (double x : m.node_xyz()) scale = std::max(scale, std::fabs(x));
  const double lateral_tol = 1e-6 * (scale + 1.0);
  // 결과에만 있는 노드를 가장 가까운(옆으로) 모델 노드에 붙인다
  std::vector<std::pair<Id, Vec3>> candidates;  // 모델 노드 → 위치
  for (const auto& [id, nn] : normal) candidates.emplace_back(id, m.node(id));
  for (Id id : line_nodes) candidates.emplace_back(id, m.node(id));
  std::map<Id, std::pair<double, double>> extent;  // 모델 노드 → (최소 오프셋, 최대 오프셋)
  for (std::size_t i = 0; i < f.node_ids.size(); ++i) {
    const Id rid = f.node_ids[i];
    if (m.has_node(rid)) continue;
    const Vec3 r{f.node_xyz[3 * i], f.node_xyz[3 * i + 1], f.node_xyz[3 * i + 2]};
    Id best = 0;
    double best_lat = 1e300, best_off = 0;
    for (const auto& [mid, p] : candidates) {
      const Vec3 d{r[0] - p[0], r[1] - p[1], r[2] - p[2]};
      double off = 0, lat2 = 0;
      auto nit = normal.find(mid);
      if (nit != normal.end()) {
        off = d[0] * nit->second[0] + d[1] * nit->second[1] + d[2] * nit->second[2];
        for (int k = 0; k < 3; ++k) {
          const double l = d[k] - off * nit->second[k];
          lat2 += l * l;
        }
      } else {  // 보: 단면 절점은 모델 절점을 지나 축에 수직인 면 안에 있다 → 축 방향 거리(이어진 축 가운데 최소)로 재고, 거리는 요소 반 길이 안이어야
        const double dist2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        auto ait = axes.find(mid);
        if (ait == axes.end() || dist2 > reach[mid] * reach[mid]) continue;
        double axial = 1e300;
        for (const Vec3& ax : ait->second) axial = std::min(axial, std::fabs(d[0] * ax[0] + d[1] * ax[1] + d[2] * ax[2]));
        lat2 = axial * axial + 1e-12 * dist2;  // 축 방향 거리가 같으면 가까운 절점
        off = 0;
      }
      if (lat2 < best_lat) best_lat = lat2, best = mid, best_off = off;
    }
    if (!best || std::sqrt(best_lat) > std::max(lateral_tol, 1e-3 * (scale + 1.0))) continue;
    auto& e = extent.emplace(best, std::make_pair(best_off, best_off)).first->second;
    e.first = std::min(e.first, best_off), e.second = std::max(e.second, best_off);
    if (!normal.count(best)) ex.mid[best].push_back(rid);  // 보 단면의 노드는 구분하지 않는다
    else if (best_off > lateral_tol) ex.top[best].push_back(rid);
    else if (best_off < -lateral_tol) ex.bottom[best].push_back(rid);
    else ex.mid[best].push_back(rid);
    ++ex.expanded;
  }
  for (const auto& [id, e] : extent) ex.thickness[id] = e.second - e.first;
  return ex;
}

std::map<Id, double> shell_face_values(const App& a, const ResultFile& f, const ResultField& field, const std::string& component, const std::string& face) {
  if (face != "top" && face != "bottom" && face != "mid")
    throw Error("out_of_range", "쉘 면은 top, bottom, mid 중 하나입니다", {{"param", "shell_face"}});
  const ShellExpansion ex = shell_expansion(a, f);
  if (ex.expanded == 0) throw Error("not_available", "전개된 쉘·보 결과가 아닙니다(모델의 쉘 노드에 대응하는 결과 노드가 없음)", {{"param", "shell_face"}});
  const std::vector<double> values = component_values(field, component);
  std::map<Id, double> at;
  for (std::size_t i = 0; i < field.count; ++i) at[field.ids[i]] = values[i];
  auto mean_of = [&](const std::vector<Id>& ids, double& out) {
    double sum = 0;
    std::size_t n = 0;
    for (Id r : ids) {
      auto it = at.find(r);
      if (it != at.end()) sum += it->second, ++n;
    }
    if (!n) return false;
    out = sum / static_cast<double>(n);
    return true;
  };
  std::map<Id, double> out;
  std::set<Id> nodes;
  for (const auto& [id, v] : ex.top) nodes.insert(id);
  for (const auto& [id, v] : ex.bottom) nodes.insert(id);
  for (const auto& [id, v] : ex.mid) nodes.insert(id);
  for (Id id : nodes) {
    double v = 0;
    if (face == "top") {
      auto it = ex.top.find(id);
      if (it != ex.top.end() && mean_of(it->second, v)) out[id] = v;
    } else if (face == "bottom") {
      auto it = ex.bottom.find(id);
      if (it != ex.bottom.end() && mean_of(it->second, v)) out[id] = v;
    } else {
      auto it = ex.mid.find(id);
      if (it != ex.mid.end() && mean_of(it->second, v)) {
        out[id] = v;
      } else {
        double t = 0, b = 0;
        auto ti = ex.top.find(id), bi = ex.bottom.find(id);
        if (ti != ex.top.end() && bi != ex.bottom.end() && mean_of(ti->second, t) && mean_of(bi->second, b)) out[id] = 0.5 * (t + b);
      }
    }
  }
  return out;
}

// ------------------------------------------------------------------ 로컬 좌표계 결과(RES-58)
// 결과의 케이스에서 프레임의 스텝에 *TRANSFORM 이 걸린 노드와 그 좌표계(bc·load 의 csys). 출력이 전역(global != false)이면 비어 있다
std::map<Id, Id> local_output_nodes(const App& a, const ResultFile& f, int step) {
  std::map<Id, Id> out;
  const Object* cs = a.model().find(f.case_id);
  if (!cs) return out;
  int n = 0;
  for (const Object* st : a.model().children(cs->id, "step")) {
    if (st->suppressed) continue;
    if (++n != step) continue;
    bool local_output = false;
    for (const Object* o : a.model().children(st->id, "output_request"))
      if (!o->suppressed && o->props.contains("global") && !o->props["global"].get<bool>()) local_output = true;
    if (!local_output) return out;
    for (const char* kind : {"bc", "load"})
      for (const Object& o : step_entries(a, *st, kind)) {  // 스텝이 참조하는 하중 셋·구속 셋의 것(하중·구속은 셋 안에만 있다)
        if (!o.props.contains("csys") || !o.props["csys"].is_number() || !o.props.contains("target")) continue;
        try {
          for (const Json& nd : resolve_target(a, o.props["target"], "nodes")) out[nd.get<Id>()] = o.props["csys"].get<Id>();
        } catch (const Error&) {
        }
      }
    break;
  }
  return out;
}

// 좌표계의 노드별 축(직교: 고정, 원통: 반경·접선·축)
void csys_axes_at(const Object& cs, const Vec3& x, Vec3 ax[3]) {
  auto vec = [&](const char* key) {
    const Json& v = cs.props.at(key);
    return Vec3{v[0].get<double>(), v[1].get<double>(), v[2].get<double>()};
  };
  auto unit = [](Vec3 v) {
    const double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return Vec3{v[0] / n, v[1] / n, v[2] / n};
  };
  auto cross = [](const Vec3& a, const Vec3& b) { return Vec3{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; };
  const Vec3 o = vec("origin"), p1 = vec("axis1_point"), p2 = vec("plane12_point");
  const Vec3 e1 = unit({p1[0] - o[0], p1[1] - o[1], p1[2] - o[2]});
  Vec3 t{p2[0] - o[0], p2[1] - o[1], p2[2] - o[2]};
  const Vec3 e3 = unit(cross(e1, t)), e2 = cross(e3, e1);
  ax[0] = e1, ax[1] = e2, ax[2] = e3;
  if (cs.props.value("type", std::string("rectangular")) == "rectangular") return;
  const Vec3 d{x[0] - o[0], x[1] - o[1], x[2] - o[2]};
  const double h = d[0] * e3[0] + d[1] * e3[1] + d[2] * e3[2];
  Vec3 radial{d[0] - h * e3[0], d[1] - h * e3[1], d[2] - h * e3[2]};
  const double rn = std::sqrt(radial[0] * radial[0] + radial[1] * radial[1] + radial[2] * radial[2]);
  if (rn < 1e-12) return;
  ax[0] = {radial[0] / rn, radial[1] / rn, radial[2] / rn};
  ax[2] = e3;
  ax[1] = cross(ax[2], ax[0]);
}

// ------------------------------------------------------------------ 스텝에서 제거된 요소(RES-59)
std::set<Id> removed_elements_in_step(const App& a, Id case_id, int step) {
  std::set<Id> removed;
  const Object* cs = a.model().find(case_id);
  if (!cs || cs->kind != "case") return removed;
  int n = 0;
  for (const Object* s : a.model().children(cs->id, "step")) {
    if (s->suppressed) continue;
    ++n;
    if (n > step) break;
    for (const Object* ch : a.model().children(s->id, "step_change")) {
      if (ch->suppressed || ch->props.value("type", std::string()) != "model_change_element" || !ch->props.contains("target")) continue;
      const bool remove = ch->props.value("action", std::string()) == "remove";
      try {
        for (const Json& e : resolve_target(a, ch->props["target"], "elements")) {
          if (remove) removed.insert(e.get<Id>());
          else removed.erase(e.get<Id>());
        }
      } catch (const Error&) {
      }
    }
  }
  return removed;
}

namespace {

// 결과 프레임의 스텝에서 제거된 요소에만 속한 노드(RES-59). 케이스를 모르면 비어 있다
std::set<Id> inactive_nodes(const App& a, const ResultFile& f, int frame_index) {
  std::set<Id> out;
  if (!f.case_id) return out;
  const ResultFrame& fr = const_cast<ResultFile&>(f).frame(frame_index);
  const std::set<Id> removed = removed_elements_in_step(a, f.case_id, fr.step);
  if (removed.empty()) return out;
  const Mesh& m = a.mesh();
  std::set<Id> alive;
  for (std::size_t i = 0; i < m.element_count(); ++i) {
    const Id eid = m.element_ids()[i];
    const Id* n = m.nodes_at(i);
    const std::size_t cnt = m.node_count_at(i);
    if (removed.count(eid)) for (std::size_t k = 0; k < cnt; ++k) out.insert(n[k]);
    else for (std::size_t k = 0; k < cnt; ++k) alive.insert(n[k]);
  }
  for (Id n : alive) out.erase(n);
  return out;
}

// nodes 매개변수 → 필드 안의 위치(없으면 전부). 없는 노드면 Error.
std::vector<std::size_t> rows_of(const App& a, const ResultField& f, const Json& p) {
  std::vector<std::size_t> rows;
  const bool from_array = a.arrays() && a.arrays()->ints.count("nodes");
  if (!has(p, "nodes") && !from_array) {
    rows.resize(f.count);
    for (std::size_t i = 0; i < f.count; ++i) rows[i] = i;
    return rows;
  }
  std::map<Id, std::size_t> where;
  for (std::size_t i = 0; i < f.count; ++i) where[f.ids[i]] = i;
  auto add = [&](std::int64_t n) {
    auto it = where.find(static_cast<Id>(n));
    if (it == where.end()) throw Error("not_found", "결과에 없는 노드입니다: " + std::to_string(n), {{"param", "nodes"}, {"node", n}});
    rows.push_back(it->second);
  };
  if (from_array)
    for (std::int64_t n : a.arrays()->ints.at("nodes")) add(n);
  else
    for (const Json& n : p["nodes"]) add(n.get<std::int64_t>());
  return rows;
}

Json frame_json(const ResultFrame& fr) {
  Json fields = Json::array();
  for (const ResultField& f : fr.fields) fields.push_back(f.name);
  return Json{{"frame", fr.index}, {"step", fr.step}, {"increment", fr.increment}, {"value", fr.value}, {"type", fr.type},
              {"attributes", fr.attributes}, {"fields", fields}};
}

Json summary(App& a, const ResultFile& f) {
  Json j{{"id", f.id}, {"path", f.path}, {"case", f.case_id ? Json(f.case_id) : Json()}, {"nodes", f.node_ids.size()},
         {"elements", f.elements.size()}, {"frames", f.frames.size()}, {"complete", f.header.at("complete") == "yes"}};
  if (!f.unit_system.empty()) j["unit_system"] = f.unit_system;
  // 결과가 나온 모델과 지금 모델이 다른가(D13: 결과 작업 공간에서 "모델이 바뀜 — 재해석 필요" 표시용). 다이제스트를 모르면 null
  j["outdated"] = f.model_digest.empty() ? Json() : Json(a.digest()["total"].get<std::string>() != f.model_digest);
  return j;
}

// ------------------------------------------------------------------ dat / sta 파일
std::vector<std::string> read_lines(const fs::path& p) {
  std::vector<std::string> out;
  std::ifstream f(p, std::ios::binary);
  for (std::string line; std::getline(f, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    out.push_back(line);
  }
  return out;
}
// 숫자 행. 끝의 "L" 은 *TRANSFORM 이 걸린 노드의 국부 좌표계 값이라는 표시(솔버로 확인)라 숫자가 아니어도 행으로 본다.
std::vector<double> numbers(const std::string& line, bool* local = nullptr) {
  std::istringstream is(line);
  std::vector<double> v;
  if (local) *local = false;
  for (std::string tok; is >> tok;) {
    char* end = nullptr;
    const double x = std::strtod(tok.c_str(), &end);
    if (*end != '\0') {
      std::string more;
      if (tok == "L" && !v.empty() && !(is >> more)) {
        if (local) *local = true;
        break;
      }
      return {};
    }
    v.push_back(x);
  }
  return v;
}
// 제목 줄(글자 사이를 띄어 쓴 "E I G E N V A L U E   O U T P U T")을 붙여 읽는다.
std::string squeeze(const std::string& s) {
  std::string out;
  for (char c : s)
    if (!std::isspace(static_cast<unsigned char>(c))) out += c;
  return out;
}
fs::path sibling(const ResultFile& f, const char* ext) { return path_of(f.path).replace_extension(ext); }

// dat 에서 제목이 title 인 표들을 읽는다: 스텝마다 숫자 행의 묶음.
struct DatTable {
  int step = 0;
  std::vector<std::vector<double>> rows;
};
// prefix: 제목이 title 로 시작하면 된다(모드 번호가 붙는 "PARTICIPATION FACTORS FOR MODE n" 같은 표)
std::vector<DatTable> dat_tables(const ResultFile& f, const std::string& title, bool prefix = false) {
  std::vector<DatTable> out;
  const auto lines = read_lines(sibling(f, ".dat"));
  int step = 0;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const std::string s = squeeze(lines[i]);
    if (s.rfind("STEP", 0) == 0 && s.size() > 4 && std::isdigit(static_cast<unsigned char>(s[4]))) step = std::atoi(s.c_str() + 4);
    if (prefix ? s.rfind(title, 0) != 0 : s != title) continue;
    DatTable t;
    t.step = step;
    std::size_t k = i + 1;
    while (k < lines.size() && numbers(lines[k]).empty()) {  // 머리 줄
      const std::string h = squeeze(lines[k]);
      if (h.rfind("STEP", 0) == 0 || (h.size() > 12 && h.find("OUTPUT") != std::string::npos)) break;
      ++k;
    }
    for (; k < lines.size(); ++k) {
      const auto v = numbers(lines[k]);
      if (v.empty()) break;
      t.rows.push_back(v);
    }
    out.push_back(std::move(t));
    i = k;
  }
  return out;
}

// dat 의 "값 for set X and time t" 표 전부: {title, quantity, set, time, step, columns, rows}
Json dat_print_tables(const ResultFile& f) {
  Json out = Json::array();
  const auto lines = read_lines(sibling(f, ".dat"));
  int step = 0;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const std::string s = squeeze(lines[i]);
    if (s.rfind("STEP", 0) == 0 && s.size() > 4 && std::isdigit(static_cast<unsigned char>(s[4]))) step = std::atoi(s.c_str() + 4);
    const std::string line = lines[i];
    const std::size_t at = line.find(" for set ");
    if (at == std::string::npos || line.empty() || std::isspace(static_cast<unsigned char>(line[0])) == 0) continue;
    const std::string title = line.substr(0, at);
    std::string rest = line.substr(at + 9);
    const std::size_t t = rest.find(" and time");
    const std::string set = t == std::string::npos ? rest : rest.substr(0, t);
    double time = 0;
    if (t != std::string::npos) time = std::strtod(rest.substr(t + 9).c_str(), nullptr);
    std::string quantity = title, columns;
    const std::size_t paren = title.find('(');
    if (paren != std::string::npos) {
      quantity = title.substr(0, paren);
      columns = title.substr(paren + 1, title.find(')', paren) - paren - 1);
    }
    auto trim = [](std::string v) {
      const std::size_t b = v.find_first_not_of(" "), e = v.find_last_not_of(" ");
      return b == std::string::npos ? std::string() : v.substr(b, e - b + 1);
    };
    Json cols = Json::array();
    std::istringstream cs(columns);
    for (std::string c; std::getline(cs, c, ',');) cols.push_back(trim(c));
    Json rows = Json::array(), local_rows = Json::array();
    std::size_t k = i + 1;
    while (k < lines.size() && numbers(lines[k]).empty() && squeeze(lines[k]).empty()) ++k;  // 빈 줄
    for (; k < lines.size(); ++k) {
      bool local = false;
      const auto v = numbers(lines[k], &local);
      if (v.empty()) break;
      if (local) local_rows.push_back(rows.size());
      rows.push_back(v);
    }
    Json table{{"title", trim(title)}, {"quantity", trim(quantity)}, {"columns", cols}, {"set", trim(set)}, {"time", time},
               {"step", step}, {"rows", rows}};
    if (!local_rows.empty()) table["local_rows"] = local_rows;  // 국부 좌표계(*TRANSFORM)로 적힌 행의 번호
    out.push_back(std::move(table));
    i = k > i ? k - 1 : i;
  }
  return out;
}

// *CONTACT PRINT, TOTALS=YES 의 "statistics for slave set S, master set M and time t" 블록(매뉴얼 7.26):
// 총 면력·모멘트, 무게중심·평균 법선, 무게중심 기준 모멘트, 면적·법선력·전단력을 표 네 개로 만든다.
Json dat_contact_statistics(const ResultFile& f) {
  Json out = Json::array();
  const auto lines = read_lines(sibling(f, ".dat"));
  int step = 0;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const std::string s = squeeze(lines[i]);
    if (s.rfind("STEP", 0) == 0 && s.size() > 4 && std::isdigit(static_cast<unsigned char>(s[4]))) step = std::atoi(s.c_str() + 4);
    const std::string& line = lines[i];
    // 접촉: "statistics for slave set S, master set M and time t" / 단면(*SECTION PRINT): "statistics for surface set S and time t"
    const std::size_t at_c = line.find("statistics for slave set "), at_s = line.find("statistics for surface set ");
    if (at_c == std::string::npos && at_s == std::string::npos) continue;
    const bool contact = at_c != std::string::npos;
    std::string rest = contact ? line.substr(at_c + 25) : line.substr(at_s + 27);
    const std::size_t m = rest.find(", master set "), t = rest.find(" and time");
    const std::string slave = rest.substr(0, std::min(m == std::string::npos ? rest.size() : m, t == std::string::npos ? rest.size() : t));
    const std::string master = m == std::string::npos ? std::string() : rest.substr(m + 13, (t == std::string::npos ? rest.size() : t) - m - 13);
    const double time = t == std::string::npos ? 0.0 : std::strtod(rest.substr(t + 9).c_str(), nullptr);
    const std::string prefix = contact ? "contact " : "section ";
    // 블록 제목은 빈칸을 뺀 앞부분으로 맞춘다(접촉은 "and shear force (size)" 까지, 단면은 "torque and bending moment" 가 더 붙는다)
    static const char* heads[] = {"totalsurfaceforce", "centerofgravity", "momentaboutthecenterofgravity", "area,normalforce"};
    static const char* names[] = {"total force", "center", "moment", "area force"};
    static const std::vector<std::vector<std::string>> cols_c = {{"fx", "fy", "fz", "mx", "my", "mz"}, {"x", "y", "z", "nx", "ny", "nz"}, {"mx", "my", "mz"}, {"area", "normal_force", "shear_force"}};
    static const std::vector<std::vector<std::string>> cols_s = {{"fx", "fy", "fz", "mx", "my", "mz"}, {"x", "y", "z", "nx", "ny", "nz"}, {"mx", "my", "mz"}, {"area", "normal_force", "shear_force", "torque", "bending_moment"}};
    std::size_t k = i + 1;
    for (std::size_t b = 0; b < 4 && k < lines.size(); ++b) {
      while (k < lines.size() && squeeze(lines[k]).rfind(heads[b], 0) != 0) {
        if (lines[k].find("statistics for ") != std::string::npos || squeeze(lines[k]).rfind("STEP", 0) == 0) break;
        ++k;
      }
      if (k >= lines.size() || squeeze(lines[k]).rfind(heads[b], 0) != 0) break;
      const std::string title = lines[k];
      ++k;
      while (k < lines.size() && numbers(lines[k]).empty()) ++k;
      if (k >= lines.size()) break;
      const std::vector<double> v = numbers(lines[k]);
      Json cols = contact ? cols_c[b] : cols_s[b];
      if (static_cast<std::size_t>(cols.size()) != v.size()) {  // 열량·항력 단면처럼 열이 다르면 값 개수에 맞춘다
        cols = Json::array();
        for (std::size_t c = 0; c < v.size(); ++c) cols.push_back("c" + std::to_string(c + 1));
      }
      Json table{{"title", title.substr(title.find_first_not_of(' '))}, {"quantity", prefix + names[b]}, {"columns", cols}, {"set", slave},
                 {"time", time}, {"step", step}, {"rows", Json::array({v})}};
      if (contact) table["master"] = master;
      out.push_back(std::move(table));
      ++k;
    }
  }
  return out;
}

Json filtered_tables(const ResultFile& f, const std::function<bool(const std::string&)>& want) {
  Json out = Json::array();
  for (const Json& t : dat_print_tables(f))
    if (want(t["quantity"].get<std::string>())) out.push_back(t);
  for (const Json& t : dat_contact_statistics(f))
    if (want(t["quantity"].get<std::string>())) out.push_back(t);
  return out;
}

}  // namespace

void register_result_commands(App& app) {
  {
    CommandSpec c = base("result.print_tables", 'Q', "dat 파일의 표(셋 단위 절점·적분점 값, 합계 등)를 조회한다. quantity 로 거른다", "RES-28");
    c.params = {id_param(), F("quantity", "string", "표의 종류(예: forces, total force, stresses). 없으면 전부").ex("total force"),
                F("set", "string", "이 셋의 표만")};
    c.fn = [](App& a, const Json& p) {
      const ResultFile& f = file_of(a, p);
      Json out = Json::array();
      for (const Json& t : dat_print_tables(f)) {
        if (has(p, "quantity") && t["quantity"] != p["quantity"]) continue;
        if (has(p, "set") && t["set"] != p["set"]) continue;
        out.push_back(t);
      }
      return out;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("result.totals", 'Q', "합계 표(합력·에너지·체적·질량 등 total·energy·volume·mass 로 시작하는 것)를 조회한다", "RES-37");
    c.params = {id_param()};
    c.fn = [](App& a, const Json& p) {
      return filtered_tables(file_of(a, p), [](const std::string& q) {
        return q.rfind("total", 0) == 0 || q.find("energy") != std::string::npos || q.rfind("volume", 0) == 0 || q.rfind("mass", 0) == 0 ||
               q.find("centre of gravity") != std::string::npos || q.find("moments of inertia") != std::string::npos;
      });
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("result.section_output", 'Q', "단면력·열량·항력 표(*SECTION PRINT)를 조회한다", "RES-38");
    c.params = {id_param()};
    c.fn = [](App& a, const Json& p) {
      return filtered_tables(file_of(a, p), [](const std::string& q) {
        return q.find("section") != std::string::npos || q.find("drag") != std::string::npos || q.find("flux") != std::string::npos;
      });
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("result.contact_summary", 'Q', "접촉력·접촉 상태 표(*CONTACT PRINT)를 조회한다", "RES-35");
    c.params = {id_param()};
    c.fn = [](App& a, const Json& p) {
      return filtered_tables(file_of(a, p), [](const std::string& q) { return q.find("contact") != std::string::npos; });
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("result.open", 'J', "결과 파일(frd)을 연다. 목록만 읽고 값은 조회할 때 읽는다", "RES-01, RES-03, RES-27");
    c.params = {F("path", "string", "결과 파일 경로(.frd)").ex("job.frd"),
                F("case", "ref", "이 케이스를 마지막으로 실행한 결과를 연다").ref("case"),
                F("file", "ref", "프로젝트의 결과 파일 객체(경로 또는 케이스를 그 객체에서 가져온다)").ref("result_file"),
                F("unit_system", "string", "결과 파일의 단위계(모델과 다르면 좌표·변위·응력·힘 등을 모델 단위계로 환산해 준다, CMN-13)")
                    .one_of({"mm-t-s", "m-kg-s", "mm-kg-ms", "cm-g-s", "in-lbf-s"}).ex("m-kg-s")};
    c.fn = [](App& a, const Json& p) {
      std::string path, run_digest;
      Id case_id = 0, file_id = 0;
      Json q = p;
      if (has(p, "file")) {
        const Object& rf = a.model().get(p["file"].get<Id>());
        if (rf.kind != "result_file") throw Error("wrong_kind", "결과 파일 객체가 아닙니다", {{"object", rf.id}, {"expected", "result_file"}});
        file_id = rf.id;
        if (has(rf.props, "path") && !rf.props["path"].get<std::string>().empty()) q["path"] = rf.props["path"];
        else if (has(rf.props, "case")) q["case"] = rf.props["case"];
        else throw Error("invalid_state", "결과 파일 객체에 경로도 케이스도 없습니다", {{"object", rf.id}});
      }
      if (has(q, "path")) {
        path = q["path"].get<std::string>();
      } else if (has(q, "case")) {
        case_id = q["case"].get<Id>();
        const Json st = a.commands().at("case.run_status").fn(a, Json{{"id", case_id}});
        if (!st.contains("files") || !st["files"].contains("frd"))
          throw Error("invalid_state", "이 케이스에는 결과 파일이 없습니다(실행하지 않았거나 결과 출력 요청이 없음)", {{"object", case_id}});
        path = st["files"]["frd"]["path"].get<std::string>();
        run_digest = st.value("model_digest", std::string());
      } else {
        throw Error("missing_param", "path, case 또는 file 이 필요합니다", {{"param", "path"}});
      }
      auto f = results(a).open(path);
      f->case_id = case_id, f->file_id = file_id;
      // 결과가 나온 모델의 다이제스트: 케이스 실행 때 것. 경로로 연 외부 파일은 지금 모델 것(이후 모델이 바뀌면 outdated)
      f->model_digest = run_digest.empty() ? a.digest()["total"].get<std::string>() : run_digest;
      if (has(p, "unit_system")) {
        check_value(a.commands().at("result.open").params[3], p["unit_system"], nullptr);
        const Object* st = a.find_settings();
        const std::string model = st ? st->props.value("unit_system", std::string("mm-t-s")) : std::string("mm-t-s");
        f->unit_system = p["unit_system"].get<std::string>();
        if (f->unit_system != model) {
          for (const char* dim : {"length", "pressure", "force", "power", "heat_flux", "velocity", "temperature"})
            f->unit_factors[dim] = unit_factor(f->unit_system, model, dim);
          const double L = f->unit_factors["length"];
          for (double& v : f->node_xyz) v *= L;
        }
      }
      return summary(a, *f);
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("result.unload", 'S', "올라와 있는 결과 값을 메모리에서 내린다(frame 을 주면 그 프레임만). 다시 조회하면 파일에서 읽는다", "RES-03");
    c.params = {id_param(), F("frame", "integer", "프레임 번호(없으면 전부)").ge(1)};
    c.fn = [](App& a, const Json& p) {
      ResultFile& f = file_of(a, p);
      std::size_t n = 0;
      for (ResultFrame& fr : f.frames) {
        if (has(p, "frame") && fr.index != p["frame"].get<int>()) continue;
        for (ResultField& fld : fr.fields)
          if (fld.loaded) {
            fld.loaded = false;
            std::vector<double>().swap(fld.data);
            std::vector<Id>().swap(fld.ids);
            ++n;
          }
      }
      return Json{{"unloaded", n}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("result.reload", 'J', "결과 파일을 다시 읽는다(실행 중 갱신된 결과)", "RES-30");
    c.params = {id_param()};
    c.fn = [](App& a, const Json& p) {
      ResultFile& old = file_of(a, p);
      const Id id = old.id, case_id = old.case_id;
      auto fresh = results(a).open(old.path);  // 새 번호로 열린다 → 원래 번호의 내용을 바꾼다
      const Id temp = fresh->id;
      ResultFile copy = *fresh;
      copy.id = id, copy.case_id = case_id;
      results(a).close(temp);
      results(a).get(id) = copy;
      return summary(a, results(a).get(id));
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("result.close", 'S', "결과 파일을 닫는다", "RES-01");
    c.params = {id_param()};
    c.fn = [](App& a, const Json& p) {
      results(a).close(p["result"].get<Id>());
      return Json::object();
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("result.info", 'Q', "열린 결과의 요약(경로·케이스·노드·요소·프레임 수·완료 여부)과 outdated(결과가 나온 뒤 모델·메시가 바뀌었는가 — 결과 작업 공간의 '재해석 필요' 표시용)를 조회한다", "RES-01, WT-32");
    c.params = {id_param()};
    c.fn = [](App& a, const Json& p) { return summary(a, file_of(a, p)); };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("result.list", 'Q', "열린 결과 전부의 요약 목록(result.info 와 같은 항목)", "RES-01, WT-32");
    c.fn = [](App& a, const Json&) {
      Json arr = Json::array();
      for (const auto& [id, f] : results(a).files()) arr.push_back(summary(a, *f));
      return arr;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("result.steps", 'Q', "프레임(스텝·증분·시간 또는 진동수) 목록을 조회한다", "RES-02, RES-03");
    c.params = {id_param()};
    c.fn = [](App& a, const Json& p) {
      Json arr = Json::array();
      for (const ResultFrame& fr : file_of(a, p).frames) arr.push_back(frame_json(fr));
      return arr;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("result.fields", 'Q', "프레임에서 쓸 수 있는 결과 종류·성분·파생량·위치를 조회한다", "RES-02, RES-33~45, RES-52");
    c.params = {id_param(), frame_param()};
    c.fn = [](App& a, const Json& p) {
      // 유사 해석 표기(RES-45): 결과의 케이스가 열전달 유사 분야면 온도·열유속 결과에 그 분야의 이름을 붙인다
      Json labels;
      ResultFile& file = file_of(a, p);
      if (file.case_id && a.commands().count("case.physics_labels")) {
        try {
          const Json l = a.commands().at("case.physics_labels").fn(a, Json{{"id", file.case_id}});
          if (l.value("thermal_analogy", false) && l.value("physics", std::string()) != "thermal" && l.value("physics", std::string()) != "thermo_mechanical") labels = l;
        } catch (const Error&) {
        }
      }
      Json arr = Json::array();
      for (const ResultField& f : file.frame(p["frame"].get<int>()).fields) {
        Json j{{"name", f.name}, {"components", f.components}, {"derived", derived_names(f)},
               {"kind", f.entity == 1 ? "scalar" : f.entity == 2 ? "vector" : "tensor"}, {"location", f.location}, {"count", f.count}};
        if (!labels.is_null()) {
          if (f.name == "NDTEMP") j["label"] = labels["temperature"], j["physics"] = labels["physics"];
          else if (f.name == "FLUX") j["label"] = labels["flux"], j["physics"] = labels["physics"];
          else if (f.name == "RFL") j["label"] = labels["heat_source"], j["physics"] = labels["physics"];
        }
        // frd 블록 이름 → 요청 키·뜻(매뉴얼 *NODE FILE·*EL FILE 의 [] 표기). 3D 유체(RES-40)·전자기(RES-42)·민감도(RES-43)·균열(RES-44)
        static const std::map<std::string, std::pair<const char*, const char*>> kBlocks = {
            {"V3DF", {"VF", "3D 유체 속도"}},        {"PS3DF", {"PSF", "3D 유체 정압"}},     {"PT3DF", {"PTF", "3D 유체 전압"}},
            {"TS3DF", {"TSF", "3D 유체 정온도"}},    {"TT3DF", {"TTF", "3D 유체 전온도"}},   {"M3DF", {"MACH", "마하수"}},
            {"CP3DF", {"CP", "압력 계수"}},          {"TURB3DF", {"TURB", "난류량(ρk, ρω, νt, y+, u+)"}}, {"VSTRES", {"SVF", "점성 응력"}},
            {"DTIMF", {"DTF", "유체 시간 증분"}},    {"DEPTH", {"DEPT", "수심"}},            {"HCRIT", {"HCRI", "임계 수심"}},
            {"MAFLOW", {"MF", "질량 유량"}},         {"STPRES", {"PS", "네트워크 정압"}},    {"TOPRES", {"PT", "네트워크 전압"}},
            {"STTEMP", {"TS", "네트워크 정온도"}},   {"TOTEMP", {"TT", "네트워크 전온도"}},
            {"ELPOT", {"POT", "전위"}},              {"CURR", {"ECD", "전류 밀도"}},         {"EMFE", {"EMFE", "전기장"}},
            {"EMFB", {"EMFB", "자기장"}},
            {"SEN", {"SEN", "민감도"}},              {"CT3D-MIS", {"KEQ", "균열: 등가 응력확대계수 등"}},
            {"ENER", {"ENER", "에너지 밀도"}},       {"PE", {"PEEQ", "등가 소성 변형률"}},
        };
        if (auto it = kBlocks.find(f.name); it != kBlocks.end()) {
          j["request"] = it->second.first;
          if (!j.contains("label")) j["label"] = it->second.second;
        } else if (f.name.rfind("SEN", 0) == 0) {  // SENENER·SENMASS 등 민감도 블록(응답 종류가 뒤에 붙는다)
          j["request"] = "SEN", j["label"] = "민감도(" + f.name.substr(3) + ")";
        }
        arr.push_back(std::move(j));
      }
      return arr;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("result.values", 'Q',
                         "결과 값을 조회한다(성분을 주면 그 성분만, 없으면 모든 성분). shell_face(top|bottom|mid)를 주면 전개된 쉘 결과를 모델 쉘 노드 기준 그 면의 값으로 준다(RES-54·RES-08). "
                         "결과의 케이스에서 그 스텝에 제거된 요소에만 속한 노드는 빠진다(include_inactive 로 포함, RES-59)",
                         "RES-12, RES-08, RES-54, RES-59, API-12, API-32");
    c.params = {id_param(), frame_param(), field_param(),
                F("component", "string", "성분 또는 파생량(예: D3, SXX, magnitude, mises)").ex("magnitude"), nodes_param(),
                F("shell_face", "string", "전개된 쉘 결과의 면").one_of({"top", "bottom", "mid"}),
                F("include_inactive", "bool", "스텝에서 제거된 요소의 노드도 포함한다"),
                F("coordinates", "string", "global 이면 로컬 좌표계로 저장된(GLOBAL=NO) 노드의 벡터 성분을 전역으로 돌려 준다(RES-58). 없으면 파일 그대로").one_of({"file", "global"})};
    c.fn = [](App& a, const Json& p) {
      ResultFile* file = nullptr;
      const ResultField& f = field_of(a, p, &file);
      // 로컬 좌표계 출력의 전역 변환(RES-58): 벡터(3성분) 결과만. 성분을 하나만 달라 해도 전역으로 돌린 뒤 고른다
      std::map<Id, Id> local;
      if (p.value("coordinates", std::string("file")) == "global" && file->case_id && f.components.size() == 3)
        local = local_output_nodes(a, *file, file->frame(p["frame"].get<int>()).step);
      auto to_global = [&](std::size_t r, double v[3]) {
        auto it = local.find(f.ids[r]);
        if (it == local.end()) return;
        const Object* cs = a.model().find(it->second);
        if (!cs) return;
        Vec3 ax[3];
        std::size_t at = 0;
        for (std::size_t i = 0; i < file->node_ids.size(); ++i) if (file->node_ids[i] == f.ids[r]) at = i;
        csys_axes_at(*cs, Vec3{file->node_xyz[3 * at], file->node_xyz[3 * at + 1], file->node_xyz[3 * at + 2]}, ax);
        const double l[3] = {v[0], v[1], v[2]};
        for (int k = 0; k < 3; ++k) v[k] = ax[0][k] * l[0] + ax[1][k] * l[1] + ax[2][k] * l[2];  // 전역 = Σ 국부 성분 × 축
      };
      if (has(p, "shell_face")) {
        if (!has(p, "component")) throw Error("missing_param", "shell_face 에는 component 가 필요합니다", {{"param", "component"}});
        const std::map<Id, double> vals = shell_face_values(a, *file, f, p["component"].get<std::string>(), p["shell_face"].get<std::string>());
        Json ids = Json::array(), values = Json::array();
        std::set<Id> only;
        if (has(p, "nodes")) for (const Json& n : p["nodes"]) only.insert(n.get<Id>());
        for (const auto& [id, v] : vals) {
          if (!only.empty() && !only.count(id)) continue;
          ids.push_back(id), values.push_back(v);
        }
        return Json{{"ids", ids}, {"component", p["component"]}, {"values", values}, {"shell_face", p["shell_face"]}};
      }
      auto rows = rows_of(a, f, p);
      if (!p.value("include_inactive", false)) {
        const std::set<Id> off = inactive_nodes(a, *file, p["frame"].get<int>());
        if (!off.empty()) rows.erase(std::remove_if(rows.begin(), rows.end(), [&](std::size_t r) { return off.count(f.ids[r]) > 0; }), rows.end());
      }
      Json ids = Json::array();
      for (std::size_t r : rows) ids.push_back(f.ids[r]);
      const std::size_t nc = f.components.size();
      if (has(p, "component")) {
        const std::string comp = p["component"].get<std::string>();
        Json values = Json::array();
        if (!local.empty()) {  // 전역으로 돌린 벡터에서 성분·크기를 고른다
          const auto cit = std::find(f.components.begin(), f.components.end(), comp);
          for (std::size_t r : rows) {
            double v[3] = {f.data[r * 3], f.data[r * 3 + 1], f.data[r * 3 + 2]};
            to_global(r, v);
            if (cit != f.components.end()) values.push_back(v[static_cast<std::size_t>(cit - f.components.begin())]);
            else if (comp == "magnitude") values.push_back(std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
            else component_values(f, comp);  // 없는 성분이면 여기서 Error
          }
        } else {
          const std::vector<double> all = component_values(f, comp);
          for (std::size_t r : rows) values.push_back(all[r]);
        }
        Json out{{"ids", ids}, {"component", comp}, {"values", values}};
        if (!local.empty()) out["coordinates"] = "global", out["transformed_nodes"] = local.size();
        return out;
      }
      Json values = Json::array();
      for (std::size_t r : rows) {
        Json row = Json::array();
        if (!local.empty()) {
          double v[3] = {f.data[r * 3], f.data[r * 3 + 1], f.data[r * 3 + 2]};
          to_global(r, v);
          for (int k = 0; k < 3; ++k) row.push_back(v[k]);
        } else {
          for (std::size_t k = 0; k < nc; ++k) row.push_back(f.data[r * nc + k]);
        }
        values.push_back(std::move(row));
      }
      Json out{{"ids", ids}, {"components", f.components}, {"values", values}};
      if (!local.empty()) out["coordinates"] = "global", out["transformed_nodes"] = local.size();
      return out;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("result.probe", 'Q', "지정 노드의 값을 성분·파생량과 함께 조회한다", "RES-12");
    c.params = {id_param(), frame_param(), field_param(), nodes_param()};
    c.params.back().must = true;
    c.fn = [](App& a, const Json& p) {
      const ResultField& f = field_of(a, p);
      const std::size_t nc = f.components.size();
      std::map<std::string, std::vector<double>> derived;
      for (const std::string& d : derived_names(f)) derived[d] = derived_values(f, d);
      Json arr = Json::array();
      for (std::size_t r : rows_of(a, f, p)) {
        Json row{{"node", f.ids[r]}};
        for (std::size_t k = 0; k < nc; ++k) row[f.components[k]] = f.data[r * nc + k];
        for (const auto& [name, v] : derived) row[name] = v[r];
        arr.push_back(std::move(row));
      }
      return arr;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("result.minmax", 'Q', "최대·최소 값과 그 노드를 조회한다", "RES-13");
    c.params = {id_param(), frame_param(), field_param(), F("component", "string", "성분 또는 파생량").call_req().ex("mises"), nodes_param()};
    c.fn = [](App& a, const Json& p) {
      const ResultField& f = field_of(a, p);
      const std::vector<double> v = component_values(f, p["component"].get<std::string>());
      const auto rows = rows_of(a, f, p);
      if (rows.empty()) throw Error("invalid_state", "대상 노드가 없습니다", {{"param", "nodes"}});
      std::size_t lo = rows[0], hi = rows[0];
      for (std::size_t r : rows) {
        if (v[r] < v[lo]) lo = r;
        if (v[r] > v[hi]) hi = r;
      }
      return Json{{"min", {{"value", v[lo]}, {"node", f.ids[lo]}}}, {"max", {{"value", v[hi]}, {"node", f.ids[hi]}}}, {"count", rows.size()}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("result.derived_scalar", 'Q', "파생량(von Mises·주응력·크기 등)의 목록을 조회하고, 이름을 주면 값을 조회한다", "RES-06");
    c.params = {id_param(), frame_param(), field_param(), F("name", "string", "파생량 이름").ex("mises"), nodes_param()};
    c.fn = [](App& a, const Json& p) {
      const ResultField& f = field_of(a, p);
      Json out{{"available", derived_names(f)}};
      if (has(p, "name")) {
        const std::vector<double> all = derived_values(f, p["name"].get<std::string>());
        Json ids = Json::array(), values = Json::array();
        for (std::size_t r : rows_of(a, f, p)) ids.push_back(f.ids[r]), values.push_back(all[r]);
        out["name"] = p["name"], out["ids"] = ids, out["values"] = values;
      }
      return out;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("result.reaction_sum", 'Q', "절점력(반력) 결과의 합력과 합모멘트를 조회한다", "RES-22");
    c.params = {id_param(), frame_param(), nodes_param(),
                F("point", "vector3", "모멘트의 기준점(없으면 원점)").unit("length"),
                F("field", "string", "절점력 결과의 이름(기본 FORC)")};
    c.fn = [](App& a, const Json& p) {
      ResultFile& file = file_of(a, p);
      ResultField& f = file.field(file.frame(p["frame"].get<int>()), p.value("field", std::string("FORC")));
      if (f.components.size() != 3) throw Error("invalid_state", "벡터 결과가 아닙니다: " + f.name, {{"param", "field"}});
      std::map<Id, std::size_t> node_at;
      for (std::size_t i = 0; i < file.node_ids.size(); ++i) node_at[file.node_ids[i]] = i;
      const Json o = has(p, "point") ? p["point"] : Json::array({0.0, 0.0, 0.0});
      double fsum[3] = {0, 0, 0}, msum[3] = {0, 0, 0};
      const auto rows = rows_of(a, f, p);
      for (std::size_t r : rows) {
        const double* v = &f.data[r * 3];
        const std::size_t n = node_at.at(f.ids[r]);
        const double x = file.node_xyz[3 * n] - o[0].get<double>(), y = file.node_xyz[3 * n + 1] - o[1].get<double>(),
                     z = file.node_xyz[3 * n + 2] - o[2].get<double>();
        for (int k = 0; k < 3; ++k) fsum[k] += v[k];
        msum[0] += y * v[2] - z * v[1], msum[1] += z * v[0] - x * v[2], msum[2] += x * v[1] - y * v[0];
      }
      return Json{{"force", {fsum[0], fsum[1], fsum[2]}}, {"moment", {msum[0], msum[1], msum[2]}}, {"count", rows.size()}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("result.history", 'Q', "한 노드의 값이 프레임에 따라 변하는 이력을 조회한다", "RES-17, RES-49");
    c.params = {id_param(), field_param(), F("component", "string", "성분 또는 파생량").call_req().ex("D3"),
                F("node", "integer", "노드").call_req().ge(1).ex(1), F("step", "integer", "이 스텝의 프레임만").ge(1)};
    c.fn = [](App& a, const Json& p) {
      ResultFile& file = file_of(a, p);
      const std::string name = p["field"].get<std::string>(), comp = p["component"].get<std::string>();
      const Id node = p["node"].get<Id>();
      Json arr = Json::array();
      for (ResultFrame& fr : file.frames) {
        if (has(p, "step") && fr.step != p["step"].get<int>()) continue;
        if (std::none_of(fr.fields.begin(), fr.fields.end(), [&](const ResultField& f) { return f.name == name; })) continue;
        const ResultField& f = file.field(fr, name);
        auto it = std::find(f.ids.begin(), f.ids.end(), node);
        if (it == f.ids.end()) throw Error("not_found", "결과에 없는 노드입니다: " + std::to_string(node), {{"param", "node"}});
        const std::size_t row = static_cast<std::size_t>(it - f.ids.begin());
        arr.push_back(Json{{"frame", fr.index}, {"step", fr.step}, {"increment", fr.increment}, {"x", fr.value},
                           {"y", component_values(f, comp)[row]}});
      }
      if (arr.empty()) throw Error("not_found", "이 결과가 든 프레임이 없습니다: " + name, {{"param", "field"}});
      return arr;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("result.modal_summary", 'Q', "고유진동수·참여계수·유효 모드 질량을 조회한다(dat 파일)", "RES-23, RES-51");
    c.params = {id_param()};
    c.fn = [](App& a, const Json& p) {
      const ResultFile& f = file_of(a, p);
      Json out = Json::array();
      const auto eig = dat_tables(f, "EIGENVALUEOUTPUT");
      const auto part = dat_tables(f, "PARTICIPATIONFACTORS");
      const auto mass = dat_tables(f, "EFFECTIVEMODALMASS");
      // 순환대칭(RES-51): 절직경마다 표가 따로 나오고 행이 [절직경, 모드, 고유치, ω, f, 허수부] 여섯 개다(솔버로 확인). 뒤에 "TURNING DIRECTION" 표(F/B)가 붙는다
      const std::vector<std::string> lines = read_lines(sibling(f, ".dat"));
      std::map<int, std::vector<std::pair<std::pair<int, int>, std::string>>> turning;  // 스텝 → [(절직경, 모드), F|B]
      {
        int step = 0;
        for (std::size_t i = 0; i < lines.size(); ++i) {
          const std::string sq = squeeze(lines[i]);
          if (sq.rfind("STEP", 0) == 0 && sq.size() > 4 && std::isdigit(static_cast<unsigned char>(sq[4]))) step = std::atoi(sq.c_str() + 4);
          if (sq.find("TURNINGDIRECTION") == std::string::npos) continue;
          bool seen = false;
          for (std::size_t k = i + 1; k < lines.size(); ++k) {
            std::istringstream is(lines[k]);
            int nd = 0, mode = 0;
            std::string dir;
            if (!(is >> nd >> mode >> dir)) {
              if (!seen && (squeeze(lines[k]).empty() || squeeze(lines[k]) == "DIAMETER")) continue;  // 머리 줄
              break;
            }
            seen = true;
            turning[step].push_back({{nd, mode}, dir});
          }
        }
      }
      std::map<int, Json> by_step;  // 스텝 → 모드 목록(절직경 표를 합친다)
      std::vector<int> order;
      for (std::size_t t = 0; t < eig.size(); ++t) {
        Json& modes = by_step[eig[t].step];
        if (modes.is_null()) modes = Json::array(), order.push_back(eig[t].step);
        for (std::size_t i = 0; i < eig[t].rows.size(); ++i) {
          const auto& r = eig[t].rows[i];
          if (r.size() < 5) continue;  // 복소 고유치 표(값 3개)는 result.complex_summary 에서
          const bool cyclic = r.size() >= 6;
          const std::size_t o = cyclic ? 1 : 0;
          Json m{{"mode", static_cast<int>(r[o])}, {"eigenvalue", r[o + 1]}, {"omega", r[o + 2]}, {"frequency", r[o + 3]}};
          if (cyclic) {
            m["nodal_diameter"] = static_cast<int>(r[0]);
            for (const auto& [key, dir] : turning[eig[t].step])
              if (key.first == static_cast<int>(r[0]) && key.second == static_cast<int>(r[1])) m["turning_direction"] = dir == "F" ? "forward" : dir == "B" ? "backward" : dir;
          }
          if (t < part.size() && i < part[t].rows.size() && part[t].rows[i].size() >= 7)
            m["participation"] = std::vector<double>(part[t].rows[i].begin() + 1, part[t].rows[i].begin() + 7);
          if (t < mass.size() && i < mass[t].rows.size() && mass[t].rows[i].size() >= 7)
            m["effective_mass"] = std::vector<double>(mass[t].rows[i].begin() + 1, mass[t].rows[i].begin() + 7);
          modes.push_back(std::move(m));
        }
      }
      for (int st : order) {
        Json entry{{"step", st}, {"modes", by_step[st]}};
        bool cyclic = false;
        for (const Json& m : by_step[st]) if (m.contains("nodal_diameter")) cyclic = true;
        if (cyclic) entry["cyclic_symmetry"] = true;
        out.push_back(std::move(entry));
      }
      return out;
    };
    app.register_command(std::move(c));
  }
  {
    // 복소 고유치(RES-61): *COMPLEX FREQUENCY 의 dat 표 — 솔버로 확인(ccx 2.22): 열이 [모드, 실수부(rad/time), 진동수(cycles/time), 허수부(rad/time)] 셋이다
    // (일반 *FREQUENCY 는 [모드, 고유치, ω, f, 허수부] 넷). 참여계수 표는 모드마다 따로 나온다("FOR MODE n", [모드, 진동수, 실수, 허수]).
    CommandSpec c = base("result.complex_summary", 'Q', "복소 고유치(*COMPLEX FREQUENCY: 실수부·허수부·진동수)와 모드별 복소 참여계수를 조회한다(dat 파일)", "RES-61");
    c.params = {id_param()};
    c.fn = [](App& a, const Json& p) {
      const ResultFile& f = file_of(a, p);
      Json out = Json::array();
      const auto eig = dat_tables(f, "EIGENVALUEOUTPUT");
      const auto part = dat_tables(f, "PARTICIPATIONFACTORSFORMODE", true);
      std::size_t pi = 0;
      for (const DatTable& t : eig) {
        if (t.rows.empty() || t.rows[0].size() != 4) continue;  // 복소 표만(모드 + 값 3개)
        Json modes = Json::array();
        for (const auto& r : t.rows) {
          if (r.size() != 4) continue;
          // 안정·감쇠 판정은 두지 않는다: 허수부의 부호 규약을 매뉴얼에서 확인하지 못했다(값은 그대로 돌려준다)
          Json m{{"mode", static_cast<int>(r[0])}, {"omega_real", r[1]}, {"frequency", r[2]}, {"omega_imag", r[3]}};
          // 이 모드의 참여계수 표(실수·허수)
          if (pi < part.size() && part[pi].step == t.step) {
            Json rows = Json::array();
            for (const auto& pr : part[pi].rows)
              if (pr.size() >= 4) rows.push_back(Json{{"mode", static_cast<int>(pr[0])}, {"frequency", pr[1]}, {"real", pr[2]}, {"imag", pr[3]}});
            m["participation"] = rows;
            ++pi;
          }
          modes.push_back(std::move(m));
        }
        out.push_back(Json{{"step", t.step}, {"modes", modes}});
      }
      return out;
    };
    app.register_command(std::move(c));
  }
  {
    // 복소 결과의 위상각 값(RES-46, RES-47): 정상상태 동해석 등은 실수부(DISP)와 허수부(DISPI)를 따로 둔다(솔버로 확인).
    // u(t) = Re[(uR + i uI) e^{iωt}] 이므로 위상각 θ 에서 uR cosθ − uI sinθ. 파생량(크기·mises)은 합친 성분으로 계산한다
    CommandSpec c = base("result.at_phase", 'Q', "복소 결과(실수부 FIELD + 허수부 FIELDI)의 지정 위상각(도)에서의 성분 값(또는 파생량)을 조회한다", "RES-46, RES-47");
    c.params = {id_param(), frame_param(), field_param(), F("component", "string", "성분 또는 파생량").call_req().ex("D3"),
                F("phase", "number", "위상각(도)").call_req().ex(90.0), nodes_param()};
    c.fn = [](App& a, const Json& p) {
      ResultFile* file = nullptr;
      const ResultField& re = field_of(a, p, &file);
      ResultFrame& frame = file->frame(p["frame"].get<int>());
      const std::string iname = re.name + "I";
      bool found = false;
      for (const ResultField& f : frame.fields) found |= f.name == iname;
      if (!found) throw Error("not_found", "허수부 결과가 없습니다: " + iname + " (복소 결과가 아닙니다)", {{"param", "field"}, {"frame", frame.index}});
      const ResultField& im = file->field(frame, iname);
      if (im.count != re.count || im.components.size() != re.components.size())
        throw Error("invalid_state", "실수부와 허수부의 크기가 다릅니다", {{"field", re.name}});
      const double th = p["phase"].get<double>() * 3.14159265358979323846 / 180.0, c = std::cos(th), s = std::sin(th);
      ResultField mixed = re;  // 합친 성분을 든 임시 필드(파생량 계산을 그대로 쓴다)
      for (std::size_t i = 0; i < mixed.data.size(); ++i) mixed.data[i] = re.data[i] * c - im.data[i] * s;
      const std::vector<double> v = component_values(mixed, p["component"].get<std::string>());
      Json ids = Json::array(), values = Json::array();
      for (std::size_t r : rows_of(a, re, p)) ids.push_back(re.ids[r]), values.push_back(v[r]);
      return Json{{"field", re.name}, {"component", p["component"]}, {"phase", p["phase"]}, {"frame", frame.index}, {"ids", ids}, {"values", values}};
    };
    app.register_command(std::move(c));
  }
  {
    // 적분점 값(RES-57): *EL PRINT 의 표 — 열이 (elem, integ.pnt., 값…) 인 표를 모아 같은 (요소, 적분점)의 좌표(COORD 표 "global coordinates")를 붙인다
    CommandSpec c = base("result.integration_point_values", 'Q',
                         "dat 의 *EL PRINT 표에서 적분점 값(응력·변형률 등, quantity 로 거름)을 요소·적분점별로 조회한다. 같은 시각의 global coordinates 표(COORD)가 있으면 좌표를 붙인다",
                         "RES-57");
    c.params = {id_param(), F("quantity", "string", "표의 종류(예: stresses, strains). 없으면 좌표 표를 뺀 전부").ex("stresses"),
                F("time", "number", "이 시각의 표만(없으면 전부)"), F("elements", "integer_list", "이 요소만").ex({1})};
    c.fn = [](App& a, const Json& p) {
      const ResultFile& f = file_of(a, p);
      std::set<Id> only;
      for (const Json& e : p.value("elements", Json::array())) only.insert(e.get<Id>());
      // (시각, 요소, 적분점) → 좌표
      std::map<std::tuple<double, int, int>, std::array<double, 3>> coords;
      Json tables = Json::array();
      for (const Json& t : dat_print_tables(f)) {
        const Json& cols = t["columns"];
        if (cols.size() < 3 || cols[0] != "elem" || cols[1].get<std::string>().rfind("integ", 0) != 0) continue;  // 적분점 표만
        if (has(p, "time") && std::fabs(t["time"].get<double>() - p["time"].get<double>()) > 1e-9 * std::max(1.0, std::fabs(p["time"].get<double>()))) continue;
        if (t["quantity"] == "global coordinates") {
          for (const Json& r : t["rows"])
            if (r.size() >= 5) coords[{t["time"].get<double>(), static_cast<int>(r[0].get<double>()), static_cast<int>(r[1].get<double>())}] = {r[2].get<double>(), r[3].get<double>(), r[4].get<double>()};
          continue;
        }
        if (has(p, "quantity") && t["quantity"] != p["quantity"]) continue;
        tables.push_back(t);
      }
      Json out = Json::array();
      for (const Json& t : tables) {
        Json rows = Json::array();
        for (const Json& r : t["rows"]) {
          const int elem = static_cast<int>(r[0].get<double>()), ip = static_cast<int>(r[1].get<double>());
          if (!only.empty() && !only.count(elem)) continue;
          Json values = Json::object();
          for (std::size_t c = 2; c < r.size() && c < t["columns"].size(); ++c) values[t["columns"][c].get<std::string>()] = r[c];
          Json row{{"element", elem}, {"point", ip}, {"values", values}};
          auto it = coords.find({t["time"].get<double>(), elem, ip});
          if (it != coords.end()) row["coordinates"] = it->second;
          rows.push_back(std::move(row));
        }
        out.push_back(Json{{"quantity", t["quantity"]}, {"set", t["set"]}, {"time", t["time"]}, {"step", t["step"]}, {"columns", t["columns"]}, {"rows", rows}});
      }
      return out;
    };
    app.register_command(std::move(c));
  }
  {
    // 보 단면력(RES-55): *EL FILE, SECTION FORCES 이면 보 노드의 STRESS 가 단면력으로 바뀐다(매뉴얼 7.21): xx = 1방향 전단력, yy = 2방향 전단력,
    // zz = 축력, xy = 비틀림, xz = 2축 굽힘 모멘트, yz = 1축 굽힘 모멘트(보의 국부 좌표계 n1·n2·t)
    CommandSpec c = base("result.beam_section_forces", 'Q',
                         "보 노드의 축력·전단력·비틀림·굽힘 모멘트를 조회한다(출력 요청의 section_forces 로 쓴 STRESS 블록을 읽는다: SZZ 축력, SXX·SYY 전단력, SXY 비틀림, SZX·SYZ 굽힘 모멘트)",
                         "RES-55");
    c.params = {id_param(), frame_param(), nodes_param()};
    c.fn = [](App& a, const Json& p) {
      Json q = p;
      q["field"] = "STRESS";
      ResultFile* file = nullptr;
      const ResultField& f = field_of(a, q, &file);
      static const std::vector<std::pair<const char*, const char*>> map = {
          {"SXX", "shear_1"}, {"SYY", "shear_2"}, {"SZZ", "normal_force"}, {"SXY", "torque"}, {"SZX", "moment_2"}, {"SYZ", "moment_1"}};  // frd 의 성분 이름은 SZX
      std::map<std::string, std::vector<double>> cols;
      for (const auto& [comp, name] : map) cols[name] = component_values(f, comp);
      // 보 요소의 노드만(결과 파일의 요소 목록에서 B 로 시작하는 타입)
      std::set<Id> beam_nodes;
      for (const ResultElement& e : file->elements)
        if (e.type == 11 || e.type == 12)  // frd 의 be2·be3(SECTION FORCES 는 OUTPUT=3D 와 함께 쓸 수 없으므로 보가 펼쳐지지 않은 채 나온다)
          for (Id n : e.nodes) beam_nodes.insert(n);
      if (beam_nodes.empty())
        throw Error("invalid_state", "결과 파일에 보 요소가 없습니다(단면력은 보 요소에만 있고, 펼친(3D) 출력에는 보 요소가 남지 않는다)", {{"result", file->id}});
      Json ids = Json::array();
      std::map<std::string, Json> out;
      for (const auto& [name, v] : cols) out[name] = Json::array();
      for (std::size_t r : rows_of(a, f, p)) {
        if (!beam_nodes.count(f.ids[r])) continue;
        ids.push_back(f.ids[r]);
        for (const auto& [name, v] : cols) out[name].push_back(v[r]);
      }
      Json j{{"frame", p["frame"]}, {"ids", ids},
             {"note", "값은 보의 국부 좌표계(1·2 방향, 접선 t)이며 출력 요청에 section_forces 가 있어야 단면력이다"}};
      for (const auto& [name, v] : out) j[name] = v;
      return j;
    };
    app.register_command(std::move(c));
  }
  {
    // 순환대칭 전개(RES-50): 한 섹터의 결과를 축 둘레로 sectors 개 복사한다. 좌표와 벡터 성분(3개)·대칭 텐서(6개: XX,YY,ZZ,XY,YZ,ZX)는 회전하고 스칼라는 그대로.
    // 복사본의 노드 번호는 sector × offset 을 더한다(offset = 가장 큰 노드 번호 이상의 10의 거듭제곱)
    CommandSpec c = base("result.expand_cyclic", 'Q',
                         "순환대칭 결과를 축(point·axis) 둘레로 sectors 개로 전개해 노드 번호·좌표·성분 값을 조회한다(벡터·대칭 텐서는 회전, 스칼라는 그대로). "
                         "constraint 로 순환대칭 구속을 주면 그 축·섹터 수를 쓴다",
                         "RES-50");
    c.params = {id_param(), frame_param(), field_param(), F("sectors", "integer", "섹터 수").ge(2).ex(12),
                F("point", "vector3", "축 위의 점").ex({0.0, 0.0, 0.0}), F("axis", "vector3", "축 방향").ex({0.0, 0.0, 1.0}),
                F("constraint", "ref", "순환대칭 구속(cyclic_symmetry)").ref("constraint"), nodes_param()};
    c.fn = [](App& a, const Json& p) {
      ResultFile* file = nullptr;
      const ResultField& f = field_of(a, p, &file);
      int sectors = 0;
      std::array<double, 3> pt{0, 0, 0}, ax{0, 0, 1};
      if (has(p, "constraint")) {
        const Object& o = a.model().get(p["constraint"].get<Id>());
        if (o.kind != "constraint" || o.props.value("type", std::string()) != "cyclic_symmetry") throw Error("wrong_kind", "순환대칭 구속이 아닙니다", {{"object", o.id}});
        sectors = o.props.value("sectors", 0);
        for (int k = 0; k < 3; ++k) pt[k] = o.props["axis_point_a"][k].get<double>(), ax[k] = o.props["axis_point_b"][k].get<double>() - pt[k];
      }
      if (has(p, "sectors")) sectors = p["sectors"].get<int>();
      if (has(p, "point")) for (int k = 0; k < 3; ++k) pt[k] = p["point"][k].get<double>();
      if (has(p, "axis")) for (int k = 0; k < 3; ++k) ax[k] = p["axis"][k].get<double>();
      if (sectors < 2) throw Error("missing_param", "섹터 수(2 이상)가 필요합니다", {{"param", "sectors"}});
      const double al = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
      if (al == 0) throw Error("out_of_range", "축 방향의 길이가 0 입니다", {{"param", "axis"}});
      for (double& v : ax) v /= al;
      auto rot = [&](double ang) {  // 로드리게스 회전 행렬
        const double c = std::cos(ang), s = std::sin(ang), t = 1 - c;
        return std::array<double, 9>{t * ax[0] * ax[0] + c,         t * ax[0] * ax[1] - s * ax[2], t * ax[0] * ax[2] + s * ax[1],
                                     t * ax[0] * ax[1] + s * ax[2], t * ax[1] * ax[1] + c,         t * ax[1] * ax[2] - s * ax[0],
                                     t * ax[0] * ax[2] - s * ax[1], t * ax[1] * ax[2] + s * ax[0], t * ax[2] * ax[2] + c};
      };
      auto apply = [](const std::array<double, 9>& R, const double v[3], double out[3]) {
        for (int i = 0; i < 3; ++i) out[i] = R[i * 3] * v[0] + R[i * 3 + 1] * v[1] + R[i * 3 + 2] * v[2];
      };
      std::map<Id, std::size_t> where;
      for (std::size_t i = 0; i < file->node_ids.size(); ++i) where[file->node_ids[i]] = i;
      Id maxid = 0;
      for (Id n : file->node_ids) maxid = std::max(maxid, n);
      Id offset = 1;
      while (offset <= maxid) offset *= 10;
      const std::vector<std::size_t> rows = rows_of(a, f, p);
      const std::size_t nc = f.components.size();
      const bool vec = nc == 3, tensor = nc == 6;
      Json ids = Json::array(), coords = Json::array(), values = Json::array(), sector_of = Json::array();
      for (int sct = 0; sct < sectors; ++sct) {
        const std::array<double, 9> R = rot(2.0 * 3.14159265358979323846 * sct / sectors);
        for (std::size_t r : rows) {
          const Id n = f.ids[r];
          auto w = where.find(n);
          if (w == where.end()) continue;
          const double* x = &file->node_xyz[3 * w->second];
          const double rel[3] = {x[0] - pt[0], x[1] - pt[1], x[2] - pt[2]};
          double xr[3];
          apply(R, rel, xr);
          ids.push_back(n + offset * sct), sector_of.push_back(sct);
          coords.push_back(Json::array({xr[0] + pt[0], xr[1] + pt[1], xr[2] + pt[2]}));
          const double* v = &f.data[r * nc];
          if (vec) {
            double vr[3];
            apply(R, v, vr);
            values.push_back(Json::array({vr[0], vr[1], vr[2]}));
          } else if (tensor) {
            // S' = R S Rᵀ  (성분 순서 XX, YY, ZZ, XY, YZ, ZX)
            const double S[3][3] = {{v[0], v[3], v[5]}, {v[3], v[1], v[4]}, {v[5], v[4], v[2]}};
            double RS[3][3] = {}, Sr[3][3] = {};
            for (int i = 0; i < 3; ++i)
              for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k) RS[i][j] += R[i * 3 + k] * S[k][j];
            for (int i = 0; i < 3; ++i)
              for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k) Sr[i][j] += RS[i][k] * R[j * 3 + k];
            values.push_back(Json::array({Sr[0][0], Sr[1][1], Sr[2][2], Sr[0][1], Sr[1][2], Sr[0][2]}));
          } else {
            Json row = Json::array();
            for (std::size_t c = 0; c < nc; ++c) row.push_back(v[c]);
            values.push_back(nc == 1 ? Json(v[0]) : row);
          }
        }
      }
      return Json{{"field", f.name}, {"components", f.components}, {"sectors", sectors}, {"offset", offset}, {"ids", ids}, {"sector", sector_of},
                  {"coordinates", coords}, {"values", values}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("result.buckling_summary", 'Q', "좌굴 하중 계수를 조회한다(dat 파일)", "RES-60");
    c.params = {id_param()};
    c.fn = [](App& a, const Json& p) {
      Json out = Json::array();
      for (const DatTable& t : dat_tables(file_of(a, p), "BUCKLINGFACTOROUTPUT")) {
        Json modes = Json::array();
        for (const auto& r : t.rows)
          if (r.size() >= 2) modes.push_back(Json{{"mode", static_cast<int>(r[0])}, {"factor", r[1]}});
        out.push_back(Json{{"step", t.step}, {"modes", modes}});
      }
      return out;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("result.convergence", 'Q', "증분·반복·잔차 이력을 조회한다(sta, cvg 파일)", "RES-29");
    c.params = {id_param()};
    c.fn = [](App& a, const Json& p) {
      const ResultFile& f = file_of(a, p);
      Json increments = Json::array(), iterations = Json::array();
      for (const std::string& line : read_lines(sibling(f, ".sta"))) {
        const auto v = numbers(line);
        if (v.size() >= 7)
          increments.push_back(Json{{"step", static_cast<int>(v[0])}, {"increment", static_cast<int>(v[1])}, {"attempt", static_cast<int>(v[2])},
                                    {"iterations", static_cast<int>(v[3])}, {"total_time", v[4]}, {"step_time", v[5]}, {"increment_size", v[6]}});
      }
      // cvg: [스텝, 증분, 시도, 반복, 접촉 요소 수, 잔차 힘(%), 변위 보정(%), 증분 크기, …]
      for (const std::string& line : read_lines(sibling(f, ".cvg"))) {
        const auto v = numbers(line);
        if (v.size() >= 7)
          iterations.push_back(Json{{"step", static_cast<int>(v[0])}, {"increment", static_cast<int>(v[1])}, {"attempt", static_cast<int>(v[2])},
                                    {"iteration", static_cast<int>(v[3])}, {"contact_elements", static_cast<int>(v[4])},
                                    {"residual_force", v[5]}, {"correction", v[6]}});
      }
      return Json{{"increments", increments}, {"iterations", iterations}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("result.map_to_model", 'Q', "결과 메시와 모델의 노드·요소 번호 대응을 조회한다", "RES-32, RES-53");
    c.params = {id_param()};
    c.fn = [](App& a, const Json& p) {
      const ResultFile& f = file_of(a, p);
      const Mesh& m = a.mesh();
      std::size_t nodes = 0, elems = 0, moved = 0;
      Json only_result = Json::array();
      double scale = 0;
      for (double x : m.node_xyz()) scale = std::max(scale, std::fabs(x));
      for (std::size_t i = 0; i < f.node_ids.size(); ++i) {
        if (!m.has_node(f.node_ids[i])) {  // 솔버가 만든 노드(쉘·보를 솔리드로 펼친 노드 등)
          if (only_result.size() < 1000) only_result.push_back(f.node_ids[i]);
          continue;
        }
        ++nodes;
        const Vec3 x = m.node(f.node_ids[i]);
        for (int k = 0; k < 3; ++k)
          if (std::fabs(x[static_cast<std::size_t>(k)] - f.node_xyz[3 * i + static_cast<std::size_t>(k)]) > 1e-5 * (scale + 1e-300)) {
            ++moved;
            break;
          }
      }
      for (const ResultElement& e : f.elements)
        if (m.has_element(e.id)) ++elems;
      Json out{{"result_nodes", f.node_ids.size()}, {"model_nodes", m.node_count()}, {"matched_nodes", nodes},
               {"moved_nodes", moved}, {"result_only_nodes", f.node_ids.size() - nodes}, {"result_only_node_ids", only_result},
               {"result_elements", f.elements.size()}, {"model_elements", m.element_count()}, {"matched_elements", elems}};
      if (f.node_ids.size() > nodes) {  // 전개된 쉘·보 결과(RES-53): 결과에만 있는 노드를 모델 쉘·보 노드에 대응시킨다
        const ShellExpansion ex = shell_expansion(a, f);
        Json exp = Json::object();
        for (const auto& [id, ids] : ex.top) exp[std::to_string(id)]["top"] = ids;
        for (const auto& [id, ids] : ex.bottom) exp[std::to_string(id)]["bottom"] = ids;
        for (const auto& [id, ids] : ex.mid) exp[std::to_string(id)]["mid"] = ids;
        Json thick = Json::object();
        for (const auto& [id, t] : ex.thickness) thick[std::to_string(id)] = t;
        out["expanded_nodes"] = ex.expanded, out["expansion"] = exp, out["expanded_thickness"] = thick;
      }
      return out;
    };
    app.register_command(std::move(c));
  }
  {
    // 파생 결과(derived_result 객체)의 값: 수식(RES-21)·조합(RES-19)·포락(RES-20). 결과 파일은 열려 있어야 한다.
    CommandSpec c = base("result.derived_values", 'Q', "파생 결과 객체(수식·조합·포락)의 노드별 값을 계산한다. 그 결과 파일(과 조합의 다른 파일)은 열려 있어야 한다",
                         "RES-19, RES-20, RES-21");
    c.target = "derived_result";
    c.params = {F("id", "ref", "파생 결과 객체").ref("derived_result").call_req(),
                F("frame", "integer", "수식의 프레임(객체의 frame 보다 우선)").ge(1), nodes_param()};
    c.fn = [](App& a, const Json& p) {
      const Object& d = a.model().get(p["id"].get<Id>());
      if (d.kind != "derived_result") throw Error("wrong_kind", "파생 결과 객체가 아닙니다", {{"object", d.id}, {"expected", "derived_result"}});
      const std::string type = d.props.value("type", std::string());
      // 결과 파일 객체 → 열린 결과
      auto open_of = [&](Id file_object) -> ResultFile& {
        for (const auto& [rid, rf] : results(a).files())
          if (rf->file_id == file_object) return *rf;
        const Object& fo = a.model().get(file_object);
        const std::string path = fo.props.value("path", std::string());
        if (!path.empty())
          for (const auto& [rid, rf] : results(a).files())
            if (rf->path == path) return *rf;
        throw Error("invalid_state", "결과 파일이 열려 있지 않습니다(result.open file=" + std::to_string(file_object) + ")", {{"object", file_object}});
      };
      ResultFile& file = open_of(d.parent);
      auto field_at = [&](ResultFile& f, int frame, const std::string& name) -> ResultField& { return f.field(f.frame(frame), name); };
      auto rows_in = [&](const ResultField& f) {
        Json q = Json::object();
        if (has(p, "nodes")) q["nodes"] = p["nodes"];
        return rows_of(a, f, q);
      };
      Json ids = Json::array(), values = Json::array();
      if (type == "custom") {
        // 확장이 등록한 계산(API-21): ext.registrations 의 derived_results 에서 이름으로 명령을 찾아 부른다
        const std::string calc = d.props.value("calculation", std::string());
        if (!a.commands().count("ext.registrations"))
          throw Error("not_available", "확장 관리자가 없어 등록된 파생 결과 계산을 찾을 수 없습니다", {{"object", d.id}});
        const Json regs = a.commands().at("ext.registrations").fn(a, Json::object());
        std::string command;
        for (const Json& r : regs.value("derived_results", Json::array()))
          if (r.value("name", std::string()) == calc) command = r.value("command", std::string());
        if (command.empty()) throw Error("not_found", "등록되지 않은 파생 결과 계산: " + calc, {{"object", d.id}, {"field", "calculation"}});
        if (!a.commands().count(command)) throw Error("unknown_command", "등록되지 않은 명령: " + command, {{"object", d.id}});
        Json q = d.props.value("params", Json::object());
        q["result"] = file.id;
        const int frame = has(p, "frame") ? p["frame"].get<int>() : d.props.value("frame", 0);
        if (frame > 0) q["frame"] = frame;
        if (has(p, "nodes")) q["nodes"] = p["nodes"];
        Json r = a.commands().at(command).fn(a, q);
        if (!r.is_object() || !r.contains("ids") || !r.contains("values"))
          throw Error("internal", "파생 결과 계산 명령은 ids·values 를 돌려줘야 합니다: " + command, {{"object", d.id}});
        r["type"] = type, r["calculation"] = calc, r["command"] = command;
        if (frame > 0) r["frame"] = frame;
        return r;
      }
      if (type == "expression") {
        const int frame = has(p, "frame") ? p["frame"].get<int>() : d.props.value("frame", 0);
        if (frame < 1) throw Error("missing_param", "프레임이 필요합니다(객체의 frame 또는 매개변수)", {{"param", "frame"}});
        const std::string expr = d.props.value("expression", std::string());
        // 수식의 변수 "FIELD.comp" 를 찾아 그 열을 읽어 둔다
        std::map<std::string, std::vector<double>> columns;
        std::map<std::string, std::map<Id, std::size_t>> row_of;
        const ResultField* base_field = nullptr;
        for (std::size_t i = 0; i < expr.size();) {
          if (!(std::isalpha(static_cast<unsigned char>(expr[i])) || expr[i] == '_')) {
            ++i;
            continue;
          }
          std::size_t j = i;
          while (j < expr.size() && (std::isalnum(static_cast<unsigned char>(expr[j])) || expr[j] == '_' || expr[j] == '.')) ++j;
          const std::string name = expr.substr(i, j - i);
          i = j;
          const std::size_t dot = name.find('.');
          if (dot == std::string::npos || columns.count(name)) continue;
          ResultField& f = field_at(file, frame, name.substr(0, dot));
          columns[name] = component_values(f, name.substr(dot + 1));
          for (std::size_t r = 0; r < f.count; ++r) row_of[name][f.ids[r]] = r;
          if (!base_field) base_field = &f;
        }
        if (!base_field) throw Error("invalid_expression", "수식에 결과 변수(결과종류.성분)가 없습니다", {{"param", "expression"}});
        // 수식 계산기의 이름에는 점이 없으므로 "FIELD.comp" 를 "FIELD__comp" 로 바꿔 계산한다
        auto flat = [](std::string s) {
          for (std::size_t k = 0; k + 1 < s.size(); ++k)
            if (s[k] == '.' && !std::isdigit(static_cast<unsigned char>(s[k + 1])) && k > 0 && !std::isdigit(static_cast<unsigned char>(s[k - 1])))
              s.replace(k, 1, "__"), ++k;
          return s;
        };
        const std::string flat_expr = flat(expr);
        std::map<std::string, double> vars;
        for (const Object* prm : a.model().by_kind("parameter"))
          if (has(prm->props, "value") && prm->props["value"].is_number()) vars[prm->name] = prm->props["value"].get<double>();
        for (std::size_t r : rows_in(*base_field)) {
          const Id node = base_field->ids[r];
          bool ok = true;
          for (const auto& [name, col] : columns) {
            auto it = row_of[name].find(node);
            if (it == row_of[name].end()) {
              ok = false;
              break;
            }
            vars[flat(name)] = col[it->second];
          }
          if (!ok) continue;
          ids.push_back(node), values.push_back(evaluate_expression(flat_expr, vars));
        }
        return Json{{"type", type}, {"frame", frame}, {"ids", ids}, {"values", values}};
      }
      const std::string field_name = d.props.value("field", std::string()), comp = d.props.value("component", std::string());
      if (type == "combination") {
        std::map<Id, double> sum;
        std::vector<Id> order;
        Json terms_out = Json::array();
        for (const Json& t : d.props.value("terms", Json::array())) {
          ResultFile& f = has(t, "result_file") ? open_of(t["result_file"].get<Id>()) : file;
          ResultField& fld = field_at(f, t["frame"].get<int>(), field_name);
          const std::vector<double> v = component_values(fld, comp);
          const double factor = t.value("factor", 1.0);
          for (std::size_t r : rows_in(fld)) {
            if (!sum.count(fld.ids[r])) order.push_back(fld.ids[r]);
            sum[fld.ids[r]] += factor * v[r];
          }
          terms_out.push_back(Json{{"result", f.id}, {"frame", t["frame"]}, {"factor", factor}});
        }
        for (Id n : order) ids.push_back(n), values.push_back(sum[n]);
        return Json{{"type", type}, {"field", field_name}, {"component", comp}, {"terms", terms_out}, {"ids", ids}, {"values", values}};
      }
      if (type == "envelope") {
        const std::string kind = d.props.value("kind", std::string("max"));
        std::vector<int> frames;
        if (has(d.props, "frames"))
          for (const Json& f : d.props["frames"]) frames.push_back(f.get<int>());
        else
          for (const ResultFrame& fr : file.frames) frames.push_back(fr.index);
        std::map<Id, std::pair<double, int>> best;  // 노드 → (값, 프레임)
        std::vector<Id> order;
        for (int frame : frames) {
          ResultFrame& fr = file.frame(frame);
          if (std::none_of(fr.fields.begin(), fr.fields.end(), [&](const ResultField& f) { return f.name == field_name; })) continue;
          ResultField& fld = file.field(fr, field_name);
          const std::vector<double> v = component_values(fld, comp);
          for (std::size_t r : rows_in(fld)) {
            const Id n = fld.ids[r];
            const double x = v[r], key = kind == "absmax" ? std::fabs(x) : x;
            auto it = best.find(n);
            if (it == best.end()) {
              best[n] = {x, frame}, order.push_back(n);
              continue;
            }
            const double cur = kind == "absmax" ? std::fabs(it->second.first) : it->second.first;
            if (kind == "min" ? key < cur : key > cur) it->second = {x, frame};
          }
        }
        if (order.empty()) throw Error("not_found", "이 결과가 든 프레임이 없습니다: " + field_name, {{"param", "field"}});
        Json sources = Json::array();
        for (Id n : order) ids.push_back(n), values.push_back(best[n].first), sources.push_back(best[n].second);
        return Json{{"type", type}, {"field", field_name}, {"component", comp}, {"kind", kind}, {"frames", frames}, {"ids", ids},
                    {"values", values}, {"source_frames", sources}};
      }
      throw Error("not_available", "알 수 없는 파생 결과 종류: " + type, {{"object", d.id}});
    };
    app.register_command(std::move(c));
  }
  {
    // 그래프 객체(plot)의 값(RES-17, RES-18, RES-49): 결과 파일 객체가 열려 있어야 한다
    auto open_result = [](App& a, Id file_object) -> ResultFile& {
      for (const auto& [rid, rf] : results(a).files())
        if (rf->file_id == file_object) return *rf;
      const Object& fo = a.model().get(file_object);
      const std::string path = fo.props.value("path", std::string());
      if (!path.empty())
        for (const auto& [rid, rf] : results(a).files())
          if (rf->path == path) return *rf;
      throw Error("invalid_state", "결과 파일이 열려 있지 않습니다(result.open file=" + std::to_string(file_object) + ")", {{"object", file_object}});
    };
    auto plot_data = [open_result](App& a, const Object& plot) {
      const std::string type = plot.props.value("type", std::string());
      const Json& q = plot.props;
      ResultFile& file = open_result(a, q["result_file"].get<Id>());
      Json common{{"result", file.id}, {"field", q["field"]}, {"component", q["component"]}};
      if (type == "history" || type == "frequency_response") {
        Json params = common;
        params["node"] = q["node"];
        if (has(q, "step")) params["step"] = q["step"];
        const Json rows = a.commands().at("result.history").fn(a, params);
        Json x = Json::array(), y = Json::array();
        for (const Json& r : rows) x.push_back(r["x"]), y.push_back(r["y"]);
        return Json{{"type", type}, {"x_label", type == "history" ? "time" : "frequency"}, {"y_label", q["field"].get<std::string>() + "." + q["component"].get<std::string>()},
                    {"x", x}, {"y", y}, {"rows", rows}};
      }
      if (type == "path") {
        Json params = common;
        params["frame"] = q["frame"], params["points"] = q["points"];
        if (has(q, "samples")) params["samples"] = q["samples"];
        const Json r = a.commands().at("result.path").fn(a, params);
        Json x = Json::array(), y = Json::array();
        for (const Json& s : r["samples"]) x.push_back(s["s"]), y.push_back(s["value"]);
        return Json{{"type", type}, {"x_label", "distance"}, {"y_label", q["field"].get<std::string>() + "." + q["component"].get<std::string>()},
                    {"x", x}, {"y", y}, {"rows", r["samples"]}};
      }
      throw Error("not_available", "알 수 없는 그래프 종류: " + type, {{"object", plot.id}});
    };
    CommandSpec c = base("plot.data", 'Q', "그래프 객체의 x·y 값을 계산한다(이력: 프레임 값, 경로: 거리, 주파수 응답: 진동수). 결과 파일은 열려 있어야 한다", "RES-17, RES-18, RES-49");
    c.target = "plot";
    c.params = {F("id", "ref", "그래프 객체").ref("plot").call_req()};
    c.fn = [plot_data](App& a, const Json& p) {
      const Object& plot = a.model().get(p["id"].get<Id>());
      if (plot.kind != "plot") throw Error("wrong_kind", "그래프 객체가 아닙니다", {{"object", plot.id}, {"expected", "plot"}});
      return plot_data(a, plot);
    };
    app.register_command(std::move(c));
    CommandSpec e = base("plot.export", 'J', "그래프의 값을 CSV 로 내보낸다", "RES-25");
    e.target = "plot";
    e.params = {F("id", "ref", "그래프 객체").ref("plot").call_req(), F("path", "string", "저장할 파일(.csv)").call_req().ex("plot.csv")};
    e.fn = [plot_data](App& a, const Json& p) {
      const Object& plot = a.model().get(p["id"].get<Id>());
      if (plot.kind != "plot") throw Error("wrong_kind", "그래프 객체가 아닙니다", {{"object", plot.id}, {"expected", "plot"}});
      const Json d = plot_data(a, plot);
      const std::string path = p["path"].get<std::string>();
      std::ofstream os(path_of(path), std::ios::binary);
      if (!os) throw Error("io_error", "파일을 쓸 수 없습니다: " + path, {{"path", path}});
      os.precision(9);
      os << d["x_label"].get<std::string>() << "," << d["y_label"].get<std::string>() << "\n";
      for (std::size_t i = 0; i < d["x"].size(); ++i) os << d["x"][i].get<double>() << "," << d["y"][i].get<double>() << "\n";
      return Json{{"path", path}, {"rows", d["x"].size()}};
    };
    app.register_command(std::move(e));
  }
  {
    // 솔버의 산출 파일 가운데 행렬·부분구조 출력(RES-62): *SUBSTRUCTURE MATRIX OUTPUT 의 .mtx, 강성·질량 행렬 출력(.sti, .mas, .dmp 등)
    CommandSpec c = base("result.matrix_files", 'Q', "결과 파일 옆의 행렬·부분구조 출력 파일 목록과 크기를 조회한다(.mtx, .sti, .mas, .dmp, .dyn, .eig)", "RES-62");
    c.params = {id_param()};
    c.fn = [](App& a, const Json& p) {
      const ResultFile& f = file_of(a, p);
      const fs::path dir = path_of(f.path).parent_path();
      const std::string stem = utf8(path_of(f.path).stem());
      static const std::set<std::string> exts = {".mtx", ".sti", ".mas", ".dmp", ".dyn", ".eig", ".rin", ".rout"};
      Json files = Json::array();
      std::error_code ec;
      for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        const std::string name = utf8(entry.path().filename());
        std::string ext = entry.path().extension().string();
        for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (!exts.count(ext) || name.rfind(stem, 0) != 0) continue;
        files.push_back(Json{{"name", name}, {"path", utf8(entry.path())}, {"bytes", entry.file_size()}, {"kind", ext.substr(1)}});
      }
      return Json{{"directory", utf8(dir)}, {"files", files}};
    };
    app.register_command(std::move(c));
  }
  {
    // 발산 진단 자료(RES-31): ResultsForLastIterations.frd(*NODE FILE 의 LAST ITERATIONS), <job>.cel(CONTACT ELEMENTS, inp 꼴 요소 셋
    // contactelements_st<스텝>_in<증분>_at<시도>_it<반복>), <job>_Warn*.nam(솔버 경고 노드·요소 셋, inp 꼴 *NSET/*ELSET). ccx 2.22 매뉴얼 2장·7.21.
    CommandSpec c = base("result.diagnostics", 'Q',
                         "발산 진단 자료를 조회한다: 마지막 증분의 반복별 결과(ResultsForLastIterations.frd — open 이면 결과로 열어 번호를 돌려줌), "
                         "반복별 접촉 요소(<job>.cel — set 을 주면 그 셋의 요소 연결), 솔버 경고 셋(<job>_Warn*.nam)",
                         "RES-31");
    c.params = {id_param(), F("open", "bool", "ResultsForLastIterations.frd 를 결과로 연다"),
                F("set", "string", "접촉 요소 셋 이름(이 셋의 요소 번호·연결을 돌려준다)").ex("contactelements_st1_in1_at1_it1")};
    c.fn = [](App& a, const Json& p) {
      const ResultFile& f = file_of(a, p);
      // 지연 로딩 통계(RES-03): 올라와 있는 필드 수·바이트, 파일 크기
      std::size_t loaded_fields = 0, loaded_bytes = 0;
      for (const ResultFrame& fr : f.frames)
        for (const ResultField& fld : fr.fields)
          if (fld.loaded) ++loaded_fields, loaded_bytes += fld.data.size() * sizeof(double) + fld.ids.size() * sizeof(Id);
      std::error_code fec;
      const std::uint64_t file_bytes = fs::is_regular_file(path_of(f.path), fec) ? static_cast<std::uint64_t>(fs::file_size(path_of(f.path), fec)) : 0;
      const fs::path dir = path_of(f.path).parent_path();
      const std::string stem = utf8(path_of(f.path).stem());
      auto upper = [](std::string s) {
        for (char& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        return s;
      };
      auto trim = [](std::string s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        std::size_t i = 0;
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        return s.substr(i);
      };
      // inp 꼴 파일의 셋을 읽는다: 키워드 줄(*ELEMENT/*NSET/*ELSET)과 그 아래 번호 줄
      struct SetBlock {
        std::string keyword, name, type;
        std::vector<std::vector<Id>> rows;
      };
      auto read_sets = [&](const fs::path& path) {
        std::vector<SetBlock> blocks;
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
          line = trim(line);
          if (line.empty() || line.rfind("**", 0) == 0) continue;
          if (line[0] == '*') {
            SetBlock b;
            std::stringstream ss(line.substr(1));
            std::string item;
            bool first = true;
            while (std::getline(ss, item, ',')) {
              item = trim(item);
              if (first) b.keyword = upper(item), first = false;
              else {
                const auto eq = item.find('=');
                const std::string key = upper(trim(item.substr(0, eq))), val = eq == std::string::npos ? "" : trim(item.substr(eq + 1));
                if (key == "ELSET" || key == "NSET") b.name = val;
                else if (key == "TYPE") b.type = upper(val);
              }
            }
            blocks.push_back(std::move(b));
            continue;
          }
          if (blocks.empty()) continue;
          std::vector<Id> row;
          std::stringstream ss(line);
          std::string item;
          while (std::getline(ss, item, ',')) {
            item = trim(item);
            if (!item.empty()) row.push_back(static_cast<Id>(std::stoll(item)));
          }
          if (!row.empty()) blocks.back().rows.push_back(std::move(row));
        }
        return blocks;
      };
      Json out{{"directory", utf8(dir)}};
      // 1) 마지막 증분의 반복별 결과
      const fs::path last = dir / "ResultsForLastIterations.frd";
      std::error_code ec;
      if (fs::is_regular_file(last, ec)) {
        Json li{{"path", utf8(last)}, {"bytes", fs::file_size(last, ec)}};
        if (p.value("open", false)) {
          auto lf = results(a).open(utf8(last));
          lf->case_id = f.case_id;
          li["result"] = lf->id, li["frames"] = lf->frames.size();
        }
        out["last_iterations"] = li;
      } else {
        out["last_iterations"] = nullptr;
      }
      // 2) 반복별 접촉 요소
      const fs::path cel = dir / (stem + ".cel");
      if (fs::is_regular_file(cel, ec)) {
        Json sets = Json::array();
        const std::string want = p.value("set", std::string());
        bool found = want.empty();
        for (const SetBlock& b : read_sets(cel)) {
          if (b.keyword != "ELEMENT") continue;
          Json s{{"name", b.name}, {"type", b.type}, {"elements", b.rows.size()}};
          // contactelements_st1_in2_at1_it3 → 스텝·증분·시도·반복
          std::size_t pos = 0;
          for (const char* key : {"_st", "_in", "_at", "_it"}) {
            const auto at = b.name.find(key, pos);
            if (at == std::string::npos) break;
            std::size_t end = at + 3;
            while (end < b.name.size() && std::isdigit(static_cast<unsigned char>(b.name[end]))) ++end;
            if (end > at + 3) s[std::string(key + 1) == "st" ? "step" : std::string(key + 1) == "in" ? "increment" : std::string(key + 1) == "at" ? "attempt" : "iteration"] =
                std::stoi(b.name.substr(at + 3, end - at - 3));
            pos = end;
          }
          if (b.name == want) {
            found = true;
            Json ids = Json::array(), conn = Json::array();
            for (const auto& row : b.rows) {
              ids.push_back(row[0]);
              conn.push_back(std::vector<Id>(row.begin() + 1, row.end()));
            }
            s["ids"] = ids, s["connectivity"] = conn;
          }
          sets.push_back(std::move(s));
        }
        if (!found) throw Error("not_found", "접촉 요소 셋이 없습니다: " + want, {{"param", "set"}, {"path", utf8(cel)}});
        out["contact_elements"] = Json{{"path", utf8(cel)}, {"sets", sets}};
      } else {
        if (has(p, "set")) throw Error("not_found", "접촉 요소 파일이 없습니다: " + utf8(cel), {{"param", "set"}});
        out["contact_elements"] = nullptr;
      }
      // 3) 솔버 경고 셋(<job>_Warn*.nam)
      Json warnings = Json::array();
      for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        const std::string name = utf8(entry.path().filename());
        if (name.rfind(stem + "_Warn", 0) != 0 || upper(entry.path().extension().string()) != ".NAM") continue;
        for (const SetBlock& b : read_sets(entry.path())) {
          if (b.keyword != "NSET" && b.keyword != "ELSET") continue;
          Json ids = Json::array();
          for (const auto& row : b.rows)
            for (Id id : row) ids.push_back(id);
          warnings.push_back(Json{{"file", name}, {"path", utf8(entry.path())}, {"kind", b.keyword == "NSET" ? "nodes" : "elements"},
                                  {"name", b.name}, {"warning", name.substr(stem.size() + 1, name.size() - stem.size() - 5)}, {"count", ids.size()}, {"ids", ids}});
        }
      }
      out["warnings"] = warnings;
      Json diag_out = out;
      diag_out["loaded_fields"] = loaded_fields, diag_out["loaded_bytes"] = loaded_bytes, diag_out["file_bytes"] = file_bytes;
      return diag_out;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("result.export_table", 'J', "결과 값을 CSV 로 내보낸다(노드 번호·좌표·성분·파생량 열)", "RES-25");
    c.params = {id_param(), frame_param(), field_param(), F("path", "string", "저장할 파일(.csv)").call_req().ex("stress.csv"),
                F("components", "string_list", "내보낼 성분·파생량(없으면 성분 전부와 파생량 전부)").ex({"SXX", "mises"}), nodes_param(),
                F("coordinates", "bool", "노드 좌표 열을 넣는다(기본 켬)")};
    c.fn = [](App& a, const Json& p) {
      ResultFile* file = nullptr;
      const ResultField& f = field_of(a, p, &file);
      std::vector<std::string> names;
      if (has(p, "components")) {
        for (const Json& n : p["components"]) names.push_back(n.get<std::string>());
      } else {
        names = f.components;
        for (const std::string& d : derived_names(f)) names.push_back(d);
      }
      std::vector<std::vector<double>> columns;
      for (const std::string& n : names) columns.push_back(component_values(f, n));
      std::map<Id, std::size_t> node_at;
      for (std::size_t i = 0; i < file->node_ids.size(); ++i) node_at[file->node_ids[i]] = i;
      const bool coords = p.value("coordinates", true);
      const std::string path = p["path"].get<std::string>();
      std::ofstream os(path_of(path), std::ios::binary);
      if (!os) throw Error("io_error", "파일을 쓸 수 없습니다: " + path, {{"path", path}});
      os << "node";
      if (coords) os << ",x,y,z";
      for (const std::string& n : names) os << "," << n;
      os << "\n";
      os.precision(9);
      const auto rows = rows_of(a, f, p);
      for (std::size_t r : rows) {
        os << f.ids[r];
        if (coords) {
          auto it = node_at.find(f.ids[r]);
          if (it == node_at.end()) os << ",,,";
          else os << "," << file->node_xyz[3 * it->second] << "," << file->node_xyz[3 * it->second + 1] << "," << file->node_xyz[3 * it->second + 2];
        }
        for (const auto& col : columns) os << "," << col[r];
        os << "\n";
      }
      if (!os) throw Error("io_error", "파일을 쓰다가 실패했습니다: " + path, {{"path", path}});
      return Json{{"path", path}, {"rows", rows.size()}, {"columns", names}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("result.path", 'Q',
                         "경로(꺾은선)를 따라 결과를 샘플링한다. 표본점을 품은 솔리드 요소(사면체·육면체의 꼭짓점) 안에서 선형 보간하고, "
                         "품은 요소가 없으면 가장 가까운 노드의 값을 쓴다",
                         "RES-18");
    c.params = {id_param(), frame_param(), field_param(), F("component", "string", "성분 또는 파생량").call_req().ex("mises"),
                F("points", "table", "경로의 꼭짓점(순서대로)").columns(3).call_req().unit("length")
                    .ex(Json::array({Json::array({0.0, 0.0, 0.0}), Json::array({100.0, 0.0, 0.0})})),
                F("samples", "integer", "표본 수(기본 50)").ge(2).le(100000)};
    c.fn = [](App& a, const Json& p) {
      ResultFile* file = nullptr;
      const ResultField& f = field_of(a, p, &file);
      check_value(a.commands().at("result.path").params[4], p["points"], nullptr);
      if (has(p, "samples")) check_value(a.commands().at("result.path").params[5], p["samples"], nullptr);
      const std::vector<double> v = component_values(f, p["component"].get<std::string>());
      std::map<Id, std::size_t> node_at;
      for (std::size_t i = 0; i < file->node_ids.size(); ++i) node_at[file->node_ids[i]] = i;
      std::vector<Vec3> pts;
      for (const Json& row : p["points"]) pts.push_back({row[0].get<double>(), row[1].get<double>(), row[2].get<double>()});
      if (pts.size() < 2) throw Error("out_of_range", "경로에는 점이 2개 이상 있어야 합니다", {{"param", "points"}});
      std::vector<double> cum{0.0};
      for (std::size_t i = 1; i < pts.size(); ++i) {
        double d2 = 0;
        for (std::size_t k = 0; k < 3; ++k) d2 += std::pow(pts[i][k] - pts[i - 1][k], 2);
        cum.push_back(cum.back() + std::sqrt(d2));
      }
      if (cum.back() <= 0) throw Error("out_of_range", "경로의 길이가 0 입니다", {{"param", "points"}});
      const int n = p.value("samples", 50);
      // 결과 값 행(노드 → 행), 솔리드 요소의 꼭짓점과 경계 상자
      std::map<Id, std::size_t> row_of;
      for (std::size_t r = 0; r < f.count; ++r) row_of[f.ids[r]] = r;
      auto xyz_of = [&](Id node) -> const double* {
        auto it = node_at.find(node);
        return it == node_at.end() ? nullptr : &file->node_xyz[3 * it->second];
      };
      struct Cell {
        std::vector<Id> corners;  // 4(사면체) 또는 8(육면체)
        double lo[3], hi[3];
      };
      std::vector<Cell> cells;
      for (const ResultElement& e : file->elements) {
        std::size_t nc = 0;
        if (e.type == 3 || e.type == 6) nc = 4;       // tet4, tet10 의 꼭짓점 4개
        else if (e.type == 1 || e.type == 4) nc = 8;  // he8, he20 의 꼭짓점 8개
        if (!nc || e.nodes.size() < nc) continue;
        Cell c;
        c.lo[0] = c.lo[1] = c.lo[2] = std::numeric_limits<double>::infinity(), c.hi[0] = c.hi[1] = c.hi[2] = -c.lo[0];
        bool ok = true;
        for (std::size_t k = 0; k < nc; ++k) {
          const double* q = xyz_of(e.nodes[k]);
          if (!q || !row_of.count(e.nodes[k])) {
            ok = false;
            break;
          }
          c.corners.push_back(e.nodes[k]);
          for (int d = 0; d < 3; ++d) c.lo[d] = std::min(c.lo[d], q[d]), c.hi[d] = std::max(c.hi[d], q[d]);
        }
        if (ok) cells.push_back(std::move(c));
      }
      // 사면체: 무게중심 좌표. 육면체: 삼선형 사상을 뉴턴법으로 뒤집는다. 성공하면 형상 함수 값을 w 에 둔다.
      auto in_tet = [&](const Cell& c, const Vec3& x, double w[8]) {
        const double* q[4] = {xyz_of(c.corners[0]), xyz_of(c.corners[1]), xyz_of(c.corners[2]), xyz_of(c.corners[3])};
        double A[3][3], b[3];
        for (int i = 0; i < 3; ++i) {
          for (int j = 0; j < 3; ++j) A[i][j] = q[j + 1][i] - q[0][i];
          b[i] = x[static_cast<std::size_t>(i)] - q[0][i];
        }
        const double det = A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) - A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) +
                           A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
        if (std::fabs(det) < 1e-300) return false;
        double l[3];
        for (int k = 0; k < 3; ++k) {  // 크래머 공식
          double M[3][3];
          for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) M[i][j] = j == k ? b[i] : A[i][j];
          l[k] = (M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
                  M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0])) / det;
        }
        const double tol = -1e-6;
        if (l[0] < tol || l[1] < tol || l[2] < tol || 1 - l[0] - l[1] - l[2] < tol) return false;
        w[0] = 1 - l[0] - l[1] - l[2], w[1] = l[0], w[2] = l[1], w[3] = l[2];
        return true;
      };
      auto in_hex = [&](const Cell& c, const Vec3& x, double w[8]) {
        const double* q[8];
        for (int k = 0; k < 8; ++k) q[k] = xyz_of(c.corners[static_cast<std::size_t>(k)]);
        static const double sgn[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}};
        double r[3] = {0, 0, 0};
        for (int iter = 0; iter < 20; ++iter) {
          double N[8], dN[8][3], X[3] = {0, 0, 0}, J[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
          for (int k = 0; k < 8; ++k) {
            const double a = 1 + sgn[k][0] * r[0], b = 1 + sgn[k][1] * r[1], g = 1 + sgn[k][2] * r[2];
            N[k] = 0.125 * a * b * g;
            dN[k][0] = 0.125 * sgn[k][0] * b * g, dN[k][1] = 0.125 * a * sgn[k][1] * g, dN[k][2] = 0.125 * a * b * sgn[k][2];
            for (int i = 0; i < 3; ++i) {
              X[i] += N[k] * q[k][i];
              for (int j = 0; j < 3; ++j) J[i][j] += dN[k][j] * q[k][i];
            }
          }
          const double res[3] = {x[0] - X[0], x[1] - X[1], x[2] - X[2]};
          const double det = J[0][0] * (J[1][1] * J[2][2] - J[1][2] * J[2][1]) - J[0][1] * (J[1][0] * J[2][2] - J[1][2] * J[2][0]) +
                             J[0][2] * (J[1][0] * J[2][1] - J[1][1] * J[2][0]);
          if (std::fabs(det) < 1e-300) return false;
          double inv[3][3];
          inv[0][0] = (J[1][1] * J[2][2] - J[1][2] * J[2][1]) / det, inv[0][1] = (J[0][2] * J[2][1] - J[0][1] * J[2][2]) / det;
          inv[0][2] = (J[0][1] * J[1][2] - J[0][2] * J[1][1]) / det, inv[1][0] = (J[1][2] * J[2][0] - J[1][0] * J[2][2]) / det;
          inv[1][1] = (J[0][0] * J[2][2] - J[0][2] * J[2][0]) / det, inv[1][2] = (J[0][2] * J[1][0] - J[0][0] * J[1][2]) / det;
          inv[2][0] = (J[1][0] * J[2][1] - J[1][1] * J[2][0]) / det, inv[2][1] = (J[0][1] * J[2][0] - J[0][0] * J[2][1]) / det;
          inv[2][2] = (J[0][0] * J[1][1] - J[0][1] * J[1][0]) / det;
          double step = 0;
          for (int i = 0; i < 3; ++i) {
            double d = 0;
            for (int j = 0; j < 3; ++j) d += inv[i][j] * res[j];
            r[i] += d, step = std::max(step, std::fabs(d));
          }
          if (step < 1e-10) {
            for (int i = 0; i < 3; ++i)
              if (r[i] < -1 - 1e-6 || r[i] > 1 + 1e-6) return false;
            for (int k = 0; k < 8; ++k) w[k] = 0.125 * (1 + sgn[k][0] * r[0]) * (1 + sgn[k][1] * r[1]) * (1 + sgn[k][2] * r[2]);
            return true;
          }
          if (std::fabs(r[0]) > 3 || std::fabs(r[1]) > 3 || std::fabs(r[2]) > 3) return false;  // 발산
        }
        return false;
      };
      Json arr = Json::array();
      std::size_t interpolated = 0;
      for (int s = 0; s < n; ++s) {
        const double t = cum.back() * s / (n - 1);
        std::size_t seg = 1;
        while (seg + 1 < pts.size() && cum[seg] < t) ++seg;
        const double u = (t - cum[seg - 1]) / std::max(cum[seg] - cum[seg - 1], 1e-300);
        Vec3 x;
        for (std::size_t k = 0; k < 3; ++k) x[k] = pts[seg - 1][k] + u * (pts[seg][k] - pts[seg - 1][k]);
        Json sample{{"s", t}, {"point", {x[0], x[1], x[2]}}};
        bool found = false;
        for (const Cell& c : cells) {
          const double eps = 1e-9 * (1 + std::fabs(c.hi[0] - c.lo[0]) + std::fabs(c.hi[1] - c.lo[1]) + std::fabs(c.hi[2] - c.lo[2]));
          if (x[0] < c.lo[0] - eps || x[0] > c.hi[0] + eps || x[1] < c.lo[1] - eps || x[1] > c.hi[1] + eps || x[2] < c.lo[2] - eps || x[2] > c.hi[2] + eps) continue;
          double w[8];
          if (!(c.corners.size() == 4 ? in_tet(c, x, w) : in_hex(c, x, w))) continue;
          double value = 0;
          for (std::size_t k = 0; k < c.corners.size(); ++k) value += w[k] * v[row_of.at(c.corners[k])];
          sample["value"] = value, sample["method"] = "interpolated";
          found = true, ++interpolated;
          break;
        }
        if (!found) {  // 가장 가까운 결과 노드
          std::size_t best = 0;
          double best_d = std::numeric_limits<double>::infinity();
          for (std::size_t r = 0; r < f.count; ++r) {
            const double* q = xyz_of(f.ids[r]);
            if (!q) continue;
            const double d = std::pow(q[0] - x[0], 2) + std::pow(q[1] - x[1], 2) + std::pow(q[2] - x[2], 2);
            if (d < best_d) best_d = d, best = r;
          }
          sample["node"] = f.ids[best], sample["distance"] = std::sqrt(best_d), sample["value"] = v[best], sample["method"] = "nearest_node";
        }
        arr.push_back(std::move(sample));
      }
      return Json{{"length", cum.back()}, {"interpolated", interpolated}, {"samples", arr}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("result.compare", 'Q', "두 결과(또는 두 프레임)의 같은 노드에서 성분 값의 차이를 조회한다", "RES-24");
    c.params = {id_param(), frame_param(), field_param(), F("component", "string", "성분 또는 파생량").call_req().ex("mises"),
                F("other", "integer", "비교할 결과(없으면 같은 결과)").ge(1), F("other_frame", "integer", "비교할 프레임(없으면 같은 번호)").ge(1),
                nodes_param()};
    c.fn = [](App& a, const Json& p) {
      const ResultField& f = field_of(a, p);
      Json q = p;
      if (has(p, "other")) q["result"] = p["other"];
      if (has(p, "other_frame")) q["frame"] = p["other_frame"];
      const ResultField& g = field_of(a, q);
      const std::string comp = p["component"].get<std::string>();
      const std::vector<double> va = component_values(f, comp), vb = component_values(g, comp);
      std::map<Id, std::size_t> where;
      for (std::size_t i = 0; i < g.count; ++i) where[g.ids[i]] = i;
      double max_abs = 0, sum2 = 0, max_ref = 0;
      Id at = 0;
      std::size_t n = 0, missing = 0;
      Json diffs = Json::array();
      const bool list = has(p, "nodes");
      for (std::size_t r : rows_of(a, f, p)) {
        auto it = where.find(f.ids[r]);
        if (it == where.end()) {
          ++missing;
          continue;
        }
        const double d = va[r] - vb[it->second];
        if (std::fabs(d) > max_abs) max_abs = std::fabs(d), at = f.ids[r];
        max_ref = std::max(max_ref, std::fabs(va[r]));
        sum2 += d * d, ++n;
        if (list) diffs.push_back(Json{{"node", f.ids[r]}, {"a", va[r]}, {"b", vb[it->second]}, {"diff", d}});
      }
      Json out{{"count", n}, {"missing", missing}, {"max_abs_diff", max_abs}, {"max_abs_diff_node", at},
               {"rms_diff", n ? std::sqrt(sum2 / static_cast<double>(n)) : 0.0}, {"max_abs_value", max_ref},
               {"relative", max_ref > 0 ? max_abs / max_ref : 0.0}};
      if (list) out["nodes"] = diffs;
      return out;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("result.transform", 'Q', "벡터(3성분)·대칭 텐서(6성분) 결과를 좌표계(직교·원통)의 성분으로 바꿔 조회한다", "RES-09, RES-58");
    c.params = {id_param(), frame_param(), field_param(), F("csys", "ref", "좌표계").ref("csys").call_req(), nodes_param()};
    c.fn = [](App& a, const Json& p) {
      ResultFile* file = nullptr;
      const ResultField& f = field_of(a, p, &file);
      const std::size_t nc = f.components.size();
      if (nc != 3 && nc != 6) throw Error("invalid_state", "벡터(3)·대칭 텐서(6) 결과만 변환합니다: " + f.name, {{"param", "field"}, {"components", nc}});
      const Object& cs = a.model().get(p["csys"].get<Id>());
      if (cs.kind != "csys") throw Error("wrong_kind", "좌표계가 아닙니다", {{"object", cs.id}, {"expected", "csys"}});
      const std::string type = cs.props.value("type", std::string("rectangular"));
      auto vec = [&](const char* key) {
        const Json& v = cs.props.at(key);
        return Vec3{v[0].get<double>(), v[1].get<double>(), v[2].get<double>()};
      };
      auto unit = [](Vec3 v) {
        const double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        return Vec3{v[0] / n, v[1] / n, v[2] / n};
      };
      auto cross = [](const Vec3& a, const Vec3& b) { return Vec3{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; };
      const Vec3 o = vec("origin"), p1 = vec("axis1_point"), p2 = vec("plane12_point");
      const Vec3 e1 = unit({p1[0] - o[0], p1[1] - o[1], p1[2] - o[2]});
      Vec3 t{p2[0] - o[0], p2[1] - o[1], p2[2] - o[2]};
      const Vec3 e3 = unit(cross(e1, t)), e2 = cross(e3, e1);
      std::map<Id, std::size_t> node_at;
      for (std::size_t i = 0; i < file->node_ids.size(); ++i) node_at[file->node_ids[i]] = i;
      // 노드마다 국부 축: 직교는 고정, 원통은 반경(r)·접선(t)·축(z)이 노드 위치에 따라 돈다
      auto axes_at = [&](Id node, Vec3 ax[3]) {
        ax[0] = e1, ax[1] = e2, ax[2] = e3;
        if (type == "rectangular") return;
        auto it = node_at.find(node);
        if (it == node_at.end()) return;
        const double* x = &file->node_xyz[3 * it->second];
        const Vec3 d{x[0] - o[0], x[1] - o[1], x[2] - o[2]};
        const double h = d[0] * e3[0] + d[1] * e3[1] + d[2] * e3[2];
        Vec3 radial{d[0] - h * e3[0], d[1] - h * e3[1], d[2] - h * e3[2]};
        const double rn = std::sqrt(radial[0] * radial[0] + radial[1] * radial[1] + radial[2] * radial[2]);
        if (rn < 1e-12) return;  // 축 위의 노드: 직교 축을 그대로 쓴다
        ax[0] = {radial[0] / rn, radial[1] / rn, radial[2] / rn};
        ax[2] = e3;
        ax[1] = cross(ax[2], ax[0]);
      };
      Json ids = Json::array(), values = Json::array();
      const std::vector<std::string> names = nc == 3 ? std::vector<std::string>{"V1", "V2", "V3"}
                                                     : std::vector<std::string>{"S11", "S22", "S33", "S12", "S13", "S23"};
      for (std::size_t r : rows_of(a, f, p)) {
        Vec3 ax[3];
        axes_at(f.ids[r], ax);
        const double* d = &f.data[r * nc];
        Json row = Json::array();
        if (nc == 3) {
          for (int i = 0; i < 3; ++i) row.push_back(ax[i][0] * d[0] + ax[i][1] * d[1] + ax[i][2] * d[2]);
        } else {
          // 대칭 텐서 [xx, yy, zz, xy, xz, yz] → S'_ij = a_i · S · a_j
          const double S[3][3] = {{d[0], d[3], d[4]}, {d[3], d[1], d[5]}, {d[4], d[5], d[2]}};
          auto comp = [&](int i, int j) {
            double s = 0;
            for (int m = 0; m < 3; ++m)
              for (int n = 0; n < 3; ++n) s += ax[i][static_cast<std::size_t>(m)] * S[m][n] * ax[j][static_cast<std::size_t>(n)];
            return s;
          };
          for (auto [i, j] : {std::pair{0, 0}, std::pair{1, 1}, std::pair{2, 2}, std::pair{0, 1}, std::pair{0, 2}, std::pair{1, 2}}) row.push_back(comp(i, j));
        }
        ids.push_back(f.ids[r]), values.push_back(std::move(row));
      }
      return Json{{"ids", ids}, {"components", names}, {"values", values}, {"csys", cs.id}, {"type", type}};
    };
    app.register_command(std::move(c));
  }
}

}  // namespace ofep
