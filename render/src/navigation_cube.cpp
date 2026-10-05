#include "navigation_cube.hpp"
#include "nasa95/error.hpp"
#include <algorithm>
#include <cmath>
#include <set>

namespace nasa95::render_detail {
namespace {
using V3 = std::array<double, 3>;
V3 add(const V3& a, const V3& b) { return {a[0]+b[0], a[1]+b[1], a[2]+b[2]}; }
V3 mul(const V3& a, double s) { return {a[0]*s, a[1]*s, a[2]*s}; }
double dot(const V3& a, const V3& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
V3 unit(const V3& a) { return mul(a, 1 / std::sqrt(dot(a,a))); }
// 방향 큐브의 표면을 그리기와 클릭 판정에서 공유한다. 모델의 위치·크기·투영에는 영향을 받지 않는다.
struct CubeFace {
  V3 normal, horizontal, vertical;
  const char* name;
};
const std::array<CubeFace, 6> kCubeFaces{{
    {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, "right"}, {{-1, 0, 0}, {0, -1, 0}, {0, 0, 1}, "left"},
    {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}, "front"}, {{0, 1, 0}, {-1, 0, 0}, {0, 0, 1}, "back"},
    {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}, "top"}, {{0, 0, -1}, {1, 0, 0}, {0, -1, 0}, "bottom"}}};
struct CubeFacet {
  std::vector<V3> points;
  V3 normal;
  int face = -1;  // -1 은 모서리·꼭짓점의 경사면
};
const std::vector<CubeFacet>& cube_facets() {
  static const std::vector<CubeFacet> facets = [] {
    std::vector<CubeFacet> out;
    constexpr double q = 0.82;
    for (int i = 0; i < 6; ++i) {
      const auto& f = kCubeFaces[i];
      CubeFacet facet{{}, f.normal, i};
      for (const auto& uv : std::array<std::array<double, 2>, 4>{{{-q, -q}, {q, -q}, {q, q}, {-q, q}}})
        facet.points.push_back(add(f.normal, add(mul(f.horizontal, uv[0]), mul(f.vertical, uv[1]))));
      out.push_back(std::move(facet));
    }
    for (int i = 0; i < 3; ++i)
      for (int j = i + 1; j < 3; ++j)
        for (double si : {-1., 1.})
          for (double sj : {-1., 1.}) {
            const int k = 3 - i - j;
            V3 n{}, a{}, b{}, c{}, d{};
            n[i] = si, n[j] = sj;
            a[i] = d[i] = si, a[j] = d[j] = sj * q;
            b[i] = c[i] = si * q, b[j] = c[j] = sj;
            a[k] = b[k] = -q, c[k] = d[k] = q;
            out.push_back({{a, b, c, d}, unit(n), -1});
          }
    for (double x : {-1., 1.})
      for (double y : {-1., 1.})
        for (double z : {-1., 1.})
          out.push_back({{{x, y * q, z * q}, {x * q, y, z * q}, {x * q, y * q, z}}, unit({x, y, z}), -1});
    return out;
  }();
  return facets;
}
struct CubeFrame {
  V3 right, up, back;
  double ox, oy, scale;
  CubeFrame(const CubeOrientation& orientation, int width, int height) {
    back = orientation.back, right = orientation.right, up = orientation.up;
    scale = std::min(2.0, std::min(width, height) / 120.0);  // 면 이름·클릭 영역을 두 배로, 작은 뷰포트에서는 맞춰 축소
    ox = 44.0 * scale, oy = height - 40.0 * scale;
  }
  V3 project(const V3& point) const {
    return {ox + 12.0 * scale * dot(right, point), oy - 12.0 * scale * dot(up, point), 0.04 - 0.008 * dot(back, point)};
  }
};

std::string target_name(const V3& direction) {
  std::vector<std::string> words;
  if (direction[1] != 0) words.push_back(direction[1] < 0 ? "front" : "back");
  if (direction[0] != 0) words.push_back(direction[0] < 0 ? "left" : "right");
  if (direction[2] != 0) words.push_back(direction[2] < 0 ? "bottom" : "top");
  std::string name;
  for (const auto& word : words) { if (!name.empty()) name += "-"; name += word; }
  return name;
}
struct CubeRegion {
  std::vector<V3> points;
  V3 normal, direction;
  std::string name;
};
const std::vector<CubeRegion>& cube_regions() {
  static const auto regions = [] {
    std::vector<CubeRegion> out;
    constexpr std::array<double, 4> cuts{-0.82, -0.60, 0.60, 0.82};
    auto append = [&](std::vector<V3> points, const V3& normal, const V3& direction) {
      out.push_back({std::move(points), normal, direction, target_name(direction)});
    };
    for (const auto& facet : cube_facets()) {
      if (facet.face >= 0) {
        const auto& f = kCubeFaces[facet.face];
        auto at = [&](double u, double v) { return add(f.normal, add(mul(f.horizontal, u), mul(f.vertical, v))); };
        // 면 가장자리의 넓은 띠와 모퉁이도 에지·꼭짓점 클릭 영역으로 사용한다.
        for (int u = 0; u < 3; ++u)
          for (int v = 0; v < 3; ++v) {
            const V3 direction = add(f.normal, add(mul(f.horizontal, u - 1), mul(f.vertical, v - 1)));
            append({at(cuts[u], cuts[v]), at(cuts[u + 1], cuts[v]), at(cuts[u + 1], cuts[v + 1]), at(cuts[u], cuts[v + 1])}, f.normal, direction);
          }
      } else {
        V3 direction{};
        int along = -1;
        for (int i = 0; i < 3; ++i) {
          direction[i] = facet.normal[i] > 0 ? 1. : facet.normal[i] < 0 ? -1. : 0.;
          if (!direction[i]) along = i;
        }
        if (along < 0) append(facet.points, facet.normal, direction);
        else for (int zone = 0; zone < 3; ++zone) {
          auto points = facet.points;
          points[0][along] = points[1][along] = cuts[zone];
          points[2][along] = points[3][along] = cuts[zone + 1];
          direction[along] = zone - 1;
          append(std::move(points), facet.normal, direction);
        }
      }
    }
    return out;
  }();
  return regions;
}

}  // namespace

void draw_navigation_cube(Hud& h, const CubeOrientation& orientation, const std::string& hovered) {
  const CubeFrame frame(orientation, h.width, h.height);
  using Color = std::array<std::uint8_t, 3>;
  auto vertex = [&](const V3& point, const Color& color) {
    const V3 p = frame.project(point);
    return h.v(p[0], p[1], color, static_cast<float>(p[2]));
  };
  auto polygon = [&](const std::vector<V3>& points, const Color& color) {
    for (std::size_t i = 1; i + 1 < points.size(); ++i) {
      h.scene.hud_triangles.push_back(vertex(points[0], color));
      h.scene.hud_triangles.push_back(vertex(points[i], color));
      h.scene.hud_triangles.push_back(vertex(points[i + 1], color));
    }
  };
  auto line = [&](const V3& a, const V3& b, const Color& color) {
    h.scene.hud_lines.push_back(vertex(a, color)), h.scene.hud_lines.push_back(vertex(b, color));
  };
  // 큐브 발치의 작고 얇은 좌표축. 큐브 뒤로 가는 부분은 같은 깊이로 가린다.
  const V3 foot{0, 0, -1.3};
  const std::array<Color, 3> colors{{{205, 75, 75}, {90, 162, 80}, {75, 105, 205}}};
  const char* axes[3] = {"X", "Y", "Z"};
  double top = frame.oy, bottom = frame.oy, left = frame.ox, right = frame.ox;
  for (const auto& facet : cube_facets())
    for (const V3& point : facet.points) {
      const V3 p = frame.project(point);
      top = std::min(top, p[1]), bottom = std::max(bottom, p[1]), left = std::min(left, p[0]), right = std::max(right, p[0]);
    }
  for (int k = 0; k < 3; ++k) {
    V3 end = foot;
    end[k] += 2.8;
    V3 p = frame.project(end);
    const V3 start = frame.project(foot);
    if (std::hypot(p[0] - start[0], p[1] - start[1]) < 3 * frame.scale) continue;
    line(foot, end, colors[k]);
    // 축 이름이 면 이름 위에 겹치지 않도록 큐브 경계 밖에 둔다.
    if (p[0] >= left - 3 * frame.scale && p[0] <= right + 3 * frame.scale && p[1] >= top - 4 * frame.scale && p[1] <= bottom + 4 * frame.scale) {
      if (std::fabs(p[0] - frame.ox) > std::fabs(p[1] - frame.oy)) p[0] = p[0] < frame.ox ? left - 5 * frame.scale : right + 5 * frame.scale;
      else p[1] = p[1] < frame.oy ? top - 6 * frame.scale : bottom + 6 * frame.scale;
    }
    h.text(p[0] - 2 * frame.scale, p[1] - 4 * frame.scale, axes[k], colors[k], 0.9 * frame.scale);
  }
  const V3 light = unit(add(mul(frame.back, 2.0), frame.up));
  for (const auto& facet : cube_facets()) {
    if (dot(facet.normal, frame.back) <= 1e-8) continue;
    const auto shade = static_cast<std::uint8_t>(std::clamp(208.0 + 37.0 * dot(facet.normal, light) + (facet.face < 0 ? 6.0 : 0.0), 200.0, 249.0));
    polygon(facet.points, {shade, shade, shade});
    for (std::size_t i = 0; i < facet.points.size(); ++i)
      line(facet.points[i], facet.points[(i + 1) % facet.points.size()], {142, 145, 148});
    if (facet.face < 0) continue;
    const auto& face = kCubeFaces[facet.face];
    const std::string name = face.name;
    const double cell = std::min(0.08, 1.42 / (6.0 * name.size() - 1.0));
    auto on_face = [&](double x, double y) { return add(mul(face.normal, 1.015), add(mul(face.horizontal, x), mul(face.vertical, y))); };
    for (std::size_t ch = 0; ch < name.size(); ++ch) {
      const char* g = glyph(static_cast<char>(std::toupper(static_cast<unsigned char>(name[ch]))));
      for (int row = 0; row < 7; ++row)
        for (int col = 0; col < 5; ++col)
          if (g[row * 5 + col] == '1') {
            const double x = (6.0 * ch + col - (6.0 * name.size() - 1.0) / 2) * cell, y = (3.5 - row) * cell;
            polygon({on_face(x, y), on_face(x + cell, y), on_face(x + cell, y - cell), on_face(x, y - cell)}, {94, 98, 103});
          }
    }
  }
  // 판정과 같은 영역을 칠한다. 글자보다 살짝 뒤, 바탕 면보다 앞에 둔다.
  for (const auto& region : cube_regions())
    if (region.name == hovered && dot(region.normal, frame.back) > 1e-8) {
      auto points = region.points;
      for (auto& point : points) point = add(point, mul(region.normal, 0.006));
      polygon(points, {133, 190, 239});
    }
}


Json pick_navigation_cube(const CubeOrientation& orientation, double x, double y, int width, int height) {
  const CubeFrame frame(orientation, width, height);
  double nearest = 1.0;
  std::string selected;
  for (const auto& region : cube_regions()) {
    if (dot(region.normal, frame.back) <= 1e-8) continue;
    const V3 a = frame.project(region.points[0]);
    for (std::size_t i = 1; i + 1 < region.points.size(); ++i) {
      const V3 b = frame.project(region.points[i]), c = frame.project(region.points[i + 1]);
      const double det = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
      if (std::fabs(det) < 1e-10) continue;
      const double u = ((b[1] - c[1]) * (x - c[0]) + (c[0] - b[0]) * (y - c[1])) / det;
      const double v = ((c[1] - a[1]) * (x - c[0]) + (a[0] - c[0]) * (y - c[1])) / det;
      if (u < -1e-8 || v < -1e-8 || u + v > 1.0 + 1e-8) continue;
      const double z = u * a[2] + v * b[2] + (1.0 - u - v) * c[2];
      if (z <= nearest) nearest = z, selected = region.name;
    }
  }
  return selected.empty() ? Json{{"hit", false}} : Json{{"hit", true}, {"view", selected}};
}

std::vector<std::string> navigation_cube_views() {
  std::set<std::string> names{"iso"};
  for (const auto& region : cube_regions()) names.insert(region.name);
  return {names.begin(), names.end()};
}
CubeTarget navigation_cube_target(const std::string& name) {
  if (name == "iso") return {{1, -1, 1}, {0, 0, 1}};
  for (const auto& region : cube_regions())
    if (region.name == name) {
      const bool vertical = region.direction[0] == 0 && region.direction[1] == 0;
      return {region.direction, vertical ? V3{0, 1, 0} : V3{0, 0, 1}};
    }
  throw Error("invalid_value", "알 수 없는 방향 큐브 뷰입니다", {{"name", name}});
}

}  // namespace nasa95::render_detail
