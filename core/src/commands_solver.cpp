// 솔버 입력 파일(덱) 출력과 메시 내보내기.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "ofep/app.hpp"
#include "ofep/deck.hpp"
#include "ofep/error.hpp"

namespace ofep {

namespace {

using F = FieldSpec;

CommandSpec base(const std::string& name, char kind, const std::string& target, const std::string& desc,
                 const std::string& features) {
  CommandSpec c;
  c.name = name, c.kind = kind, c.undoable = false, c.target = target, c.desc = desc, c.features = features;
  return c;
}

const Object& case_of(const App& a, const Json& p) {
  const Object& o = a.model().get(p["id"].get<Id>());
  if (o.kind != "case")
    throw Error("wrong_kind", "id=" + std::to_string(o.id) + " 는 해석 케이스가 아닙니다", {{"object", o.id}, {"expected", "case"}});
  return o;
}

std::size_t line_count(const std::string& text) { return static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')); }

namespace fs = std::filesystem;
fs::path path_of(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }
std::string utf8(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}

// 경로는 UTF-8 로 받는다.
void write_file(const std::string& path, const std::string& text) {
  const fs::path fs_path = path_of(path);
  std::ofstream f(fs_path, std::ios::binary);
  if (!f) throw Error("io_error", "파일을 쓸 수 없습니다: " + path, {{"path", path}});
  f.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!f) throw Error("io_error", "파일을 쓰다가 실패했습니다: " + path, {{"path", path}});
}

}  // namespace

void register_solver_commands(App& app) {
  {
    CommandSpec c = base("case.preview_deck", 'Q', "case", "솔버 입력 파일의 내용을 파일로 쓰지 않고 조회한다", "CAS-10");
    c.params = {F("id", "ref", "해석 케이스").call_req(),
                F("max_lines", "integer", "이 줄 수까지만 돌려준다(없으면 전부)").ge(1)};
    c.fn = [](App& a, const Json& p) {
      DeckResult r = write_deck(a, case_of(a, p));
      const std::size_t lines = line_count(r.text);
      bool truncated = false;
      if (p.contains("max_lines") && !p["max_lines"].is_null()) {
        check_value(a.commands().at("case.preview_deck").params[1], p["max_lines"], nullptr);
        std::size_t limit = p["max_lines"].get<std::size_t>(), pos = 0;
        while (limit-- && pos != std::string::npos) pos = r.text.find('\n', pos) == std::string::npos ? std::string::npos : r.text.find('\n', pos) + 1;
        if (pos != std::string::npos && pos < r.text.size()) r.text.resize(pos), truncated = true;
      }
      return Json{{"text", r.text}, {"lines", lines}, {"truncated", truncated}, {"warnings", r.warnings}, {"skipped", r.skipped}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("case.export_deck", 'J', "case", "솔버 입력 파일을 쓴다", "CAS-10, CAS-40");
    c.params = {F("id", "ref", "해석 케이스").call_req(), F("path", "string", "파일 경로(.inp)").call_req().ex("job.inp"),
                F("split", "bool", "메시(노드·요소·셋)를 <이름>_mesh.inp 로 나누고 본 파일에서 *INCLUDE 로 읽게 한다(CAS-40)")};
    c.fn = [](App& a, const Json& p) {
      const DeckResult r = write_deck(a, case_of(a, p));
      const std::string path = p["path"].get<std::string>();
      Json out{{"path", path}, {"warnings", r.warnings}, {"skipped", r.skipped}};
      if (p.value("split", false)) {
        const fs::path main = path_of(path);
        const fs::path mesh = main.parent_path() / (main.stem().string() + "_mesh.inp");
        const std::string mesh_name = utf8(mesh.filename());
        if (!std::all_of(mesh_name.begin(), mesh_name.end(), [](unsigned char c) { return c < 128 && c != ' '; }))
          throw Error("invalid_param", "분할 파일 이름은 공백 없는 ASCII 여야 합니다(*INCLUDE 가 그대로 읽는다)", {{"param", "path"}});
        const std::size_t head = r.text.size() - r.mesh_text.size() - r.model_text.size();
        const std::string body = r.text.substr(0, head) + "*INCLUDE, INPUT=" + mesh_name + "\n" + r.model_text;
        write_file(utf8(mesh), r.mesh_text);
        write_file(path, body);
        out["lines"] = line_count(body), out["bytes"] = body.size();
        out["files"] = Json{{"mesh", utf8(mesh)}};
        out["mesh_lines"] = line_count(r.mesh_text);
      } else {
        write_file(path, r.text);
        out["lines"] = line_count(r.text), out["bytes"] = r.text.size();
      }
      return out;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("mesh.export", 'J', "mesh", "메시를 범용 형식으로 내보낸다", "MSH-03");
    c.params = {F("path", "string", "파일 경로").call_req().ex("mesh.inp"),
                F("format", "string", "형식").one_of({"inp"})};
    c.fn = [](App& a, const Json& p) {
      if (p.contains("format") && !p["format"].is_null()) check_value(a.commands().at("mesh.export").params[1], p["format"], nullptr);
      if (a.mesh().node_count() == 0) throw Error("invalid_state", "내보낼 메시가 없습니다");
      const DeckResult r = write_mesh_deck(a);
      const std::string path = p["path"].get<std::string>();
      write_file(path, r.text);
      return Json{{"path", path}, {"format", "inp"}, {"nodes", a.mesh().node_count()}, {"elements", a.mesh().element_count()},
                  {"lines", line_count(r.text)}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("mesh.import", 'J', "mesh", "메시 파일에서 노드·요소·셋을 읽어 모델에 더한다(번호가 겹치면 통째로 민다)", "MSH-02");
    c.undoable = true;
    c.params = {F("path", "string", "파일 경로").call_req().ex("mesh.inp"),
                F("format", "string", "형식").one_of({"inp"}),
                F("scale", "number", "좌표에 곱할 배율(원본 단위 → 모델 단위)").gt(0).ex(1000.0),
                F("unit_system", "string", "원본 파일의 단위계(모델 단위계로 길이를 환산한다. scale 과 함께 주면 둘 다 곱한다)")
                    .one_of({"mm-t-s", "m-kg-s", "mm-kg-ms", "cm-g-s", "in-lbf-s"}).ex("m-kg-s")};
    c.fn = [](App& a, const Json& p) {
      for (std::size_t i : {std::size_t(1), std::size_t(2), std::size_t(3)})
        if (p.contains(a.commands().at("mesh.import").params[i].name) && !p[a.commands().at("mesh.import").params[i].name].is_null())
          check_value(a.commands().at("mesh.import").params[i], p[a.commands().at("mesh.import").params[i].name], nullptr);
      const std::size_t before = a.mesh().node_count();
      Json r = import_mesh_deck(a, p["path"].get<std::string>());
      double scale = p.contains("scale") && !p["scale"].is_null() ? p["scale"].get<double>() : 1.0;
      if (p.contains("unit_system") && !p["unit_system"].is_null()) {
        const Object* st = a.find_settings();
        const std::string model = st ? st->props.value("unit_system", std::string("mm-t-s")) : std::string("mm-t-s");
        scale *= unit_factor(p["unit_system"].get<std::string>(), model, "length");
      }
      if (scale != 1.0) {  // 새로 들어온 노드(목록의 끝쪽)만 환산한다
        const auto& ids = a.mesh().node_ids();
        const auto& xyz = a.mesh().node_xyz();
        std::vector<Id> moved;
        std::vector<double> coords;
        for (std::size_t i = before; i < ids.size(); ++i) {
          moved.push_back(ids[i]);
          for (int k = 0; k < 3; ++k) coords.push_back(xyz[3 * i + k] * scale);
        }
        a.mesh().move_nodes(moved, coords);
        r["scale"] = scale;
      }
      return r;
    };
    app.register_command(std::move(c));
  }
  {
    // 메시 파일을 임시 App 에 읽어 통계를 낸다(모델은 그대로).
    auto stats_of_file = [](const std::string& path) {
      App other;
      const Json r = other.execute("mesh.import", Json{{"path", path}});  // 기록 중인 명령 안에서만 읽을 수 있다
      Json s = other.commands().at("mesh.statistics").fn(other, Json::object());
      s["imported"] = Json{{"nodes", r["nodes"]}, {"elements", r["elements"]}, {"unsupported_types", r["unsupported_types"]}};
      return s;
    };
    auto compare = [](const Json& a, const Json& b) {
      Json out{{"model", a}, {"other", b}};
      auto diff = [&](const char* key) {
        if (a.contains(key) && b.contains(key) && a[key].is_number() && b[key].is_number()) {
          const double x = a[key].get<double>(), y = b[key].get<double>();
          out["diff"][key] = Json{{"model", x}, {"other", y}, {"ratio", x != 0.0 ? Json(y / x) : Json()}};
        }
      };
      for (const char* k : {"nodes", "elements", "volume", "area", "mass"}) diff(k);
      return out;
    };
    CommandSpec c = base("mesh.compare", 'Q', "mesh", "모델의 메시와 다른 메시 파일(inp)의 통계(노드·요소 수, 종류별 개수, 체적)를 비교한다", "MSH-39");
    c.params = {F("path", "string", "비교할 메시 파일(.inp)").call_req().ex("job.fin")};
    c.fn = [stats_of_file, compare](App& a, const Json& p) {
      const Json mine = a.commands().at("mesh.statistics").fn(a, Json::object());
      return compare(mine, stats_of_file(p["path"].get<std::string>()));
    };
    app.register_command(std::move(c));
    // ccx 2.22 로 확인: 세분화 메시는 <job>.rfn.inp 에 "*MODEL CHANGE,TYPE=ELEMENT,REMOVE"(세분화로 대체된 원래 요소) +
    // 전체 *NODE + 요소마다 "*ELEMENT,PARENT=…,TYPE=C3D10" 으로 나온다(매뉴얼의 <job>.fin 설명과 다르다). 이 빌드는 C3D4 메시에서는
    // 세분화 중 죽는다(힙 손상) — 2차 사면체(C3D10)에서 확인했다.
    CommandSpec c2 = base("mesh.import_refined", 'J', "mesh",
                          "솔버가 *REFINE MESH 로 만든 메시(작업 폴더의 <job>.rfn.inp 또는 <job>.fin)를 비교한 뒤 모델에 넣는다. "
                          "replace 면 그 파일이 지우라고 한(*MODEL CHANGE … REMOVE) 원래 요소를 지우고 새 요소를 넣는다. 하중·구속은 다시 확인한다",
                          "MSH-39");
    c2.undoable = true;
    c2.params = {F("id", "ref", "해석 케이스(실행한 작업 폴더에서 찾는다)").ref("case"),
                 F("path", "string", "케이스 대신 파일을 직접 지정").ex("job.rfn.inp"),
                 F("replace", "bool", "세분화로 대체된 원래 요소를 지운다(기본은 번호를 밀어 더하기만 한다)")};
    c2.fn = [stats_of_file, compare](App& a, const Json& p) {
      std::string path;
      if (p.contains("path") && !p["path"].is_null()) {
        path = p["path"].get<std::string>();
      } else if (p.contains("id") && !p["id"].is_null()) {
        const Json st = a.commands().at("case.run_status").fn(a, Json{{"id", p["id"]}});
        if (!st.contains("work_directory")) throw Error("invalid_state", "이 케이스는 아직 실행한 적이 없습니다", {{"object", p["id"]}});
        const fs::path dir = path_of(st["work_directory"].get<std::string>());
        const std::string job = st.value("job", std::string("job"));
        for (const std::string& name : {job + ".rfn.inp", job + ".fin"})
          if (fs::exists(dir / name)) path = utf8(dir / name);
        if (path.empty())
          throw Error("not_found", "세분화 메시 파일이 없습니다(*REFINE MESH 를 쓴 스텝이 있어야 한다): " + utf8(dir / (job + ".rfn.inp")),
                      {{"path", utf8(dir / (job + ".rfn.inp"))}});
      } else {
        throw Error("missing_param", "id 또는 path 가 필요합니다", {{"param", "id"}});
      }
      const Json mine = a.commands().at("mesh.statistics").fn(a, Json::object());
      const Json other = stats_of_file(path);
      // 파일이 지우라고 한 요소(*MODEL CHANGE,TYPE=ELEMENT,REMOVE 아래의 번호)
      std::vector<Id> removed;
      {
        std::ifstream f(path_of(path));
        bool in_remove = false;
        for (std::string line; std::getline(f, line);) {
          if (!line.empty() && line.back() == '\r') line.pop_back();
          const std::size_t s = line.find_first_not_of(" \t");
          if (s == std::string::npos) continue;
          if (line[s] == '*') {
            std::string key = line.substr(s);
            for (char& c : key) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            in_remove = key.rfind("*MODEL CHANGE", 0) == 0 && key.find("REMOVE") != std::string::npos;
            continue;
          }
          if (!in_remove) continue;
          std::istringstream is(line);
          for (std::string tok; std::getline(is, tok, ',');) {
            const std::size_t b = tok.find_first_not_of(" \t");
            if (b != std::string::npos) removed.push_back(static_cast<Id>(std::stoll(tok.substr(b))));
          }
        }
      }
      std::size_t deleted = 0;
      if (p.value("replace", false) && !removed.empty()) {
        std::vector<Id> present;
        for (Id e : removed)
          if (a.mesh().has_element(e)) present.push_back(e);
        if (!present.empty()) {
          std::set<Id> touched;
          for (Id e : present)
            for (Id n : a.mesh().element(e).nodes)
              if (n) touched.insert(n);
          a.invoke("mesh.elements_delete", Json{{"ids", present}});
          deleted = present.size();
          // 지운 요소만 쓰던 노드(남은 요소가 안 쓰는 것)는 함께 지운다 — 새 메시가 같은 자리에 새 번호의 노드를 둔다
          const std::vector<Id> used = a.mesh().used_nodes();
          std::vector<Id> orphans;
          for (Id n : touched)
            if (!std::binary_search(used.begin(), used.end(), n)) orphans.push_back(n);
          if (!orphans.empty()) a.invoke("mesh.nodes_delete", Json{{"ids", orphans}});
        }
      }
      // replace 면 번호를 이어 쓴 파일이므로 밀지 않고 합친다(남은 원래 요소와 절점을 함께 쓴다)
      Json r = import_mesh_deck(a, path, p.value("replace", false));
      r["comparison"] = compare(mine, other);
      r["path"] = path, r["removed_listed"] = removed.size(), r["removed"] = deleted;
      return r;
    };
    app.register_command(std::move(c2));
  }
  {
    CommandSpec c = base("deck.import", 'J', "", "CalculiX 입력 파일을 읽어 모델로 만든다(해석하지 못한 내용은 보존한다)", "CAS-11, CAS-12, CAS-40");
    c.undoable = true;
    c.params = {F("path", "string", "파일 경로(.inp)").call_req().ex("job.inp")};
    c.fn = [](App& a, const Json& p) { return import_deck(a, p["path"].get<std::string>()); };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("deck.unsupported", 'Q', "", "가져온 덱에서 해석하지 못해 보존만 한 내용을 조회한다", "CAS-12, WT-33");
    c.params = {F("id", "ref", "이 케이스의 것만(없으면 전부)").ref("case")};
    c.fn = [](App& a, const Json& p) {
      const Id only = p.contains("id") && !p["id"].is_null() ? p["id"].get<Id>() : 0;
      Json arr = Json::array();
      for (const Object* b : a.model().by_kind("deck_block")) {
        const Object& parent = a.model().get(b->parent);
        const Id owner = parent.kind == "step" ? parent.parent : parent.id;
        if (only && owner != only) continue;
        const std::string text = b->props.value("text", std::string());
        arr.push_back(Json{{"id", b->id}, {"case", owner}, {"step", parent.kind == "step" ? Json(parent.id) : Json()},
                           {"keyword", text.substr(0, text.find_first_of(",\n"))}, {"source", b->props.value("source", std::string())},
                           {"lines", static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'))}, {"suppressed", b->suppressed}});
      }
      return arr;
    };
    app.register_command(std::move(c));
  }
}

}  // namespace ofep
