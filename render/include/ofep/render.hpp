#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ofep/app.hpp"

namespace ofep {

// 렌더러(7단계). Vulkan 을 직접 쓴다 — 렌더링 라이브러리를 쓰지 않는다(아키텍처 규칙 6).
// 지금은 창 없이 이미지로 그리는 경로(오프스크린, RND-45)와 ID 버퍼(RND-48)다. 창에 그리는 경로는 UI 단계에서 붙인다.

struct RenderVertex {
  float pos[3];
  float normal[3];        // 0 이면 조명 없이 그린다(선)
  std::uint8_t color[4];
  std::uint32_t id;       // ID 버퍼에 쓸 값(0 = 없음)
};

struct RenderScene {
  int pixel_scale = 1;  // SSAA의 내부 해상도 배율. 선 폭은 출력 픽셀 기준으로 유지한다.
  std::vector<RenderVertex> triangles;  // 3 개씩
  std::vector<RenderVertex> lines;      // 2 개씩
  std::vector<RenderVertex> overlay;    // 3 개씩. 강조 표시: 면·선을 그린 뒤 위에 그린다(법선 0 → 조명 없이, 카메라 쪽으로 당겨서)
  // 투명 면(RND-17): 불투명한 것을 다 그린 뒤 색을 섞어 그린다(깊이는 쓰지 않는다). 겹친 투명 면끼리의 순서는 맞추지 않는다.
  std::vector<RenderVertex> transparent;
  // 화면 고정 요소(좌표축·글자·범례, RND-35~37): 위치가 클립 좌표(x, y ∈ [-1, 1], 왼쪽 위가 (-1, -1), z ∈ [0, 1])다.
  // 변환 없이 맨 위에 그린다. ID 는 0 으로 둔다(픽킹 대상이 아니다).
  std::vector<RenderVertex> hud_triangles;
  std::vector<RenderVertex> hud_lines;
  std::array<float, 4> background{1.0f, 1.0f, 1.0f, 1.0f};
  // 클리핑 평면(RND-19): (nx, ny, nz, d) — nx·x + ny·y + nz·z + d < 0 인 쪽을 지운다. 최대 8개(kMaxClipPlanes). 화면 고정 요소에는 적용하지 않는다
  std::vector<std::array<float, 4>> clip_planes;
};
constexpr int kMaxClipPlanes = 8;

struct RenderImage {
  int width = 0, height = 0;
  std::vector<std::uint8_t> rgba;   // 4 × 너비 × 높이, 위에서 아래로
  std::vector<std::uint32_t> ids;   // 너비 × 높이
};

// Vulkan 장치와 파이프라인. 프로그램에 하나만 둔다.
class Renderer {
 public:
  Renderer();   // Vulkan 을 쓸 수 없으면 Error("render_unavailable")
  ~Renderer();
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  // mvp, view: 열 우선 4×4. view 의 4번째 열(12~15)은 회전에 쓰지 않으므로 클리핑 평면 (nx, ny, nz, d) 를 싣는다
  // (nx·x + ny·y + nz·z + d < 0 인 쪽을 지운다. 법선이 0 이면 클리핑 없음, RND-19).
  RenderImage render(const RenderScene& scene, const float mvp[16], const float view[16], int width, int height);
  Json info() const;  // GPU 이름, API 버전, 마지막 프레임의 통계

  // 창에 그리기(RND-02): 네이티브 창 핸들(Windows: HWND)을 붙이고, 프레임마다 present 를 부른다.
  void attach_window(void* native_handle);
  void detach_window();
  bool has_window() const;
  std::array<int, 2> present(const RenderScene& scene, const float mvp[16], const float view[16]);  // 창 크기(0 이면 그리지 않음)
  std::array<int, 2> window_size() const;
  // 투영/HUD 계산 전에 현재 surface 크기로 출력 자원을 준비한다.
  std::array<int, 2> prepare_window(int pixel_scale);
  std::uint32_t pick_window(int x, int y);  // 마지막 프레임의 ID 버퍼

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// 뷰 명령(view.*)을 등록한다. 코어에는 렌더러가 없으므로 App 을 만든 쪽(파이썬 모듈)이 부른다.
void register_view_commands(App& app);
// 현재 뷰를 그린다(파이썬의 배열 조회용).
RenderImage render_view(App& app, int width, int height);
// 창: 붙이기·떼기·프레임 내기·픽킹(UI 가 부른다)
void view_attach_window(App& app, void* native_handle);
void view_detach_window(App& app);
std::array<int, 2> view_present(App& app);
Json view_pick_window(App& app, int x, int y);
// 창 크기 기준 카메라 조작(UI 가 마우스 움직임을 넘긴다)
Json view_orbit(App& app, double dx_pixels, double dy_pixels, double height = 0);
Json view_pan(App& app, double dx_pixels, double dy_pixels);
Json view_zoom(App& app, double factor, double x_pixel, double y_pixel);
// 영역 확대: 화면 사각형(픽셀, 뷰포트 width×height 기준)이 꽉 차게. width/height 0 이면 창 크기
Json view_zoom_region(App& app, double x0, double y0, double x1, double y1, int width, int height);
// 마우스 오버 강조(RND-49): 창의 픽셀 아래 객체를 강조 대상으로 삼는다. 바뀌었으면 changed = true (다시 그려야 한다)
Json view_hover(App& app, int x, int y);

}  // namespace ofep
