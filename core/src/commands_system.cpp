// 프로젝트·시스템 명령 (`.agent/proj-api-list.md` 1절)
#include <algorithm>
#include <any>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>

#include "nasa95/app.hpp"
#include "nasa95/mesh.hpp"
#include "nasa95/geometry.hpp"
#include "nasa95/mesher.hpp"
#include "nasa95/error.hpp"

namespace nasa95 {

namespace {

using F = FieldSpec;

bool has(const Json& p, const char* key) { return p.contains(key) && !p[key].is_null(); }

CommandSpec make(std::string name, char kind, std::string desc, Fields params, std::string features,
                 std::function<Json(App&, const Json&)> fn) {
  CommandSpec c;
  c.name = std::move(name), c.kind = kind, c.desc = std::move(desc), c.undoable = (kind == 'C');
  c.params = std::move(params), c.features = std::move(features), c.fn = std::move(fn);
  return c;
}

Json tree_node(const App& a, const Object& o);

// 한 상위 객체 아래의 항목들을 폴더(WT-07)로 묶어 트리 항목 목록으로 만든다. 폴더는 자기 종류의 항목 앞에 나오고,
// 폴더에 든 항목은 그 폴더의 children 에, 나머지는 그대로 나온다. 없는 폴더를 가리키는 항목은 폴더 밖으로 본다.
// 트리 조회 옵션(WT-30·WT-37·WT-38·WT-08): 정렬, 스텝 기준 비활성, 요약.
struct TreeOptions {
  std::string sort;  // "" = 생성 순서(ID), "name", "id"
  bool descending = false;
  bool summary = true;
  std::set<Id> removed_elements;   // step 기준으로 제거된 요소(비활성 판정)
  std::set<Id> removed_nodes;      // 제거된 요소에만 속한 노드
  bool has_step = false;
};

// 스텝·피처는 정렬해도 순서가 바뀌지 않는다(실행 순서·이력 순서가 의미를 가진다).
bool keeps_order(const std::string& kind) { return kind == "step" || kind == "feature" || kind == "folder"; }

void sort_nodes(std::vector<Json>& nodes, const TreeOptions& opt) {
  if (opt.sort.empty() && !opt.descending) return;
  if (!nodes.empty() && keeps_order(nodes.front().value("kind", std::string()))) return;
  std::stable_sort(nodes.begin(), nodes.end(), [&](const Json& x, const Json& y) {
    bool less;
    if (opt.sort == "name") {
      const std::string nx = x["name"].get<std::string>(), ny = y["name"].get<std::string>();
      less = nx != ny ? nx < ny : x["id"].get<Id>() < y["id"].get<Id>();
    } else {
      less = x["id"].get<Id>() < y["id"].get<Id>();
    }
    return opt.descending ? !less && (x["id"] != y["id"]) : less;
  });
}

Json tree_node(const App& a, const Object& o, const TreeOptions& opt);

Json tree_items(const App& a, Id parent, const std::string& only_kind, const TreeOptions& opt) {
  Json items = Json::array();
  std::map<Id, Json> folder_nodes;  // 폴더 → 트리 항목(children 을 채운다)
  std::vector<Id> folder_order;
  for (const Object* f : a.model().children(parent, "folder")) {
    if (!only_kind.empty() && f->props.value("kind", std::string()) != only_kind) continue;
    folder_nodes[f->id] = tree_node(a, *f, opt);
    folder_nodes[f->id]["children"] = Json::array();
    folder_order.push_back(f->id);
  }
  std::vector<Json> loose;
  std::map<Id, std::vector<Json>> in_folder;
  for (const Object* c : a.model().children(parent)) {
    if (c->kind == "folder") continue;
    if (!only_kind.empty() && c->kind != only_kind) continue;
    if (c->kind == "part" && c->props.contains("assembly") && c->props["assembly"].is_number() && a.model().find(c->props["assembly"].get<Id>()))
      continue;  // 어셈블리에 든 파트는 그 파트 아래에 나온다(WT-06)
    Json node = tree_node(a, *c, opt);
    const Id folder = c->props.contains("folder") && c->props["folder"].is_number() ? c->props["folder"].get<Id>() : 0;
    auto it = folder_nodes.find(folder);
    if (folder && it != folder_nodes.end()) in_folder[folder].push_back(std::move(node));
    else loose.push_back(std::move(node));
  }
  for (auto& [f, nodes] : in_folder) {
    sort_nodes(nodes, opt);
    for (Json& n : nodes) folder_nodes[f]["children"].push_back(std::move(n));
  }
  sort_nodes(loose, opt);
  for (Id f : folder_order) items.push_back(std::move(folder_nodes[f]));
  for (Json& n : loose) items.push_back(std::move(n));
  return items;
}

// 항목 요약(WT-08): 종류별로 한눈에 보일 값.
Json tree_summary(const App& a, const Object& o) {
  Json s = Json::object();
  const Json& q = o.props;
  if (o.kind == "material") {
    Json names = Json::array();
    if (q.contains("behaviors"))
      for (auto it = q["behaviors"].begin(); it != q["behaviors"].end(); ++it) names.push_back(it.key());
    s["behaviors"] = names;
  } else if (o.kind == "property" || o.kind == "load" || o.kind == "bc" || o.kind == "initial_condition" || o.kind == "contact_pair") {
    if (q.contains("material") && q["material"].is_number()) {
      const Object* m = a.model().find(q["material"].get<Id>());
      if (m) s["material"] = m->name;
    }
    if (q.contains("target")) s["target"] = q["target"];
    for (const char* k : {"thickness", "value", "magnitude", "dofs", "components", "section"})
      if (q.contains(k)) s[k] = q[k];
  } else if (o.kind == "mesh_part") {
    std::size_t n = 0;
    const Mesh& m = a.mesh();
    for (std::size_t i = 0; i < m.element_count(); ++i)
      if (m.element_at(i).part == o.id) ++n;
    s["elements"] = n;
  } else if (o.kind == "set") {
    for (const char* k : {"ids", "faces", "nodes", "elements"})
      if (q.contains(k) && q[k].is_array()) s["size"] = q[k].size();
  } else if (o.kind == "case") {
    s["steps"] = a.model().children(o.id, "step").size();
    if (q.contains("physics")) s["physics"] = q["physics"];
  } else if (o.kind == "step") {  // 스텝이 참조하는 하중 셋·구속 셋의 항목 수(하중·구속은 셋 안에만 있다)
    s["loads"] = step_entries(a, o, "load").size();
    s["bcs"] = step_entries(a, o, "bc").size();
    Json ls = Json::array(), bs = Json::array();
    for (const Json& r : q.value("load_sets", Json::array()))
      if (r.is_object() && r.contains("set")) ls.push_back(r["set"]);
    s["load_sets"] = ls, s["bc_sets"] = q.value("bc_sets", Json::array());
  } else if (o.kind == "load_set" || o.kind == "bc_set") {
    s["items"] = a.model().children(o.id, o.kind == "load_set" ? "load" : "bc").size();
    if (q.contains("step")) s["step"] = q["step"];  // 스텝 전용 셋
  } else if (o.kind == "part") {
    s["features"] = a.model().children(o.id, "feature").size();
    if (q.contains("rollback")) s["rollback"] = q["rollback"];
  } else if (o.kind == "function") {
    if (q.contains("points")) s["points"] = q["points"].size();
  }
  return s;
}

// 스텝 기준 비활성(WT-30): 대상 요소(또는 그 노드)가 모두 그 스텝에서 제거되어 있으면 inactive.
bool inactive_in_step(const App& a, const Object& o, const TreeOptions& opt) {
  if (!opt.has_step || opt.removed_elements.empty()) return false;
  if (o.kind != "property" && o.kind != "load" && o.kind != "bc" && o.kind != "set") return false;
  const Json* t = nullptr;
  if (o.kind == "set") {
    const std::string st = o.props.value("type", std::string());
    if (st == "element" && o.props.contains("ids")) {
      for (const Json& e : o.props["ids"]) if (!opt.removed_elements.count(e.get<Id>())) return false;
      return !o.props["ids"].empty();
    }
    if (st == "node" && o.props.contains("ids")) {
      for (const Json& n : o.props["ids"]) if (!opt.removed_nodes.count(n.get<Id>())) return false;
      return !o.props["ids"].empty();
    }
    return false;
  }
  if (!o.props.contains("target")) return false;
  t = &o.props["target"];
  try {
    const Json elems = resolve_target(a, *t, "elements");
    if (!elems.empty()) {
      for (const Json& e : elems) if (!opt.removed_elements.count(e.get<Id>())) return false;
      return true;
    }
  } catch (...) {
  }
  try {
    const Json nodes = resolve_target(a, *t, "nodes");
    if (!nodes.empty()) {
      for (const Json& n : nodes) if (!opt.removed_nodes.count(n.get<Id>())) return false;
      return true;
    }
  } catch (...) {
  }
  return false;
}

Json tree_node(const App& a, const Object& o, const TreeOptions& opt) {
  Json j{{"id", o.id}, {"kind", o.kind}, {"name", o.name}, {"suppressed", o.suppressed}};
  if (o.props.contains("type")) j["type"] = o.props["type"];
  if (o.kind == "folder") j["folder_kind"] = o.props.value("kind", std::string());
  const Json issues = diagnose_object(a, o);
  if (!issues.empty()) {
    const bool err = std::any_of(issues.begin(), issues.end(),
                                 [](const Json& i) { return i["severity"] == "error"; });
    j["status"] = err ? "error" : "incomplete";
  }
  if (o.kind == "folder") return j;  // 폴더의 children 은 tree_items 가 채운다
  if (inactive_in_step(a, o, opt)) j["inactive"] = true;
  if (opt.summary) {
    Json s = tree_summary(a, o);
    if (!s.empty()) j["summary"] = s;
  }
  // 피처(WT-05): 적용 상태(ok·error·skipped·suppressed·rolled_back)와 롤백 위치
  if (o.kind == "feature" && a.commands().count("feature.status")) {
    try {
      const Json st = a.commands().at("feature.status").fn(const_cast<App&>(a), Json{{"id", o.parent}});
      for (const Json& f : st["features"])
        if (f["feature"] == o.id) {
          j["state"] = f["state"];
          break;
        }
    } catch (...) {
    }
  }
  if (o.kind == "part" && o.props.contains("rollback")) j["rollback"] = o.props["rollback"];
  Json kids = tree_items(a, o.id, "", opt);
  if (o.kind == "part") {  // 어셈블리 계층(GEO-03, WT-06): assembly 가 이 파트인 파트들
    std::vector<Json> sub;
    for (const Object* c : a.model().by_kind("part"))
      if (c->props.contains("assembly") && c->props["assembly"].is_number() && c->props["assembly"].get<Id>() == o.id) sub.push_back(tree_node(a, *c, opt));
    sort_nodes(sub, opt);
    for (Json& n : sub) kids.push_back(std::move(n));
  }
  if (!kids.empty()) j["children"] = kids;
  return j;
}

// --- 프로그램 설정(CAS-41): 모델이 아니라 사용자 환경에 속한다. 환경 변수 NASA95_SETTINGS 가 가리키는 파일,
// 없으면 %APPDATA%\open-fep\settings.json(Windows) 또는 ~/.config/open-fep/settings.json 에 둔다.
namespace fs = std::filesystem;
fs::path fs_path(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }
std::string utf8(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}
std::string env(const char* name) {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
  const char* v = std::getenv(name);
#ifdef _MSC_VER
#pragma warning(pop)
#endif
  return v ? v : "";
}
fs::path settings_file() {
  const std::string given = env("NASA95_SETTINGS");
  if (!given.empty()) return fs_path(given);
  std::string base = env("APPDATA");
  if (base.empty()) base = env("XDG_CONFIG_HOME");
  if (base.empty()) base = env("HOME") + "/.config";
  return fs_path(base) / "NASA-95" / "settings.json";
}
struct Settings {
  Json data = Json::object();
  bool loaded = false;
  std::string path;
};
Settings& settings(App& a) {
  std::any& slot = a.runtime("settings");
  if (!slot.has_value()) slot = std::make_shared<Settings>();
  Settings& s = *std::any_cast<std::shared_ptr<Settings>&>(slot);
  if (!s.loaded) {
    s.loaded = true;
    s.path = utf8(settings_file());
    std::ifstream f(settings_file(), std::ios::binary);
    if (f) {
      try {
        s.data = Json::parse(f);
        if (!s.data.is_object()) s.data = Json::object();
      } catch (const Json::exception&) {
        s.data = Json::object();  // 깨진 파일은 빈 설정으로 시작한다(저장하면 덮어쓴다)
      }
    }
  }
  return s;
}
void save_settings(const Settings& s) {
  const fs::path p = fs_path(s.path);
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (!f) throw Error("io_error", "설정 파일을 쓸 수 없습니다: " + s.path, {{"path", s.path}});
  f << s.data.dump(2) << "\n";
}

}  // namespace

Json program_setting(App& a, const std::string& key) {
  const Json& d = settings(a).data;
  return d.contains(key) ? d[key] : Json();
}

void register_system_commands(App& app) {
  // --- 프로그램
  app.register_command(make("app.version", 'Q', "프로그램·API 버전을 조회한다", {}, "API-25", [](App&, const Json&) {
    // geometry: 형상 커널이 들어 있는지, mesher: 자동 메셔의 이름("" = 없음)
    return Json{{"version", App::version()}, {"api_version", App::api_version()}, {"geometry", geometry_available()}, {"mesher", mesher_name()}};
  }));
  app.register_command(make("app.settings_get", 'Q', "프로그램 설정을 조회한다(키를 주면 그 값, 없으면 전부와 파일 경로). 모델이 아니라 사용자 환경이다",
                            {F("key", "string", "설정 키(예: solver_executable, threads, work_directory)").ex("solver_executable")}, "CAS-41",
                            [](App& a, const Json& p) {
                              Settings& s = settings(a);
                              if (p.contains("key") && !p["key"].is_null()) {
                                const std::string key = p["key"].get<std::string>();
                                return Json{{"key", key}, {"value", s.data.contains(key) ? s.data[key] : Json()}};
                              }
                              return Json{{"path", s.path}, {"values", s.data}};
                            }));
  app.register_command(make("app.settings_set", 'S',
                            "프로그램 설정을 바꾸고 파일에 저장한다(값이 null 이면 지운다). solver_executable·threads·work_directory 는 "
                            "케이스에 지정이 없을 때의 기본값이 된다",
                            {F("key", "string", "설정 키").call_req().ex("solver_executable"), F("value", "any", "값(null = 지움)").ex("ccx.exe")},
                            "CAS-41", [](App& a, const Json& p) {
                              Settings& s = settings(a);
                              const std::string key = p["key"].get<std::string>();
                              if (key.empty()) throw Error("invalid_param", "설정 키가 비었습니다", {{"param", "key"}});
                              if (!p.contains("value") || p["value"].is_null()) s.data.erase(key);
                              else s.data[key] = p["value"];
                              if (key == "threads" && s.data.contains(key) && (!s.data[key].is_number_integer() || s.data[key].get<int>() < 1)) {
                                s.data.erase(key);
                                throw Error("invalid_param", "threads 는 1 이상의 정수여야 합니다", {{"param", "value"}});
                              }
                              save_settings(s);
                              return Json{{"key", key}, {"value", s.data.contains(key) ? s.data[key] : Json()}, {"path", s.path}};
                            }));
  app.register_command(make("app.commands", 'Q', "명령 등록부(이름·매개변수·설명·되돌릴 수 있는지 여부)를 조회한다",
                            {}, "API-01, CMN-18", [](App& a, const Json&) {
                              Json arr = Json::array();
                              for (const auto& [name, spec] : a.commands()) arr.push_back(spec.to_json());
                              return arr;
                            }));
  app.register_command(make("app.kinds", 'Q', "객체 종류 정의(속성·하위 종류·상위 객체)를 조회한다", {}, "API-01",
                            [](App& a, const Json&) {
                              Json arr = Json::array();
                              for (const KindSpec& k : a.schema().all()) arr.push_back(k.to_json());
                              return arr;
                            }));
  app.register_command(make("app.execute", 'S', "이름과 매개변수로 임의의 명령을 실행한다(실행 전 입력 검증)",
                            {F("command", "string", "명령 이름").call_req(), F("params", "object", "매개변수")},
                            "API-02, API-27", [](App& a, const Json& p) {
                              return a.execute(p["command"].get<std::string>(), p.value("params", Json::object()));
                            }));

  {
    // 미리보기(RND-39): 변경 명령을 묶음 안에서 실행해 결과와 변경 요약을 얻고 전부 되돌린다. 모델·이력은 그대로다.
    CommandSpec c = make("app.command_preview", 'S',
                         "변경 명령을 실행하지 않은 것처럼 미리본다: 묶음 안에서 실행해 결과와 생기는 변경(created·updated·deleted·mesh)을 "
                         "돌려주고 전부 되돌린다. 변경 명령(C)만 된다",
                         {F("command", "string", "명령 이름").call_req().ex("part.create_box"), F("params", "object", "매개변수").ex(Json::object())},
                         "RND-39", [](App& a, const Json& p) {
                           const std::string name = p["command"].get<std::string>();
                           auto it = a.commands().find(name);
                           if (it == a.commands().end()) throw Error("unknown_command", "등록되지 않은 명령: " + name, {{"param", "command"}});
                           if (it->second.kind != 'C')
                             throw Error("not_available", "미리보기는 변경 명령(C)만 된다: " + name, {{"param", "command"}, {"kind", std::string(1, it->second.kind)}});
                           if (a.in_transaction()) throw Error("invalid_state", "묶음 실행 중에는 미리볼 수 없습니다");
                           a.transaction_begin("preview " + name);
                           Json result, changes;
                           try {
                             result = a.execute(name, p.value("params", Json::object()));
                             changes = a.transaction_changes();
                           } catch (...) {
                             if (a.in_transaction()) a.transaction_rollback();
                             throw;
                           }
                           a.transaction_rollback();
                           return Json{{"command", name}, {"result", result}, {"changes", changes}};
                         });
    app.register_command(std::move(c));
  }

  // --- 통지 구독(명령 계층, API-06·API-22·API-31): 폴링 방식. Python 은 App.subscribe, REST 는 SSE 를 쓴다.
  struct Subscriptions {
    struct Sub {
      int token = 0;
      std::vector<Json> buffer;
      std::size_t dropped = 0;
    };
    std::map<int, Sub> subs;
    int next = 1;
  };
  auto subs_of = [](App& a) -> Subscriptions& {
    std::any& s = a.runtime("event.subscriptions");
    if (!s.has_value()) s = Subscriptions{};
    return *std::any_cast<Subscriptions>(&s);
  };
  app.register_command(make("event.subscribe", 'S',
                            "통지 구독을 만든다(id 없이) 또는 구독에 쌓인 통지를 꺼낸다(id 를 주면 쌓인 것을 돌려주고 비운다). "
                            "구독마다 최대 limit 개를 쌓고 넘치면 오래된 것부터 버린다(dropped)",
                            {F("id", "integer", "구독 번호(없으면 새로 만든다)").ge(1), F("limit", "integer", "쌓아 둘 최대 개수(기본 1000)").ge(1)},
                            "API-06, API-22, API-31", [subs_of](App& a, const Json& p) {
                              Subscriptions& s = subs_of(a);
                              if (has(p, "id")) {
                                auto it = s.subs.find(p["id"].get<int>());
                                if (it == s.subs.end()) throw Error("not_found", "구독이 없습니다", {{"param", "id"}});
                                Json events = Json::array();
                                for (Json& e : it->second.buffer) events.push_back(std::move(e));
                                it->second.buffer.clear();
                                const std::size_t dropped = it->second.dropped;
                                it->second.dropped = 0;
                                return Json{{"id", it->first}, {"events", events}, {"dropped", dropped}};
                              }
                              const int id = s.next++;
                              const std::size_t limit = p.value("limit", 1000);
                              App* app_ptr = &a;
                              const int token = a.subscribe([app_ptr, subs_of, id, limit](const Json& ev) {
                                Subscriptions& s = subs_of(*app_ptr);
                                auto it = s.subs.find(id);
                                if (it == s.subs.end()) return;
                                if (it->second.buffer.size() >= limit) it->second.buffer.erase(it->second.buffer.begin()), ++it->second.dropped;
                                it->second.buffer.push_back(ev);
                              });
                              s.subs[id] = Subscriptions::Sub{token, {}, 0};
                              return Json{{"id", id}, {"events", Json::array()}, {"dropped", 0}};
                            }));
  app.register_command(make("event.unsubscribe", 'S', "통지 구독을 끝낸다", {F("id", "integer", "구독 번호").call_req().ge(1)},
                            "API-06, API-22, API-31", [subs_of](App& a, const Json& p) {
                              Subscriptions& s = subs_of(a);
                              auto it = s.subs.find(p["id"].get<int>());
                              if (it == s.subs.end()) throw Error("not_found", "구독이 없습니다", {{"param", "id"}});
                              a.unsubscribe(it->second.token);
                              s.subs.erase(it);
                              return Json{{"id", p["id"]}, {"remaining", s.subs.size()}};
                            }));

  // --- 확장 데이터(API-23): 모델의 설정 객체에 확장 이름별로 두어 프로젝트 파일에 함께 저장되고 Undo 에 들어간다
  app.register_command(make("ext.storage_get", 'Q', "확장이 프로젝트에 저장한 데이터를 읽는다(key 가 없으면 그 확장의 전부)",
                            {F("extension", "string", "확장 이름").call_req().ex("hello_ext"), F("key", "string", "키").ex("count")},
                            "API-23", [](App& a, const Json& p) {
                              const Object* s = a.find_settings();
                              const Json all = s ? s->props.value("extensions", Json::object()) : Json::object();
                              const Json data = all.value(p["extension"].get<std::string>(), Json::object());
                              if (has(p, "key")) {
                                const std::string key = p["key"].get<std::string>();
                                return Json{{"extension", p["extension"]}, {"key", key}, {"value", data.contains(key) ? data[key] : Json()}};
                              }
                              return Json{{"extension", p["extension"]}, {"values", data}};
                            }));
  app.register_command(make("ext.storage_set", 'C', "확장 데이터를 프로젝트에 저장한다(값이 null 이면 지운다)",
                            {F("extension", "string", "확장 이름").call_req().ex("hello_ext"), F("key", "string", "키").call_req().ex("count"),
                             F("value", "any", "값(null = 지움)").ex(1)},
                            "API-23", [](App& a, const Json& p) {
                              const std::string ext = p["extension"].get<std::string>(), key = p["key"].get<std::string>();
                              if (ext.empty() || key.empty()) throw Error("invalid_param", "확장 이름과 키는 비울 수 없습니다", {{"param", ext.empty() ? "extension" : "key"}});
                              Object s = a.settings();
                              Json all = s.props.value("extensions", Json::object());
                              Json data = all.value(ext, Json::object());
                              if (!p.contains("value") || p["value"].is_null()) data.erase(key);
                              else data[key] = p["value"];
                              if (data.empty()) all.erase(ext);
                              else all[ext] = data;
                              if (all.empty()) s.props.erase("extensions");
                              else s.props["extensions"] = all;
                              a.model().replace(s);
                              return Json{{"extension", ext}, {"key", key}, {"value", data.contains(key) ? data[key] : Json()}};
                            }));

  // --- Undo/Redo
  {
    CommandSpec c = make("app.undo", 'S', "한 단계 되돌린다", {}, "CMN-08", [](App& a, const Json&) { return a.undo(); });
    c.journaled = true;
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = make("app.redo", 'S', "한 단계 다시 실행한다", {}, "CMN-08", [](App& a, const Json&) { return a.redo(); });
    c.journaled = true;
    app.register_command(std::move(c));
  }
  app.register_command(make("app.history", 'Q', "Undo 이력 목록을 조회한다", {}, "CMN-16",
                            [](App& a, const Json&) { return a.history(); }));
  {
    CommandSpec c = make("app.history_goto", 'S', "이력의 지정 시점으로 이동한다",
                         {F("serial", "integer", "시점(0 = 맨 처음)").call_req().ge(0)}, "CMN-16",
                         [](App& a, const Json& p) {
                           a.history_goto(p["serial"].get<std::uint64_t>());
                           return a.history();
                         });
    c.journaled = true;
    app.register_command(std::move(c));
  }
  app.register_command(make("app.history_limit", 'S', "이력 한도를 설정한다",
                            {F("limit", "integer", "보관할 단계 수").call_req().ge(0)}, "CMN-17",
                            [](App& a, const Json& p) {
                              check_value(F("limit", "integer", "").ge(0), p["limit"], nullptr);
                              a.set_history_limit(p["limit"].get<std::size_t>());
                              return a.history();
                            }));

  // --- 묶음 실행
  {
    CommandSpec c = make("app.transaction_begin", 'S', "여러 명령을 한 Undo 단계로 묶기 시작한다",
                         {F("name", "string", "이력에 표시할 이름")}, "API-04, WT-17, CMN-15, CMN-19",
                         [](App& a, const Json& p) {
                           a.transaction_begin(p.value("name", std::string()));
                           return Json::object();
                         });
    c.journaled = true;
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = make("app.transaction_commit", 'S', "묶음을 확정한다(Undo 1단계가 된다)", {},
                         "API-04, WT-17, CMN-15, CMN-19", [](App& a, const Json&) {
                           a.transaction_commit();
                           return Json::object();
                         });
    c.journaled = true;
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = make("app.transaction_rollback", 'S', "묶음을 취소하고 묶음 전 상태로 되돌린다", {},
                         "API-04, WT-17, CMN-15, CMN-19", [](App& a, const Json&) {
                           a.transaction_rollback();
                           return Json::object();
                         });
    c.journaled = true;
    app.register_command(std::move(c));
  }

  // --- 프로젝트
  app.register_command(make("project.new", 'S', "새 프로젝트를 만든다", {}, "CMN-09", [](App& a, const Json&) {
    a.new_project();
    return a.info();
  }));
  app.register_command(make("project.close", 'S', "프로젝트를 닫는다", {}, "CMN-09", [](App& a, const Json&) {
    a.new_project();
    return Json::object();
  }));
  app.register_command(make("project.open", 'J', "프로젝트 파일을 연다", {F("path", "string", "파일 경로").call_req()},
                            "CMN-09", [](App& a, const Json& p) {
                              a.open(p["path"].get<std::string>());
                              return a.info();
                            }));
  app.register_command(make("project.save", 'J', "프로젝트를 저장한다", {F("path", "string", "파일 경로(없으면 현재 경로)")},
                            "CMN-09", [](App& a, const Json& p) {
                              std::string path = p.value("path", std::string());
                              if (path.empty()) path = a.info()["path"].get<std::string>();
                              if (path.empty())
                                throw Error("missing_param", "저장할 경로가 없습니다", {{"param", "path"}});
                              a.save(path);
                              return a.info();
                            }));
  app.register_command(make("project.save_as", 'J', "프로젝트를 다른 이름으로 저장한다",
                            {F("path", "string", "파일 경로").call_req()}, "CMN-09", [](App& a, const Json& p) {
                              a.save(p["path"].get<std::string>());
                              return a.info();
                            }));
  app.register_command(make("project.info", 'Q', "경로·저장 여부·객체 수 요약을 조회한다", {}, "CMN-20",
                            [](App& a, const Json&) { return a.info(); }));
  app.register_command(make("project.digest", 'Q', "모델 전체와 영역별 상태 요약값을 조회한다(뷰 상태 제외)", {},
                            "API-39", [](App& a, const Json&) { return a.digest(); }));
  app.register_command(make("project.tree", 'Q', "워크 트리 전체 또는 일부 가지를 상태와 함께 조회한다",
                            {F("kind", "string", "이 종류의 가지만"),
                             F("sort", "string", "정렬 기준(없으면 생성 순서. 스텝·피처는 정렬하지 않는다)").one_of({"name", "id"}),
                             F("descending", "bool", "내림차순"),
                             F("offset", "integer", "가지의 최상위 항목 중 이 번째부터(0 부터)").ge(0),
                             F("limit", "integer", "가지의 최상위 항목을 이 개수까지만").ge(0),
                             F("step", "ref", "이 스텝 기준으로 제거된 요소에만 걸린 항목을 inactive 로 표시한다").ref("step"),
                             F("summary", "bool", "항목 요약(재료의 구성 모델, 메시 파트의 요소 수 등)을 넣는다(기본 켬)")},
                            "WT-01, WT-02, WT-05, WT-06, WT-08, WT-30, WT-37, WT-38, CMN-01", [](App& a, const Json& p) {
                              const std::string only = p.value("kind", std::string());
                              TreeOptions opt;
                              opt.sort = p.value("sort", std::string());
                              if (!opt.sort.empty() && opt.sort != "name" && opt.sort != "id")
                                throw Error("out_of_range", "정렬 기준은 name 또는 id 입니다", {{"param", "sort"}});
                              opt.descending = p.value("descending", false);
                              opt.summary = p.value("summary", true);
                              if (p.contains("step") && !p["step"].is_null()) {
                                const Object& st = a.model().get(p["step"].get<Id>());
                                if (st.kind != "step") throw Error("invalid_reference", "스텝이 아닙니다", {{"param", "step"}});
                                opt.has_step = true;
                                // 같은 케이스의 이 스텝까지 누적된 제거/추가
                                for (const Object* sib : a.model().children(st.parent, "step")) {
                                  if (sib->suppressed) continue;
                                  for (const Object* ch : a.model().children(sib->id, "step_change")) {
                                    if (ch->suppressed || ch->props.value("type", std::string()) != "model_change_element") continue;
                                    const bool remove = ch->props.value("action", std::string()) == "remove";
                                    try {
                                      const Json ids = resolve_target(a, ch->props["target"], "elements");
                                      for (const Json& e : ids) {
                                        if (remove) opt.removed_elements.insert(e.get<Id>());
                                        else opt.removed_elements.erase(e.get<Id>());
                                      }
                                    } catch (...) {
                                    }
                                  }
                                  if (sib->id == st.id) break;
                                }
                                // 제거된 요소에만 속한 노드
                                const Mesh& m = a.mesh();
                                std::set<Id> alive;
                                for (std::size_t i = 0; i < m.element_count(); ++i) {
                                  const Id eid = m.element_ids()[i];
                                  const Id* n = m.nodes_at(i);
                                  const std::size_t cnt = m.node_count_at(i);
                                  if (opt.removed_elements.count(eid)) {
                                    for (std::size_t k = 0; k < cnt; ++k) opt.removed_nodes.insert(n[k]);
                                  } else {
                                    for (std::size_t k = 0; k < cnt; ++k) alive.insert(n[k]);
                                  }
                                }
                                for (Id n : alive) opt.removed_nodes.erase(n);
                              }
                              const std::size_t offset = p.contains("offset") && !p["offset"].is_null() ? p["offset"].get<std::size_t>() : 0;
                              const bool limited = p.contains("limit") && !p["limit"].is_null();
                              const std::size_t limit = limited ? p["limit"].get<std::size_t>() : 0;
                              Json branches = Json::array();
                              for (const KindSpec& k : a.schema().all()) {
                                if (!k.parents.empty()) continue;  // 하위 종류는 상위 객체 아래에 나온다(폴더도 가지 안에 나온다)
                                if (!only.empty() && k.kind != only) continue;
                                Json items = tree_items(a, 0, k.kind, opt);
                                const std::size_t count = a.model().children(0, k.kind).size();  // 폴더는 세지 않는다
                                Json branch{{"kind", k.kind}, {"label", k.label}, {"count", count}};
                                if (offset || limited) {
                                  Json page = Json::array();
                                  for (std::size_t i = offset; i < items.size() && (!limited || page.size() < limit); ++i) page.push_back(items[i]);
                                  branch["items"] = page, branch["offset"] = offset, branch["total"] = items.size();
                                } else {
                                  branch["items"] = items;
                                }
                                branches.push_back(branch);
                              }
                              return branches;
                            }));
  app.register_command(make("project.search", 'Q', "이름·ID·종류·상태로 객체를 찾는다",
                            {F("text", "string", "이름에 들어 있는 글자"), F("kind", "string", "객체 종류"),
                             F("id", "integer", "객체 ID"), F("suppressed", "bool", "억제 여부")},
                            "WT-34, WT-35", [](App& a, const Json& p) {
                              Json arr = Json::array();
                              for (const Object* o : a.model().all()) {
                                if (p.contains("text") && o->name.find(p["text"].get<std::string>()) == std::string::npos)
                                  continue;
                                if (p.contains("kind") && o->kind != p["kind"].get<std::string>()) continue;
                                if (p.contains("id") && o->id != p["id"].get<Id>()) continue;
                                if (p.contains("suppressed") && o->suppressed != p["suppressed"].get<bool>()) continue;
                                arr.push_back(Json{{"id", o->id}, {"kind", o->kind}, {"name", o->name},
                                                   {"parent", o->parent}, {"suppressed", o->suppressed}});
                              }
                              return arr;
                            }));
  app.register_command(make("project.validate", 'Q', "모델 전체의 오류·경고 목록을 조회한다", {}, "WT-27",
                            [](App& a, const Json&) {
                              Json all = Json::array();
                              for (const Object* o : a.model().all())
                                for (const Json& i : diagnose_object(a, *o)) all.push_back(i);
                              return all;
                            }));

  // --- 저널
  app.register_command(make("journal.start", 'S', "명령 기록을 시작한다", {F("path", "string", "기록 파일").call_req()},
                            "API-08", [](App& a, const Json& p) {
                              a.journal_start(p["path"].get<std::string>());
                              return Json::object();
                            }));
  app.register_command(make("journal.stop", 'S', "명령 기록을 중지한다", {}, "API-08", [](App& a, const Json&) {
    a.journal_stop();
    return Json::object();
  }));
  app.register_command(make("journal.replay", 'J', "기록 파일을 다시 실행한다", {F("path", "string", "기록 파일").call_req()},
                            "API-08",
                            [](App& a, const Json& p) { return a.journal_replay(p["path"].get<std::string>()); }));
}

}  // namespace nasa95
