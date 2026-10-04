#include "ofep/mesh.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>

#include "ofep/error.hpp"

namespace ofep {

namespace {

template <typename T>
std::ptrdiff_t find_sorted(const std::vector<T>& v, const T& x) {
  auto it = std::lower_bound(v.begin(), v.end(), x);
  return (it != v.end() && *it == x) ? it - v.begin() : -1;
}

void require_unique(std::vector<Id> ids, const char* what) {
  std::sort(ids.begin(), ids.end());
  auto dup = std::adjacent_find(ids.begin(), ids.end());
  if (dup != ids.end())
    throw Error("name_conflict", std::string(what) + " ID 가 입력 안에서 겹칩니다: " + std::to_string(*dup), {{"id", *dup}});
  if (!ids.empty() && ids.front() < 1) throw Error("out_of_range", std::string(what) + " ID 는 1 이상이어야 합니다");
}

}  // namespace

// ------------------------------------------------------------------ 노드
std::ptrdiff_t Mesh::find_node(Id id) const { return find_sorted(node_ids_, id); }

Vec3 Mesh::node(Id id) const {
  const std::ptrdiff_t i = find_node(id);
  if (i < 0) throw Error("not_found", "노드가 없습니다: " + std::to_string(id), {{"node", id}});
  return {node_xyz_[3 * i], node_xyz_[3 * i + 1], node_xyz_[3 * i + 2]};
}

void Mesh::raw_add_nodes(const std::vector<Id>& ids, const std::vector<double>& xyz) {
  if (ids.empty()) return;
  const bool append = std::is_sorted(ids.begin(), ids.end()) && (node_ids_.empty() || ids.front() > node_ids_.back());
  if (append) {
    node_ids_.insert(node_ids_.end(), ids.begin(), ids.end());
    node_xyz_.insert(node_xyz_.end(), xyz.begin(), xyz.end());
    return;
  }
  // 정렬된 기존 배열과 새 항목을 합친다.
  std::vector<std::size_t> order(ids.size());
  for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return ids[a] < ids[b]; });
  std::vector<Id> out_ids;
  std::vector<double> out_xyz;
  out_ids.reserve(node_ids_.size() + ids.size());
  out_xyz.reserve(node_xyz_.size() + xyz.size());
  std::size_t i = 0, j = 0;
  auto take_old = [&] {
    out_ids.push_back(node_ids_[i]);
    out_xyz.insert(out_xyz.end(), node_xyz_.begin() + 3 * i, node_xyz_.begin() + 3 * i + 3);
    ++i;
  };
  auto take_new = [&] {
    const std::size_t k = order[j];
    out_ids.push_back(ids[k]);
    out_xyz.insert(out_xyz.end(), xyz.begin() + 3 * k, xyz.begin() + 3 * k + 3);
    ++j;
  };
  while (i < node_ids_.size() && j < order.size()) (node_ids_[i] < ids[order[j]]) ? take_old() : take_new();
  while (i < node_ids_.size()) take_old();
  while (j < order.size()) take_new();
  node_ids_ = std::move(out_ids);
  node_xyz_ = std::move(out_xyz);
}

void Mesh::raw_remove_nodes(const std::vector<Id>& ids) {
  if (ids.empty()) return;
  const std::set<Id> gone(ids.begin(), ids.end());
  std::size_t w = 0;
  for (std::size_t r = 0; r < node_ids_.size(); ++r) {
    if (gone.count(node_ids_[r])) continue;
    if (w != r) {
      node_ids_[w] = node_ids_[r];
      std::copy(node_xyz_.begin() + 3 * r, node_xyz_.begin() + 3 * r + 3, node_xyz_.begin() + 3 * w);
    }
    ++w;
  }
  node_ids_.resize(w);
  node_xyz_.resize(3 * w);
}

void Mesh::raw_move_nodes(const std::vector<Id>& ids, const std::vector<double>& xyz) {
  for (std::size_t k = 0; k < ids.size(); ++k) {
    const std::ptrdiff_t i = find_node(ids[k]);
    std::copy(xyz.begin() + 3 * k, xyz.begin() + 3 * k + 3, node_xyz_.begin() + 3 * i);
  }
}

void Mesh::add_nodes(const std::vector<Id>& ids, const std::vector<double>& xyz) {
  if (xyz.size() != 3 * ids.size()) throw Error("invalid_param_type", "좌표는 노드마다 3개여야 합니다", {{"param", "coords"}});
  require_unique(ids, "노드");
  for (Id id : ids)
    if (has_node(id)) throw Error("name_conflict", "노드 ID 가 이미 있습니다: " + std::to_string(id), {{"node", id}});
  raw_add_nodes(ids, xyz);
  if (recording_ && !ids.empty()) log_.push_back(MeshOp{MeshOp::AddNodes, ids, xyz, {}, {}, {}});
}

void Mesh::remove_nodes(const std::vector<Id>& ids) {
  require_unique(ids, "노드");
  const std::vector<Id> used = used_nodes();
  std::vector<double> old;
  old.reserve(3 * ids.size());
  for (Id id : ids) {
    const Vec3 p = node(id);
    if (find_sorted(used, id) >= 0)
      throw Error("referenced", "요소가 쓰는 노드는 지울 수 없습니다: " + std::to_string(id), {{"node", id}});
    old.insert(old.end(), p.begin(), p.end());
  }
  raw_remove_nodes(ids);
  if (recording_ && !ids.empty()) log_.push_back(MeshOp{MeshOp::RemoveNodes, ids, {}, old, {}, {}});
}

void Mesh::move_nodes(const std::vector<Id>& ids, const std::vector<double>& xyz) {
  if (xyz.size() != 3 * ids.size()) throw Error("invalid_param_type", "좌표는 노드마다 3개여야 합니다", {{"param", "coords"}});
  require_unique(ids, "노드");
  std::vector<double> old;
  old.reserve(xyz.size());
  for (Id id : ids) {
    const Vec3 p = node(id);
    old.insert(old.end(), p.begin(), p.end());
  }
  if (old == xyz) return;
  raw_move_nodes(ids, xyz);
  if (recording_) log_.push_back(MeshOp{MeshOp::MoveNodes, ids, xyz, old, {}, {}});
}

// ------------------------------------------------------------------ 요소
std::ptrdiff_t Mesh::find_element(Id id) const { return find_sorted(elem_ids_, id); }

std::uint16_t Mesh::intern(const std::string& type) {
  auto it = std::find(types_.begin(), types_.end(), type);
  if (it != types_.end()) return static_cast<std::uint16_t>(it - types_.begin());
  types_.push_back(type);
  return static_cast<std::uint16_t>(types_.size() - 1);
}

Element Mesh::element_at(std::size_t i) const {
  Element e;
  e.id = elem_ids_[i], e.shape = elem_shape_[i], e.part = elem_part_[i], e.type = types_[elem_type_[i]];
  e.nodes.assign(conn_.begin() + elem_offset_[i], conn_.begin() + elem_offset_[i + 1]);
  return e;
}

Element Mesh::element(Id id) const {
  const std::ptrdiff_t i = find_element(id);
  if (i < 0) throw Error("not_found", "요소가 없습니다: " + std::to_string(id), {{"element", id}});
  return element_at(static_cast<std::size_t>(i));
}

std::vector<Element> Mesh::all_elements() const {
  std::vector<Element> v;
  v.reserve(elem_ids_.size());
  for (std::size_t i = 0; i < elem_ids_.size(); ++i) v.push_back(element_at(i));
  return v;
}

void Mesh::rebuild_elements(std::vector<Element> all) {
  std::sort(all.begin(), all.end(), [](const Element& a, const Element& b) { return a.id < b.id; });
  elem_ids_.clear(), elem_shape_.clear(), elem_part_.clear(), elem_type_.clear(), conn_.clear();
  elem_offset_.assign(1, 0);
  for (const Element& e : all) {
    elem_ids_.push_back(e.id);
    elem_shape_.push_back(e.shape);
    elem_part_.push_back(e.part);
    elem_type_.push_back(intern(e.type));
    conn_.insert(conn_.end(), e.nodes.begin(), e.nodes.end());
    elem_offset_.push_back(conn_.size());
  }
}

void Mesh::raw_add_elements(const std::vector<Element>& elems) {
  if (elems.empty()) return;
  const bool append = std::is_sorted(elems.begin(), elems.end(), [](const Element& a, const Element& b) { return a.id < b.id; }) &&
                      (elem_ids_.empty() || elems.front().id > elem_ids_.back());
  if (append) {
    for (const Element& e : elems) {
      elem_ids_.push_back(e.id);
      elem_shape_.push_back(e.shape);
      elem_part_.push_back(e.part);
      elem_type_.push_back(intern(e.type));
      conn_.insert(conn_.end(), e.nodes.begin(), e.nodes.end());
      elem_offset_.push_back(conn_.size());
    }
    return;
  }
  std::vector<Element> all = all_elements();
  all.insert(all.end(), elems.begin(), elems.end());
  rebuild_elements(std::move(all));
}

void Mesh::raw_remove_elements(const std::vector<Id>& ids) {
  if (ids.empty()) return;
  const std::set<Id> gone(ids.begin(), ids.end());
  std::vector<Element> keep;
  keep.reserve(elem_ids_.size());
  for (std::size_t i = 0; i < elem_ids_.size(); ++i)
    if (!gone.count(elem_ids_[i])) keep.push_back(element_at(i));
  rebuild_elements(std::move(keep));
}

namespace {
void check_element(const Mesh& m, const Element& e) {
  const ShapeInfo& info = shape_info(e.shape);
  if (static_cast<int>(e.nodes.size()) != info.nodes)
    throw Error("invalid_param_type",
                std::string(info.name) + " 요소의 절점은 " + std::to_string(info.nodes) + "개여야 합니다",
                {{"element", e.id}, {"param", "connectivity"}});
  for (Id n : e.nodes)
    if (!(n == 0 && e.type == "D") && !m.has_node(n))  // 네트워크 요소의 입구·출구는 절점 0 으로 적는다
      throw Error("not_found", "요소가 가리키는 노드가 없습니다: " + std::to_string(n), {{"element", e.id}, {"node", n}});
  if (!e.type.empty() &&
      std::none_of(info.types.begin(), info.types.end(), [&](const char* t) { return e.type == t; }))
    throw Error("out_of_range", std::string(info.name) + " 형상에 쓸 수 없는 솔버 요소 타입입니다: " + e.type,
                {{"element", e.id}, {"param", "type"}});
}
}  // namespace

void Mesh::add_elements(const std::vector<Element>& elems) {
  std::vector<Id> ids;
  ids.reserve(elems.size());
  for (const Element& e : elems) ids.push_back(e.id);
  require_unique(ids, "요소");
  for (const Element& e : elems) {
    if (has_element(e.id)) throw Error("name_conflict", "요소 ID 가 이미 있습니다: " + std::to_string(e.id), {{"element", e.id}});
    check_element(*this, e);
  }
  raw_add_elements(elems);
  if (recording_ && !elems.empty()) log_.push_back(MeshOp{MeshOp::AddElements, {}, {}, {}, elems, {}});
}

void Mesh::remove_elements(const std::vector<Id>& ids) {
  require_unique(ids, "요소");
  std::vector<Element> old;
  old.reserve(ids.size());
  for (Id id : ids) old.push_back(element(id));
  raw_remove_elements(ids);
  if (recording_ && !ids.empty()) log_.push_back(MeshOp{MeshOp::RemoveElements, {}, {}, {}, {}, old});
}

void Mesh::replace_elements(const std::vector<Element>& elems) {
  std::vector<Id> ids;
  std::vector<Element> old;
  for (const Element& e : elems) {
    ids.push_back(e.id);
    old.push_back(element(e.id));
    check_element(*this, e);
  }
  require_unique(ids, "요소");
  raw_remove_elements(ids);
  raw_add_elements(elems);
  if (recording_ && !elems.empty()) log_.push_back(MeshOp{MeshOp::ReplaceElements, {}, {}, {}, elems, old});
}

std::vector<Id> Mesh::used_nodes() const {
  std::vector<Id> v = conn_;
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
  if (!v.empty() && v.front() == 0) v.erase(v.begin());  // 네트워크 요소의 0번 절점은 노드가 아니다
  return v;
}

// ------------------------------------------------------------------ 기록·되돌리기
void Mesh::begin_record() {
  if (recording_) throw Error("internal", "이미 메시 변경 기록 중입니다");
  recording_ = true;
  log_.clear();
}

MeshLog Mesh::end_record() {
  recording_ = false;
  MeshLog log = std::move(log_);
  log_.clear();
  return log;
}

namespace {
std::vector<Id> ids_of(const std::vector<Element>& elems) {
  std::vector<Id> ids;
  ids.reserve(elems.size());
  for (const Element& e : elems) ids.push_back(e.id);
  return ids;
}
}  // namespace

void Mesh::apply_inverse(const MeshLog& log) {
  for (auto it = log.rbegin(); it != log.rend(); ++it) {
    switch (it->kind) {
      case MeshOp::AddNodes: raw_remove_nodes(it->ids); break;
      case MeshOp::RemoveNodes: raw_add_nodes(it->ids, it->old_xyz); break;
      case MeshOp::MoveNodes: raw_move_nodes(it->ids, it->old_xyz); break;
      case MeshOp::AddElements: raw_remove_elements(ids_of(it->elems)); break;
      case MeshOp::RemoveElements: raw_add_elements(it->old_elems); break;
      case MeshOp::ReplaceElements:
        raw_remove_elements(ids_of(it->elems));
        raw_add_elements(it->old_elems);
        break;
    }
  }
}

void Mesh::apply_forward(const MeshLog& log) {
  for (const MeshOp& op : log) {
    switch (op.kind) {
      case MeshOp::AddNodes: raw_add_nodes(op.ids, op.xyz); break;
      case MeshOp::RemoveNodes: raw_remove_nodes(op.ids); break;
      case MeshOp::MoveNodes: raw_move_nodes(op.ids, op.xyz); break;
      case MeshOp::AddElements: raw_add_elements(op.elems); break;
      case MeshOp::RemoveElements: raw_remove_elements(ids_of(op.old_elems)); break;
      case MeshOp::ReplaceElements:
        raw_remove_elements(ids_of(op.old_elems));
        raw_add_elements(op.elems);
        break;
    }
  }
}

void Mesh::clear() {
  node_ids_.clear(), node_xyz_.clear();
  elem_ids_.clear(), elem_shape_.clear(), elem_part_.clear(), elem_type_.clear(), conn_.clear();
  elem_offset_.assign(1, 0);
  types_.assign(1, "");
  recording_ = false;
  log_.clear();
}

// ------------------------------------------------------------------ 저장·다이제스트
Json Mesh::to_json() const {
  std::vector<int> shapes;
  std::vector<std::string> types;
  for (std::size_t i = 0; i < elem_ids_.size(); ++i) {
    shapes.push_back(static_cast<int>(elem_shape_[i]));
    types.push_back(types_[elem_type_[i]]);
  }
  return Json{{"node_ids", node_ids_}, {"node_xyz", node_xyz_},   {"elem_ids", elem_ids_},
              {"elem_shape", shapes},  {"elem_part", elem_part_}, {"elem_type", types},
              {"elem_offset", elem_offset_}, {"conn", conn_}};
}

void Mesh::load_json(const Json& j) {
  Mesh m;
  m.node_ids_ = j.at("node_ids").get<std::vector<Id>>();
  m.node_xyz_ = j.at("node_xyz").get<std::vector<double>>();
  const auto ids = j.at("elem_ids").get<std::vector<Id>>();
  const auto shapes = j.at("elem_shape").get<std::vector<int>>();
  const auto parts = j.at("elem_part").get<std::vector<Id>>();
  const auto types = j.at("elem_type").get<std::vector<std::string>>();
  const auto offset = j.at("elem_offset").get<std::vector<std::size_t>>();
  const auto conn = j.at("conn").get<std::vector<Id>>();
  const std::size_t n = ids.size();
  if (m.node_xyz_.size() != 3 * m.node_ids_.size() || !std::is_sorted(m.node_ids_.begin(), m.node_ids_.end()) ||
      shapes.size() != n || parts.size() != n || types.size() != n || offset.size() != n + 1 ||
      (n > 0 && offset.back() != conn.size()))
    throw Error("invalid_file", "프로젝트 파일의 메시 자료가 올바르지 않습니다");
  std::vector<Element> all;
  all.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    if (shapes[i] < 0 || shapes[i] >= static_cast<int>(all_shapes().size()) || offset[i] > offset[i + 1])
      throw Error("invalid_file", "프로젝트 파일의 메시 자료가 올바르지 않습니다");
    Element e;
    e.id = ids[i], e.shape = static_cast<Shape>(shapes[i]), e.part = parts[i], e.type = types[i];
    e.nodes.assign(conn.begin() + offset[i], conn.begin() + offset[i + 1]);
    check_element(m, e);
    all.push_back(std::move(e));
  }
  m.rebuild_elements(std::move(all));
  *this = std::move(m);
}

std::string Mesh::digest() const {
  std::uint64_t h = 1469598103934665603ULL;
  auto feed = [&](const void* data, std::size_t size) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
      h ^= p[i];
      h *= 1099511628211ULL;
    }
  };
  feed(node_ids_.data(), node_ids_.size() * sizeof(Id));
  feed(node_xyz_.data(), node_xyz_.size() * sizeof(double));
  feed(elem_ids_.data(), elem_ids_.size() * sizeof(Id));
  feed(elem_shape_.data(), elem_shape_.size() * sizeof(Shape));
  feed(elem_part_.data(), elem_part_.size() * sizeof(Id));
  for (std::size_t i = 0; i < elem_type_.size(); ++i) {
    const std::string& t = types_[elem_type_[i]];
    feed(t.data(), t.size());
    feed("|", 1);
  }
  feed(conn_.data(), conn_.size() * sizeof(Id));
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
  return buf;
}

}  // namespace ofep
