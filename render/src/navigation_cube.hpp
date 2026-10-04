#pragma once
#include <array>
#include <string>
#include <vector>

#include "hud.hpp"

namespace ofep::render_detail {

// 카메라나 앱 상태를 소유하지 않는 화면 고정 방향 큐브 컴포넌트.
struct CubeOrientation {
  std::array<double, 3> right, up, back;
};
struct CubeTarget {
  std::array<double, 3> direction, up;
};

void draw_navigation_cube(Hud& hud, const CubeOrientation& orientation, const std::string& hovered);
Json pick_navigation_cube(const CubeOrientation& orientation, double x, double y, int width, int height);
std::vector<std::string> navigation_cube_views();  // 6면 + 12에지 + 8꼭짓점 + iso 별칭
CubeTarget navigation_cube_target(const std::string& name);

}  // namespace ofep::render_detail
