#include "ofep/model.hpp"

#include <algorithm>
#include <cstdio>

#include "ofep/error.hpp"

namespace ofep {

Json Object::to_json() const {
  return Json{{"id", id},       {"kind", kind},           {"name", name},  {"parent", parent},
              {"order", order}, {"suppressed", suppressed}, {"props", props}};
}

Object Object::from_json(const Json& j) {
  Object o;
  o.id = j.at("id").get<Id>();
  o.kind = j.at("kind").get<std::string>();
  o.name = j.value("name", std::string());
  o.parent = j.value("parent", Id{0});
  o.order = j.value("order", std::int64_t{0});
  o.suppressed = j.value("suppressed", false);
  o.props = j.value("props", Json::object());
  return o;
}

bool Object::operator==(const Object& o) const {
  return id == o.id && kind == o.kind && name == o.name && parent == o.parent && order == o.order &&
         suppressed == o.suppressed && props == o.props;
}

const Object* Model::find(Id id) const {
  auto it = objs_.find(id);
  return it == objs_.end() ? nullptr : &it->second;
}

const Object& Model::get(Id id) const {
  if (const Object* o = find(id)) return *o;
  throw Error("not_found", "객체를 찾을 수 없습니다: id=" + std::to_string(id), {{"object", id}});
}

std::vector<const Object*> Model::all() const {
  std::vector<const Object*> v;
  v.reserve(objs_.size());
  for (const auto& [id, o] : objs_) v.push_back(&o);
  return v;
}

std::vector<const Object*> Model::by_kind(const std::string& kind) const {
  std::vector<const Object*> v;
  for (const auto& [id, o] : objs_)
    if (o.kind == kind) v.push_back(&o);
  std::sort(v.begin(), v.end(), [](const Object* a, const Object* b) {
    if (a->parent != b->parent) return a->parent < b->parent;
    if (a->order != b->order) return a->order < b->order;
    return a->id < b->id;
  });
  return v;
}

std::vector<const Object*> Model::children(Id parent, const std::string& kind) const {
  std::vector<const Object*> v;
  for (const auto& [id, o] : objs_)
    if (o.parent == parent && (kind.empty() || o.kind == kind)) v.push_back(&o);
  std::sort(v.begin(), v.end(), [](const Object* a, const Object* b) {
    if (a->order != b->order) return a->order < b->order;
    return a->id < b->id;
  });
  return v;
}

void Model::record(Change c) {
  if (recording_) rec_.push_back(std::move(c));
}

Id Model::create(Object o) {
  if (o.id == 0)
    o.id = next_++;
  else if (o.id >= next_)
    next_ = o.id + 1;
  if (objs_.count(o.id)) throw Error("internal", "ID 가 이미 있습니다: " + std::to_string(o.id));
  const Id id = o.id;
  record(Change{id, std::nullopt, o});
  objs_.emplace(id, std::move(o));
  return id;
}

void Model::replace(const Object& o) {
  auto it = objs_.find(o.id);
  if (it == objs_.end())
    throw Error("not_found", "객체를 찾을 수 없습니다: id=" + std::to_string(o.id), {{"object", o.id}});
  if (it->second == o) return;
  record(Change{o.id, it->second, o});
  it->second = o;
}

void Model::remove(Id id) {
  auto it = objs_.find(id);
  if (it == objs_.end())
    throw Error("not_found", "객체를 찾을 수 없습니다: id=" + std::to_string(id), {{"object", id}});
  record(Change{id, it->second, std::nullopt});
  objs_.erase(it);
}

void Model::begin_record() {
  if (recording_) throw Error("internal", "이미 변경 기록 중입니다");
  recording_ = true;
  rec_.clear();
}

ChangeSet Model::end_record() {
  recording_ = false;
  ChangeSet cs = std::move(rec_);
  rec_.clear();
  return cs;
}

void Model::apply_inverse(const ChangeSet& cs) {
  for (auto it = cs.rbegin(); it != cs.rend(); ++it) {
    if (it->before)
      objs_[it->id] = *it->before;
    else
      objs_.erase(it->id);
  }
}

void Model::apply_forward(const ChangeSet& cs) {
  for (const Change& c : cs) {
    if (c.after)
      objs_[c.id] = *c.after;
    else
      objs_.erase(c.id);
  }
}

void Model::clear() {
  objs_.clear();
  next_ = 1;
  recording_ = false;
  rec_.clear();
}

Json Model::to_json() const {
  Json arr = Json::array();
  for (const auto& [id, o] : objs_) arr.push_back(o.to_json());
  return Json{{"format", "open-fep"}, {"version", 1}, {"next_id", next_}, {"objects", arr}};
}

void Model::load_json(const Json& j) {
  if (!j.is_object() || j.value("format", std::string()) != "open-fep")
    throw Error("invalid_file", "open-fep 프로젝트 파일이 아닙니다");
  std::map<Id, Object> objs;
  for (const Json& jo : j.at("objects")) {
    Object o = Object::from_json(jo);
    objs.emplace(o.id, std::move(o));
  }
  Id next = j.value("next_id", Id{1});
  for (const auto& [id, o] : objs)
    if (id >= next) next = id + 1;
  objs_ = std::move(objs);
  next_ = next;
  recording_ = false;
  rec_.clear();
}

namespace {
std::string fnv1a_hex(const std::string& s) {
  std::uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
  return buf;
}
}  // namespace

Json Model::digest() const {
  std::map<std::string, Json> by_kind;
  Json all = Json::array();
  for (const auto& [id, o] : objs_) {
    Json jo = o.to_json();
    by_kind[o.kind].push_back(jo);
    all.push_back(std::move(jo));
  }
  Json areas = Json::object();
  for (const auto& [kind, arr] : by_kind) areas[kind] = fnv1a_hex(arr.dump());
  return Json{{"total", fnv1a_hex(all.dump())}, {"areas", areas}};
}

}  // namespace ofep
