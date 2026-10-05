// 자동 메싱(6단계): 형상 파트를 메싱해 메시 파트를 만들고, 형상-메시 연관을 기록한다(MSH-05~11).
// 메셔 라이브러리와 형상 커널은 여기서 직접 쓰지 않는다(mesher.hpp, geometry.hpp 의 인터페이스만 쓴다).
#include <algorithm>
#include <any>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <thread>

#include "mesh_util.hpp"
#include "nasa95/error.hpp"
#include "nasa95/geometry.hpp"
#include "nasa95/mesher.hpp"

namespace nasa95 {

namespace {

using namespace meshutil;

CommandSpec base(const std::string& name, char kind, const std::string& desc, const std::string& features) {
  CommandSpec c;
  c.name = name, c.kind = kind, c.undoable = (kind == 'C'), c.target = "part", c.desc = desc, c.features = features;
  return c;
}

const Object& part_of(const App& a, Id id) {
  const Object& o = a.model().get(id);
  if (o.kind != "part") throw Error("wrong_kind", "id=" + std::to_string(o.id) + " 는 파트가 아닙니다", {{"object", o.id}, {"expected", "part"}});
  return o;
}

// 이 형상 파트를 메싱한 메시 파트(없으면 nullptr).
const Object* mesh_part_of(const App& a, Id part) {
  for (const Object* mp : a.model().by_kind("mesh_part"))
    if (has(mp->props, "geometry") && mp->props["geometry"].get<Id>() == part) return mp;
  return nullptr;
}

std::vector<Id> elements_in(const Mesh& m, Id mesh_part) {
  std::vector<Id> ids;
  for (std::size_t i = 0; i < m.element_count(); ++i)
    if (m.part_at(i) == mesh_part) ids.push_back(m.element_ids()[i]);
  return ids;
}

// 메시 파트의 요소와, 그 요소만 쓰던 노드를 지운다.
std::pair<std::size_t, std::size_t> clear_mesh(App& a, Id mesh_part) {
  Mesh& m = a.mesh();
  const std::vector<Id> elems = elements_in(m, mesh_part);
  if (elems.empty()) return {0, 0};
  std::set<Id> nodes;
  for (Id e : elems)
    for (Id n : m.element(e).nodes)
      if (n) nodes.insert(n);
  m.remove_elements(elems);
  const std::vector<Id> used = m.used_nodes();
  std::vector<Id> orphans;
  for (Id n : nodes)
    if (!std::binary_search(used.begin(), used.end(), n)) orphans.push_back(n);
  m.remove_nodes(orphans);
  return {elems.size(), orphans.size()};
}

// 메셔가 준 중간 절점을 변에 맞춘다: 변의 중점에 가장 가까운 것을 그 변의 자리에 놓는다.
std::vector<Id> ordered_nodes(Shape shape, const std::vector<Id>& raw, const std::map<Id, Vec3>& xyz) {
  const ShapeInfo& info = shape_info(shape);
  if (info.order == 1) return raw;
  const std::size_t nc = static_cast<std::size_t>(info.corners);
  std::vector<Id> corners(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(nc)), mids(raw.begin() + static_cast<std::ptrdiff_t>(nc), raw.end());
  std::vector<Id> slots;
  for (const auto& edge : shape_edges(shape)) {
    const Vec3 mid = mul(add(xyz.at(corners[static_cast<std::size_t>(edge[0])]), xyz.at(corners[static_cast<std::size_t>(edge[1])])), 0.5);
    std::size_t best = 0;
    double best_d = 1e300;
    for (std::size_t k = 0; k < mids.size(); ++k) {
      const double d = norm(sub(xyz.at(mids[k]), mid));
      if (d < best_d) best_d = d, best = k;
    }
    slots.push_back(mids[best]);
    mids.erase(mids.begin() + static_cast<std::ptrdiff_t>(best));
  }
  if (shape == Shape::Line3) return {corners[0], slots[0], corners[1]};
  corners.insert(corners.end(), slots.begin(), slots.end());
  return corners;
}

std::string solver_name_for(const App& a, const Object& part) {
  std::string name;
  for (unsigned char c : part.name)
    if (std::isalnum(c) || c == '_' || c == '-' || c == '.') name += static_cast<char>(c);
  if (name.empty()) name = "PART" + std::to_string(part.id);
  std::set<std::string> used;
  for (const Object* o : a.model().by_kind("mesh_part")) used.insert(o->name);
  std::string out = name;
  for (int n = 2; used.count(out); ++n) out = name + "_" + std::to_string(n);
  return out;
}

// 파트에 해당하는 메시 제어(MSH-04)를 모은다: 억제되지 않은 mesh_control 가운데 parts 가 비었거나 이 파트를 품은 것,
// target 의 형상 삼중항이 이 파트를 가리키는 것.
struct Controls {
  Json global = Json::object();                             // global_size(마지막 것)
  Json curvature = Json::object();                          // curvature(마지막 것)
  std::map<int, double> face_sizes, edge_sizes, vertex_sizes;  // local_size
  std::map<int, int> edge_divisions;                        // edge_division
  std::map<int, double> edge_bias;                          // edge_division.bias, bias.ratio
  std::vector<Id> used;                                     // 쓴 제어 객체
};
Controls collect_controls(App& a, Id part) {
  Controls c;
  for (const Object* o : a.model().by_kind("mesh_control")) {
    if (o->suppressed) continue;
    const std::string t = o->props.value("type", std::string());
    const Json& q = o->props;
    if (t == "global_size" || t == "curvature") {
      bool mine = !has(q, "parts") || q["parts"].empty();
      if (!mine)
        for (const Json& p : q["parts"])
          if (p.get<Id>() == part) mine = true;
      if (!mine) continue;
      (t == "global_size" ? c.global : c.curvature) = q;
      c.used.push_back(o->id);
      continue;
    }
    if (!has(q, "target") || q["target"].value("type", std::string()) != "geometry") continue;
    bool any = false;
    for (const Json& item : q["target"]["ids"]) {
      if (item[0].get<Id>() != part) continue;
      any = true;
      const std::string kind = item[1].get<std::string>();
      const int index = item[2].get<int>();
      if (t == "local_size") {
        const double size = q.value("size", 0.0);
        if (kind == "face") c.face_sizes[index] = size;
        else if (kind == "edge") c.edge_sizes[index] = size;
        else if (kind == "vertex") c.vertex_sizes[index] = size;
      } else if (t == "edge_division" && kind == "edge") {
        c.edge_divisions[index] = q.value("divisions", 1);
        if (has(q, "bias")) c.edge_bias[index] = q["bias"].get<double>();
      } else if (t == "bias" && kind == "edge") {
        c.edge_bias[index] = q.value("ratio", 1.0);
      }
    }
    if (any) c.used.push_back(o->id);
  }
  return c;
}
double edge_length(App& a, Id part, int edge) {
  const std::vector<double> pts = geometry_edge_points(a, part, edge, {0, 0, 0}, 16);
  double len = 0;
  for (std::size_t i = 3; i < pts.size(); i += 3) len += norm(sub({pts[i], pts[i + 1], pts[i + 2]}, {pts[i - 3], pts[i - 2], pts[i - 1]}));
  return len;
}

// 메싱 요청을 만든다(형상을 읽고 매개변수를 검사한다). 주 스레드. 명령의 매개변수가 메시 제어 객체보다 우선한다.
MesherRequest prepare_request(App& a, const Object& part, const Json& params) {
  if (!mesher_available()) throw Error("not_available", "이 빌드에는 자동 메셔가 없습니다");
  const Json counts = geometry_counts(a, part.id);
  if (counts["faces"].get<int>() == 0) throw Error("invalid_state", "파트에 면이 없습니다(모서리뿐이면 dimension=1)", {{"object", part.id}, {"param", "dimension"}});
  const Controls ctl = collect_controls(a, part.id);
  MesherRequest rq;
  rq.dimension = params.value("dimension", counts["solids"].get<int>() > 0 ? 3 : 2);
  if (rq.dimension == 3 && counts["solids"].get<int>() == 0)
    throw Error("invalid_state", "솔리드가 없는 파트는 3차원으로 메싱할 수 없습니다", {{"object", part.id}, {"param", "dimension"}});
  rq.order = params.value("order", 1);
  rq.min_size = has(params, "min_size") ? params["min_size"].get<double>() : ctl.global.value("min_size", ctl.curvature.value("min_size", 0.0));
  rq.grading = has(params, "grading") ? params["grading"].get<double>() : ctl.global.value("grading", 0.3);
  if (has(params, "size")) {
    rq.max_size = params["size"].get<double>();
  } else if (has(ctl.global, "size")) {
    rq.max_size = ctl.global["size"].get<double>();
  } else {  // 기본: 경계 상자 대각선의 1/10
    const Json bbox = a.commands().at("geometry.measure").fn(a, Json{{"id", part.id}})["bbox"];
    double d2 = 0;
    for (std::size_t k = 0; k < 3; ++k) d2 += std::pow(bbox["max"][k].get<double>() - bbox["min"][k].get<double>(), 2);
    rq.max_size = std::sqrt(d2) / 10.0;
  }
  if (has(ctl.curvature, "safety")) rq.curvature_safety = ctl.curvature["safety"].get<double>();
  for (const auto& [face, h] : ctl.face_sizes) rq.face_sizes.push_back({face, h});
  for (const auto& [edge, h] : ctl.edge_sizes) rq.edge_sizes.push_back({edge, h});
  for (const auto& [vertex, h] : ctl.vertex_sizes) rq.vertex_sizes.push_back({vertex, h});
  for (const auto& [edge, n] : ctl.edge_divisions)  // 사면체 메셔는 분할 수를 길이/분할수의 크기로 쓴다(편향은 쓰지 못한다)
    rq.edge_sizes.push_back({edge, edge_length(a, part.id, edge) / std::max(n, 1)});
  for (const Json& row : params.value("face_sizes", Json::array())) {
    const int face = static_cast<int>(row[0].get<double>());
    if (face < 1 || face > counts["faces"].get<int>())
      throw Error("not_found", "없는 면입니다: " + std::to_string(face), {{"param", "face_sizes"}});
    if (row[1].get<double>() <= 0) throw Error("out_of_range", "크기는 0 보다 커야 합니다", {{"param", "face_sizes"}});
    rq.face_sizes.push_back({face, row[1].get<double>()});
  }
  for (const auto& [edge, h] : rq.edge_sizes)
    if (edge < 1 || edge > counts["edges"].get<int>()) throw Error("not_found", "없는 모서리입니다: " + std::to_string(edge), {{"param", "target"}});
  for (const auto& [v, h] : rq.vertex_sizes)
    if (v < 1 || v > counts["vertices"].get<int>()) throw Error("not_found", "없는 꼭짓점입니다: " + std::to_string(v), {{"param", "target"}});
  return rq;
}

// 메셔의 결과를 모델에 넣는다(메시 파트·노드·요소·형상-메시 연관). 주 스레드, 명령 실행 안에서.
Json apply_mesh(App& a, const Object& part, const Json& params, const MesherRequest& rq, const MesherOutput& out) {
  // 메시 파트: 이미 있으면 그 객체를 그대로 쓴다(프로퍼티 할당이 유지된다)
  Mesh& m = a.mesh();
  Id mp = 0;
  if (const Object* existing = mesh_part_of(a, part.id)) {
    mp = existing->id;
    clear_mesh(a, mp);
  } else {
    mp = a.invoke("mesh_part.create", Json{{"name", solver_name_for(a, part)}, {"geometry", part.id}})["id"].get<Id>();
  }

  // 노드
  const std::size_t nn = out.xyz.size() / 3;
  const Id first_node = m.max_node_id() + 1;
  std::vector<Id> node_ids(nn);
  std::map<Id, Vec3> xyz;
  for (std::size_t i = 0; i < nn; ++i) {
    node_ids[i] = first_node + i;
    xyz[node_ids[i]] = {out.xyz[3 * i], out.xyz[3 * i + 1], out.xyz[3 * i + 2]};
  }
  auto block_nodes = [&](const MesherBlock& b, std::size_t e) {
    std::vector<Id> raw;
    for (int k = 0; k < b.nodes_per; ++k) raw.push_back(node_ids[static_cast<std::size_t>(b.conn[e * static_cast<std::size_t>(b.nodes_per) + static_cast<std::size_t>(k)])]);
    return raw;
  };
  auto shape_for = [&](int dim, int nodes) {
    for (const ShapeInfo& s : all_shapes())
      if (s.dim == dim && s.nodes == nodes && (dim == 3 ? s.corners == 4 : dim == 2 ? s.corners == 3 : true)) return s.shape;
    throw Error("mesh_failed", "메셔가 지원하지 않는 요소를 만들었습니다(절점 " + std::to_string(nodes) + "개)");
  };

  // 요소
  const MesherBlock& main = rq.dimension == 3 ? out.volume : out.surface;
  const Shape shape = shape_for(rq.dimension, main.nodes_per);
  std::vector<Element> elems;
  Id next = m.max_element_id() + 1;
  std::map<int, std::vector<Id>> by_tag;
  for (std::size_t e = 0; e < main.tag.size(); ++e) {
    Element el;
    el.id = next++, el.shape = shape, el.part = mp;
    el.nodes = ordered_nodes(shape, block_nodes(main, e), xyz);
    if (rq.dimension == 3) {  // 부피가 양수가 되게
      std::vector<Vec3> pts;
      for (int k : corner_positions(shape)) pts.push_back(xyz.at(el.nodes[static_cast<std::size_t>(k)]));
      if (element_size(shape_info(shape), pts) < 0) flip(el);
    }
    by_tag[main.tag[e]].push_back(el.id);
    elems.push_back(std::move(el));
  }
  // 쓰이지 않는 노드는 넣지 않는다(메셔가 남긴 점)
  std::set<Id> used;
  for (const Element& e : elems) used.insert(e.nodes.begin(), e.nodes.end());
  std::vector<Id> add_ids;
  std::vector<double> add_xyz;
  for (Id n : node_ids)
    if (used.count(n)) {
      add_ids.push_back(n);
      const Vec3& x = xyz.at(n);
      add_xyz.insert(add_xyz.end(), x.begin(), x.end());
    }
  m.add_nodes(add_ids, add_xyz);
  m.add_elements(elems);

  // --- 형상-메시 연관(MSH-09)
  Json assoc{{"dimension", rq.dimension}, {"faces", Json::object()}, {"edges", Json::object()}, {"vertices", Json::object()}};
  if (rq.dimension == 3) {
    Json solids = Json::object();
    for (const auto& [tag, ids] : by_tag) solids[std::to_string(tag)] = ids;
    assoc["solids"] = solids;
    // 표면 삼각형(꼭짓점 3개) → 형상의 면. 사면체의 면 가운데 같은 꼭짓점을 가진 것을 찾는다.
    std::map<std::array<Id, 3>, int> surface;
    for (std::size_t s = 0; s < out.surface.tag.size(); ++s) {
      const std::vector<Id> raw = block_nodes(out.surface, s);
      std::array<Id, 3> key{raw[0], raw[1], raw[2]};
      std::sort(key.begin(), key.end());
      surface[key] = out.surface.tag[s];
    }
    std::map<int, Json> faces;
    const auto& table = shape_faces(shape);
    for (const Element& el : elems)
      for (std::size_t f = 0; f < table.size(); ++f) {
        std::array<Id, 3> key{el.nodes[static_cast<std::size_t>(table[f][0])], el.nodes[static_cast<std::size_t>(table[f][1])],
                              el.nodes[static_cast<std::size_t>(table[f][2])]};
        std::sort(key.begin(), key.end());
        auto it = surface.find(key);
        if (it == surface.end()) continue;
        Json& list = faces.try_emplace(it->second, Json::array()).first->second;
        list.push_back(el.id), list.push_back(f + 1);
      }
    for (const auto& [tag, list] : faces) assoc["faces"][std::to_string(tag)] = list;
  } else {
    for (const auto& [tag, ids] : by_tag) assoc["faces"][std::to_string(tag)] = ids;
  }
  // 모서리: 메셔의 모서리 번호는 형상의 번호와 다를 수 있으므로, 묶음마다 선분 하나의 가운데 점으로 형상의 모서리를 찾는다.
  std::map<int, std::set<Id>> by_mesher_edge;
  std::map<int, Vec3> probe;
  std::set<Id> boundary_nodes;
  for (std::size_t s = 0; s < out.edge.tag.size(); ++s) {
    const std::vector<Id> raw = block_nodes(out.edge, s);
    if (!used.count(raw[0]) || !used.count(raw[1])) continue;
    for (Id n : raw)
      if (used.count(n)) by_mesher_edge[out.edge.tag[s]].insert(n), boundary_nodes.insert(n);
    probe.try_emplace(out.edge.tag[s], mul(add(xyz.at(raw[0]), xyz.at(raw[1])), 0.5));
  }
  std::vector<double> probes;
  for (const auto& [tag, p] : probe) probes.insert(probes.end(), p.begin(), p.end());
  const std::vector<int> nearest = geometry_nearest_edges(a, part.id, probes);
  std::map<int, std::set<Id>> edge_nodes;
  std::size_t k = 0;
  for (const auto& [tag, p] : probe) {
    const int edge = nearest[k++];
    if (edge) edge_nodes[edge].insert(by_mesher_edge[tag].begin(), by_mesher_edge[tag].end());
  }
  for (const auto& [tag, nodes] : edge_nodes) assoc["edges"][std::to_string(tag)] = std::vector<Id>(nodes.begin(), nodes.end());
  // 꼭짓점: 좌표가 같은 모서리 노드
  const std::vector<double> vertices = geometry_vertices(a, part.id);
  for (std::size_t v = 0; v < vertices.size() / 3; ++v) {
    const Vec3 p{vertices[3 * v], vertices[3 * v + 1], vertices[3 * v + 2]};
    Id best = 0;
    double best_d = 1e-6 * (rq.max_size + norm(p)) + 1e-12;
    for (Id n : boundary_nodes) {
      const double d = norm(sub(xyz.at(n), p));
      if (d < best_d) best_d = d, best = n;
    }
    if (best) assoc["vertices"][std::to_string(v + 1)] = best;
  }

  Json stored{{"dimension", rq.dimension}, {"order", rq.order}, {"size", rq.max_size}, {"min_size", rq.min_size}, {"grading", rq.grading}};
  if (has(params, "face_sizes")) stored["face_sizes"] = params["face_sizes"];
  if (has(params, "element_type")) stored["element_type"] = params["element_type"];
  a.invoke("mesh_part.update", Json{{"id", mp}, {"mesh_params", stored}, {"geometry_digest", geometry_digest(a, part.id)}, {"association", assoc}});
  if (has(params, "element_type")) a.invoke("mesh.set_element_type", Json{{"part", mp}, {"type", params["element_type"]}});
  return Json{{"mesh_part", mp}, {"nodes", add_ids.size()}, {"elements", elems.size()}, {"shape", shape_info(shape).name},
              {"first_node", add_ids.empty() ? 0 : add_ids.front()}, {"first_element", elems.empty() ? 0 : elems.front().id},
              {"size", rq.max_size}, {"mesher", mesher_name()}};
}

// ------------------------------------------------------------ 매핑 육면체 메싱(MSH-07, 자체 구현)
// 솔리드가 육면체 위상(면 6·모서리 12·꼭짓점 8)이면: 모서리를 호 길이로 고르게 나누고, 면은 Coons 보간 뒤 곡면에 투영,
// 안쪽은 3차원 투영(transfinite) 보간으로 노드를 둔다. 요소는 C3D8(2차면 변환 명령으로 C3D20).
Json generate_mapped(App& a, const Object& part, const Json& params) {
  const Json counts = geometry_counts(a, part.id);
  if (counts["solids"].get<int>() != 1)
    throw Error("not_block", "매핑 메싱은 솔리드 하나짜리 파트에만 쓴다(솔리드 " + std::to_string(counts["solids"].get<int>()) + "개)", {{"object", part.id}});
  const BlockTopology t = geometry_block_topology(a, part.id, 1);
  if (!t.ok) throw Error("not_block", "매핑 메싱을 할 수 없는 형상입니다: " + t.reason, {{"object", part.id}});

  // 꼭짓점 자리 (a, b) → 방향(0 = i, 1 = j, 2 = k). 자리 번호: 아랫면 0 1 2 3, 윗면 4 5 6 7
  auto direction = [](int a_, int b_) {
    const int lo = std::min(a_, b_), hi = std::max(a_, b_);
    if (hi - lo == 4) return 2;
    if ((lo == 0 && hi == 1) || (lo == 3 && hi == 2) || (lo == 2 && hi == 3) || (lo == 4 && hi == 5) || (lo == 7 && hi == 6) || (lo == 6 && hi == 7)) return 0;
    return 1;
  };
  // 그 방향의 양의 끝이 되는 자리(i: 0→1, 3→2, 4→5, 7→6; j: 0→3, 1→2, 4→7, 5→6; k: 0→4 …)
  auto positive = [](int a_, int b_, int dir) {
    if (dir == 2) return b_ > a_;
    if (dir == 0) return (b_ == 1 && a_ == 0) || (b_ == 2 && a_ == 3) || (b_ == 5 && a_ == 4) || (b_ == 6 && a_ == 7);
    return (b_ == 3 && a_ == 0) || (b_ == 2 && a_ == 1) || (b_ == 7 && a_ == 4) || (b_ == 6 && a_ == 5);
  };
  const std::vector<double> verts = geometry_vertices(a, part.id);
  auto corner = [&](int slot) {
    const std::size_t v = static_cast<std::size_t>(t.corners[static_cast<std::size_t>(slot)] - 1);
    return Vec3{verts[3 * v], verts[3 * v + 1], verts[3 * v + 2]};
  };
  // 모서리 길이(점 9개의 꺾은선)로 방향별 분할 수
  std::array<double, 3> longest{0, 0, 0};
  for (const auto& e : t.edges) {
    const int dir = direction(e[1], e[2]);
    const std::vector<double> pts = geometry_edge_points(a, part.id, e[0], corner(e[1]), 8);
    double len = 0;
    for (std::size_t i = 3; i < pts.size(); i += 3) len += norm(sub({pts[i], pts[i + 1], pts[i + 2]}, {pts[i - 3], pts[i - 2], pts[i - 1]}));
    longest[static_cast<std::size_t>(dir)] = std::max(longest[static_cast<std::size_t>(dir)], len);
  }
  // 메시 제어(MSH-04): 모서리 분할 수는 그 방향의 분할 수가 되고(여럿이면 최대), 편향은 그 방향의 모든 모서리에 같이 건다
  const Controls ctl = collect_controls(a, part.id);
  std::array<int, 3> n{};
  std::array<int, 3> from_control{0, 0, 0};
  std::array<double, 3> bias{1.0, 1.0, 1.0};
  for (const auto& e : t.edges) {
    const int dir = direction(e[1], e[2]);
    const bool forward = positive(e[1], e[2], dir);  // 형상 모서리의 v1→v2 가 블록의 양의 방향인지
    if (auto it = ctl.edge_divisions.find(e[0]); it != ctl.edge_divisions.end())
      from_control[static_cast<std::size_t>(dir)] = std::max(from_control[static_cast<std::size_t>(dir)], it->second);
    if (auto it = ctl.edge_bias.find(e[0]); it != ctl.edge_bias.end() && it->second > 0)
      bias[static_cast<std::size_t>(dir)] = forward ? it->second : 1.0 / it->second;
  }
  if (has(params, "divisions")) {
    const Json& d = params["divisions"];
    if (!d.is_array() || d.size() != 3) throw Error("invalid_param_type", "divisions 는 [ni, nj, nk] 여야 합니다", {{"param", "divisions"}});
    for (std::size_t k = 0; k < 3; ++k) {
      n[k] = d[k].get<int>();
      if (n[k] < 1) throw Error("out_of_range", "분할 수는 1 이상이어야 합니다", {{"param", "divisions"}});
    }
  } else {
    double size = params.value("size", ctl.global.value("size", 0.0));
    if (size <= 0) size = std::max({longest[0], longest[1], longest[2]}) / 10.0;
    for (std::size_t k = 0; k < 3; ++k) n[k] = from_control[k] > 0 ? from_control[k] : std::max(1, static_cast<int>(std::lround(longest[k] / size)));
  }
  const int ni = n[0], nj = n[1], nk = n[2];
  // 방향별 호 길이 비율: 편향 비 r(마지막 간격/첫 간격)이면 간격이 g = r^(1/(n-1)) 씩 커진다
  std::array<std::vector<double>, 3> fractions;
  for (std::size_t k = 0; k < 3; ++k) {
    const int nn = n[k];
    const double g = nn > 1 ? std::pow(bias[k], 1.0 / (nn - 1)) : 1.0;
    std::vector<double> cum{0.0};
    double step = 1.0;
    for (int i = 0; i < nn; ++i) cum.push_back(cum.back() + step), step *= g;
    for (double& c : cum) c /= cum.back();
    fractions[k] = cum;
  }

  // 모서리 점: 자리 (a, b) 를 양의 방향으로 둔 배열
  std::map<std::pair<int, int>, std::vector<Vec3>> edge_pts;
  for (const auto& e : t.edges) {
    const int dir = direction(e[1], e[2]);
    int a_ = e[1], b_ = e[2];
    if (!positive(a_, b_, dir)) std::swap(a_, b_);
    const std::vector<double> pts = geometry_edge_points_at(a, part.id, e[0], corner(a_), fractions[static_cast<std::size_t>(dir)]);
    std::vector<Vec3> v;
    for (std::size_t i = 0; i + 2 < pts.size(); i += 3) v.push_back({pts[i], pts[i + 1], pts[i + 2]});
    edge_pts[{a_, b_}] = v;
  }
  auto edge = [&](int a_, int b_) -> const std::vector<Vec3>& {
    auto it = edge_pts.find({a_, b_});
    if (it == edge_pts.end()) throw Error("geometry_failed", "모서리 점이 없습니다");
    return it->second;
  };
  // 면 격자: 자리 (p00, p10, p11, p01) 와 (u, v) 의 분할 수. Coons 보간 뒤 면에 투영.
  auto face_index = [&](std::array<int, 4> slots) {
    std::sort(slots.begin(), slots.end());
    for (const auto& f : t.faces) {
      std::array<int, 4> s{f[1], f[2], f[3], f[4]};
      std::sort(s.begin(), s.end());
      if (s == slots) return f[0];
    }
    throw Error("geometry_failed", "면을 찾지 못했습니다");
  };
  auto coons = [&](int p00, int p10, int p11, int p01, int nu, int nv) {
    const auto& bottom = edge(p00, p10);  // u 방향, v = 0
    const auto& top = edge(p01, p11);     // u 방향, v = 1
    const auto& left = edge(p00, p01);    // v 방향, u = 0
    const auto& right = edge(p10, p11);   // v 방향, u = 1
    std::vector<Vec3> grid(static_cast<std::size_t>((nu + 1) * (nv + 1)));
    for (int i = 0; i <= nu; ++i)
      for (int j = 0; j <= nv; ++j) {
        const double u = static_cast<double>(i) / nu, v = static_cast<double>(j) / nv;
        Vec3 p{};
        for (std::size_t c = 0; c < 3; ++c)
          p[c] = (1 - v) * bottom[static_cast<std::size_t>(i)][c] + v * top[static_cast<std::size_t>(i)][c] +
                 (1 - u) * left[static_cast<std::size_t>(j)][c] + u * right[static_cast<std::size_t>(j)][c] -
                 ((1 - u) * (1 - v) * bottom[0][c] + u * (1 - v) * bottom[static_cast<std::size_t>(nu)][c] +
                  (1 - u) * v * top[0][c] + u * v * top[static_cast<std::size_t>(nu)][c]);
        grid[static_cast<std::size_t>(i * (nv + 1) + j)] = p;
      }
    std::vector<double> flat;
    for (const Vec3& p : grid) flat.insert(flat.end(), p.begin(), p.end());
    geometry_project_to_face(a, part.id, face_index({p00, p10, p11, p01}), flat);
    for (std::size_t k = 0; k < grid.size(); ++k) grid[k] = {flat[3 * k], flat[3 * k + 1], flat[3 * k + 2]};
    return grid;
  };
  // 여섯 면: 매개변수 (u, v) 가 그 면에서 어느 격자 축인지 함께 둔다
  const auto F_bottom = coons(0, 1, 2, 3, ni, nj);  // (i, j), k = 0
  const auto F_top = coons(4, 5, 6, 7, ni, nj);     // (i, j), k = nk
  const auto F_front = coons(0, 1, 5, 4, ni, nk);   // (i, k), j = 0
  const auto F_back = coons(3, 2, 6, 7, ni, nk);    // (i, k), j = nj
  const auto F_left = coons(0, 3, 7, 4, nj, nk);    // (j, k), i = 0
  const auto F_right = coons(1, 2, 6, 5, nj, nk);   // (j, k), i = ni
  auto at = [](const std::vector<Vec3>& g, int u, int v, int nv) { return g[static_cast<std::size_t>(u * (nv + 1) + v)]; };

  // 노드 격자(3차원 투영 보간)
  Mesh& m = a.mesh();
  const Id first_node = m.max_node_id() + 1;
  auto index = [&](int i, int j, int k) { return static_cast<std::size_t>((i * (nj + 1) + j) * (nk + 1) + k); };
  std::vector<Vec3> P(static_cast<std::size_t>((ni + 1) * (nj + 1) * (nk + 1)));
  for (int i = 0; i <= ni; ++i)
    for (int j = 0; j <= nj; ++j)
      for (int k = 0; k <= nk; ++k) {
        const double r = static_cast<double>(i) / ni, s = static_cast<double>(j) / nj, w = static_cast<double>(k) / nk;
        Vec3 p{};
        for (std::size_t c = 0; c < 3; ++c) {
          const double faces_ = (1 - r) * at(F_left, j, k, nk)[c] + r * at(F_right, j, k, nk)[c] + (1 - s) * at(F_front, i, k, nk)[c] +
                                s * at(F_back, i, k, nk)[c] + (1 - w) * at(F_bottom, i, j, nj)[c] + w * at(F_top, i, j, nj)[c];
          const double edges_ = (1 - s) * (1 - w) * edge(0, 1)[static_cast<std::size_t>(i)][c] + s * (1 - w) * edge(3, 2)[static_cast<std::size_t>(i)][c] +
                                (1 - s) * w * edge(4, 5)[static_cast<std::size_t>(i)][c] + s * w * edge(7, 6)[static_cast<std::size_t>(i)][c] +
                                (1 - r) * (1 - w) * edge(0, 3)[static_cast<std::size_t>(j)][c] + r * (1 - w) * edge(1, 2)[static_cast<std::size_t>(j)][c] +
                                (1 - r) * w * edge(4, 7)[static_cast<std::size_t>(j)][c] + r * w * edge(5, 6)[static_cast<std::size_t>(j)][c] +
                                (1 - r) * (1 - s) * edge(0, 4)[static_cast<std::size_t>(k)][c] + r * (1 - s) * edge(1, 5)[static_cast<std::size_t>(k)][c] +
                                r * s * edge(2, 6)[static_cast<std::size_t>(k)][c] + (1 - r) * s * edge(3, 7)[static_cast<std::size_t>(k)][c];
          const double corners_ = (1 - r) * (1 - s) * (1 - w) * corner(0)[c] + r * (1 - s) * (1 - w) * corner(1)[c] + r * s * (1 - w) * corner(2)[c] +
                                  (1 - r) * s * (1 - w) * corner(3)[c] + (1 - r) * (1 - s) * w * corner(4)[c] + r * (1 - s) * w * corner(5)[c] +
                                  r * s * w * corner(6)[c] + (1 - r) * s * w * corner(7)[c];
          p[c] = faces_ - edges_ + corners_;
        }
        P[index(i, j, k)] = p;
      }
  std::vector<Id> node_ids(P.size());
  std::vector<double> xyz;
  for (std::size_t q = 0; q < P.size(); ++q) {
    node_ids[q] = first_node + static_cast<Id>(q);
    xyz.insert(xyz.end(), P[q].begin(), P[q].end());
  }
  auto nid = [&](int i, int j, int k) { return node_ids[index(i, j, k)]; };

  // 메시 파트와 요소
  Id mp = 0;
  if (const Object* existing = mesh_part_of(a, part.id)) {
    mp = existing->id;
    clear_mesh(a, mp);
  } else {
    mp = a.invoke("mesh_part.create", Json{{"name", solver_name_for(a, part)}, {"geometry", part.id}})["id"].get<Id>();
  }
  m.add_nodes(node_ids, xyz);
  std::vector<Element> elems;
  Id next = m.max_element_id() + 1;
  for (int i = 0; i < ni; ++i)
    for (int j = 0; j < nj; ++j)
      for (int k = 0; k < nk; ++k) {
        Element el;
        el.id = next++, el.shape = Shape::Hex8, el.part = mp;
        el.nodes = {nid(i, j, k), nid(i + 1, j, k), nid(i + 1, j + 1, k), nid(i, j + 1, k),
                    nid(i, j, k + 1), nid(i + 1, j, k + 1), nid(i + 1, j + 1, k + 1), nid(i, j + 1, k + 1)};
        elems.push_back(std::move(el));
      }
  m.add_elements(elems);

  // 연관(MSH-09): 솔리드 → 요소, 면 → 요소면, 모서리 → 노드, 꼭짓점 → 노드
  Json assoc{{"dimension", 3}, {"faces", Json::object()}, {"edges", Json::object()}, {"vertices", Json::object()}, {"solids", Json::object()}};
  std::vector<Id> all;
  for (const Element& e : elems) all.push_back(e.id);
  assoc["solids"]["1"] = all;
  auto elem_at = [&](int i, int j, int k) { return elems[static_cast<std::size_t>((i * nj + j) * nk + k)]; };
  const auto& table = shape_faces(Shape::Hex8);
  auto face_of = [&](const Element& el, const std::set<Id>& on_face) {
    for (std::size_t f = 0; f < table.size(); ++f) {
      bool all_on = true;
      for (int pos : table[f])
        if (!on_face.count(el.nodes[static_cast<std::size_t>(pos)])) all_on = false;
      if (all_on) return static_cast<int>(f + 1);
    }
    return 0;
  };
  struct Side {
    std::array<int, 4> slots;
    int fixed_axis, fixed_value;  // 0 = i, 1 = j, 2 = k
  };
  const Side sides[6] = {{{0, 1, 2, 3}, 2, 0}, {{4, 5, 6, 7}, 2, nk}, {{0, 1, 5, 4}, 1, 0}, {{3, 2, 6, 7}, 1, nj}, {{0, 3, 7, 4}, 0, 0}, {{1, 2, 6, 5}, 0, ni}};
  for (const Side& sd : sides) {
    std::set<Id> on_face;
    Json list = Json::array();
    for (int i = 0; i <= ni; ++i)
      for (int j = 0; j <= nj; ++j)
        for (int k = 0; k <= nk; ++k) {
          const int v = sd.fixed_axis == 0 ? i : sd.fixed_axis == 1 ? j : k;
          if (v == sd.fixed_value) on_face.insert(nid(i, j, k));
        }
    for (int i = 0; i < ni; ++i)
      for (int j = 0; j < nj; ++j)
        for (int k = 0; k < nk; ++k) {
          const int v = sd.fixed_axis == 0 ? i : sd.fixed_axis == 1 ? j : k;
          const int limit = sd.fixed_value == 0 ? 0 : sd.fixed_value - 1;
          if (v != limit) continue;
          const Element& el = elem_at(i, j, k);
          const int f = face_of(el, on_face);
          if (f) list.push_back(el.id), list.push_back(f);
        }
    assoc["faces"][std::to_string(face_index(sd.slots))] = list;
  }
  for (const auto& e : t.edges) {
    const int dir = direction(e[1], e[2]);
    int a_ = e[1], b_ = e[2];
    if (!positive(a_, b_, dir)) std::swap(a_, b_);
    // 자리 a_ 의 (i, j, k) 에서 dir 방향으로 간다
    const int i0 = (a_ == 1 || a_ == 2 || a_ == 5 || a_ == 6) ? ni : 0, j0 = (a_ == 2 || a_ == 3 || a_ == 6 || a_ == 7) ? nj : 0, k0 = a_ >= 4 ? nk : 0;
    std::vector<Id> nodes;
    for (int s = 0; s <= n[static_cast<std::size_t>(dir)]; ++s)
      nodes.push_back(nid(dir == 0 ? s : i0, dir == 1 ? s : j0, dir == 2 ? s : k0));
    assoc["edges"][std::to_string(e[0])] = nodes;
  }
  for (int slot = 0; slot < 8; ++slot) {
    const int i = (slot == 1 || slot == 2 || slot == 5 || slot == 6) ? ni : 0, j = (slot == 2 || slot == 3 || slot == 6 || slot == 7) ? nj : 0, k = slot >= 4 ? nk : 0;
    assoc["vertices"][std::to_string(t.corners[static_cast<std::size_t>(slot)])] = nid(i, j, k);
  }

  Json stored{{"method", "hex_mapped"}, {"dimension", 3}, {"order", params.value("order", 1)}, {"divisions", {ni, nj, nk}}};
  if (has(params, "size")) stored["size"] = params["size"];
  if (has(params, "element_type")) stored["element_type"] = params["element_type"];
  if (params.value("order", 1) == 2) {
    a.invoke("mesh.convert_order", Json{{"part", mp}, {"order", 2}});  // 중간 절점은 변의 중점에 생긴다
    // 경계의 중간 절점을 형상 위로 옮긴다: 면 위의 것은 면으로, 모서리 위의 것은 모서리로(모서리가 나중이라 우선한다)
    // 요소면의 중간 절점: 면의 꼭짓점 자리 4개를 잇는 변(shape_edges 의 e 번째)의 중간 절점은 자리 8 + e 에 있다
    const auto& table20 = shape_faces(Shape::Hex20);
    const auto& edges20 = shape_edges(Shape::Hex20);
    std::map<int, std::set<Id>> face_mids;  // 형상 면 → 중간 절점
    for (auto& [key, list] : assoc["faces"].items()) {
      for (std::size_t q = 0; q + 1 < list.size(); q += 2) {
        const Element el = m.element(list[q].get<Id>());
        const auto& corners4 = table20[static_cast<std::size_t>(list[q + 1].get<int>() - 1)];
        for (std::size_t e = 0; e < edges20.size(); ++e) {
          const bool on_face = std::find(corners4.begin(), corners4.end(), edges20[e][0]) != corners4.end() &&
                               std::find(corners4.begin(), corners4.end(), edges20[e][1]) != corners4.end();
          if (on_face) face_mids[std::stoi(key)].insert(el.nodes[8 + e]);
        }
      }
    }
    auto move_to = [&](const std::set<Id>& nodes, const std::function<void(std::vector<double>&)>& project) {
      if (nodes.empty()) return;
      std::vector<Id> ids(nodes.begin(), nodes.end());
      std::vector<double> xyz;
      for (Id n_ : ids) {
        const Vec3 x = m.node(n_);
        xyz.insert(xyz.end(), x.begin(), x.end());
      }
      project(xyz);
      m.move_nodes(ids, xyz);
    };
    // 모서리의 중간 절점: 옮기기 전(변의 중점에 있을 때) 찾아 둔다
    std::map<int, std::pair<std::vector<Id>, std::set<Id>>> edge_chains;  // 모서리 → (노드 사슬, 중간 절점)
    for (auto& [key, list] : assoc["edges"].items()) {
      std::vector<Id> chain;
      std::set<Id> mids;
      for (std::size_t q = 0; q < list.size(); ++q) {
        chain.push_back(list[q].get<Id>());
        if (q + 1 < list.size()) {
          const Id u = list[q].get<Id>(), v = list[q + 1].get<Id>();
          const Vec3 mid = mul(add(m.node(u), m.node(v)), 0.5);
          Id found = 0;
          for (const auto& [face, fm] : face_mids)
            for (Id n_ : fm)
              if (!found && norm(sub(m.node(n_), mid)) < 1e-9 * (1 + norm(mid))) found = n_;
          if (found) chain.push_back(found), mids.insert(found);
        }
      }
      edge_chains[std::stoi(key)] = {chain, mids};
    }
    for (const auto& [face, nodes] : face_mids) move_to(nodes, [&](std::vector<double>& xyz) { geometry_project_to_face(a, part.id, face, xyz); });
    for (const auto& [edge_no, cm] : edge_chains) {
      move_to(cm.second, [&](std::vector<double>& xyz) { geometry_project_to_edge(a, part.id, edge_no, xyz); });
      assoc["edges"][std::to_string(edge_no)] = cm.first;
    }
  }
  a.invoke("mesh_part.update", Json{{"id", mp}, {"mesh_params", stored}, {"geometry_digest", geometry_digest(a, part.id)}, {"association", assoc}});
  if (has(params, "element_type")) a.invoke("mesh.set_element_type", Json{{"part", mp}, {"type", params["element_type"]}});
  return Json{{"mesh_part", mp}, {"nodes", node_ids.size()}, {"elements", elems.size()}, {"shape", params.value("order", 1) == 2 ? "hex20" : "hex8"},
              {"first_node", first_node}, {"first_element", elems.empty() ? 0 : elems.front().id}, {"divisions", {ni, nj, nk}}, {"mesher", "mapped"}};
}

// ------------------------------------------------------------ 1D 메싱(MSH-05, 자체 구현)
// 파트의 모든 모서리를 호 길이로 고르게 나눠 선 요소(B31, 2차는 B32: 끝-가운데-끝)를 만든다. 분할 수는 모서리 분할 제어(edge_division)가 있으면 그 값,
// 없으면 ceil(길이 / 크기)(국부 크기 제어가 모서리에 있으면 그 크기). 공유 꼭짓점의 노드는 한 번만 만든다. 2차의 가운데 절점도 곡선 위에 둔다.
Json generate_lines(App& a, const Object& part, const Json& params) {
  const Json counts = geometry_counts(a, part.id);
  const int n_edges = counts["edges"].get<int>();
  if (n_edges == 0) throw Error("invalid_state", "파트에 모서리가 없습니다", {{"object", part.id}, {"param", "dimension"}});
  const Controls ctl = collect_controls(a, part.id);
  const int order = params.value("order", 1);
  double size = 0;
  if (has(params, "size")) {
    size = params["size"].get<double>();
  } else if (has(ctl.global, "size")) {
    size = ctl.global["size"].get<double>();
  } else {
    double total = 0;
    for (int e = 1; e <= n_edges; ++e) total += edge_length(a, part.id, e);
    size = total / 10.0;
  }
  Mesh& m = a.mesh();
  Id mp = 0;
  if (const Object* existing = mesh_part_of(a, part.id)) {
    mp = existing->id;
    clear_mesh(a, mp);
  } else {
    mp = a.invoke("mesh_part.create", Json{{"name", solver_name_for(a, part)}, {"geometry", part.id}})["id"].get<Id>();
  }
  // 꼭짓점 노드(공유)
  const std::vector<double> vertices = geometry_vertices(a, part.id);
  Id next_node = m.max_node_id() + 1;
  std::vector<Id> node_ids;
  std::vector<double> node_xyz;
  std::map<Id, Vec3> xyz;
  auto new_node = [&](const Vec3& p) {
    const Id id = next_node++;
    node_ids.push_back(id), node_xyz.insert(node_xyz.end(), p.begin(), p.end()), xyz[id] = p;
    return id;
  };
  std::vector<Id> vertex_node(vertices.size() / 3, 0);
  auto vertex_at = [&](const Vec3& p, double tol) -> std::size_t {
    for (std::size_t v = 0; v < vertex_node.size(); ++v)
      if (norm(sub(p, {vertices[3 * v], vertices[3 * v + 1], vertices[3 * v + 2]})) <= tol) return v + 1;
    return 0;
  };
  Json assoc{{"dimension", 1}, {"faces", Json::object()}, {"edges", Json::object()}, {"edge_elements", Json::object()}, {"vertices", Json::object()}};
  std::vector<Element> elems;
  Id next_elem = m.max_element_id() + 1;
  for (int e = 1; e <= n_edges; ++e) {
    const double len = edge_length(a, part.id, e);
    int n = 1;
    if (auto it = ctl.edge_divisions.find(e); it != ctl.edge_divisions.end()) {
      n = std::max(it->second, 1);
    } else {
      const double h = ctl.edge_sizes.count(e) ? ctl.edge_sizes.at(e) : size;
      n = std::max(1, static_cast<int>(std::ceil(len / h - 1e-9)));
    }
    const int samples = order == 2 ? 2 * n : n;  // 2차: 가운데 절점도 곡선 위의 등간격 점
    const std::vector<double> pts = geometry_edge_points(a, part.id, e, {0, 0, 0}, samples);
    const double tol = 1e-6 * (len + 1.0);
    std::vector<Id> chain;
    std::vector<Id> edge_nodes;
    for (int i = 0; i <= samples; ++i) {
      const Vec3 p{pts[3 * static_cast<std::size_t>(i)], pts[3 * static_cast<std::size_t>(i) + 1], pts[3 * static_cast<std::size_t>(i) + 2]};
      Id id = 0;
      if (i == 0 || i == samples) {  // 끝점: 꼭짓점 노드를 공유
        const std::size_t v = vertex_at(p, tol);
        if (v && vertex_node[v - 1]) id = vertex_node[v - 1];
        else {
          id = new_node(p);
          if (v) vertex_node[v - 1] = id, assoc["vertices"][std::to_string(v)] = id;
        }
      } else {
        id = new_node(p);
      }
      chain.push_back(id);
      if (i != 0 && i != samples) edge_nodes.push_back(id);
    }
    std::vector<Id> edge_elems;  // 이 모서리의 선 요소(보 프로퍼티·선하중의 대상)
    for (int k = 0; k < n; ++k) {
      Element el;
      el.id = next_elem++, el.part = mp;
      edge_elems.push_back(el.id);
      if (order == 2) {
        el.shape = Shape::Line3;
        el.nodes = {chain[2 * static_cast<std::size_t>(k)], chain[2 * static_cast<std::size_t>(k) + 1], chain[2 * static_cast<std::size_t>(k) + 2]};
      } else {
        el.shape = Shape::Line2;
        el.nodes = {chain[static_cast<std::size_t>(k)], chain[static_cast<std::size_t>(k) + 1]};
      }
      elems.push_back(std::move(el));
    }
    assoc["edges"][std::to_string(e)] = edge_nodes;
    assoc["edge_elements"][std::to_string(e)] = edge_elems;
  }
  m.add_nodes(node_ids, node_xyz);
  m.add_elements(elems);
  Json stored{{"dimension", 1}, {"order", order}, {"size", size}};
  if (has(params, "element_type")) stored["element_type"] = params["element_type"];
  a.invoke("mesh_part.update", Json{{"id", mp}, {"mesh_params", stored}, {"geometry_digest", geometry_digest(a, part.id)}, {"association", assoc}});
  if (has(params, "element_type")) a.invoke("mesh.set_element_type", Json{{"part", mp}, {"type", params["element_type"]}});
  return Json{{"mesh_part", mp}, {"nodes", node_ids.size()}, {"elements", elems.size()}, {"shape", order == 2 ? "line3" : "line2"},
              {"first_node", node_ids.empty() ? 0 : node_ids.front()}, {"first_element", elems.empty() ? 0 : elems.front().id},
              {"size", size}, {"mesher", "lines"}};
}

Json generate(App& a, const Object& part, const Json& params) {
  if (params.value("method", std::string("auto")) == "hex_mapped") return generate_mapped(a, part, params);
  {
    const Json counts = geometry_counts(a, part.id);
    const int dim = params.value("dimension", counts["solids"].get<int>() > 0 ? 3 : counts["faces"].get<int>() > 0 ? 2 : 1);
    if (dim == 1) return generate_lines(a, part, params);
  }
  const MesherRequest rq = prepare_request(a, part, params);
  const MesherOutput out = run_mesher(a, part.id, rq);
  return apply_mesh(a, part, params, rq, out);
}

// ------------------------------------------------------------ 배경 메싱(아키텍처 규칙 5)
// 메셔는 작업 스레드에서 돌고, 결과는 주 스레드가 mesh.job_finish 로 모델에 넣는다(변경은 명령 안에서만).
// 메셔의 전역 상태 때문에 작업은 한 번에 하나다.
struct MeshJob {
  Id part = 0;
  Json params;
  MesherRequest rq;
  std::string digest;  // 시작할 때의 형상 다이제스트(끝날 때 형상이 바뀌어 있으면 결과를 버린다)
  std::thread thread;
  std::atomic<int> state{0};  // 0 = 돌고 있음, 1 = 끝남, 2 = 실패, 3 = 멈춤
  MesherOutput out;
  std::string code, message;
  std::chrono::steady_clock::time_point started, ended;
  bool applied = false;
  ~MeshJob() {
    if (thread.joinable()) mesher_cancel(), thread.join();
  }
};

std::shared_ptr<MeshJob>& job_slot(App& a) {
  std::any& slot = a.runtime("mesh_job");
  if (!slot.has_value()) slot = std::shared_ptr<MeshJob>();
  return std::any_cast<std::shared_ptr<MeshJob>&>(slot);
}

const char* state_name(int s) { return s == 0 ? "running" : s == 1 ? "done" : s == 2 ? "failed" : "cancelled"; }

Json job_status(App& a) {
  const std::shared_ptr<MeshJob>& job = job_slot(a);
  if (!job) return Json{{"state", "none"}};
  const int s = job->state.load();
  const auto end = s == 0 ? std::chrono::steady_clock::now() : job->ended;
  Json r{{"state", state_name(s)}, {"part", job->part}, {"applied", job->applied},
         {"elapsed", std::chrono::duration<double>(end - job->started).count()}};
  if (s == 0) {
    const MesherProgress p = mesher_progress();
    r["percent"] = p.percent, r["task"] = p.task;
  } else {
    r["percent"] = s == 1 ? 100.0 : 0.0;
    if (s == 1) r["nodes"] = job->out.xyz.size() / 3, r["elements"] = (job->rq.dimension == 3 ? job->out.volume : job->out.surface).tag.size();
    if (s >= 2) r["error"] = Json{{"code", job->code}, {"message", job->message}};
  }
  return r;
}

Json start_job(App& a, const Object& part, const Json& params) {
  std::shared_ptr<MeshJob>& slot = job_slot(a);
  if (slot && slot->state.load() == 0) throw Error("busy", "메싱이 이미 돌고 있습니다(mesh.job_status)", {{"part", slot->part}});
  auto job = std::make_shared<MeshJob>();
  job->part = part.id, job->params = params;
  job->rq = prepare_request(a, part, params);
  job->digest = geometry_digest(a, part.id);
  const MesherGeometry geometry = prepare_mesher_geometry(a, part.id);
  job->started = std::chrono::steady_clock::now();
  slot = job;
  job->thread = std::thread([job, geometry] {
    try {
      job->out = run_mesher(geometry, job->rq);
      job->ended = std::chrono::steady_clock::now();
      job->state = 1;
    } catch (const Error& e) {
      job->code = e.code(), job->message = e.what();
      job->ended = std::chrono::steady_clock::now();
      job->state = e.code() == "cancelled" ? 3 : 2;
    } catch (const std::exception& e) {
      job->code = "mesh_failed", job->message = e.what();
      job->ended = std::chrono::steady_clock::now();
      job->state = 2;
    }
  });
  return Json{{"job", "mesh"}, {"part", part.id}, {"state", "running"}, {"size", job->rq.max_size}};
}

Json finish_job(App& a) {
  std::shared_ptr<MeshJob>& slot = job_slot(a);
  if (!slot) throw Error("invalid_state", "돌고 있거나 끝난 메싱이 없습니다");
  std::shared_ptr<MeshJob> job = slot;
  if (job->state.load() == 0) throw Error("invalid_state", "메싱이 아직 돌고 있습니다(mesh.job_status 로 기다린다)", {{"part", job->part}});
  if (job->thread.joinable()) job->thread.join();
  if (job->applied) throw Error("invalid_state", "이미 모델에 넣은 메싱입니다");
  if (job->state.load() != 1) throw Error(job->code, job->message, {{"part", job->part}});
  const Object* part = a.model().find(job->part);
  if (!part || part->kind != "part") throw Error("not_found", "메싱한 형상 파트가 없어졌습니다", {{"part", job->part}});
  if (geometry_digest(a, part->id) != job->digest)
    throw Error("geometry_changed", "메싱하는 동안 형상이 바뀌었습니다(다시 메싱한다)", {{"part", job->part}});
  const Json r = apply_mesh(a, *part, job->params, job->rq, job->out);
  job->applied = true;
  job->out = MesherOutput();  // 메모리를 돌려준다
  return r;
}

const Json& association_of(const App& a, Id part) {
  const Object* mp = mesh_part_of(a, part);
  if (!mp || !has(mp->props, "association"))
    throw Error("not_available", "이 형상에는 메시가 없습니다(mesh.generate 로 먼저 메싱한다)", {{"object", part}});
  return mp->props["association"];
}

}  // namespace

Json resolve_geometry_target(const App& a, const Json& ids, const std::string& what) {
  std::set<Id> out;
  std::set<std::pair<Id, int>> faces;
  auto merge = [&](const Json& sub) {
    const Json r = resolve_target(a, sub, what);
    if (what == "faces")
      for (const Json& f : r) faces.insert({f[0].get<Id>(), f[1].get<int>()});
    else
      for (const Json& x : r) out.insert(x.get<Id>());
  };
  for (const Json& item : ids) {
    const Id part = item[0].get<Id>();
    const std::string type = item[1].get<std::string>();
    int index = item[2].get<int>();
    if (item.size() >= 4 && item[3].is_string()) {  // 영속 이름표(D9)가 있으면 지금 번호로
      const int by_name = geometry_entity_index(const_cast<App&>(a), part, type, item[3].get<std::string>());
      if (!by_name) throw Error("broken_target", "형상에 " + type + " '" + item[3].get<std::string>() + "' 이(가) 더는 없습니다(형상이 바뀜)", {{"object", part}, {"type", type}, {"name", item[3]}});
      index = by_name;
    }
    const std::string key = std::to_string(index);
    const Json& assoc = association_of(a, part);
    const bool solid_mesh = assoc.value("dimension", 3) == 3;
    auto list = [&](const char* group) -> const Json& {
      auto g = assoc.find(group);
      if (g == assoc.end() || !g->contains(key))
        throw Error("not_found", "메시에 대응하는 형상 엔티티가 없습니다: " + type + " " + key, {{"object", part}, {"type", type}, {"index", item[2]}});
      return (*g)[key];
    };
    if (type == "solid") {
      merge(Json{{"type", "elements"}, {"ids", list("solids")}});
    } else if (type == "face") {
      const Json& l = list("faces");
      if (solid_mesh) {
        Json pairs = Json::array();
        for (std::size_t i = 0; i + 1 < l.size(); i += 2) pairs.push_back(Json::array({l[i], l[i + 1]}));
        merge(Json{{"type", "faces"}, {"ids", pairs}});
      } else {
        merge(Json{{"type", "elements"}, {"ids", l}});
      }
    } else if (type == "edge") {
      if (what == "elements" && assoc.value("dimension", 3) == 1) {
        // 1차원 메시(보·트러스): 모서리의 요소 = 그 모서리를 나눈 선 요소(메싱 때 기록, 보 프로퍼티·선하중의 대상)
        merge(Json{{"type", "elements"}, {"ids", list("edge_elements")}});
      } else {
        merge(Json{{"type", "nodes"}, {"ids", list("edges")}});
      }
    } else {
      merge(Json{{"type", "nodes"}, {"ids", Json::array({list("vertices")})}});
    }
  }
  if (what == "faces") {
    Json arr = Json::array();
    for (const auto& [e, f] : faces) arr.push_back(Json::array({e, f}));
    return arr;
  }
  return Json(std::vector<Id>(out.begin(), out.end()));
}

void register_meshgen_commands(App& app) {
  {
    CommandSpec c = base("mesh.generate", 'J', "형상 파트를 메싱한다(솔리드는 사면체, 면은 삼각형). 이미 메싱돼 있으면 새로 만든다", "MSH-05~08, MSH-10");
    c.undoable = true;
    c.params = {F("id", "ref", "형상 파트").call_req(), F("size", "number", "요소 크기(없으면 경계 상자 대각선의 1/10)").gt(0).unit("length"),
                F("min_size", "number", "요소 크기의 하한").ge(0).unit("length"),
                F("grading", "number", "크기가 변하는 빠르기(0 에 가까울수록 고르게, 기본 0.3)").gt(0).le(1),
                F("order", "integer", "요소 차수(1 또는 2)").ge(1).le(2), F("dimension", "integer", "3 = 솔리드, 2 = 면, 1 = 모서리(선 요소. 면이 없는 파트의 기본)").ge(1).le(3),
                F("face_sizes", "table", "[면 번호, 크기] 의 목록(면별 크기)").columns(2).ex(Json::array({Json::array({1, 2.0})})),
                F("element_type", "string", "솔버 요소 타입(없으면 형상의 기본 타입)").ex("C3D10"),
                F("method", "string", "auto = 사면체(Netgen), hex_mapped = 매핑 육면체(육면체 위상의 솔리드 하나짜리 파트)").one_of({"auto", "tet", "hex_mapped"}),
                F("divisions", "integer_list", "hex_mapped: 세 방향의 분할 수 [ni, nj, nk](없으면 size 로 정한다)").ex({10, 4, 2}),
                F("background", "bool", "작업 스레드에서 메싱한다. 진행률은 mesh.job_status, 결과 반영은 mesh.job_finish(한 번에 하나)")};
    c.fn = [](App& a, const Json& p) {
      const Fields& specs = a.commands().at("mesh.generate").params;
      for (const FieldSpec& f : specs)
        if (f.name != "id" && has(p, f.name.c_str())) check_value(f, p[f.name], nullptr);
      const Object& part = part_of(a, p["id"].get<Id>());
      const bool lines = p.value("dimension", 0) == 1 || geometry_counts(a, part.id)["faces"].get<int>() == 0;
      if (p.value("background", false) && p.value("method", std::string("auto")) != "hex_mapped" && !lines) return start_job(a, part, p);  // 매핑·1D 는 빨라서 바로 한다
      return generate(a, part, p);
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("mesh.job_status", 'Q', "배경 메싱의 상태(running·done·failed·cancelled·none)·진행률·하는 일을 조회한다", "MSH-05, API-07, API-30");
    c.fn = [](App& a, const Json&) { return job_status(a); };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("mesh.job_finish", 'J', "끝난 배경 메싱의 결과를 모델에 넣는다(실패했으면 그 오류를 낸다)", "MSH-05, API-07, API-30");
    c.undoable = true;
    c.fn = [](App& a, const Json&) { return finish_job(a); };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("mesh.job_cancel", 'S', "돌고 있는 배경 메싱을 멈춘다", "MSH-05, API-07, API-30");
    c.fn = [](App& a, const Json&) {
      const std::shared_ptr<MeshJob>& job = job_slot(a);
      if (!job || job->state.load() != 0) return Json{{"cancelled", false}};
      mesher_cancel();
      return Json{{"cancelled", true}, {"part", job->part}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("mesh.remesh", 'J', "형상이 바뀐 파트를 앞서 쓴 설정으로 다시 메싱한다(파트를 주면 그 파트만)", "MSH-11, GEO-45");
    c.undoable = true;
    c.params = {F("id", "ref", "형상 파트(없으면 갱신이 필요한 모든 파트)")};
    c.fn = [](App& a, const Json& p) {
      Json done = Json::array();
      std::vector<Id> targets;
      if (has(p, "id")) {
        targets.push_back(part_of(a, p["id"].get<Id>()).id);
      } else {
        for (const Object* part : a.model().by_kind("part")) {
          const Object* mp = mesh_part_of(a, part->id);
          if (mp && mp->props.value("geometry_digest", std::string()) != geometry_digest(a, part->id)) targets.push_back(part->id);
        }
      }
      for (Id id : targets) {
        const Object* mp = mesh_part_of(a, id);
        if (!mp || !has(mp->props, "mesh_params")) throw Error("invalid_state", "앞서 메싱한 적이 없는 파트입니다", {{"object", id}});
        const Json params = mp->props["mesh_params"];
        done.push_back(generate(a, part_of(a, id), params));
      }
      return Json{{"remeshed", done}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("mesh.clear", 'C', "형상 파트의 메시를 지운다(메시 파트 객체와 프로퍼티 할당은 남는다)", "MSH-11");
    c.params = {F("id", "ref", "형상 파트").call_req()};
    c.fn = [](App& a, const Json& p) {
      const Object* mp = mesh_part_of(a, part_of(a, p["id"].get<Id>()).id);
      if (!mp) return Json{{"elements", 0}, {"nodes", 0}};
      const Id id = mp->id;
      const auto [elems, nodes] = clear_mesh(a, id);
      a.invoke("mesh_part.update", Json{{"id", id}, {"association", nullptr}, {"geometry_digest", nullptr}});
      return Json{{"elements", elems}, {"nodes", nodes}, {"mesh_part", id}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("mesh.status", 'Q', "형상 파트별 메시 상태(none·current·outdated)를 조회한다", "WT-29");
    c.fn = [](App& a, const Json&) {
      Json arr = Json::array();
      for (const Object* part : a.model().by_kind("part")) {
        Json row{{"part", part->id}, {"name", part->name}, {"state", "none"}, {"mesh_part", nullptr}, {"elements", 0}};
        if (const Object* mp = mesh_part_of(a, part->id)) {
          const std::size_t n = elements_in(a.mesh(), mp->id).size();
          row["mesh_part"] = mp->id, row["elements"] = n;
          if (n && has(mp->props, "association"))
            row["state"] = mp->props.value("geometry_digest", std::string()) == geometry_digest(a, part->id) ? "current" : "outdated";
        }
        arr.push_back(std::move(row));
      }
      return arr;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("set.from_geometry", 'C', "형상 엔티티에 놓인 노드·요소·요소면으로 셋을 만든다(만든 뒤에는 형상과 이어지지 않는 보통 셋이다)", "CMN-04, MSH-09");
    c.target = "set";
    c.params = {F("entities", "any", "[파트, solid|face|edge|vertex, 번호] 의 목록").call_req().ex(Json::array({Json::array({1, "face", 1})})),
                F("as", "string", "만들 셋의 종류").call_req().one_of({"node", "element", "surface"}).ex("node"),
                F("name", "string", "셋 이름(없으면 자동)")};
    c.fn = [](App& a, const Json& p) {
      check_value(a.commands().at("set.from_geometry").params[1], p["as"], nullptr);
      // 형상 대상과 같은 검사를 거친다
      FieldSpec spec("entities", "target", "");
      const Json target{{"type", "geometry"}, {"ids", p["entities"]}};
      check_value(spec, target, &a.model());
      const std::string as = p["as"].get<std::string>();
      Json q = Json::object();
      if (has(p, "name")) q["name"] = p["name"];
      if (as == "surface") {
        q["faces"] = resolve_geometry_target(a, p["entities"], "faces");
        if (q["faces"].empty()) throw Error("invalid_state", "대상에 요소면이 없습니다", {{"param", "entities"}});
        return a.invoke("set.create_surface", q);
      }
      q["ids"] = resolve_geometry_target(a, p["entities"], as == "node" ? "nodes" : "elements");
      if (q["ids"].empty()) throw Error("invalid_state", "대상에 노드(요소)가 없습니다", {{"param", "entities"}});
      return a.invoke(as == "node" ? "set.create_node" : "set.create_element", q);
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("mesh.association", 'Q', "형상 엔티티에 놓인 노드·요소·요소면을 조회한다", "MSH-09");
    c.params = {F("id", "ref", "형상 파트").call_req(), F("type", "string", "엔티티 종류").call_req().one_of({"solid", "face", "edge", "vertex"}).ex("face"),
                F("index", "integer", "엔티티 번호").call_req().ge(1).ex(1)};
    c.fn = [](App& a, const Json& p) {
      const Id part = part_of(a, p["id"].get<Id>()).id;
      check_value(a.commands().at("mesh.association").params[1], p["type"], nullptr);
      const Json ids = Json::array({Json::array({part, p["type"], p["index"]})});
      const std::string type = p["type"].get<std::string>();
      const bool solid_mesh = association_of(a, part).value("dimension", 3) == 3;
      Json out{{"nodes", resolve_geometry_target(a, ids, "nodes")}};
      if (type == "solid" || (type == "face" && !solid_mesh)) out["elements"] = resolve_geometry_target(a, ids, "elements");
      if (type == "face" && solid_mesh) out["faces"] = resolve_geometry_target(a, ids, "faces");
      return out;
    };
    app.register_command(std::move(c));
  }
}

}  // namespace nasa95
