// 스케치 구속 해석기(D8: 자체 구현). sketch_solver.hpp 참고.
#include "sketch_solver.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>

#include "ofep/error.hpp"

namespace ofep {

namespace {

constexpr double kPi = 3.14159265358979323846;

// 요소마다 변수 자리: 요소 번호 → (좌표 키, 변수 시작 인덱스). 참조 요소는 변수가 없다(고정).
struct Slot {
  std::string key;  // "start", "end", "center", "middle", "position", "radius", "corner", "size", "radii", "angle", "points"
  int index = -1;   // 변수 벡터의 시작 위치(-1 = 고정)
  int count = 0;
};

struct Layout {
  std::map<int, std::map<std::string, Slot>> slots;  // 요소 번호 → 키 → 자리
  std::map<int, const Json*> entity;                 // 요소 번호 → 요소
  std::vector<double> x0;                            // 초기 변수 값
};

std::vector<std::string> coordinate_keys(const Json& e) {
  const std::string kind = e.value("kind", std::string());
  if (kind == "line") return {"start", "end"};
  if (kind == "circle") return {"center", "radius"};
  if (kind == "arc") return {"start", "middle", "end"};
  if (kind == "rectangle") return {"corner", "size"};
  if (kind == "ellipse") return {"center", "radii", "angle"};
  if (kind == "spline") return {"points"};
  if (kind == "point") return {"position"};
  return {};
}

Layout make_layout(const Json& entities) {
  Layout L;
  for (const Json& e : entities) {
    const int id = e.value("id", 0);
    L.entity[id] = &e;
    const bool fixed = e.value("reference", false);
    for (const std::string& key : coordinate_keys(e)) {
      if (!e.contains(key)) continue;
      Slot s;
      s.key = key;
      std::vector<double> vals;
      const Json& v = e[key];
      if (v.is_number()) vals.push_back(v.get<double>());
      else if (v.is_array() && !v.empty() && v[0].is_array()) {
        for (const Json& p : v) vals.push_back(p[0].get<double>()), vals.push_back(p[1].get<double>());
      } else if (v.is_array()) {
        for (const Json& x : v) vals.push_back(x.get<double>());
      }
      s.count = static_cast<int>(vals.size());
      if (!fixed) {
        s.index = static_cast<int>(L.x0.size());
        L.x0.insert(L.x0.end(), vals.begin(), vals.end());
      }
      L.slots[id][key] = s;
    }
  }
  return L;
}

// 변수 벡터에서 (요소, 키) 의 값을 읽는다(고정이면 요소에서).
struct Reader {
  const Layout& L;
  const std::vector<double>& x;
  double at(int id, const std::string& key, int k) const {
    auto ei = L.slots.find(id);
    if (ei == L.slots.end()) throw Error("not_found", "스케치에 없는 요소입니다: " + std::to_string(id), {{"entity", id}});
    auto si = ei->second.find(key);
    if (si == ei->second.end()) throw Error("invalid_param", "요소 " + std::to_string(id) + " 에 " + key + " 가 없습니다", {{"entity", id}, {"key", key}});
    if (si->second.index >= 0) return x[static_cast<std::size_t>(si->second.index + k)];
    const Json& v = (*L.entity.at(id))[key];
    if (v.is_number()) return v.get<double>();
    if (v.is_array() && !v.empty() && v[0].is_array()) return v[k / 2][k % 2].get<double>();
    return v[static_cast<std::size_t>(k)].get<double>();
  }
  std::string kind(int id) const {
    auto it = L.entity.find(id);
    if (it == L.entity.end()) throw Error("not_found", "스케치에 없는 요소입니다: " + std::to_string(id), {{"entity", id}});
    return it->second->value("kind", std::string());
  }
};

struct P2 {
  double x, y;
};
P2 operator-(const P2& a, const P2& b) { return {a.x - b.x, a.y - b.y}; }
P2 operator+(const P2& a, const P2& b) { return {a.x + b.x, a.y + b.y}; }
P2 operator*(const P2& a, double s) { return {a.x * s, a.y * s}; }
double dot(const P2& a, const P2& b) { return a.x * b.x + a.y * b.y; }
double cross(const P2& a, const P2& b) { return a.x * b.y - a.y * b.x; }
double norm(const P2& a) { return std::sqrt(dot(a, a)); }

// 구속의 점 지정: {entity, point} (point: start|end|center|middle|position|corner) 또는 [요소, 점] 또는 요소 번호 하나
P2 point_of(const Reader& r, const Json& spec) {
  int id;
  std::string key;
  if (spec.is_object()) {
    if (!spec.contains("entity")) throw Error("missing_param", "점 지정에는 entity 가 필요합니다", {{"param", "points"}});
    id = spec["entity"].get<int>();
    key = spec.value("point", std::string());
  } else if (spec.is_array()) {
    id = spec[0].get<int>();
    key = spec.size() > 1 ? spec[1].get<std::string>() : std::string();
  } else {
    id = spec.get<int>();
  }
  const std::string kind = r.kind(id);
  if (key.empty()) {
    if (kind == "point") key = "position";
    else if (kind == "circle" || kind == "ellipse") key = "center";
    else if (kind == "rectangle") key = "corner";
    else throw Error("invalid_param", "요소 " + std::to_string(id) + " 의 어느 점인지 지정해야 합니다(start·end·center·middle)", {{"entity", id}});
  }
  if (kind == "arc" && key == "center") {  // 세 점으로 정한 원호의 중심
    const P2 a{r.at(id, "start", 0), r.at(id, "start", 1)}, b{r.at(id, "middle", 0), r.at(id, "middle", 1)}, c{r.at(id, "end", 0), r.at(id, "end", 1)};
    const double d = 2 * (a.x * (b.y - c.y) + b.x * (c.y - a.y) + c.x * (a.y - b.y));
    if (std::fabs(d) < 1e-300) return b;
    const double ux = ((a.x * a.x + a.y * a.y) * (b.y - c.y) + (b.x * b.x + b.y * b.y) * (c.y - a.y) + (c.x * c.x + c.y * c.y) * (a.y - b.y)) / d;
    const double uy = ((a.x * a.x + a.y * a.y) * (c.x - b.x) + (b.x * b.x + b.y * b.y) * (a.x - c.x) + (c.x * c.x + c.y * c.y) * (b.x - a.x)) / d;
    return {ux, uy};
  }
  return {r.at(id, key, 0), r.at(id, key, 1)};
}

// 선분(또는 사각형 변)의 방향 벡터
P2 direction_of(const Reader& r, int id) {
  const std::string kind = r.kind(id);
  if (kind != "line") throw Error("invalid_param", "선분이 아닙니다: " + std::to_string(id), {{"entity", id}});
  return P2{r.at(id, "end", 0), r.at(id, "end", 1)} - P2{r.at(id, "start", 0), r.at(id, "start", 1)};
}

// 원·원호의 중심과 반지름
void circle_of(const Reader& r, int id, P2& c, double& rad) {
  const std::string kind = r.kind(id);
  if (kind == "circle") {
    c = {r.at(id, "center", 0), r.at(id, "center", 1)}, rad = r.at(id, "radius", 0);
  } else if (kind == "arc") {
    c = point_of(r, Json::array({id, "center"}));
    rad = norm(P2{r.at(id, "start", 0), r.at(id, "start", 1)} - c);
  } else {
    throw Error("invalid_param", "원이나 원호가 아닙니다: " + std::to_string(id), {{"entity", id}});
  }
}

// 구속 → 잔차 식들. 각 구속이 몇 개의 식을 내는지는 종류로 정해진다.
void residuals_of(const Reader& r, const Json& c, std::vector<double>& out) {
  const std::string kind = c.value("kind", std::string());
  const Json ents = c.value("entities", Json::array());
  const Json pts = c.value("points", Json::array());
  auto ent = [&](std::size_t i) {
    if (i >= ents.size()) throw Error("missing_param", "구속 " + kind + " 에는 요소가 " + std::to_string(i + 1) + "개 이상 필요합니다", {{"param", "entities"}});
    return ents[i].get<int>();
  };
  auto pt = [&](std::size_t i) {
    if (i >= pts.size()) throw Error("missing_param", "구속 " + kind + " 에는 점이 " + std::to_string(i + 1) + "개 이상 필요합니다", {{"param", "points"}});
    return point_of(r, pts[i]);
  };
  const double value = c.value("value", 0.0);
  if (kind == "coincident") {
    const P2 d = pt(0) - pt(1);
    out.push_back(d.x), out.push_back(d.y);
  } else if (kind == "horizontal") {
    if (!pts.empty()) out.push_back(pt(0).y - pt(1).y);
    else out.push_back(direction_of(r, ent(0)).y);
  } else if (kind == "vertical") {
    if (!pts.empty()) out.push_back(pt(0).x - pt(1).x);
    else out.push_back(direction_of(r, ent(0)).x);
  } else if (kind == "parallel") {
    const P2 a = direction_of(r, ent(0)), b = direction_of(r, ent(1));
    out.push_back(cross(a, b) / std::max(norm(a) * norm(b), 1e-300));
  } else if (kind == "perpendicular") {
    const P2 a = direction_of(r, ent(0)), b = direction_of(r, ent(1));
    out.push_back(dot(a, b) / std::max(norm(a) * norm(b), 1e-300));
  } else if (kind == "tangent") {
    const std::string k0 = r.kind(ent(0)), k1 = r.kind(ent(1));
    if (k0 == "line" || k1 == "line") {
      const int line = k0 == "line" ? ent(0) : ent(1), circ = k0 == "line" ? ent(1) : ent(0);
      P2 cc;
      double rad;
      circle_of(r, circ, cc, rad);
      const P2 s{r.at(line, "start", 0), r.at(line, "start", 1)}, d = direction_of(r, line);
      out.push_back(std::fabs(cross(d, cc - s)) / std::max(norm(d), 1e-300) - rad);  // 중심에서 직선까지 거리 = 반지름
    } else {
      P2 c0, c1;
      double r0, r1;
      circle_of(r, ent(0), c0, r0), circle_of(r, ent(1), c1, r1);
      const double dist = norm(c0 - c1);
      // 바깥 접촉(r0+r1)과 안쪽 접촉(|r0-r1|) 가운데 처음 상태에 가까운 쪽
      const double ext = std::fabs(dist - (r0 + r1)), in = std::fabs(dist - std::fabs(r0 - r1));
      out.push_back(c.value("internal", in < ext) ? dist - std::fabs(r0 - r1) : dist - (r0 + r1));
    }
  } else if (kind == "equal") {
    const std::string k0 = r.kind(ent(0));
    if (k0 == "line") out.push_back(norm(direction_of(r, ent(0))) - norm(direction_of(r, ent(1))));
    else {
      P2 c0, c1;
      double r0, r1;
      circle_of(r, ent(0), c0, r0), circle_of(r, ent(1), c1, r1);
      out.push_back(r0 - r1);
    }
  } else if (kind == "concentric") {
    P2 c0, c1;
    double r0, r1;
    circle_of(r, ent(0), c0, r0), circle_of(r, ent(1), c1, r1);
    out.push_back(c0.x - c1.x), out.push_back(c0.y - c1.y);
  } else if (kind == "fixed") {
    const P2 p = pt(0);
    if (!c.contains("at") || !c["at"].is_array()) throw Error("missing_param", "fixed 구속에는 고정 좌표 at 이 있어야 합니다(sketch.add_constraint 가 채운다)", {{"param", "at"}});
    out.push_back(p.x - c["at"][0].get<double>()), out.push_back(p.y - c["at"][1].get<double>());
  } else if (kind == "symmetric") {  // 두 점이 축 선분에 대해 대칭: 중점이 축 위, 연결선이 축과 직교
    const P2 a = pt(0), b = pt(1);
    const int axis = ent(0);
    const P2 s{r.at(axis, "start", 0), r.at(axis, "start", 1)}, d = direction_of(r, axis);
    const P2 mid = (a + b) * 0.5;
    out.push_back(cross(d, mid - s) / std::max(norm(d), 1e-300));
    out.push_back(dot(d, b - a) / std::max(norm(d), 1e-300));
  } else if (kind == "point_on_line") {
    const P2 p = pt(0);
    const int line = ent(0);
    const P2 s{r.at(line, "start", 0), r.at(line, "start", 1)}, d = direction_of(r, line);
    out.push_back(cross(d, p - s) / std::max(norm(d), 1e-300));
  } else if (kind == "point_on_circle") {
    P2 cc;
    double rad;
    circle_of(r, ent(0), cc, rad);
    out.push_back(norm(pt(0) - cc) - rad);
  } else if (kind == "length") {
    out.push_back(norm(direction_of(r, ent(0))) - value);
  } else if (kind == "distance") {
    if (pts.size() >= 2) out.push_back(norm(pt(0) - pt(1)) - value);
    else {  // 점과 선분 사이의 거리
      const P2 p = pt(0);
      const int line = ent(0);
      const P2 s{r.at(line, "start", 0), r.at(line, "start", 1)}, d = direction_of(r, line);
      out.push_back(std::fabs(cross(d, p - s)) / std::max(norm(d), 1e-300) - value);
    }
  } else if (kind == "horizontal_distance") {
    out.push_back(pt(1).x - pt(0).x - value);
  } else if (kind == "vertical_distance") {
    out.push_back(pt(1).y - pt(0).y - value);
  } else if (kind == "angle") {
    const P2 a = direction_of(r, ent(0)), b = direction_of(r, ent(1));
    const double ang = std::atan2(cross(a, b), dot(a, b)) * 180.0 / kPi;
    double diff = ang - value;
    while (diff > 180.0) diff -= 360.0;
    while (diff < -180.0) diff += 360.0;
    out.push_back(diff * kPi / 180.0);
  } else if (kind == "radius") {
    P2 cc;
    double rad;
    circle_of(r, ent(0), cc, rad);
    out.push_back(rad - value);
  } else if (kind == "diameter") {
    P2 cc;
    double rad;
    circle_of(r, ent(0), cc, rad);
    out.push_back(2 * rad - value);
  } else {
    throw Error("invalid_choice", "알 수 없는 구속 종류입니다: " + kind, {{"param", "kind"}});
  }
}

std::vector<double> all_residuals(const Layout& L, const std::vector<double>& x, const Json& constraints, std::vector<std::pair<int, int>>* rows_of = nullptr) {
  std::vector<double> out;
  Reader r{L, x};
  for (const Json& c : constraints) {
    const std::size_t before = out.size();
    residuals_of(r, c, out);
    if (rows_of) rows_of->push_back({static_cast<int>(before), static_cast<int>(out.size() - before)});
  }
  return out;
}

// 수치 야코비안(중앙 차분)
std::vector<std::vector<double>> jacobian(const Layout& L, const std::vector<double>& x, const Json& constraints, std::size_t m) {
  const std::size_t n = x.size();
  std::vector<std::vector<double>> J(m, std::vector<double>(n, 0.0));
  std::vector<double> xp = x;
  for (std::size_t j = 0; j < n; ++j) {
    const double h = 1e-6 * std::max(1.0, std::fabs(x[j]));
    xp[j] = x[j] + h;
    const std::vector<double> fp = all_residuals(L, xp, constraints);
    xp[j] = x[j] - h;
    const std::vector<double> fm = all_residuals(L, xp, constraints);
    xp[j] = x[j];
    for (std::size_t i = 0; i < m; ++i) J[i][j] = (fp[i] - fm[i]) / (2 * h);
  }
  return J;
}

// 행렬의 계수(열 피벗 가우스 소거, 상대 허용 오차). dependent 에는 독립이 아닌 행 번호를 담는다.
int rank_of(std::vector<std::vector<double>> A, std::vector<int>* dependent = nullptr, double tol = 1e-8) {
  const std::size_t m = A.size(), n = m ? A[0].size() : 0;
  double scale = 0;
  for (const auto& row : A)
    for (double v : row) scale = std::max(scale, std::fabs(v));
  if (scale <= 0) {
    if (dependent) for (std::size_t i = 0; i < m; ++i) dependent->push_back(static_cast<int>(i));
    return 0;
  }
  // 행 순서를 지키며(앞 행이 우선) 각 행이 앞 행들의 조합인지 본다: 그람-슈미트식 소거
  std::vector<std::vector<double>> basis;
  int rank = 0;
  for (std::size_t i = 0; i < m; ++i) {
    std::vector<double> v = A[i];
    for (const auto& b : basis) {
      double d = 0, bb = 0;
      for (std::size_t k = 0; k < n; ++k) d += v[k] * b[k], bb += b[k] * b[k];
      if (bb > 0) for (std::size_t k = 0; k < n; ++k) v[k] -= d / bb * b[k];
    }
    double nv = 0, na = 0;
    for (std::size_t k = 0; k < n; ++k) nv += v[k] * v[k], na += A[i][k] * A[i][k];
    if (std::sqrt(nv) > tol * std::max(std::sqrt(na), scale * 1e-3)) basis.push_back(v), ++rank;
    else if (dependent) dependent->push_back(static_cast<int>(i));
  }
  return rank;
}

// (JᵀJ + λ diag) δ = -Jᵀf  를 가우스 소거로 푼다
bool solve_normal(const std::vector<std::vector<double>>& J, const std::vector<double>& f, double lambda, std::vector<double>& delta) {
  const std::size_t m = J.size(), n = m ? J[0].size() : 0;
  std::vector<std::vector<double>> A(n, std::vector<double>(n + 1, 0.0));
  for (std::size_t i = 0; i < m; ++i)
    for (std::size_t a = 0; a < n; ++a) {
      if (J[i][a] == 0) continue;
      for (std::size_t b = 0; b < n; ++b) A[a][b] += J[i][a] * J[i][b];
      A[a][n] -= J[i][a] * f[i];
    }
  for (std::size_t a = 0; a < n; ++a) A[a][a] += lambda * (A[a][a] + 1e-12);
  for (std::size_t c = 0; c < n; ++c) {  // 부분 피벗
    std::size_t p = c;
    for (std::size_t r = c + 1; r < n; ++r)
      if (std::fabs(A[r][c]) > std::fabs(A[p][c])) p = r;
    if (std::fabs(A[p][c]) < 1e-300) return false;
    std::swap(A[c], A[p]);
    for (std::size_t r = 0; r < n; ++r) {
      if (r == c) continue;
      const double k = A[r][c] / A[c][c];
      if (k == 0) continue;
      for (std::size_t j = c; j <= n; ++j) A[r][j] -= k * A[c][j];
    }
  }
  delta.assign(n, 0.0);
  for (std::size_t a = 0; a < n; ++a) delta[a] = A[a][n] / A[a][a];
  return true;
}

double max_abs(const std::vector<double>& v) {
  double m = 0;
  for (double x : v) m = std::max(m, std::fabs(x));
  return m;
}

// Levenberg-Marquardt. 돌려주는 값: 수렴 여부. x 는 풀린 값으로.
bool levenberg_marquardt(const Layout& L, std::vector<double>& x, const Json& constraints, double tol, int& iterations) {
  std::vector<double> f = all_residuals(L, x, constraints);
  if (f.empty() || x.empty()) return max_abs(f) <= tol;
  double lambda = 1e-3;
  iterations = 0;
  for (int it = 0; it < 200; ++it) {
    ++iterations;
    const double err = max_abs(f);
    if (err <= tol) return true;
    const auto J = jacobian(L, x, constraints, f.size());
    bool improved = false;
    for (int tries = 0; tries < 12; ++tries) {
      std::vector<double> delta;
      if (!solve_normal(J, f, lambda, delta)) {
        lambda *= 10;
        continue;
      }
      std::vector<double> xn = x;
      for (std::size_t j = 0; j < x.size(); ++j) xn[j] += delta[j];
      const std::vector<double> fn = all_residuals(L, xn, constraints);
      double sq = 0, sqn = 0;
      for (double v : f) sq += v * v;
      for (double v : fn) sqn += v * v;
      if (sqn < sq) {
        x = xn, f = fn, lambda = std::max(lambda / 10, 1e-12), improved = true;
        break;
      }
      lambda *= 10;
    }
    if (!improved) return max_abs(f) <= tol;
  }
  return max_abs(f) <= tol;
}

void write_back(const Layout& L, const std::vector<double>& x, Json& entities) {
  for (Json& e : entities) {
    const int id = e.value("id", 0);
    auto ei = L.slots.find(id);
    if (ei == L.slots.end()) continue;
    for (const auto& [key, s] : ei->second) {
      if (s.index < 0) continue;
      Json& v = e[key];
      if (v.is_number()) v = x[static_cast<std::size_t>(s.index)];
      else if (v.is_array() && !v.empty() && v[0].is_array()) {
        for (std::size_t p = 0; p < v.size(); ++p) v[p][0] = x[static_cast<std::size_t>(s.index) + 2 * p], v[p][1] = x[static_cast<std::size_t>(s.index) + 2 * p + 1];
      } else {
        for (std::size_t k = 0; k < v.size(); ++k) v[k] = x[static_cast<std::size_t>(s.index) + k];
      }
    }
  }
}

}  // namespace

SketchSolveResult solve_sketch(Json& entities, const Json& constraints, bool identify_conflicts, double tolerance) {
  SketchSolveResult res;
  const Layout L = make_layout(entities);
  std::vector<double> x = L.x0;
  res.variables = static_cast<int>(x.size());
  std::vector<std::pair<int, int>> rows;
  const std::vector<double> f0 = all_residuals(L, x, constraints, &rows);  // 구속 정의 검사도 겸한다
  res.equations = static_cast<int>(f0.size());
  if (constraints.empty()) {
    res.converged = true, res.status = "no_constraints", res.dof = res.variables;
    return res;
  }
  // 잔차의 크기 기준: 좌표 크기에 비례한 허용 오차
  double scale = 1.0;
  for (double v : x) scale = std::max(scale, std::fabs(v));
  const double tol = tolerance * scale;
  res.converged = levenberg_marquardt(L, x, constraints, tol, res.iterations);
  const std::vector<double> f = all_residuals(L, x, constraints);
  res.residual = max_abs(f);
  // 계수와 과구속(독립이 아닌 식)
  std::vector<int> dependent_rows;
  if (!x.empty() && !f.empty()) {
    const auto J = jacobian(L, x, constraints, f.size());
    res.rank = rank_of(J, &dependent_rows);
  }
  res.dof = res.variables - res.rank;
  std::set<int> redundant;
  for (int row : dependent_rows)
    for (std::size_t ci = 0; ci < rows.size(); ++ci)
      if (row >= rows[ci].first && row < rows[ci].first + rows[ci].second) redundant.insert(constraints[ci].value("id", static_cast<int>(ci) + 1));
  res.redundant.assign(redundant.begin(), redundant.end());
  if (res.converged) {
    write_back(L, x, entities);
    res.status = !redundant.empty() ? "over_constrained" : res.dof > 0 ? "unconstrained" : "fully_constrained";
    return res;
  }
  res.status = "conflict";
  if (identify_conflicts) {  // 구속을 하나씩 빼 보며 그것만 빼면 풀리는 구속을 모은다
    for (std::size_t ci = 0; ci < constraints.size(); ++ci) {
      Json sub = Json::array();
      for (std::size_t k = 0; k < constraints.size(); ++k)
        if (k != ci) sub.push_back(constraints[k]);
      std::vector<double> xs = L.x0;
      int its = 0;
      if (levenberg_marquardt(L, xs, sub, tol, its)) res.conflicts.push_back(constraints[ci].value("id", static_cast<int>(ci) + 1));
    }
  }
  return res;
}

std::array<double, 2> sketch_point_position(const Json& entities, const Json& spec) {
  const Layout L = make_layout(entities);
  const Reader r{L, L.x0};
  const P2 p = point_of(r, spec);
  return {p.x, p.y};
}

}  // namespace ofep
