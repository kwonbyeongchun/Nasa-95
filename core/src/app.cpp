#include "nasa95/app.hpp"
#include "nasa95/geometry.hpp"
#include "nasa95/mesher.hpp"
#include "nasa95/results.hpp"

#include <algorithm>
#include <filesystem>
#include <set>

#include "nasa95/error.hpp"

namespace nasa95 {

namespace {
// UTF-8 경로 → 파일 시스템 경로(Windows 에서 한글 경로가 깨지지 않게)
std::filesystem::path fs_path(const std::string& utf8) { return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())); }
}  // namespace

Json CommandSpec::to_json() const {
  Json ps = Json::array();
  for (const auto& p : params) ps.push_back(p.to_json());
  return Json{{"name", name},         {"kind", std::string(1, kind)}, {"desc", desc},
              {"params", ps},         {"undoable", undoable},         {"target", target},
              {"features", features}};
}

App::App() {
  register_builtin_kinds(schema_);
  register_object_commands(*this);
  register_material_commands(*this);
  register_section_commands(*this);
  register_system_commands(*this);
  register_model_commands(*this);
  register_mesh_commands(*this);
  register_mesh_op_commands(*this);
  register_solver_commands(*this);
  register_run_commands(*this);
  register_result_commands(*this);
  register_geometry_commands(*this);
  register_meshgen_commands(*this);
}

void App::register_command(CommandSpec spec) {
  if (commands_.count(spec.name)) throw Error("name_conflict", "명령이 이미 등록되어 있습니다: " + spec.name);
  commands_.emplace(spec.name, std::move(spec));
}

void App::unregister_command(const std::string& name) {
  auto it = commands_.find(name);
  if (it == commands_.end()) throw Error("unknown_command", "등록되지 않은 명령: " + name, {{"command", name}});
  if (!it->second.external) throw Error("invalid_state", "내장 명령은 등록 해제할 수 없습니다: " + name);
  commands_.erase(it);
}

const Object* App::find_settings() const {
  const auto v = model_.by_kind("settings");
  return v.empty() ? nullptr : v.front();
}

Object App::settings() {
  if (const Object* o = find_settings()) return *o;
  Object o;
  o.kind = "settings", o.name = "settings";
  const Id id = model_.create(o);
  return model_.get(id);
}

Json App::digest() const {
  Json d = model_.digest();
  if (!mesh_.empty()) {
    const std::string m = mesh_.digest();
    d["areas"]["mesh"] = m;
    d["total"] = d["total"].get<std::string>() + m;
  }
  return d;
}

namespace {

// 호출 수준의 입력 검증: 알 수 없는 매개변수, 필수 누락, 타입.
// 값의 범위·참조 대상 검사는 모델이 필요하므로 각 명령 처리기에서 한다.
// 배열로 따로 넘긴 매개변수(arrays)는 JSON 에 없어도 준 것으로 본다.
void validate_params(const CommandSpec& spec, const Json& params, const Arrays* arrays) {
  if (!params.is_object())
    throw Error("invalid_param_type", "매개변수는 객체(이름-값)여야 합니다");
  for (auto it = params.begin(); it != params.end(); ++it) {
    auto ps = std::find_if(spec.params.begin(), spec.params.end(),
                           [&](const FieldSpec& f) { return f.name == it.key(); });
    if (ps == spec.params.end())
      throw Error("unknown_param", "알 수 없는 매개변수: " + it.key(), {{"param", it.key()}});
    if (it.value().is_null()) continue;  // null = 값 지우기(처리기가 허용 여부를 정한다)
    if (!type_matches(ps->type, it.value()))
      throw Error("invalid_param_type",
                  "'" + ps->name + "' 의 타입이 맞지 않습니다(기대: " + ps->type + ")",
                  {{"param", ps->name}, {"expected", ps->type}});
    // 타입뿐 아니라 선택지·범위·목록 항목(object_list 의 항목 속성)까지 스키마대로 검사한다(참조 존재 여부는 처리기가 본다).
    // 그래야 잘못된 입력이 처리기 안의 내부 오류가 아니라 매개변수 이름이 붙은 구조화된 오류로 돌아온다
    // 객체의 update 명령은 하위 종류별 속성 정의를 합친 것이라(예: 대상 종류의 선택지) 처리기가 그 객체의 종류에 맞춰 검사한다
    const bool merged_spec = !spec.target.empty() && spec.name.size() > 7 && spec.name.compare(spec.name.size() - 7, 7, ".update") == 0;
    if (!merged_spec) check_value(*ps, it.value(), nullptr);
  }
  auto in_arrays = [&](const std::string& n) {
    return arrays && (arrays->doubles.count(n) || arrays->ints.count(n));
  };
  if (arrays) {
    auto known = [&](const std::string& n) {
      return std::any_of(spec.params.begin(), spec.params.end(), [&](const FieldSpec& f) { return f.name == n; });
    };
    for (const auto& [n, v] : arrays->doubles)
      if (!known(n)) throw Error("unknown_param", "알 수 없는 매개변수: " + n, {{"param", n}});
    for (const auto& [n, v] : arrays->ints)
      if (!known(n)) throw Error("unknown_param", "알 수 없는 매개변수: " + n, {{"param", n}});
  }
  for (const FieldSpec& f : spec.params)
    if (f.must && !in_arrays(f.name) && (!params.contains(f.name) || params[f.name].is_null()))
      throw Error("missing_param", "필수 매개변수가 없습니다: " + f.name, {{"param", f.name}});
}

// 저널·매크로용: 배열로 넘긴 매개변수를 JSON 목록으로 풀어 넣는다.
Json with_arrays(const Json& params, const Arrays* arrays) {
  if (!arrays) return params;
  Json p = params;
  auto put = [&](const std::string& n, const auto& v) {
    auto c = arrays->cols.find(n);
    const std::size_t cols = c == arrays->cols.end() ? 0 : static_cast<std::size_t>(c->second);
    if (cols == 0) {
      p[n] = v;
      return;
    }
    Json rows = Json::array();
    for (std::size_t i = 0; i + cols <= v.size(); i += cols) {
      Json row = Json::array();
      for (std::size_t k = 0; k < cols; ++k) row.push_back(v[i + k]);
      rows.push_back(std::move(row));
    }
    p[n] = std::move(rows);
  };
  for (const auto& [n, v] : arrays->doubles) put(n, v);
  for (const auto& [n, v] : arrays->ints) put(n, v);
  return p;
}

}  // namespace

Json App::execute(const std::string& name, const Json& params_in, const Arrays* arrays) {
  auto it = commands_.find(name);
  if (it == commands_.end()) throw Error("unknown_command", "등록되지 않은 명령: " + name, {{"command", name}});
  const CommandSpec& spec = it->second;
  const Json params = params_in.is_null() ? Json::object() : params_in;
  const Arrays* saved = arrays_;
  arrays_ = arrays;
  try {
    validate_params(spec, params, arrays);
    Json result;
    if (spec.composite) {
      // 안에서 실행하는 명령들을 한 묶음으로 만든다. 이미 묶음이 열려 있으면 그 묶음에 들어간다.
      const bool own = !group_;
      if (own) group_ = Txn{0, spec.name, {}, {}};
      try {
        result = spec.fn(*this, params);
      } catch (...) {
        if (own && group_) transaction_rollback();
        throw;
      }
      if (own && group_) transaction_commit();
      arrays_ = saved;
      return result;  // 안의 명령들이 각자 저널·매크로에 남는다
    }
    if (spec.undoable)
      result = run_recorded(spec, params);
    else if (spec.kind == 'Q' || spec.kind == 'V')
      result = model_.recording() ? spec.fn(*this, params) : run_readonly(spec, params);  // 명령 안에서 부른 조회는 바깥 기록이 지킨다
    else
      result = spec.fn(*this, params);
    if (spec.undoable || spec.journaled) journal_write(name, with_arrays(params, arrays));
    arrays_ = saved;
    return result;
  } catch (Error& e) {
    arrays_ = saved;
    e.details()["command"] = name;
    throw;
  } catch (...) {
    arrays_ = saved;
    throw;
  }
}

Json App::invoke(const std::string& name, const Json& params) {
  auto it = commands_.find(name);
  if (it == commands_.end()) throw Error("unknown_command", "등록되지 않은 명령: " + name, {{"command", name}});
  if (!model_.recording()) throw Error("internal", "invoke 는 실행 중인 명령 안에서만 쓴다: " + name);
  validate_params(it->second, params, nullptr);
  const Arrays* saved = arrays_;
  arrays_ = nullptr;  // 바깥 명령이 받은 배열은 안쪽 명령의 것이 아니다
  try {
    Json r = it->second.fn(*this, params);
    arrays_ = saved;
    return r;
  } catch (...) {
    arrays_ = saved;
    throw;
  }
}

void App::revert(const Txn& t) {
  mesh_.apply_inverse(t.mesh);
  model_.apply_inverse(t.changes);
}

Id App::default_mesh_part() {
  for (const Object* o : model_.children(0, "mesh_part"))
    if (o->name == "MESH" && !(o->props.contains("geometry") && o->props["geometry"].is_number())) return o->id;
  return invoke("mesh_part.create", Json{{"name", "MESH"}})["id"].get<Id>();
}

void App::ensure_mesh_parts() {
  std::vector<Element> orphans;
  for (std::size_t i = 0; i < mesh_.element_count(); ++i) {
    const Id pid = mesh_.part_at(i);
    const Object* o = pid ? model_.find(pid) : nullptr;
    if (!o || o->kind != "mesh_part") orphans.push_back(mesh_.element(mesh_.element_ids()[i]));
  }
  if (orphans.empty()) return;
  const Id part = default_mesh_part();
  for (Element& e : orphans) e.part = part;
  mesh_.replace_elements(orphans);
}

void App::reapply(const Txn& t) {
  model_.apply_forward(t.changes);
  mesh_.apply_forward(t.mesh);
}

Json App::run_recorded(const CommandSpec& spec, const Json& params) {
  model_.begin_record();
  mesh_.begin_record();
  Json result;
  try {
    result = spec.fn(*this, params);
  } catch (...) {
    // 실패는 전부 아니면 전무(CMN-19): 이 명령의 변경을 되돌리고, 묶음 안이면 묶음 전체를 되돌린다.
    Txn failed{0, spec.name, model_.end_record(), mesh_.end_record()};
    revert(failed);
    if (group_) {
      const Txn g = std::move(*group_);
      group_.reset();
      revert(g);
      emit(g.changes, g.mesh, true, "rollback", spec.name);
    }
    throw;
  }
  try {
    ensure_mesh_parts();  // 같은 기록 안에서 — Undo 하면 같이 되돌아간다
  } catch (...) {
    revert(Txn{0, spec.name, model_.end_record(), mesh_.end_record()});
    throw;
  }
  Txn t{0, spec.name, model_.end_record(), mesh_.end_record()};
  // 이력에 먼저 넣고 알린다: 통지 처리가 실패해도 변경과 이력·수정 상태가 어긋나지 않는다(CMN-19, CMN-20)
  const ChangeSet changes = t.changes;
  const MeshLog mesh_log = t.mesh;
  if (!t.empty()) {
    if (group_) {
      group_->changes.insert(group_->changes.end(), t.changes.begin(), t.changes.end());
      group_->mesh.insert(group_->mesh.end(), std::make_move_iterator(t.mesh.begin()), std::make_move_iterator(t.mesh.end()));
    } else {
      push_txn(std::move(t));
    }
  }
  emit(changes, mesh_log, false, "command", spec.name);
  return result;
}

Json App::run_readonly(const CommandSpec& spec, const Json& params) {
  model_.begin_record();
  mesh_.begin_record();
  Json result;
  try {
    result = spec.fn(*this, params);
  } catch (...) {
    revert(Txn{0, spec.name, model_.end_record(), mesh_.end_record()});
    throw;
  }
  const Txn t{0, spec.name, model_.end_record(), mesh_.end_record()};
  if (!t.empty()) {
    revert(t);
    throw Error("internal", "조회·뷰 명령이 모델을 바꾸려 했습니다: " + spec.name);
  }
  return result;
}

void App::push_txn(Txn t) {
  t.serial = next_serial_++;
  undo_.push_back(std::move(t));
  redo_.clear();
  while (undo_.size() > history_limit_) {
    base_token_ = undo_.front().serial;
    undo_.erase(undo_.begin());
  }
}

std::uint64_t App::state_token() const { return undo_.empty() ? base_token_ : undo_.back().serial; }

void App::reset_history() {
  undo_.clear();
  redo_.clear();
  group_.reset();
  base_token_ = next_serial_++;
  saved_token_ = base_token_;
}

Json App::undo() {
  if (group_) throw Error("invalid_state", "묶음 실행 중에는 되돌릴 수 없습니다");
  if (undo_.empty()) return Json{{"done", false}};
  Txn t = std::move(undo_.back());
  undo_.pop_back();
  revert(t);
  emit(t.changes, t.mesh, true, "undo", t.name);
  Json r{{"done", true}, {"name", t.name}, {"serial", t.serial}};
  redo_.push_back(std::move(t));
  return r;
}

Json App::redo() {
  if (group_) throw Error("invalid_state", "묶음 실행 중에는 다시 실행할 수 없습니다");
  if (redo_.empty()) return Json{{"done", false}};
  Txn t = std::move(redo_.back());
  redo_.pop_back();
  reapply(t);
  emit(t.changes, t.mesh, false, "redo", t.name);
  Json r{{"done", true}, {"name", t.name}, {"serial", t.serial}};
  undo_.push_back(std::move(t));
  return r;
}

Json App::history() const {
  Json u = Json::array(), r = Json::array();
  for (const Txn& t : undo_) u.push_back(Json{{"serial", t.serial}, {"name", t.name}});
  for (auto it = redo_.rbegin(); it != redo_.rend(); ++it)
    r.push_back(Json{{"serial", it->serial}, {"name", it->name}});
  return Json{{"undo", u}, {"redo", r}, {"limit", history_limit_}, {"in_transaction", in_transaction()}};
}

void App::history_goto(std::uint64_t serial) {
  auto in = [&](const std::vector<Txn>& v) {
    return std::any_of(v.begin(), v.end(), [&](const Txn& t) { return t.serial == serial; });
  };
  if (serial == 0) {
    while (!undo_.empty()) undo();
  } else if (in(undo_)) {
    while (!undo_.empty() && undo_.back().serial != serial) undo();
  } else if (in(redo_)) {
    while (undo_.empty() || undo_.back().serial != serial) redo();
  } else {
    throw Error("not_found", "이력에 없는 시점입니다: " + std::to_string(serial), {{"serial", serial}});
  }
}

void App::set_history_limit(std::size_t limit) {
  history_limit_ = limit;
  while (undo_.size() > history_limit_) {
    base_token_ = undo_.front().serial;
    undo_.erase(undo_.begin());
  }
}

void App::transaction_begin(const std::string& name) {
  if (group_) throw Error("invalid_state", "묶음 실행이 이미 열려 있습니다(중첩 불가)");
  group_ = Txn{0, name.empty() ? std::string("묶음") : name, {}, {}};
}

void App::transaction_commit() {
  if (!group_) throw Error("invalid_state", "열려 있는 묶음 실행이 없습니다");
  Txn t = std::move(*group_);
  group_.reset();
  if (!t.empty()) push_txn(std::move(t));
}

void App::transaction_rollback() {
  if (!group_) throw Error("invalid_state", "열려 있는 묶음 실행이 없습니다");
  const Txn t = std::move(*group_);
  group_.reset();
  revert(t);
  emit(t.changes, t.mesh, true, "rollback", t.name);
}

Json App::transaction_changes() const {
  if (!group_) throw Error("invalid_state", "열려 있는 묶음 실행이 없습니다");
  // 같은 객체가 여러 번 바뀌면 처음 before 와 마지막 after 로 판단한다
  std::map<Id, std::pair<std::optional<Object>, std::optional<Object>>> net;
  std::vector<Id> order;
  for (const Change& c : group_->changes) {
    auto it = net.find(c.id);
    if (it == net.end()) net[c.id] = {c.before, c.after}, order.push_back(c.id);
    else it->second.second = c.after;
  }
  Json created = Json::array(), updated = Json::array(), deleted = Json::array();
  auto brief = [](const Object& o) { return Json{{"id", o.id}, {"kind", o.kind}, {"name", o.name}}; };
  for (Id id : order) {
    const auto& [before, after] = net[id];
    if (!before && after) created.push_back(brief(*after));
    else if (before && !after) deleted.push_back(brief(*before));
    else if (before && after && (before->props != after->props || before->name != after->name || before->parent != after->parent ||
                                 before->suppressed != after->suppressed || before->order != after->order))
      updated.push_back(brief(*after));
  }
  return Json{{"created", created}, {"updated", updated}, {"deleted", deleted}, {"mesh", !group_->mesh.empty()},
              {"mesh_ops", group_->mesh.size()}};
}

void App::new_project() {
  model_.clear();
  mesh_.clear();
  path_.clear();
  reset_history();
  emit({}, {}, false, "reset", "project.new");
}

void App::save(const std::string& path) {
  if (group_) throw Error("invalid_state", "묶음 실행 중에는 저장할 수 없습니다");
  std::ofstream f(fs_path(path), std::ios::binary | std::ios::trunc);  // 경로는 UTF-8(한글 경로)
  if (!f) throw Error("io_error", "파일을 쓸 수 없습니다: " + path, {{"path", path}});
  Json j = model_.to_json();
  if (!mesh_.empty()) j["mesh"] = mesh_.to_json();
  f << j.dump(1);
  f.close();
  if (!f) throw Error("io_error", "파일 쓰기에 실패했습니다: " + path, {{"path", path}});
  path_ = path;
  saved_token_ = state_token();
}

void App::open(const std::string& path) {
  std::ifstream f(fs_path(path), std::ios::binary);
  if (!f) throw Error("io_error", "파일을 열 수 없습니다: " + path, {{"path", path}});
  Json j;
  try {
    j = Json::parse(f);
  } catch (const Json::exception& e) {
    throw Error("invalid_file", std::string("프로젝트 파일을 읽을 수 없습니다: ") + e.what(), {{"path", path}});
  }
  Model loaded;
  Mesh loaded_mesh;
  try {
    loaded.load_json(j);
    if (j.contains("mesh")) loaded_mesh.load_json(j["mesh"]);
  } catch (const Json::exception& e) {
    throw Error("invalid_file", std::string("프로젝트 파일의 내용이 올바르지 않습니다: ") + e.what(),
                {{"path", path}});
  }
  // 옛 파일의 파트 없는 요소도 열기 통지 전에 보완한다. invoke에 필요한
  // 변경 기록만 열고, 보완은 불러온 상태의 일부이므로 Undo 이력에는 넣지 않는다.
  Model previous_model = std::move(model_);
  Mesh previous_mesh = std::move(mesh_);
  model_ = std::move(loaded);
  mesh_ = std::move(loaded_mesh);
  try {
    model_.begin_record();
    mesh_.begin_record();
    ensure_mesh_parts();
    model_.end_record();
    mesh_.end_record();
  } catch (...) {
    model_ = std::move(previous_model);
    mesh_ = std::move(previous_mesh);
    throw;  // 보완 실패 시에도 기존 모델·경로·이력·화면은 그대로 둔다.
  }
  path_ = path;
  reset_history();
  emit({}, {}, false, "reset", "project.open");
}

Json App::info() const {
  std::map<std::string, std::size_t> counts;
  for (const Object* o : model_.all()) ++counts[o->kind];
  return Json{{"path", path_},
              {"modified", state_token() != saved_token_},
              {"objects", model_.size()},
              {"counts", counts},
              {"nodes", mesh_.node_count()},
              {"elements", mesh_.element_count()},
              {"listener_error", last_listener_error_}};  // 마지막 통지 구독자 오류(없으면 null)
}

void App::journal_start(const std::string& path) {
  journal_stop();
  journal_.open(fs_path(path), std::ios::binary | std::ios::trunc);
  if (!journal_) throw Error("io_error", "저널 파일을 쓸 수 없습니다: " + path, {{"path", path}});
}

void App::journal_stop() {
  if (journal_.is_open()) journal_.close();
}

namespace {
// JSON 값을 Python 리터럴로 쓴다(매크로용).
std::string py_literal(const Json& v) {
  if (v.is_null()) return "None";
  if (v.is_boolean()) return v.get<bool>() ? "True" : "False";
  if (v.is_array()) {
    std::string s = "[";
    for (std::size_t i = 0; i < v.size(); ++i) s += (i ? ", " : "") + py_literal(v[i]);
    return s + "]";
  }
  if (v.is_object()) {
    std::string s = "{";
    bool first = true;
    for (auto it = v.begin(); it != v.end(); ++it) {
      s += (first ? "" : ", ") + Json(it.key()).dump() + ": " + py_literal(it.value());
      first = false;
    }
    return s + "}";
  }
  return v.dump();  // 숫자·문자열은 JSON 표기가 Python 에서도 유효하다
}
}  // namespace

void App::journal_write(const std::string& name, const Json& params) {
  if (replaying_) return;
  if (macro_on_) macro_lines_.push_back("app.execute(" + Json(name).dump() + ", " + py_literal(params) + ")");
  if (!journal_.is_open()) return;
  journal_ << Json{{"command", name}, {"params", params}}.dump() << "\n";
  journal_.flush();
}

void App::macro_start() {
  macro_on_ = true;
  macro_lines_.clear();
}

std::string App::macro_stop() {
  macro_on_ = false;
  std::string code = "# NASA-95 매크로. `app` 은 nasa95.App 객체다.\n";
  for (const std::string& line : macro_lines_) code += line + "\n";
  macro_lines_.clear();
  return code;
}

Json App::journal_replay(const std::string& path) {
  std::ifstream f(fs_path(path), std::ios::binary);
  if (!f) throw Error("io_error", "저널 파일을 열 수 없습니다: " + path, {{"path", path}});
  std::size_t n = 0;
  std::string line;
  replaying_ = true;
  try {
    while (std::getline(f, line)) {
      if (line.empty()) continue;
      Json j;
      try {
        j = Json::parse(line);
      } catch (const Json::exception&) {
        throw Error("invalid_file", "저널의 " + std::to_string(n + 1) + "번째 줄을 읽을 수 없습니다",
                    {{"path", path}, {"line", n + 1}});
      }
      execute(j.at("command").get<std::string>(), j.value("params", Json::object()));
      ++n;
    }
  } catch (...) {
    replaying_ = false;
    throw;
  }
  replaying_ = false;
  return Json{{"executed", n}};
}

int App::subscribe(Listener fn) {
  const int token = next_listener_++;
  listeners_.emplace(token, std::move(fn));
  return token;
}

void App::unsubscribe(int token) { listeners_.erase(token); }

void App::emit(const ChangeSet& cs, const MeshLog& mesh, bool inverse, const std::string& source,
               const std::string& command) {
  if (listeners_.empty()) return;
  // 같은 객체가 여러 번 바뀌었으면 처음 상태와 마지막 상태만 본다.
  std::map<Id, std::pair<std::optional<Object>, std::optional<Object>>> net;
  for (const Change& c : cs) {
    auto it = net.find(c.id);
    if (it == net.end())
      net.emplace(c.id, std::make_pair(c.before, c.after));
    else
      it->second.second = c.after;
  }
  Json created = Json::array(), modified = Json::array(), deleted = Json::array();
  for (auto& [id, ba] : net) {
    const auto& before = inverse ? ba.second : ba.first;
    const auto& after = inverse ? ba.first : ba.second;
    if (!before && after)
      created.push_back(id);
    else if (before && !after)
      deleted.push_back(id);
    else if (before && after && !(*before == *after))
      modified.push_back(id);
  }
  // 메시 변경은 개수만 알린다(목록은 클 수 있으므로 필요하면 조회한다).
  std::size_t na = 0, nr = 0, nm = 0, ea = 0, er = 0, em = 0;
  for (const MeshOp& op : mesh) {
    switch (op.kind) {
      case MeshOp::AddNodes: (inverse ? nr : na) += op.ids.size(); break;
      case MeshOp::RemoveNodes: (inverse ? na : nr) += op.ids.size(); break;
      case MeshOp::MoveNodes: nm += op.ids.size(); break;
      case MeshOp::AddElements: (inverse ? er : ea) += op.elems.size(); break;
      case MeshOp::RemoveElements: (inverse ? ea : er) += op.old_elems.size(); break;
      case MeshOp::ReplaceElements: em += op.elems.size(); break;
    }
  }
  const bool mesh_changed = na || nr || nm || ea || er || em;
  if (source != "reset" && created.empty() && modified.empty() && deleted.empty() && !mesh_changed) return;
  Json ev{{"source", source}, {"command", command}, {"created", created}, {"modified", modified}, {"deleted", deleted}};
  if (mesh_changed)
    ev["mesh"] = Json{{"nodes_added", na},    {"nodes_removed", nr},    {"nodes_moved", nm},
                      {"elements_added", ea}, {"elements_removed", er}, {"elements_modified", em}};
  const auto copy = listeners_;  // 통지 처리 중 구독이 바뀌어도 안전하게
  for (const auto& [token, fn] : copy) {
    // 구독자의 오류는 명령을 실패시키지 않는다(변경은 이미 끝났고 이력에 들어 있다). 마지막 오류를 기억해 조회할 수 있게 한다.
    try {
      fn(ev);
    } catch (const std::exception& e) {
      last_listener_error_ = Json{{"token", token}, {"source", source}, {"command", command}, {"message", e.what()}};
    } catch (...) {
      last_listener_error_ = Json{{"token", token}, {"source", source}, {"command", command}, {"message", "unknown"}};
    }
  }
}

}  // namespace nasa95
