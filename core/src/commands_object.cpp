// 객체 공통 동작(생성·속성 변경·이름 변경·복제·삭제·억제·이동·목록·조회·참조 조회·검증)을
// 객체 종류 정의(KindSpec)에서 만들어 등록한다. (`.agent/proj-api-list.md` 0절 "공통 동작")
#include <algorithm>
#include <cctype>
#include <map>
#include <set>

#include "nasa95/app.hpp"
#include "nasa95/error.hpp"
#include "nasa95/geometry.hpp"
#include "nasa95/mesh.hpp"

namespace nasa95 {

namespace {

using F = FieldSpec;

const Object& get_of_kind(const Model& m, Id id, const std::string& kind) {
  const Object& o = m.get(id);
  if (o.kind != kind)
    throw Error("wrong_kind", "id=" + std::to_string(id) + " 는 " + kind + " 가 아닙니다",
                {{"object", id}, {"kind", o.kind}, {"expected", kind}});
  return o;
}

std::string subtype_of(const Object& o) { return o.props.value("type", std::string()); }

// 솔버 입력 파일에 이름이 그대로 나가는 객체의 이름 규칙.
void check_solver_name(const std::string& name) {
  const bool ok = !name.empty() && name.size() <= 80 &&
                  std::all_of(name.begin(), name.end(), [](unsigned char c) {
                    return std::isalnum(c) || c == '_' || c == '-' || c == '.';
                  });
  if (!ok)
    throw Error("invalid_name", "솔버 이름 규칙에 맞지 않습니다(영문·숫자·_ - . 만, 80자 이하): " + name,
                {{"param", "name"}, {"name", name}});
}

void check_name(const App& app, const KindSpec& ks, Id parent, const std::string& name, Id self) {
  if (name.empty()) throw Error("invalid_name", "이름이 비어 있습니다", {{"param", "name"}});
  if (ks.solver_name) check_solver_name(name);
  // 솔버 이름을 쓰는 종류는 모델 전체에서, 그 밖은 같은 부모 안에서 유일해야 한다.
  for (const Object* o : app.model().by_kind(ks.kind)) {
    if (o->id == self || o->name != name) continue;
    if (ks.solver_name || o->parent == parent)
      throw Error("name_conflict", "같은 이름이 이미 있습니다: " + name,
                  {{"param", "name"}, {"name", name}, {"object", o->id}});
  }
}

std::string auto_name(const App& app, const KindSpec& ks, Id parent, const std::string& base) {
  std::set<std::string> used;
  for (const Object* o : app.model().by_kind(ks.kind))
    if (ks.solver_name || o->parent == parent) used.insert(o->name);
  for (int n = 1;; ++n) {
    std::string name = base + "-" + std::to_string(n);
    if (!used.count(name)) return name;
  }
}

// 하중·구속의 부모로 스텝을 주면 그 스텝 전용 셋으로 바꾼다(하중은 반드시 하중 셋 안에, 구속은 구속 셋 안에 — 사용자 결정 2026-10-04).
// 기존 호출(parent=스텝)과 덱 가져오기는 그대로 되고, 트리에는 셋 아래에만 나온다
Id redirect_parent(App& app, const KindSpec& ks, Id parent) {
  if ((ks.kind == "load" || ks.kind == "bc") && parent != 0) {
    const Object* p = app.model().find(parent);
    if (p && p->kind == "step") return step_own_set(app, ks.kind, parent);
  }
  return parent;
}

void check_parent(const App& app, const KindSpec& ks, Id parent) {
  if (ks.parents.empty()) {
    if (parent != 0)
      throw Error("invalid_parent", ks.kind + " 는 최상위에만 둘 수 있습니다", {{"param", "parent"}});
    return;
  }
  if (parent == 0) {
    if (std::find(ks.parents.begin(), ks.parents.end(), "") != ks.parents.end()) return;  // "" = 최상위도 된다(폴더)
    throw Error("missing_param", ks.kind + " 는 상위 객체가 필요합니다", {{"param", "parent"}});
  }
  const Object& p = app.model().get(parent);
  if (std::find(ks.parents.begin(), ks.parents.end(), p.kind) == ks.parents.end())
    throw Error("invalid_parent", ks.kind + " 를 " + p.kind + " 아래에 둘 수 없습니다",
                {{"param", "parent"}, {"object", parent}, {"kind", p.kind}});
}

std::int64_t next_order(const App& app, const std::string& kind, Id parent) {
  std::int64_t mx = -1;
  for (const Object* o : app.model().children(parent, kind)) mx = std::max(mx, o->order);
  return mx + 1;
}

// 속성 값을 검사해 props 에 넣는다. null 은 그 속성을 지운다.
// 형상 대상 [파트, 종류, 번호] 에 영속 이름표를 채운다(D9): [파트, 종류, 번호, 이름]. 이름이 이미 있으면 그것으로 번호를 맞춘다.
Json with_entity_names(const App& app, const Json& target) {
  if (!target.is_object() || target.value("type", std::string()) != "geometry" || !target.contains("ids")) return target;
  Json out = target;
  for (Json& e : out["ids"]) {
    if (!e.is_array() || e.size() < 3) continue;
    const Id part = e[0].get<Id>();
    const std::string type = e[1].get<std::string>();
    App& a = const_cast<App&>(app);
    try {
      if (e.size() >= 4) {  // 이름표가 있으면 지금 번호로 맞춘다
        const int idx = geometry_entity_index(a, part, type, e[3].get<std::string>());
        if (idx) e[2] = idx;
      } else {
        const std::string name = geometry_entity_name(a, part, type, e[2].get<int>());
        if (!name.empty()) e.push_back(name);
      }
    } catch (const Error&) {  // 형상을 만들 수 없으면 번호만 둔다
    }
  }
  return out;
}

void apply_fields(const App& app, const Fields& fields, const Json& params, Json& props) {
  for (const FieldSpec& f : fields) {
    auto it = params.find(f.name);
    if (it == params.end()) continue;
    if (it->is_null()) {
      props.erase(f.name);
      continue;
    }
    check_value(f, *it, &app.model());
    props[f.name] = f.type == "target" ? with_entity_names(app, *it) : *it;
  }
}

std::set<Id> subtree(const Model& m, Id root) {
  std::set<Id> ids{root};
  std::vector<Id> stack{root};
  while (!stack.empty()) {
    const Id cur = stack.back();
    stack.pop_back();
    for (const Object* c : m.children(cur))
      if (ids.insert(c->id).second) stack.push_back(c->id);
  }
  return ids;
}

// ids 안의 객체를 밖에서 참조하는 곳을 찾는다.
Json external_references(const App& app, const std::set<Id>& ids) {
  Json refs = Json::array();
  for (const Object* o : app.model().all()) {
    if (ids.count(o->id)) continue;
    const KindSpec* ks = app.schema().find(o->kind);
    if (!ks) continue;
    for_each_ref(ks->fields_for(subtype_of(*o)), o->props, [&](const FieldSpec& f, Id target) {
      if (ids.count(target))
        refs.push_back(Json{{"id", o->id}, {"kind", o->kind}, {"name", o->name},
                            {"field", f.name}, {"target", target}});
    });
  }
  return refs;
}

Json summary(const Object& o) {
  Json j{{"id", o.id}, {"kind", o.kind}, {"name", o.name}, {"parent", o.parent},
         {"order", o.order}, {"suppressed", o.suppressed}};
  if (o.props.contains("type")) j["type"] = o.props["type"];
  return j;
}

Fields create_params(const KindSpec& ks, const std::string& sub) {
  Fields ps{F("name", "string", "이름(없으면 자동)")};
  if (!ks.parents.empty()) {
    const bool root_ok = std::find(ks.parents.begin(), ks.parents.end(), "") != ks.parents.end();
    ps.push_back(root_ok ? F("parent", "ref", "상위 객체(없으면 최상위)") : F("parent", "ref", "상위 객체").call_req());
  }
  for (FieldSpec f : ks.fields_for(sub)) {
    f.required = false;  // 호출 수준의 필수는 must 로만 판단한다
    ps.push_back(std::move(f));
  }
  return ps;
}

void add_create(App& app, const KindSpec& ks, const std::string& sub) {
  CommandSpec c;
  c.name = ks.kind + (sub.empty() ? ".create" : ".create_" + sub);
  c.kind = 'C', c.undoable = true, c.target = ks.kind, c.features = ks.features;
  c.desc = ks.label + (sub.empty() ? "" : "(" + sub + ")") + " 생성";
  c.params = create_params(ks, sub);
  const std::string kind = ks.kind;
  c.fn = [kind, sub](App& a, const Json& p) {
    const KindSpec& k = a.schema().get(kind);
    const Id parent = redirect_parent(a, k, p.value("parent", Id{0}));
    check_parent(a, k, parent);
    Object o;
    o.kind = kind;
    o.parent = parent;
    o.name = p.contains("name") && !p["name"].is_null() ? p["name"].get<std::string>()
                                                        : auto_name(a, k, parent, sub.empty() ? kind : sub);
    check_name(a, k, parent, o.name, 0);
    if (!sub.empty()) o.props["type"] = sub;
    apply_fields(a, k.fields_for(sub), p, o.props);
    o.order = next_order(a, kind, parent);
    const Id id = a.model().create(o);
    if (k.check) k.check(a.model(), a.model().get(id));
    return Json{{"id", id}, {"name", o.name}};
  };
  app.register_command(std::move(c));
}

void add_common(App& app, const KindSpec& ks) {
  const std::string kind = ks.kind;
  auto base = [&](const std::string& action, char k, bool undoable, const std::string& desc) {
    CommandSpec c;
    c.name = kind + "." + action;
    c.kind = k, c.undoable = undoable, c.target = kind, c.features = ks.features;
    c.desc = ks.label + " " + desc;
    c.params = {F("id", "ref", "대상 객체").call_req()};
    return c;
  };

  {  // update: 준 속성만 바꾼다. 하위 종류의 속성은 그 종류 객체에만 쓸 수 있다.
    CommandSpec c = base("update", 'C', true, "속성 변경");
    // 하위 종류마다 같은 이름의 속성이 다른 타입일 수 있다. 그런 매개변수는 호출 수준에서는
    // 타입을 보지 않고("any"), 처리기가 그 객체의 하위 종류 정의로 검사한다.
    auto add = [&](const Fields& fs) {
      for (FieldSpec f : fs) {
        auto prev = std::find_if(c.params.begin(), c.params.end(), [&](const FieldSpec& p) { return p.name == f.name; });
        if (prev == c.params.end()) {
          f.required = false, f.must = false;
          c.params.push_back(std::move(f));
        } else if (prev->type != f.type) {
          prev->type = "any";
          prev->example = Json();
        }
      }
    };
    add(ks.fields);
    for (const auto& [name, fs] : ks.subtypes) add(fs);
    c.fn = [kind](App& a, const Json& p) {
      const KindSpec& k = a.schema().get(kind);
      Object o = get_of_kind(a.model(), p["id"].get<Id>(), kind);
      const Fields fields = k.fields_for(subtype_of(o));
      for (auto it = p.begin(); it != p.end(); ++it) {
        if (it.key() == "id") continue;
        if (std::none_of(fields.begin(), fields.end(), [&](const FieldSpec& f) { return f.name == it.key(); }))
          throw Error("unknown_param", "'" + it.key() + "' 는 이 종류(" + subtype_of(o) + ")의 속성이 아닙니다",
                      {{"param", it.key()}, {"object", o.id}});
      }
      apply_fields(a, fields, p, o.props);
      a.model().replace(o);
      if (k.check) k.check(a.model(), a.model().get(o.id));
      return Json{{"id", o.id}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("rename", 'C', true, "이름 변경");
    c.params.push_back(F("name", "string", "새 이름").call_req());
    c.fn = [kind](App& a, const Json& p) {
      Object o = get_of_kind(a.model(), p["id"].get<Id>(), kind);
      const std::string name = p["name"].get<std::string>();
      check_name(a, a.schema().get(kind), o.parent, name, o.id);
      o.name = name;
      a.model().replace(o);
      return Json{{"id", o.id}, {"name", name}};
    };
    app.register_command(std::move(c));
  }
  if (ks.copyable) {  // copy: 하위 객체까지 복제하고, 복제된 범위 안을 가리키던 참조는 복제본을 가리키게 한다.
    CommandSpec c = base("copy", 'C', true, "복제");
    c.params.push_back(F("name", "string", "복제본 이름(없으면 자동)"));
    c.params.push_back(F("parent", "ref", "복제본의 상위 객체(없으면 원본과 같음)"));
    c.fn = [kind](App& a, const Json& p) {
      Model& m = a.model();
      const Object src = get_of_kind(m, p["id"].get<Id>(), kind);
      const KindSpec& k = a.schema().get(kind);
      const Id parent = redirect_parent(a, k, p.contains("parent") && !p["parent"].is_null() ? p["parent"].get<Id>() : src.parent);
      check_parent(a, k, parent);
      std::map<Id, Id> remap;
      std::function<Id(const Object&, Id, const std::string&)> clone = [&](const Object& s, Id par,
                                                                          const std::string& name) -> Id {
        Object o = s;
        o.id = 0, o.parent = par, o.name = name;
        o.order = next_order(a, o.kind, par);
        const Id nid = m.create(o);
        remap[s.id] = nid;
        std::vector<Object> kids;  // create 가 맵을 바꾸므로 먼저 복사해 둔다
        for (const Object* ch : m.children(s.id)) kids.push_back(*ch);
        for (const Object& ch : kids)
          if (!remap.count(ch.id)) clone(ch, nid, ch.name);
        return nid;
      };
      std::string name;
      if (p.contains("name") && !p["name"].is_null()) {
        name = p["name"].get<std::string>();
        check_name(a, k, parent, name, 0);
      } else {
        name = auto_name(a, k, parent, src.name + "-copy");
      }
      const Id nid = clone(src, parent, name);
      // 스텝을 복제하면 그 스텝 전용 하중 셋·구속 셋("<스텝> 하중/구속")도 함께 복제해 새 스텝이 가리키게 한다
      // (공용 셋은 그대로 공유). 전용 셋은 스텝에 직접 만든 하중의 그릇이므로 스텝과 운명을 같이한다
      std::vector<std::pair<Id, Id>> steps;  // (원본 스텝, 복제 스텝)
      for (const auto& [old_id, new_id] : remap)
        if (m.get(new_id).kind == "step") steps.emplace_back(old_id, new_id);
      for (const auto& [old_step, new_step] : steps) {
        for (const char* sk : {"load_set", "bc_set"}) {
          std::vector<Object> own;
          for (const Object* s : m.by_kind(sk))
            if (s->props.contains("step") && s->props["step"].is_number() && s->props["step"].get<Id>() == old_step) own.push_back(*s);
          for (const Object& s : own) {
            const Id new_set = clone(s, 0, auto_name(a, a.schema().get(sk), 0, m.get(new_step).name + (std::string(sk) == "load_set" ? " 하중" : " 구속")));
            Object set_obj = m.get(new_set);
            set_obj.props["step"] = new_step;
            m.replace(set_obj);
          }
        }
      }
      for (const auto& [old_id, new_id] : remap) {
        (void)old_id;
        Object o = m.get(new_id);
        const KindSpec* ok = a.schema().find(o.kind);
        if (!ok) continue;
        remap_refs(ok->fields_for(subtype_of(o)), o.props, remap);  // 복제된 스텝의 load_sets/bc_sets 도 새 전용 셋으로 바뀐다
        m.replace(o);
      }
      return Json{{"id", nid}, {"name", name}};
    };
    app.register_command(std::move(c));
  }
  {  // delete: 하위 객체까지 지운다. 밖에서 참조하면 거부하고 참조하는 곳을 알려 준다(CMN-07).
    CommandSpec c = base("delete", 'C', true, "삭제(참조가 있으면 오류와 영향 범위 반환)");
    c.fn = [kind](App& a, const Json& p) {
      Model& m = a.model();
      const Id id = get_of_kind(m, p["id"].get<Id>(), kind).id;
      std::set<Id> ids = subtree(m, id);
      if (kind == "step" || kind == "case") {
        // 스텝 전용 하중 셋·구속 셋("<스텝> 하중/구속", props.step = 그 스텝)은 스텝과 함께 지운다 — 스텝에 직접 만든 하중의 그릇이라 스텝이 없으면 뜻이 없다
        for (const char* sk : {"load_set", "bc_set"})
          for (const Object* s : m.by_kind(sk))
            if (s->props.contains("step") && s->props["step"].is_number() && ids.count(s->props["step"].get<Id>())) {
              const std::set<Id> sub = subtree(m, s->id);
              ids.insert(sub.begin(), sub.end());
            }
      }
      const Json refs = external_references(a, ids);
      if (!refs.empty())
        throw Error("referenced", "다른 객체가 참조하고 있어 삭제할 수 없습니다",
                    {{"object", id}, {"references", refs}});
      if (kind == "folder")  // 폴더를 지우면 든 항목은 폴더 밖으로 나온다(항목은 남는다)
        for (const Object* o : m.all())
          if (o->props.contains("folder") && o->props["folder"].is_number() && o->props["folder"].get<Id>() == id) {
            Object copy = *o;
            copy.props.erase("folder");
            m.replace(copy);
          }
      for (auto it = ids.rbegin(); it != ids.rend(); ++it) m.remove(*it);
      return Json{{"deleted", std::vector<Id>(ids.begin(), ids.end())}};
    };
    app.register_command(std::move(c));
  }
  if (ks.suppressible)
    for (const bool on : {true, false}) {
    CommandSpec c = base(on ? "suppress" : "unsuppress", 'C', true, on ? "해석에서 제외" : "제외 해제");
    c.fn = [kind, on](App& a, const Json& p) {
      Object o = get_of_kind(a.model(), p["id"].get<Id>(), kind);
      o.suppressed = on;
      a.model().replace(o);
      return Json{{"id", o.id}, {"suppressed", on}};
    };
    app.register_command(std::move(c));
  }
  {  // move: 상위 객체와 형제 사이의 위치를 바꾼다.
    CommandSpec c = base("move", 'C', true, "순서·상위 객체·폴더 변경");
    c.params.push_back(F("parent", "ref", "새 상위 객체(없으면 그대로)"));
    c.params.push_back(F("index", "integer", "형제 사이의 위치(0 부터, 없으면 맨 끝)").ge(0));
    if (kind != "folder") {
      c.params.push_back(F("folder", "ref", "넣을 폴더(같은 상위 아래의 이 종류 폴더, WT-07)").ref("folder"));
      c.params.push_back(F("out_of_folder", "bool", "폴더에서 뺀다"));
    }
    if (kind == "part") c.params.push_back(F("assembly", "ref", "옮겨 갈 어셈블리 파트(null 이면 최상위로, GEO-03·WT-06). 형상·ID 는 그대로다").ref("part"));
    c.fn = [kind](App& a, const Json& p) {
      Model& m = a.model();
      if (kind == "part" && p.contains("assembly")) {
        if (!p["assembly"].is_null() && p["assembly"].get<Id>() == p["id"].get<Id>())
          throw Error("cyclic_dependency", "파트를 자기 자신 아래로 옮길 수 없습니다", {{"param", "assembly"}});
        a.invoke("part.update", Json{{"id", p["id"]}, {"assembly", p["assembly"]}});
      }
      Object o = get_of_kind(m, p["id"].get<Id>(), kind);
      const KindSpec& k = a.schema().get(kind);
      const Id parent = redirect_parent(a, k, p.contains("parent") && !p["parent"].is_null() ? p["parent"].get<Id>() : o.parent);
      check_parent(a, k, parent);
      if (parent != o.parent) check_name(a, k, parent, o.name, o.id);
      if (p.value("out_of_folder", false)) {
        o.props.erase("folder");
      } else if (p.contains("folder") && !p["folder"].is_null()) {
        const Id folder = p["folder"].get<Id>();
        const Object* f = m.find(folder);
        if (!f || f->kind != "folder") throw Error("not_found", "없는 폴더입니다: " + std::to_string(folder), {{"param", "folder"}});
        if (f->props.value("kind", std::string()) != kind || f->parent != parent)
          throw Error("invalid_param", "폴더는 같은 상위 객체 아래의 같은 종류(" + kind + ")만 담는다", {{"param", "folder"}, {"object", folder}});
        o.props["folder"] = folder;
      } else if (parent != o.parent) {
        o.props.erase("folder");  // 상위가 바뀌면 폴더에서 나온다
      }
      std::vector<Object> sib;
      std::size_t current = 0;
      for (const Object* s : m.children(parent, kind)) {
        if (s->id == o.id) current = sib.size();
        else sib.push_back(*s);
      }
      std::size_t index = parent == o.parent ? current : sib.size();  // 상위가 그대로면 자리도 그대로(폴더만 바꿀 때)
      if (p.contains("index") && !p["index"].is_null()) {
        check_value(F("index", "integer", "").ge(0), p["index"], nullptr);
        index = std::min<std::size_t>(p["index"].get<std::size_t>(), sib.size());
      }
      o.parent = parent;
      sib.insert(sib.begin() + static_cast<std::ptrdiff_t>(index), o);
      for (std::size_t i = 0; i < sib.size(); ++i) {
        sib[i].order = static_cast<std::int64_t>(i);
        m.replace(sib[i]);
      }
      return Json{{"id", o.id}, {"parent", parent}, {"index", index}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c;
    c.name = kind + ".list", c.kind = 'Q', c.target = kind, c.features = ks.features;
    c.desc = ks.label + " 목록·요약 조회";
    c.params = {F("parent", "ref", "이 상위 객체 아래의 것만")};
    c.fn = [kind](App& a, const Json& p) {
      Json arr = Json::array();
      if (p.contains("parent") && !p["parent"].is_null()) {
        const Id parent = a.model().get(p["parent"].get<Id>()).id;
        for (const Object* o : a.model().children(parent, kind)) arr.push_back(summary(*o));
      } else {
        for (const Object* o : a.model().by_kind(kind)) arr.push_back(summary(*o));
      }
      return arr;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("get", 'Q', false, "속성 전체 조회");
    c.fn = [kind](App& a, const Json& p) { return get_of_kind(a.model(), p["id"].get<Id>(), kind).to_json(); };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("references", 'Q', false, "참조하는 객체 조회");
    c.fn = [kind](App& a, const Json& p) {
      const Id id = get_of_kind(a.model(), p["id"].get<Id>(), kind).id;
      return external_references(a, std::set<Id>{id});
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("validate", 'Q', false, "오류·경고 조회");
    c.fn = [kind](App& a, const Json& p) {
      return diagnose_object(a, get_of_kind(a.model(), p["id"].get<Id>(), kind));
    };
    app.register_command(std::move(c));
  }
}

}  // namespace

// 객체 하나의 진단: 필수 속성 누락(미완성), 끊어진 참조, 물리적 범위 위반, 종류별 추가 진단.
Json diagnose_object(const App& app, const Object& o) {
  Json issues = Json::array();
  const KindSpec* ks = app.schema().find(o.kind);
  if (!ks) return issues;
  auto add = [&](const char* severity, const char* code, const std::string& field, const std::string& msg) {
    issues.push_back(Json{{"severity", severity}, {"code", code}, {"object", o.id},
                          {"kind", o.kind},       {"name", o.name}, {"field", field}, {"message", msg}});
  };
  const Fields fields = ks->fields_for(subtype_of(o));
  for (const FieldSpec& f : fields) {
    auto it = o.props.find(f.name);
    if ((it == o.props.end() || it->is_null()) && f.required)
      add("incomplete", "missing_field", f.name, "'" + f.name + "' 이 지정되지 않았습니다");
  }
  for_each_soft_violation(fields, o.props, "", [&](const std::string& path) {
    add("error", "out_of_range", path, "'" + path + "' 의 값이 물리적 범위를 벗어났습니다");
  });
  for_each_ref(fields, o.props, [&](const FieldSpec& f, Id target) {
    const Object* t = app.model().find(target);
    if (!t)
      add("error", "dangling_reference", f.name, "'" + f.name + "' 이 가리키는 객체가 없습니다");
    else if (t->suppressed && !o.suppressed)
      add("error", "suppressed_reference", f.name, "'" + f.name + "' 이 가리키는 객체가 억제되어 있습니다");
  });
  // 끊어진 대상(WT-29): 적용 대상이 가리키는 셋·파트가 없거나, 형상 엔티티 번호가 지금 형상에 없거나, 메시 노드·요소가 없으면
  for (const FieldSpec& f : fields) {
    if (f.type != "target") continue;
    auto it = o.props.find(f.name);
    if (it == o.props.end() || !it->is_object() || !it->contains("type") || !it->contains("ids")) continue;
    const std::string type = (*it)["type"].get<std::string>();
    const Json& ids = (*it)["ids"];
    std::string broken;
    if (type == "set" || type == "parts") {
      for (const Json& id : ids)
        if (id.is_number() && !app.model().find(id.get<Id>())) broken = "가리키는 " + std::string(type == "set" ? "셋" : "파트") + " 이 없습니다";
    } else if (type == "geometry") {
      for (const Json& item : ids) {
        if (!item.is_array() || item.size() < 3) continue;
        const Object* part = app.model().find(item[0].get<Id>());
        if (!part || part->kind != "part") {
          broken = "가리키는 형상 파트가 없습니다";
          break;
        }
        if (!app.commands().count("geometry.entities")) continue;
        try {
          const std::string t = item[1].get<std::string>();
          if (item.size() >= 4) {  // 이름표가 있으면 그 엔티티가 아직 있는지로 본다(번호가 밀려도 따라간다)
            if (!geometry_entity_index(const_cast<App&>(app), part->id, t, item[3].get<std::string>())) {
              broken = "형상에 " + t + " '" + item[3].get<std::string>() + "' 이(가) 더는 없습니다(형상이 바뀜)";
              break;
            }
            continue;
          }
          const Json ent = app.commands().at("geometry.entities").fn(const_cast<App&>(app), Json{{"id", part->id}});
          const int count = ent.value(t == "solid" ? "solids" : t == "face" ? "faces" : t == "edge" ? "edges" : "vertices", 0);
          if (item[2].get<int>() > count) {
            broken = "형상에 " + t + " " + std::to_string(item[2].get<int>()) + " 이(가) 없습니다(형상이 바뀜)";
            break;
          }
        } catch (const Error&) {
          broken = "형상을 만들 수 없어 대상을 찾지 못합니다";
          break;
        }
      }
    } else if ((type == "nodes" || type == "elements") && !app.mesh().empty()) {  // 메시가 없으면(메싱 전) 판정하지 않는다
      const Mesh& m = app.mesh();
      for (const Json& id : ids)
        if (id.is_number() && !(type == "nodes" ? m.has_node(id.get<Id>()) : m.has_element(id.get<Id>()))) {
          broken = "메시에 " + std::string(type == "nodes" ? "노드 " : "요소 ") + std::to_string(id.get<Id>()) + " 이(가) 없습니다";
          break;
        }
    } else if (type == "faces" && !app.mesh().empty()) {
      const Mesh& m = app.mesh();
      for (const Json& pair : ids)
        if (pair.is_array() && !pair.empty() && pair[0].is_number() && !m.has_element(pair[0].get<Id>())) {
          broken = "메시에 요소 " + std::to_string(pair[0].get<Id>()) + " 이(가) 없습니다";
          break;
        }
    }
    if (!broken.empty()) add("error", "broken_target", f.name, "'" + f.name + "' 의 대상이 끊어졌습니다: " + broken);
  }
  if (ks->diagnose) ks->diagnose(app.model(), o, issues);
  return issues;
}

void register_object_commands(App& app) {
  for (const KindSpec& ks : app.schema().all()) {
    if (ks.subtypes.empty())
      add_create(app, ks, "");
    else
      for (const auto& [sub, fields] : ks.subtypes) add_create(app, ks, sub);
    add_common(app, ks);
  }
}

}  // namespace nasa95
