// 메시 연산(병합·변환·돌출·회전·차수 변환·분할·세분화·연결 요소), 적용 대상 전개, 합력, 재번호.
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <tuple>

#include "mesh_util.hpp"
#include "nasa95/app.hpp"
#include "nasa95/error.hpp"
#include "nasa95/mesher.hpp"

namespace nasa95 {

namespace {

using namespace meshutil;

const double kPi = 3.14159265358979323846;

CommandSpec base(const std::string& name, char kind, const std::string& target, const std::string& desc,
                 const std::string& features) {
  CommandSpec c;
  c.name = name, c.kind = kind, c.undoable = (kind == 'C'), c.target = target, c.desc = desc, c.features = features;
  return c;
}

std::string subtype_of(const Object& o) { return o.props.value("type", std::string()); }

// 요소가 가리키는 노드 가운데 실제 노드(0 은 네트워크 요소의 입구·출구).
void append_nodes(std::set<Id>& out, const Element& e) {
  for (Id n : e.nodes)
    if (n != 0) out.insert(n);
}

// 새 노드를 만들고 ID 를 돌려준다.
struct NodeMaker {
  explicit NodeMaker(const Mesh& m) : next(m.max_node_id() + 1) {}
  Id make(const Vec3& p) {
    ids.push_back(next);
    xyz.insert(xyz.end(), p.begin(), p.end());
    return next++;
  }
  void commit(Mesh& m) { m.add_nodes(ids, xyz); }
  Id next;
  std::vector<Id> ids;
  std::vector<double> xyz;
};

// 1차 형상의 부피(넓이)가 음수면 뒤집는다.
void make_positive(const Mesh& m, Element& e, const std::map<Id, Vec3>& fresh) {
  if (shape_info(e.shape).dim != 3) return;
  std::vector<Vec3> p;
  for (int k : corner_positions(e.shape)) {
    const Id n = e.nodes[static_cast<std::size_t>(k)];
    auto it = fresh.find(n);
    p.push_back(it != fresh.end() ? it->second : m.node(n));
  }
  if (element_size(shape_info(e.shape), p) < 0) flip(e);
}

// 객체가 가리키는 노드·요소 번호를 새 번호로 바꾼다(재번호·병합).
void remap_object_refs(App& a, const std::map<Id, Id>& nodes, const std::map<Id, Id>& elems) {
  auto fix = [&](const std::string& what, Json& v) {
    if (!v.is_number_integer() || v.get<std::int64_t>() < 1) return;
    const auto& map = what == "node" ? nodes : elems;
    auto it = map.find(v.get<Id>());
    if (it != map.end()) v = it->second;
  };
  std::vector<Object> changed;
  for (const Object* o : a.model().all()) {
    Object copy = *o;
    if (const KindSpec* ks = a.schema().find(o->kind)) {
      for_each_mesh_ref(ks->fields_for(subtype_of(*o)), copy.props, fix);
    }
    if (o->kind == "mesh_part" && copy.props.contains("association") && copy.props["association"].is_object()) {
      // 형상-메시 연관: 솔리드의 요소, 면의 [요소, 면 번호] 쌍(솔리드 메시) 또는 요소(쉘 메시), 모서리·꼭짓점의 노드
      Json& as = copy.props["association"];
      const bool solid_mesh = as.value("dimension", 3) == 3;
      for (const char* group : {"solids", "faces", "edges", "vertices"}) {
        if (!as.contains(group)) continue;
        const std::string g = group;
        for (auto& [key, list] : as[group].items()) {
          if (g == "vertices") {
            fix("node", list);
            continue;
          }
          for (std::size_t i = 0; i < list.size(); ++i) {
            if (g == "faces" && solid_mesh && i % 2 == 1) continue;  // 면 번호
            fix(g == "edges" ? "node" : "element", list[i]);
          }
        }
      }
    } else if (o->kind == "settings") {
      if (copy.props.contains("normals"))
        for (Json& row : copy.props["normals"]) fix("element", row[0]), fix("node", row[1]);
      if (copy.props.contains("beam_directions"))
        for (Json& row : copy.props["beam_directions"]) fix("element", row[0]);
    }
    if (!(copy == *o)) changed.push_back(std::move(copy));
  }
  for (const Object& o : changed) a.model().replace(o);
}

}  // namespace

// ------------------------------------------------------------------ 적용 대상 전개
Json resolve_target(const App& a, const Json& target, const std::string& what) {
  const Mesh& m = a.mesh();
  if (!target.is_object() || !target.contains("type") || !target.contains("ids"))
    throw Error("invalid_state", "적용 대상이 지정되지 않았습니다");
  const std::string type = target["type"].get<std::string>();
  std::set<Id> ids;
  std::set<std::pair<Id, int>> faces;
  auto add_element_nodes = [&](Id e) { append_nodes(ids, m.element(e)); };
  auto add_face = [&](Id e, int f) {
    const Element el = m.element(e);
    const auto& table = shape_faces(el.shape);
    if (f < 1 || f > static_cast<int>(table.size()))
      throw Error("out_of_range", "요소 " + std::to_string(e) + " 에 없는 면 번호입니다: " + std::to_string(f),
                  {{"element", e}, {"face", f}});
    if (what == "faces") {
      faces.insert({e, f});
      return;
    }
    const std::vector<int>& corners = table[static_cast<std::size_t>(f - 1)];
    for (int k : corners) ids.insert(el.nodes[static_cast<std::size_t>(k)]);
    // 2차 요소: 그 면의 변에 놓인 중간 절점도 면의 노드다(중간 절점은 꼭짓점 뒤에 변 순서로 있다).
    const ShapeInfo& info = shape_info(el.shape);
    if (info.order == 2 && info.dim >= 2) {
      const auto& edges = shape_edges(el.shape);
      for (std::size_t i = 0; i < edges.size(); ++i) {
        const bool on_face = std::find(corners.begin(), corners.end(), edges[i][0]) != corners.end() &&
                             std::find(corners.begin(), corners.end(), edges[i][1]) != corners.end();
        if (on_face) ids.insert(el.nodes[static_cast<std::size_t>(info.corners) + i]);
      }
    }
  };
  auto elements_of_part = [&](Id part) {
    std::vector<Id> out;
    for (std::size_t i = 0; i < m.element_count(); ++i)
      if (m.part_at(i) == part) out.push_back(m.element_ids()[i]);
    return out;
  };
  auto need = [&](bool ok) {
    if (!ok) throw Error("invalid_state", type + " 대상은 " + what + " 로 풀 수 없습니다", {{"target_type", type}});
  };

  if (type == "nodes") {
    need(what == "nodes");
    for (const Json& n : target["ids"]) {
      if (!m.has_node(n.get<Id>())) throw Error("not_found", "노드가 없습니다: " + n.dump(), {{"node", n}});
      ids.insert(n.get<Id>());
    }
  } else if (type == "elements") {
    need(what != "faces");
    for (const Json& e : target["ids"]) {
      if (!m.has_element(e.get<Id>())) throw Error("not_found", "요소가 없습니다: " + e.dump(), {{"element", e}});
      if (what == "nodes") add_element_nodes(e.get<Id>()); else ids.insert(e.get<Id>());
    }
  } else if (type == "faces") {
    need(what != "elements");
    for (const Json& f : target["ids"]) add_face(f[0].get<Id>(), f[1].get<int>());
  } else if (type == "parts") {
    need(what != "faces");
    for (const Json& p : target["ids"])
      for (Id e : elements_of_part(p.get<Id>())) {
        if (what == "nodes") add_element_nodes(e); else ids.insert(e);
      }
  } else if (type == "set") {
    for (const Json& sid : target["ids"]) {
      const Object& set = a.model().get(sid.get<Id>());
      const std::string st = subtype_of(set);
      if (st == "node" || st == "node_surface") {
        need(what == "nodes");
        for (const Json& n : set.props.value("ids", Json::array())) ids.insert(n.get<Id>());
      } else if (st == "element") {
        need(what != "faces");
        for (const Json& e : set.props.value("ids", Json::array())) {
          if (what == "nodes") add_element_nodes(e.get<Id>()); else ids.insert(e.get<Id>());
        }
      } else if (st == "surface") {
        need(what != "elements");
        for (const Json& f : set.props.value("faces", Json::array())) add_face(f[0].get<Id>(), f[1].get<int>());
      } else {
        throw Error("not_available", "형상 셋은 형상-메시 연관이 있어야 풀 수 있습니다", {{"object", set.id}});
      }
    }
  } else {  // geometry: 형상-메시 연관으로 푼다
    return resolve_geometry_target(a, target["ids"], what);
  }
  if (what == "faces") {
    Json arr = Json::array();
    for (const auto& [e, f] : faces) arr.push_back(Json::array({e, f}));
    return arr;
  }
  return Json(std::vector<Id>(ids.begin(), ids.end()));
}

namespace {

// 억제되지 않은 프로퍼티의 할당을 요소별로 푼다: 요소 → 프로퍼티 ID.
std::map<Id, Id> property_of_elements(const App& a) {
  std::map<Id, Id> out;
  for (const Object* p : a.model().by_kind("property")) {
    if (p->suppressed || !has(p->props, "target")) continue;
    Json elems;
    try {
      elems = resolve_target(a, p->props["target"], "elements");
    } catch (const Error&) {
      continue;  // 풀 수 없는 대상(형상 등)은 건너뛴다
    }
    for (const Json& e : elems) out[e.get<Id>()] = p->id;
  }
  return out;
}

int property_dimension(const std::string& subtype) {
  if (subtype == "solid") return 3;
  if (subtype == "shell" || subtype == "composite" || subtype == "membrane") return 2;
  if (subtype == "beam" || subtype == "truss" || subtype == "spring" || subtype == "dashpot" || subtype == "gap" ||
      subtype == "fluid")
    return 1;
  if (subtype == "mass") return 0;
  return -1;  // user, substructure: 검사하지 않는다
}

// 스텝에서 유효한 하중 ID (commands_model.cpp 의 step.effective 를 쓴다)
std::vector<Id> effective_loads(App& a, Id step) {
  std::vector<Id> out;
  const Json effective = a.commands().at("step.effective").fn(a, Json{{"id", step}});  // 임시 객체를 바로 순회하면 안 된다
  for (const Json& l : effective["loads"]) out.push_back(l["id"].get<Id>());
  return out;
}

}  // namespace

void register_mesh_op_commands(App& app) {
  // ---------------------------------------------------------------- 외부 필드 매핑(LOD-14)
  {
    // 점 자료를 메시의 노드 또는 면 중심으로 보간한 값을 돌려준다. mapped_field 하중 객체(load)를 주면 그 객체의 점·대상·방법을 쓴다.
    // 덱에는 mapped_field 하중이 같은 보간으로 노드·면마다 쓰인다.
    CommandSpec c = base("load.map_field", 'Q', "",
                         "외부 점 자료 [x, y, z, 값] 을 메시의 노드(what=nodes) 또는 면 중심(what=faces)으로 보간한다(nearest·idw). "
                         "load 를 주면 그 mapped_field 하중의 점·대상·방법으로 계산한다",
                         "LOD-14");
    c.params = {F("load", "ref", "mapped_field 하중 객체").ref("load"),
                F("points", "table", "[x, y, z, 값] 점 자료").columns(4).ex(Json::array({Json::array({0.0, 0.0, 0.0, 1.0})})),
                F("target", "target", "대상 노드·면·셋·형상").one_of({"nodes", "faces", "set", "geometry"}),
                F("what", "string", "노드 값(nodes) 또는 면 중심 값(faces)").one_of({"nodes", "faces"}),
                F("method", "string", "보간 방법(기본 idw)").one_of({"nearest", "idw"}), F("power", "number", "idw 거듭제곱(기본 2)").gt(0),
                F("radius", "number", "idw 반지름").gt(0).unit("length")};
    c.fn = [](App& a, const Json& p) {
      Json q = p;
      std::string what = p.value("what", std::string());
      if (has(p, "load")) {
        const Object& l = a.model().get(p["load"].get<Id>());
        if (l.kind != "load" || l.props.value("type", std::string()) != "mapped_field")
          throw Error("wrong_kind", "mapped_field 하중 객체가 아닙니다", {{"object", l.id}, {"expected", "load.mapped_field"}});
        for (const char* key : {"points", "target", "method", "power", "radius"})
          if (!has(q, key) && has(l.props, key)) q[key] = l.props[key];
        if (what.empty()) what = l.props.value("quantity", std::string()) == "temperature" ? "nodes" : "faces";
      }
      if (what.empty()) what = "nodes";
      for (const char* key : {"points", "target"})
        if (!has(q, key)) throw Error("missing_param", std::string(key) + " 이(가) 필요합니다", {{"param", key}});
      std::vector<Vec3> pts;
      std::vector<double> vals;
      for (const Json& row : q["points"]) {
        if (!row.is_array() || row.size() != 4) throw Error("invalid_param_type", "점 자료는 [x, y, z, 값] 이어야 합니다", {{"param", "points"}});
        pts.push_back({row[0].get<double>(), row[1].get<double>(), row[2].get<double>()}), vals.push_back(row[3].get<double>());
      }
      const Mesh& m = a.mesh();
      const std::string method = q.value("method", std::string("idw"));
      const double power = q.value("power", 2.0), radius = q.value("radius", 0.0);
      if (what == "nodes") {
        Json ids = resolve_target(a, q["target"], "nodes");
        std::vector<Vec3> at;
        for (const Json& n : ids) at.push_back(m.node(n.get<Id>()));
        return Json{{"what", "nodes"}, {"ids", ids}, {"values", interpolate_points(pts, vals, at, method, power, radius)}, {"method", method}};
      }
      Json faces = resolve_target(a, q["target"], "faces");
      std::vector<Vec3> at;
      for (const Json& f : faces) {
        const Element el = m.element(f[0].get<Id>());
        const auto& table = shape_faces(el.shape);
        const int fn = f[1].get<int>();
        if (fn < 1 || fn > static_cast<int>(table.size())) throw Error("out_of_range", "없는 면 번호입니다", {{"element", el.id}, {"face", fn}});
        Vec3 c{0, 0, 0};
        for (int k : table[static_cast<std::size_t>(fn - 1)]) c = add(c, m.node(el.nodes[static_cast<std::size_t>(k)]));
        at.push_back(mul(c, 1.0 / static_cast<double>(table[static_cast<std::size_t>(fn - 1)].size())));
      }
      return Json{{"what", "faces"}, {"faces", faces}, {"values", interpolate_points(pts, vals, at, method, power, radius)}, {"method", method}};
    };
    app.register_command(std::move(c));
  }
  // ---------------------------------------------------------------- 균열면(MSH-37)
  {
    // 균열면 만들기: 요소면 집합(균열면) 위의 노드를 한쪽 요소들에 대해 복제해 메시를 가른다. 노드마다 그 노드를 쓰는 요소들을
    // 균열면이 아닌 면으로 이어지는 덩어리로 나누고, 균열면 요소(faces 에 적힌 쪽)가 든 덩어리는 원래 노드를, 나머지 덩어리는 새 노드를 쓴다.
    // 균열 앞선(면 집합의 가장자리) 노드는 둘레 요소가 균열 밖으로 이어져 한 덩어리라 저절로 나뉘지 않는다.
    CommandSpec c = base("mesh.create_crack", 'C', "mesh",
                         "균열면 메시를 만든다: faces(요소면 집합) 위의 노드를 반대쪽 요소들에 대해 복제해 메시를 가르고, 양쪽 면 셋(<이름>_a, <이름>_b)을 만든다. "
                         "균열 앞선 노드는 그대로 이어진다",
                         "MSH-37");
    c.params = {F("faces", "target", "균열면이 될 요소면(안쪽 면)").call_req().one_of({"faces", "set"}).ex(Json{{"type", "faces"}, {"ids", Json::array({Json::array({1, 2})})}}),
                F("name", "string", "면 셋 이름 접두(기본 crack)").ex("crack")};
    c.fn = [](App& a, const Json& p) {
      Mesh& m = a.mesh();
      const Json faces = resolve_target(a, p["faces"], "faces");
      if (faces.empty()) throw Error("empty_target", "균열면이 비었습니다", {{"param", "faces"}});
      // 균열면의 키(정렬한 노드)와 그 면을 가진 요소(a 쪽)
      std::map<std::vector<Id>, Id> crack;
      std::set<Id> crack_nodes;
      for (const Json& f : faces) {
        const Element e = m.element(f[0].get<Id>());
        const auto& table = shape_faces(e.shape);
        const int fn = f[1].get<int>();
        if (shape_info(e.shape).dim != 3 || fn < 1 || fn > static_cast<int>(table.size()))
          throw Error("invalid_param", "균열면은 솔리드 요소의 면이어야 합니다", {{"param", "faces"}, {"element", e.id}, {"face", fn}});
        std::vector<Id> key;
        for (int k : table[static_cast<std::size_t>(fn - 1)]) key.push_back(e.nodes[static_cast<std::size_t>(k)]), crack_nodes.insert(e.nodes[static_cast<std::size_t>(k)]);
        std::sort(key.begin(), key.end());
        crack[key] = e.id;
      }
      // 노드 → 요소, 요소 → 면 키 목록
      std::unordered_map<Id, std::vector<std::size_t>> elems_of;
      for (std::size_t i = 0; i < m.element_count(); ++i) {
        if (shape_info(m.shape_at(i)).dim != 3) continue;
        const Id* n = m.nodes_at(i);
        for (std::size_t k = 0; k < m.node_count_at(i); ++k)
          if (crack_nodes.count(n[k])) elems_of[n[k]].push_back(i);
      }
      auto face_keys = [&](std::size_t ei) {
        std::vector<std::vector<Id>> keys;
        const Element e = m.element_at(ei);
        for (const auto& fc : shape_faces(e.shape)) {
          std::vector<Id> key;
          for (int k : fc) key.push_back(e.nodes[static_cast<std::size_t>(k)]);
          std::sort(key.begin(), key.end());
          keys.push_back(std::move(key));
        }
        return keys;
      };
      // 노드마다 덩어리 나누기 → (요소, 노드) 쌍의 새 노드 번호
      std::map<std::pair<std::size_t, Id>, Id> remap;
      std::vector<Id> new_ids;
      std::vector<double> new_xyz;
      Id next = m.max_node_id() + 1;
      std::set<Id> b_elems;
      for (Id n : crack_nodes) {
        const std::vector<std::size_t>& around = elems_of[n];
        std::map<std::size_t, std::vector<std::vector<Id>>> keys;
        for (std::size_t ei : around) keys[ei] = face_keys(ei);
        // 균열면이 아닌 면을 공유하면 같은 덩어리
        std::map<std::size_t, int> comp;
        int ncomp = 0;
        for (std::size_t seed : around) {
          if (comp.count(seed)) continue;
          const int id = ncomp++;
          std::vector<std::size_t> stack{seed};
          comp[seed] = id;
          while (!stack.empty()) {
            const std::size_t cur = stack.back();
            stack.pop_back();
            for (std::size_t other : around) {
              if (comp.count(other)) continue;
              bool linked = false;
              for (const auto& k1 : keys[cur]) {
                if (crack.count(k1)) continue;
                if (std::find(k1.begin(), k1.end(), n) == k1.end()) continue;  // 이 노드를 품은 면만
                for (const auto& k2 : keys[other])
                  if (k1 == k2) linked = true;
              }
              if (linked) comp[other] = id, stack.push_back(other);
            }
          }
        }
        if (ncomp < 2) continue;  // 앞선 노드 등: 나뉘지 않는다
        // a 쪽 덩어리 = faces 에 적힌 요소가 든 덩어리. 나머지 덩어리마다 새 노드
        std::set<int> a_comps;
        for (const auto& [key, eid] : crack) {
          const std::ptrdiff_t ei = m.find_element(eid);
          if (ei >= 0 && comp.count(static_cast<std::size_t>(ei)) && std::find(key.begin(), key.end(), n) != key.end()) a_comps.insert(comp[static_cast<std::size_t>(ei)]);
        }
        std::map<int, Id> fresh;
        for (const auto& [ei, cid] : comp) {
          if (a_comps.count(cid)) continue;
          if (!fresh.count(cid)) {
            fresh[cid] = next++;
            const Vec3 x = m.node(n);
            new_ids.push_back(fresh[cid]), new_xyz.insert(new_xyz.end(), {x[0], x[1], x[2]});
          }
          remap[{ei, n}] = fresh[cid], b_elems.insert(m.element_ids()[ei]);
        }
      }
      if (new_ids.empty()) throw Error("invalid_state", "가를 노드가 없습니다(균열면이 메시 안쪽 면이 아니거나 이미 갈라져 있음)", {{"param", "faces"}});
      m.add_nodes(new_ids, new_xyz);
      std::vector<Element> changed;
      std::set<std::size_t> touched;
      for (const auto& [key, nid] : remap) touched.insert(key.first);
      for (std::size_t ei : touched) {
        Element e = m.element_at(ei);
        for (Id& nd : e.nodes) {
          auto it = remap.find({ei, nd});
          if (it != remap.end()) nd = it->second;
        }
        changed.push_back(std::move(e));
      }
      m.replace_elements(changed);
      // 양쪽 면 셋: a = faces 그대로, b = 반대쪽 요소의 같은 자리 면(새 노드로 바뀐 뒤)
      Json faces_b = Json::array();
      for (const auto& [key, eid] : crack) {
        for (Id be : b_elems) {
          const Element e = m.element(be);
          const auto& table = shape_faces(e.shape);
          for (std::size_t f = 0; f < table.size(); ++f) {
            // b 요소의 면 노드를 원래 번호로 되돌려 비교한다
            std::vector<Id> k2;
            for (int k : table[f]) {
              Id nd = e.nodes[static_cast<std::size_t>(k)];
              for (const auto& [rk, nn] : remap)
                if (nn == nd) nd = rk.second;
              k2.push_back(nd);
            }
            std::sort(k2.begin(), k2.end());
            if (k2 == key) faces_b.push_back(Json::array({be, static_cast<int>(f) + 1}));
          }
        }
      }
      const std::string name = p.value("name", std::string("crack"));
      const Id set_a = a.invoke("set.create_surface", Json{{"name", name + "_a"}, {"faces", faces}})["id"].get<Id>();
      const Id set_b = a.invoke("set.create_surface", Json{{"name", name + "_b"}, {"faces", faces_b}})["id"].get<Id>();
      return Json{{"new_nodes", new_ids}, {"split_elements", std::vector<Id>(b_elems.begin(), b_elems.end())}, {"set_a", set_a}, {"set_b", set_b},
                  {"faces", faces.size()}, {"faces_b", faces_b.size()}};
    };
    app.register_command(std::move(c));
  }
  // ---------------------------------------------------------------- 노드 병합
  {
    CommandSpec c = base("mesh.merge_nodes", 'C', "mesh", "허용 거리 안의 노드를 병합한다(번호가 작은 노드가 남는다)", "MSH-15");
    c.params = {F("tolerance", "number", "이 거리보다 가까운 노드를 병합한다").call_req().ge(0).unit("length").ex(1e-6),
                F("ids", "integer_list", "대상 노드(없으면 전부)").ex(Json::array({1}))};
    c.fn = [](App& a, const Json& p) {
      Mesh& m = a.mesh();
      const double tol = p["tolerance"].get<double>();
      std::vector<Id> ids = given(a, p, "ids") ? id_list(a, p, "ids") : m.node_ids();
      std::sort(ids.begin(), ids.end());
      std::map<Id, Id> merged;  // 사라지는 노드 → 남는 노드
      if (tol > 0) {
        std::map<std::tuple<long long, long long, long long>, std::vector<Id>> grid;
        auto cell = [&](const Vec3& x) {
          return std::make_tuple(static_cast<long long>(std::floor(x[0] / tol)), static_cast<long long>(std::floor(x[1] / tol)),
                                 static_cast<long long>(std::floor(x[2] / tol)));
        };
        for (Id id : ids) {
          const Vec3 x = m.node(id);
          const auto [ci, cj, ck] = cell(x);
          Id keep = 0;
          for (long long i = ci - 1; i <= ci + 1 && !keep; ++i)
            for (long long j = cj - 1; j <= cj + 1 && !keep; ++j)
              for (long long k = ck - 1; k <= ck + 1 && !keep; ++k) {
                auto it = grid.find(std::make_tuple(i, j, k));
                if (it == grid.end()) continue;
                for (Id other : it->second)
                  if (norm(sub(m.node(other), x)) < tol) {
                    keep = other;
                    break;
                  }
              }
          if (keep)
            merged[id] = keep;
          else
            grid[cell(x)].push_back(id);
        }
      }
      if (merged.empty()) return Json{{"merged", 0}, {"elements_changed", 0}};
      std::vector<Element> changed;
      for (std::size_t i = 0; i < m.element_count(); ++i) {
        Element e = m.element_at(i);
        bool hit = false;
        for (Id& n : e.nodes) {
          auto it = merged.find(n);
          if (it != merged.end()) n = it->second, hit = true;
        }
        if (hit) changed.push_back(std::move(e));
      }
      m.replace_elements(changed);
      std::vector<Id> gone;
      for (const auto& [from, to] : merged) gone.push_back(from);
      m.remove_nodes(gone);
      remap_object_refs(a, merged, {});
      return Json{{"merged", merged.size()}, {"elements_changed", changed.size()}};
    };
    app.register_command(std::move(c));
  }

  // ---------------------------------------------------------------- 변환(이동·회전·대칭·스케일, 복사)
  {
    CommandSpec c = base("mesh.transform", 'C', "mesh", "메시를 이동·회전·대칭·스케일하거나 복사한다", "MSH-16");
    c.params = select_params();
    c.params[0].desc = "대상 요소(없으면 전체 또는 part). 그 요소의 노드가 변환된다";
    c.params.push_back(F("nodes", "integer_list", "대상 노드(요소 대신 노드만 옮길 때)").ex(Json::array({1})));
    c.params.push_back(F("translate", "vector3", "이동량").unit("length").ex({10.0, 0.0, 0.0}));
    c.params.push_back(F("rotate_point", "vector3", "회전축 위의 점").unit("length"));
    c.params.push_back(F("rotate_axis", "vector3", "회전축 방향"));
    c.params.push_back(F("rotate_angle", "number", "회전각(도)"));
    c.params.push_back(F("mirror_point", "vector3", "대칭면 위의 점").unit("length"));
    c.params.push_back(F("mirror_normal", "vector3", "대칭면의 법선"));
    c.params.push_back(F("scale_center", "vector3", "스케일 중심").unit("length"));
    c.params.push_back(F("scale_factor", "number", "스케일 배율").gt(0));
    c.params.push_back(F("copy", "bool", "원본을 두고 변환된 사본을 만든다"));
    c.params.push_back(F("copy_part", "ref", "사본을 넣을 메시 파트(없으면 원본과 같음)").ref("mesh_part"));
    c.fn = [](App& a, const Json& p) {
      Mesh& m = a.mesh();
      const int ops = int(has(p, "translate")) + int(has(p, "rotate_angle")) + int(has(p, "mirror_normal")) + int(has(p, "scale_factor"));
      if (ops != 1) throw Error("missing_param", "변환을 하나만 지정해야 합니다(translate, rotate_*, mirror_*, scale_*)", {{"param", "translate"}});
      std::function<Vec3(const Vec3&)> f;
      const bool mirror = has(p, "mirror_normal");
      if (has(p, "translate")) {
        const Vec3 t = vec3(p["translate"]);
        f = [t](const Vec3& x) { return add(x, t); };
      } else if (has(p, "rotate_angle")) {
        if (!has(p, "rotate_axis")) throw Error("missing_param", "필수 매개변수가 없습니다: rotate_axis", {{"param", "rotate_axis"}});
        const Vec3 o = has(p, "rotate_point") ? vec3(p["rotate_point"]) : Vec3{0, 0, 0};
        const Vec3 k = unit(vec3(p["rotate_axis"]), "rotate_axis");
        const double th = p["rotate_angle"].get<double>() * kPi / 180.0, cs = std::cos(th), sn = std::sin(th);
        f = [o, k, cs, sn](const Vec3& x) {  // 로드리게스 회전
          const Vec3 v = sub(x, o);
          return add(o, add(add(mul(v, cs), mul(cross(k, v), sn)), mul(k, dot(k, v) * (1 - cs))));
        };
      } else if (mirror) {
        const Vec3 o = has(p, "mirror_point") ? vec3(p["mirror_point"]) : Vec3{0, 0, 0};
        const Vec3 n = unit(vec3(p["mirror_normal"]), "mirror_normal");
        f = [o, n](const Vec3& x) { return sub(x, mul(n, 2 * dot(sub(x, o), n))); };
      } else {
        check_value(a.commands().at("mesh.transform").params[10], p["scale_factor"], nullptr);
        const Vec3 cen = has(p, "scale_center") ? vec3(p["scale_center"]) : Vec3{0, 0, 0};
        const double s = p["scale_factor"].get<double>();
        f = [cen, s](const Vec3& x) { return add(cen, mul(sub(x, cen), s)); };
      }

      const bool node_mode = given(a, p, "nodes");
      std::vector<Id> elem_ids;
      std::set<Id> nodes;
      if (node_mode) {
        for (Id n : id_list(a, p, "nodes")) {
          m.node(n);
          nodes.insert(n);
        }
      } else {
        elem_ids = select_elements(a, p);
        for (Id e : elem_ids) append_nodes(nodes, m.element(e));
        if (!given(a, p, "ids") && !has(p, "part"))
          for (Id n : m.node_ids()) nodes.insert(n);  // 전체: 요소에 속하지 않은 노드도 함께
      }
      const std::vector<Id> node_list(nodes.begin(), nodes.end());

      if (p.value("copy", false)) {
        NodeMaker maker(m);
        std::map<Id, Id> remap;
        for (Id n : node_list) remap[n] = maker.make(f(m.node(n)));
        maker.commit(m);
        Id part = 0;
        if (has(p, "copy_part")) {
          check_value(F("copy_part", "ref", "").ref("mesh_part"), p["copy_part"], &a.model());
          part = p["copy_part"].get<Id>();
        }
        std::vector<Element> created;
        Id next = m.max_element_id() + 1;
        for (Id eid : elem_ids) {
          Element e = m.element(eid);
          e.id = next++;
          for (Id& n : e.nodes)
            if (n != 0) n = remap.at(n);
          if (part) e.part = part;
          if (mirror) flip(e);
          created.push_back(std::move(e));
        }
        m.add_elements(created);
        Json r{{"nodes", node_list.size()}, {"elements", created.size()}};
        if (!maker.ids.empty()) r["first_node"] = maker.ids.front(), r["last_node"] = maker.ids.back();
        if (!created.empty()) r["first_element"] = created.front().id, r["last_element"] = created.back().id;
        return r;
      }

      std::vector<double> xyz;
      xyz.reserve(3 * node_list.size());
      for (Id n : node_list) {
        const Vec3 q = f(m.node(n));
        xyz.insert(xyz.end(), q.begin(), q.end());
      }
      m.move_nodes(node_list, xyz);
      std::size_t flipped = 0;
      if (mirror) {  // 대칭 이동하면 방향이 뒤집히므로 절점 순서를 바꿔 준다(모든 절점이 옮겨진 요소)
        std::vector<Element> changed;
        for (std::size_t i = 0; i < m.element_count(); ++i) {
          Element e = m.element_at(i);
          if (std::all_of(e.nodes.begin(), e.nodes.end(), [&](Id n) { return n == 0 || nodes.count(n); })) {
            flip(e);
            changed.push_back(std::move(e));
          }
        }
        m.replace_elements(changed);
        flipped = changed.size();
      }
      return Json{{"nodes", node_list.size()}, {"elements", elem_ids.size()}, {"flipped", flipped}};
    };
    app.register_command(std::move(c));
  }

  // ---------------------------------------------------------------- 돌출·회전(상위 차원 요소 만들기)
  // 선 → 사각형, 삼각형 → 웨지, 사각형 → 육면체. 1차 요소만.
  auto sweep = [](App& a, const Json& p, int layers, const std::function<Vec3(const Vec3&, int)>& at, bool closed) {
    Mesh& m = a.mesh();
    const std::vector<Id> src = select_elements(a, p);
    std::set<Id> nodes;
    for (Id id : src) {
      const Element e = m.element(id);
      if (e.shape != Shape::Line2 && e.shape != Shape::Tri3 && e.shape != Shape::Quad4)
        throw Error("unsupported", std::string(shape_info(e.shape).name) + " 요소는 돌출·회전할 수 없습니다(1차 선·삼각형·사각형만)",
                    {{"element", id}});
      append_nodes(nodes, e);
    }
    NodeMaker maker(m);
    std::map<Id, Vec3> fresh;
    // layer[l][노드] = l 번째 층의 노드. 0 번째 층은 원래 노드, 닫힌 회전이면 마지막 층도 원래 노드.
    std::vector<std::map<Id, Id>> layer(static_cast<std::size_t>(layers) + 1);
    for (Id n : nodes) layer[0][n] = n;
    for (int l = 1; l <= layers; ++l)
      for (Id n : nodes) {
        if (closed && l == layers) {
          layer[static_cast<std::size_t>(l)][n] = n;
          continue;
        }
        const Vec3 x = at(m.node(n), l);
        const Id id = maker.make(x);
        fresh[id] = x;
        layer[static_cast<std::size_t>(l)][n] = id;
      }
    Id part = 0;
    if (has(p, "new_part")) {
      check_value(F("new_part", "ref", "").ref("mesh_part"), p["new_part"], &a.model());
      part = p["new_part"].get<Id>();
    }
    std::vector<Element> created;
    Id next = m.max_element_id() + 1;
    for (Id id : src) {
      const Element e = m.element(id);
      for (int l = 0; l < layers; ++l) {
        const auto& lo = layer[static_cast<std::size_t>(l)];
        const auto& hi = layer[static_cast<std::size_t>(l) + 1];
        Element n;
        n.id = next++, n.part = part ? part : e.part;
        if (e.shape == Shape::Line2) {
          n.shape = Shape::Quad4;
          n.nodes = {lo.at(e.nodes[0]), lo.at(e.nodes[1]), hi.at(e.nodes[1]), hi.at(e.nodes[0])};
        } else {
          n.shape = e.shape == Shape::Tri3 ? Shape::Wedge6 : Shape::Hex8;
          for (Id k : e.nodes) n.nodes.push_back(lo.at(k));
          for (Id k : e.nodes) n.nodes.push_back(hi.at(k));
          make_positive(m, n, fresh);
        }
        created.push_back(std::move(n));
      }
    }
    maker.commit(m);
    m.add_elements(created);
    if (p.value("delete_source", false)) m.remove_elements(src);
    Json r{{"nodes", maker.ids.size()}, {"elements", created.size()}};
    if (!created.empty()) r["first_element"] = created.front().id, r["last_element"] = created.back().id;
    return r;
  };
  auto sweep_params = [] {
    Fields ps = select_params();
    ps.push_back(F("layers", "integer", "층 수").ge(1));
    ps.push_back(F("new_part", "ref", "새 요소를 넣을 메시 파트(없으면 원본과 같음)").ref("mesh_part"));
    ps.push_back(F("delete_source", "bool", "원본 요소를 지운다"));
    return ps;
  };
  {
    CommandSpec c = base("mesh.extrude", 'C', "mesh", "요소를 한 방향으로 돌출시켜 상위 차원 요소를 만든다", "MSH-16");
    c.params = sweep_params();
    c.params.push_back(F("direction", "vector3", "돌출 방향과 거리").call_req().unit("length").ex({0.0, 0.0, 5.0}));
    c.fn = [sweep](App& a, const Json& p) {
      const int layers = p.value("layers", 1);
      check_value(F("layers", "integer", "").ge(1), Json(layers), nullptr);
      const Vec3 d = vec3(p["direction"]);
      unit(d, "direction");
      return sweep(a, p, layers, [d, layers](const Vec3& x, int l) { return add(x, mul(d, double(l) / layers)); }, false);
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("mesh.revolve", 'C', "mesh", "요소를 축 둘레로 회전시켜 상위 차원 요소를 만든다", "MSH-16");
    c.params = sweep_params();
    c.params.push_back(F("axis_point", "vector3", "회전축 위의 점").call_req().unit("length").ex({0.0, 0.0, 0.0}));
    c.params.push_back(F("axis_direction", "vector3", "회전축 방향").call_req().ex({0.0, 0.0, 1.0}));
    c.params.push_back(F("angle", "number", "회전각(도). 360 이면 닫힌 형상").call_req().gt(0).le(360).ex(90.0));
    c.fn = [sweep](App& a, const Json& p) {
      const int layers = p.value("layers", 1);
      check_value(F("layers", "integer", "").ge(1), Json(layers), nullptr);
      check_value(F("angle", "number", "").gt(0).le(360), p["angle"], nullptr);
      const Vec3 o = vec3(p["axis_point"]), k = unit(vec3(p["axis_direction"]), "axis_direction");
      const double angle = p["angle"].get<double>();
      const bool closed = std::fabs(angle - 360.0) < 1e-9;
      if (closed && layers < 3) throw Error("out_of_range", "360° 회전에는 층이 3개 이상 필요합니다", {{"param", "layers"}});
      // 축 위의 노드는 회전해도 제자리라 요소가 찌그러진다.
      for (Id id : select_elements(a, p))
        for (Id n : a.mesh().element(id).nodes) {
          const Vec3 v = sub(a.mesh().node(n), o);
          if (norm(sub(v, mul(k, dot(k, v)))) < 1e-12 * (1.0 + norm(v)))
            throw Error("unsupported", "회전축 위에 있는 노드가 있습니다: " + std::to_string(n), {{"node", n}});
        }
      return sweep(a, p, layers, [o, k, angle, layers](const Vec3& x, int l) {
        const double th = angle * double(l) / layers * kPi / 180.0, cs = std::cos(th), sn = std::sin(th);
        const Vec3 v = sub(x, o);
        return add(o, add(add(mul(v, cs), mul(cross(k, v), sn)), mul(k, dot(k, v) * (1 - cs))));
      }, closed);
    };
    app.register_command(std::move(c));
  }

  // ---------------------------------------------------------------- 차수 변환
  {
    CommandSpec c = base("mesh.convert_order", 'C', "mesh", "1차↔2차 요소를 변환한다(중간 절점은 변의 중점에 둔다)", "MSH-12");
    c.params = select_params();
    c.params.push_back(F("order", "integer", "바꿀 차수").call_req().ge(1).le(2).ex(2));
    c.fn = [](App& a, const Json& p) {
      Mesh& m = a.mesh();
      check_value(F("order", "integer", "").ge(1).le(2), p["order"], nullptr);
      const int order = p["order"].get<int>();
      static const std::map<Shape, Shape> up = {{Shape::Line2, Shape::Line3}, {Shape::Tri3, Shape::Tri6},
                                                {Shape::Quad4, Shape::Quad8}, {Shape::Tet4, Shape::Tet10},
                                                {Shape::Hex8, Shape::Hex20}, {Shape::Wedge6, Shape::Wedge15}};
      static const std::map<std::string, std::string> type_up = {
          {"C3D4", "C3D10"}, {"C3D8", "C3D20"}, {"C3D8R", "C3D20R"}, {"C3D8I", "C3D20"}, {"C3D6", "C3D15"},
          {"S3", "S6"}, {"S4", "S8"}, {"S4R", "S8R"}, {"M3D3", "M3D6"}, {"M3D4", "M3D8"}, {"M3D4R", "M3D8R"},
          {"CPS3", "CPS6"}, {"CPS4", "CPS8"}, {"CPS4R", "CPS8R"}, {"CPE3", "CPE6"}, {"CPE4", "CPE8"}, {"CPE4R", "CPE8R"},
          {"CAX3", "CAX6"}, {"CAX4", "CAX8"}, {"CAX4R", "CAX8R"}, {"B31", "B32"}, {"B31R", "B32R"}, {"T3D2", "T3D3"}};
      std::vector<Element> changed;
      std::set<Id> dropped;
      NodeMaker maker(m);
      std::map<std::pair<Id, Id>, Id> mid;  // 변 → 중간 절점
      for (Id id : select_elements(a, p)) {
        Element e = m.element(id);
        const ShapeInfo& info = shape_info(e.shape);
        if (info.order == order || info.dim == 0) continue;
        if (order == 2) {
          auto it = up.find(e.shape);
          if (it == up.end()) continue;  // 2차형이 없는 형상(피라미드)
          std::vector<Id> mids;
          for (const auto& ed : shape_edges(e.shape)) {
            const Id u = e.nodes[static_cast<std::size_t>(ed[0])], v = e.nodes[static_cast<std::size_t>(ed[1])];
            auto key = std::make_pair(std::min(u, v), std::max(u, v));
            auto f = mid.find(key);
            if (f == mid.end()) f = mid.emplace(key, maker.make(mul(add(m.node(u), m.node(v)), 0.5))).first;
            mids.push_back(f->second);
          }
          if (e.shape == Shape::Line2)
            e.nodes = {e.nodes[0], mids[0], e.nodes[1]};
          else
            e.nodes.insert(e.nodes.end(), mids.begin(), mids.end());
          e.shape = it->second;
          auto t = type_up.find(e.type);
          e.type = t == type_up.end() ? "" : t->second;
        } else {
          Shape lower = e.shape;
          for (const auto& [lo, hi] : up)
            if (hi == e.shape) lower = lo;
          std::vector<Id> corners;
          const auto positions = corner_positions(e.shape);
          const std::set<int> keep(positions.begin(), positions.end());
          for (std::size_t i = 0; i < e.nodes.size(); ++i) {
            if (keep.count(static_cast<int>(i)))
              corners.push_back(e.nodes[i]);
            else
              dropped.insert(e.nodes[i]);
          }
          e.nodes = corners, e.shape = lower;
          std::string down;
          for (const auto& [lo, hi] : type_up)
            if (hi == e.type && down.empty()) down = lo;
          e.type = down;
        }
        changed.push_back(std::move(e));
      }
      maker.commit(m);
      m.replace_elements(changed);
      std::vector<Id> gone;
      if (!dropped.empty()) {  // 쓰이지 않게 된 중간 절점을 지운다
        const std::vector<Id> used = m.used_nodes();
        for (Id n : dropped)
          if (!std::binary_search(used.begin(), used.end(), n)) gone.push_back(n);
        m.remove_nodes(gone);
      }
      return Json{{"elements", changed.size()}, {"nodes_added", maker.ids.size()}, {"nodes_removed", gone.size()}};
    };
    app.register_command(std::move(c));
  }

  // ---------------------------------------------------------------- 분할·결합·세분화
  {
    CommandSpec c = base("mesh.elements_split", 'C', "mesh", "요소를 나눈다(사각형 → 삼각형 2, 육면체 → 사면체 6, 웨지 → 사면체 3)", "MSH-14");
    c.params = select_params();
    c.fn = [](App& a, const Json& p) {
      Mesh& m = a.mesh();
      static const std::map<Shape, std::pair<Shape, std::vector<std::vector<int>>>> table = {
          {Shape::Quad4, {Shape::Tri3, {{0, 1, 2}, {0, 2, 3}}}},
          {Shape::Hex8, {Shape::Tet4, {{0, 1, 2, 6}, {0, 2, 3, 6}, {0, 3, 7, 6}, {0, 7, 4, 6}, {0, 4, 5, 6}, {0, 5, 1, 6}}}},
          {Shape::Wedge6, {Shape::Tet4, {{0, 1, 2, 3}, {1, 2, 3, 4}, {2, 3, 4, 5}}}},
      };
      std::vector<Id> removed;
      std::vector<Element> created;
      Id next = m.max_element_id() + 1;
      for (Id id : select_elements(a, p)) {
        const Element e = m.element(id);
        auto it = table.find(e.shape);
        if (it == table.end())
          throw Error("unsupported", std::string(shape_info(e.shape).name) + " 요소는 나눌 수 없습니다", {{"element", id}});
        for (const auto& sub_nodes : it->second.second) {
          Element n;
          n.id = next++, n.shape = it->second.first, n.part = e.part;
          for (int k : sub_nodes) n.nodes.push_back(e.nodes[static_cast<std::size_t>(k)]);
          created.push_back(std::move(n));
        }
        removed.push_back(id);
      }
      m.remove_elements(removed);
      m.add_elements(created);
      return Json{{"removed", removed.size()}, {"created", created.size()}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("mesh.elements_combine", 'C', "mesh", "변을 공유하는 삼각형 둘을 사각형 하나로 합친다", "MSH-14");
    c.params = {F("ids", "integer_list", "삼각형 요소 2개").call_req().ex(Json::array({1, 2}))};
    c.fn = [](App& a, const Json& p) {
      Mesh& m = a.mesh();
      const std::vector<Id> ids = id_list(a, p, "ids");
      if (ids.size() != 2) throw Error("invalid_param_type", "요소 2개를 지정해야 합니다", {{"param", "ids"}});
      const Element t1 = m.element(ids[0]), t2 = m.element(ids[1]);
      if (t1.shape != Shape::Tri3 || t2.shape != Shape::Tri3)
        throw Error("unsupported", "1차 삼각형 요소만 합칠 수 있습니다", {{"param", "ids"}});
      std::vector<Id> apex;
      for (Id n : t2.nodes)
        if (std::find(t1.nodes.begin(), t1.nodes.end(), n) == t1.nodes.end()) apex.push_back(n);
      if (apex.size() != 1) throw Error("invalid_state", "두 삼각형이 변 하나를 공유해야 합니다", {{"param", "ids"}});
      // t1 에서 공유 변의 맞은편 절점을 첫 절점으로 돌린 뒤, 공유 변 사이에 t2 의 꼭짓점을 넣는다.
      std::size_t lone = 0;
      for (std::size_t i = 0; i < 3; ++i)
        if (std::find(t2.nodes.begin(), t2.nodes.end(), t1.nodes[i]) == t2.nodes.end()) lone = i;
      Element q;
      q.id = m.max_element_id() + 1, q.shape = Shape::Quad4, q.part = t1.part;
      q.nodes = {t1.nodes[lone], t1.nodes[(lone + 1) % 3], apex[0], t1.nodes[(lone + 2) % 3]};
      m.remove_elements(ids);
      m.add_elements({q});
      return Json{{"id", q.id}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("mesh.refine", 'C', "mesh", "요소를 고르게 세분화한다(선 2, 삼각형·사각형 4, 사면체·육면체 8)", "MSH-14");
    c.params = select_params();
    c.fn = [](App& a, const Json& p) {
      Mesh& m = a.mesh();
      const std::vector<Id> src = select_elements(a, p);
      NodeMaker maker(m);
      std::map<Id, Vec3> fresh;
      std::map<std::vector<Id>, Id> shared;  // 정렬된 꼭짓점 묶음 → 그 중심의 노드(변 중점, 면 중심, 요소 중심)
      auto center = [&](std::vector<Id> corners) {
        std::sort(corners.begin(), corners.end());
        corners.erase(std::unique(corners.begin(), corners.end()), corners.end());
        if (corners.size() == 1) return corners[0];
        auto it = shared.find(corners);
        if (it != shared.end()) return it->second;
        Vec3 x{0, 0, 0};
        for (Id n : corners) {
          auto f = fresh.find(n);
          x = add(x, f != fresh.end() ? f->second : m.node(n));
        }
        x = mul(x, 1.0 / static_cast<double>(corners.size()));
        const Id id = maker.make(x);
        fresh[id] = x;
        shared[corners] = id;
        return id;
      };
      std::vector<Element> created;
      Id next = m.max_element_id() + 1;
      auto emit = [&](const Element& parent, Shape shape, std::vector<Id> nodes) {
        Element n;
        n.id = next++, n.shape = shape, n.part = parent.part, n.type = parent.type, n.nodes = std::move(nodes);
        make_positive(m, n, fresh);
        created.push_back(std::move(n));
      };
      for (Id id : src) {
        const Element e = m.element(id);
        const std::vector<Id>& n = e.nodes;
        if (e.shape == Shape::Line2) {
          const Id c = center({n[0], n[1]});
          emit(e, Shape::Line2, {n[0], c});
          emit(e, Shape::Line2, {c, n[1]});
        } else if (e.shape == Shape::Tri3) {
          const Id a01 = center({n[0], n[1]}), a12 = center({n[1], n[2]}), a20 = center({n[2], n[0]});
          emit(e, Shape::Tri3, {n[0], a01, a20});
          emit(e, Shape::Tri3, {a01, n[1], a12});
          emit(e, Shape::Tri3, {a20, a12, n[2]});
          emit(e, Shape::Tri3, {a01, a12, a20});
        } else if (e.shape == Shape::Quad4 || e.shape == Shape::Hex8) {
          // 격자점 (i, j, k) ∈ {0,1,2}³ 는 그 좌표가 1 인 축에서 양쪽 꼭짓점의 평균이다.
          const bool hex = e.shape == Shape::Hex8;
          auto corner = [&](int i, int j, int k) {
            static const int quad[2][2] = {{0, 3}, {1, 2}};  // [i][j]
            const int base_idx = quad[i][j];
            return n[static_cast<std::size_t>(hex ? base_idx + 4 * k : base_idx)];
          };
          auto lattice = [&](int i, int j, int k) {
            std::vector<Id> cs;
            for (int ii : (i == 1 ? std::vector<int>{0, 1} : std::vector<int>{i / 2}))
              for (int jj : (j == 1 ? std::vector<int>{0, 1} : std::vector<int>{j / 2}))
                for (int kk : (hex ? (k == 1 ? std::vector<int>{0, 1} : std::vector<int>{k / 2}) : std::vector<int>{0}))
                  cs.push_back(corner(ii, jj, kk));
            return center(cs);
          };
          for (int k = 0; k < (hex ? 2 : 1); ++k)
            for (int j = 0; j < 2; ++j)
              for (int i = 0; i < 2; ++i) {
                std::vector<Id> nodes = {lattice(i, j, k), lattice(i + 1, j, k), lattice(i + 1, j + 1, k), lattice(i, j + 1, k)};
                if (hex)
                  for (const auto& [di, dj] : std::vector<std::pair<int, int>>{{0, 0}, {1, 0}, {1, 1}, {0, 1}})
                    nodes.push_back(lattice(i + di, j + dj, k + 1));
                emit(e, e.shape, nodes);
              }
        } else if (e.shape == Shape::Tet4) {
          const Id a01 = center({n[0], n[1]}), a12 = center({n[1], n[2]}), a20 = center({n[2], n[0]});
          const Id a03 = center({n[0], n[3]}), a13 = center({n[1], n[3]}), a23 = center({n[2], n[3]});
          emit(e, Shape::Tet4, {n[0], a01, a20, a03});
          emit(e, Shape::Tet4, {a01, n[1], a12, a13});
          emit(e, Shape::Tet4, {a20, a12, n[2], a23});
          emit(e, Shape::Tet4, {a03, a13, a23, n[3]});
          emit(e, Shape::Tet4, {a01, a12, a20, a13});  // 가운데 팔면체를 대각선 a20–a13 으로 나눈다
          emit(e, Shape::Tet4, {a01, a20, a03, a13});
          emit(e, Shape::Tet4, {a20, a12, a23, a13});
          emit(e, Shape::Tet4, {a20, a23, a03, a13});
        } else {
          throw Error("unsupported", std::string(shape_info(e.shape).name) + " 요소는 세분화할 수 없습니다", {{"element", id}});
        }
      }
      maker.commit(m);
      m.remove_elements(src);
      m.add_elements(created);
      return Json{{"removed", src.size()}, {"created", created.size()}, {"nodes_added", maker.ids.size()},
                  {"partial", m.element_count() > created.size()}};  // 일부만 세분화하면 경계에 매달린 절점이 생긴다
    };
    app.register_command(std::move(c));
  }

  // ---------------------------------------------------------------- 연결 요소·네트워크
  {
    CommandSpec c = base("mesh.create_connector", 'C', "mesh", "스프링·대시팟·갭·질량·커플링 요소를 만든다", "MSH-21, MSH-32, BC-29");
    c.params = {F("kind", "string", "요소 종류").call_req().one_of({"spring", "spring_to_ground", "spring_fixed_direction", "dashpot", "gap", "mass", "coupling"}).ex("spring"),
                F("nodes", "integer_list", "노드(질량·지면 스프링·커플링은 1개, 그 밖은 2개)").call_req().ex(Json::array({1, 2})),
                F("part", "ref", "메시 파트").ref("mesh_part")};
    c.fn = [](App& a, const Json& p) {
      check_value(a.commands().at("mesh.create_connector").params[0], p["kind"], nullptr);
      static const std::map<std::string, std::pair<const char*, std::size_t>> table = {
          {"spring", {"SPRINGA", 2}}, {"spring_fixed_direction", {"SPRING2", 2}}, {"spring_to_ground", {"SPRING1", 1}},
          {"dashpot", {"DASHPOTA", 2}}, {"gap", {"GAPUNI", 2}}, {"mass", {"MASS", 1}}, {"coupling", {"DCOUP3D", 1}}};
      const auto& [type, count] = table.at(p["kind"].get<std::string>());
      const std::vector<Id> nodes = id_list(a, p, "nodes");
      if (nodes.size() != count)
        throw Error("invalid_param_type", "이 요소는 노드가 " + std::to_string(count) + "개여야 합니다", {{"param", "nodes"}});
      Json args{{"shape", count == 1 ? "point1" : "line2"}, {"type", type}, {"connectivity", Json::array({nodes})}};
      if (has(p, "part")) args["part"] = p["part"];
      return a.invoke("mesh.elements_create", args);
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("mesh.create_network", 'C', "mesh", "네트워크 요소(끝·가운데·끝 절점)를 만든다. 입구·출구는 끝 절점을 0 으로 준다", "MSH-33");
    c.params = {F("connectivity", "table", "[끝, 가운데, 끝] 절점의 목록").columns(3).call_req().ex(Json::array({Json::array({0, 1, 2})})),
                F("part", "ref", "메시 파트").ref("mesh_part")};
    c.fn = [](App& a, const Json& p) {
      for (const Json& row : p["connectivity"])
        if (row.size() != 3) throw Error("invalid_param_type", "행마다 절점이 3개여야 합니다", {{"param", "connectivity"}});
      Json args{{"shape", "line3"}, {"type", "D"}, {"connectivity", p["connectivity"]}};
      if (has(p, "part")) args["part"] = p["part"];
      return a.invoke("mesh.elements_create", args);
    };
    app.register_command(std::move(c));
  }

  // ---------------------------------------------------------------- 절점 법선·보 방향 (모델 설정 객체에 둔다)
  {
    CommandSpec c = base("mesh.set_normal", 'C', "mesh", "쉘·보의 (요소, 절점) 쌍에 법선을 직접 지정한다", "MSH-36");
    c.params = {F("entries", "table", "[요소, 절점, nx, ny, nz] 의 목록(빈 목록이면 모두 지운다)").columns(5).call_req().ex(Json::array({Json::array({1, 1, 0.0, 0.0, 1.0})}))};
    c.fn = [](App& a, const Json& p) {
      Object s = a.settings();
      std::map<std::pair<Id, Id>, Json> all;
      if (!p["entries"].empty())
        for (const Json& row : s.props.value("normals", Json::array())) all[{row[0].get<Id>(), row[1].get<Id>()}] = row;
      for (const Json& row : p["entries"]) {
        if (row.size() != 5) throw Error("invalid_param_type", "행마다 값이 5개여야 합니다", {{"param", "entries"}});
        const Element e = a.mesh().element(static_cast<Id>(row[0].get<double>()));
        const Id node = static_cast<Id>(row[1].get<double>());
        if (std::find(e.nodes.begin(), e.nodes.end(), node) == e.nodes.end())
          throw Error("not_found", "요소 " + std::to_string(e.id) + " 의 절점이 아닙니다: " + std::to_string(node), {{"param", "entries"}});
        if (shape_info(e.shape).dim == 3 || shape_info(e.shape).dim == 0)
          throw Error("unsupported", "법선은 쉘·보 요소에만 지정할 수 있습니다", {{"element", e.id}});
        unit({row[2].get<double>(), row[3].get<double>(), row[4].get<double>()}, "entries");
        all[{e.id, node}] = Json::array({e.id, node, row[2], row[3], row[4]});
      }
      Json out = Json::array();
      for (const auto& [key, row] : all) out.push_back(row);
      s.props["normals"] = out;
      a.model().replace(s);
      return Json{{"count", out.size()}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("mesh.set_beam_direction", 'C', "mesh", "보 요소의 단면 1축 방향을 지정한다", "MSH-20, PRP-07");
    c.params = {F("ids", "integer_list", "보 요소").call_req().ex(Json::array({1})),
                F("direction", "vector3", "단면 1축 방향").call_req().ex({0.0, 0.0, 1.0})};
    c.fn = [](App& a, const Json& p) {
      const Vec3 d = unit(vec3(p["direction"]), "direction");
      Object s = a.settings();
      std::map<Id, Json> all;
      for (const Json& row : s.props.value("beam_directions", Json::array())) all[row[0].get<Id>()] = row;
      for (Id id : id_list(a, p, "ids")) {
        const Element e = a.mesh().element(id);
        if (shape_info(e.shape).dim != 1) throw Error("unsupported", "보 요소가 아닙니다: " + std::to_string(id), {{"element", id}});
        const std::vector<Vec3> pts = corner_points(a.mesh(), e);
        const Vec3 axis = unit(sub(pts[1], pts[0]), "ids");
        if (norm(cross(axis, d)) < 1e-9)
          throw Error("invalid_geometry", "방향이 보의 축과 평행합니다(요소 " + std::to_string(id) + ")", {{"element", id}, {"param", "direction"}});
        all[id] = Json::array({id, d[0], d[1], d[2]});
      }
      Json out = Json::array();
      for (const auto& [key, row] : all) out.push_back(row);
      s.props["beam_directions"] = out;
      a.model().replace(s);
      return Json{{"count", out.size()}};
    };
    app.register_command(std::move(c));
  }

  // ---------------------------------------------------------------- 하중·경계조건의 대상 전개와 합력
  for (const std::string kind : {"load", "bc"}) {
    CommandSpec c = base(kind + ".resolve", 'Q', kind,
                         kind == "load" ? "하중이 걸리는 노드·요소면·요소를 조회한다" : "경계조건이 걸리는 노드를 조회한다",
                         kind == "load" ? "LOD-14" : "BC-01");
    c.params = {F("id", "ref", "대상 객체").call_req()};
    c.fn = [kind](App& a, const Json& p) {
      const Object& o = a.model().get(p["id"].get<Id>());
      if (o.kind != kind)
        throw Error("wrong_kind", "id=" + std::to_string(o.id) + " 는 " + kind + " 가 아닙니다", {{"object", o.id}, {"expected", kind}});
      if (!has(o.props, "target")) throw Error("invalid_state", "적용 대상이 지정되지 않았습니다", {{"object", o.id}});
      const Json& t = o.props["target"];
      // 그 하중·경계조건이 받는 대상 종류대로 푼다: 노드에 거는 것은 노드, 면에 거는 것은 요소면, 그 밖은 요소.
      std::string what = "nodes";
      for (const FieldSpec& f : a.schema().get(kind).fields_for(subtype_of(o))) {
        if (f.name != "target") continue;
        auto allows = [&](const char* type) { return std::find(f.choices.begin(), f.choices.end(), type) != f.choices.end(); };
        what = allows("nodes") ? "nodes" : allows("faces") ? "faces" : "elements";
      }
      if (what == "faces" && kind == "load") {  // 면과 요소를 모두 받는 하중(쉘의 압력): 요소로 준 대상은 요소로 푼다
        std::string tt = t["type"].get<std::string>();
        if (tt == "set" && !t["ids"].empty()) tt = subtype_of(a.model().get(t["ids"][0].get<Id>())) == "element" ? "elements" : tt;
        if (tt == "elements" || tt == "parts") what = "elements";
        if (tt == "geometry") {  // 면 메시(쉘)의 형상 면은 요소로 풀린다
          try {
            resolve_target(a, t, "faces");
          } catch (const Error&) {
            what = "elements";
          }
        }
      }
      const Json items = resolve_target(a, t, what);
      return Json{{"what", what}, {"count", items.size()}, {what, items}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("load.resultant", 'Q', "load", "스텝에서 유효한 하중의 합력·합모멘트(원점 기준)를 계산한다", "LOD-17");
    c.params = {F("id", "ref", "스텝").ref("step").call_req()};
    c.fn = [](App& a, const Json& p) {
      const Mesh& m = a.mesh();
      const Object& step = a.model().get(p["id"].get<Id>());
      if (step.kind != "step") throw Error("wrong_kind", "스텝이 아닙니다", {{"object", step.id}, {"expected", "step"}});
      Vec3 force{0, 0, 0}, moment{0, 0, 0};
      Json unsupported = Json::array(), terms = Json::array();
      auto apply = [&](const Vec3& f, const Vec3& at) {
        force = add(force, f);
        moment = add(moment, cross(at, f));
      };
      // 요소별 밀도(중력 하중용)
      std::map<Id, double> rho;
      const std::map<Id, Id> prop_of = property_of_elements(a);
      auto density = [&](Id element) {
        auto it = rho.find(element);
        if (it != rho.end()) return it->second;
        double d = -1.0;
        auto pit = prop_of.find(element);
        if (pit != prop_of.end()) {
          const Object& prop = a.model().get(pit->second);
          if (has(prop.props, "material")) {
            const Object& mat = a.model().get(prop.props["material"].get<Id>());
            auto b = mat.props.find("behaviors");
            if (b != mat.props.end() && b->contains("density")) d = (*b)["density"]["data"][0][0].get<double>();
          }
        }
        rho[element] = d;
        return d;
      };
      for (Id lid : effective_loads(a, step.id)) {
        const Object& l = a.model().get(lid);
        const std::string t = subtype_of(l);
        const Vec3 f0 = force, m0 = moment;
        bool ok = has(l.props, "target") && !has(l.props, "csys") && !has(l.props, "distribution");
        try {
          if (!ok) {
          } else if (t == "force" && has(l.props, "components")) {
            const Vec3 f = vec3(l.props["components"]);
            for (const Json& n : resolve_target(a, l.props["target"], "nodes")) apply(f, m.node(n.get<Id>()));
          } else if (t == "moment" && has(l.props, "components")) {
            const Vec3 mm = vec3(l.props["components"]);
            for (const Json& n : resolve_target(a, l.props["target"], "nodes")) (void)n, moment = add(moment, mm);
          } else if (t == "pressure" && has(l.props, "value")) {
            // 면 번호의 절점 순서로 구한 법선은 요소 안쪽을 향한다. 양의 압력은 그 방향으로 민다.
            const double pr = l.props["value"].get<double>();
            for (const Json& fc : resolve_target(a, l.props["target"], "faces")) {
              const Element e = m.element(fc[0].get<Id>());
              if (shape_info(e.shape).dim != 3) {
                ok = false;
                break;
              }
              std::vector<Vec3> pts;
              Vec3 cen{0, 0, 0};
              for (int k : shape_faces(e.shape)[static_cast<std::size_t>(fc[1].get<int>() - 1)]) {
                pts.push_back(m.node(e.nodes[static_cast<std::size_t>(k)]));
                cen = add(cen, pts.back());
              }
              apply(mul(face_normal_area(pts), pr), mul(cen, 1.0 / static_cast<double>(pts.size())));
            }
          } else if (t == "total_force" && has(l.props, "components")) {
            // 작용점은 면의 도심으로 본다.
            Vec3 cen{0, 0, 0};
            double area = 0;
            for (const Json& fc : resolve_target(a, l.props["target"], "faces")) {
              const Element e = m.element(fc[0].get<Id>());
              std::vector<Vec3> pts;
              Vec3 c{0, 0, 0};
              for (int k : shape_faces(e.shape)[static_cast<std::size_t>(fc[1].get<int>() - 1)]) {
                pts.push_back(m.node(e.nodes[static_cast<std::size_t>(k)]));
                c = add(c, pts.back());
              }
              const double ar = norm(face_normal_area(pts));
              cen = add(cen, mul(c, ar / static_cast<double>(pts.size())));
              area += ar;
            }
            if (area > 0) apply(vec3(l.props["components"]), mul(cen, 1.0 / area)); else ok = false;
          } else if ((t == "gravity" || t == "acceleration") && has(l.props, "value") && has(l.props, "direction")) {
            const Vec3 g = mul(unit(vec3(l.props["direction"]), "direction"), l.props["value"].get<double>());
            for (const Json& eid : resolve_target(a, l.props["target"], "elements")) {
              const Element e = m.element(eid.get<Id>());
              const double d = density(e.id);
              if (d < 0 || shape_info(e.shape).dim != 3) {
                ok = false;
                break;
              }
              const std::vector<Vec3> pts = corner_points(m, e);
              Vec3 cen{0, 0, 0};
              for (const Vec3& x : pts) cen = add(cen, x);
              apply(mul(g, d * element_size(shape_info(e.shape), pts)), mul(cen, 1.0 / static_cast<double>(pts.size())));
            }
          } else {
            ok = false;
          }
        } catch (const Error&) {
          ok = false;
        }
        if (!ok) {
          force = f0, moment = m0;
          unsupported.push_back(lid);
        } else {
          terms.push_back(Json{{"id", lid}, {"force", sub(force, f0)}, {"moment", sub(moment, m0)}});
        }
      }
      return Json{{"force", force}, {"moment", moment}, {"terms", terms}, {"unsupported", unsupported}};
    };
    app.register_command(std::move(c));
  }

  // ---------------------------------------------------------------- 번호
  {
    CommandSpec c = base("id.renumber", 'C', "mesh", "노드·요소 번호를 다시 매긴다(번호 순서는 유지하고, 가리키는 곳도 함께 바꾼다)", "CMN-06, MSH-23");
    c.params = {F("what", "string", "대상").call_req().one_of({"nodes", "elements"}).ex("nodes"),
                F("start", "integer", "시작 번호").call_req().ge(1).ex(1000),
                F("ids", "integer_list", "다시 매길 번호(없으면 전부)").ex(Json::array({1}))};
    c.fn = [](App& a, const Json& p) {
      Mesh& m = a.mesh();
      check_value(a.commands().at("id.renumber").params[0], p["what"], nullptr);
      check_value(a.commands().at("id.renumber").params[1], p["start"], nullptr);
      const bool nodes = p["what"] == "nodes";
      std::vector<Id> ids = given(a, p, "ids") ? id_list(a, p, "ids") : (nodes ? m.node_ids() : m.element_ids());
      std::sort(ids.begin(), ids.end());
      if (std::adjacent_find(ids.begin(), ids.end()) != ids.end())
        throw Error("name_conflict", "ids 에 같은 번호가 두 번 있습니다", {{"param", "ids"}});
      std::map<Id, Id> map;
      Id next = p["start"].get<Id>();
      for (Id id : ids) {
        if (nodes ? !m.has_node(id) : !m.has_element(id))
          throw Error("not_found", std::string(nodes ? "노드" : "요소") + "가 없습니다: " + std::to_string(id), {{"id", id}});
        map[id] = next++;
      }
      const std::set<Id> moving(ids.begin(), ids.end());
      for (const auto& [from, to] : map)
        if (!moving.count(to) && (nodes ? m.has_node(to) : m.has_element(to)))
          throw Error("name_conflict", "새 번호가 다른 " + std::string(nodes ? "노드" : "요소") + "와 겹칩니다: " + std::to_string(to), {{"id", to}});
      std::vector<Element> elems;
      for (std::size_t i = 0; i < m.element_count(); ++i) elems.push_back(m.element_at(i));
      std::vector<Id> all_elem_ids = m.element_ids();
      if (nodes) {
        std::vector<Id> old_ids = ids, new_ids;
        std::vector<double> xyz;
        for (Id id : ids) {
          const Vec3 x = m.node(id);
          xyz.insert(xyz.end(), x.begin(), x.end());
          new_ids.push_back(map.at(id));
        }
        m.remove_elements(all_elem_ids);  // 요소가 노드를 붙잡고 있으므로 잠시 떼어 낸다
        m.remove_nodes(old_ids);
        m.add_nodes(new_ids, xyz);
        for (Element& e : elems)
          for (Id& n : e.nodes) {
            auto it = map.find(n);
            if (it != map.end()) n = it->second;
          }
        m.add_elements(elems);
        remap_object_refs(a, map, {});
      } else {
        std::vector<Element> renamed;
        for (const Element& e : elems)
          if (map.count(e.id)) {
            Element n = e;
            n.id = map.at(e.id);
            renamed.push_back(std::move(n));
          }
        m.remove_elements(ids);
        m.add_elements(renamed);
        remap_object_refs(a, {}, map);
      }
      return Json{{"count", ids.size()}, {"first", map.empty() ? 0 : map.begin()->second}, {"last", next - 1}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("id.check", 'Q', "mesh", "번호 범위와, 객체가 가리키지만 메시에 없는 노드·요소를 조회한다", "CMN-06");
    c.fn = [](App& a, const Json&) {
      const Mesh& m = a.mesh();
      Json missing = Json::array();
      for (const Object* o : a.model().all()) {
        const KindSpec* ks = a.schema().find(o->kind);
        if (!ks) continue;
        Json props = o->props;
        std::set<std::pair<std::string, Id>> bad;
        for_each_mesh_ref(ks->fields_for(subtype_of(*o)), props, [&](const std::string& what, Json& v) {
          if (!v.is_number_integer() || v.get<std::int64_t>() < 1) return;
          const Id id = v.get<Id>();
          if (what == "node" ? !m.has_node(id) : !m.has_element(id)) bad.insert({what, id});
        });
        for (const auto& [what, id] : bad)
          missing.push_back(Json{{"object", o->id}, {"kind", o->kind}, {"name", o->name}, {"what", what}, {"id", id}});
      }
      auto range = [](const std::vector<Id>& ids) {
        return Json{{"count", ids.size()}, {"min", ids.empty() ? 0 : ids.front()}, {"max", ids.empty() ? 0 : ids.back()},
                    {"contiguous", ids.empty() || ids.back() - ids.front() + 1 == ids.size()}};
      };
      return Json{{"nodes", range(m.node_ids())}, {"elements", range(m.element_ids())}, {"missing", missing}};
    };
    app.register_command(std::move(c));
  }

  // ---------------------------------------------------------------- 프로퍼티 할당 현황(메시 기준)
  {
    CommandSpec c = base("property.assignments", 'Q', "property", "할당 현황을 조회한다: 프로퍼티별 대상, 미할당 요소, 요소 종류와 맞지 않는 프로퍼티", "PRP-14");
    c.fn = [](App& a, const Json&) {
      const Mesh& m = a.mesh();
      Json arr = Json::array();
      for (const Object* o : a.model().by_kind("property"))
        arr.push_back(Json{{"id", o->id}, {"name", o->name}, {"type", subtype_of(*o)},
                           {"suppressed", o->suppressed}, {"target", o->props.value("target", Json())}});
      const std::map<Id, Id> prop_of = property_of_elements(a);
      Json unassigned = Json::array(), mismatched = Json::array();
      for (std::size_t i = 0; i < m.element_count(); ++i) {
        const Id e = m.element_ids()[i];
        auto it = prop_of.find(e);
        if (it == prop_of.end()) {
          unassigned.push_back(e);
          continue;
        }
        const int want = property_dimension(subtype_of(a.model().get(it->second)));
        if (want >= 0 && want != shape_info(m.shape_at(i)).dim)
          mismatched.push_back(Json{{"element", e}, {"property", it->second}});
      }
      return Json{{"assignments", arr}, {"elements", m.element_count()}, {"assigned", prop_of.size()},
                  {"unassigned", unassigned}, {"mismatched", mismatched}};
    };
    app.register_command(std::move(c));
  }
}

}  // namespace nasa95
