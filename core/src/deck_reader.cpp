// CalculiX(Abaqus 형식) 입력 파일 읽기.
//
// 메시 가져오기(MSH-02): *NODE, *ELEMENT, *NSET, *ELSET, *SURFACE, *INCLUDE 를 읽는다.
//  - *ELEMENT 의 ELSET 은 메시 파트가 된다. 따로 적힌 *ELSET 은 요소 셋이 된다.
//  - 이미 메시가 있으면 번호가 겹치지 않게 노드·요소 번호를 통째로 민다.
// 덱 가져오기(CAS-11): 메시에 더해 재료·방향·함수·섹션·구속·접촉·초기 조건·좌표 변환·스텝·경계조건·하중·스텝 중 변경·
//   출력 요청을 읽어 케이스 하나를 만든다.
//  - 해석하지 못한 카드(모르는 키워드, 모르는 매개변수가 붙은 카드)는 버리지 않고 deck_block 으로 보존한다(CAS-12).
//    보존한 내용은 덱을 쓸 때 그대로 다시 나간다.
//  - 보존한 내용은 노드·요소 번호와 셋 이름을 글자 그대로 가리키므로, 덱 가져오기는 메시가 빈 모델에서만 한다.
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>

#include "ofep/deck.hpp"
#include "ofep/error.hpp"

namespace ofep {

namespace {

struct Card {
  std::string keyword;                        // 대문자, 빈칸 하나로 정리
  std::map<std::string, std::string> params;  // 키는 대문자, 값은 적힌 그대로
  std::vector<std::string> lines;             // 데이터 줄(빈 줄 제외)
  std::vector<std::string> raw_lines;         // 데이터 줄(가운데의 빈 줄 포함 — *SPRING 의 빈 자유도 줄처럼 뜻이 있는 경우가 있다)
  std::string head;                           // 키워드 줄(적힌 그대로)
  std::string file;
  std::size_t line = 0;
};

std::string trim(const std::string& s) {
  std::size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}
std::string upper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return s;
}
std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> out;
  std::size_t start = 0;
  for (;;) {
    const std::size_t pos = s.find(',', start);
    out.push_back(trim(s.substr(start, pos == std::string::npos ? pos : pos - start)));
    if (pos == std::string::npos) break;
    start = pos + 1;
  }
  return out;
}
std::filesystem::path fs_path(const std::string& utf8) { return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())); }
std::string utf8(const std::filesystem::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}

void read_cards(const std::filesystem::path& path, std::vector<Card>& cards, int depth) {
  const std::string shown = utf8(path);
  if (depth > 20) throw Error("invalid_state", "*INCLUDE 가 너무 깊습니다(순환 포함?)", {{"path", shown}});
  std::ifstream f(path, std::ios::binary);
  if (!f) throw Error("io_error", "파일을 열 수 없습니다: " + shown, {{"path", shown}});
  std::string raw, pending;
  std::size_t n = 0;
  while (std::getline(f, raw)) {
    ++n;
    if (!raw.empty() && raw.back() == '\r') raw.pop_back();
    const std::string text = trim(raw);
    if (text.rfind("**", 0) == 0) continue;
    if (text.empty()) {
      if (!cards.empty() && pending.empty()) cards.back().raw_lines.push_back("");
      continue;
    }
    if (text[0] != '*' && pending.empty()) {
      if (!cards.empty()) cards.back().lines.push_back(text), cards.back().raw_lines.push_back(text);
      continue;
    }
    if (pending.empty() && !cards.empty())  // 카드가 끝났다: 끝에 붙은 빈 줄은 버린다
      while (!cards.back().raw_lines.empty() && cards.back().raw_lines.back().empty()) cards.back().raw_lines.pop_back();
    // 키워드 줄: 쉼표로 끝나면 다음 줄로 이어진다.
    pending += text;
    if (pending.back() == ',') continue;
    const std::vector<std::string> parts = split(pending.substr(1));
    Card c;
    c.file = shown, c.line = n, c.head = pending;
    pending.clear();
    std::string kw = upper(parts[0]);
    kw.erase(std::unique(kw.begin(), kw.end(), [](char a, char b) { return a == ' ' && b == ' '; }), kw.end());
    c.keyword = kw;
    for (std::size_t i = 1; i < parts.size(); ++i) {
      const std::size_t eq = parts[i].find('=');
      c.params[upper(trim(parts[i].substr(0, eq)))] = eq == std::string::npos ? "" : trim(parts[i].substr(eq + 1));
    }
    if (c.keyword == "INCLUDE") {
      auto it = c.params.find("INPUT");
      if (it == c.params.end()) throw Error("parse_error", "*INCLUDE 에 INPUT 이 없습니다", {{"path", shown}, {"line", n}});
      read_cards(path.parent_path() / fs_path(it->second), cards, depth + 1);
      continue;
    }
    cards.push_back(std::move(c));
  }
  if (!cards.empty())
    while (!cards.back().raw_lines.empty() && cards.back().raw_lines.back().empty()) cards.back().raw_lines.pop_back();
}

[[noreturn]] void bad(const Card& c, const std::string& what, const std::string& text) {
  throw Error("parse_error", "*" + c.keyword + ": " + what + " — \"" + text + "\"", {{"path", c.file}, {"line", c.line}, {"keyword", c.keyword}});
}
std::int64_t to_int(const Card& c, const std::string& s) {
  char* end = nullptr;
  const long long v = std::strtoll(s.c_str(), &end, 10);
  if (s.empty() || *end != '\0') bad(c, "정수가 아닙니다", s);
  return v;
}
double to_double(const Card& c, const std::string& s) {
  if (s.empty()) return 0.0;
  std::string t = s;
  for (char& ch : t)
    if (ch == 'd' || ch == 'D') ch = 'e';  // Fortran 지수 표기(1.0d0)도 받는다
  char* end = nullptr;
  const double v = std::strtod(t.c_str(), &end);
  if (*end != '\0') bad(c, "숫자가 아닙니다", s);
  return v;
}
bool is_number(const std::string& s) { return !s.empty() && (std::isdigit(static_cast<unsigned char>(s[0])) || s[0] == '-' || s[0] == '+'); }

const std::map<std::string, Shape>& shape_of_type() {
  static const std::map<std::string, Shape> m = [] {
    std::map<std::string, Shape> t;
    for (const ShapeInfo& s : all_shapes())
      for (const char* name : s.types) t[name] = s.shape;
    return t;
  }();
  return m;
}

bool is_shell_type(const std::string& t) { return t.rfind("S", 0) == 0 || t.rfind("M3D", 0) == 0; }

// 카드의 매개변수를 읽으면서, 읽지 않은 것이 남았는지 확인한다.
class Params {
 public:
  explicit Params(const Card& c) : c_(c) {}
  bool flag(const char* key) {
    seen_.insert(key);
    return c_.params.count(key) > 0;
  }
  std::optional<std::string> get(const char* key) {
    seen_.insert(key);
    auto it = c_.params.find(key);
    return it == c_.params.end() ? std::nullopt : std::optional<std::string>(it->second);
  }
  std::string text(const char* key) { return upper(get(key).value_or("")); }
  bool yes(const char* key) { return text(key) == "YES"; }
  // 읽지 않은 매개변수가 있으면 그 이름.
  std::string unknown() const {
    for (const auto& [k, v] : c_.params)
      if (!seen_.count(k)) return k;
    return "";
  }
  void require_known() const {
    const std::string u = unknown();
    if (!u.empty()) throw Error("unknown_param", "해석하지 못한 매개변수: " + u, {{"param", u}});
  }

 private:
  const Card& c_;
  std::set<std::string> seen_;
};

class Importer {
 public:
  Importer(App& app, const std::string& path) : a_(app), m_(app.mesh()), path_(path) {
    read_cards(fs_path(path), cards_, 0);
    used_.assign(cards_.size(), 0);
  }

  // ---------------------------------------------------------------- 메시와 셋
  // merge: 번호를 밀지 않고 합친다(이미 있는 노드는 그대로 두고, 요소 번호는 겹치면 안 된다) — 솔버의 세분화 메시처럼 번호를 이어 쓴 파일용
  Json mesh(bool merge = false) {
    node_offset_ = merge ? 0 : m_.max_node_id(), elem_offset_ = merge ? 0 : m_.max_element_id();  // 빈 메시면 0
    std::vector<Id> node_ids;
    std::vector<double> xyz;
    std::vector<Element> elems;
    std::map<std::string, std::vector<Id>> nsets, elsets;  // 대문자 이름 → 번호(파일의 번호)
    std::vector<std::string> nset_order, elset_order, part_order;
    std::map<std::string, std::string> shown;              // 대문자 이름 → 파일에 적힌 이름
    struct Surface {
      std::string name;
      bool node_type;
      std::vector<std::pair<Id, int>> faces;
      std::vector<Id> nodes;
    };
    std::vector<Surface> surfaces;
    std::map<std::string, std::size_t> unsupported_types;
    std::size_t skipped_faces = 0;

    auto remember = [&](std::map<std::string, std::vector<Id>>& sets, std::vector<std::string>& order, const std::string& name) -> std::vector<Id>& {
      const std::string key = upper(name);
      if (!sets.count(key)) order.push_back(key), shown[key] = name;
      return sets[key];
    };
    // 셋의 데이터 줄: 번호 또는 다른 셋의 이름. GENERATE 면 [처음, 끝, 간격].
    auto read_set = [&](const Card& c, const std::map<std::string, std::vector<Id>>& sets, std::vector<Id>& out) {
      const bool generate = c.params.count("GENERATE") > 0;
      for (const std::string& line : c.lines) {
        const std::vector<std::string> cells = split(line);
        if (generate) {
          if (cells.size() < 2) bad(c, "GENERATE 줄에는 처음과 끝이 있어야 합니다", line);
          const std::int64_t first = to_int(c, cells[0]), last = to_int(c, cells[1]);
          const std::int64_t step = cells.size() > 2 && !cells[2].empty() ? to_int(c, cells[2]) : 1;
          if (step <= 0 || first < 1 || last < first) bad(c, "GENERATE 범위가 잘못되었습니다", line);
          for (std::int64_t v = first; v <= last; v += step) out.push_back(static_cast<Id>(v));
          continue;
        }
        for (const std::string& cell : cells) {
          if (cell.empty()) continue;
          if (std::isdigit(static_cast<unsigned char>(cell[0]))) {
            out.push_back(static_cast<Id>(to_int(c, cell)));
          } else {
            auto it = sets.find(upper(cell));
            if (it == sets.end()) bad(c, "앞에 정의되지 않은 셋입니다", cell);
            out.insert(out.end(), it->second.begin(), it->second.end());
          }
        }
      }
    };
    auto all_elsets = [&] {  // 요소 셋은 파트 이름으로도 가리킬 수 있다
      std::map<std::string, std::vector<Id>> known = elsets;
      for (const auto& [k, v] : part_elems_) known.emplace(k, v);
      return known;
    };

    for (std::size_t i = 0; i < cards_.size(); ++i) {
      const Card& c = cards_[i];
      used_[i] = 1;
      if (c.keyword == "NODE") {
        std::vector<Id>* set = c.params.count("NSET") ? &remember(nsets, nset_order, c.params.at("NSET")) : nullptr;
        for (const std::string& line : c.lines) {
          const std::vector<std::string> cells = split(line);
          const std::int64_t id = to_int(c, cells[0]);
          if (id < 1) bad(c, "노드 번호는 1 이상이어야 합니다", line);
          node_ids.push_back(static_cast<Id>(id));
          for (std::size_t k = 1; k <= 3; ++k) xyz.push_back(k < cells.size() ? to_double(c, cells[k]) : 0.0);
          if (set) set->push_back(static_cast<Id>(id));
        }
      } else if (c.keyword == "ELEMENT") {
        auto tp = c.params.find("TYPE");
        if (tp == c.params.end()) bad(c, "TYPE 이 없습니다", "");
        const std::string type = upper(tp->second);
        auto sh = shape_of_type().find(type);
        const std::size_t need = sh == shape_of_type().end() ? 0 : static_cast<std::size_t>(shape_info(sh->second).nodes);
        std::vector<Id>* part = c.params.count("ELSET") ? &remember(part_elems_, part_order, c.params.at("ELSET")) : nullptr;
        std::vector<std::int64_t> row;
        for (const std::string& line : c.lines) {
          for (const std::string& cell : split(line))
            if (!cell.empty()) row.push_back(to_int(c, cell));
          if (sh == shape_of_type().end()) {  // 모르는 타입: 줄이 쉼표로 끝나면 이어진 줄
            if (line.back() != ',') ++unsupported_types[type], row.clear();
            continue;
          }
          if (row.size() < need + 1) continue;  // 다음 줄로 이어진다
          // 번호가 남으면(예: C3D20 연결을 C3D8 로 읽는 배포본 예제) 솔버처럼 앞의 것만 쓴다
          if (row.size() > need + 1) ++extra_connectivity_;
          Element e;
          e.id = static_cast<Id>(row[0]), e.shape = sh->second;
          e.type = type == shape_info(sh->second).default_type ? "" : type;
          for (std::size_t k = 1; k <= need; ++k) e.nodes.push_back(static_cast<Id>(row[k]));
          type_of_[e.id] = type;
          if (part) part->push_back(e.id);
          elems.push_back(std::move(e));
          row.clear();
        }
        if (!row.empty()) bad(c, "요소의 절점이 모자랍니다", c.lines.back());
      } else if (c.keyword == "NSET") {
        if (!c.params.count("NSET")) bad(c, "NSET 이름이 없습니다", "");
        std::vector<Id> ids;
        read_set(c, nsets, ids);
        std::vector<Id>& dst = remember(nsets, nset_order, c.params.at("NSET"));
        dst.insert(dst.end(), ids.begin(), ids.end());
      } else if (c.keyword == "ELSET") {
        if (!c.params.count("ELSET")) bad(c, "ELSET 이름이 없습니다", "");
        std::vector<Id> ids;
        read_set(c, all_elsets(), ids);
        std::vector<Id>& dst = remember(elsets, elset_order, c.params.at("ELSET"));
        dst.insert(dst.end(), ids.begin(), ids.end());
      } else if (c.keyword == "SURFACE") {
        if (!c.params.count("NAME")) bad(c, "NAME 이 없습니다", "");
        Surface s;
        s.name = c.params.at("NAME");
        s.node_type = c.params.count("TYPE") && upper(c.params.at("TYPE")) == "NODE";
        const auto known = all_elsets();
        for (const std::string& line : c.lines) {
          const std::vector<std::string> cells = split(line);
          if (cells[0].empty()) continue;
          const bool number = std::isdigit(static_cast<unsigned char>(cells[0][0])) != 0;
          if (s.node_type) {
            if (number) {
              s.nodes.push_back(static_cast<Id>(to_int(c, cells[0])));
            } else {
              auto it = nsets.find(upper(cells[0]));
              if (it == nsets.end()) bad(c, "앞에 정의되지 않은 노드 셋입니다", cells[0]);
              s.nodes.insert(s.nodes.end(), it->second.begin(), it->second.end());
            }
            continue;
          }
          if (cells.size() < 2) bad(c, "면 라벨이 없습니다", line);
          const std::string label = upper(cells[1]);
          std::vector<Id> owners;
          if (number) {
            owners.push_back(static_cast<Id>(to_int(c, cells[0])));
          } else {
            auto it = known.find(upper(cells[0]));
            if (it == known.end()) bad(c, "앞에 정의되지 않은 요소 셋입니다", cells[0]);
            owners = it->second;
          }
          if (label.size() < 2 || label[0] != 'S' || !std::isdigit(static_cast<unsigned char>(label[1]))) {
            skipped_faces += owners.size();  // SPOS·SNEG 등
            continue;
          }
          const int n = static_cast<int>(to_int(c, label.substr(1)));
          for (Id e : owners) {
            const int face = face_number(c, e, n);
            if (face < 1) ++skipped_faces;
            else s.faces.push_back({e, face});
          }
        }
        surfaces.push_back(std::move(s));
      } else {
        used_[i] = 0;
      }
    }

    // --- 메시에 넣는다(번호가 겹치지 않게 민다. merge 면 이미 있는 노드는 건너뛴다)
    if (node_ids.empty()) throw Error("invalid_state", "파일에 노드가 없습니다", {{"path", path_}});
    std::set<Id> seen;
    for (Id& id : node_ids) {
      if (!seen.insert(id).second) throw Error("parse_error", "노드 번호가 두 번 나옵니다: " + std::to_string(id), {{"path", path_}, {"node", id}});
      id += node_offset_;
    }
    if (merge) {
      // 요소가 쓰는 노드만 넣는다(세분화 파일은 원래 메시의 노드를 전부 다시 적는데, 대체된 요소의 중간 절점처럼 더는 쓰지 않는 것이 섞여 있다)
      std::set<Id> referenced;
      for (const Element& e : elems)
        for (Id n : e.nodes)
          if (n) referenced.insert(n);
      std::vector<Id> fresh_ids;
      std::vector<double> fresh_xyz;
      for (std::size_t i = 0; i < node_ids.size(); ++i) {
        if (m_.has_node(node_ids[i]) || !referenced.count(node_ids[i])) continue;
        fresh_ids.push_back(node_ids[i]);
        fresh_xyz.insert(fresh_xyz.end(), xyz.begin() + static_cast<std::ptrdiff_t>(3 * i), xyz.begin() + static_cast<std::ptrdiff_t>(3 * i + 3));
      }
      m_.add_nodes(fresh_ids, fresh_xyz);
      for (const Element& e : elems)
        if (m_.has_element(e.id)) throw Error("parse_error", "이미 있는 요소 번호입니다: " + std::to_string(e.id), {{"path", path_}, {"element", e.id}});
    } else {
      m_.add_nodes(node_ids, xyz);
    }

    // 파트: *ELEMENT 의 ELSET. 이름이 EALL 인 것은 '전체' 를 뜻하는 관례라 파트로 만들지 않는다.
    Json parts = Json::array(), sets = Json::array();
    std::map<Id, Id> part_of;
    const bool only_eall = part_order.size() == 1 && part_order[0] == "EALL";  // ELSET 이 EALL 뿐이면 그것이 곧 파트다
    for (const std::string& key : part_order) {
      if ((key == "EALL" && !only_eall) || part_elems_[key].empty()) continue;
      const Id pid = a_.invoke("mesh_part.create", Json{{"name", free_name("mesh_part", shown[key])}})["id"].get<Id>();
      for (Id e : part_elems_[key]) part_of[e] = pid;
      parts.push_back(pid);
      part_id_[key] = pid;
    }
    seen.clear();
    for (Element& e : elems) {
      if (!seen.insert(e.id).second) throw Error("parse_error", "요소 번호가 두 번 나옵니다: " + std::to_string(e.id), {{"path", path_}, {"element", e.id}});
      auto p = part_of.find(e.id);
      if (p != part_of.end()) e.part = p->second;
      e.id += elem_offset_;
      for (Id& n : e.nodes)
        if (n != 0) n += node_offset_;
    }
    m_.add_elements(elems);  // 없는 노드를 가리키면 여기서 오류

    auto unique_ids = [](std::vector<Id> v, Id offset) {
      std::sort(v.begin(), v.end());
      v.erase(std::unique(v.begin(), v.end()), v.end());
      for (Id& x : v) x += offset;
      return v;
    };
    auto create_set = [&](const char* command, const std::string& name, Json props) {
      props["name"] = free_name("set", name);
      const Id id = a_.invoke(command, props)["id"].get<Id>();
      sets.push_back(id);
      return id;
    };
    for (const std::string& key : nset_order) {
      nset_nodes_[key] = unique_ids(nsets[key], node_offset_);
      if (key == "NALL" || nsets[key].empty()) continue;
      nset_id_[key] = create_set("set.create_node", shown[key], Json{{"ids", nset_nodes_[key]}});
    }
    for (const std::string& key : elset_order) {
      if (key == "EALL" || elsets[key].empty()) continue;
      std::vector<Id> ids;
      for (Id e : elsets[key])
        if (type_of_.count(e)) ids.push_back(e);  // 읽지 못한 타입의 요소는 뺀다
      if (!ids.empty()) elset_id_[key] = create_set("set.create_element", shown[key], Json{{"ids", unique_ids(ids, elem_offset_)}});
      elset_elems_[key] = elsets[key];
    }
    for (const auto& [key, ids] : part_elems_) elset_elems_.emplace(key, ids);
    for (const Surface& s : surfaces) {
      if (s.node_type) {
        if (!s.nodes.empty()) surface_id_[upper(s.name)] = create_set("set.create_node_surface", s.name, Json{{"ids", unique_ids(s.nodes, node_offset_)}});
      } else if (!s.faces.empty()) {
        Json faces = Json::array();
        for (const auto& [e, f] : s.faces) faces.push_back(Json::array({e + elem_offset_, f}));
        surface_id_[upper(s.name)] = create_set("set.create_surface", s.name, Json{{"faces", faces}});
      }
    }
    Json summary{{"nodes", node_ids.size()}, {"elements", elems.size()}, {"parts", parts}, {"sets", sets},
                 {"node_offset", node_offset_}, {"element_offset", elem_offset_},
                 {"unsupported_types", unsupported_types}, {"skipped_faces", skipped_faces}};
    if (extra_connectivity_) summary["extra_connectivity"] = extra_connectivity_;
    return summary;
  }

  // 메시 단계에서 읽지 않은 카드의 종류별 개수.
  Json ignored() const {
    std::map<std::string, std::size_t> count;
    for (std::size_t i = 0; i < cards_.size(); ++i)
      if (!used_[i] && cards_[i].keyword != "HEADING") ++count[cards_[i].keyword];
    return count;
  }

  // ---------------------------------------------------------------- 모델(재료·섹션·스텝 …)
  Json model() {
    const std::string stem = utf8(fs_path(path_).stem());
    Json case_params{{"name", unique_case_name(stem.empty() ? "case" : stem)}};
    for (std::size_t i = 0; i < cards_.size(); ++i)
      if (cards_[i].keyword == "HEADING" && !cards_[i].lines.empty()) {
        case_params["description"] = cards_[i].lines[0];
        used_[i] = 1;
      }
    case_ = a_.invoke("case.create", case_params)["id"].get<Id>();

    for (std::size_t i = 0; i < cards_.size(); ++i) {
      if (used_[i]) continue;
      const Card& c = cards_[i];
      if (c.keyword == "MATERIAL") {
        i = material(i);
      } else if (c.keyword == "SURFACE INTERACTION") {
        i = interaction(i);
      } else if (c.keyword == "COUPLING" || c.keyword == "TIE") {
        i = grouped(i);
      } else if (c.keyword == "STEP") {
        i = step(i);
      } else {
        handle(i, 0);
      }
    }
    if (!model_bcs_.empty()) {  // 스텝이 하나도 없었다: 모델 수준의 경계조건은 그대로 보존한다
      for (std::size_t i : model_bc_cards_) preserve(case_, i, i, "no_step");
    }
    Json r{{"case", case_}, {"created", created_}, {"preserved", preserved_}};
    if (extra_connectivity_) r["extra_connectivity"] = extra_connectivity_;
    return r;
  }

 private:
  // ------------------------------------------------------------ 공통 도구
  std::string free_name(const std::string& kind, std::string name) const {
    for (char& ch : name)
      if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-' || ch == '.')) ch = '_';
    if (name.empty()) name = "SET";
    if (name.size() > 70) name.resize(70);
    std::set<std::string>& taken = taken_[kind];
    if (!taken_ready_.count(kind)) {
      for (const Object* o : a_.model().by_kind(kind)) taken.insert(upper(o->name));
      taken_ready_.insert(kind);
    }
    std::string out = name;
    for (int n = 2; taken.count(upper(out)); ++n) out = name + "_" + std::to_string(n);
    taken.insert(upper(out));
    return out;
  }
  std::string unique_case_name(const std::string& base) const {
    std::set<std::string> used;
    for (const Object* o : a_.model().by_kind("case")) used.insert(o->name);
    std::string out = base;
    for (int n = 2; used.count(out); ++n) out = base + "-" + std::to_string(n);
    return out;
  }
  // 솔버의 면 라벨 번호 → 면 번호. 쉘의 1, 2 는 아랫면·윗면이라 변은 3 부터다.
  int face_number(const Card& c, Id element, int label) const {
    auto t = type_of_.find(element);
    if (t == type_of_.end()) bad(c, "앞에 정의되지 않은 요소입니다", std::to_string(element));
    return is_shell_type(t->second) ? label - 2 : label;
  }

  Id invoke(const std::string& command, const Json& params) {
    const Json r = a_.invoke(command, params);
    const Id id = r.is_object() && r.contains("id") ? r["id"].get<Id>() : 0;
    if (command.find(".create") != std::string::npos) ++created_[command.substr(0, command.find('.'))];
    return id;
  }
  // 카드 [first, last] 를 그대로 보존한다.
  void preserve(Id parent, std::size_t first, std::size_t last, const std::string& reason) {
    std::string text;
    for (std::size_t i = first; i <= last; ++i) {
      text += cards_[i].head + "\n";
      for (const std::string& line : cards_[i].raw_lines) text += line + "\n";
      used_[i] = 1;
    }
    const Card& c = cards_[first];
    const std::string source = utf8(fs_path(c.file).filename()) + ":" + std::to_string(c.line);
    Json q{{"parent", parent}, {"text", text}, {"source", source}};
    // 케이스 아래에 두는 블록: 스텝(통째로 보존한 스텝 포함)이 이미 나왔으면 그 순서 안에 둔다. 덱의 순서가 바뀌지 않게.
    if (parent == case_ && (in_steps_ || cards_[first].keyword == "STEP")) {
      q["place"] = "steps";
      if (last_step_) q["after"] = last_step_;
      in_steps_ = true;
    }
    const Id id = a_.invoke("deck_block.create", q)["id"].get<Id>();
    preserved_.push_back(Json{{"id", id}, {"keyword", c.keyword}, {"source", source}, {"reason", reason}});
  }

  // 여러 객체를 만드는 카드: 하나라도 실패하면 만든 것을 지우고 카드를 보존한다.
  using Ops = std::vector<std::pair<std::string, Json>>;
  void commit(const Ops& ops) {
    std::vector<std::pair<std::string, Id>> done;
    try {
      for (const auto& [command, params] : ops) {
        const Id id = invoke(command, params);
        if (command.find(".create") != std::string::npos) done.push_back({command.substr(0, command.find('.')), id});  // 수정은 되돌리지 않는다
      }
    } catch (const Error&) {
      for (auto it = done.rbegin(); it != done.rend(); ++it) {
        a_.invoke(it->first + ".delete", Json{{"id", it->second}});
        --created_[it->first];
      }
      throw;
    }
  }

  // --- 이름으로 가리키는 것
  Json elset_target(const std::string& name) const {
    const std::string key = upper(name);
    if (is_number(name)) return Json{{"type", "elements"}, {"ids", Json::array({std::stoll(name)})}};
    if (auto it = part_id_.find(key); it != part_id_.end()) return Json{{"type", "parts"}, {"ids", Json::array({it->second})}};
    if (auto it = elset_id_.find(key); it != elset_id_.end()) return Json{{"type", "set"}, {"ids", Json::array({it->second})}};
    if (key == "EALL" && !part_id_.empty()) {  // 관례: 전체 = 모든 메시 파트
      Json ids = Json::array();
      for (const auto& [k, pid] : part_id_) ids.push_back(pid);
      return Json{{"type", "parts"}, {"ids", ids}};
    }
    throw Error("unknown_set", "정의되지 않은 요소 셋: " + name);
  }
  Json nset_target(const std::string& name) const {
    const std::string key = upper(name);
    if (auto it = nset_id_.find(key); it != nset_id_.end()) return Json{{"type", "set"}, {"ids", Json::array({it->second})}};
    if (key == "NALL" && nset_nodes_.count("NALL")) {  // 관례: 전체 노드. 처음 가리킬 때 셋을 만든다
      const Id id = a_.invoke("set.create_node", Json{{"name", free_name("set", "Nall")}, {"ids", nset_nodes_.at("NALL")}})["id"].get<Id>();
      nset_id_[key] = id;  // mutable
      return Json{{"type", "set"}, {"ids", Json::array({id})}};
    }
    throw Error("unknown_set", "정의되지 않은 노드 셋: " + name);
  }
  std::vector<Id> elements_of(const Card& c, const std::string& name) const {
    if (is_number(name)) return {static_cast<Id>(to_int(c, name))};
    auto it = elset_elems_.find(upper(name));
    if (it == elset_elems_.end()) throw Error("unknown_set", "정의되지 않은 요소 셋: " + name);
    return it->second;
  }
  Id ref(const std::map<std::string, Id>& table, const std::string& name, const char* what) const {
    auto it = table.find(upper(name));
    if (it == table.end()) throw Error("unknown_reference", std::string("정의되지 않은 ") + what + ": " + name);
    return it->second;
  }
  // 하중·경계조건 카드의 공통 매개변수(시간 함수).
  void amplitude(Params& p, Json& props) {
    if (auto v = p.get("AMPLITUDE")) props["amplitude"] = ref(function_id_, *v, "AMPLITUDE");
    if (auto v = p.get("TIME DELAY")) props["time_delay"] = std::strtod(v->c_str(), nullptr);
  }
  static std::vector<double> numbers(const Card& c, const std::string& line) {
    std::vector<std::string> cells = split(line);
    while (!cells.empty() && cells.back().empty()) cells.pop_back();
    std::vector<double> out;
    for (const std::string& cell : cells) out.push_back(to_double(c, cell));
    return out;
  }
  // 데이터 줄을 논리 행으로 묶는다: 상수 width 개(+온도)가 한 줄에 8개씩 이어진다.
  static Json table(const Card& c, int width) {
    const std::size_t per_row = static_cast<std::size_t>((width + 1 + 7) / 8);
    Json rows = Json::array();
    for (std::size_t i = 0; i < c.lines.size(); i += per_row) {
      Json row = Json::array();
      for (std::size_t k = i; k < std::min(i + per_row, c.lines.size()); ++k)
        for (double v : numbers(c, c.lines[k])) row.push_back(v);
      if (!row.empty()) rows.push_back(std::move(row));
    }
    return rows;
  }

  // ------------------------------------------------------------ 재료
  std::size_t material(std::size_t first) {
    static const std::set<std::string> block = {
        "ELASTIC", "DENSITY", "PLASTIC", "CYCLIC HARDENING", "RATE DEPENDENT", "DEFORMATION PLASTICITY", "CREEP", "HYPERELASTIC",
        "HYPERFOAM", "MOHR COULOMB", "MOHR COULOMB HARDENING", "USER MATERIAL", "DEPVAR", "EXPANSION", "CONDUCTIVITY", "SPECIFIC HEAT",
        "DAMPING", "ELECTRICAL CONDUCTIVITY", "MAGNETIC PERMEABILITY", "FLUID CONSTANTS", "SPECIFIC GAS CONSTANT"};
    std::size_t last = first;
    while (last + 1 < cards_.size() && !used_[last + 1] && block.count(cards_[last + 1].keyword)) ++last;
    const Card& head = cards_[first];
    const std::string name = head.params.count("NAME") ? head.params.at("NAME") : "";
    Id id = 0;
    try {
      Params hp(head);
      hp.get("NAME");
      hp.require_known();
      if (name.empty()) throw Error("missing_param", "NAME 이 없습니다");
      id = invoke("material.create", Json{{"name", free_name("material", name)}});
      for (std::size_t i = first + 1; i <= last; ++i) behavior(id, cards_[i]);
    } catch (const Error& e) {
      if (id) a_.invoke("material.delete", Json{{"id", id}}), --created_["material"];
      preserve(case_, first, last, e.code());
      raw_materials_.insert(upper(name));
      return last;
    }
    for (std::size_t i = first; i <= last; ++i) used_[i] = 1;
    material_id_[upper(name)] = id;
    return last;
  }

  void behavior(Id mat, const Card& c) {
    static const std::map<std::string, const char*> elastic = {
        {"", "iso"}, {"ISO", "iso"}, {"ORTHO", "ortho"}, {"ENGINEERING CONSTANTS", "engineering_constants"}, {"ANISO", "aniso"}};
    static const std::map<std::string, const char*> hardening = {
        {"", "isotropic"}, {"ISOTROPIC", "isotropic"}, {"KINEMATIC", "kinematic"}, {"COMBINED", "combined"}};
    static const std::map<std::string, const char*> tensor = {{"", "iso"}, {"ISO", "iso"}, {"ORTHO", "ortho"}, {"ANISO", "aniso"}};
    static const std::map<std::string, std::pair<const char*, int>> hyper = {  // 모델, N=1 일 때 상수 개수(0 = N 에 따라)
        {"ARRUDA-BOYCE", {"arruda_boyce", 3}}, {"MOONEY-RIVLIN", {"mooney_rivlin", 3}}, {"NEO HOOKE", {"neo_hooke", 2}},
        {"OGDEN", {"ogden", 0}}, {"POLYNOMIAL", {"polynomial", 0}}, {"REDUCED POLYNOMIAL", {"reduced_polynomial", 0}}, {"YEOH", {"yeoh", 6}}};
    Params p(c);
    Json q{{"id", mat}};
    std::string command;
    auto pick = [&](const std::map<std::string, const char*>& names, const char* key) {
      auto it = names.find(p.text(key));
      if (it == names.end()) throw Error("unknown_param", std::string("해석하지 못한 값: ") + key);
      return std::string(it->second);
    };
    const std::string& k = c.keyword;
    if (k == "ELASTIC") {
      const std::string t = pick(elastic, "TYPE");
      command = "set_elastic", q["type"] = t, q["data"] = table(c, t == "iso" ? 2 : t == "aniso" ? 21 : 9);
    } else if (k == "DENSITY") {
      command = "set_density", q["data"] = table(c, 1);
    } else if (k == "PLASTIC") {
      command = "set_plastic", q["hardening"] = pick(hardening, "HARDENING"), q["data"] = table(c, 2);
    } else if (k == "CYCLIC HARDENING") {
      command = "set_cyclic_hardening", q["data"] = table(c, 2);
    } else if (k == "DEFORMATION PLASTICITY") {
      command = "set_deformation_plasticity", q["data"] = table(c, 5);
    } else if (k == "CREEP") {
      const std::string law = p.text("LAW");
      if (!law.empty() && law != "NORTON") throw Error("unknown_param", "해석하지 못한 값: LAW");
      command = "set_creep", q["data"] = table(c, 3);
    } else if (k == "HYPERELASTIC") {
      const int n = p.get("N") ? std::atoi(c.params.at("N").c_str()) : 1;
      for (const auto& [flag, model] : hyper) {
        if (!p.flag(flag.c_str())) continue;
        const std::string name = model.first;
        const int width = model.second ? model.second : name == "ogden" ? 3 * n : name == "reduced_polynomial" ? 2 * n : n == 1 ? 3 : n == 2 ? 7 : 12;
        command = "set_hyperelastic", q["model"] = name, q["data"] = table(c, width);
        if (model.second == 0) q["n"] = n;
      }
      if (command.empty()) throw Error("unknown_param", "초탄성 모델을 알 수 없습니다");
    } else if (k == "EXPANSION" || k == "CONDUCTIVITY") {
      const std::string t = pick(tensor, "TYPE");
      command = k == "EXPANSION" ? "set_expansion" : "set_conductivity";
      q["type"] = t, q["data"] = table(c, t == "iso" ? 1 : t == "ortho" ? 3 : 6);
      if (k == "EXPANSION")
        if (auto z = p.get("ZERO")) q["zero"] = std::strtod(z->c_str(), nullptr);
    } else if (k == "SPECIFIC HEAT") {
      command = "set_specific_heat", q["data"] = table(c, 1);
    } else if (k == "FLUID CONSTANTS") {  // 정압 비열, 점성(, 온도)
      command = "set_fluid_constants", q["data"] = table(c, 2);
    } else if (k == "ELECTRICAL CONDUCTIVITY") {
      command = "set_electrical_conductivity", q["data"] = table(c, 1);
    } else if (k == "MAGNETIC PERMEABILITY") {  // 투자율, 영역 번호(, 온도)
      command = "set_magnetic_permeability", q["data"] = table(c, 2);
    } else if (k == "SPECIFIC GAS CONSTANT") {
      if (c.lines.empty()) throw Error("missing_param", "기체 상수가 없습니다");
      command = "set_specific_gas_constant", q["value"] = to_double(c, split(c.lines[0])[0]);
    } else if (k == "USER MATERIAL") {
      const auto n = p.get("CONSTANTS");
      if (!n) throw Error("missing_param", "CONSTANTS 가 없습니다");
      const std::string type = p.text("TYPE");
      if (!type.empty() && type != "MECHANICAL" && type != "THERMAL") throw Error("unknown_param", "해석하지 못한 값: TYPE");
      command = "set_user", q["constants"] = std::atoi(n->c_str()), q["data"] = table(c, std::atoi(n->c_str()));
      if (type == "THERMAL") q["type"] = "thermal";
    } else if (k == "DEPVAR") {
      if (c.lines.empty()) throw Error("missing_param", "상태변수 개수가 없습니다");
      command = "set_depvar", q["count"] = to_int(c, split(c.lines[0])[0]);
    } else if (k == "DAMPING" && c.params.count("STRUCTURAL")) {
      command = "set_structural_damping", q["value"] = std::strtod(p.get("STRUCTURAL")->c_str(), nullptr);
    } else {
      throw Error("material_behavior", "아직 읽지 못하는 구성 모델: " + k);
    }
    p.require_known();
    a_.invoke("material." + command, q);
  }

  // ------------------------------------------------------------ 카드 하나 (스텝 밖이면 step = 0)
  void handle(std::size_t i, Id step) {
    const Card& c = cards_[i];
    try {
      Params p(c);
      Ops ops;
      const std::string& k = c.keyword;
      if (k == "BOUNDARY") {
        ops = boundary(c, p, step);
        if (!step) {  // 모델 수준의 경계조건은 첫 스텝에 둔다(앞 스텝의 것은 뒤 스텝으로 이어지므로 같은 뜻이다)
          p.require_known();
          model_bcs_.insert(model_bcs_.end(), ops.begin(), ops.end());
          model_bc_cards_.push_back(i);
          return;
        }
      } else if (k == "ORIENTATION" && !step) {
        ops = orientation(c, p);
      } else if (k == "AMPLITUDE" && !step) {
        ops = amplitude_card(c, p);
      } else if (k == "TIME POINTS" && !step) {
        ops = time_points(c, p);
      } else if (k == "TRANSFORM" && !step) {
        ops = transform(c, p);
      } else if (k == "VALUES AT INFINITY" && !step) {  // 3D 유체(매뉴얼 7.139)
        if (c.lines.empty()) throw Error("missing_param", "값이 없습니다");
        const std::vector<double> v = numbers(c, c.lines[0]);
        if (v.size() != 5) bad(c, "값 5개(정온도, 속도, 정압, 밀도, 영역 길이)여야 합니다", c.lines[0]);
        ops = {{"case.update", Json{{"id", case_}, {"values_at_infinity", v}}}};
      } else if ((k == "DESIGN VARIABLES" || k == "DESIGNVARIABLES") && !step) {  // 민감도 해석의 설계 변수(매뉴얼 7.34)
        const std::string t = p.text("TYPE");
        if (t != "COORDINATE" && t != "ORIENTATION") throw Error("unknown_param", "해석하지 못한 값: TYPE");
        Json q{{"id", case_}, {"design_variable_type", t == "COORDINATE" ? "coordinate" : "orientation"}};
        if (t == "COORDINATE") {
          if (c.lines.empty()) throw Error("missing_param", "설계 노드 셋이 없습니다");
          q["design_nodes"] = nset_target(split(c.lines[0])[0]);
        }
        ops = {{"case.update", q}};
      } else if (k == "PHYSICAL CONSTANTS" && !step) {
        Json q;
        if (auto v = p.get("ABSOLUTE ZERO")) q["absolute_zero"] = std::strtod(v->c_str(), nullptr);
        if (auto v = p.get("STEFAN BOLTZMANN")) q["stefan_boltzmann"] = std::strtod(v->c_str(), nullptr);
        if (auto v = p.get("NEWTON GRAVITY")) q["newton_gravity"] = std::strtod(v->c_str(), nullptr);
        ops = {{"physical_constants.set", q}};
      } else if ((k == "SOLID SECTION" || k == "SHELL SECTION" || k == "MEMBRANE SECTION" || k == "BEAM SECTION" || k == "MASS" || k == "SPRING" ||
                  k == "DASHPOT" || k == "GAP" || k == "FLUID SECTION" || k == "USER SECTION") && !step) {
        ops = section(c, p);
      } else if ((k == "EQUATION" || k == "MPC" || k == "RIGID BODY" || k == "CONTACT PAIR" || k == "CLEARANCE") && !step) {
        ops = constraint(c, p);
      } else if (k == "INITIAL CONDITIONS" && !step) {
        ops = initial_condition(c, p);
      } else if (step && (k == "CLOAD" || k == "DLOAD" || k == "TEMPERATURE" || k == "CFLUX" || k == "DFLUX" || k == "FILM" || k == "RADIATE")) {
        ops = load(c, p, step);
      } else if (step && k == "BASE MOTION") {
        Json q{{"parent", step}};
        amplitude(p, q);
        if (!q.contains("amplitude")) throw Error("missing_param", "AMPLITUDE 가 없습니다");
        const auto dof = p.get("DOF");
        if (!dof) throw Error("missing_param", "DOF 가 없습니다");
        q["dof"] = to_int(c, *dof);
        if (auto t = p.get("TYPE")) q["motion"] = upper(*t) == "ACCELERATION" ? "acceleration" : "displacement";
        ops = {{"bc.create_base_motion", q}};
      } else if (step && (k == "NODE FILE" || k == "EL FILE" || k == "NODE PRINT" || k == "EL PRINT" || k == "CONTACT FILE" ||
                          k == "CONTACT PRINT" || k == "SECTION PRINT")) {
        ops = output(c, p, step);
      } else if (step && (k == "MODEL CHANGE" || k == "CHANGE FRICTION" || k == "CHANGE SURFACE BEHAVIOR" || k == "CHANGE MATERIAL" ||
                          k == "CHANGE SOLID SECTION")) {
        ops = change(i, p, step);
      } else if (step && k == "CONTROLS") {
        ops = controls(c, p, step);
      } else if (step && k == "RESTART" && p.flag("WRITE")) {  // *RESTART, WRITE(CAS-38). READ 는 스텝 앞의 카드라 보존된다
        Json q{{"id", step}, {"restart_write", true}};
        if (auto v = p.get("FREQUENCY")) q["restart_frequency"] = static_cast<int>(to_double(c, *v));
        if (p.flag("OVERLAY")) q["restart_overlay"] = true;
        ops = {{"step.update", q}};
      } else if (step && k == "MODAL DAMPING") {
        const bool rayleigh = p.flag("RAYLEIGH");
        Json rows = Json::array();
        for (const std::string& line : c.lines) {
          const std::vector<std::string> cells = split(line);
          if (rayleigh) {
            if (cells.size() < 4) bad(c, "α, β 가 없습니다", line);
            rows.push_back(Json::array({to_double(c, cells[2]), to_double(c, cells[3])}));
          } else {
            if (cells.size() < 3) bad(c, "첫 모드, 끝 모드, 감쇠비가 있어야 합니다", line);
            rows.push_back(Json::array({to_double(c, cells[0]), to_double(c, cells[1]), to_double(c, cells[2])}));
          }
        }
        ops = {{"step.update", Json{{"id", step}, {"damping_type", rayleigh ? "rayleigh" : "direct"}, {"modal_damping", rows}}}};
      } else {
        throw Error("unknown_keyword", "아직 읽지 못하는 카드: " + k);
      }
      p.require_known();
      commit(ops);
      used_[i] = 1;
      // 이름으로 가리킬 수 있게 기억한다
      if (k == "ORIENTATION") orientation_id_[upper(c.params.at("NAME"))] = last_created("orientation");
      if (k == "AMPLITUDE") function_id_[upper(c.params.at("NAME"))] = last_created("function");
      if (k == "TIME POINTS") time_points_id_[upper(c.params.at("NAME"))] = last_created("time_points");
      if (k == "CONTACT PAIR") pair_id_[last_pair_] = last_created("contact_pair");
      if (k == "TRANSFORM") {
        const Id cs = last_created("csys");
        for (Id n : transform_nodes_) csys_of_node_[n] = cs;
      }
      if (k == "CHANGE MATERIAL" || k == "CHANGE FRICTION" || k == "CHANGE SURFACE BEHAVIOR") used_[i + 1] = 1;  // 뒤따르는 카드까지 읽었다
    } catch (const Error& e) {
      preserve(step ? step : case_, i, i, e.code());
    }
  }

  // *TRANSFORM: 노드 셋에 국부 좌표계를 건다. 직교(R): a 는 1축 방향, b 는 1-2 평면 위(전역 원점 기준 벡터).
  // 원통(C): a, b 는 축 위의 두 점. 모델의 좌표계(원점·1축 점·1-2 평면 점)로 바꾼다.
  Ops transform(const Card& c, Params& p) {
    const auto set = p.get("NSET");
    if (!set) throw Error("missing_param", "NSET 이 없습니다");
    const std::string type = p.text("TYPE");
    if (!type.empty() && type != "R" && type != "C") throw Error("unknown_param", "해석하지 못한 값: TYPE");
    if (c.lines.empty()) throw Error("missing_param", "좌표가 없습니다");
    const std::vector<double> v = numbers(c, c.lines[0]);
    if (v.size() != 6) bad(c, "좌표 6개가 있어야 합니다", c.lines[0]);
    auto it = nset_nodes_.find(upper(*set));
    if (it == nset_nodes_.end()) throw Error("unknown_set", "정의되지 않은 노드 셋: " + *set);
    for (Id n : it->second)
      if (csys_of_node_.count(n)) throw Error("transform_conflict", "노드 " + std::to_string(n) + " 에 이미 좌표 변환이 있습니다");
    transform_nodes_ = it->second;
    Json q{{"name", free_name("csys", "T_" + *set)}};
    if (type == "C") {
      // 축 방향 d = b - a. 1축 점은 d 에 수직인 아무 방향, 1-2 평면 점은 d × (그 방향) 쪽으로 둔다.
      const double d[3] = {v[3] - v[0], v[4] - v[1], v[5] - v[2]};
      const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
      if (len <= 0.0) bad(c, "축의 두 점이 같습니다", c.lines[0]);
      const int k = std::fabs(d[0]) <= std::fabs(d[1]) && std::fabs(d[0]) <= std::fabs(d[2]) ? 0 : std::fabs(d[1]) <= std::fabs(d[2]) ? 1 : 2;
      double e[3] = {0.0, 0.0, 0.0};
      e[k] = 1.0;
      double u[3] = {e[1] * d[2] - e[2] * d[1], e[2] * d[0] - e[0] * d[2], e[0] * d[1] - e[1] * d[0]};  // e × d ⟂ d
      const double ul = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
      for (double& x : u) x /= ul;
      const double w[3] = {d[1] * u[2] - d[2] * u[1], d[2] * u[0] - d[0] * u[2], d[0] * u[1] - d[1] * u[0]};  // d × u → u × w ∥ d
      q["origin"] = {v[0], v[1], v[2]};
      q["axis1_point"] = {v[0] + u[0], v[1] + u[1], v[2] + u[2]};
      q["plane12_point"] = {v[0] + w[0] / len, v[1] + w[1] / len, v[2] + w[2] / len};
      return {{"csys.create_cylindrical", q}};
    }
    q["origin"] = {0.0, 0.0, 0.0}, q["axis1_point"] = {v[0], v[1], v[2]}, q["plane12_point"] = {v[3], v[4], v[5]};
    return {{"csys.create_rectangular", q}};
  }
  // 대상 노드에 걸린 좌표 변환. 모두 같은 좌표계면 그 번호, 하나도 없으면 0, 섞여 있으면 오류.
  Id csys_of_target(const Json& target) const {
    if (csys_of_node_.empty()) return 0;
    std::vector<Id> nodes;
    if (target["type"] == "nodes") {
      for (const Json& n : target["ids"]) nodes.push_back(n.get<Id>());
    } else {
      const Object& s = a_.model().get(target["ids"][0].get<Id>());
      for (const Json& n : s.props.value("ids", Json::array())) nodes.push_back(n.get<Id>());
    }
    Id cs = 0;
    bool any_plain = false;
    for (Id n : nodes) {
      auto it = csys_of_node_.find(n);
      if (it == csys_of_node_.end()) any_plain = true;
      else if (cs && it->second != cs) throw Error("transform_conflict", "한 카드의 노드들에 서로 다른 좌표 변환이 걸려 있습니다");
      else cs = it->second;
    }
    if (cs && any_plain) throw Error("transform_conflict", "한 카드의 노드 일부에만 좌표 변환이 걸려 있습니다");
    return cs;
  }
  // 노드 자유도에 걸리는 경계조건·집중 하중에 좌표 변환을 반영한다.
  void apply_transform(Ops& ops) const {
    for (auto& [command, params] : ops) {
      if (!params.contains("target")) continue;
      // 좌표계를 받는 종류(변위·힘·모멘트 등)에만 붙인다. 스칼라(온도·압력·전위)에는 변환이 뜻이 없다
      const std::size_t dot = command.find('.');
      const std::string kind = command.substr(0, dot), action = command.substr(dot + 1);
      const std::string sub = action.rfind("create_", 0) == 0 ? action.substr(7) : "";
      const KindSpec* ks = a_.schema().find(kind);
      if (!ks) continue;
      bool accepts = false;
      for (const FieldSpec& f : ks->fields_for(sub)) accepts = accepts || f.name == "csys";
      if (!accepts) continue;
      if (const Id cs = csys_of_target(params["target"])) params["csys"] = cs;
    }
  }

  Ops initial_condition(const Card& c, Params& p) {
    const std::string type = p.text("TYPE");
    Ops ops;
    if (type == "TEMPERATURE") {
      for (const std::string& line : c.lines) {
        const std::vector<std::string> cells = split(line);
        if (cells.size() < 2) bad(c, "값이 없습니다", line);
        Json q{{"target", node_target(c, cells[0])}, {"value", to_double(c, cells[1])}};
        if (cells.size() > 2 && !cells[2].empty()) q["gradient"] = to_double(c, cells[2]);
        ops.push_back({"initial_condition.create_temperature", q});
      }
    } else if (type == "VELOCITY" || type == "DISPLACEMENT" || type == "FLUID VELOCITY") {
      // "노드(셋), 자유도, 값" 을 대상별로 모아 성분 벡터로 만든다.
      const std::string command = type == "VELOCITY" ? "initial_condition.create_velocity"
                                  : type == "DISPLACEMENT" ? "initial_condition.create_displacement" : "initial_condition.create_fluid_velocity";
      std::vector<std::string> order;
      std::map<std::string, std::array<double, 3>> comps;
      for (const std::string& line : c.lines) {
        const std::vector<std::string> cells = split(line);
        if (cells.size() < 3) bad(c, "자유도와 값이 있어야 합니다", line);
        const int dof = static_cast<int>(to_int(c, cells[1]));
        if (dof < 1 || dof > 3) throw Error("unknown_dof", "아직 읽지 못하는 자유도: " + cells[1]);
        if (!comps.count(cells[0])) order.push_back(cells[0]), comps[cells[0]] = {};
        comps[cells[0]][static_cast<std::size_t>(dof - 1)] = to_double(c, cells[2]);
      }
      for (const std::string& name : order) {
        const auto& v = comps[name];
        ops.push_back({command, Json{{"target", node_target(c, name)}, {"components", {v[0], v[1], v[2]}}}});
      }
    } else if (type == "MASS FLOW" || type == "TOTAL PRESSURE" || type == "PRESSURE") {
      const std::string command = type == "MASS FLOW" ? "initial_condition.create_mass_flow"
                                  : type == "TOTAL PRESSURE" ? "initial_condition.create_total_pressure" : "initial_condition.create_pressure";
      for (const std::string& line : c.lines) {
        const std::vector<std::string> cells = split(line);
        if (cells.size() < 2) bad(c, "값이 없습니다", line);
        ops.push_back({command, Json{{"target", node_target(c, cells[0])}, {"value", to_double(c, cells[1])}}});
      }
    } else {
      throw Error("unknown_param", "아직 읽지 못하는 초기 조건: " + type);
    }
    return ops;
  }

  // *CONTROLS: PARAMETERS 별 데이터 줄을 그대로 스텝에 둔다(RESET 은 controls_reset).
  Ops controls(const Card& c, Params& p, Id step) {
    static const std::map<std::string, const char*> names = {{"TIME INCREMENTATION", "time_incrementation"}, {"FIELD", "field"},
                                                             {"LINE SEARCH", "line_search"}, {"NETWORK", "network"}, {"CFD", "cfd"},
                                                             {"CONTACT", "contact"}};
    Json q{{"id", step}};
    if (p.flag("RESET")) {
      q["controls_reset"] = true;
      return {{"step.set_controls", q}};
    }
    auto it = names.find(p.text("PARAMETERS"));
    if (it == names.end()) throw Error("unknown_param", "해석하지 못한 값: PARAMETERS");
    Json rows = Json::array();
    if (it->first == "TIME INCREMENTATION") {
      // 논리 행이 정수 10개, 실수 7개라 8개씩 줄바꿈된다. 한 행이 다 찰 때까지 줄을 잇는다.
      const std::size_t widths[] = {10, 7};
      std::size_t w = 0, i = 0;
      while (i < c.lines.size()) {
        Json row = Json::array();
        const std::size_t width = w < 2 ? widths[w] : 8;
        while (i < c.lines.size() && row.size() < width) {
          for (double v : numbers(c, c.lines[i])) row.push_back(v);
          ++i;
        }
        rows.push_back(row);
        ++w;
      }
    } else {
      for (const std::string& line : c.lines) rows.push_back(numbers(c, line));
    }
    Json list = a_.model().get(step).props.value("controls", Json::array());
    list.push_back(Json{{"parameters", it->second}, {"values", rows}});
    q["controls"] = list;
    return {{"step.set_controls", q}};
  }

  // 스텝 중 변경: *MODEL CHANGE, *CHANGE FRICTION, *CHANGE SURFACE BEHAVIOR, *CHANGE MATERIAL(+*CHANGE PLASTIC), *CHANGE SOLID SECTION
  Ops change(std::size_t i, Params& p, Id step) {
    const Card& c = cards_[i];
    const std::string& k = c.keyword;
    Json q{{"parent", step}};
    if (k == "MODEL CHANGE") {
      const std::string type = p.text("TYPE");
      if (!p.flag("ADD") && !p.flag("REMOVE")) throw Error("missing_param", "ADD 또는 REMOVE 가 없습니다");
      q["action"] = p.flag("ADD") ? "add" : "remove";
      if (c.lines.empty()) throw Error("missing_param", "대상이 없습니다");
      const std::vector<std::string> cells = split(c.lines[0]);
      if (type == "ELEMENT") {
        q["target"] = elset_target(cells[0]);
        return {{"step_change.create_model_change_element", q}};
      }
      if (type != "CONTACT PAIR") throw Error("unknown_param", "해석하지 못한 값: TYPE");
      if (cells.size() < 2) bad(c, "종속 면과 주 면이 있어야 합니다", c.lines[0]);
      q["pair"] = pair_of(cells[0], cells[1]);
      return {{"step_change.create_model_change_contact", q}};
    }
    // 뒤따르는 카드(*FRICTION, *SURFACE BEHAVIOR, *CHANGE PLASTIC)에 내용이 있다.
    auto follower = [&](const char* keyword) -> const Card& {
      if (i + 1 >= cards_.size() || cards_[i + 1].keyword != keyword) throw Error("missing_card", std::string("*") + keyword + " 이 뒤따라야 합니다");
      return cards_[i + 1];
    };
    if (k == "CHANGE FRICTION" || k == "CHANGE SURFACE BEHAVIOR") {
      const auto name = p.get("INTERACTION");
      if (!name) throw Error("missing_param", "INTERACTION 이 없습니다");
      q["interaction"] = ref(interaction_id_, *name, "접촉 속성");
      if (k == "CHANGE FRICTION") {
        const Card& fc = follower("FRICTION");
        Params fp(fc);
        friction(fc, q);
        fp.require_known();
        return {{"step_change.create_change_friction", q}};
      }
      const Card& sc = follower("SURFACE BEHAVIOR");
      Params sp(sc);
      surface_behavior(sc, sp, q);
      sp.require_known();
      return {{"step_change.create_change_surface_behavior", q}};
    }
    if (k == "CHANGE MATERIAL") {
      const auto name = p.get("NAME");
      if (!name) throw Error("missing_param", "NAME 이 없습니다");
      q["material"] = ref(material_id_, *name, "재료");
      const Card& pc = follower("CHANGE PLASTIC");
      Params pp(pc);
      const std::string h = pp.text("HARDENING");
      if (!h.empty() && h != "ISOTROPIC" && h != "KINEMATIC") throw Error("unknown_param", "해석하지 못한 값: HARDENING");
      pp.require_known();
      q["hardening"] = h == "KINEMATIC" ? "kinematic" : "isotropic";
      q["data"] = table(pc, 2);
      return {{"step_change.create_change_material", q}};
    }
    // CHANGE SOLID SECTION
    const auto elset = p.get("ELSET"), material = p.get("MATERIAL");
    if (!elset || !material) throw Error("missing_param", "ELSET 과 MATERIAL 이 있어야 합니다");
    q["target"] = elset_target(*elset), q["material"] = ref(material_id_, *material, "재료");
    if (auto o = p.get("ORIENTATION")) q["orientation"] = ref(orientation_id_, *o, "방향");
    return {{"step_change.create_change_section", q}};
  }
  Id pair_of(const std::string& slave, const std::string& master) const {
    auto s = surface_id_.find(upper(slave)), m = surface_id_.find(upper(master));
    if (s == surface_id_.end() || m == surface_id_.end()) throw Error("unknown_set", "정의되지 않은 면: " + slave + ", " + master);
    auto it = pair_id_.find({s->second, m->second});
    if (it == pair_id_.end()) throw Error("unknown_reference", "정의되지 않은 접촉 쌍: " + slave + ", " + master);
    return it->second;
  }
  Id last_created(const std::string& kind) const { return a_.model().by_kind(kind).empty() ? 0 : newest(kind); }
  Id newest(const std::string& kind) const {
    Id id = 0;
    for (const Object* o : a_.model().by_kind(kind)) id = std::max(id, o->id);
    return id;
  }

  Json node_target(const Card& c, const std::string& cell) {
    if (is_number(cell)) return Json{{"type", "nodes"}, {"ids", Json::array({to_int(c, cell)})}};
    return nset_target(cell);
  }

  Json surface_target(const std::string& name, bool nodes_ok = false) const {
    if (auto it = surface_id_.find(upper(name)); it != surface_id_.end()) return Json{{"type", "set"}, {"ids", Json::array({it->second})}};
    if (nodes_ok)
      if (auto it = nset_id_.find(upper(name)); it != nset_id_.end()) return Json{{"type", "set"}, {"ids", Json::array({it->second})}};
    throw Error("unknown_set", "정의되지 않은 면: " + name);
  }

  // ------------------------------------------------------------ 구속·접촉 쌍
  std::pair<Id, Id> last_pair_;
  Ops constraint(const Card& c, Params& p) {
    const std::string& k = c.keyword;
    Ops ops;
    if (k == "EQUATION") {
      // 묶음마다: 항 수 한 줄, 그 뒤 [노드, 자유도, 계수] 가 한 줄에 4항씩
      std::size_t i = 0;
      while (i < c.lines.size()) {
        const int n = static_cast<int>(to_int(c, split(c.lines[i])[0]));
        if (n < 1) bad(c, "항 수가 잘못되었습니다", c.lines[i]);
        ++i;
        // 항은 [노드 또는 노드 셋, 자유도, 계수]. 셋으로 적은 항은 하나까지만 읽고, 그 셋의 노드마다 구속식을 하나씩 만든다.
        std::vector<std::string> cells;
        while (static_cast<int>(cells.size()) < 3 * n && i < c.lines.size()) {
          for (const std::string& cell : split(c.lines[i]))
            if (!cell.empty()) cells.push_back(cell);
          ++i;
        }
        if (static_cast<int>(cells.size()) != 3 * n) bad(c, "구속식의 항이 모자랍니다", c.lines[i - 1]);
        Json terms = Json::array();
        int set_term = -1;
        for (int t = 0; t < n; ++t) {
          const std::string& node = cells[static_cast<std::size_t>(3 * t)];
          if (!is_number(node)) {
            if (set_term >= 0) throw Error("unknown_param", "노드 셋으로 적은 항이 둘 이상인 구속식은 아직 읽지 못합니다");
            set_term = t;
          }
          terms.push_back(Json::array({is_number(node) ? static_cast<Id>(to_int(c, node)) : 0, static_cast<int>(to_int(c, cells[static_cast<std::size_t>(3 * t + 1)])),
                                       to_double(c, cells[static_cast<std::size_t>(3 * t + 2)])}));
        }
        if (set_term < 0) {
          ops.push_back({"constraint.create_equation", Json{{"terms", terms}}});
          continue;
        }
        auto it = nset_nodes_.find(upper(cells[static_cast<std::size_t>(3 * set_term)]));
        if (it == nset_nodes_.end()) throw Error("unknown_set", "정의되지 않은 노드 셋: " + cells[static_cast<std::size_t>(3 * set_term)]);
        for (Id node : it->second) {
          Json one = terms;
          one[static_cast<std::size_t>(set_term)][0] = node;
          ops.push_back({"constraint.create_equation", Json{{"terms", one}}});
        }
      }
    } else if (k == "MPC") {
      static const std::map<std::string, const char*> kinds = {
          {"PLANE", "mpc_plane"}, {"STRAIGHT", "mpc_straight"}, {"BEAM", "mpc_beam"}, {"MEANROT", "mpc_meanrot"}, {"DIST", "mpc_dist"}};
      for (const std::string& line : c.lines) {
        const std::vector<std::string> cells = split(line);
        auto it = kinds.find(upper(cells[0]));
        if (it == kinds.end()) throw Error("unknown_param", "아직 읽지 못하는 MPC: " + cells[0]);
        std::vector<Id> nodes;
        for (std::size_t i = 1; i < cells.size(); ++i)
          if (!cells[i].empty()) nodes.push_back(static_cast<Id>(to_int(c, cells[i])));
        Json q;
        if (it->first == "MEANROT" || it->first == "DIST") {
          if (nodes.size() < 2) bad(c, "노드가 모자랍니다", line);
          q["pilot_node"] = nodes.back();
          nodes.pop_back();
        }
        q["nodes"] = nodes;
        ops.push_back({std::string("constraint.create_") + it->second, q});
      }
    } else if (k == "RIGID BODY") {
      Json q;
      if (auto s = p.get("NSET")) q["target"] = nset_target(*s);
      else if (auto s2 = p.get("ELSET")) q["target"] = elset_target(*s2);
      else throw Error("missing_param", "NSET 또는 ELSET 이 필요합니다");
      if (auto v = p.get("REF NODE")) q["ref_node"] = to_int(c, *v);
      if (auto v = p.get("ROT NODE")) q["rot_node"] = to_int(c, *v);
      ops.push_back({"constraint.create_rigid_body", q});
    } else if (k == "CONTACT PAIR") {
      static const std::map<std::string, const char*> types = {
          {"NODE TO SURFACE", "node_to_surface"}, {"SURFACE TO SURFACE", "surface_to_surface"}, {"MORTAR", "mortar"}, {"MASSLESS", "massless"}};
      const auto inter = p.get("INTERACTION");
      if (!inter) throw Error("missing_param", "INTERACTION 이 없습니다");
      if (c.lines.empty()) throw Error("missing_param", "종속 면과 주 면이 없습니다");
      const std::vector<std::string> cells = split(c.lines[0]);
      if (cells.size() < 2) bad(c, "종속 면과 주 면이 있어야 합니다", c.lines[0]);
      Json q{{"slave", surface_target(cells[0], true)}, {"master", surface_target(cells[1])}, {"interaction", ref(interaction_id_, *inter, "접촉 속성")}};
      if (p.flag("SMALL SLIDING")) q["small_sliding"] = true;
      if (auto v = p.get("ADJUST")) q["adjust"] = std::strtod(v->c_str(), nullptr);
      if (auto t = p.get("TYPE")) {
        auto it = types.find(upper(*t));
        if (it == types.end()) throw Error("unknown_param", "해석하지 못한 값: TYPE");
        a_.invoke("case.update", Json{{"id", case_}, {"contact_method", it->second}});
      }
      last_pair_ = {q["slave"]["ids"][0].get<Id>(), q["master"]["ids"][0].get<Id>()};
      ops.push_back({"contact_pair.create", q});
    } else {  // CLEARANCE: 앞서 만든 접촉 쌍에 붙인다
      const auto s = p.get("SLAVE"), m = p.get("MASTER"), v = p.get("VALUE");
      if (!s || !m || !v) throw Error("missing_param", "SLAVE, MASTER, VALUE 가 필요합니다");
      auto it = pair_id_.find({surface_target(*s, true)["ids"][0].get<Id>(), surface_target(*m)["ids"][0].get<Id>()});
      if (it == pair_id_.end()) throw Error("unknown_reference", "앞서 정의한 접촉 쌍이 없습니다");
      ops.push_back({"contact_pair.update", Json{{"id", it->second}, {"clearance", std::strtod(v->c_str(), nullptr)}}});
    }
    return ops;
  }

  // *COUPLING + *KINEMATIC|*DISTRIBUTING, *TIE [+ *CYCLIC SYMMETRY MODEL]: 두 카드가 한 구속이다
  std::size_t grouped(std::size_t first) {
    const Card& c = cards_[first];
    std::size_t last = first;
    try {
      Params p(c);
      Json q;
      std::string command;
      if (c.keyword == "COUPLING") {
        if (last + 1 >= cards_.size() || (cards_[last + 1].keyword != "KINEMATIC" && cards_[last + 1].keyword != "DISTRIBUTING"))
          throw Error("missing_param", "*COUPLING 뒤에 *KINEMATIC 또는 *DISTRIBUTING 이 있어야 합니다");
        ++last;
        const Card& d = cards_[last];
        Params dp(d);
        const auto ref_node = p.get("REF NODE"), surface = p.get("SURFACE");
        p.get("CONSTRAINT NAME");
        if (!ref_node || !surface) throw Error("missing_param", "REF NODE 와 SURFACE 가 필요합니다");
        q["ref_node"] = to_int(c, *ref_node), q["surface"] = surface_target(*surface);
        if (auto o = p.get("ORIENTATION")) q["orientation"] = ref(orientation_id_, *o, "방향");
        std::set<int> dofs;
        for (const std::string& line : d.lines) {
          const std::vector<std::string> cells = split(line);
          const int a = static_cast<int>(to_int(d, cells[0]));
          const int b = cells.size() > 1 && !cells[1].empty() ? static_cast<int>(to_int(d, cells[1])) : a;
          for (int k = a; k <= b; ++k) dofs.insert(k);
        }
        if (dofs.empty()) throw Error("missing_param", "자유도가 없습니다");
        q["dofs"] = std::vector<int>(dofs.begin(), dofs.end());
        dp.require_known();
        command = d.keyword == "KINEMATIC" ? "constraint.create_coupling_kinematic" : "constraint.create_coupling_distributing";
      } else {  // TIE
        p.get("NAME");
        if (c.lines.empty()) throw Error("missing_param", "종속 면과 주 면이 없습니다");
        const std::vector<std::string> cells = split(c.lines[0]);
        if (cells.size() < 2) bad(c, "종속 면과 주 면이 있어야 합니다", c.lines[0]);
        const bool cyclic = p.flag("CYCLIC SYMMETRY"), multistage = p.flag("MULTISTAGE");
        if (auto v = p.get("POSITION TOLERANCE")) q["position_tolerance"] = std::strtod(v->c_str(), nullptr);
        if (multistage) {
          q["slave"] = nset_target(cells[0]), q["master"] = nset_target(cells[1]);
          command = "constraint.create_multistage";
        } else if (cyclic) {
          if (last + 1 >= cards_.size() || cards_[last + 1].keyword != "CYCLIC SYMMETRY MODEL")
            throw Error("missing_param", "*TIE, CYCLIC SYMMETRY 뒤에 *CYCLIC SYMMETRY MODEL 이 있어야 합니다");
          ++last;
          const Card& m = cards_[last];
          Params mp(m);
          const auto n = mp.get("N");
          mp.get("TIE");
          if (!n || m.lines.empty()) throw Error("missing_param", "N 과 축이 필요합니다");
          const std::vector<double> ax = numbers(m, m.lines[0]);
          if (ax.size() < 6) bad(m, "축 위의 두 점이 있어야 합니다", m.lines[0]);
          q["slave"] = surface_target(cells[0], true), q["master"] = surface_target(cells[1], true);
          q["sectors"] = to_int(m, *n);
          q["axis_point_a"] = {ax[0], ax[1], ax[2]}, q["axis_point_b"] = {ax[3], ax[4], ax[5]};
          if (auto g = mp.get("NGRAPH")) q["ngraph"] = to_int(m, *g);
          if (auto e = mp.get("ELSET")) q["elements"] = elset_target(*e);
          mp.require_known();
          command = "constraint.create_cyclic_symmetry";
        } else {
          q["slave"] = surface_target(cells[0], true), q["master"] = surface_target(cells[1]);
          if (auto adj = p.get("ADJUST"); adj && upper(*adj) == "NO") q["adjust"] = false;
          command = "constraint.create_tie";
        }
      }
      p.require_known();
      commit({{command, q}});
    } catch (const Error& e) {
      preserve(case_, first, last, e.code());
      return last;
    }
    for (std::size_t i = first; i <= last; ++i) used_[i] = 1;
    return last;
  }

  // *SURFACE BEHAVIOR / *CHANGE SURFACE BEHAVIOR 의 내용
  static void surface_behavior(const Card& c, Params& p, Json& q) {
    static const std::map<std::string, const char*> po = {
        {"HARD", "hard"}, {"LINEAR", "linear"}, {"EXPONENTIAL", "exponential"}, {"TABULAR", "tabular"}, {"TIED", "tied"}};
    auto it = po.find(p.text("PRESSURE-OVERCLOSURE"));
    if (it == po.end()) throw Error("unknown_param", "해석하지 못한 값: PRESSURE-OVERCLOSURE");
    q["pressure_overclosure"] = it->second;
    const std::vector<double> v = c.lines.empty() ? std::vector<double>{} : numbers(c, c.lines[0]);
    if (it->first == "LINEAR") {
      if (!v.empty()) q["slope"] = v[0];
      if (v.size() > 1) q["sigma_inf"] = v[1];
      if (v.size() > 2) q["c0"] = v[2];
    } else if (it->first == "EXPONENTIAL") {
      if (v.size() < 2) throw Error("missing_param", "c0, p0 가 필요합니다");
      q["c0"] = v[0], q["p0"] = v[1];
    } else if (it->first == "TIED") {
      if (!v.empty()) q["slope"] = v[0];
    } else if (it->first == "TABULAR") {
      q["table"] = table(c, 2);
    }
  }
  // *FRICTION / *CHANGE FRICTION 의 내용
  static void friction(const Card& c, Json& q) {
    const std::vector<double> v = c.lines.empty() ? std::vector<double>{} : numbers(c, c.lines[0]);
    if (v.empty()) throw Error("missing_param", "마찰계수가 없습니다");
    q["friction_coefficient"] = v[0];
    if (v.size() > 1) q["stick_slope"] = v[1];
  }

  // *SURFACE INTERACTION 과 그 아래 카드들
  std::size_t interaction(std::size_t first) {
    static const std::set<std::string> block = {"SURFACE BEHAVIOR", "FRICTION", "CONTACT DAMPING", "GAP CONDUCTANCE", "GAP HEAT GENERATION"};
    std::size_t last = first;
    while (last + 1 < cards_.size() && !used_[last + 1] && block.count(cards_[last + 1].keyword)) ++last;
    const Card& head = cards_[first];
    try {
      Params hp(head);
      const auto name = hp.get("NAME");
      hp.require_known();
      if (!name) throw Error("missing_param", "NAME 이 없습니다");
      Json q{{"name", free_name("contact_property", *name)}};
      for (std::size_t i = first + 1; i <= last; ++i) {
        const Card& c = cards_[i];
        Params p(c);
        auto first_row = [&]() -> std::vector<double> { return c.lines.empty() ? std::vector<double>{} : numbers(c, c.lines[0]); };
        if (c.keyword == "SURFACE BEHAVIOR") {
          surface_behavior(c, p, q);
        } else if (c.keyword == "FRICTION") {
          friction(c, q);
        } else if (c.keyword == "CONTACT DAMPING") {
          const std::vector<double> v = first_row();
          if (v.empty()) throw Error("missing_param", "감쇠 계수가 없습니다");
          q["damping"] = v[0];
          if (auto t = p.get("TANGENT FRACTION")) q["damping_tangent_fraction"] = std::strtod(t->c_str(), nullptr);
        } else if (c.keyword == "GAP CONDUCTANCE") {
          q["conductance"] = table(c, 2);
        } else {  // GAP HEAT GENERATION
          const std::vector<double> v = first_row();
          if (v.empty()) throw Error("missing_param", "열 변환율이 없습니다");
          q["heat_conversion"] = v[0];
          if (v.size() > 1) q["heat_slave_fraction"] = v[1];
          if (v.size() > 2) q["sliding_velocity"] = v[2];
        }
        p.require_known();
      }
      interaction_id_[upper(*name)] = invoke("contact_property.create", q);
    } catch (const Error& e) {
      preserve(case_, first, last, e.code());
      return last;
    }
    for (std::size_t i = first; i <= last; ++i) used_[i] = 1;
    return last;
  }

  Ops orientation(const Card& c, Params& p) {
    const auto name = p.get("NAME");
    const std::string system = p.text("SYSTEM");
    if (!name || c.lines.empty()) throw Error("missing_param", "NAME 또는 데이터가 없습니다");
    if (!system.empty() && system != "RECTANGULAR" && system != "CYLINDRICAL") throw Error("unknown_param", "해석하지 못한 값: SYSTEM");
    const std::vector<double> v = numbers(c, c.lines[0]);
    if (v.size() != 6) bad(c, "좌표 6개가 있어야 합니다", c.lines[0]);
    Json q{{"name", free_name("orientation", *name)}, {"a", {v[0], v[1], v[2]}}, {"b", {v[3], v[4], v[5]}}};
    if (c.lines.size() > 1) {
      const std::vector<double> r = numbers(c, c.lines[1]);
      if (r.size() == 2) q["rotation_axis"] = static_cast<int>(r[0]), q["rotation_angle"] = r[1];
    }
    return {{system == "CYLINDRICAL" ? "orientation.create_cylindrical" : "orientation.create_rectangular", q}};
  }
  Ops amplitude_card(const Card& c, Params& p) {
    const auto name = p.get("NAME");
    const std::string time = p.text("TIME");
    if (!name) throw Error("missing_param", "NAME 이 없습니다");
    if (!time.empty() && time != "TOTAL TIME" && time != "STEP TIME") throw Error("unknown_param", "해석하지 못한 값: TIME");
    std::vector<double> v;
    for (const std::string& line : c.lines)
      for (double x : numbers(c, line)) v.push_back(x);
    if (v.empty() || v.size() % 2) throw Error("parse_error", "시간과 값이 짝이 맞지 않습니다");
    Json pts = Json::array();
    for (std::size_t i = 0; i < v.size(); i += 2) pts.push_back(Json::array({v[i], v[i + 1]}));
    Json q{{"name", free_name("function", *name)}, {"points", pts}};
    if (time == "TOTAL TIME") q["time"] = "total";
    return {{"function.create_amplitude", q}};
  }
  Ops time_points(const Card& c, Params& p) {
    const auto name = p.get("NAME");
    const std::string time = p.text("TIME");
    if (!name) throw Error("missing_param", "NAME 이 없습니다");
    if (!time.empty() && time != "TOTAL TIME" && time != "STEP TIME") throw Error("unknown_param", "해석하지 못한 값: TIME");
    Json times = Json::array();
    for (const std::string& line : c.lines)
      for (double x : numbers(c, line)) times.push_back(x);
    Json q{{"name", free_name("time_points", *name)}, {"times", times}};
    if (time == "TOTAL TIME") q["time"] = "total";
    return {{"time_points.create", q}};
  }

  Ops section(const Card& c, Params& p) {
    const std::string& k = c.keyword;
    const auto elset = p.get("ELSET");
    if (!elset) throw Error("missing_param", "ELSET 이 없습니다");
    Json q{{"target", elset_target(*elset)}};
    auto material = [&] {
      const auto m = p.get("MATERIAL");
      if (!m) throw Error("missing_param", "MATERIAL 이 없습니다");
      q["material"] = ref(material_id_, *m, "재료");
    };
    auto orientation = [&] {
      if (auto o = p.get("ORIENTATION")) q["orientation"] = ref(orientation_id_, *o, "방향");
    };
    auto first = [&]() -> std::vector<double> { return c.lines.empty() ? std::vector<double>{} : numbers(c, c.lines[0]); };
    if (k == "SOLID SECTION") {
      material(), orientation();
      if (!first().empty()) q["thickness"] = first()[0];
      return {{"property.create_solid", q}};
    }
    if (k == "SHELL SECTION" || k == "MEMBRANE SECTION") {
      orientation();
      if (auto o = p.get("OFFSET")) q["offset"] = std::strtod(o->c_str(), nullptr);
      if (k == "SHELL SECTION" && p.flag("COMPOSITE")) {
        Json layers = Json::array();
        for (const std::string& line : c.lines) {
          const std::vector<std::string> cells = split(line);
          if (cells.size() < 3) bad(c, "층에는 두께와 재료가 있어야 합니다", line);
          Json layer{{"thickness", to_double(c, cells[0])}, {"material", ref(material_id_, cells[2], "재료")}};
          if (cells.size() > 3 && !cells[3].empty()) layer["orientation"] = ref(orientation_id_, cells[3], "방향");
          layers.push_back(layer);
        }
        q["layers"] = layers;
        return {{"property.create_composite", q}};
      }
      material();
      if (first().empty()) throw Error("missing_param", "두께가 없습니다");
      q["thickness"] = first()[0];
      if (k == "SHELL SECTION" && p.flag("NODAL THICKNESS")) q["nodal_thickness"] = true;
      return {{k == "SHELL SECTION" ? "property.create_shell" : "property.create_membrane", q}};
    }
    if (k == "BEAM SECTION") {
      static const std::map<std::string, const char*> sections = {
          {"RECT", "rect"}, {"CIRC", "circ"}, {"PIPE", "pipe"}, {"BOX", "box"}, {"GENERAL", "general"}};
      material(), orientation();
      auto s = sections.find(p.text("SECTION"));
      if (s == sections.end() || c.lines.empty()) throw Error("unknown_param", "해석하지 못한 값: SECTION");
      q["section"] = s->second, q["dimensions"] = first();
      if (auto o = p.get("OFFSET1")) q["offset1"] = std::strtod(o->c_str(), nullptr);
      if (auto o = p.get("OFFSET2")) q["offset2"] = std::strtod(o->c_str(), nullptr);
      if (c.lines.size() > 1) {
        const std::vector<double> d = numbers(c, c.lines[1]);
        if (d.size() == 3) q["direction"] = d;
      }
      return {{"property.create_beam", q}};
    }
    if (k == "MASS") {
      if (first().empty()) throw Error("missing_param", "질량이 없습니다");
      q["mass"] = first()[0];
      return {{"property.create_mass", q}};
    }
    if (k == "GAP") {
      // 틈, 법선 방향(3), (빈 칸), 강성, 인장력
      const std::vector<std::string> cells = c.lines.empty() ? std::vector<std::string>{} : split(c.lines[0]);
      if (cells.size() < 4) throw Error("missing_param", "틈과 방향이 있어야 합니다");
      q["clearance"] = to_double(c, cells[0]);
      q["direction"] = {to_double(c, cells[1]), to_double(c, cells[2]), to_double(c, cells[3])};
      if (cells.size() > 5 && !cells[5].empty()) q["stiffness"] = to_double(c, cells[5]);
      if (cells.size() > 6 && !cells[6].empty()) q["tension_force"] = to_double(c, cells[6]);
      return {{"property.create_gap", q}};
    }
    if (k == "FLUID SECTION") {
      material();
      const auto type = p.get("TYPE");
      if (!type) throw Error("missing_param", "TYPE 이 없습니다");
      q["section_type"] = *type;
      if (auto o = p.get("OIL")) q["oil"] = *o;
      std::vector<double> constants;
      for (const std::string& line : c.lines)
        for (double v : numbers(c, line)) constants.push_back(v);
      if (!constants.empty()) q["constants"] = constants;
      return {{"property.create_fluid", q}};
    }
    if (k == "USER SECTION") {
      material();
      p.get("CONSTANTS");  // 개수는 데이터 줄에서 센다
      std::vector<double> constants;
      for (const std::string& line : c.lines)
        for (double v : numbers(c, line)) constants.push_back(v);
      q["constants"] = constants;
      return {{"property.create_user", q}};
    }
    // SPRING·DASHPOT: 첫 줄은 자유도(SPRINGA·DASHPOTA 는 빈 줄), 그다음이 강성(감쇠) 또는 표.
    const bool dashpot = k == "DASHPOT";
    if (!dashpot) orientation();
    const bool nonlinear = p.flag("NONLINEAR");
    if (c.raw_lines.size() < 2) throw Error("missing_param", "자유도 줄과 강성 줄이 있어야 합니다");
    const std::vector<std::string> dofs = split(c.raw_lines[0]);
    if (!dofs[0].empty()) q["dof1"] = to_int(c, dofs[0]);
    if (dofs.size() > 1 && !dofs[1].empty()) q["dof2"] = to_int(c, dofs[1]);
    Json rows = Json::array();
    for (std::size_t i = 1; i < c.raw_lines.size(); ++i)
      if (!c.raw_lines[i].empty()) rows.push_back(numbers(c, c.raw_lines[i]));
    if (rows.empty() || rows[0].empty()) throw Error("missing_param", dashpot ? "감쇠 계수가 없습니다" : "강성이 없습니다");
    if (nonlinear || (dashpot && (rows.size() > 1 || rows[0].size() > 1))) q["table"] = rows;  // 감쇠는 [계수, 주파수] 표
    else q[dashpot ? "coefficient" : "stiffness"] = rows[0][0];
    return {{dashpot ? "property.create_dashpot" : "property.create_spring", q}};
  }

  // *BOUNDARY: 노드 번호로 적힌 줄은 구속 내용이 같은 노드끼리 묶어 객체 하나로 만든다.
  Ops boundary(const Card& c, Params& p, Id step) {
    Json common = Json::object();
    if (step) common["parent"] = step;
    const bool cfd_step = step && a_.model().get(step).props.value("type", std::string()) == "cfd";
    amplitude(p, common);
    const bool fixed = p.flag("FIXED");
    if (p.flag("SUBMODEL")) throw Error("unknown_param", "서브모델 경계조건은 *SUBMODEL 카드와 함께 보존한다");
    const std::string op = p.text("OP");
    if (op == "NEW" && step) a_.invoke("step.update", Json{{"id", step}, {"bcs_inheritance", "new"}});
    struct Acc {
      std::vector<int> dofs;
      std::vector<double> values;
      std::optional<double> temperature;
      std::optional<double> potential;  // 자유도 8: 전위(전자기)·압력(3D 유체) — 전자기 경계조건으로 읽는다
    };
    std::vector<std::string> order;
    std::map<std::string, Acc> by_target;
    for (const std::string& line : c.lines) {
      const std::vector<std::string> cells = split(line);
      if (cells.size() < 2) bad(c, "자유도가 없습니다", line);
      const int first = static_cast<int>(to_int(c, cells[1]));
      const int last = cells.size() > 2 && !cells[2].empty() ? static_cast<int>(to_int(c, cells[2])) : first;
      const double value = cells.size() > 3 ? to_double(c, cells[3]) : 0.0;
      if (!by_target.count(cells[0])) order.push_back(cells[0]);
      Acc& acc = by_target[cells[0]];
      for (int d = first; d <= last; ++d) {
        if (d == 11) acc.temperature = value;
        else if (d == 8) acc.potential = value;
        else if (d >= 1 && d <= 6) acc.dofs.push_back(d), acc.values.push_back(value);
        else throw Error("unknown_dof", "아직 읽지 못하는 자유도: " + std::to_string(d));
      }
    }
    // 같은 구속을 받는 노드 번호를 한데 모은다.
    std::vector<std::string> groups;
    std::map<std::string, std::pair<Acc, Json>> merged;  // 구속 내용 → (내용, 대상)
    for (const std::string& name : order) {
      const Acc& acc = by_target[name];
      const bool number = is_number(name);
      const std::string key = number ? Json(acc.dofs).dump() + Json(acc.values).dump() + (acc.temperature ? std::to_string(*acc.temperature) : "") +
                                           (acc.potential ? "V" + std::to_string(*acc.potential) : "")
                                     : "set:" + upper(name);
      if (!merged.count(key)) {
        groups.push_back(key);
        merged[key] = {acc, number ? Json{{"type", "nodes"}, {"ids", Json::array()}} : nset_target(name)};
      }
      if (number) merged[key].second["ids"].push_back(to_int(c, name));
    }
    Ops ops;
    for (const std::string& key : groups) {
      const auto& [acc, target] = merged[key];
      if (!acc.dofs.empty()) {
        Json q = common;
        q["target"] = target, q["dofs"] = acc.dofs;
        if (fixed) {
          ops.push_back({"bc.create_fixed_current", q});
        } else {
          if (std::any_of(acc.values.begin(), acc.values.end(), [](double v) { return v != 0.0; })) q["values"] = acc.values;
          ops.push_back({"bc.create_displacement", q});
        }
      }
      if (acc.temperature) {
        Json q = common;
        q["target"] = target, q["value"] = *acc.temperature;
        ops.push_back({"bc.create_temperature", q});
      }
      if (acc.potential) {
        Json q = common;
        q["target"] = target;
        if (cfd_step) {  // 3D 유체에서 자유도 8 은 압력(매뉴얼 6.9)
          q["quantity"] = "pressure", q["values"] = Json::array({*acc.potential});
          ops.push_back({"bc.create_fluid", q});
        } else {
          q["value"] = *acc.potential;
          ops.push_back({"bc.create_electromagnetic", q});
        }
      }
    }
    apply_transform(ops);
    return ops;
  }

  // 요소면 대상: "요소(셋), 라벨번호" 를 [요소, 면 번호] 로 푼다.
  void add_faces(const Card& c, const std::string& owner, int label, Json& faces) const {
    for (Id e : elements_of(c, owner)) {
      const int face = face_number(c, e, label);
      if (face < 1) throw Error("shell_face", "쉘의 윗면·아랫면에 건 하중은 아직 읽지 못합니다");
      faces.push_back(Json::array({e, face}));
    }
  }

  Ops load(const Card& c, Params& p, Id step) {
    const std::string& k = c.keyword;
    Json common{{"parent", step}};
    amplitude(p, common);
    if (p.text("OP") == "NEW") a_.invoke("step.update", Json{{"id", step}, {"loads_inheritance", "new"}});
    if (k == "CLOAD" || k == "DLOAD") {
      if (auto v = p.get("LOAD CASE")) common["phase"] = *v == "2" ? "imaginary" : "real";
      if (auto v = p.get("SECTOR")) common["sector"] = to_int(c, *v);
    }
    Ops ops;
    // 값이 같은 것끼리 묶는 도구: key → (속성, 대상 목록)
    std::vector<std::string> order;
    std::map<std::string, Json> grouped;
    auto group = [&](const std::string& command, const Json& props, const char* target_type) -> Json& {
      const std::string key = command + props.dump();
      if (!grouped.count(key)) {
        order.push_back(key);
        Json q = common;
        q.update(props);
        q["target"] = Json{{"type", target_type}, {"ids", Json::array()}};
        grouped[key] = Json{{"command", command}, {"params", q}};
      }
      return grouped[key]["params"]["target"]["ids"];
    };
    auto direct = [&](const std::string& command, Json props, const Json& target) {
      Json q = common;
      q.update(props);
      q["target"] = target;
      ops.push_back({command, q});
    };

    if (k == "TEMPERATURE" && c.params.count("FILE")) {
      Json q = common;
      q["file"] = *p.get("FILE");
      if (auto b = p.get("BSTEP")) q["begin_step"] = to_int(c, *b);
      return {{"load.create_temperature_from_file", q}};
    }
    if (k == "FILM")
      if (auto v = p.get("FILM AMPLITUDE")) common["coefficient_amplitude"] = ref(function_id_, *v, "AMPLITUDE");

    std::vector<std::string> cload_order;
    std::map<std::string, std::array<double, 6>> cload;
    for (const std::string& line : c.lines) {
      const std::vector<std::string> cells = split(line);
      if (cells.size() < 2) bad(c, "값이 모자랍니다", line);
      const std::string label = upper(cells[1]);
      auto value = [&](std::size_t i) { return i < cells.size() ? to_double(c, cells[i]) : 0.0; };
      if (k == "CLOAD") {
        const int dof = static_cast<int>(to_int(c, cells[1]));
        if (dof < 1 || dof > 6) throw Error("unknown_dof", "아직 읽지 못하는 자유도: " + cells[1]);
        if (!cload.count(cells[0])) cload_order.push_back(cells[0]), cload[cells[0]] = {};
        cload[cells[0]][static_cast<std::size_t>(dof - 1)] = value(2);
      } else if (k == "TEMPERATURE") {
        if (cells.size() > 2 && !cells[2].empty())
          direct("load.create_temperature_gradient", Json{{"value", value(1)}, {"gradient", value(2)}}, node_target(c, cells[0]));
        else if (is_number(cells[0])) group("load.create_temperature", Json{{"value", value(1)}}, "nodes").push_back(to_int(c, cells[0]));
        else direct("load.create_temperature", Json{{"value", value(1)}}, nset_target(cells[0]));
      } else if (k == "CFLUX") {
        if (is_number(cells[0])) group("load.create_concentrated_flux", Json{{"value", value(2)}}, "nodes").push_back(to_int(c, cells[0]));
        else direct("load.create_concentrated_flux", Json{{"value", value(2)}}, nset_target(cells[0]));
      } else if (k == "DLOAD" && label == "P") {
        direct("load.create_pressure", Json{{"value", value(2)}}, elset_target(cells[0]));
      } else if (k == "DLOAD" && label == "GRAV") {
        direct("load.create_gravity", Json{{"value", value(2)}, {"direction", {value(3), value(4), value(5)}}}, elset_target(cells[0]));
      } else if (k == "DLOAD" && label == "CENTRIF") {
        if (value(2) < 0) throw Error("out_of_range", "각속도의 제곱이 음수입니다");
        direct("load.create_centrifugal",
               Json{{"omega", std::sqrt(value(2))}, {"axis_point", {value(3), value(4), value(5)}}, {"axis_direction", {value(6), value(7), value(8)}}},
               elset_target(cells[0]));
      } else if (k == "DFLUX" && label == "BF") {
        direct("load.create_body_flux", Json{{"value", value(2)}}, elset_target(cells[0]));
      } else {
        // 요소면 하중: P1, S1, F1, R1 …
        const char prefix = k == "DLOAD" ? 'P' : k == "DFLUX" ? 'S' : k == "FILM" ? 'F' : 'R';
        if (label.size() != 2 || label[0] != prefix || !std::isdigit(static_cast<unsigned char>(label[1])))
          throw Error("unknown_label", "아직 읽지 못하는 하중 라벨: " + label);
        const int n = label[1] - '0';
        Json props;
        std::string command;
        if (k == "DLOAD") command = "load.create_pressure", props = Json{{"value", value(2)}};
        else if (k == "DFLUX") command = "load.create_surface_flux", props = Json{{"value", value(2)}};
        else if (k == "FILM") command = "load.create_film", props = Json{{"sink_temperature", value(2)}, {"coefficient", value(3)}};
        else command = "load.create_radiation", props = Json{{"sink_temperature", value(2)}, {"emissivity", value(3)}};
        add_faces(c, cells[0], n, group(command, props, "faces"));
      }
    }
    // 집중 하중: 성분이 같은 노드 번호끼리 묶는다. 1~3 은 힘, 4~6 은 모멘트.
    for (const std::string& name : cload_order) {
      const auto& v = cload[name];
      for (int part = 0; part < 2; ++part) {
        const Json comps = Json::array({v[static_cast<std::size_t>(3 * part)], v[static_cast<std::size_t>(3 * part + 1)], v[static_cast<std::size_t>(3 * part + 2)]});
        if (comps == Json::array({0.0, 0.0, 0.0})) continue;
        const char* command = part == 0 ? "load.create_force" : "load.create_moment";
        if (is_number(name)) group(command, Json{{"components", comps}}, "nodes").push_back(to_int(c, name));
        else direct(command, Json{{"components", comps}}, nset_target(name));
      }
    }
    for (const std::string& key : order) ops.push_back({grouped[key]["command"].get<std::string>(), grouped[key]["params"]});
    if (k == "CLOAD") apply_transform(ops);
    return ops;
  }

  Ops output(const Card& c, Params& p, Id step) {
    const std::string& k = c.keyword;
    Json q{{"parent", step}};
    Json vars = Json::array();
    for (const std::string& line : c.lines)
      for (const std::string& cell : split(line))
        if (!cell.empty()) vars.push_back(upper(cell));
    q["variables"] = vars;
    if (auto v = p.get("FREQUENCY")) q["frequency"] = to_int(c, *v);
    if (auto v = p.get("FREQUENCYF")) q["frequency_f"] = to_int(c, *v);
    if (auto v = p.get("TIME POINTS")) q["time_points"] = ref(time_points_id_, *v, "TIME POINTS");
    auto totals = [&] {
      const std::string t = p.text("TOTALS");
      if (t == "YES" || t == "ONLY") q["totals"] = t == "YES" ? "yes" : "only";
    };
    if (k == "NODE FILE" || k == "EL FILE") {
      if (auto v = p.get("NSET")) q["target"] = nset_target(*v);
      if (auto v = p.get("GLOBAL")) q["global"] = upper(*v) != "NO";
      if (auto v = p.get("OUTPUT")) q["expand"] = upper(*v) == "2D" ? "2d" : "3d";
      if (k == "EL FILE" && p.flag("SECTION FORCES")) q["section_forces"] = true;
      if (p.flag("LAST ITERATIONS")) q["last_iterations"] = true;
      if (p.flag("CONTACT ELEMENTS")) q["contact_elements"] = true;
      return {{k == "NODE FILE" ? "output_request.create_node_file" : "output_request.create_element_file", q}};
    }
    if (k == "CONTACT FILE") return {{"output_request.create_contact_file", q}};
    if (k == "CONTACT PRINT") {
      const auto slave = p.get("SLAVE"), master = p.get("MASTER");
      if (slave && master) q["pair"] = pair_of(*slave, *master);
      else if (slave || master) throw Error("missing_param", "SLAVE 와 MASTER 는 함께 적어야 합니다");
      totals();
      return {{"output_request.create_contact_print", q}};
    }
    if (k == "SECTION PRINT") {
      const auto surface = p.get("SURFACE"), name = p.get("NAME");
      if (!surface || !name) throw Error("missing_param", "SURFACE 와 NAME 이 있어야 합니다");
      q["target"] = surface_target(*surface), q["label"] = *name;
      return {{"output_request.create_section_print", q}};
    }
    if (k == "NODE PRINT") {
      const auto set = p.get("NSET");
      if (!set) throw Error("missing_param", "NSET 이 없습니다");
      q["target"] = nset_target(*set);
      if (auto v = p.get("GLOBAL")) q["global"] = upper(*v) == "YES";
      totals();
      return {{"output_request.create_node_print", q}};
    }
    const auto set = p.get("ELSET");
    if (!set) throw Error("missing_param", "ELSET 이 없습니다");
    q["target"] = elset_target(*set);
    if (auto v = p.get("GLOBAL")) q["global"] = upper(*v) == "YES";
    totals();
    return {{"output_request.create_element_print", q}};
  }

  // ------------------------------------------------------------ 스텝
  std::size_t step(std::size_t first) {
    static const std::map<std::string, const char*> procedures = {
        {"STATIC", "static"}, {"FREQUENCY", "frequency"}, {"COMPLEX FREQUENCY", "complex_frequency"}, {"BUCKLE", "buckle"},
        {"MODAL DYNAMIC", "modal_dynamic"}, {"STEADY STATE DYNAMICS", "steady_state_dynamics"}, {"DYNAMIC", "dynamic"},
        {"HEAT TRANSFER", "heat_transfer"}, {"COUPLED TEMPERATURE-DISPLACEMENT", "coupled_temperature_displacement"},
        {"UNCOUPLED TEMPERATURE-DISPLACEMENT", "uncoupled_temperature_displacement"}, {"VISCO", "visco"}, {"GREEN", "green"},
        {"CFD", "cfd"}, {"ELECTROMAGNETICS", "electromagnetics"}, {"SENSITIVITY", "sensitivity"}, {"CRACK PROPAGATION", "crack_propagation"},
        {"FEASIBLE DIRECTION", "feasible_direction"}, {"ROBUST DESIGN", "robust_design"}};
    std::size_t last = first, proc = 0;
    while (last + 1 < cards_.size() && cards_[last].keyword != "END STEP") {
      ++last;
      if (!proc && procedures.count(cards_[last].keyword)) proc = last;
    }
    Id id = 0;
    try {
      if (cards_[last].keyword != "END STEP") throw Error("parse_error", "*END STEP 이 없습니다");
      if (!proc) throw Error("unknown_procedure", "아직 읽지 못하는 해석 절차입니다");
      const Card& sc = cards_[first];
      const Card& pc = cards_[proc];
      Params sp(sc), pp(pc);
      const std::string sub = procedures.at(pc.keyword);
      Json q{{"parent", case_}};
      if (sp.flag("NLGEOM")) {
        const std::string v = upper(sc.params.at("NLGEOM"));
        q["nlgeom"] = v != "NO";
      }
      if (sp.flag("PERTURBATION")) q["perturbation"] = true;
      if (auto v = sp.get("INC")) q["max_increments"] = to_int(sc, *v);
      if (auto v = sp.get("INCF")) q["max_fluid_increments"] = to_int(sc, *v);
      if (auto v = sp.get("AMPLITUDE")) {
        const std::string a = upper(*v);
        if (a != "RAMP" && a != "STEP") throw Error("unknown_param", "해석하지 못한 값: AMPLITUDE");
        q["load_application"] = a == "STEP" ? "step" : "ramp";
      }
      sp.require_known();
      procedure(pc, pp, sub, q);
      pp.require_known();
      if (sub == "sensitivity" || sub == "crack_propagation" || sub == "feasible_direction" || sub == "robust_design")
        step_extras(first, last, sub, q);  // 절차에 딸린 카드(*DESIGN RESPONSE·*OBJECTIVE·*GEOMETRIC TOLERANCES 등)
      id = invoke("step.create_" + sub, q);
    } catch (const Error& e) {
      preserve(case_, first, last, e.code());  // 스텝 전체를 그대로 보존한다
      return last;
    }
    used_[first] = used_[proc] = used_[last] = 1;
    last_step_ = id, in_steps_ = true;
    if (!model_bcs_.empty()) {  // 모델 수준의 경계조건을 첫 스텝에 둔다
      Ops ops = model_bcs_;
      for (auto& [command, params] : ops) params["parent"] = id;
      try {
        commit(ops);
        for (std::size_t i : model_bc_cards_) used_[i] = 1;
      } catch (const Error& e) {
        for (std::size_t i : model_bc_cards_) preserve(case_, i, i, e.code());
      }
      model_bcs_.clear(), model_bc_cards_.clear();
    }
    for (std::size_t i = first + 1; i < last; ++i)
      if (!used_[i]) handle(i, id);
    return last;
  }

  void procedure(const Card& c, Params& p, const std::string& sub, Json& q) {
    static const std::map<std::string, const char*> solvers = {
        {"PASTIX", "pastix"}, {"PARDISO", "pardiso"}, {"SPOOLES", "spooles"}, {"TAUCS", "taucs"},
        {"ITERATIVE SCALING", "iterative_scaling"}, {"ITERATIVE CHOLESKY", "iterative_cholesky"}};
    const std::vector<double> v = c.lines.empty() ? std::vector<double>{} : numbers(c, c.lines[0]);
    const std::vector<std::string> cells = c.lines.empty() ? std::vector<std::string>{} : split(c.lines[0]);
    auto given = [&](std::size_t i) { return i < cells.size() && !cells[i].empty(); };
    auto time_line = [&] {
      static const char* names[] = {"initial_increment", "period", "min_increment", "max_increment"};
      for (std::size_t i = 0; i < 4; ++i)
        if (given(i)) q[names[i]] = v[i];
    };
    auto time_params = [&] {
      if (p.flag("DIRECT")) q["direct"] = true;
      if (p.flag("TIME RESET")) q["time_reset"] = true;
      if (auto t = p.get("TOTAL TIME AT START")) q["total_time_at_start"] = std::strtod(t->c_str(), nullptr);
    };
    auto number = [&](const char* key, const char* name) {
      if (auto t = p.get(key)) q[name] = std::strtod(t->c_str(), nullptr);
    };
    auto modes = [&] {
      if (!given(0)) throw Error("missing_param", "모드 수가 없습니다");
      q["num_modes"] = static_cast<int>(v[0]);
    };
    if (auto s = p.get("SOLVER")) {
      auto it = solvers.find(upper(*s));
      if (it == solvers.end()) throw Error("unknown_param", "해석하지 못한 값: SOLVER");
      q["solver"] = it->second;
    }
    if (sub == "static") {
      time_params(), time_line();
    } else if (sub == "frequency") {
      modes();
      if (given(1)) q["freq_min"] = v[1];
      if (given(2)) q["freq_max"] = v[2];
      if (p.yes("STORAGE")) q["storage"] = true;
      if (auto g = p.get("GLOBAL")) q["global"] = upper(*g) != "NO";
      if (auto g = p.get("CYCMPC")) q["cycmpc"] = upper(*g) == "INACTIVE" ? "inactive" : "active";
    } else if (sub == "complex_frequency") {
      modes();
      if (p.flag("CORIOLIS")) q["coriolis"] = true;
    } else if (sub == "buckle") {
      modes();
      if (given(1)) q["accuracy"] = v[1];
    } else if (sub == "green") {
      modes();
      if (p.yes("STORAGE")) q["storage"] = true;
    } else if (sub == "modal_dynamic") {
      if (p.flag("DIRECT")) q["direct"] = true;
      if (p.flag("STEADY STATE")) q["steady_state"] = true;
      time_line();
    } else if (sub == "steady_state_dynamics") {
      if (!given(0) || !given(1)) throw Error("missing_param", "주파수 범위가 없습니다");
      q["freq_min"] = v[0], q["freq_max"] = v[1];
      if (given(2)) q["points"] = static_cast<int>(v[2]);
      if (given(3)) q["bias"] = v[3];
      if (given(4)) q["fourier_terms"] = static_cast<int>(v[4]);
      if (given(5)) q["period_start"] = v[5];
      if (given(6)) q["period_end"] = v[6];
      if (auto h = p.get("HARMONIC")) q["harmonic"] = upper(*h) != "NO";
    } else if (sub == "dynamic") {
      time_params(), time_line(), number("ALPHA", "alpha");
      if (p.flag("EXPLICIT")) q["explicit_scheme"] = "explicit";
      if (p.flag("RELATIVE TO ABSOLUTE")) q["relative_to_absolute"] = true;
    } else if (sub == "heat_transfer") {
      time_params(), number("DELTMX", "deltmx");
      if (p.flag("STEADY STATE")) q["steady_state"] = true;
      if (p.flag("MODAL DYNAMIC")) q["modal"] = true;
      if (p.yes("STORAGE")) q["storage"] = true;
      if (p.flag("FREQUENCY")) {
        q["eigenmodes"] = true;
        if (given(0)) q["num_modes"] = static_cast<int>(v[0]);
      } else {
        time_line();
      }
    } else if (sub == "coupled_temperature_displacement" || sub == "uncoupled_temperature_displacement") {
      time_params(), time_line(), number("ALPHA", "alpha"), number("DELTMX", "deltmx");
      if (p.flag("STEADY STATE")) q["steady_state"] = true;
    } else if (sub == "visco") {
      const auto cetol = p.get("CETOL");
      if (!cetol) throw Error("missing_param", "CETOL 이 없습니다");
      q["cetol"] = std::strtod(cetol->c_str(), nullptr);
      time_params(), time_line();
    } else if (sub == "cfd") {  // *CFD(매뉴얼 7.13)
      static const std::map<std::string, const char*> turbulence = {
          {"NONE", "none"}, {"K-EPSILON", "k-epsilon"}, {"K-OMEGA", "k-omega"}, {"SST", "sst"}};
      if (p.flag("STEADY STATE")) q["steady_state"] = true;
      if (p.flag("COMPRESSIBLE")) q["compressible"] = true;
      if (p.flag("SHALLOW WATER")) q["shallow_water"] = true;
      if (auto t = p.get("TURBULENCE MODEL")) {
        auto it = turbulence.find(upper(*t));
        if (it == turbulence.end()) throw Error("unknown_param", "해석하지 못한 값: TURBULENCE MODEL");
        q["turbulence_model"] = it->second;
      }
      time_params(), time_line();
    } else if (sub == "electromagnetics") {  // *ELECTROMAGNETICS(매뉴얼 7.44)
      if (p.flag("MAGNETOSTATICS")) q["magnetostatics"] = true;
      if (p.flag("FREQUENCY")) q["frequency_domain"] = true;
      if (p.flag("NO HEAT TRANSFER")) q["no_heat_transfer"] = true;
      time_params(), number("OMEGA", "omega"), number("DELTMX", "deltmx"), time_line();
    } else if (sub == "sensitivity") {  // *SENSITIVITY(매뉴얼 7.116): NLGEOM 은 *STEP 쪽에서 읽는다. READ·WRITE 는 파일 캐시라 쓰지 않는다
      if (p.flag("NLGEOM")) q["nlgeom"] = true;
    } else if (sub == "crack_propagation") {  // *CRACK PROPAGATION(매뉴얼 7.26)
      const auto input = p.get("INPUT");
      const auto mat = p.get("MATERIAL");
      if (!input || !mat) throw Error("missing_param", "INPUT 과 MATERIAL 이 필요합니다");
      q["input_file"] = *input;
      q["material"] = ref(material_id_, *mat, "MATERIAL");
      if (auto l = p.get("LENGTH")) {
        const std::string m = upper(*l);
        if (m != "CUMULATIVE" && m != "INTERSECTION") throw Error("unknown_param", "해석하지 못한 값: LENGTH");
        q["length_method"] = m == "CUMULATIVE" ? "cumulative" : "intersection";
      }
      if (given(0)) q["max_increment"] = v[0];
      if (given(1)) q["max_angle"] = v[1];
    } else if (sub == "feasible_direction") {  // *FEASIBLE DIRECTION(매뉴얼 7.56)
      if (auto m = p.get("METHOD")) {
        const std::string mm = upper(*m);
        if (mm != "GRADIENT DESCENT" && mm != "GRADIENT PROJECTION") throw Error("unknown_param", "해석하지 못한 값: METHOD");
        q["method"] = mm == "GRADIENT PROJECTION" ? "gradient_projection" : "gradient_descent";
      }
      if (given(0)) q["step_size"] = v[0];
    } else if (sub == "robust_design") {  // *ROBUST DESIGN(매뉴얼 7.111). 배포본 예제는 RANDOMFIELD ONLY 로도 쓴다
      if (p.flag("RANDOM FIELD ONLY") || p.flag("RANDOMFIELD ONLY")) q["random_field_only"] = true;
      if (!given(0)) throw Error("missing_param", "정확도가 없습니다");
      q["accuracy"] = v[0];
    }
  }

  // 민감도·균열 전파 스텝에 딸린 카드: *DESIGN RESPONSE·*FILTER / *HCF. 스텝 안의 다른 카드는 handle 이 처리한다
  void step_extras(std::size_t first, std::size_t last, const std::string& sub, Json& q) {
    for (std::size_t i = first + 1; i < last; ++i) {
      const Card& c = cards_[i];
      Params p(c);
      if (sub == "sensitivity" && c.keyword == "DESIGN RESPONSE") {
        const auto name = p.get("NAME");
        Json responses = q.value("design_responses", Json::array());
        for (const std::string& line : c.lines) {
          const std::vector<std::string> cells = split(line);
          if (cells.empty() || cells[0].empty()) continue;
          Json r{{"name", name ? *name : "resp" + std::to_string(responses.size() + 1)}, {"type", upper(cells[0])}};
          if (cells.size() > 1 && !cells[1].empty()) {
            const std::string set = cells[1];
            const std::string key = upper(set);
            if (part_id_.count(key) || elset_id_.count(key) || key == "EALL") r["target"] = elset_target(set);
            else r["target"] = nset_target(set);
          }
          Json values = Json::array();
          for (std::size_t k = 2; k < cells.size(); ++k)
            if (!cells[k].empty()) values.push_back(to_double(c, cells[k]));
          if (!values.empty()) r["values"] = values;
          responses.push_back(r);
        }
        q["design_responses"] = responses;
        p.require_known();
        used_[i] = 1;
      } else if (sub == "sensitivity" && c.keyword == "FILTER") {
        if (auto t = p.get("TYPE")) q["filter_type"] = upper(*t) == "EXPLICIT" ? "explicit" : "implicit";
        if (p.yes("BOUNDARY WEIGHTING")) q["boundary_weighting"] = true;
        if (p.yes("EDGE PRESERVATION")) q["edge_preservation"] = true;
        if (p.yes("DIRECTION WEIGHTING")) q["direction_weighting"] = true;
        if (!c.lines.empty()) {
          const std::vector<std::string> cells = split(c.lines[0]);
          if (!cells.empty() && !cells[0].empty()) q["filter_radius"] = to_double(c, cells[0]);
        }
        p.require_known();
        used_[i] = 1;
      } else if (sub == "feasible_direction" && c.keyword == "OBJECTIVE") {
        if (c.lines.empty()) throw Error("missing_param", "*OBJECTIVE 에 설계 응답 이름이 없습니다");
        q["objective"] = split(c.lines[0])[0];
        if (auto t = p.get("TARGET")) q["objective_target"] = upper(*t) == "MAX" ? "max" : "min";
        p.require_known();
        used_[i] = 1;
      } else if (sub == "feasible_direction" && c.keyword == "CONSTRAINT") {
        Json list = q.value("constraints", Json::array());
        for (const std::string& line : c.lines) {
          const std::vector<std::string> cells = split(line);
          if (cells.size() < 2 || cells[0].empty()) bad(c, "설계 응답 이름과 LE|GE 가 필요합니다", line);
          const std::string rel = upper(cells[1]);
          if (rel != "LE" && rel != "GE") bad(c, "LE 또는 GE 여야 합니다", line);
          Json r{{"response", cells[0]}, {"relation", rel == "GE" ? "ge" : "le"}};
          if (cells.size() > 2 && !cells[2].empty()) r["relative_value"] = to_double(c, cells[2]);
          if (cells.size() > 3 && !cells[3].empty()) r["absolute_value"] = to_double(c, cells[3]);
          list.push_back(r);
        }
        q["constraints"] = list;
        p.require_known();
        used_[i] = 1;
      } else if (sub == "feasible_direction" && c.keyword == "GEOMETRIC CONSTRAINT") {
        Json list = q.value("geometric_constraints", Json::array());
        for (const std::string& line : c.lines) {
          const std::vector<std::string> cells = split(line);
          if (cells.size() < 2 || cells[0].empty()) bad(c, "종류와 노드 셋이 필요합니다", line);
          const std::string type = upper(cells[0]);
          Json g{{"type", type}, {"target", node_target(c, cells[1])}};
          const bool pair = type == "MAXMEMBERSIZE" || type == "MINMEMBERSIZE" || type == "PACKAGING";
          if (pair && cells.size() > 2 && !cells[2].empty()) g["other_target"] = node_target(c, cells[2]);
          const std::size_t vi = pair ? 3 : 2;
          if (cells.size() > vi && !cells[vi].empty()) g["value"] = to_double(c, cells[vi]);
          list.push_back(g);
        }
        q["geometric_constraints"] = list;
        p.require_known();
        used_[i] = 1;
      } else if (sub == "robust_design" && c.keyword == "CORRELATION LENGTH") {
        if (c.lines.empty()) throw Error("missing_param", "상관 길이가 없습니다");
        q["correlation_length"] = to_double(c, split(c.lines[0])[0]);
        p.require_known();
        used_[i] = 1;
      } else if (sub == "robust_design" && c.keyword == "GEOMETRIC TOLERANCES") {
        if (upper(p.text("TYPE")) != "NORMAL") throw Error("unknown_param", "해석하지 못한 값: TYPE");
        if (p.flag("CONSTRAINED")) q["constrained"] = true;
        Json list = q.value("tolerances", Json::array());
        for (const std::string& line : c.lines) {
          const std::vector<std::string> cells = split(line);
          if (cells.size() < 3) bad(c, "노드(셋), 평균, 표준편차가 필요합니다", line);
          list.push_back(Json{{"target", node_target(c, cells[0])}, {"mean", to_double(c, cells[1])}, {"deviation", to_double(c, cells[2])}});
        }
        q["tolerances"] = list;
        p.require_known();
        used_[i] = 1;
      } else if (sub == "crack_propagation" && c.keyword == "HCF") {
        const auto input = p.get("INPUT");
        if (!input) throw Error("missing_param", "*HCF 에 INPUT 이 없습니다");
        q["hcf_input_file"] = *input;
        if (auto m = p.get("MODE")) q["hcf_mode"] = to_int(c, *m);
        if (auto m = p.get("MISSION STEP")) q["hcf_mission_step"] = to_int(c, *m);
        if (auto m = p.get("MISSIONSTEP")) q["hcf_mission_step"] = to_int(c, *m);
        if (auto m = p.get("MAX CYCLE")) q["hcf_max_cycle"] = to_double(c, *m);
        if (auto m = p.get("MAXCYCLE")) q["hcf_max_cycle"] = to_double(c, *m);
        if (auto m = p.get("SCALING")) q["hcf_scaling"] = to_double(c, *m);
        p.require_known();
        used_[i] = 1;
      }
    }
    if (sub == "sensitivity" && !q.contains("design_responses")) throw Error("missing_param", "*SENSITIVITY 스텝에 *DESIGN RESPONSE 가 없습니다");
    if (sub == "feasible_direction" && !q.contains("objective")) throw Error("missing_param", "*FEASIBLE DIRECTION 스텝에 *OBJECTIVE 가 없습니다");
    if (sub == "robust_design" && (!q.contains("correlation_length") || !q.contains("tolerances")))
      throw Error("missing_param", "*ROBUST DESIGN 스텝에 *CORRELATION LENGTH 와 *GEOMETRIC TOLERANCES 가 필요합니다");
  }

  App& a_;
  Mesh& m_;
  std::string path_;
  std::vector<Card> cards_;
  std::vector<char> used_;

  Id node_offset_ = 0, elem_offset_ = 0;
  int extra_connectivity_ = 0;  // 요소 연결에 남는 번호가 있던 요소 수(앞의 것만 썼다)
  std::map<Id, std::string> type_of_;                         // 요소(파일의 번호) → 솔버 타입
  std::map<std::string, std::vector<Id>> part_elems_, elset_elems_;
  std::map<std::string, std::vector<Id>> nset_nodes_;                      // 대문자 이름 → 노드(모델 번호)
  std::map<Id, Id> csys_of_node_;                                          // *TRANSFORM 이 걸린 노드 → 좌표계
  std::vector<Id> transform_nodes_;                                        // 방금 읽은 *TRANSFORM 의 노드
  std::map<std::string, Id> part_id_, elset_id_, surface_id_;             // 대문자 이름 → 객체
  mutable std::map<std::string, Id> nset_id_;                              // Nall 은 처음 가리킬 때 만든다(const 조회에서도)
  std::map<std::string, Id> interaction_id_;                              // 접촉 속성
  std::map<std::pair<Id, Id>, Id> pair_id_;                               // (종속 면, 주 면) → 접촉 쌍
  std::map<std::string, Id> material_id_, orientation_id_, function_id_, time_points_id_;
  std::set<std::string> raw_materials_;
  mutable std::map<std::string, std::set<std::string>> taken_;
  mutable std::set<std::string> taken_ready_;

  Id case_ = 0;
  Id last_step_ = 0;      // 마지막으로 만든 스텝
  bool in_steps_ = false; // 스텝이 하나라도 나왔다(보존한 스텝 포함)
  Ops model_bcs_;
  std::vector<std::size_t> model_bc_cards_;
  std::map<std::string, std::size_t> created_;
  Json preserved_ = Json::array();
};

}  // namespace

Json import_mesh_deck(App& app, const std::string& path, bool merge) {
  Importer imp(app, path);
  Json r = imp.mesh(merge);
  r["ignored"] = imp.ignored();
  return r;
}

Json import_deck(App& app, const std::string& path) {
  if (!app.mesh().empty())
    throw Error("invalid_state", "덱 가져오기는 메시가 빈 모델에서만 할 수 있습니다(메시만 더하려면 mesh.import)");
  Importer imp(app, path);
  Json r = imp.mesh();
  r.update(imp.model());
  return r;
}

}  // namespace ofep
