// CalculiX 결과 파일(.frd, ASCII) 읽기 (RES-01, RES-27).
// 열 때는 파일을 한 번 훑어 메시와 결과 목록(프레임·필드·파일 위치)만 만들고, 값은 필요할 때 읽는다(RES-03).
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "nasa95/error.hpp"
#include "nasa95/results.hpp"

namespace nasa95 {

namespace {

std::filesystem::path fs_path(const std::string& utf8) { return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())); }

std::string trim(const std::string& s) {
  const std::size_t b = s.find_first_not_of(" \t\r");
  return b == std::string::npos ? "" : s.substr(b, s.find_last_not_of(" \t\r") - b + 1);
}
std::string cut(const std::string& s, std::size_t pos, std::size_t len) { return pos >= s.size() ? "" : trim(s.substr(pos, len)); }
long long to_int(const std::string& s) { return s.empty() ? 0 : std::strtoll(s.c_str(), nullptr, 10); }

// 고정 폭 실수. 지수의 E 가 빠진 표기("1.00000-100")도 읽는다.
double to_real(const std::string& text) {
  std::string s = trim(text);
  if (s.empty()) return 0.0;
  for (std::size_t i = 1; i < s.size(); ++i)
    if ((s[i] == '-' || s[i] == '+') && s[i - 1] != 'E' && s[i - 1] != 'e' && s[i - 1] != 'D' && s[i - 1] != 'd') {
      s.insert(i, "E");
      break;
    }
  std::replace(s.begin(), s.end(), 'D', 'E');
  return std::strtod(s.c_str(), nullptr);
}

bool getline_clean(std::ifstream& f, std::string& line) {
  if (!std::getline(f, line)) return false;
  if (!line.empty() && line.back() == '\r') line.pop_back();
  return true;
}

[[noreturn]] void bad(const std::string& path, std::size_t line, const std::string& what) {
  throw Error("parse_error", "결과 파일을 읽을 수 없습니다: " + what, {{"path", path}, {"line", line}});
}

const char* frame_type(int ictype) {
  switch (ictype) {
    case 0: return "static";
    case 1: return "time";
    case 2: return "frequency";
    case 3: return "load_step";
    default: return "user";
  }
}

// 값 줄을 읽는다: " -1" + 절점 번호 + 값들(한 줄에 6개, 이어지는 줄은 " -2").
void read_values(std::ifstream& f, ResultField& fld, const std::string& path) {
  const std::size_t nc = fld.components.size();
  if (fld.format >= 2) {
    fld.ids.assign(fld.count, 0), fld.data.assign(fld.count * nc, 0.0);
    for (std::size_t i = 0; i < fld.count; ++i) {
      std::int32_t id = 0;
      f.read(reinterpret_cast<char*>(&id), 4);
      fld.ids[i] = static_cast<Id>(id);
      if (fld.format == 2) {
        std::vector<float> v(nc);
        f.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(nc * 4));
        for (std::size_t c = 0; c < nc; ++c) fld.data[i * nc + c] = v[c];
      } else {
        f.read(reinterpret_cast<char*>(&fld.data[i * nc]), static_cast<std::streamsize>(nc * 8));
      }
      if (!f) bad(path, 0, "이진 값 블록이 짧습니다(" + fld.name + ")");
    }
    fld.loaded = true;
    return;
  }
  const std::size_t id_width = fld.format == 0 ? 5 : 10;
  fld.ids.clear(), fld.data.clear();
  fld.ids.reserve(fld.count), fld.data.reserve(fld.count * nc);
  std::string line;
  std::size_t got = 0;  // 지금 절점에서 읽은 성분 수
  while (getline_clean(f, line)) {
    const std::string key = line.substr(0, std::min<std::size_t>(3, line.size()));
    if (key == " -3") break;
    std::size_t pos = 3 + id_width;
    if (key == " -1") {
      if (got != 0 && got != nc) bad(path, 0, "성분 수가 맞지 않습니다(" + fld.name + ")");
      fld.ids.push_back(static_cast<Id>(to_int(cut(line, 3, id_width))));
      got = 0;
    } else if (key != " -2") {
      bad(path, 0, "값 줄이 아닙니다(" + fld.name + "): " + line.substr(0, 20));
    }
    for (; pos + 12 <= line.size() && got < nc; pos += 12, ++got) fld.data.push_back(to_real(line.substr(pos, 12)));
  }
  if (fld.data.size() != fld.ids.size() * nc) bad(path, 0, "값의 개수가 맞지 않습니다(" + fld.name + ")");
  fld.count = fld.ids.size();
  fld.loaded = true;
}

}  // namespace

std::shared_ptr<ResultFile> ResultStore::open(const std::string& path) {
  std::ifstream f(fs_path(path), std::ios::binary);
  if (!f) throw Error("io_error", "결과 파일을 열 수 없습니다: " + path, {{"path", path}});
  auto file = std::make_shared<ResultFile>();
  file->path = path;
  std::string line;
  std::size_t n = 0;
  std::map<std::string, std::string> attrs;  // 다음 결과 블록에 붙을 1P 줄
  int last_key = -1;
  double last_value = 0;
  bool finished = false;
  while (getline_clean(f, line)) {
    ++n;
    const std::string key = line.substr(0, std::min<std::size_t>(6, line.size()));
    if (key == "    1U") {
      const std::string name = cut(line, 6, 14);
      if (!name.empty()) file->header[name] = cut(line, 20, 60);
    } else if (key == "    2C") {
      const int format = static_cast<int>(to_int(cut(line, 73, 2)));
      if (format >= 2) {  // 이진: 노드 번호(int32) + 좌표 3개(double)
        const std::size_t count = static_cast<std::size_t>(to_int(cut(line, 24, 12)));
        for (std::size_t i = 0; i < count; ++i) {
          std::int32_t id = 0;
          double xyz[3];
          f.read(reinterpret_cast<char*>(&id), 4), f.read(reinterpret_cast<char*>(xyz), 24);
          if (!f) bad(path, n, "이진 노드 블록이 짧습니다");
          file->node_ids.push_back(static_cast<Id>(id));
          file->node_xyz.insert(file->node_xyz.end(), xyz, xyz + 3);
        }
        continue;
      }
      const std::size_t id_width = format == 0 ? 5 : 10;
      while (getline_clean(f, line)) {
        ++n;
        if (line.rfind(" -3", 0) == 0) break;
        if (line.rfind(" -1", 0) != 0) bad(path, n, "노드 줄이 아닙니다");
        file->node_ids.push_back(static_cast<Id>(to_int(cut(line, 3, id_width))));
        for (int k = 0; k < 3; ++k) file->node_xyz.push_back(to_real(line.substr(std::min(line.size(), 3 + id_width + 12 * static_cast<std::size_t>(k)), 12)));
      }
    } else if (key == "    3C") {
      const int format = static_cast<int>(to_int(cut(line, 73, 2)));
      if (format >= 2) {  // 이진: 번호·타입·그룹·재료(int32) + 절점(int32 × 타입별 개수)
        static const int nodes_of_type[] = {0, 8, 6, 4, 20, 15, 10, 3, 6, 4, 8, 2, 3};
        const std::size_t count = static_cast<std::size_t>(to_int(cut(line, 24, 12)));
        for (std::size_t i = 0; i < count; ++i) {
          std::int32_t head[4];
          f.read(reinterpret_cast<char*>(head), 16);
          if (!f || head[1] < 1 || head[1] > 12) bad(path, n, "이진 요소 블록을 읽을 수 없습니다");
          ResultElement e;
          e.id = static_cast<Id>(head[0]), e.type = head[1];
          std::vector<std::int32_t> nodes(static_cast<std::size_t>(nodes_of_type[head[1]]));
          f.read(reinterpret_cast<char*>(nodes.data()), static_cast<std::streamsize>(nodes.size() * 4));
          for (std::int32_t v : nodes) e.nodes.push_back(static_cast<Id>(v));
          file->elements.push_back(std::move(e));
        }
        continue;
      }
      const std::size_t w = format == 0 ? 5 : 10;
      while (getline_clean(f, line)) {
        ++n;
        if (line.rfind(" -3", 0) == 0) break;
        if (line.rfind(" -1", 0) == 0) {
          ResultElement e;
          e.id = static_cast<Id>(to_int(cut(line, 3, w)));
          e.type = static_cast<int>(to_int(cut(line, 3 + w, 5)));
          file->elements.push_back(std::move(e));
        } else if (line.rfind(" -2", 0) == 0 && !file->elements.empty()) {
          for (std::size_t pos = 3; pos + w <= line.size(); pos += w) {
            const std::string cell = cut(line, pos, w);
            if (!cell.empty()) file->elements.back().nodes.push_back(static_cast<Id>(to_int(cell)));
          }
        } else {
          bad(path, n, "요소 줄이 아닙니다");
        }
      }
    } else if (key == "    1P") {
      // "    1PSTEP        n  inc  step": 이름과 그 뒤의 값
      const std::string rest = line.substr(6);
      const std::size_t sp = rest.find(' ');
      attrs[rest.substr(0, sp)] = sp == std::string::npos ? "" : trim(rest.substr(sp));
    } else if (key == "  100C") {
      const double value = to_real(line.substr(std::min<std::size_t>(12, line.size()), 12));
      const std::size_t numnod = static_cast<std::size_t>(to_int(cut(line, 24, 12)));
      const int ictype = static_cast<int>(to_int(cut(line, 56, 2)));
      const int numstp = static_cast<int>(to_int(cut(line, 58, 5)));
      const int format = static_cast<int>(to_int(cut(line, 73, 2)));
      if (file->frames.empty() || numstp != last_key || value != last_value) {
        ResultFrame fr;
        fr.index = static_cast<int>(file->frames.size()) + 1;
        fr.value = value, fr.type = frame_type(ictype);
        if (auto it = attrs.find("STEP"); it != attrs.end()) {  // (블록 번호, 증분, 스텝)
          long long a = 0, inc = 0, step = 0;
          std::istringstream(it->second) >> a >> inc >> step;
          fr.increment = static_cast<int>(inc), fr.step = static_cast<int>(step);
        }
        file->frames.push_back(std::move(fr));
        last_key = numstp, last_value = value;
      }
      ResultFrame& fr = file->frames.back();
      for (const auto& [k, v] : attrs)
        if (k != "STEP") fr.attributes[k] = v;
      attrs.clear();
      // 필드 머리: -4 이름·성분 수, -5 성분들
      if (!getline_clean(f, line) || line.rfind(" -4", 0) != 0) bad(path, n + 1, "결과 이름 줄(-4)이 없습니다");
      ++n;
      ResultField fld;
      fld.name = cut(line, 5, 8);
      const int ncomp = static_cast<int>(to_int(cut(line, 13, 5)));
      fld.count = numnod, fld.format = format;
      for (int c = 0; c < ncomp; ++c) {
        if (!getline_clean(f, line) || line.rfind(" -5", 0) != 0) bad(path, n + 1, "성분 줄(-5)이 없습니다");
        ++n;
        const int ictype5 = static_cast<int>(to_int(cut(line, 18, 5)));
        const int exist = static_cast<int>(to_int(cut(line, 33, 5)));
        if (exist == 0) {  // 1 이면 파일에 값이 없는 성분(표시 프로그램이 계산하는 크기 등)
          fld.components.push_back(cut(line, 5, 8));
          fld.entity = ictype5;
        }
      }
      fld.offset = static_cast<long long>(f.tellg());
      if (format >= 2) {  // 이진: 노드 번호(int32) + 값(format 2 는 float, 3 은 double). 끝 표시(-3)가 없다
        const std::streamoff bytes = static_cast<std::streamoff>(numnod) * static_cast<std::streamoff>(4 + fld.components.size() * (format == 2 ? 4 : 8));
        f.seekg(bytes, std::ios::cur);
      } else {  // 값은 건너뛴다
        while (getline_clean(f, line)) {
          ++n;
          if (line.rfind(" -3", 0) == 0) break;
        }
      }
      fr.fields.push_back(std::move(fld));
    } else if (key.rfind(" 9999", 0) == 0) {
      finished = true;
      break;
    }
  }
  if (file->node_ids.empty()) throw Error("parse_error", "결과 파일에 노드가 없습니다", {{"path", path}});
  file->header["complete"] = finished ? "yes" : "no";  // 솔버가 아직 쓰는 중이면 끝 표시(9999)가 없다
  file->id = next_++;
  files_[file->id] = file;
  return file;
}

ResultFile& ResultStore::get(Id id) {
  auto it = files_.find(id);
  if (it == files_.end()) throw Error("not_found", "열려 있지 않은 결과입니다: " + std::to_string(id), {{"result", id}});
  return *it->second;
}

void ResultStore::close(Id id) {
  if (!files_.erase(id)) throw Error("not_found", "열려 있지 않은 결과입니다: " + std::to_string(id), {{"result", id}});
}

ResultFrame& ResultFile::frame(int index) {
  if (index < 1 || index > static_cast<int>(frames.size()))
    throw Error("out_of_range", "없는 프레임입니다: " + std::to_string(index), {{"param", "frame"}, {"count", frames.size()}});
  return frames[static_cast<std::size_t>(index - 1)];
}

ResultField& ResultFile::field(ResultFrame& fr, const std::string& name) {
  for (ResultField& f : fr.fields) {
    if (f.name != name) continue;
    if (!f.loaded) {
      std::ifstream in(fs_path(path), std::ios::binary);
      if (!in) throw Error("io_error", "결과 파일을 열 수 없습니다: " + path, {{"path", path}});
      in.seekg(f.offset);
      read_values(in, f, path);
      // 단위 환산(CMN-13): 결과 종류의 차원에 맞는 계수를 곱한다(모르는 종류는 그대로)
      if (!unit_factors.empty()) {
        static const std::map<std::string, std::string> dims = {
            {"DISP", "length"}, {"PDISP", "length"}, {"DISPI", "length"}, {"COPEN", "length"}, {"STRESS", "pressure"}, {"STRESSI", "pressure"},
            {"PSTRESS", "pressure"}, {"ZZSTR", "pressure"}, {"CPRESS", "pressure"}, {"CSHEAR", "pressure"}, {"ENER", "pressure"},
            {"FORC", "force"}, {"FORCI", "force"}, {"CFORC", "force"}, {"RFL", "power"}, {"FLUX", "heat_flux"}, {"VELO", "velocity"},
            {"NDTEMP", "temperature"}, {"SECTION", "force"}};
        auto d = dims.find(f.name);
        if (d != dims.end()) {
          auto k = unit_factors.find(d->second);
          if (k != unit_factors.end() && k->second != 1.0)
            for (double& v : f.data) v *= k->second;
        }
      }
    }
    return f;
  }
  Json names = Json::array();
  for (const ResultField& f : fr.fields) names.push_back(f.name);
  throw Error("not_found", "이 프레임에 없는 결과입니다: " + name, {{"param", "field"}, {"available", names}});
}

ResultStore& results(App& app) {
  std::any& slot = app.runtime("results");
  if (!slot.has_value()) slot = std::make_shared<ResultStore>();
  return *std::any_cast<std::shared_ptr<ResultStore>&>(slot);
}

// ------------------------------------------------------------------ 파생량
std::vector<std::string> derived_names(const ResultField& f) {
  if (f.components.size() == 3 && f.entity == 2) return {"magnitude"};
  if (f.components.size() == 6 && f.entity == 4) return {"mises", "tresca", "p1", "p2", "p3", "pressure"};
  return {};
}

std::vector<double> derived_values(const ResultField& f, const std::string& name) {
  const auto names = derived_names(f);
  if (std::find(names.begin(), names.end(), name) == names.end())
    throw Error("not_found", f.name + " 에 없는 파생량입니다: " + name, {{"param", "component"}, {"available", names}});
  const std::size_t nc = f.components.size();
  std::vector<double> out(f.count);
  const double pi = 3.14159265358979323846;
  for (std::size_t i = 0; i < f.count; ++i) {
    const double* v = &f.data[i * nc];
    if (name == "magnitude") {
      out[i] = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
      continue;
    }
    // 텐서: xx, yy, zz, xy, yz, zx
    const double xx = v[0], yy = v[1], zz = v[2], xy = v[3], yz = v[4], zx = v[5];
    const double mean = (xx + yy + zz) / 3.0;
    if (name == "pressure") {
      out[i] = -mean;
      continue;
    }
    const double j2 = ((xx - yy) * (xx - yy) + (yy - zz) * (yy - zz) + (zz - xx) * (zz - xx)) / 6.0 + xy * xy + yz * yz + zx * zx;
    if (name == "mises") {
      out[i] = std::sqrt(3.0 * j2);
      continue;
    }
    // 주값: 편차 텐서의 세 고유값(삼각함수 풀이)
    double p1 = mean, p2 = mean, p3 = mean;
    if (j2 > 0) {
      const double sx = xx - mean, sy = yy - mean, sz = zz - mean;
      const double j3 = sx * sy * sz + 2 * xy * yz * zx - sx * yz * yz - sy * zx * zx - sz * xy * xy;
      const double r = std::sqrt(j2 / 3.0);
      const double c = std::clamp(j3 / (2.0 * r * r * r), -1.0, 1.0);
      const double th = std::acos(c) / 3.0;
      p1 = mean + 2 * r * std::cos(th);
      p2 = mean + 2 * r * std::cos(th - 2 * pi / 3);
      p3 = mean + 2 * r * std::cos(th + 2 * pi / 3);
    }
    out[i] = name == "p1" ? p1 : name == "p2" ? p2 : name == "p3" ? p3 : (p1 - p3);  // tresca = 최대 − 최소 주값
  }
  return out;
}

}  // namespace nasa95
