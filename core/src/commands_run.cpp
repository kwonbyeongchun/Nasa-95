// 솔버 실행(CAS-13, CAS-39, CAS-41): 덱을 쓰고 CalculiX 를 별도 프로세스로 띄운다. 상태·로그·수렴 이력 조회, 중지.
// 실행 상태는 모델에 속하지 않는다(저장·Undo 대상이 아님).
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <thread>

#include "nasa95/app.hpp"
#include "nasa95/deck.hpp"
#include "nasa95/solver.hpp"
#include "nasa95/error.hpp"
#include "nasa95/process.hpp"

namespace nasa95 {

namespace {

using F = FieldSpec;
namespace fs = std::filesystem;

struct Run {
  Process process;
  std::string job, work, deck, log, solver;
  std::string solver_name;  // 백엔드 이름(calculix·opensees·mystran)
  std::string digest;  // 실행 시작 때의 모델·메시 다이제스트(결과가 모델보다 오래됐는지 판정)
  bool check_only = false, stopped = false;
  std::chrono::steady_clock::time_point started, finished;
  bool finish_seen = false;
};
using Runs = std::map<Id, std::shared_ptr<Run>>;

Runs& runs(App& a) {
  std::any& slot = a.runtime("solver_runs");
  if (!slot.has_value()) slot = std::make_shared<Runs>();
  return *std::any_cast<std::shared_ptr<Runs>&>(slot);
}

fs::path path_of(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }
std::string utf8(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}

const Object& case_of(const App& a, const Json& p) {
  const Object& o = a.model().get(p["id"].get<Id>());
  if (o.kind != "case")
    throw Error("wrong_kind", "id=" + std::to_string(o.id) + " 는 해석 케이스가 아닙니다", {{"object", o.id}, {"expected", "case"}});
  return o;
}

// 솔버 실행 파일: 케이스의 지정 → 프로그램 설정(백엔드의 setting_key: solver_executable·opensees_executable·mystran_executable) → 환경 변수(백엔드의 env_var).
std::string solver_path(App& a, const Object& cs) {
  const SolverSpec& spec = solver_spec(case_solver(cs));
  std::string exe = cs.props.value("solver_executable", std::string());
  if (exe.empty()) {
    const Json s = program_setting(a, spec.setting_key);
    if (s.is_string()) exe = s.get<std::string>();
  }
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)  // getenv: 읽기만 한다
#endif
  const char* env = exe.empty() ? std::getenv(spec.env_var.c_str()) : nullptr;
#ifdef _MSC_VER
#pragma warning(pop)
#endif
  if (env) exe = env;
  if (exe.empty())
    throw Error("solver_not_found",
                spec.label + " 실행 파일이 지정되지 않았습니다(케이스의 solver_executable, 프로그램 설정 " + spec.setting_key + ", 환경 변수 " + spec.env_var + ")",
                {{"object", cs.id}, {"solver", spec.name}});
  std::error_code ec;
  if (!fs::is_regular_file(path_of(exe), ec))
    throw Error("solver_not_found", "솔버 실행 파일이 없습니다: " + exe, {{"object", cs.id}, {"path", exe}, {"solver", spec.name}});
  return exe;
}

// 작업 이름: 솔버가 파일 이름으로 쓰므로 영문·숫자만 남긴다.
std::string job_name(const Object& cs) {
  std::string s;
  for (unsigned char c : cs.name)
    if (std::isalnum(c) || c == '_' || c == '-') s += static_cast<char>(c);
  return s.empty() ? "case" + std::to_string(cs.id) : s;
}

// 작업 폴더: 케이스의 지정(상대 경로면 프로젝트 파일 기준) → 프로그램 설정 work_directory(그 아래 작업 이름 폴더) → 프로젝트 파일 옆 → 임시 폴더.
fs::path work_dir(App& a, const Object& cs, const std::string& job) {
  const std::string project = a.info().value("path", std::string());
  const fs::path base = project.empty() ? fs::path() : path_of(project).parent_path();
  std::string given = cs.props.value("work_directory", std::string());
  if (given.empty()) {
    const Json s = program_setting(a, "work_directory");
    if (s.is_string() && !s.get<std::string>().empty()) given = utf8(path_of(s.get<std::string>()) / job);
  }
  fs::path dir;
  if (!given.empty()) {
    dir = path_of(given);
    if (dir.is_relative()) dir = (base.empty() ? fs::current_path() : base) / dir;
  } else if (!project.empty()) {
    dir = base / (utf8(path_of(project).stem()) + ".work") / job;
  } else {
    dir = fs::temp_directory_path() / "nasa95" / job;
  }
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) throw Error("io_error", "작업 폴더를 만들 수 없습니다: " + utf8(dir), {{"path", utf8(dir)}});
  return dir;
}

std::string read_tail(const fs::path& p, std::size_t limit = 4 * 1024 * 1024) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return "";
  f.seekg(0, std::ios::end);
  const std::streamoff size = f.tellg();
  const std::streamoff start = size > static_cast<std::streamoff>(limit) ? size - static_cast<std::streamoff>(limit) : 0;
  f.seekg(start);
  std::string s(static_cast<std::size_t>(size - start), '\0');
  f.read(s.data(), static_cast<std::streamsize>(s.size()));
  return s;
}

std::vector<std::string> lines_of(const std::string& text) {
  std::vector<std::string> out;
  std::istringstream is(text);
  for (std::string line; std::getline(is, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    out.push_back(line);
  }
  return out;
}

std::string trimmed(const std::string& s) {
  const std::size_t b = s.find_first_not_of(" \t");
  return b == std::string::npos ? "" : s.substr(b, s.find_last_not_of(" \t") - b + 1);
}

Json status(App& a, const Object& cs, std::size_t tail_lines) {
  auto it = runs(a).find(cs.id);
  if (it == runs(a).end()) return Json{{"state", "none"}, {"case", cs.id}};
  Run& r = *it->second;
  const bool running = r.process.running();
  if (!running && !r.finish_seen) {
    r.finished = std::chrono::steady_clock::now(), r.finish_seen = true;
    // 백엔드의 뒷정리(MyStran: F06 → frd). 실패는 로그에 적고 상태는 solver 의 종료 코드로 판단한다
    const SolverSpec& spec = solver_spec(r.solver_name.empty() ? case_solver(cs) : r.solver_name);
    if (spec.after_run && !r.stopped && !r.check_only) {
      try {
        spec.after_run(a, cs, r.work, r.job);
      } catch (const Error& e) {
        std::ofstream lf(path_of(r.log), std::ios::app);
        lf << "\n*ERROR NASA-95: 결과 변환 실패: " << e.what() << "\n";
      }
    }
  }
  const auto end = running ? std::chrono::steady_clock::now() : r.finished;
  const std::string log = read_tail(path_of(r.log));
  const std::vector<std::string> lines = lines_of(log);

  // 오류: "*ERROR" 로 시작하는 줄과 이어지는 설명 줄(빈 줄 전까지)
  Json errors = Json::array();
  std::size_t warnings = 0;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].find("*WARNING") != std::string::npos) ++warnings;
    if (lines[i].find("*ERROR") == std::string::npos) continue;
    std::string msg = trimmed(lines[i]);
    for (std::size_t k = i + 1; k < lines.size() && !trimmed(lines[k]).empty() && lines[k].find('*') == std::string::npos; ++k)
      msg += " " + trimmed(lines[k]), i = k;
    errors.push_back(msg);
  }
  std::string state = running ? "running" : r.stopped ? "stopped" : (r.process.exit_code() == 0 && errors.empty()) ? "completed" : "failed";

  // 수렴 이력(.sta): [스텝, 증분, 시도, 반복 수, 전체 시간, 스텝 시간, 증분 크기]
  Json increments = Json::array();
  const fs::path stem = path_of(r.work) / r.job;
  for (const std::string& line : lines_of(read_tail(fs::path(stem).concat(".sta")))) {
    std::istringstream is(line);
    std::vector<double> v;
    for (double x; is >> x;) v.push_back(x);
    if (v.size() >= 7 && is.eof())
      increments.push_back(Json{{"step", static_cast<int>(v[0])}, {"increment", static_cast<int>(v[1])}, {"attempt", static_cast<int>(v[2])},
                                {"iterations", static_cast<int>(v[3])}, {"total_time", v[4]}, {"step_time", v[5]}, {"increment_size", v[6]}});
  }
  Json files = Json::object();
  for (const char* ext : {"inp", "tcl", "bdf", "frd", "dat", "sta", "cvg", "eig", "F06", "OP2"}) {
    std::error_code ec;
    const fs::path f = fs::path(stem).concat(std::string(".") + ext);
    if (fs::is_regular_file(f, ec)) files[ext] = Json{{"path", utf8(f)}, {"bytes", static_cast<std::uint64_t>(fs::file_size(f, ec))}};
  }
  Json tail = Json::array();
  for (std::size_t i = lines.size() > tail_lines ? lines.size() - tail_lines : 0; i < lines.size(); ++i) tail.push_back(lines[i]);
  Json out{{"state", state}, {"case", cs.id}, {"job", r.job}, {"work_directory", r.work}, {"solver", r.solver},
           {"check_only", r.check_only}, {"elapsed", std::chrono::duration<double>(end - r.started).count()},
           {"errors", errors}, {"warnings", warnings}, {"increments", increments}, {"files", files}, {"log", tail}};
  if (!running) out["exit_code"] = r.process.exit_code();
  if (!running) out["outdated"] = !r.digest.empty() && a.digest()["total"].get<std::string>() != r.digest;  // WT-32
  if (!r.digest.empty()) out["model_digest"] = r.digest;  // 결과를 열 때 이어받는다(result.info 의 outdated, D13)
  return out;
}

Json start(App& a, const Json& p, bool check_only, int restart_step = 0) {
  const Object& cs = case_of(a, p);
  auto existing = runs(a).find(cs.id);
  if (existing != runs(a).end() && existing->second->process.running())
    throw Error("invalid_state", "이 케이스는 이미 실행 중입니다", {{"object", cs.id}});
  const std::string exe = solver_path(a, cs);
  const SolverSpec& spec = solver_spec(case_solver(cs));
  if (!spec.restart && restart_step > 0) throw Error("not_supported", spec.label + " 케이스는 재시작을 지원하지 않습니다", {{"object", cs.id}});
  DeckOptions options;
  options.no_analysis = check_only;
  options.restart_from_step = restart_step;
  const DeckResult deck = write_deck(a, cs, options);
  if (!deck.skipped.empty() && !p.value("allow_skipped", false))
    throw Error("deck_incomplete", "덱에 쓰지 못한 객체가 있어 실행하지 않습니다(allow_skipped 로 강제할 수 있다)",
                {{"object", cs.id}, {"skipped", deck.skipped}});

  auto run = std::make_shared<Run>();
  run->digest = a.digest()["total"].get<std::string>();
  const std::string base_job = job_name(cs);
  run->job = restart_step > 0 ? base_job + "_restart" + std::to_string(restart_step) : base_job;
  const fs::path dir = work_dir(a, cs, base_job);
  if (restart_step > 0) {
    // 재시작(CAS-38): 앞선 실행의 <job>.rout 를 새 작업 이름의 .rin 으로 복사한다(매뉴얼 7.110: 다른 이름의 작업은 .rin 으로 바꿔야 한다)
    const fs::path rout = (p.contains("restart_file") && !p["restart_file"].is_null()) ? path_of(p["restart_file"].get<std::string>()) : dir / (base_job + ".rout");
    std::error_code ec;
    if (!fs::is_regular_file(rout, ec))
      throw Error("not_found", "재시작 파일이 없습니다(앞선 실행의 스텝에 restart_write 가 있어야 한다): " + utf8(rout), {{"object", cs.id}, {"path", utf8(rout)}});
    fs::copy_file(rout, dir / (run->job + ".rin"), fs::copy_options::overwrite_existing, ec);
    if (ec) throw Error("io_error", "재시작 파일을 복사할 수 없습니다: " + utf8(rout), {{"path", utf8(rout)}});
  }
  run->work = utf8(dir), run->solver = exe, run->check_only = check_only;
  run->deck = utf8(dir / (run->job + "." + spec.deck_extension));
  run->solver_name = spec.name;
  run->log = utf8(dir / (run->job + ".log"));
  // 앞선 실행의 산출물이 남아 있으면 이번 결과와 헷갈린다.
  for (const char* ext : {"frd", "dat", "sta", "cvg", "log", "F06", "ERR", "OP2"}) {
    std::error_code ec;
    fs::remove(dir / (run->job + "." + ext), ec);
  }
  {
    std::ofstream f(path_of(run->deck), std::ios::binary);
    if (!f) throw Error("io_error", "덱을 쓸 수 없습니다: " + run->deck, {{"path", run->deck}});
    f.write(deck.text.data(), static_cast<std::streamsize>(deck.text.size()));
  }
  std::map<std::string, std::string> env;
  if (cs.props.contains("threads") && !cs.props["threads"].is_null()) env["OMP_NUM_THREADS"] = cs.props["threads"].dump();
  else if (const Json t = program_setting(a, "threads"); t.is_number_integer()) env["OMP_NUM_THREADS"] = t.dump();
  run->started = std::chrono::steady_clock::now();
  run->process.start(exe, spec.run_args(run->job), run->work, env, run->log);  // OpenSees 는 스크립트가 <job>.frd·<job>.sta 를 쓴다
  runs(a)[cs.id] = run;
  if (p.value("wait", false)) run->process.wait();
  Json out = status(a, cs, 20);
  out["deck"] = run->deck, out["skipped"] = deck.skipped, out["deck_warnings"] = deck.warnings;
  if (restart_step > 0) out["restart_step"] = restart_step;
  return out;
}

CommandSpec base(const std::string& name, char kind, const std::string& desc, const std::string& features) {
  CommandSpec c;
  c.name = name, c.kind = kind, c.undoable = false, c.target = "case", c.desc = desc, c.features = features;
  return c;
}

}  // namespace

void register_run_commands(App& app) {
  for (const bool check_only : {false, true}) {
    CommandSpec c = check_only ? base("case.run_check_only", 'J', "계산 없이 입력 검사만 실행한다", "CAS-39")
                               : base("case.run", 'J', "덱을 쓰고 솔버를 실행한다", "CAS-13, CAS-41");
    c.params = {F("id", "ref", "해석 케이스").call_req(), F("wait", "bool", "끝날 때까지 기다린다(없으면 바로 돌아온다)"),
                F("allow_skipped", "bool", "덱에 쓰지 못한 객체가 있어도 실행한다")};
    c.fn = [check_only](App& a, const Json& p) { return start(a, p, check_only); };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("case.restart", 'J',
                         "재시작 파일에서 이어서 실행한다: 앞선 실행의 <job>.rout(또는 restart_file)를 <job>_restart<n>.rin 으로 두고, "
                         "`*RESTART, READ, STEP=n` 과 n 번째 뒤의 스텝만 쓴 덱으로 솔버를 실행한다. 상태·결과는 이 케이스로 조회한다",
                         "CAS-38");
    c.params = {F("id", "ref", "해석 케이스").call_req(), F("step", "integer", "이어서 시작할 기준 스텝(덱에 쓰인 순번, 이 스텝까지의 결과를 읽는다)").call_req().ge(1).ex(1),
                F("restart_file", "string", "재시작 파일(.rout) 경로(없으면 이 케이스 작업 폴더의 것)"),
                F("wait", "bool", "끝날 때까지 기다린다"), F("allow_skipped", "bool", "덱에 쓰지 못한 객체가 있어도 실행한다")};
    c.fn = [](App& a, const Json& p) {
      check_value(a.commands().at("case.restart").params[1], p["step"], nullptr);
      return start(a, p, false, p["step"].get<int>());
    };
    app.register_command(std::move(c));
  }
  {
    // 결과 연결(CAS-14): 결과 파일 객체(result_file)를 만들어 케이스에 잇는다. 경로가 없으면 이 케이스의 실행 폴더에서 frd 를 찾는다.
    CommandSpec c = base("case.attach_results", 'C',
                         "결과 파일을 케이스에 연결한다(result_file 객체를 만든다). path 가 없으면 이 케이스 실행의 frd 를 쓴다. "
                         "이후 result.open file= 으로 연다",
                         "CAS-14");
    c.undoable = true;
    c.params = {F("id", "ref", "해석 케이스").call_req(), F("path", "string", "결과 파일(frd) 경로").ex("job.frd"),
                F("name", "string", "결과 파일 객체 이름(없으면 파일 이름)"), F("description", "string", "설명")};
    c.fn = [](App& a, const Json& p) {
      const Object& cs = case_of(a, p);
      std::string path = p.value("path", std::string());
      if (path.empty()) {
        const Json st = status(a, cs, 0);
        if (!st.contains("files") || !st["files"].contains("frd"))
          throw Error("not_found", "이 케이스의 결과 파일이 없습니다(실행하지 않았거나 결과 출력 요청이 없음). path 를 주세요", {{"object", cs.id}, {"param", "path"}});
        path = st["files"]["frd"]["path"].get<std::string>();
      }
      std::error_code ec;
      if (!fs::is_regular_file(path_of(path), ec)) throw Error("not_found", "결과 파일이 없습니다: " + path, {{"param", "path"}, {"path", path}});
      Json q{{"case", cs.id}, {"path", path}, {"name", p.value("name", utf8(path_of(path).filename()))}};
      if (p.contains("description") && !p["description"].is_null()) q["description"] = p["description"];
      const Json r = a.invoke("result_file.create", q);
      return Json{{"id", r["id"]}, {"case", cs.id}, {"path", path}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("case.run_status", 'Q', "실행 상태·로그·오류·수렴 이력·산출 파일을 조회한다", "CAS-13, RES-29, WT-32");
    c.params = {F("id", "ref", "해석 케이스").call_req(), F("log_lines", "integer", "돌려줄 로그의 끝 줄 수(기본 20)").ge(0)};
    c.fn = [](App& a, const Json& p) {
      const std::size_t n = p.contains("log_lines") && !p["log_lines"].is_null() ? p["log_lines"].get<std::size_t>() : 20;
      return status(a, case_of(a, p), n);
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("case.run_stop", 'S', "실행 중인 솔버를 중지한다", "CAS-13");
    c.params = {F("id", "ref", "해석 케이스").call_req()};
    c.fn = [](App& a, const Json& p) {
      const Object& cs = case_of(a, p);
      auto it = runs(a).find(cs.id);
      if (it != runs(a).end() && it->second->process.running()) {
        it->second->process.kill();
        it->second->stopped = true;
      }
      return status(a, cs, 20);
    };
    app.register_command(std::move(c));
  }
  // --- 작업(job, API-07): 솔버 실행과 배경 메싱을 한 목록으로 본다. 작업 ID 는 "solver:<케이스 ID>" 와 "mesh".
  {
    auto job_list = [](App& a) {
      Json arr = Json::array();
      for (const auto& [case_id, run] : runs(a)) {
        const Object* cs = a.model().find(case_id);
        if (!cs) continue;
        Json st = status(a, *cs, 0);
        arr.push_back(Json{{"id", "solver:" + std::to_string(case_id)}, {"kind", "solver"}, {"object", case_id}, {"state", st["state"]},
                           {"elapsed", st["elapsed"]}, {"progress", st.contains("increments") && !st["increments"].empty() ? st["increments"].back() : Json()}});
      }
      if (a.commands().count("mesh.job_status")) {
        const Json st = a.commands().at("mesh.job_status").fn(a, Json::object());
        if (st["state"] != "none")
          arr.push_back(Json{{"id", "mesh"}, {"kind", "mesh"}, {"object", st.value("part", Id(0))}, {"state", st["state"]},
                             {"elapsed", st.value("elapsed", 0.0)}, {"progress", Json{{"percent", st.value("percent", 0.0)}, {"task", st.value("task", std::string())}}}});
      }
      return arr;
    };
    auto find_job = [job_list](App& a, const std::string& id) {
      for (const Json& j : job_list(a))
        if (j["id"] == id) return j;
      throw Error("not_found", "없는 작업입니다: " + id, {{"param", "id"}});
    };
    CommandSpec l = base("job.list", 'Q', "작업(솔버 실행·배경 메싱) 목록과 상태·진행률을 조회한다", "API-07, API-30");
    l.fn = [job_list](App& a, const Json&) { return job_list(a); };
    app.register_command(std::move(l));
    CommandSpec g = base("job.get", 'Q', "작업 하나의 상태·진행률을 조회한다", "API-07, API-30");
    g.params = {F("id", "string", "작업 ID(solver:<케이스> 또는 mesh)").call_req().ex("mesh")};
    g.fn = [find_job](App& a, const Json& p) { return find_job(a, p["id"].get<std::string>()); };
    app.register_command(std::move(g));
    CommandSpec k = base("job.cancel", 'S', "작업을 취소한다(솔버는 중지, 메싱은 멈춤)", "API-07");
    k.params = {F("id", "string", "작업 ID").call_req().ex("mesh")};
    k.fn = [find_job](App& a, const Json& p) {
      const std::string id = p["id"].get<std::string>();
      const Json j = find_job(a, id);
      if (j["kind"] == "solver") return a.commands().at("case.run_stop").fn(a, Json{{"id", j["object"]}});
      return a.commands().at("mesh.job_cancel").fn(a, Json::object());
    };
    app.register_command(std::move(k));
    CommandSpec w = base("job.wait", 'Q', "작업이 끝날 때까지 기다린다(timeout 초, 기본 600). 끝난 상태를 돌려준다", "API-07");
    w.params = {F("id", "string", "작업 ID").call_req().ex("mesh"), F("timeout", "number", "최대 대기(초)").gt(0)};
    w.fn = [find_job](App& a, const Json& p) {
      const std::string id = p["id"].get<std::string>();
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(p.value("timeout", 600.0));
      for (;;) {
        const Json j = find_job(a, id);
        if (j["state"] != "running") return j;
        if (std::chrono::steady_clock::now() > deadline) throw Error("timeout", "작업이 제한 시간 안에 끝나지 않았습니다", {{"param", "timeout"}, {"job", id}});
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    };
    app.register_command(std::move(w));
  }
  {
    CommandSpec c = base("solver.list", 'Q', "등록된 솔버 백엔드와 지원 범위(기능 표)를 조회한다: 이름·덱 확장자·실행 파일 환경 변수·프로그램 설정 키·라이선스 메모·"
                                         "지원 스텝 종류·요소 형상·하중·경계조건·프로퍼티·재료 구성 모델·구속, 실행 파일이 잡히는지(executable)", "CAS-44, CAS-45");
    c.target = "";
    c.params = {};
    c.fn = [](App& a, const Json&) {
      Json out = Json::array();
      for (const SolverSpec& spec : solver_specs()) {
        Json j = solver_capabilities(spec);
        Object fake;
        fake.kind = "case", fake.props["solver"] = spec.name;
        try {
          j["executable"] = solver_path(a, fake);
        } catch (const Error&) {
          j["executable"] = nullptr;
        }
        out.push_back(j);
      }
      return out;
    };
    app.register_command(std::move(c));
  }
}

}  // namespace nasa95
