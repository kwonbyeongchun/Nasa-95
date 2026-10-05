// 메시 명령 (`.agent/proj-api-list.md` 4절 중 형상·메셔가 필요 없는 것)
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>

#include "mesh_util.hpp"
#include "nasa95/app.hpp"
#include "nasa95/error.hpp"

namespace nasa95 {

namespace {

using namespace meshutil;

CommandSpec base(const std::string& name, char kind, const std::string& desc, const std::string& features) {
  CommandSpec c;
  c.name = name, c.kind = kind, c.undoable = (kind == 'C'), c.target = "mesh", c.desc = desc, c.features = features;
  return c;
}

// ------------------------------------------------------------------ 품질
struct Quality {
  double aspect = 1, skew = 0, warpage = 0, jacobian = 1, min_length = 0, size = 0;
};

double angle_deg(const Vec3& a, const Vec3& b) {
  const double c = dot(a, b) / (norm(a) * norm(b));
  return std::acos(std::max(-1.0, std::min(1.0, c))) * 180.0 / 3.14159265358979323846;
}

Quality quality_of(const Mesh& m, const Element& e) {
  const ShapeInfo& info = shape_info(e.shape);
  const std::vector<Vec3> p = corner_points(m, e);
  Quality q;
  q.size = element_size(info, p);
  double lo = 1e300, hi = 0;
  for (const auto& ed : shape_edges(e.shape)) {
    const double l = norm(sub(p[static_cast<std::size_t>(ed[1])], p[static_cast<std::size_t>(ed[0])]));
    lo = std::min(lo, l), hi = std::max(hi, l);
  }
  q.min_length = hi == 0 ? 0 : lo;
  q.aspect = lo > 0 ? hi / lo : 1e300;
  if (info.dim == 2) {
    // 내각이 이상적인 각(삼각형 60°, 사각형 90°)에서 벗어난 최대값
    const std::size_t n = p.size();
    const double ideal = n == 3 ? 60.0 : 90.0;
    for (std::size_t i = 0; i < n; ++i) {
      const double ang = angle_deg(sub(p[(i + 1) % n], p[i]), sub(p[(i + n - 1) % n], p[i]));
      q.skew = std::max(q.skew, std::fabs(ang - ideal));
    }
    if (n == 4) {  // 대각선으로 나눈 두 삼각형의 법선 사이 각
      const Vec3 n1 = cross(sub(p[1], p[0]), sub(p[2], p[0])), n2 = cross(sub(p[2], p[0]), sub(p[3], p[0]));
      if (norm(n1) > 0 && norm(n2) > 0) q.warpage = angle_deg(n1, n2);
    }
    q.jacobian = q.size > 0 ? 1.0 : 0.0;
  } else if (e.shape == Shape::Hex8 || e.shape == Shape::Hex20) {
    // 꼭짓점마다 세 변의 정규화된 삼중곱(1 = 직육면체, 0 이하 = 뒤집힘)
    static const int nb[8][3] = {{1, 3, 4}, {2, 0, 5}, {3, 1, 6}, {0, 2, 7}, {7, 5, 0}, {4, 6, 1}, {5, 7, 2}, {6, 4, 3}};
    double mn = 1e300;
    for (int c = 0; c < 8; ++c) {
      const Vec3 u = sub(p[nb[c][0]], p[c]), v = sub(p[nb[c][1]], p[c]), w = sub(p[nb[c][2]], p[c]);
      const double den = norm(u) * norm(v) * norm(w);
      mn = std::min(mn, den > 0 ? dot(u, cross(v, w)) / den : 0.0);
    }
    q.jacobian = mn;
  } else if (info.dim == 3) {
    q.jacobian = q.size > 0 ? 1.0 : (q.size < 0 ? -1.0 : 0.0);
  }
  return q;
}

Json quality_json(const Quality& q) {
  return Json{{"aspect", q.aspect}, {"skew", q.skew}, {"warpage", q.warpage},
              {"jacobian", q.jacobian}, {"min_length", q.min_length}, {"size", q.size}};
}

// ------------------------------------------------------------------ 면·변 추출
struct FaceKey {
  std::vector<Id> nodes;  // 정렬된 꼭짓점 절점
  bool operator<(const FaceKey& o) const { return nodes < o.nodes; }
};

// 한 번만 나오는 면(3D 요소) 또는 변(2D 요소)을 찾는다. 돌려주는 값: [요소, 면 번호(1 부터)].
std::vector<std::pair<Id, int>> free_boundaries(const Mesh& m, const std::vector<Id>& ids, int dim) {
  std::map<FaceKey, std::vector<std::pair<Id, int>>> seen;
  for (Id id : ids) {
    const Element e = m.element(id);
    if (shape_info(e.shape).dim != dim) continue;
    const auto& faces = shape_faces(e.shape);
    for (std::size_t f = 0; f < faces.size(); ++f) {
      FaceKey key;
      for (int k : faces[f]) key.nodes.push_back(e.nodes[static_cast<std::size_t>(k)]);
      std::sort(key.nodes.begin(), key.nodes.end());
      seen[key].push_back({id, static_cast<int>(f) + 1});
    }
  }
  std::vector<std::pair<Id, int>> out;
  for (const auto& [key, owners] : seen)
    if (owners.size() == 1) out.push_back(owners[0]);
  std::sort(out.begin(), out.end());
  return out;
}

Json pairs_json(const std::vector<std::pair<Id, int>>& v) {
  Json arr = Json::array();
  for (const auto& [e, f] : v) arr.push_back(Json::array({e, f}));
  return arr;
}

}  // namespace

void register_mesh_commands(App& app) {
  std::vector<std::string> shape_names;
  for (const ShapeInfo& i : all_shapes()) shape_names.push_back(i.name);

  // ---------------------------------------------------------------- 노드
  {
    CommandSpec c = base("mesh.nodes_create", 'C', "노드를 만든다(배열 입력 가능)", "MSH-13, API-09");
    c.params = {F("coords", "table", "좌표 [x, y, z] 의 목록").columns(3).call_req().unit("length").ex(Json::array({Json::array({0.0, 0.0, 0.0})})),
                F("ids", "integer_list", "노드 ID(없으면 가장 큰 ID 다음부터)").ge(1)};
    c.fn = [](App& a, const Json& p) {
      const std::vector<double> xyz = doubles(a, p, "coords");
      if (xyz.size() % 3 != 0) throw Error("invalid_param_type", "좌표는 노드마다 3개여야 합니다", {{"param", "coords"}});
      std::vector<Id> ids = id_list(a, p, "ids");
      if (ids.empty()) {
        Id next = a.mesh().max_node_id() + 1;
        for (std::size_t i = 0; i < xyz.size() / 3; ++i) ids.push_back(next++);
      }
      a.mesh().add_nodes(ids, xyz);
      Json r{{"count", ids.size()}, {"first", ids.empty() ? 0 : ids.front()}, {"last", ids.empty() ? 0 : ids.back()}};
      if (ids.size() <= 1000) r["ids"] = ids;
      return r;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("mesh.nodes_delete", 'C', "노드를 지운다(요소가 쓰는 노드는 지울 수 없다)", "MSH-13");
    c.params = {F("ids", "integer_list", "노드 ID").call_req().ex(Json::array({1}))};
    c.fn = [](App& a, const Json& p) {
      const std::vector<Id> ids = id_list(a, p, "ids");
      a.mesh().remove_nodes(ids);
      return Json{{"count", ids.size()}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("mesh.nodes_move", 'C', "노드를 옮긴다(새 좌표 또는 이동량)", "MSH-13, API-09");
    c.params = {F("ids", "integer_list", "노드 ID").call_req().ex(Json::array({1})),
                F("coords", "table", "새 좌표 [x, y, z] 의 목록").columns(3).unit("length"),
                F("translation", "vector3", "이동량(coords 대신)").unit("length")};
    c.fn = [](App& a, const Json& p) {
      const std::vector<Id> ids = id_list(a, p, "ids");
      std::vector<double> xyz;
      if (given(a, p, "coords")) {
        xyz = doubles(a, p, "coords");
      } else if (has(p, "translation")) {
        const Vec3 t = vec3(p["translation"]);
        for (Id id : ids) {
          const Vec3 q = add(a.mesh().node(id), t);
          xyz.insert(xyz.end(), q.begin(), q.end());
        }
      } else {
        throw Error("missing_param", "coords 또는 translation 이 필요합니다", {{"param", "coords"}});
      }
      a.mesh().move_nodes(ids, xyz);
      return Json{{"count", ids.size()}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("mesh.nodes_project", 'C', "노드를 평면에 투영한다", "MSH-13");
    c.params = {F("ids", "integer_list", "노드 ID").call_req().ex(Json::array({1})),
                F("point", "vector3", "평면 위의 점").call_req().unit("length").ex({0.0, 0.0, 0.0}),
                F("normal", "vector3", "평면의 법선").call_req().ex({0.0, 0.0, 1.0})};
    c.fn = [](App& a, const Json& p) {
      const std::vector<Id> ids = id_list(a, p, "ids");
      const Vec3 o = vec3(p["point"]), n = unit(vec3(p["normal"]), "normal");
      std::vector<double> xyz;
      for (Id id : ids) {
        const Vec3 x = a.mesh().node(id);
        const Vec3 q = sub(x, mul(n, dot(sub(x, o), n)));
        xyz.insert(xyz.end(), q.begin(), q.end());
      }
      a.mesh().move_nodes(ids, xyz);
      return Json{{"count", ids.size()}};
    };
    app.register_command(std::move(c));
  }

  // ---------------------------------------------------------------- 요소
  {
    CommandSpec c = base("mesh.elements_create", 'C', "요소를 만든다(배열 입력 가능)", "MSH-14, MSH-01, API-09");
    c.params = {F("shape", "string", "요소 형상").call_req().one_of(shape_names).ex("hex8"),
                F("connectivity", "table", "요소마다의 절점 ID 목록").call_req().ex(Json::array({Json::array({1, 2, 3, 4, 5, 6, 7, 8})})),
                F("ids", "integer_list", "요소 ID(없으면 가장 큰 ID 다음부터)").ge(1),
                F("part", "ref", "메시 파트").ref("mesh_part"),
                F("type", "string", "솔버 요소 타입(없으면 형상의 기본 타입)").ex("C3D8R")};
    c.fn = [](App& a, const Json& p) {
      const ShapeInfo* info = find_shape(p["shape"].get<std::string>());
      if (!info)
        throw Error("out_of_range", "알 수 없는 요소 형상: " + p["shape"].get<std::string>(), {{"param", "shape"}});
      const std::vector<Id> conn = id_list(a, p, "connectivity");
      const std::size_t n = static_cast<std::size_t>(info->nodes);
      if (conn.empty() || conn.size() % n != 0)
        throw Error("invalid_param_type", std::string(info->name) + " 요소의 절점은 " + std::to_string(n) + "개여야 합니다",
                    {{"param", "connectivity"}});
      const std::size_t count = conn.size() / n;
      std::vector<Id> ids = id_list(a, p, "ids");
      if (ids.empty()) {
        Id next = a.mesh().max_element_id() + 1;
        for (std::size_t i = 0; i < count; ++i) ids.push_back(next++);
      }
      if (ids.size() != count)
        throw Error("invalid_param_type", "ids 의 개수가 요소 수와 다릅니다", {{"param", "ids"}});
      Id part = 0;
      if (has(p, "part")) {
        check_value(F("part", "ref", "").ref("mesh_part"), p["part"], &a.model());
        part = p["part"].get<Id>();
      }
      const std::string type = p.value("type", std::string());
      std::vector<Element> elems(count);
      for (std::size_t i = 0; i < count; ++i) {
        elems[i].id = ids[i], elems[i].shape = info->shape, elems[i].part = part, elems[i].type = type;
        elems[i].nodes.assign(conn.begin() + static_cast<std::ptrdiff_t>(i * n), conn.begin() + static_cast<std::ptrdiff_t>((i + 1) * n));
      }
      a.mesh().add_elements(elems);
      Json r{{"count", count}, {"first", ids.front()}, {"last", ids.back()}};
      if (count <= 1000) r["ids"] = ids;
      return r;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("mesh.elements_delete", 'C', "요소를 지운다", "MSH-14");
    c.params = {F("ids", "integer_list", "요소 ID").call_req().ex(Json::array({1})),
                F("delete_unused_nodes", "bool", "쓰이지 않게 된 노드도 지운다")};
    c.fn = [](App& a, const Json& p) {
      const std::vector<Id> ids = id_list(a, p, "ids");
      std::set<Id> candidates;
      if (p.value("delete_unused_nodes", false))
        for (Id id : ids)
          for (Id n : a.mesh().element(id).nodes) candidates.insert(n);
      a.mesh().remove_elements(ids);
      std::vector<Id> gone;
      if (!candidates.empty()) {
        const std::vector<Id> used = a.mesh().used_nodes();
        for (Id n : candidates)
          if (n != 0 && !std::binary_search(used.begin(), used.end(), n)) gone.push_back(n);
        a.mesh().remove_nodes(gone);
      }
      return Json{{"count", ids.size()}, {"nodes_deleted", gone.size()}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("mesh.set_element_type", 'C', "요소에 솔버 요소 타입(적분 방식 포함)을 지정한다", "MSH-26, PRP-11");
    c.params = select_params();
    c.params.push_back(F("type", "string", "솔버 요소 타입(빈 문자열 = 형상의 기본 타입)").call_req().ex("C3D8R"));
    c.fn = [](App& a, const Json& p) {
      std::vector<Element> elems;
      for (Id id : select_elements(a, p)) {
        Element e = a.mesh().element(id);
        e.type = p["type"].get<std::string>();
        elems.push_back(std::move(e));
      }
      a.mesh().replace_elements(elems);  // 형상에 맞지 않는 타입이면 여기서 거부된다
      return Json{{"count", elems.size()}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("mesh.element_types", 'Q', "요소 형상별로 쓸 수 있는 솔버 요소 타입 목록을 조회한다", "MSH-27~31, MSH-34, MSH-35");
    c.fn = [](App&, const Json&) {
      Json arr = Json::array();
      for (const ShapeInfo& i : all_shapes()) {
        Json types = Json::array();
        for (const char* t : i.types) types.push_back(t);
        arr.push_back(Json{{"shape", i.name}, {"nodes", i.nodes}, {"dimension", i.dim}, {"order", i.order},
                           {"default_type", i.default_type}, {"types", types}});
      }
      return arr;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("mesh.flip_normals", 'C', "요소의 방향(쉘 법선)을 뒤집는다", "MSH-20");
    c.params = select_params();
    c.fn = [](App& a, const Json& p) {
      std::vector<Element> elems;
      for (Id id : select_elements(a, p)) {
        Element e = a.mesh().element(id);
        flip(e);
        elems.push_back(std::move(e));
      }
      a.mesh().replace_elements(elems);
      return Json{{"count", elems.size()}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("mesh.align_normals", 'C', "이웃한 쉘 요소의 법선 방향을 맞춘다", "MSH-20");
    c.params = select_params();
    c.fn = [](App& a, const Json& p) {
      const Mesh& m = a.mesh();
      std::map<Id, Element> elems;
      for (Id id : select_elements(a, p)) {
        Element e = m.element(id);
        if (shape_info(e.shape).dim == 2) elems.emplace(id, std::move(e));
      }
      // 변(정렬된 두 절점) → 그 변을 쓰는 요소
      std::map<std::pair<Id, Id>, std::vector<Id>> by_edge;
      auto edges_of = [](const Element& e) {
        std::vector<std::pair<Id, Id>> out;
        for (const auto& f : shape_faces(e.shape))
          out.push_back({e.nodes[static_cast<std::size_t>(f[0])], e.nodes[static_cast<std::size_t>(f[1])]});
        return out;
      };
      for (const auto& [id, e] : elems)
        for (auto [u, v] : edges_of(e)) by_edge[{std::min(u, v), std::max(u, v)}].push_back(id);
      std::set<Id> done;
      std::vector<Element> changed;
      for (const auto& [seed, unused] : elems) {
        (void)unused;
        if (done.count(seed)) continue;
        std::vector<Id> queue{seed};
        done.insert(seed);
        while (!queue.empty()) {
          const Id cur = queue.back();
          queue.pop_back();
          for (auto [u, v] : edges_of(elems.at(cur))) {
            for (Id nb : by_edge[{std::min(u, v), std::max(u, v)}]) {
              if (nb == cur || done.count(nb)) continue;
              // 방향이 맞으면 공유 변을 서로 반대 방향으로 돈다.
              bool same = false;
              for (auto [x, y] : edges_of(elems.at(nb)))
                if (x == u && y == v) same = true;
              if (same) {
                flip(elems.at(nb));
                changed.push_back(elems.at(nb));
              }
              done.insert(nb);
              queue.push_back(nb);
            }
          }
        }
      }
      a.mesh().replace_elements(changed);
      return Json{{"flipped", changed.size()}};
    };
    app.register_command(std::move(c));
  }

  // ---------------------------------------------------------------- 조회
  {
    CommandSpec c = base("mesh.nodes", 'Q', "노드 ID 와 좌표를 조회한다", "MSH-01, API-12, API-32");
    c.params = {F("ids", "integer_list", "노드 ID(없으면 전부)").ex(Json::array({1}))};
    c.fn = [](App& a, const Json& p) {
      const Mesh& m = a.mesh();
      Json ids = Json::array(), coords = Json::array();
      if (given(a, p, "ids")) {
        for (Id id : id_list(a, p, "ids")) {
          const Vec3 x = m.node(id);
          ids.push_back(id);
          coords.push_back(x);
        }
      } else {
        for (std::size_t i = 0; i < m.node_count(); ++i) {
          ids.push_back(m.node_ids()[i]);
          coords.push_back(Json::array({m.node_xyz()[3 * i], m.node_xyz()[3 * i + 1], m.node_xyz()[3 * i + 2]}));
        }
      }
      return Json{{"ids", ids}, {"coords", coords}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("mesh.elements", 'Q', "요소의 형상·타입·파트·절점을 조회한다", "MSH-01, API-12, API-32");
    c.params = select_params();
    c.fn = [](App& a, const Json& p) {
      Json arr = Json::array();
      for (Id id : select_elements(a, p)) arr.push_back(element_json(a.mesh().element(id)));
      return arr;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("mesh.statistics", 'Q', "요소 종류별 개수와 길이·넓이·부피 합계를 조회한다", "MSH-25");
    c.params = select_params();
    c.fn = [](App& a, const Json& p) {
      const Mesh& m = a.mesh();
      std::map<std::string, std::size_t> by_shape, by_type;
      double length = 0, area = 0, volume = 0;
      std::set<Id> nodes;
      const std::vector<Id> ids = select_elements(a, p);
      for (Id id : ids) {
        const Element e = m.element(id);
        const ShapeInfo& info = shape_info(e.shape);
        ++by_shape[info.name];
        ++by_type[e.type.empty() ? std::string(info.default_type) : e.type];
        for (Id n : e.nodes) nodes.insert(n);
        if (std::find(e.nodes.begin(), e.nodes.end(), Id{0}) != e.nodes.end()) continue;  // 입구·출구 요소
        const double s = element_size(info, corner_points(m, e));
        (info.dim == 1 ? length : info.dim == 2 ? area : volume) += s;
      }
      nodes.erase(0);
      const bool whole = !given(a, p, "ids") && !has(p, "part");
      return Json{{"nodes", whole ? m.node_count() : nodes.size()}, {"elements", ids.size()}, {"by_shape", by_shape},
                  {"by_type", by_type}, {"length", length}, {"area", area}, {"volume", volume}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("mesh.find", 'Q', "조건(위치·형상·파트)으로 노드·요소를 찾는다", "MSH-17, MSH-19");
    c.params = {F("what", "string", "찾을 것").call_req().one_of({"nodes", "elements"}).ex("nodes"),
                F("box_min", "vector3", "범위의 최소 좌표").unit("length").ex({-1.0, -1.0, -1.0}),
                F("box_max", "vector3", "범위의 최대 좌표").unit("length").ex({1.0, 1.0, 1.0}),
                F("shape", "string", "요소 형상").one_of(shape_names), F("part", "ref", "메시 파트").ref("mesh_part")};
    c.fn = [](App& a, const Json& p) {
      const Mesh& m = a.mesh();
      check_value(a.commands().at("mesh.find").params[0], p["what"], nullptr);
      const bool box = has(p, "box_min") && has(p, "box_max");
      const Vec3 lo = box ? vec3(p["box_min"]) : Vec3{}, hi = box ? vec3(p["box_max"]) : Vec3{};
      auto inside = [&](const Vec3& x) {
        return !box || (x[0] >= lo[0] && x[0] <= hi[0] && x[1] >= lo[1] && x[1] <= hi[1] && x[2] >= lo[2] && x[2] <= hi[2]);
      };
      Json ids = Json::array();
      if (p["what"] == "nodes") {
        for (std::size_t i = 0; i < m.node_count(); ++i)
          if (inside({m.node_xyz()[3 * i], m.node_xyz()[3 * i + 1], m.node_xyz()[3 * i + 2]})) ids.push_back(m.node_ids()[i]);
      } else {
        const ShapeInfo* shape = has(p, "shape") ? find_shape(p["shape"].get<std::string>()) : nullptr;
        const Id part = has(p, "part") ? p["part"].get<Id>() : 0;
        for (std::size_t i = 0; i < m.element_count(); ++i) {
          if (shape && m.shape_at(i) != shape->shape) continue;
          if (part && m.part_at(i) != part) continue;
          if (box) {  // 요소의 중심이 범위 안
            const Element e = m.element_at(i);
            Vec3 cen{0, 0, 0};
            std::size_t n = 0;
            for (Id nd : e.nodes)
              if (nd != 0) cen = add(cen, m.node(nd)), ++n;
            if (n == 0 || !inside(mul(cen, 1.0 / static_cast<double>(n)))) continue;
          }
          ids.push_back(m.element_ids()[i]);
        }
      }
      return Json{{"ids", ids}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("mesh.free_faces", 'Q', "솔리드 요소의 외부 면([요소, 면 번호])을 추출한다", "MSH-22");
    c.params = select_params();
    c.fn = [](App& a, const Json& p) {
      const auto faces = free_boundaries(a.mesh(), select_elements(a, p), 3);
      return Json{{"count", faces.size()}, {"faces", pairs_json(faces)}};
    };
    app.register_command(std::move(c));
  }
  {
    // 품질 개선(MSH-18): 안쪽 노드의 라플라스 스무딩. 경계(자유 면·자유 변) 위의 노드와 keep 노드는 두고, 옮긴 뒤 붙은 요소의
    // 품질이 나빠지면(뒤집힘, 종횡비 악화) 그 노드는 되돌린다. 2차 요소의 중간 절점은 꼭짓점을 옮긴 뒤 변의 중점으로 둔다.
    CommandSpec c = base("mesh.improve", 'C', "라플라스 스무딩으로 안쪽 노드를 옮겨 품질을 개선한다(경계 노드·keep 노드는 두고, 나빠지는 이동은 되돌림)", "MSH-18");
    c.params = select_params();
    c.params.push_back(F("iterations", "integer", "반복 횟수(기본 5)").ge(1));
    c.params.push_back(F("relaxation", "number", "이동 비율(0~1, 기본 0.5)").gt(0).le(1));
    c.params.push_back(F("keep", "integer_list", "옮기지 않을 노드").nodes());
    c.fn = [](App& a, const Json& p) {
      Mesh& m = a.mesh();
      const std::vector<Id> ids = select_elements(a, p);
      const int iterations = p.value("iterations", 5);
      const double relax = p.value("relaxation", 0.5);
      // 노드 → 붙은 요소, 노드 → 변으로 이어진 꼭짓점 이웃
      std::map<Id, std::vector<Id>> elems_of, nbrs;
      std::set<Id> fixed;
      for (const Json& k : p.value("keep", Json::array())) fixed.insert(k.get<Id>());
      int dim = 0;
      std::map<Id, std::pair<Id, Id>> mid_of;  // 중간 절점 → 양 끝 꼭짓점
      for (Id id : ids) {
        const Element e = m.element(id);
        const ShapeInfo& info = shape_info(e.shape);
        if (info.dim < 2 || std::find(e.nodes.begin(), e.nodes.end(), Id{0}) != e.nodes.end()) continue;
        dim = std::max(dim, info.dim);
        for (Id n : e.nodes) elems_of[n].push_back(id);
        const std::vector<int> corners = corner_positions(e.shape);
        const auto& edges = shape_edges(e.shape);
        // 2차 요소의 중간 절점은 꼭짓점 뒤에 변 순서로 온다(요소 형상 표의 변 순서 = 중간 절점 순서)
        const bool quadratic = e.nodes.size() == corners.size() + edges.size();
        for (std::size_t ei = 0; ei < edges.size(); ++ei) {
          const Id u = e.nodes[static_cast<std::size_t>(edges[ei][0])], v = e.nodes[static_cast<std::size_t>(edges[ei][1])];
          nbrs[u].push_back(v), nbrs[v].push_back(u);
          if (quadratic) mid_of[e.nodes[corners.size() + ei]] = {u, v};
        }
        for (std::size_t k = corners.size(); k < e.nodes.size(); ++k)
          if (!mid_of.count(e.nodes[k])) fixed.insert(e.nodes[k]);  // 변의 중점이 아닌 고차 절점은 고정
      }
      if (dim == 0) throw Error("invalid_state", "스무딩할 2D·3D 요소가 없습니다");
      // 경계 노드: 3D 는 자유 면, 2D 는 자유 변 위의 노드
      for (const auto& [eid, f] : free_boundaries(m, ids, dim)) {
        const Element e = m.element(eid);
        const auto& faces = shape_faces(e.shape);  // 2D 요소면 변 목록
        for (int k : faces[static_cast<std::size_t>(f - 1)]) fixed.insert(e.nodes[static_cast<std::size_t>(k)]);
      }
      // 2D 요소가 3D 요소와 섞여 있으면 2D 요소의 노드는 모두 둔다(면 위에 있어야 하므로)
      if (dim == 3)
        for (Id id : ids) {
          const Element e = m.element(id);
          if (shape_info(e.shape).dim == 2)
            for (Id n : e.nodes) fixed.insert(n);
        }
      auto worst_of = [&](const std::vector<Id>& elems) {
        double jac = 1e300, aspect = 0;
        for (Id eid : elems) {
          const Quality q = quality_of(m, m.element(eid));
          jac = std::min(jac, q.jacobian), aspect = std::max(aspect, q.aspect);
        }
        return std::make_pair(jac, aspect);
      };
      const auto before = worst_of(ids);
      std::set<Id> moved;
      for (int it = 0; it < iterations; ++it) {
        for (const auto& [n, nb] : nbrs) {
          if (fixed.count(n) || mid_of.count(n) || nb.empty()) continue;
          Vec3 cen{0, 0, 0};
          for (Id v : nb) cen = add(cen, m.node(v));
          cen = mul(cen, 1.0 / static_cast<double>(nb.size()));
          const Vec3 old = m.node(n);
          const Vec3 target = add(old, mul(sub(cen, old), relax));
          const auto prev = worst_of(elems_of[n]);
          m.move_nodes({n}, {target[0], target[1], target[2]});
          // 붙은 중간 절점을 변의 중점으로
          std::vector<Id> mids;
          std::vector<double> mxyz;
          for (Id eid : elems_of[n])
            for (Id q : m.element(eid).nodes) {
              auto mi = mid_of.find(q);
              if (mi == mid_of.end() || (mi->second.first != n && mi->second.second != n)) continue;
              if (std::find(mids.begin(), mids.end(), q) != mids.end()) continue;
              const Vec3 c = mul(add(m.node(mi->second.first), m.node(mi->second.second)), 0.5);
              mids.push_back(q), mxyz.insert(mxyz.end(), {c[0], c[1], c[2]});
            }
          if (!mids.empty()) m.move_nodes(mids, mxyz);
          const auto now = worst_of(elems_of[n]);
          if (now.first <= 0 || now.first < prev.first - 1e-12 || now.second > prev.second + 1e-9) {
            // 나빠졌다: 되돌린다
            m.move_nodes({n}, {old[0], old[1], old[2]});
            if (!mids.empty()) {
              std::vector<double> back;
              for (Id q : mids) {
                const Vec3 c = mul(add(m.node(mid_of[q].first), m.node(mid_of[q].second)), 0.5);
                back.insert(back.end(), {c[0], c[1], c[2]});
              }
              m.move_nodes(mids, back);
            }
            continue;
          }
          moved.insert(n);
        }
      }
      const auto after = worst_of(ids);
      return Json{{"elements", ids.size()}, {"iterations", iterations}, {"moved", moved.size()}, {"fixed", fixed.size()},
                  {"before", Json{{"jacobian", before.first}, {"aspect", before.second}}},
                  {"after", Json{{"jacobian", after.first}, {"aspect", after.second}}}};
    };
    app.register_command(std::move(c));
  }
  {
    // 접촉 자동 탐지(BC-11): 솔리드 요소의 외부 면 가운데 다른 파트의 면과 가까이(거리 ≤ tolerance) 마주 보는(법선이 반대) 면을 찾아
    // 파트 쌍마다 후보로 묶는다. 면의 중심에서 상대 면 평면까지의 거리로 판단한다.
    CommandSpec c = base("contact.detect", 'Q', "파트 사이에서 거리 기준으로 마주 보는 외부 면을 찾아 접촉·타이 쌍 후보를 돌려준다", "BC-11");
    c.target = "";
    c.params = {F("tolerance", "number", "면 사이 거리 상한").call_req().gt(0).unit("length").ex(0.1),
                F("angle", "number", "법선이 반대인지 판정할 각도 허용(도, 기본 30)").ge(0).le(90),
                F("parts", "ref_list", "탐지할 파트(없으면 전부)").ref("mesh_part")};
    c.fn = [](App& a, const Json& p) {
      const Mesh& m = a.mesh();
      const double tol = p["tolerance"].get<double>();
      const double cos_tol = std::cos(p.value("angle", 30.0) * 3.14159265358979323846 / 180.0);
      std::set<Id> only;
      for (const Json& x : p.value("parts", Json::array())) only.insert(x.get<Id>());
      // 파트별 요소
      std::map<Id, std::vector<Id>> by_part;
      for (std::size_t i = 0; i < m.element_count(); ++i) {
        const Id part = m.part_at(i);
        if (part == 0 || (!only.empty() && !only.count(part))) continue;
        by_part[part].push_back(m.element_ids()[i]);
      }
      if (by_part.size() < 2) throw Error("invalid_state", "접촉을 찾으려면 파트가 둘 이상 있어야 합니다", {{"parts", by_part.size()}});
      struct Face {
        Id elem;
        int number;
        Vec3 center, normal;
        double size;
        std::vector<Id> nodes;
        std::vector<Vec3> corners;
      };
      // 점을 면의 평면에 투영했을 때 볼록 다각형 안에 있는지(변마다 안쪽 판정)
      auto inside = [](const Face& f, const Vec3& q) {
        const std::size_t n = f.corners.size();
        for (std::size_t i = 0; i < n; ++i) {
          const Vec3 e = sub(f.corners[(i + 1) % n], f.corners[i]);
          const Vec3 w = sub(q, f.corners[i]);
          if (dot(cross(e, w), f.normal) < -1e-9 * f.size) return false;
        }
        return true;
      };
      std::map<Id, std::vector<Face>> faces;
      for (const auto& [part, ids] : by_part) {
        for (const auto& [eid, f] : free_boundaries(m, ids, 3)) {
          const Element e = m.element(eid);
          const auto& fn = shape_faces(e.shape)[static_cast<std::size_t>(f - 1)];
          Face face{eid, f, {0, 0, 0}, {0, 0, 0}, 0, {}};
          std::vector<Vec3> pts;
          for (int k : fn) {
            face.nodes.push_back(e.nodes[static_cast<std::size_t>(k)]);
            pts.push_back(m.node(e.nodes[static_cast<std::size_t>(k)]));
          }
          // 꼭짓점만(2차 요소의 면 목록은 꼭짓점이 먼저 온다)
          const std::size_t nc = pts.size() >= 6 ? (pts.size() == 6 ? 3 : 4) : pts.size();
          std::vector<Vec3> corners(pts.begin(), pts.begin() + static_cast<std::ptrdiff_t>(nc));
          for (const Vec3& q : corners) face.center = add(face.center, q);
          face.center = mul(face.center, 1.0 / static_cast<double>(nc));
          const Vec3 na = face_normal_area(corners);
          face.size = norm(na);
          if (face.size <= 0) continue;
          face.normal = mul(na, 1.0 / face.size);
          face.corners = std::move(corners);
          faces[part].push_back(std::move(face));
        }
      }
      Json pairs = Json::array();
      std::vector<Id> parts;
      for (const auto& [part, _] : faces) parts.push_back(part);
      for (std::size_t i = 0; i < parts.size(); ++i)
        for (std::size_t j = i + 1; j < parts.size(); ++j) {
          const auto& fa = faces[parts[i]];
          const auto& fb = faces[parts[j]];
          std::set<std::pair<Id, int>> sa, sb;
          double dmin = 1e300, dmax = 0;
          for (const Face& x : fa)
            for (const Face& y : fb) {
              if (dot(x.normal, y.normal) > -cos_tol) continue;  // 마주 보지 않음
              const Vec3 d = sub(y.center, x.center);
              const double gap = std::fabs(dot(d, x.normal));
              // 겹침: 한쪽 면의 중심이 상대 면(평면에 투영) 안에 있어야 한다
              if (gap > tol || !(inside(x, y.center) || inside(y, x.center))) continue;
              sa.insert({x.elem, x.number}), sb.insert({y.elem, y.number});
              dmin = std::min(dmin, gap), dmax = std::max(dmax, gap);
            }
          if (sa.empty()) continue;
          Json fa_json = Json::array(), fb_json = Json::array();
          for (const auto& [e, f] : sa) fa_json.push_back(Json::array({e, f}));
          for (const auto& [e, f] : sb) fb_json.push_back(Json::array({e, f}));
          // 면이 적은 쪽(더 거친 쪽)을 주 면으로 — 솔버 권장: 주 면은 거친 쪽
          const bool a_master = sa.size() <= sb.size();
          pairs.push_back(Json{{"part_a", parts[i]}, {"part_b", parts[j]}, {"faces_a", fa_json}, {"faces_b", fb_json},
                               {"master", a_master ? "a" : "b"}, {"gap_min", dmin}, {"gap_max", dmax}, {"count", sa.size() + sb.size()}});
        }
      return Json{{"tolerance", tol}, {"pairs", pairs}};
    };
    app.register_command(std::move(c));
  }
  {
    // 탐지 결과로 접촉 쌍 또는 타이 구속을 만든다(BC-11): 면 셋 둘(주·종속)을 만들고 그것을 가리키는 객체를 만든다
    CommandSpec c = base("contact.create_from_detection", 'C', "contact.detect 의 후보(pair)로 면 셋 둘과 접촉 쌍(interaction 필요) 또는 타이 구속을 만든다", "BC-11");
    c.target = "";
    c.params = {F("pair", "object", "contact.detect 가 돌려준 pairs 의 항목 하나").call_req().ex(Json::object()),
                F("kind", "string", "만들 것(기본 contact)").one_of({"contact", "tie"}).ex("tie"),
                F("interaction", "ref", "contact: 접촉 속성").ref("contact_property"),
                F("name", "string", "이름(셋은 <이름>_master/_slave)").ex("contact1"),
                F("position_tolerance", "number", "tie: 위치 허용 오차").ge(0).unit("length")};
    c.fn = [](App& a, const Json& p) {
      const Json& pair = p["pair"];
      for (const char* key : {"part_a", "part_b", "faces_a", "faces_b"})
        if (!pair.contains(key)) throw Error("invalid_param", std::string("pair 에 ") + key + " 가 없습니다", {{"param", "pair"}});
      const std::string kind = p.value("kind", std::string("contact"));
      if (kind == "contact" && !has(p, "interaction")) throw Error("missing_param", "접촉 쌍에는 interaction(접촉 속성)이 필요합니다", {{"param", "interaction"}});
      const bool a_master = pair.value("master", std::string("a")) == "a";
      const std::string name = p.value("name", std::string(kind == "tie" ? "tie" : "contact") + "_" + std::to_string(pair["part_a"].get<Id>()) + "_" +
                                                       std::to_string(pair["part_b"].get<Id>()));
      const Json master_faces = a_master ? pair["faces_a"] : pair["faces_b"], slave_faces = a_master ? pair["faces_b"] : pair["faces_a"];
      const Id master = a.invoke("set.create_surface", Json{{"name", name + "_master"}, {"faces", master_faces}})["id"].get<Id>();
      const Id slave = a.invoke("set.create_surface", Json{{"name", name + "_slave"}, {"faces", slave_faces}})["id"].get<Id>();
      Json q{{"name", name}, {"slave", Json{{"type", "set"}, {"ids", Json::array({slave})}}}, {"master", Json{{"type", "set"}, {"ids", Json::array({master})}}}};
      Id id;
      if (kind == "tie") {
        if (has(p, "position_tolerance")) q["position_tolerance"] = p["position_tolerance"];
        id = a.invoke("constraint.create_tie", q)["id"].get<Id>();
      } else {
        q["interaction"] = p["interaction"];
        id = a.invoke("contact_pair.create", q)["id"].get<Id>();
      }
      return Json{{"id", id}, {"kind", kind}, {"master_set", master}, {"slave_set", slave}, {"master_faces", master_faces.size()}, {"slave_faces", slave_faces.size()}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("mesh.free_edges", 'Q', "쉘·평면 요소의 자유 경계([요소, 변 번호])를 추출한다", "MSH-22");
    c.params = select_params();
    c.fn = [](App& a, const Json& p) {
      const auto edges = free_boundaries(a.mesh(), select_elements(a, p), 2);
      return Json{{"count", edges.size()}, {"edges", pairs_json(edges)}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("mesh.quality", 'Q', "품질 지표를 계산하고 기준 미달 요소를 조회한다", "MSH-17");
    c.params = select_params();
    c.params.push_back(F("aspect_max", "number", "종횡비(가장 긴 변 / 가장 짧은 변) 상한").gt(0));
    c.params.push_back(F("skew_max", "number", "내각 편차(도) 상한").ge(0));
    c.params.push_back(F("warpage_max", "number", "뒤틀림 각(도) 상한").ge(0));
    c.params.push_back(F("jacobian_min", "number", "정규화 jacobian 하한"));
    c.params.push_back(F("values", "bool", "요소별 값도 돌려준다"));
    c.fn = [](App& a, const Json& p) {
      const Mesh& m = a.mesh();
      Json failed = Json::array(), values = Json::array();
      Quality worst;
      worst.jacobian = 1e300, worst.aspect = 0;
      const std::vector<Id> ids = select_elements(a, p);
      for (Id id : ids) {
        const Element e = m.element(id);
        if (shape_info(e.shape).dim == 0 || std::find(e.nodes.begin(), e.nodes.end(), Id{0}) != e.nodes.end()) continue;
        const Quality q = quality_of(m, e);
        worst.aspect = std::max(worst.aspect, q.aspect), worst.skew = std::max(worst.skew, q.skew);
        worst.warpage = std::max(worst.warpage, q.warpage), worst.jacobian = std::min(worst.jacobian, q.jacobian);
        auto fail = [&](const char* metric, double v) { failed.push_back(Json{{"id", id}, {"metric", metric}, {"value", v}}); };
        if (has(p, "aspect_max") && q.aspect > p["aspect_max"].get<double>()) fail("aspect", q.aspect);
        if (has(p, "skew_max") && q.skew > p["skew_max"].get<double>()) fail("skew", q.skew);
        if (has(p, "warpage_max") && q.warpage > p["warpage_max"].get<double>()) fail("warpage", q.warpage);
        if (has(p, "jacobian_min") && q.jacobian < p["jacobian_min"].get<double>()) fail("jacobian", q.jacobian);
        if (p.value("values", false)) {
          Json v = quality_json(q);
          v["id"] = id;
          values.push_back(v);
        }
      }
      Json r{{"count", ids.size()}, {"failed", failed},
             {"worst", Json{{"aspect", worst.aspect}, {"skew", worst.skew}, {"warpage", worst.warpage},
                            {"jacobian", ids.empty() ? 1.0 : worst.jacobian}}}};
      if (p.value("values", false)) r["values"] = values;
      return r;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("mesh.check", 'Q', "중복 요소·미참조 노드·뒤집힌 요소·법선 불일치·미지원 요소를 조회한다", "MSH-19, MSH-38");
    c.params = select_params();
    c.fn = [](App& a, const Json& p) {
      const Mesh& m = a.mesh();
      const std::vector<Id> ids = select_elements(a, p);
      Json duplicates = Json::array(), inverted = Json::array(), degenerate = Json::array(), unsupported = Json::array();
      std::map<std::vector<Id>, Id> seen;
      std::map<std::pair<Id, Id>, std::vector<std::pair<Id, bool>>> shell_edges;  // 변 → (요소, 정방향)
      for (Id id : ids) {
        const Element e = m.element(id);
        const ShapeInfo& info = shape_info(e.shape);
        if (info.types.empty()) unsupported.push_back(id);
        std::vector<Id> key = e.nodes;
        std::sort(key.begin(), key.end());
        if (std::adjacent_find(key.begin(), key.end()) != key.end() && key.front() != 0) degenerate.push_back(id);
        auto [it, fresh] = seen.emplace(key, id);
        if (!fresh) duplicates.push_back(Json::array({it->second, id}));
        if (std::find(e.nodes.begin(), e.nodes.end(), Id{0}) != e.nodes.end()) continue;
        if (info.dim == 3) {
          const double v = element_size(info, corner_points(m, e));
          if (v < 0) inverted.push_back(id);
          if (v == 0) degenerate.push_back(id);
        }
        if (info.dim == 2)
          for (const auto& f : shape_faces(e.shape)) {
            const Id u = e.nodes[static_cast<std::size_t>(f[0])], v = e.nodes[static_cast<std::size_t>(f[1])];
            shell_edges[{std::min(u, v), std::max(u, v)}].push_back({id, u < v});
          }
      }
      // 이웃한 두 쉘이 공유 변을 같은 방향으로 돌면 법선이 서로 반대다.
      std::set<Id> flipped;
      for (const auto& [edge, owners] : shell_edges)
        if (owners.size() == 2 && owners[0].second == owners[1].second) flipped.insert(owners[1].first);
      Json unreferenced = Json::array();
      if (!given(a, p, "ids") && !has(p, "part")) {
        const std::vector<Id> used = m.used_nodes();
        for (Id n : m.node_ids())
          if (!std::binary_search(used.begin(), used.end(), n)) unreferenced.push_back(n);
      }
      return Json{{"duplicate_elements", duplicates}, {"unreferenced_nodes", unreferenced}, {"inverted_elements", inverted},
                  {"degenerate_elements", degenerate}, {"unsupported_elements", unsupported},
                  {"inconsistent_normals", std::vector<Id>(flipped.begin(), flipped.end())},
                  {"free_edges", free_boundaries(m, ids, 2).size()},
                  {"ok", duplicates.empty() && unreferenced.empty() && inverted.empty() && degenerate.empty() &&
                             unsupported.empty() && flipped.empty()}};
    };
    app.register_command(std::move(c));
  }
}

}  // namespace nasa95
