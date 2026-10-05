#pragma once
#include <cmath>
#include "nasa95/render.hpp"

namespace nasa95::render_detail {

// 하중 화살표: 끝점·방향·길이를 보존하는 닫힌 원통 몸통과 원뿔 화살촉.
// 모델 ID를 만들지 않으며 기존 조명·깊이·클리핑 파이프라인을 사용한다.
inline void add_load_arrow(RenderScene& scene, const std::array<double, 3>& tip,
                           const std::array<double, 3>& direction, double length,
                           const std::array<std::uint8_t, 3>& color) {
  using V3 = std::array<double, 3>;
  auto add = [](const V3& a, const V3& b) -> V3 { return {a[0]+b[0], a[1]+b[1], a[2]+b[2]}; };
  auto mul = [](const V3& a, double s) -> V3 { return {a[0]*s, a[1]*s, a[2]*s}; };
  auto cross = [](const V3& a, const V3& b) -> V3 {
    return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]};
  };
  auto norm = [](const V3& a) { return std::hypot(a[0], a[1], a[2]); };
  const double magnitude = norm(direction);
  if (!(length > 0) || !std::isfinite(length) || !(magnitude > 0) || !std::isfinite(magnitude)) return;
  const V3 axis = mul(direction, 1.0 / magnitude);
  const V3 side = cross(axis, std::fabs(axis[2]) < 0.9 ? V3{0, 0, 1} : V3{1, 0, 0});
  const V3 u = mul(side, 1.0 / norm(side)), v = cross(axis, u);
  const double shaft_radius = 0.045 * length, head_radius = 0.13 * length, head_length = 0.28 * length;
  const V3 tail = add(tip, mul(axis, -length)), neck = add(tip, mul(axis, -head_length));
  auto vertex = [&](const V3& p, const V3& n) {
    RenderVertex out{};
    for (std::size_t k = 0; k < 3; ++k) {
      out.pos[k] = static_cast<float>(p[k]);
      out.normal[k] = static_cast<float>(n[k]);
      out.color[k] = color[k];
    }
    out.color[3] = 255;
    return out;
  };
  auto tri = [&](const V3& a, const V3& b, const V3& c, const V3& na, const V3& nb, const V3& nc) {
    scene.triangles.push_back(vertex(a, na));
    scene.triangles.push_back(vertex(b, nb));
    scene.triangles.push_back(vertex(c, nc));
  };
  constexpr int segments = 16;
  constexpr double tau = 6.28318530717958647692;
  const V3 backward = mul(axis, -1);
  auto radial = [&](double angle) { return add(mul(u, std::cos(angle)), mul(v, std::sin(angle))); };
  auto cone_normal = [&](const V3& r) {
    const V3 n = add(mul(r, head_length), mul(axis, head_radius));
    return mul(n, 1.0 / norm(n));
  };
  for (int i = 0; i < segments; ++i) {
    const V3 r0 = radial(tau * i / segments), r1 = radial(tau * (i + 1) / segments);
    const V3 t0 = add(tail, mul(r0, shaft_radius)), t1 = add(tail, mul(r1, shaft_radius));
    const V3 s0 = add(neck, mul(r0, shaft_radius)), s1 = add(neck, mul(r1, shaft_radius));
    const V3 h0 = add(neck, mul(r0, head_radius)), h1 = add(neck, mul(r1, head_radius));
    // 원통 옆면과 뒤쪽 마개. 둘레 법선을 보간하여 매끈한 곡면으로 보인다.
    tri(t0, t1, s1, r0, r1, r1);
    tri(t0, s1, s0, r0, r1, r0);
    tri(tail, t1, t0, backward, backward, backward);
    // 몸통과 화살촉 사이 고리 면(내부에 겹치는 원판을 만들지 않는다).
    tri(s0, h1, h0, backward, backward, backward);
    tri(s0, s1, h1, backward, backward, backward);
    tri(h0, h1, tip, cone_normal(r0), cone_normal(r1), cone_normal(radial(tau * (i + 0.5) / segments)));
  }
}

}  // namespace nasa95::render_detail
