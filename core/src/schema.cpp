#include "nasa95/schema.hpp"

#include <algorithm>

#include "nasa95/error.hpp"

namespace nasa95 {

Json FieldSpec::to_json() const {
  Json j{{"name", name}, {"type", type}, {"desc", desc}, {"required", required}, {"must", must}};
  if (!ref_kind.empty()) j["ref_kind"] = ref_kind;
  if (!dim.empty()) j["dim"] = dim;
  if (min) j["min"] = *min, j["min_exclusive"] = min_excl;
  if (max) j["max"] = *max, j["max_exclusive"] = max_excl;
  if (soft_min) j["soft_min"] = *soft_min, j["soft_min_exclusive"] = soft_min_excl;
  if (soft_max) j["soft_max"] = *soft_max, j["soft_max_exclusive"] = soft_max_excl;
  if (!choices.empty()) j["choices"] = choices;
  if (cols) j["cols"] = cols;
  if (scalable) j["scalable"] = true;
  if (!mesh_ref.empty()) j["mesh_ref"] = mesh_ref;
  if (!example.is_null()) j["example"] = example;
  if (!items.empty()) {
    Json arr = Json::array();
    for (const FieldSpec& f : items) arr.push_back(f.to_json());
    j["items"] = arr;
  }
  return j;
}

bool KindSpec::has_subtype(const std::string& s) const {
  return std::any_of(subtypes.begin(), subtypes.end(), [&](const auto& p) { return p.first == s; });
}

Fields KindSpec::fields_for(const std::string& subtype) const {
  Fields f = fields;
  for (const auto& [name, sub] : subtypes)
    if (name == subtype) f.insert(f.end(), sub.begin(), sub.end());
  return f;
}

Json KindSpec::to_json() const {
  Json j{{"kind", kind},       {"label", label},           {"collection", collection},
         {"parents", parents}, {"solver_name", solver_name}, {"features", features},
         {"suppressible", suppressible}, {"copyable", copyable}};
  Json fs = Json::array();
  for (const auto& f : fields) fs.push_back(f.to_json());
  j["fields"] = fs;
  Json subs = Json::array();
  for (const auto& [name, sub] : subtypes) {
    Json sf = Json::array();
    for (const auto& f : sub) sf.push_back(f.to_json());
    subs.push_back(Json{{"name", name}, {"fields", sf}});
  }
  j["subtypes"] = subs;
  return j;
}

void Schema::add(KindSpec k) {
  if (find(k.kind)) throw Error("internal", "객체 종류가 이미 등록되어 있습니다: " + k.kind);
  kinds_.push_back(std::move(k));
}

const KindSpec* Schema::find(const std::string& kind) const {
  for (const auto& k : kinds_)
    if (k.kind == kind) return &k;
  return nullptr;
}

const KindSpec& Schema::get(const std::string& kind) const {
  if (const KindSpec* k = find(kind)) return *k;
  throw Error("internal", "등록되지 않은 객체 종류: " + kind);
}

namespace {

const std::vector<std::string> kTargetTypes = {"nodes", "elements", "faces", "set", "parts", "geometry"};

// 대상 종류가 다른 객체를 가리키면 그 객체 종류("" = 가리키지 않음)
std::string target_ref_kind(const std::string& type) { return type == "set" ? "set" : type == "parts" ? "mesh_part" : ""; }

bool is_id(const Json& v) { return v.is_number_integer() && v.get<std::int64_t>() >= 1; }
bool all_numbers(const Json& v) {
  return v.is_array() && std::all_of(v.begin(), v.end(), [](const Json& e) { return e.is_number(); });
}
bool all_integers(const Json& v) {
  return v.is_array() && std::all_of(v.begin(), v.end(), [](const Json& e) { return e.is_number_integer(); });
}

Error type_error(const FieldSpec& f, const std::string& why = "") {
  return Error("invalid_param_type",
               "'" + f.name + "' 의 형식이 맞지 않습니다(기대: " + f.type + ")" + (why.empty() ? "" : ": " + why),
               {{"param", f.name}, {"expected", f.type}});
}

bool out_of_hard_range(const FieldSpec& f, double x) {
  const bool low = f.min && (f.min_excl ? x <= *f.min : x < *f.min);
  const bool high = f.max && (f.max_excl ? x >= *f.max : x > *f.max);
  return low || high;
}

bool out_of_soft_range(const FieldSpec& f, double x) {
  const bool low = f.soft_min && (f.soft_min_excl ? x <= *f.soft_min : x < *f.soft_min);
  const bool high = f.soft_max && (f.soft_max_excl ? x >= *f.soft_max : x > *f.soft_max);
  return low || high;
}

void check_ref(const FieldSpec& f, const std::string& kind, Id id, const Model& model) {
  const Object* o = model.find(id);
  if (!o)
    throw Error("not_found", "'" + f.name + "' 이 가리키는 객체가 없습니다: id=" + std::to_string(id),
                {{"param", f.name}, {"object", id}});
  if (!kind.empty() && o->kind != kind)
    throw Error("wrong_kind", "'" + f.name + "' 은 " + kind + " 객체를 가리켜야 합니다",
                {{"param", f.name}, {"object", id}, {"kind", o->kind}, {"expected", kind}});
}

}  // namespace

bool type_matches(const std::string& type, const Json& v) {
  if (type == "any") return true;
  if (type == "number") return v.is_number();
  if (type == "integer") return v.is_number_integer();
  if (type == "string") return v.is_string();
  if (type == "bool") return v.is_boolean();
  if (type == "vector3") return all_numbers(v) && v.size() == 3;
  if (type == "number_list") return all_numbers(v);
  if (type == "integer_list") return all_integers(v);
  if (type == "string_list")
    return v.is_array() && std::all_of(v.begin(), v.end(), [](const Json& e) { return e.is_string(); });
  if (type == "table") return v.is_array() && std::all_of(v.begin(), v.end(), all_numbers);
  if (type == "array") return v.is_array();
  if (type == "object") return v.is_object();
  if (type == "target") return v.is_object() && v.contains("type") && v["type"].is_string() && v.contains("ids") && v["ids"].is_array();
  if (type == "ref") return is_id(v);
  if (type == "ref_list") return v.is_array() && std::all_of(v.begin(), v.end(), is_id);
  if (type == "object_list")
    return v.is_array() && std::all_of(v.begin(), v.end(), [](const Json& e) { return e.is_object(); });
  return false;
}

void check_value(const FieldSpec& f, const Json& v, const Model* model) {
  if (!type_matches(f.type, v)) throw type_error(f);

  if (f.type == "string" && !f.choices.empty() &&
      std::find(f.choices.begin(), f.choices.end(), v.get<std::string>()) == f.choices.end())
    throw Error("out_of_range", "'" + f.name + "' 의 값이 허용된 선택지에 없습니다",
                {{"param", f.name}, {"choices", f.choices}});

  auto check_number = [&](const Json& e) {
    if (e.is_number() && out_of_hard_range(f, e.get<double>()))
      throw Error("out_of_range", "'" + f.name + "' 의 값이 허용 범위를 벗어났습니다",
                  {{"param", f.name}, {"value", e.get<double>()}});
  };
  if (f.type == "number" || f.type == "integer") {
    check_number(v);
  } else if (f.type == "vector3" || f.type == "number_list" || f.type == "integer_list") {
    for (const Json& e : v) check_number(e);
  } else if (f.type == "table") {
    for (const Json& row : v)
      if (f.cols > 0 && static_cast<int>(row.size()) != f.cols)
        throw type_error(f, "행마다 값이 " + std::to_string(f.cols) + "개여야 합니다");
  } else if (f.type == "target") {
    const std::string t = v["type"].get<std::string>();
    const auto& allowed = f.choices.empty() ? kTargetTypes : f.choices;
    if (std::find(allowed.begin(), allowed.end(), t) == allowed.end())
      throw Error("out_of_range", "'" + f.name + "' 에 쓸 수 없는 대상 종류입니다: " + t,
                  {{"param", f.name}, {"choices", allowed}});
    const Json& ids = v["ids"];
    if (t == "faces") {
      const bool ok = std::all_of(ids.begin(), ids.end(),
                                  [](const Json& e) { return all_integers(e) && e.size() == 2; });
      if (!ok) throw type_error(f, "faces 의 ids 는 [요소, 면 번호] 쌍의 목록이어야 합니다");
    } else if (t == "geometry") {  // 형상 엔티티: [파트, 종류, 번호]
      static const std::vector<std::string> kinds = {"solid", "face", "edge", "vertex"};
      for (const Json& e : ids) {
        const bool ok = e.is_array() && (e.size() == 3 || (e.size() == 4 && e[3].is_string())) && is_id(e[0]) && e[1].is_string() && is_id(e[2]) &&
                        std::find(kinds.begin(), kinds.end(), e[1].get<std::string>()) != kinds.end();
        if (!ok) throw type_error(f, "geometry 의 ids 는 [파트, solid|face|edge|vertex, 번호(, 이름표)] 의 목록이어야 합니다");
        if (model) check_ref(f, "part", e[0].get<Id>(), *model);
      }
    } else if (!std::all_of(ids.begin(), ids.end(), is_id)) {
      throw type_error(f, "ids 는 1 이상의 정수 목록이어야 합니다");
    }
    if (const std::string rk = target_ref_kind(t); !rk.empty() && model)
      for (const Json& e : ids) check_ref(f, rk, e.get<Id>(), *model);
  } else if (f.type == "ref") {
    if (model) check_ref(f, f.ref_kind, v.get<Id>(), *model);
  } else if (f.type == "ref_list") {
    if (model)
      for (const Json& e : v) check_ref(f, f.ref_kind, e.get<Id>(), *model);
  } else if (f.type == "object_list") {
    if (f.items.empty()) return;  // 항목 정의가 없는 목록은 자유 형식(예: 확장이 등록하는 명령의 매개변수 정의)
    std::size_t index = 0;
    for (const Json& item : v) {
      const std::string where = f.name + "[" + std::to_string(index++) + "]";
      for (auto it = item.begin(); it != item.end(); ++it) {
        auto sub = std::find_if(f.items.begin(), f.items.end(), [&](const FieldSpec& s) { return s.name == it.key(); });
        if (sub == f.items.end())
          throw Error("unknown_param", "알 수 없는 항목 속성: " + where + "." + it.key(), {{"param", f.name}});
        try {
          check_value(*sub, it.value(), model);
        } catch (Error& e) {
          e.details()["param"] = f.name;  // 호출자가 준 매개변수 이름으로 보고한다
          e.details()["item"] = where + "." + it.key();
          throw;
        }
      }
      for (const FieldSpec& s : f.items)
        if (s.must && !item.contains(s.name))
          throw Error("missing_param", "항목에 필수 속성이 없습니다: " + where + "." + s.name, {{"param", f.name}});
    }
  }
}

void for_each_ref(const Fields& fields, const Json& props, const std::function<void(const FieldSpec&, Id)>& fn) {
  if (!props.is_object()) return;
  for (const FieldSpec& f : fields) {
    auto it = props.find(f.name);
    if (it == props.end() || it->is_null()) continue;
    if (f.type == "ref") {
      if (is_id(*it)) fn(f, it->get<Id>());
    } else if (f.type == "ref_list") {
      if (it->is_array())
        for (const Json& e : *it)
          if (is_id(e)) fn(f, e.get<Id>());
    } else if (f.type == "target") {
      if (it->is_object() && !target_ref_kind(it->value("type", std::string())).empty() && it->contains("ids"))
        for (const Json& e : (*it)["ids"])
          if (is_id(e)) fn(f, e.get<Id>());
      if (it->is_object() && it->value("type", std::string()) == "geometry" && it->contains("ids"))
        for (const Json& e : (*it)["ids"])
          if (e.is_array() && !e.empty() && is_id(e[0])) fn(f, e[0].get<Id>());  // 형상 대상은 파트를 가리킨다
    } else if (f.type == "object_list") {
      if (it->is_array())
        for (const Json& item : *it) for_each_ref(f.items, item, fn);
    }
  }
}

void remap_refs(const Fields& fields, Json& props, const std::map<Id, Id>& map) {
  if (!props.is_object()) return;
  auto fix = [&](Json& v) {
    if (!is_id(v)) return;
    auto r = map.find(v.get<Id>());
    if (r != map.end()) v = r->second;
  };
  for (const FieldSpec& f : fields) {
    auto it = props.find(f.name);
    if (it == props.end() || it->is_null()) continue;
    if (f.type == "ref") {
      fix(*it);
    } else if (f.type == "ref_list") {
      if (it->is_array())
        for (Json& e : *it) fix(e);
    } else if (f.type == "target") {
      if (it->is_object() && !target_ref_kind(it->value("type", std::string())).empty() && it->contains("ids"))
        for (Json& e : (*it)["ids"]) fix(e);
      if (it->is_object() && it->value("type", std::string()) == "geometry" && it->contains("ids"))
        for (Json& e : (*it)["ids"])
          if (e.is_array() && !e.empty()) fix(e[0]);
    } else if (f.type == "object_list") {
      if (it->is_array())
        for (Json& item : *it) remap_refs(f.items, item, map);
    }
  }
}

void for_each_mesh_ref(const Fields& fields, Json& props, const std::function<void(const std::string&, Json&)>& fn) {
  if (!props.is_object()) return;
  for (const FieldSpec& f : fields) {
    auto it = props.find(f.name);
    if (it == props.end() || it->is_null()) continue;
    if (f.type == "target") {
      if (!it->is_object() || !it->contains("ids")) continue;
      const std::string t = it->value("type", std::string());
      if (t == "nodes")
        for (Json& e : (*it)["ids"]) fn("node", e);
      else if (t == "elements")
        for (Json& e : (*it)["ids"]) fn("element", e);
      else if (t == "faces")
        for (Json& e : (*it)["ids"])
          if (e.is_array() && !e.empty()) fn("element", e[0]);
    } else if (f.type == "object_list") {
      if (it->is_array())
        for (Json& item : *it) for_each_mesh_ref(f.items, item, fn);
    } else if (!f.mesh_ref.empty()) {
      if (it->is_number_integer()) {
        fn(f.mesh_ref, *it);
      } else if (it->is_array()) {
        for (Json& e : *it) {
          if (e.is_number_integer())
            fn(f.mesh_ref, e);
          else if (e.is_array() && f.mesh_col >= 0 && static_cast<int>(e.size()) > f.mesh_col)
            fn(f.mesh_ref, e[static_cast<std::size_t>(f.mesh_col)]);
        }
      }
    }
  }
}

void for_each_soft_violation(const Fields& fields, const Json& props, const std::string& prefix,
                             const std::function<void(const std::string&)>& fn) {
  if (!props.is_object()) return;
  for (const FieldSpec& f : fields) {
    auto it = props.find(f.name);
    if (it == props.end() || it->is_null()) continue;
    if (it->is_number()) {
      if (out_of_soft_range(f, it->get<double>())) fn(prefix + f.name);
    } else if (f.type == "object_list" && it->is_array()) {
      std::size_t i = 0;
      for (const Json& item : *it)
        for_each_soft_violation(f.items, item, prefix + f.name + "[" + std::to_string(i++) + "].", fn);
    }
  }
}

}  // namespace nasa95
