// MyStran 의 F06(글 결과)을 읽어 CalculiX 식 <job>.frd(절점 결과)와 <job>.dat(고유치·좌굴 표)로 쓴다(D17).
// 결과 읽기·표시·그래프·REST 가 모두 frd 경로를 타므로 변환만 하면 나머지는 그대로다(OpenSees 와 같은 방식).
// 읽는 표(MyStran 19.0.0 으로 확인): D I S P L A C E M E N T S / E I G E N V E C T O R, S P C   F O R C E S, R E A L   E I G E N V A L U E S,
// E L E M E N T   S T R E S S E S(HEXA·PENTA·TETRA 의 GRD 줄 → 절점 평균, QUAD4·TRIA3 의 CENTER 위쪽 섬유 → 절점 평균),
// E L E M E N T   E N G I N E E R I N G   F O R C E S(BAR → 보 절점의 단면력: SZZ 축력, SXX·SYY 전단, SXY 비틀림, SYZ·SZX 모멘트).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "nasa95/app.hpp"
#include "nasa95/error.hpp"
#include "nasa95/mesh.hpp"
#include "nasa95/solver.hpp"

namespace nasa95 {

namespace {

namespace fs = std::filesystem;
using Vec6 = std::array<double, 6>;

std::string squeeze(const std::string& s) {
  std::string out;
  for (char c : s)
    if (c != ' ' && c != '\t' && c != '\r') out += c;
  return out;
}
std::vector<std::string> tokens(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream is(s);
  for (std::string t; is >> t;) out.push_back(t);
  return out;
}
bool is_number(const std::string& t) {
  char* end = nullptr;
  std::strtod(t.c_str(), &end);
  return end && *end == '\0' && !t.empty();
}
double num(const std::string& t) { return std::strtod(t.c_str(), nullptr); }

struct NodeVec {  // 절점 → 6성분(T1 T2 T3 R1 R2 R3)
  std::map<Id, Vec6> at;
};
struct Frame {
  std::string kind;  // static | mode | buckle
  int subcase = 0, mode = 0;
  double value = 0;  // 정적: 서브케이스 번호, 모드: 진동수(Hz), 좌굴: 하중 계수
  NodeVec disp, spc;
  std::map<Id, std::pair<Vec6, int>> stress;  // 절점 → (응력 6성분 합, 개수) — 평균용
  std::map<Id, std::pair<Vec6, int>> beam;    // 보 절점 → 단면력(합, 개수)
};

int frd_type(const ShapeInfo& info) {
  static const std::map<std::string, int> t = {{"hex8", 1}, {"wedge6", 2}, {"tet4", 3}, {"hex20", 4}, {"wedge15", 5}, {"tet10", 6},
                                               {"tri3", 7}, {"tri6", 8}, {"quad4", 9}, {"quad8", 10}, {"line2", 11}, {"line3", 12}};
  auto it = t.find(info.name);
  return it == t.end() ? 0 : it->second;
}

class F06 {
 public:
  explicit F06(const Mesh& m) : m_(m) {}

  void parse(const std::vector<std::string>& lines) {
    for (std::size_t i = 0; i < lines.size(); ++i) {
      const std::string sq = squeeze(lines[i]);
      if (sq.rfind("OUTPUTFORSUBCASE", 0) == 0) {
        subcase_ = std::atoi(sq.c_str() + 16);
        mode_ = 0;
      } else if (sq.rfind("OUTPUTFOREIGENVECTOR", 0) == 0) {
        mode_ = std::atoi(sq.c_str() + 20);
      } else if (sq == "REALEIGENVALUES") {
        i = read_eigenvalues(lines, i + 1);
      } else if (sq == "DISPLACEMENTS" || sq == "EIGENVECTOR") {
        i = read_nodevec(lines, i + 1, true);
      } else if (sq == "SPCFORCES") {
        i = read_nodevec(lines, i + 1, false);
      } else if (sq.rfind("ELEMENTSTRESSESIN", 0) == 0 && i + 1 < lines.size()) {
        const std::string type = squeeze(lines[i + 1]);  // FORELEMENTTYPEHEXA8 …
        if (type.find("HEXA") != std::string::npos || type.find("PENTA") != std::string::npos || type.find("TETRA") != std::string::npos) i = read_solid_stress(lines, i + 2);
        else if (type.find("QUAD4") != std::string::npos || type.find("TRIA3") != std::string::npos) i = read_shell_stress(lines, i + 2);
      } else if (sq.rfind("ELEMENTENGINEERINGFORCES", 0) == 0 && i + 1 < lines.size()) {
        const std::string type = squeeze(lines[i + 1]);
        if (type.find("BAR") != std::string::npos || type.find("BEAM") != std::string::npos) i = read_bar_forces(lines, i + 2);
      }
    }
  }

  Frame& frame() {
    const std::string kind = mode_ > 0 ? (buckling_ ? "buckle" : "mode") : "static";
    const std::pair<int, int> key{mode_ > 0 ? 0 : subcase_, mode_};
    auto it = frames_.find(key);
    if (it == frames_.end()) {
      Frame f;
      f.kind = kind, f.subcase = subcase_, f.mode = mode_;
      f.value = mode_ > 0 ? (mode_ <= static_cast<int>(eigen_.size()) ? eigen_[static_cast<std::size_t>(mode_ - 1)] : mode_) : subcase_;
      it = frames_.emplace(key, std::move(f)).first;
      order_.push_back(key);
    }
    return it->second;
  }

  // 고유치 표: MODE, EXTRACTION, EIGENVALUE, RADIANS, CYCLES, …
  std::size_t read_eigenvalues(const std::vector<std::string>& lines, std::size_t i) {
    for (; i < lines.size(); ++i) {
      const auto t = tokens(lines[i]);
      // 진동: MODE, EXTRACTION, EIGENVALUE, RADIANS, CYCLES … / 좌굴(SOL 105): MODE, EXTRACTION, EIGENVALUE(= 하중 계수) 세 칸
      if (t.size() >= 3 && is_number(t[0]) && is_number(t[1]) && is_number(t[2]) && t[0].find('.') == std::string::npos && t[1].find('.') == std::string::npos) {
        const double ev = num(t[2]);
        const double omega = t.size() >= 4 ? num(t[3]) : (ev > 0 ? std::sqrt(ev) : 0.0);
        const double hz = t.size() >= 5 ? num(t[4]) : omega / (2 * 3.14159265358979323846);
        eigen_rows_.push_back({num(t[0]), ev, omega, hz});
        eigen_.push_back(buckling_ ? ev : hz);  // 좌굴: 고유치 = 하중 계수, 진동: Hz
      } else if (!eigen_rows_.empty() && t.empty()) {
        break;
      } else if (!eigen_rows_.empty() && !t.empty() && !is_number(t[0])) {
        break;
      }
    }
    return i;
  }

  std::size_t read_nodevec(const std::vector<std::string>& lines, std::size_t i, bool disp) {
    Frame& f = frame();
    NodeVec& nv = disp ? f.disp : f.spc;
    bool seen = false;
    for (; i < lines.size(); ++i) {
      const auto t = tokens(lines[i]);
      if (t.size() >= 8 && is_number(t[0]) && t[0].find('.') == std::string::npos && is_number(t[2])) {
        const Id n = static_cast<Id>(std::strtoll(t[0].c_str(), nullptr, 10));
        Vec6 v{};
        for (int k = 0; k < 6; ++k) v[static_cast<std::size_t>(k)] = num(t[static_cast<std::size_t>(2 + k)]);
        nv.at[n] = v;
        seen = true;
      } else if (seen && (t.empty() || t[0].rfind("---", 0) == 0 || t[0] == "MAX*" || t[0] == "MIN*")) {
        break;
      }
    }
    return i;
  }

  // 솔리드 응력: "   eid  CENTER  sxx syy szz txy tyz tzx vm" 다음 "GRD  n  sxx …" 줄들
  std::size_t read_solid_stress(const std::vector<std::string>& lines, std::size_t i) {
    Frame& f = frame();
    bool seen = false;
    for (; i < lines.size(); ++i) {
      const auto t = tokens(lines[i]);
      if (t.size() >= 9 && t[0] == "GRD" && is_number(t[1])) {
        const Id n = static_cast<Id>(std::strtoll(t[1].c_str(), nullptr, 10));
        auto& acc = f.stress[n];
        // MyStran 순서 xx yy zz xy yz zx → frd 순서 xx yy zz xy yz zx(CalculiX 와 같다)
        for (int k = 0; k < 6; ++k) acc.first[static_cast<std::size_t>(k)] += num(t[static_cast<std::size_t>(2 + k)]);
        acc.second++;
        seen = true;
      } else if (t.size() >= 2 && (t[0] == "MAX*" || t[0] == "MIN*" || t[0] == "ABS*")) {
        if (seen) break;
      } else if (seen && t.empty()) {
        // 요소 사이의 빈 줄은 건너뛴다
      }
    }
    return i;
  }

  // 쉘 응력: "  eid  CENTER  fiber  nx ny nxy angle major minor vm …" + 둘째 섬유 줄. 위쪽 섬유(+1)를 요소 절점에 평균
  std::size_t read_shell_stress(const std::vector<std::string>& lines, std::size_t i) {
    Frame& f = frame();
    Id eid = 0;
    bool seen = false;
    for (; i < lines.size(); ++i) {
      const auto t = tokens(lines[i]);
      if (t.size() >= 9 && is_number(t[0]) && t[0].find('.') == std::string::npos && t[1] == "CENTER") {
        eid = static_cast<Id>(std::strtoll(t[0].c_str(), nullptr, 10));
        seen = true;  // 첫 섬유(아래, -1) — 다음 줄의 위쪽 섬유를 쓴다
      } else if (seen && eid && t.size() >= 7 && is_number(t[0]) && num(t[0]) > 0 && is_number(t[1])) {
        if (!m_.has_element(eid)) continue;
        const Element e = m_.element(eid);
        Vec6 v{num(t[1]), num(t[2]), 0.0, num(t[3]), 0.0, 0.0};
        for (Id n : e.nodes) {
          auto& acc = f.stress[n];
          for (int k = 0; k < 6; ++k) acc.first[static_cast<std::size_t>(k)] += v[static_cast<std::size_t>(k)];
          acc.second++;
        }
        eid = 0;
      } else if (t.size() >= 2 && (t[0] == "MAX*" || t[0] == "MIN*" || t[0] == "ABS*")) {
        if (seen) break;
      }
    }
    return i;
  }

  // 보 단면력: eid, M1A, M2A, M1B, M2B, V1, V2, Axial, Torque → 끝 A·B 절점에 (SZZ 축력, SXX V1, SYY V2, SXY 비틀림, SYZ M1, SZX M2)
  std::size_t read_bar_forces(const std::vector<std::string>& lines, std::size_t i) {
    Frame& f = frame();
    bool seen = false;
    for (; i < lines.size(); ++i) {
      const auto t = tokens(lines[i]);
      if (t.size() >= 9 && is_number(t[0]) && t[0].find('.') == std::string::npos) {
        const Id eid = static_cast<Id>(std::strtoll(t[0].c_str(), nullptr, 10));
        if (!m_.has_element(eid)) continue;
        const Element e = m_.element(eid);
        if (e.nodes.size() < 2) continue;
        const double m1a = num(t[1]), m2a = num(t[2]), m1b = num(t[3]), m2b = num(t[4]), v1 = num(t[5]), v2 = num(t[6]), ax = num(t[7]), tq = num(t[8]);
        for (int end = 0; end < 2; ++end) {
          auto& acc = f.beam[e.nodes[static_cast<std::size_t>(end)]];
          const Vec6 v{v1, v2, ax, tq, end == 0 ? m1a : m1b, end == 0 ? m2a : m2b};
          for (int k = 0; k < 6; ++k) acc.first[static_cast<std::size_t>(k)] += v[static_cast<std::size_t>(k)];
          acc.second++;
        }
        seen = true;
      } else if (t.size() >= 2 && (t[0] == "MAX*" || t[0] == "MIN*" || t[0] == "ABS*")) {
        if (seen) break;
      }
    }
    return i;
  }

  void set_buckling(bool b) { buckling_ = b; }
  const std::vector<std::pair<int, int>>& order() const { return order_; }
  Frame& at(const std::pair<int, int>& key) { return frames_.at(key); }
  const std::vector<std::array<double, 4>>& eigen_rows() const { return eigen_rows_; }

 private:
  const Mesh& m_;
  int subcase_ = 1, mode_ = 0;
  bool buckling_ = false;
  std::vector<double> eigen_;
  std::vector<std::array<double, 4>> eigen_rows_;
  std::map<std::pair<int, int>, Frame> frames_;
  std::vector<std::pair<int, int>> order_;
};

std::string fmtE(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%12.5E", v);
  return buf;
}

}  // namespace

int convert_f06_to_frd(const App& a, const Object& cs, const std::string& work, const std::string& job) {
  const fs::path dir = fs::path(std::u8string(work.begin(), work.end()));
  const fs::path f06 = dir / (job + ".F06");
  std::ifstream in(f06);
  if (!in) throw Error("not_found", "F06 결과 파일이 없습니다: " + job + ".F06", {{"path", job + ".F06"}});
  std::vector<std::string> lines;
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(line);
  }
  // 솔버 오류는 F06 에만 찍힌다(표준 출력은 진행 메시지뿐) → 로그로 올려 case.run 상태가 failed 가 되게 한다
  for (const std::string& line : lines)
    if (line.find("*ERROR") != std::string::npos || line.find("*FATAL") != std::string::npos)
      throw Error("solver_error", "MyStran: " + line.substr(line.find('*')), {{"path", job + ".F06"}});
  const Mesh& m = a.mesh();
  F06 p(m);
  bool buckling = false;
  for (const Object* s : a.model().children(cs.id, "step"))
    if (!s->suppressed && s->props.value("type", std::string()) == "buckle") buckling = true;
  p.set_buckling(buckling);
  p.parse(lines);
  // GRID CD 가 있는 절점의 변위·SPC 힘은 국부 성분으로 나온다 → 좌표계 축으로 전역으로 돌린다(원통 좌표계는 절점 위치의 축)
  const std::map<Id, Id> local = mystran_node_csys(a, cs);
  if (!local.empty())
    for (const auto& key : p.order()) {
      Frame& fr = p.at(key);
      for (NodeVec* nv : {&fr.disp, &fr.spc})
        for (auto& [n, v] : nv->at) {
          auto it = local.find(n);
          const Object* csys = it == local.end() ? nullptr : a.model().find(it->second);
          if (!csys || !m.has_node(n)) continue;
          Vec3 ax[3];
          csys_axes_at(*csys, m.node(n), ax);
          Vec6 g{};
          for (int c = 0; c < 3; ++c)
            for (int k = 0; k < 3; ++k) g[static_cast<std::size_t>(c)] += ax[k][static_cast<std::size_t>(c)] * v[static_cast<std::size_t>(k)], g[static_cast<std::size_t>(c + 3)] += ax[k][static_cast<std::size_t>(c)] * v[static_cast<std::size_t>(k + 3)];
          v = g;
        }
    }
  // frd 쓰기: 절점(케이스 범위), 요소, 프레임마다 DISP·FORC·STRESS
  std::set<Id> nodes;
  std::vector<std::size_t> elems;
  std::set<Id> scope;
  if (cs.props.contains("scope") && !cs.props["scope"].is_null())
    for (const Json& e : resolve_target(a, cs.props["scope"], "elements")) scope.insert(e.get<Id>());
  for (std::size_t i = 0; i < m.element_count(); ++i) {
    if (!scope.empty() && !scope.count(m.element_ids()[i])) continue;
    if (!frd_type(shape_info(m.shape_at(i)))) continue;
    elems.push_back(i);
    for (std::size_t k = 0; k < m.node_count_at(i); ++k) nodes.insert(m.nodes_at(i)[k]);
  }
  if (nodes.empty())
    for (Id n : m.node_ids()) nodes.insert(n);
  std::ofstream out(dir / (job + ".frd"), std::ios::binary);
  if (!out) throw Error("io_error", "frd 를 쓸 수 없습니다", {{"path", job + ".frd"}});
  out << "    1C" << job << "\n";
  out << "    1UUSER\n    1UPGM               MyStran (NASA-95 F06 convert)\n";
  char buf[160];
  std::snprintf(buf, sizeof buf, "    2C                  %12zu                                     1\n", nodes.size());
  out << buf;
  for (Id n : nodes) {
    const Vec3 c = m.node(n);
    std::snprintf(buf, sizeof buf, " -1%10lld%s%s%s\n", static_cast<long long>(n), fmtE(c[0]).c_str(), fmtE(c[1]).c_str(), fmtE(c[2]).c_str());
    out << buf;
  }
  out << " -3\n";
  std::snprintf(buf, sizeof buf, "    3C                  %12zu                                     1\n", elems.size());
  out << buf;
  for (std::size_t i : elems) {
    const ShapeInfo& info = shape_info(m.shape_at(i));
    std::snprintf(buf, sizeof buf, " -1%10lld%5d%5d%5d\n", static_cast<long long>(m.element_ids()[i]), frd_type(info), 0, 1);
    out << buf;
    std::string line = " -2";
    const Id* nn = m.nodes_at(i);
    int k = 0;
    for (std::size_t c = 0; c < m.node_count_at(i); ++c) {
      std::snprintf(buf, sizeof buf, "%10lld", static_cast<long long>(nn[c]));
      line += buf;
      if (++k == 10 && c + 1 < m.node_count_at(i)) out << line << "\n", line = " -2", k = 0;
    }
    out << line << "\n";
  }
  out << " -3\n";
  int blocks = 0, frame_no = 0;
  auto block = [&](const Frame& fr, const std::string& name, const std::vector<std::string>& comps, int ictype, const std::map<Id, Vec6>& values, int ncomp, int step, int inc) {
    std::snprintf(buf, sizeof buf, "    1PSTEP%19d%12d%12d\n", frame_no, inc, step);
    out << buf;
    std::snprintf(buf, sizeof buf, "  100CL  101%s%12zu                    %2d%5d          1\n", fmtE(fr.value).c_str(), nodes.size(), ictype, frame_no);
    out << buf;
    std::snprintf(buf, sizeof buf, " -4  %-8s%5d    1\n", name.c_str(), static_cast<int>(comps.size()) + 1);
    out << buf;
    int k = 0;
    for (const std::string& c : comps) {
      std::snprintf(buf, sizeof buf, " -5  %-8s    1    %d%5d    0\n", c.c_str(), ncomp == 6 ? 4 : 2, ++k);
      out << buf;
    }
    out << " -5  ALL         1    2    0    0    1ALL\n";
    for (Id n : nodes) {
      auto it = values.find(n);
      Vec6 v{};
      if (it != values.end()) v = it->second;
      std::string line = " -1";
      std::snprintf(buf, sizeof buf, "%10lld", static_cast<long long>(n));
      line += buf;
      for (int c = 0; c < static_cast<int>(comps.size()); ++c) line += fmtE(v[static_cast<std::size_t>(c)]);
      out << line << "\n";
    }
    out << " -3\n";
    ++blocks;
  };
  for (const auto& key : p.order()) {
    Frame& fr = p.at(key);
    ++frame_no;
    const int ictype = fr.kind == "static" ? 0 : 2;
    const int step = fr.kind == "static" ? fr.subcase : 1, inc = fr.kind == "static" ? 1 : fr.mode;
    if (fr.kind != "static") {
      std::snprintf(buf, sizeof buf, "    1PGM%21.5E\n", 1.0);
      out << buf;
      std::snprintf(buf, sizeof buf, "    1PMODE%20d\n", fr.mode);
      out << buf;
    }
    block(fr, "DISP", {"D1", "D2", "D3"}, ictype, fr.disp.at, 3, step, inc);
    if (!fr.spc.at.empty()) block(fr, "FORC", {"F1", "F2", "F3"}, ictype, fr.spc.at, 3, step, inc);
    std::map<Id, Vec6> stress;
    for (const auto& [n, acc] : fr.stress) {
      Vec6 v{};
      for (int k = 0; k < 6; ++k) v[static_cast<std::size_t>(k)] = acc.first[static_cast<std::size_t>(k)] / acc.second;
      stress[n] = v;
    }
    for (const auto& [n, acc] : fr.beam) {  // 보 절점: 단면력을 STRESS 자리에(ccx 의 SECTION FORCES 와 같은 배치)
      if (stress.count(n)) continue;
      Vec6 v{};
      for (int k = 0; k < 6; ++k) v[static_cast<std::size_t>(k)] = acc.first[static_cast<std::size_t>(k)] / acc.second;
      stress[n] = v;
    }
    if (!stress.empty()) block(fr, "STRESS", {"SXX", "SYY", "SZZ", "SXY", "SYZ", "SZX"}, ictype, stress, 6, step, inc);
  }
  out << " 9999\n";
  // dat: 고유치·좌굴 표(result.modal_summary / buckling_summary 가 읽는다)
  if (!p.eigen_rows().empty()) {
    std::ofstream dat(dir / (job + ".dat"));
    dat << "\n     STEP   1\n\n";
    if (buckling) {
      dat << "     B U C K L I N G   F A C T O R   O U T P U T\n\n     MODE NO       BUCKLING FACTOR\n\n";
      for (const auto& r : p.eigen_rows()) {
        std::snprintf(buf, sizeof buf, "      %7d  %18.7E\n", static_cast<int>(r[0]), r[1]);
        dat << buf;
      }
    } else {
      dat << "     E I G E N V A L U E   O U T P U T\n\n MODE NO    EIGENVALUE                       FREQUENCY\n                              REAL PART            IMAGINARY PART\n                      (RAD/TIME)      (CYCLES/TIME)\n\n";
      for (const auto& r : p.eigen_rows()) {
        std::snprintf(buf, sizeof buf, "      %7d  %16.7E  %16.7E  %16.7E  %16.7E\n", static_cast<int>(r[0]), r[1], r[2], r[3], 0.0);
        dat << buf;
      }
    }
  }
  return blocks;
}

}  // namespace nasa95
