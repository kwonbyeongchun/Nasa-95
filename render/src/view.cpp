// 뷰: 모델에서 장면(면·선)을 만들고, 카메라를 다루고, 이미지 저장·픽킹 명령을 낸다.
// 그리는 일은 Renderer(Vulkan)가 한다. 뷰 상태(카메라·표시 모드·결과 표시)는 모델이 아니다(Undo·저장 대상이 아님).
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "nasa95/error.hpp"
#include "nasa95/geometry.hpp"
#include "nasa95/render.hpp"
#include "nasa95/results.hpp"
#include "navigation_cube.hpp"
#include "load_arrow.hpp"

namespace nasa95 {

constexpr double kPi = 3.14159265358979323846;

namespace {

using F = FieldSpec;
using V3 = std::array<double, 3>;

bool has(const Json& p, const char* key) { return p.contains(key) && !p[key].is_null(); }
std::string subtype_of(const Object& o) { return o.props.value("type", std::string()); }
V3 sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 add(const V3& a, const V3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 mul(const V3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 cross(const V3& a, const V3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double norm(const V3& a) { return std::sqrt(dot(a, a)); }
V3 unit(const V3& a) {
  const double n = norm(a);
  return n > 0 ? mul(a, 1.0 / n) : V3{0, 0, 1};
}
V3 v3(const Json& j) { return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()}; }

struct Camera {
  V3 eye{1, -1, 1}, target{0, 0, 0}, up{0, 0, 1};
  V3 pivot{0, 0, 0};        // 드래그 시작 시 고른 세계 좌표. target 과 독립(화면을 재중앙화하지 않는다)
  double fov = 30.0;        // 원근 투영의 세로 시야각(도)
  bool ortho = true;        // 직교 투영이 기본(CAE 에서는 치수가 왜곡되지 않는 쪽이 낫다)
  double height = 2.0;      // 직교 투영에서 화면에 담기는 세로 길이
};

struct PickRecord {
  std::string kind;  // face(형상의 면) | element_face | element
  Id a = 0;          // 파트 / 요소
  int b = 0;         // 면 번호
};

struct CameraTransition {
  Camera from, to, last;
};

struct ViewState {
  Camera camera;
  std::string cube_hover;
  std::optional<CameraTransition> camera_transition;
  bool camera_set = false;            // 사용자가 정한 적이 없으면 그릴 때 전체에 맞춘다
  std::string mode = "shaded_edges";  // shaded | shaded_edges | wireframe
  std::string show = "auto";          // auto(메시가 있으면 메시) | geometry | mesh | both
  Json result = nullptr;              // {result, frame, field, component, deform_scale, min, max}
  std::vector<Id> selected_objects;   // 트리에서 고른 모델 객체(WT-22): 적용 대상을 강조한다
  double result_phase = 0.0;          // 위상 애니메이션(RES-48): 변형 배율에 cos(위상)을 곱한다
  Json legend = nullptr;              // 마지막으로 그린 컨투어의 범위
  std::vector<PickRecord> picks;      // 마지막으로 그린 장면의 ID 표(ID 버퍼 값 − 1 이 위치)
  std::unique_ptr<Renderer> renderer;
  // 장면 캐시: 모델·표시 설정이 그대로면 다시 만들지 않는다(카메라만 바뀔 때는 그리기만 한다)
  std::string scene_key;
  std::shared_ptr<struct Built> scene;
  std::array<int, 2> window_size{0, 0};
  std::uint32_t hover = 0;                  // 마우스 아래 객체(ID 표의 값, 0 = 없음)
  Json symbols = nullptr;                   // 하중·경계조건 심볼 {step, size}. null = 끔
  std::vector<PickRecord> highlighted;      // 강조 표시 대상
  Json hud = Json::object();                // 화면 고정 요소 설정 {triad, legend, labels}
  // 클리핑 평면 목록(RND-19): [{id, point, normal, enabled}]. 법선이 가리키는 쪽을 남긴다. 최대 kMaxClipPlanes 개. 빈 배열 = 끔
  Json clip = Json::array();
  int next_clip = 1;
  Json transparency = nullptr;              // 투명 {what: geometry|mesh|all, alpha}. null = 불투명
  std::set<Id> hidden;                      // 숨긴 객체(형상 파트·메시 파트, RND-23)
  std::string color_by = "part";            // 메시 색 기준: part | property | material
  Json labels = Json::object();             // 라벨 {nodes: [...], elements: [...], values: bool}
  double tess_deflection = 0, tess_angle = 0;  // 표시용 삼각화 정밀도(0 = 기본)
  Json saved_views = Json::object();        // 이름 → 뷰 상태
  Json animation = nullptr;                 // {frames: [...], index, interval_ms, loop, playing}
  Json selection = Json::array();           // 선택(WT-14): 픽 기록 {kind, part/index 또는 element/face}
  std::set<std::string> pick_filter;        // 선택 가능한 종류(비어 있으면 전부, RND-26)
  Json legend_opts = Json::object();        // 범례(RES-14): {levels(0 = 연속), colormap, out_of_range: clamp|gray}. 범위는 result.min/max
  Json result_filter = nullptr;             // 결과 표시 제한(RES-16): {target, min, max}. 밖은 바탕색·회색
  Json appearance = Json::object();         // 객체별 모양(WT-25): 객체 ID(문자열) → {color: [r,g,b], alpha}
  Json mesh_options = Json::object();       // 메시 표시(RND-20~22): {edges: bool, shrink: 0~1}
  Json tree_state = Json::object();         // 워크 트리 펼침 상태(WT-36): UI 가 넣고 읽는 값
  std::vector<Id> target_nodes;             // 적용 대상 표시(WT-24)의 노드 표식
  Json overlay = Json::object();            // 화면 요소(RND-37): {ruler: bool, background: [r,g,b]}
  Json quality = Json::object();            // 품질(RND-17·18·42): {antialiasing: none|ssaa2, transparency: unsorted|sorted, simplify_during_interaction}
  Json result_options = Json::object();     // 결과 표시 옵션(RES-07·10, RND-30·32): {undeformed: bool, vectors: {field, scale}}
  Json expand = nullptr;                    // 전개 표시(RES-50·56): {kind: cyclic, sectors, point, axis} | {kind: axisymmetric, segments, angle}
  Json beam_diagram = nullptr;              // 보 단면력 선도(RES-55): {quantity, scale, direction}
  Json layout = nullptr;                    // 뷰포트 분할(RND-03): {rows, cols, cells: [{view}]} — 오프스크린 이미지에 합성
  Json section = nullptr;                   // 단면 결과(RES-15): {point, normal} — 3D 요소를 평면으로 자른 단면 다각형(결과 색)
  Json iso = nullptr;                       // 등가면(RES-41): {values: [...]} — 사면체 분할 마칭
  Json streamlines = nullptr;               // 유선(RND-33): {field, seeds, steps, step_size, direction}
};

ViewState& state(App& a) {
  std::any& slot = a.runtime("view");
  if (!slot.has_value()) slot = std::make_shared<ViewState>();
  return *std::any_cast<std::shared_ptr<ViewState>&>(slot);
}
Renderer& renderer(App& a) {
  ViewState& s = state(a);
  if (!s.renderer) s.renderer = std::make_unique<Renderer>();
  return *s.renderer;
}

const std::array<std::array<std::uint8_t, 3>, 12> kPalette = {{{122, 162, 204}, {214, 158, 96}, {134, 190, 138}, {196, 136, 176},
                                                              {186, 186, 110}, {120, 190, 190}, {208, 130, 130}, {160, 160, 200},
                                                              {228, 196, 92}, {104, 150, 120}, {176, 120, 92}, {150, 120, 210}}};

// 객체 색(메시 파트·재료·프로퍼티·형상 파트): 지정한 색(view.set_appearance)이 있으면 그것, 없으면 그 종류 안의 순서로 팔레트.
// 트리 아이콘과 화면의 색 기준(view.color_by)이 같은 색을 쓰게 하는 한 곳이다(view.object_colors)
std::array<std::uint8_t, 3> object_color(const App& a, const Json& appearance, const std::string& kind, Id id) {
  auto it = appearance.find(std::to_string(id));
  if (it != appearance.end() && it->contains("color"))
    return {static_cast<std::uint8_t>(std::clamp((*it)["color"][0].get<int>(), 0, 255)), static_cast<std::uint8_t>(std::clamp((*it)["color"][1].get<int>(), 0, 255)),
            static_cast<std::uint8_t>(std::clamp((*it)["color"][2].get<int>(), 0, 255))};
  std::size_t k = 0;
  for (const Object* o : a.model().by_kind(kind)) {
    if (o->id == id) return kPalette[k % kPalette.size()];
    ++k;
  }
  return {170, 170, 170};
}

// 0~1 값을 색으로. 색상표(RES-14): rainbow(파랑 → 청록 → 초록 → 노랑 → 빨강), grayscale, blue_red, heat(검정 → 빨강 → 노랑 → 흰색)
std::array<std::uint8_t, 3> colormap(double t, const std::string& name = "rainbow", int levels = 0) {
  t = std::clamp(t, 0.0, 1.0);
  if (levels > 0) t = (std::min(static_cast<int>(t * levels), levels - 1) + 0.5) / levels;  // 단계별: 구간의 가운데 색
  static const double rainbow[5][3] = {{0, 0, 1}, {0, 1, 1}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}};
  static const double gray[2][3] = {{0.1, 0.1, 0.1}, {0.95, 0.95, 0.95}};
  static const double blue_red[3][3] = {{0.2, 0.3, 0.9}, {0.95, 0.95, 0.95}, {0.9, 0.2, 0.2}};
  static const double heat[4][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {1, 1, 1}};
  const double (*c)[3] = rainbow;
  int n = 5;
  if (name == "grayscale") c = gray, n = 2;
  else if (name == "blue_red") c = blue_red, n = 3;
  else if (name == "heat") c = heat, n = 4;
  const double x = t * (n - 1);
  const int seg = std::min(n - 2, static_cast<int>(x));
  const double f = x - seg;
  std::array<std::uint8_t, 3> out{};
  for (int k = 0; k < 3; ++k) out[static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(std::lround(255.0 * (c[seg][k] + f * (c[seg + 1][k] - c[seg][k]))));
  return out;
}
const std::vector<std::string> kColormaps = {"rainbow", "grayscale", "blue_red", "heat"};

struct Bounds {
  V3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
  void add(const V3& p) {
    for (std::size_t k = 0; k < 3; ++k) lo[k] = std::min(lo[k], p[k]), hi[k] = std::max(hi[k], p[k]);
  }
  bool empty() const { return lo[0] > hi[0]; }
  V3 center() const { return mul(nasa95::add(lo, hi), 0.5); }
  double radius() const { return empty() ? 1.0 : std::max(0.5 * norm(sub(hi, lo)), 1e-12); }
};

// 축(point, dir) 둘레로 각 ang 만큼 돌린다(로드리게스)
V3 rotate_about(const V3& p, const V3& point, const V3& dir, double ang) {
  const V3 k = unit(dir), v = sub(p, point);
  const double c = std::cos(ang), s = std::sin(ang);
  const V3 r = add(add(mul(v, c), mul(cross(k, v), s)), mul(k, dot(k, v) * (1.0 - c)));
  return add(point, r);
}
V3 rotate_dir(const V3& v, const V3& dir, double ang) { return rotate_about(v, {0, 0, 0}, dir, ang); }

// 3D 요소(꼭짓점 기준)를 사면체로 나눈다: 단면·등가면·유선이 같은 분할을 쓴다
const std::vector<std::array<int, 4>>& tets_of(Shape shape) {
  static const std::vector<std::array<int, 4>> tet = {{0, 1, 2, 3}};
  static const std::vector<std::array<int, 4>> hex = {{0, 1, 3, 4}, {1, 2, 3, 6}, {1, 4, 5, 6}, {3, 4, 6, 7}, {1, 3, 4, 6}};
  static const std::vector<std::array<int, 4>> wedge = {{0, 1, 2, 3}, {1, 2, 3, 4}, {2, 3, 4, 5}};
  static const std::vector<std::array<int, 4>> pyramid = {{0, 1, 2, 4}, {0, 2, 3, 4}};
  static const std::vector<std::array<int, 4>> none;
  switch (shape) {
    case Shape::Tet4: case Shape::Tet10: return tet;
    case Shape::Hex8: case Shape::Hex20: return hex;
    case Shape::Wedge6: case Shape::Wedge15: return wedge;
    case Shape::Pyramid5: return pyramid;
    default: return none;
  }
}

RenderVertex vertex(const V3& p, const V3& n, const std::array<std::uint8_t, 3>& c, std::uint32_t id) {
  RenderVertex v{};
  for (std::size_t k = 0; k < 3; ++k) v.pos[k] = static_cast<float>(p[k]), v.normal[k] = static_cast<float>(n[k]), v.color[k] = c[k];
  v.color[3] = 255, v.id = id;
  return v;
}

// 구속 표시(BC-13, RND-34): 구속 하나마다 적용 영역 색칠과 이름표 하나. 축 기호는 고른 구속에만 그린다.
// 장면을 만들 때 모으고, 그리는 것은 프레임마다 한다(화면 고정 크기·선택 연동이라 카메라와 선택에 따라 달라진다).
struct BcMark {
  Id id = 0, set = 0;                  // 구속과 그 구속 셋
  std::string text;                    // 이름표: FIX, UXYZ, UX RZ, SYM X, T=20 …
  std::vector<int> dofs;               // 축 기호로 그릴 자유도(1~3 병진, 4~6 회전)
  std::array<std::uint8_t, 3> color{};
  V3 anchor{};                         // 이름표 자리(대상의 가운데에 가장 가까운 노드)
  std::vector<V3> points;              // 축 기호 자리(솎은 노드)
  std::vector<V3> nodes;               // 면으로 칠할 수 없는 대상(점·선)의 노드 표식
  std::vector<std::size_t> triangles;  // 색칠할 삼각형(scene.triangles 안의 첫 정점 위치)
};

struct Built {
  RenderScene scene;
  Bounds bounds;
  std::vector<BcMark> bc_marks;
  std::vector<PickRecord> picks;
  std::unordered_map<Id, V3> node_pos;  // 그린 노드 위치(변형 표시면 옮긴 자리) — 표식·영역 선택에 쓴다
  Json legend = nullptr;
};

struct KeyHash {
  std::size_t operator()(const std::array<Id, 4>& k) const {
    std::size_t h = 1469598103934665603ull;
    for (Id x : k) h = (h ^ static_cast<std::size_t>(x)) * 1099511628211ull;
    return h;
  }
};

std::string format_value(double v);

// 구속된 자유도의 이름표: 전부면 FIX, 아니면 병진 U·회전 R 뒤에 축(UXYZ, UX RZ)
std::string dof_text(const std::set<int>& dofs) {
  if (dofs.size() == 6) return "FIX";
  std::string u, r;
  for (int k = 1; k <= 3; ++k) {
    if (dofs.count(k)) u += "XYZ"[k - 1];
    if (dofs.count(k + 3)) r += "XYZ"[k - 1];
  }
  std::string s = u.empty() ? "" : "U" + u;
  if (!r.empty()) s += (s.empty() ? "R" : " R") + r;
  return s;
}

// 하중·경계조건 심볼(RND-34): 하중은 원통·원뿔 화살표. 구속은 적용 영역 색칠·이름표·축 기호(BcMark)로 모은다.
void add_symbols(App& a, const ViewState& vs, Built& b) {
  const Mesh& m = a.mesh();
  const Id step = vs.symbols.value("step", 0);
  const bool by_sets = vs.symbols.contains("sets") && vs.symbols["sets"].is_array();  // 하중 셋·구속 셋을 바로 그린다(스텝 없이)
  if (!by_sets && (!step || !a.model().find(step))) return;
  const double size = vs.symbols.value("size", 0.0) > 0 ? vs.symbols.value("size", 0.0) : 0.06 * b.bounds.radius();
  auto arrow = [&](const V3& tip, const V3& dir_unit, double length, const std::array<std::uint8_t, 3>& c) {
    render_detail::add_load_arrow(b.scene, tip, dir_unit, length, c);
  };
  const std::array<std::uint8_t, 3> load_color{200, 30, 30};
  // 구속마다 다른 색(하중의 빨강·강조의 주황은 피한다)
  static const std::array<std::array<std::uint8_t, 3>, 6> palette = {
      {{30, 110, 200}, {0, 150, 136}, {142, 68, 173}, {39, 174, 96}, {200, 60, 140}, {150, 105, 60}}};
  std::unordered_map<std::uint32_t, std::vector<std::size_t>> pick_marks;  // 픽 번호 → 그 면을 칠할 구속
  // 스텝에서 유효한 것(승계 포함), 또는 지정한 셋의 항목. 숨긴 셋(view.hide 의 load_set·bc_set)의 항목은 그리지 않는다
  Json effective{{"loads", Json::array()}, {"bcs", Json::array()}};
  if (by_sets) {
    for (const Json& sid : vs.symbols["sets"]) {
      const Object* set = a.model().find(sid.get<Id>());
      if (!set || set->suppressed || (set->kind != "load_set" && set->kind != "bc_set")) continue;
      for (const Object* o : a.model().children(set->id, set->kind == "load_set" ? "load" : "bc"))
        if (!o->suppressed) effective[set->kind == "load_set" ? "loads" : "bcs"].push_back(Json{{"id", o->id}});
    }
  } else {
    try {
      effective = a.commands().at("step.effective").fn(a, Json{{"id", step}});
    } catch (const Error&) {
      return;
    }
  }
  auto set_hidden = [&](const Object& o) { return vs.hidden.count(o.parent) > 0; };
  for (const Json& entry : effective["loads"]) {
    const Object& l = a.model().get(entry["id"].get<Id>());
    if (set_hidden(l)) continue;
    const Json& q = l.props;
    const std::string t = subtype_of(l);
    if (!has(q, "target")) continue;
    try {
      if ((t == "force" || t == "moment") && has(q, "components")) {
        const V3 c = v3(q["components"]);
        if (norm(c) == 0) continue;
        for (const Json& n : resolve_target(a, q["target"], "nodes")) arrow(m.node(n.get<Id>()), unit(c), size, load_color);
      } else if (t == "pressure" && has(q, "value")) {
        const double sign = q["value"].get<double>() >= 0 ? 1.0 : -1.0;
        for (const Json& f : resolve_target(a, q["target"], "faces")) {
          const Element e = m.element(f[0].get<Id>());
          const auto& corners = shape_faces(e.shape)[static_cast<std::size_t>(f[1].get<int>() - 1)];
          std::vector<V3> pts;
          V3 center{0, 0, 0};
          for (int k : corners) pts.push_back(m.node(e.nodes[static_cast<std::size_t>(k)])), center = add(center, pts.back());
          center = mul(center, 1.0 / static_cast<double>(pts.size()));
          const V3 inward = unit(cross(sub(pts[1], pts[0]), sub(pts[2], pts[0])));  // 면 번호 순서의 법선은 요소 안쪽
          arrow(center, mul(inward, sign), size, load_color);
        }
      } else if (t == "traction" && has(q, "components")) {  // 면 분포 힘: 면 중심마다 힘 방향 화살표(RND-34)
        const V3 c = v3(q["components"]);
        if (norm(c) == 0) continue;
        for (const Json& f : resolve_target(a, q["target"], "faces")) {
          const Element e = m.element(f[0].get<Id>());
          const auto& corners = shape_faces(e.shape)[static_cast<std::size_t>(f[1].get<int>() - 1)];
          V3 center{0, 0, 0};
          for (int k : corners) center = add(center, m.node(e.nodes[static_cast<std::size_t>(k)]));
          arrow(mul(center, 1.0 / static_cast<double>(corners.size())), unit(c), size, load_color);
        }
      } else if ((t == "gravity" || t == "acceleration") && has(q, "direction")) {
        const V3 d = unit(v3(q["direction"]));
        const V3 c = b.bounds.center();
        arrow(add(c, mul(d, size)), d, 2 * size, load_color);
      }
    } catch (const Error&) {
      continue;  // 풀 수 없는 대상(형상에 메시가 없음 등)은 그리지 않는다
    }
  }
  for (const Json& entry : effective["bcs"]) {
    const Object& bc = a.model().get(entry["id"].get<Id>());
    if (set_hidden(bc)) continue;
    const Json& q = bc.props;
    const std::string t = subtype_of(bc);
    if (!has(q, "target")) continue;
    BcMark mark;
    mark.id = bc.id, mark.set = bc.parent;
    std::set<int> dofs;
    if ((t == "displacement" || t == "fixed_current") && has(q, "dofs")) {
      for (const Json& d : q["dofs"])
        if (d.get<int>() >= 1 && d.get<int>() <= 6) dofs.insert(d.get<int>());
      if (dofs.empty()) continue;
      bool moving = false;  // 0 이 아닌 값을 주는 구속(강제 변위)
      if (t == "displacement" && has(q, "values"))
        for (const Json& v : q["values"]) moving = moving || (v.is_number() && v.get<double>() != 0.0);
      mark.text = (t == "fixed_current" ? "HOLD " : moving ? "DISP " : "") + dof_text(dofs);
    } else if ((t == "symmetry" || t == "antisymmetry") && has(q, "normal")) {
      // 대칭: 면에 수직인 병진과 면 안의 두 축 회전을 막는다. 반대칭은 그 나머지
      const int axis = std::clamp(static_cast<int>(q["normal"].get<std::string>().at(0) - 'x'), 0, 2);
      const int o1 = (axis + 1) % 3, o2 = (axis + 2) % 3;
      dofs = t == "symmetry" ? std::set<int>{axis + 1, o1 + 4, o2 + 4} : std::set<int>{o1 + 1, o2 + 1, axis + 4};
      mark.text = std::string(t == "symmetry" ? "SYM " : "ASYM ") + "XYZ"[axis];
    } else if (t == "temperature") {
      mark.text = has(q, "value") && q["value"].is_number() ? "T=" + format_value(q["value"].get<double>()) : "TEMP";
    } else {
      continue;
    }
    mark.dofs.assign(dofs.begin(), dofs.end());
    std::unordered_set<Id> in_target;
    std::vector<V3> pts;
    try {
      for (const Json& n : resolve_target(a, q["target"], "nodes"))
        if (in_target.insert(n.get<Id>()).second) pts.push_back(m.node(n.get<Id>()));
    } catch (const Error&) {
      continue;
    }
    if (pts.empty()) continue;
    // 이름표 자리: 대상의 가운데에 가장 가까운 노드. 축 기호 자리: 거기서부터 서로 가장 먼 노드를 차례로 골라 9개까지
    V3 center{0, 0, 0};
    for (const V3& p : pts) center = add(center, p);
    center = mul(center, 1.0 / static_cast<double>(pts.size()));
    std::size_t first = 0;
    for (std::size_t i = 1; i < pts.size(); ++i)
      if (norm(sub(pts[i], center)) < norm(sub(pts[first], center))) first = i;
    mark.anchor = pts[first];
    std::vector<double> nearest(pts.size(), 1e300);
    for (std::size_t pick = first; mark.points.size() < 9;) {
      mark.points.push_back(pts[pick]);
      for (std::size_t i = 0; i < pts.size(); ++i) nearest[i] = std::min(nearest[i], norm(sub(pts[i], pts[pick])));
      const std::size_t far = static_cast<std::size_t>(std::max_element(nearest.begin(), nearest.end()) - nearest.begin());
      if (nearest[far] <= 0.0) break;  // 남은 노드가 없다
      pick = far;
    }
    // 색칠할 면: 꼭짓점이 모두 대상에 든 요소면(2D 요소는 요소 자체), 형상 표시 중이면 대상으로 준 형상의 면
    std::set<std::pair<Id, int>> geometry_faces;
    if (q["target"].value("type", std::string()) == "geometry" && has(q["target"], "ids"))
      for (const Json& g : q["target"]["ids"])
        if (g.is_array() && g.size() == 3 && g[1] == "face") geometry_faces.insert({g[0].get<Id>(), g[2].get<int>()});
    const std::size_t index = b.bc_marks.size();
    for (std::size_t i = 0; i < b.picks.size(); ++i) {
      const PickRecord& rec = b.picks[i];
      bool inside = false;
      if (rec.kind == "face") {
        inside = geometry_faces.count({rec.a, rec.b}) > 0;
      } else if (rec.kind == "element_face" || rec.kind == "element") {
        const std::ptrdiff_t e = m.find_element(rec.a);
        if (e < 0) continue;
        const Shape shape = m.shape_at(static_cast<std::size_t>(e));
        if (rec.kind == "element" && shape_info(shape).dim != 2) continue;
        const Id* nodes = m.nodes_at(static_cast<std::size_t>(e));
        const std::vector<int>& corners = rec.kind == "element" ? corner_positions(shape) : shape_faces(shape)[static_cast<std::size_t>(rec.b - 1)];
        inside = std::all_of(corners.begin(), corners.end(), [&](int k) { return in_target.count(nodes[k]) > 0; });
      }
      if (inside) pick_marks[static_cast<std::uint32_t>(i + 1)].push_back(index);
    }
    mark.color = palette[index % palette.size()];
    mark.nodes = std::move(pts);
    b.bc_marks.push_back(std::move(mark));
  }
  if (!pick_marks.empty())
    for (std::size_t k = 0; k + 2 < b.scene.triangles.size(); k += 3) {
      const auto it = pick_marks.find(b.scene.triangles[k].id);
      if (it == pick_marks.end()) continue;
      for (std::size_t index : it->second) b.bc_marks[index].triangles.push_back(k);
    }
  for (BcMark& mark : b.bc_marks) {
    if (!mark.triangles.empty()) mark.nodes.clear();        // 면으로 칠했으면 노드 표식은 필요 없다
    else if (mark.nodes.size() > 3000) mark.nodes.resize(3000);
  }
}

Built build_scene(App& a, bool surface_pick = false) {
  ViewState& vs = state(a);
  Built b;
  const bool faces_on = surface_pick || vs.mode != "wireframe", edges_on = vs.mode != "shaded";
  const std::array<std::uint8_t, 3> edge_color{30, 30, 30};
  auto pick_id = [&](PickRecord r) {
    b.picks.push_back(std::move(r));
    return static_cast<std::uint32_t>(b.picks.size());
  };
  auto colored_line = [&](const V3& p, const V3& q, const std::array<std::uint8_t, 3>& color) {
    b.scene.lines.push_back(vertex(p, {0, 0, 0}, color, 0));
    b.scene.lines.push_back(vertex(q, {0, 0, 0}, color, 0));
  };
  auto line = [&](const V3& p, const V3& q) { colored_line(p, q, edge_color); };
  // 투명(RND-17): 지정한 쪽(형상·메시)의 면은 알파를 붙여 투명 목록에 넣는다
  const std::string transparent_what = vs.transparency.is_null() ? "" : vs.transparency.value("what", std::string("all"));
  const std::uint8_t alpha = vs.transparency.is_null() ? 255 : static_cast<std::uint8_t>(std::lround(255.0 * vs.transparency.value("alpha", 0.3)));
  // 객체별 모양(WT-25, RND-16): 색은 바탕색을 바꾸고(컨투어는 그대로), alpha < 1 이면 그 객체의 면만 투명하게
  auto appearance_of = [&](Id owner) -> const Json* {
    auto it = vs.appearance.find(std::to_string(owner));
    return it == vs.appearance.end() ? nullptr : &*it;
  };
  auto push_triangle = [&](bool is_geometry, RenderVertex v, Id owner = 0) {
    bool transparent = transparent_what == "all" || (is_geometry ? transparent_what == "geometry" : transparent_what == "mesh");
    std::uint8_t a8 = alpha;
    if (const Json* ap = appearance_of(owner); ap && has(*ap, "alpha") && (*ap)["alpha"].get<double>() < 1.0)
      transparent = true, a8 = static_cast<std::uint8_t>(std::lround(255.0 * (*ap)["alpha"].get<double>()));
    if (!transparent) {
      b.scene.triangles.push_back(v);
    } else {
      v.color[3] = a8;
      b.scene.transparent.push_back(v);
    }
  };
  auto base_of = [&](Id owner, std::array<std::uint8_t, 3> color) {
    if (const Json* ap = appearance_of(owner); ap && has(*ap, "color"))
      for (std::size_t k = 0; k < 3; ++k) color[k] = static_cast<std::uint8_t>(std::clamp((*ap)["color"][k].get<int>(), 0, 255));
    return color;
  };

  const Mesh& m = a.mesh();
  // 메시가 보이는 형상 파트(auto 모드에서는 메시가 형상을 대신한다). 메시 파트를 숨기면(눈 아이콘) 형상이 다시 보인다
  std::set<Id> meshed;
  std::map<Id, std::size_t> mesh_part_elems;
  for (std::size_t i = 0; i < m.element_count(); ++i) ++mesh_part_elems[m.part_at(i)];
  for (const Object* mp : a.model().by_kind("mesh_part"))
    if (has(mp->props, "geometry") && mesh_part_elems.count(mp->id) && !vs.hidden.count(mp->id))
      meshed.insert(mp->props["geometry"].get<Id>());

  // --- 형상
  if (vs.show != "mesh" && geometry_available()) {
    std::size_t index = 0;
    for (const Object* part : a.model().by_kind("part")) {
      const std::size_t color_index = index++;
      if (part->suppressed || vs.hidden.count(part->id)) continue;
      if (vs.show == "auto" && meshed.count(part->id)) continue;
      const Json counts = geometry_counts(a, part->id);
      const bool wire_only = counts["faces"].get<int>() == 0;
      if (wire_only && counts["edges"].get<int>() == 0) continue;
      const Tessellation t = geometry_tessellation(a, part->id, vs.tess_deflection, vs.tess_angle);
      const auto color = base_of(part->id, kPalette[color_index % kPalette.size()]);
      if (wire_only) {
        // 면이 없는 파트(선 파트 — 보·트러스 골조): 모서리를 파트 색으로 그린다(표시 모드와 무관, 어두운 배경에서도 보이게)
        for (std::size_t e = 0; e + 1 < t.edge_offsets.size(); ++e)
          for (std::int64_t k = t.edge_offsets[e]; k + 1 < t.edge_offsets[e + 1]; ++k) {
            const std::size_t i0 = static_cast<std::size_t>(k) * 3;
            const V3 p{t.edge_points[i0], t.edge_points[i0 + 1], t.edge_points[i0 + 2]}, q{t.edge_points[i0 + 3], t.edge_points[i0 + 4], t.edge_points[i0 + 5]};
            colored_line(p, q, color);
            b.bounds.add(p), b.bounds.add(q);
          }
        continue;
      }
      std::map<std::int64_t, std::uint32_t> face_ids;
      if (faces_on)
        for (std::size_t k = 0; k < t.triangle_face.size(); ++k) {
          auto it = face_ids.find(t.triangle_face[k]);
          if (it == face_ids.end())
            it = face_ids.emplace(t.triangle_face[k], pick_id({"face", part->id, static_cast<int>(t.triangle_face[k])})).first;
          for (std::size_t c = 0; c < 3; ++c) {
            const std::size_t n = static_cast<std::size_t>(t.triangles[3 * k + c]);
            const V3 p{t.points[3 * n], t.points[3 * n + 1], t.points[3 * n + 2]};
            push_triangle(true, vertex(p, {t.normals[3 * n], t.normals[3 * n + 1], t.normals[3 * n + 2]}, color, it->second), part->id);
          }
        }
      for (std::size_t n = 0; n < t.points.size() / 3; ++n) b.bounds.add({t.points[3 * n], t.points[3 * n + 1], t.points[3 * n + 2]});
      for (std::size_t e = 0; edges_on && e + 1 < t.edge_offsets.size(); ++e)
        for (std::int64_t k = t.edge_offsets[e]; k + 1 < t.edge_offsets[e + 1]; ++k) {
          const std::size_t i0 = static_cast<std::size_t>(k) * 3;
          line({t.edge_points[i0], t.edge_points[i0 + 1], t.edge_points[i0 + 2]}, {t.edge_points[i0 + 3], t.edge_points[i0 + 4], t.edge_points[i0 + 5]});
        }
    }
  }

  // --- 메시
  const std::size_t mesh_tri0 = b.scene.triangles.size(), mesh_line0 = b.scene.lines.size(), mesh_tr0 = b.scene.transparent.size();
  if (vs.show != "geometry" && m.element_count() > 0) {
    // 결과 표시: 절점 값(컨투어)과 변위(변형 형상)
    std::unordered_map<Id, double> value;
    std::unordered_map<Id, V3> shift;
    double vmin = 0, vmax = 0;
    bool contour = false;
    bool expanded_shell = false;  // 결과 노드가 솔버가 펼친 쉘·보 노드라 모델 노드로 대응해 입혔다
    if (!vs.result.is_null()) {
      ResultFile& file = results(a).get(vs.result["result"].get<Id>());
      ResultFrame& frame = file.frame(vs.result["frame"].get<int>());
      if (has(vs.result, "field")) {
        const ResultField& f = file.field(frame, vs.result["field"].get<std::string>());
        const std::string comp = vs.result["component"].get<std::string>();
        std::vector<double> v;
        const auto it = std::find(f.components.begin(), f.components.end(), comp);
        if (it != f.components.end()) {
          const std::size_t c = static_cast<std::size_t>(it - f.components.begin()), nc = f.components.size();
          v.resize(f.count);
          for (std::size_t i = 0; i < f.count; ++i) v[i] = f.data[i * nc + c];
        } else {
          v = derived_values(f, comp);
        }
        vmin = 1e300, vmax = -1e300;
        for (std::size_t i = 0; i < f.count; ++i) {
          value[f.ids[i]] = v[i];
          if (m.has_node(f.ids[i])) vmin = std::min(vmin, v[i]), vmax = std::max(vmax, v[i]);
        }
        // 전개된 쉘 결과를 모델 쉘 노드 기준 면 값으로(RES-54·RES-08). shell_face 를 주지 않았는데 결과 노드가 모델 노드와 하나도 겹치지
        // 않으면(솔버가 쉘·보를 펼친 결과) 중립면(mid) 값으로 자동 대응한다 — 아니면 아무 색도 입지 않아 결과가 없어 보인다.
        if (has(vs.result, "shell_face") || vmin > vmax) {
          const std::string face = has(vs.result, "shell_face") ? vs.result["shell_face"].get<std::string>() : std::string("mid");
          std::map<Id, double> mapped;
          try {
            mapped = shell_face_values(a, file, f, comp, face);
          } catch (const Error&) {
            if (has(vs.result, "shell_face")) throw;
          }
          if (!mapped.empty()) {
            value.clear();
            for (const auto& [id, v] : mapped) value[id] = v;
            expanded_shell = true;
            vmin = 1e300, vmax = -1e300;
            for (const auto& [id, v] : value) vmin = std::min(vmin, v), vmax = std::max(vmax, v);
          }
        }
        if (has(vs.result, "min")) vmin = vs.result["min"].get<double>();
        if (has(vs.result, "max")) vmax = vs.result["max"].get<double>();
        contour = !value.empty() && vmin <= vmax;
        if (contour)
          b.legend = Json{{"field", f.name}, {"component", comp}, {"min", vmin}, {"max", vmax}, {"frame", frame.index},
                          {"levels", vs.legend_opts.value("levels", 0)}, {"colormap", vs.legend_opts.value("colormap", std::string("rainbow"))}};
      }
      const double scale = vs.result.value("deform_scale", 0.0) * (vs.result_phase != 0.0 ? std::cos(vs.result_phase * kPi / 180.0) : 1.0);
      if (scale != 0.0) {
        const ResultField& d = file.field(frame, "DISP");
        for (std::size_t i = 0; i < d.count; ++i) shift[d.ids[i]] = {scale * d.data[3 * i], scale * d.data[3 * i + 1], scale * d.data[3 * i + 2]};
        if (expanded_shell) {  // 펼쳐진 결과: 변위도 중립면 값으로 모델 노드에 입힌다
          const char* comps[3] = {"D1", "D2", "D3"};
          for (int k = 0; k < 3; ++k)
            for (const auto& [id, v] : shell_face_values(a, file, d, comps[k], "mid")) shift[id][static_cast<std::size_t>(k)] = scale * v;
        }
      }
    }
    auto pos = [&](Id n) {
      V3 p = m.node(n);
      auto it = shift.find(n);
      if (it != shift.end()) p = add(p, it->second);
      b.node_pos[n] = p;
      return p;
    };
    // 색 기준(MSH-24, PRP-13): 파트 / 프로퍼티 / 재료. 프로퍼티·재료는 요소 → 프로퍼티 대응을 만들어 쓴다
    // 색은 object_color 한 곳에서: 메시 파트·재료·프로퍼티마다 다른 색(지정 색 우선) — 트리 아이콘과 같다
    std::map<Id, std::array<std::uint8_t, 3>> part_color;
    std::unordered_map<Id, std::array<std::uint8_t, 3>> elem_color;  // color_by 가 property·material 일 때 요소 → 색
    {
      for (const Object* mp : a.model().by_kind("mesh_part")) part_color[mp->id] = object_color(a, vs.appearance, "mesh_part", mp->id);
      if (vs.color_by != "part") {
        for (const Object* pr : a.model().by_kind("property")) {
          if (pr->suppressed || !has(pr->props, "target")) continue;
          const Id mat = pr->props.contains("material") && pr->props["material"].is_number() ? pr->props["material"].get<Id>() : 0;
          if (vs.color_by == "material" && !mat) continue;
          const auto col = vs.color_by == "material" ? object_color(a, vs.appearance, "material", mat) : object_color(a, vs.appearance, "property", pr->id);
          try {
            for (const Json& e : resolve_target(a, pr->props["target"], "elements")) elem_color[e.get<Id>()] = col;
          } catch (const Error&) {
          }
        }
      }
    }
    auto base_color = [&](std::size_t elem_index) -> std::array<std::uint8_t, 3> {
      if (vs.color_by != "part") {
        auto it = elem_color.find(m.element_ids()[elem_index]);
        return it == elem_color.end() ? std::array<std::uint8_t, 3>{170, 170, 170} : it->second;  // 할당 없음 = 회색
      }
      const auto pc = part_color.find(m.part_at(elem_index));
      return pc == part_color.end() ? std::array<std::uint8_t, 3>{170, 170, 170} : pc->second;
    };
    // 결과 표시 중 그 스텝에서 제거된 요소(RES-59)는 숨긴다(픽킹 대상에서도 빠진다)
    std::set<Id> removed_now;
    if (!vs.result.is_null()) {
      ResultFile& rf = results(a).get(vs.result["result"].get<Id>());
      if (rf.case_id) removed_now = removed_elements_in_step(a, rf.case_id, rf.frame(vs.result["frame"].get<int>()).step);
    }
    auto hidden_elem = [&](std::size_t elem_index) {
      return vs.hidden.count(m.part_at(elem_index)) > 0 || (!removed_now.empty() && removed_now.count(m.element_ids()[elem_index]) > 0);
    };
    // 결과 표시 제한(RES-16): 대상 요소 밖은 컨투어 없이 바탕색, 값 범위 밖은 회색
    std::unordered_set<Id> filter_elems;
    bool filter_by_elems = false;
    double fmin = -1e300, fmax = 1e300;
    if (contour && !vs.result_filter.is_null()) {
      if (has(vs.result_filter, "target")) {
        filter_by_elems = true;
        for (const Json& e : resolve_target(a, vs.result_filter["target"], "elements")) filter_elems.insert(e.get<Id>());
      }
      if (has(vs.result_filter, "min")) fmin = vs.result_filter["min"].get<double>();
      if (has(vs.result_filter, "max")) fmax = vs.result_filter["max"].get<double>();
    }
    const std::string cmap_name = vs.legend_opts.value("colormap", std::string("rainbow"));
    const int cmap_levels = vs.legend_opts.value("levels", 0);
    const bool out_gray = vs.legend_opts.value("out_of_range", std::string("clamp")) == "gray";
    const bool element_values = contour && vs.result.value("location", std::string("nodal")) == "element";
    std::unordered_map<std::size_t, double> elem_value;  // location=element: 요소 꼭짓점 값의 평균
    auto value_at = [&](Id node, std::size_t elem_index, double& out) -> bool {
      if (element_values) {
        auto it = elem_value.find(elem_index);
        if (it == elem_value.end()) {
          const Id* en = m.nodes_at(elem_index);
          double sum = 0;
          std::size_t cnt = 0;
          for (int k : corner_positions(m.shape_at(elem_index))) {
            auto v = value.find(en[k]);
            if (v != value.end()) sum += v->second, ++cnt;
          }
          if (!cnt) return false;
          it = elem_value.emplace(elem_index, sum / static_cast<double>(cnt)).first;
        }
        out = it->second;
        return true;
      }
      auto it = value.find(node);
      if (it == value.end()) return false;
      out = it->second;
      return true;
    };
    auto color_of = [&](Id node, const std::array<std::uint8_t, 3>& base, std::size_t elem_index) {
      if (!contour || (filter_by_elems && !filter_elems.count(m.element_ids()[elem_index]))) return base;
      double v;
      if (!value_at(node, elem_index, v)) return std::array<std::uint8_t, 3>{128, 128, 128};
      if (v < fmin || v > fmax) return std::array<std::uint8_t, 3>{150, 150, 150};
      if (out_gray && (v < vmin || v > vmax)) return std::array<std::uint8_t, 3>{150, 150, 150};
      return colormap(vmax > vmin ? (v - vmin) / (vmax - vmin) : 0.5, cmap_name, cmap_levels);
    };
    // 메시 표시 옵션(RND-20~22): 요소 경계선 끔, 요소 축소(면을 요소 중심 쪽으로 줄여 요소 하나하나가 보이게)
    const bool mesh_edges = vs.mesh_options.value("edges", true);
    const double shrink = std::clamp(vs.mesh_options.value("shrink", 0.0), 0.0, 0.9);
    std::vector<V3> shrink_centers;
    if (shrink > 0.0) {
      shrink_centers.resize(m.element_count(), V3{0, 0, 0});
      for (std::size_t i = 0; i < m.element_count(); ++i) {
        const Id* nodes = m.nodes_at(i);
        std::size_t count = 0;
        for (int k : corner_positions(m.shape_at(i)))
          if (nodes[k]) shrink_centers[i] = add(shrink_centers[i], pos(nodes[k])), ++count;
        if (count) shrink_centers[i] = mul(shrink_centers[i], 1.0 / static_cast<double>(count));
      }
    }
    auto shrink_point = [&](const V3& p, std::size_t i) {
      return shrink > 0.0 ? add(shrink_centers[i], mul(sub(p, shrink_centers[i]), 1.0 - shrink)) : p;
    };
    // 1D/2D 입체 표시(RND-22): 요소 → 프로퍼티(두께·단면)를 찾아 쉘은 두께만큼의 프리즘, 보는 단면 상자로 그린다
    const bool solid_1d_2d = vs.mesh_options.value("solid_1d_2d", false);
    const bool beam_axes = vs.mesh_options.value("beam_axes", false);
    std::unordered_map<Id, const Object*> elem_property;
    std::map<Id, V3> beam_dir;
    if (solid_1d_2d || beam_axes) {
      for (const Object* pr : a.model().by_kind("property")) {
        if (pr->suppressed || !has(pr->props, "target")) continue;
        try {
          for (const Json& e : resolve_target(a, pr->props["target"], "elements")) elem_property[e.get<Id>()] = pr;
        } catch (const Error&) {
        }
      }
      if (const Object* st = a.find_settings())
        for (const Json& row : st->props.value("beam_directions", Json::array()))
          beam_dir[row[0].get<Id>()] = V3{row[1].get<double>(), row[2].get<double>(), row[3].get<double>()};
    }
    // 두께(쉘·멤브레인·복합재 층 합). 없으면 0
    auto shell_thickness = [&](std::size_t elem_index, double& offset) -> double {
      auto it = elem_property.find(m.element_ids()[elem_index]);
      if (it == elem_property.end()) return 0.0;
      const Json& q = it->second->props;
      const std::string t = q.value("type", std::string());
      offset = q.value("offset", 0.0);
      if (t == "shell" || t == "membrane") return q.value("thickness", 0.0);
      if (t == "composite") {
        double sum = 0;
        for (const Json& layer : q.value("layers", Json::array())) sum += layer.value("thickness", 0.0);
        return sum;
      }
      return 0.0;
    };
    // 보 단면의 두 반폭(1축·2축)과 1축 방향. 단면을 모르면 false
    // 보 단면(PRP-05, D15): 부분 직사각형 목록(형강은 여러 개)과 1·2축, 전체 단면 중심의 이동(오프셋: 축 = 기준선 - offset × 크기)
    std::string sec_kind;              // beam_section 이 채운다: 단면 종류("circ"·"pipe" 는 원통으로 그린다)
    std::vector<double> sec_dims;
    auto beam_section = [&](std::size_t elem_index, const V3& axis, std::vector<BeamRect>& rects, V3& a1, V3& a2, V3& shift) -> bool {
      auto it = elem_property.find(m.element_ids()[elem_index]);
      if (it == elem_property.end()) return false;
      const Json& q = it->second->props;
      const std::string t = q.value("type", std::string());
      rects.clear();
      sec_kind.clear(), sec_dims.clear();
      double o1 = 0, o2 = 0, H = 0, B = 0;
      if (t == "truss") {
        const double side = std::sqrt(std::max(q.value("area", 0.0), 0.0));
        rects.push_back({side, side, 0, 0});
      } else if (t == "beam" && has(q, "dimensions")) {
        const std::string sec = q.value("section", std::string("rect"));
        sec_kind = sec, sec_dims = q["dimensions"].get<std::vector<double>>();
        rects = beam_section_rects(sec, q["dimensions"].get<std::vector<double>>());
        std::tie(H, B) = beam_section_extent(sec, q["dimensions"].get<std::vector<double>>());
        o1 = q.value("offset1", 0.0), o2 = q.value("offset2", 0.0);
        if (!beam_section_composite(sec) && !rects.empty()) H = rects[0].t1, B = rects[0].t2;  // 오프셋 단위 = 그 방향 두께
      } else {
        return false;
      }
      for (const BeamRect& r : rects)
        if (r.t1 <= 0 || r.t2 <= 0) return false;
      if (rects.empty()) return false;
      V3 dir{0, 0, -1};  // 솔버 기본 1축 방향
      auto bd = beam_dir.find(m.element_ids()[elem_index]);
      if (bd != beam_dir.end()) dir = bd->second;
      else if (has(q, "direction")) dir = v3(q["direction"]);
      a1 = sub(dir, mul(axis, dot(dir, axis)));
      if (norm(a1) < 1e-9) a1 = cross(axis, std::fabs(axis[0]) < 0.9 ? V3{1, 0, 0} : V3{0, 1, 0});
      a1 = unit(a1);
      a2 = unit(cross(axis, a1));
      shift = add(mul(a1, -o1 * H), mul(a2, -o2 * B));
      return true;
    };
    auto quad = [&](const V3& p0, const V3& p1, const V3& p2, const V3& p3, const std::array<std::uint8_t, 3>& c, std::uint32_t id, Id owner) {
      const V3 nn = unit(cross(sub(p1, p0), sub(p2, p0)));
      push_triangle(false, vertex(p0, nn, c, id), owner), push_triangle(false, vertex(p1, nn, c, id), owner), push_triangle(false, vertex(p2, nn, c, id), owner);
      push_triangle(false, vertex(p0, nn, c, id), owner), push_triangle(false, vertex(p2, nn, c, id), owner), push_triangle(false, vertex(p3, nn, c, id), owner);
    };

    // 솔리드의 바깥 면: 요소 하나만 가진 면
    struct FaceRef {
      std::size_t elem;
      int face;
      int count;
    };
    std::unordered_map<std::array<Id, 4>, FaceRef, KeyHash> faces;
    std::vector<FaceRef> separated_faces;
    for (std::size_t i = 0; i < m.element_count(); ++i) {
      const Shape shape = m.shape_at(i);
      if (shape_info(shape).dim != 3 || hidden_elem(i)) continue;
      const Id* n = m.nodes_at(i);
      const auto& table = shape_faces(shape);
      for (std::size_t f = 0; f < table.size(); ++f) {
        if (shrink > 0.0) {
          separated_faces.push_back(FaceRef{i, static_cast<int>(f), 1});
          continue;  // 요소 사이가 벌어지면 공유하던 내부 면도 노출된다.
        }
        std::array<Id, 4> key{0, 0, 0, 0};
        for (std::size_t k = 0; k < table[f].size() && k < 4; ++k) key[k] = n[table[f][k]];
        std::sort(key.begin(), key.end());
        auto it = faces.find(key);
        if (it == faces.end()) faces.emplace(key, FaceRef{i, static_cast<int>(f), 1});
        else ++it->second.count;
      }
    }
    std::set<std::pair<Id, Id>> edges;
    auto polygon = [&](const std::vector<Id>& ids, const std::array<std::uint8_t, 3>& base, std::uint32_t id, std::size_t elem_index) {
      std::vector<V3> p;
      for (Id n : ids) p.push_back(pos(n)), b.bounds.add(p.back());
      for (V3& q : p) q = shrink_point(q, elem_index);
      if (faces_on)
        for (std::size_t k = 1; k + 1 < p.size(); ++k) {  // 부채꼴로 삼각형 분할
          const V3 normal = unit(cross(sub(p[k], p[0]), sub(p[k + 1], p[0])));
          push_triangle(false, vertex(p[0], normal, color_of(ids[0], base, elem_index), id), m.part_at(elem_index));
          push_triangle(false, vertex(p[k], normal, color_of(ids[k], base, elem_index), id), m.part_at(elem_index));
          push_triangle(false, vertex(p[k + 1], normal, color_of(ids[k + 1], base, elem_index), id), m.part_at(elem_index));
        }
      if (edges_on && mesh_edges)
        for (std::size_t k = 0; k < ids.size(); ++k) {
          const Id u = ids[k], v = ids[(k + 1) % ids.size()];
          if (shrink > 0.0 || edges.insert({std::min(u, v), std::max(u, v)}).second) line(p[k], p[(k + 1) % ids.size()]);
        }
    };
    auto draw_face = [&](const FaceRef& ref) {
      const Id* n = m.nodes_at(ref.elem);
      const auto& corners = shape_faces(m.shape_at(ref.elem))[static_cast<std::size_t>(ref.face)];
      std::vector<Id> ids;
      // 요소면의 절점 순서는 법선이 요소 안쪽을 향한다 → 뒤집어 바깥을 향하게 그린다
      for (auto it = corners.rbegin(); it != corners.rend(); ++it) ids.push_back(n[*it]);
      polygon(ids, base_color(ref.elem), pick_id({"element_face", m.element_ids()[ref.elem], ref.face + 1}), ref.elem);
    };
    for (const auto& [key, ref] : faces)
      if (ref.count == 1) draw_face(ref);
    for (const FaceRef& ref : separated_faces) draw_face(ref);
    // --- 단면 결과(RES-15): 볼록 요소를 평면으로 자르면 변과의 교점이 볼록 다각형을 이룬다 → 교점을 중심 둘레로 정렬해 부채꼴로
    if (!vs.section.is_null()) {
      const V3 sp = v3(vs.section["point"]), sn = unit(v3(vs.section["normal"]));
      const std::array<std::uint8_t, 3> plain{200, 200, 200};
      for (std::size_t i = 0; i < m.element_count(); ++i) {
        const ShapeInfo& info = shape_info(m.shape_at(i));
        if (info.dim != 3 || hidden_elem(i)) continue;
        const Id* n = m.nodes_at(i);
        const auto& edges = shape_edges(info.shape);
        struct Cut {
          V3 p;
          double v;
          double ang;
        };
        std::vector<Cut> cuts;
        for (const auto& ed : edges) {
          const Id u = n[ed[0]], w = n[ed[1]];
          const V3 pu = pos(u), pw = pos(w);
          const double du = dot(sub(pu, sp), sn), dw = dot(sub(pw, sp), sn);
          if ((du < 0) == (dw < 0) || du == dw) continue;
          const double t = du / (du - dw);
          double val = 0;
          if (contour) {
            auto a_ = value.find(u), b_ = value.find(w);
            if (a_ != value.end() && b_ != value.end()) val = a_->second + t * (b_->second - a_->second);
          }
          cuts.push_back({add(pu, mul(sub(pw, pu), t)), val, 0});
        }
        if (cuts.size() < 3) continue;
        V3 c{0, 0, 0};
        for (const Cut& k : cuts) c = add(c, k.p);
        c = mul(c, 1.0 / static_cast<double>(cuts.size()));
        const V3 ex = unit(cross(sn, std::fabs(sn[0]) < 0.9 ? V3{1, 0, 0} : V3{0, 1, 0})), ey = cross(sn, ex);
        for (Cut& k : cuts) k.ang = std::atan2(dot(sub(k.p, c), ey), dot(sub(k.p, c), ex));
        std::sort(cuts.begin(), cuts.end(), [](const Cut& x, const Cut& y) { return x.ang < y.ang; });
        const std::uint32_t id = pick_id({"element", m.element_ids()[i], 0});
        auto col = [&](const Cut& k) { return contour ? colormap(vmax > vmin ? (k.v - vmin) / (vmax - vmin) : 0.5, cmap_name, cmap_levels) : plain; };
        for (std::size_t k = 1; k + 1 < cuts.size(); ++k) {
          b.scene.triangles.push_back(vertex(cuts[0].p, sn, col(cuts[0]), id));
          b.scene.triangles.push_back(vertex(cuts[k].p, sn, col(cuts[k]), id));
          b.scene.triangles.push_back(vertex(cuts[k + 1].p, sn, col(cuts[k + 1]), id));
        }
      }
    }
    // --- 등가면(RES-41): 요소를 사면체로 나눠 마칭(변마다 보간점, 3점 또는 4점 → 삼각형 1~2개)
    if (!vs.iso.is_null() && contour) {
      for (const Json& iv : vs.iso.value("values", Json::array())) {
        const double level = iv.get<double>();
        const auto color = colormap(vmax > vmin ? (level - vmin) / (vmax - vmin) : 0.5, cmap_name, cmap_levels);
        for (std::size_t i = 0; i < m.element_count(); ++i) {
          const ShapeInfo& info = shape_info(m.shape_at(i));
          if (info.dim != 3 || hidden_elem(i)) continue;
          const Id* n = m.nodes_at(i);
          const std::uint32_t id = pick_id({"element", m.element_ids()[i], 0});
          for (const auto& t4 : tets_of(info.shape)) {
            double f[4];
            V3 p[4];
            bool ok = true;
            for (int k = 0; k < 4 && ok; ++k) {
              auto it = value.find(n[t4[k]]);
              if (it == value.end()) ok = false;
              else f[k] = it->second - level, p[k] = pos(n[t4[k]]);
            }
            if (!ok) continue;
            std::vector<V3> pts;
            static const int pairs[6][2] = {{0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}};
            for (const auto& pr : pairs) {
              const double a_ = f[pr[0]], b_ = f[pr[1]];
              if ((a_ < 0) == (b_ < 0) || a_ == b_) continue;
              pts.push_back(add(p[pr[0]], mul(sub(p[pr[1]], p[pr[0]]), a_ / (a_ - b_))));
            }
            if (pts.size() < 3) continue;
            if (pts.size() == 4) {  // 네 점은 사각형: 중심 둘레로 정렬
              V3 c{0, 0, 0};
              for (const V3& q : pts) c = add(c, q);
              c = mul(c, 0.25);
              const V3 nn = unit(cross(sub(pts[1], pts[0]), sub(pts[2], pts[0])));
              const V3 ex = unit(sub(pts[0], c)), ey = cross(nn, ex);
              std::sort(pts.begin(), pts.end(), [&](const V3& x, const V3& y) {
                return std::atan2(dot(sub(x, c), ey), dot(sub(x, c), ex)) < std::atan2(dot(sub(y, c), ey), dot(sub(y, c), ex));
              });
            }
            for (std::size_t k = 1; k + 1 < pts.size(); ++k) {
              const V3 nn = unit(cross(sub(pts[k], pts[0]), sub(pts[k + 1], pts[0])));
              b.scene.triangles.push_back(vertex(pts[0], nn, color, id)), b.scene.triangles.push_back(vertex(pts[k], nn, color, id)), b.scene.triangles.push_back(vertex(pts[k + 1], nn, color, id));
            }
          }
        }
      }
    }
    // --- 유선(RND-33): 벡터 결과를 사면체 분할의 무게중심 보간으로 RK2 적분한 꺾은선(크기로 색)
    if (!vs.streamlines.is_null() && !vs.result.is_null()) {
      ResultFile& file = results(a).get(vs.result["result"].get<Id>());
      ResultFrame& frame = file.frame(vs.result["frame"].get<int>());
      const ResultField& vf = file.field(frame, vs.streamlines.value("field", std::string("DISP")));
      if (vf.components.size() >= 3) {
        std::unordered_map<Id, V3> vec;
        double vmag = 0;
        const std::size_t nc = vf.components.size();
        for (std::size_t i = 0; i < vf.count; ++i) {
          const V3 v{vf.data[i * nc], vf.data[i * nc + 1], vf.data[i * nc + 2]};
          vec[vf.ids[i]] = v, vmag = std::max(vmag, norm(v));
        }
        // 노드 → 요소, 노드 좌표 목록(가까운 노드 찾기)
        std::unordered_map<Id, std::vector<std::size_t>> elems_of;
        for (std::size_t i = 0; i < m.element_count(); ++i) {
          if (shape_info(m.shape_at(i)).dim != 3) continue;
          const Id* n = m.nodes_at(i);
          for (std::size_t k = 0; k < shape_info(m.shape_at(i)).corners; ++k) elems_of[n[k]].push_back(i);
        }
        auto sample = [&](const V3& q, V3& out) -> bool {  // q 를 담은 사면체를 찾아 무게중심 보간
          double best = 1e300;
          Id near = 0;
          for (std::size_t i = 0; i < m.node_count(); ++i) {
            const Id nid = m.node_ids()[i];
            if (!elems_of.count(nid)) continue;
            const double d = norm(sub(pos(nid), q));
            if (d < best) best = d, near = nid;
          }
          if (!near) return false;
          std::set<std::size_t> cand(elems_of[near].begin(), elems_of[near].end());
          for (std::size_t e : elems_of[near])  // 이웃의 이웃까지
            for (std::size_t k = 0; k < shape_info(m.shape_at(e)).corners; ++k)
              for (std::size_t e2 : elems_of[m.nodes_at(e)[k]]) cand.insert(e2);
          for (std::size_t e : cand) {
            const Id* n = m.nodes_at(e);
            for (const auto& t4 : tets_of(m.shape_at(e))) {
              const V3 p0 = pos(n[t4[0]]), p1 = pos(n[t4[1]]), p2 = pos(n[t4[2]]), p3 = pos(n[t4[3]]);
              const V3 e1 = sub(p1, p0), e2 = sub(p2, p0), e3 = sub(p3, p0), r = sub(q, p0);
              const double det = dot(e1, cross(e2, e3));
              if (std::fabs(det) < 1e-300) continue;
              const double l1 = dot(r, cross(e2, e3)) / det, l2 = dot(e1, cross(r, e3)) / det, l3 = dot(e1, cross(e2, r)) / det, l0 = 1 - l1 - l2 - l3;
              const double eps = -1e-6;
              if (l0 < eps || l1 < eps || l2 < eps || l3 < eps) continue;
              const double l[4] = {l0, l1, l2, l3};
              out = {0, 0, 0};
              for (int k = 0; k < 4; ++k) {
                auto it = vec.find(n[t4[k]]);
                if (it == vec.end()) return false;
                out = add(out, mul(it->second, l[k]));
              }
              return true;
            }
          }
          return false;
        };
        const int steps = vs.streamlines.value("steps", 200);
        const double h = has(vs.streamlines, "step_size") ? vs.streamlines["step_size"].get<double>() : 0.01 * b.bounds.radius();
        const std::string direction = vs.streamlines.value("direction", std::string("forward"));
        for (const Json& seed : vs.streamlines.value("seeds", Json::array())) {
          for (const double sign : {1.0, -1.0}) {
            if ((sign > 0 && direction == "backward") || (sign < 0 && direction == "forward")) continue;
            V3 q = v3(seed);
            V3 v;
            if (!sample(q, v)) continue;
            for (int st = 0; st < steps; ++st) {
              const double mag = norm(v);
              if (mag <= 0) break;
              V3 mid = add(q, mul(v, sign * 0.5 * h / mag)), vm;
              if (!sample(mid, vm) || norm(vm) <= 0) break;
              const V3 next = add(q, mul(vm, sign * h / norm(vm)));
              V3 vn;
              if (!sample(next, vn)) break;
              const auto c0 = colormap(vmag > 0 ? mag / vmag : 0.5, cmap_name, cmap_levels), c1 = colormap(vmag > 0 ? norm(vn) / vmag : 0.5, cmap_name, cmap_levels);
              b.scene.lines.push_back(vertex(q, {0, 0, 0}, c0, 0)), b.scene.lines.push_back(vertex(next, {0, 0, 0}, c1, 0));
              q = next, v = vn;
            }
          }
        }
      }
    }
    for (std::size_t i = 0; i < m.element_count(); ++i) {
      const ShapeInfo& info = shape_info(m.shape_at(i));
      if (info.dim == 3 || info.dim == 0 || hidden_elem(i)) continue;
      const Id* n = m.nodes_at(i);
      std::vector<Id> ids;
      for (int k : corner_positions(info.shape)) ids.push_back(n[k]);
      if (std::any_of(ids.begin(), ids.end(), [](Id x) { return x == 0; })) continue;  // 네트워크 요소의 입구·출구
      const auto base = base_color(i);
      const std::uint32_t eid = pick_id({"element", m.element_ids()[i], 0});
      double offset = 0, thick = 0;
      if (info.dim == 2 && solid_1d_2d && (thick = shell_thickness(i, offset)) > 0) {
        // 쉘 프리즘: 기준면에서 오프셋(두께 단위)만큼 옮긴 중립면의 위·아래 면과 옆면
        std::vector<V3> p;
        for (Id n_ : ids) p.push_back(pos(n_));
        V3 nn{0, 0, 0};
        for (std::size_t k = 1; k + 1 < p.size(); ++k) nn = add(nn, cross(sub(p[k], p[0]), sub(p[k + 1], p[0])));
        nn = unit(nn);
        const V3 up = mul(nn, thick * (0.5 - offset)), down = mul(nn, -thick * (0.5 + offset));
        std::vector<V3> top, bottom;
        for (const V3& q : p) top.push_back(add(q, up)), bottom.push_back(add(q, down)), b.bounds.add(top.back()), b.bounds.add(bottom.back());
        for (V3& q : top) q = shrink_point(q, i);
        for (V3& q : bottom) q = shrink_point(q, i);
        if (faces_on) {
          for (std::size_t k = 1; k + 1 < p.size(); ++k) {
            push_triangle(false, vertex(top[0], nn, color_of(ids[0], base, i), eid), m.part_at(i));
            push_triangle(false, vertex(top[k], nn, color_of(ids[k], base, i), eid), m.part_at(i));
            push_triangle(false, vertex(top[k + 1], nn, color_of(ids[k + 1], base, i), eid), m.part_at(i));
            const V3 dn = mul(nn, -1.0);
            push_triangle(false, vertex(bottom[0], dn, color_of(ids[0], base, i), eid), m.part_at(i));
            push_triangle(false, vertex(bottom[k + 1], dn, color_of(ids[k + 1], base, i), eid), m.part_at(i));
            push_triangle(false, vertex(bottom[k], dn, color_of(ids[k], base, i), eid), m.part_at(i));
          }
          for (std::size_t k = 0; k < p.size(); ++k) {
            const std::size_t j = (k + 1) % p.size();
            quad(bottom[k], bottom[j], top[j], top[k], color_of(ids[k], base, i), eid, m.part_at(i));
          }
        }
        if (edges_on && mesh_edges)
          for (std::size_t k = 0; k < p.size(); ++k) {
            const std::size_t j = (k + 1) % p.size();
            line(top[k], top[j]), line(bottom[k], bottom[j]), line(top[k], bottom[k]);
          }
      } else if (info.dim == 2) {
        polygon(ids, base, eid, i);
      } else {
        const V3 p = pos(ids[0]), q = pos(ids[1]);
        b.bounds.add(p), b.bounds.add(q);
        std::vector<BeamRect> rects;
        V3 a1, a2, shift;
        const V3 axis = unit(sub(q, p));
        if (solid_1d_2d && norm(sub(q, p)) > 0 && beam_section(i, axis, rects, a1, a2, shift)) {
          const auto col0 = color_of(ids[0], base, i), col1 = color_of(ids[1], base, i);
          if ((sec_kind == "circ" || sec_kind == "pipe") && sec_dims.size() >= 2 && sec_dims[0] > 0) {
            // 타원·원형 단면은 원통, 파이프는 속 빈 원통(바깥 반지름 r, 두께 t)으로 — 24 각형 근사
            const int N = 24;
            const bool hollow = sec_kind == "pipe";
            const double r1 = hollow ? sec_dims[0] : sec_dims[0] / 2, r2 = hollow ? sec_dims[0] : sec_dims[1] / 2;
            const double ri = hollow ? std::max(sec_dims[0] - sec_dims[1], 0.0) : 0.0;
            const V3 pc = add(p, shift), qc = add(q, shift);
            auto ring = [&](const V3& c, double ra, double rb, int k) {
              const double ang = 2 * 3.14159265358979323846 * k / N;
              return add(c, add(mul(a1, ra * std::cos(ang)), mul(a2, rb * std::sin(ang))));
            };
            auto wall = [&](double ra, double rb, bool inward) {  // 옆면(원통 벽). inward: 안쪽 벽은 법선이 안쪽
              for (int k = 0; k < N; ++k) {
                const int j = (k + 1) % N;
                V3 p0 = ring(pc, ra, rb, k), p1 = ring(pc, ra, rb, j), q0 = ring(qc, ra, rb, k), q1 = ring(qc, ra, rb, j);
                b.bounds.add(p0), b.bounds.add(q0);
                p0 = shrink_point(p0, i), p1 = shrink_point(p1, i), q0 = shrink_point(q0, i), q1 = shrink_point(q1, i);
                if (faces_on) {
                  V3 nn = unit(cross(sub(p1, p0), sub(q0, p0)));
                  if (inward) nn = mul(nn, -1.0);
                  if (inward) std::swap(p0, p1), std::swap(q0, q1);
                  push_triangle(false, vertex(p0, nn, col0, eid), m.part_at(i)), push_triangle(false, vertex(p1, nn, col0, eid), m.part_at(i)), push_triangle(false, vertex(q1, nn, col1, eid), m.part_at(i));
                  push_triangle(false, vertex(p0, nn, col0, eid), m.part_at(i)), push_triangle(false, vertex(q1, nn, col1, eid), m.part_at(i)), push_triangle(false, vertex(q0, nn, col1, eid), m.part_at(i));
                }
                if (edges_on && mesh_edges) line(p0, p1), line(q0, q1);
              }
            };
            wall(r1, r2, false);
            if (hollow && ri > 0) wall(ri, ri, true);
            if (faces_on)  // 끝면: 원은 부채꼴, 파이프는 고리
              for (int end = 0; end < 2; ++end) {
                const V3& c = end ? qc : pc;
                const V3 nn = end ? axis : mul(axis, -1.0);
                const auto col = end ? col1 : col0;
                for (int k = 0; k < N; ++k) {
                  const int j = (k + 1) % N;
                  V3 o0 = shrink_point(ring(c, r1, r2, k), i), o1 = shrink_point(ring(c, r1, r2, j), i);
                  if (hollow && ri > 0) {
                    const V3 i0 = shrink_point(ring(c, ri, ri, k), i), i1 = shrink_point(ring(c, ri, ri, j), i);
                    if (end) quad(o0, o1, i1, i0, col, eid, m.part_at(i));
                    else quad(o1, o0, i0, i1, col, eid, m.part_at(i));
                  } else {
                    const V3 cc = shrink_point(c, i);
                    if (end) push_triangle(false, vertex(cc, nn, col, eid), m.part_at(i)), push_triangle(false, vertex(o0, nn, col, eid), m.part_at(i)), push_triangle(false, vertex(o1, nn, col, eid), m.part_at(i));
                    else push_triangle(false, vertex(cc, nn, col, eid), m.part_at(i)), push_triangle(false, vertex(o1, nn, col, eid), m.part_at(i)), push_triangle(false, vertex(o0, nn, col, eid), m.part_at(i));
                  }
                }
              }
            rects.clear();  // 아래 상자 그리기는 건너뛴다
          }
          // 부분 직사각형마다 보 상자: 1축(a1)·2축(a2) 반폭만큼의 직사각형을 요소 양 끝에 두고 옆면 4개 + 끝면 2개(박스 단면은 벽 4개 → 속이 빈다)
          for (const BeamRect& r : rects) {
            const V3 center = add(shift, add(mul(a1, r.c1), mul(a2, r.c2)));
            const V3 e1 = mul(a1, r.t1 / 2), e2 = mul(a2, r.t2 / 2);
            const V3 pc = add(p, center), qc = add(q, center);
            std::array<V3, 4> c0{add(add(pc, e1), e2), add(sub(pc, e1), e2), sub(sub(pc, e1), e2), sub(add(pc, e1), e2)};
            std::array<V3, 4> c1{add(add(qc, e1), e2), add(sub(qc, e1), e2), sub(sub(qc, e1), e2), sub(add(qc, e1), e2)};
            for (const V3& v : c0) b.bounds.add(v);
            for (const V3& v : c1) b.bounds.add(v);
            for (V3& v : c0) v = shrink_point(v, i);
            for (V3& v : c1) v = shrink_point(v, i);
            if (faces_on) {
              for (int k = 0; k < 4; ++k) {
                const int j = (k + 1) % 4;
                const V3 nn = unit(cross(sub(c0[static_cast<std::size_t>(j)], c0[static_cast<std::size_t>(k)]), sub(c1[static_cast<std::size_t>(k)], c0[static_cast<std::size_t>(k)])));
                push_triangle(false, vertex(c0[static_cast<std::size_t>(k)], nn, col0, eid), m.part_at(i));
                push_triangle(false, vertex(c0[static_cast<std::size_t>(j)], nn, col0, eid), m.part_at(i));
                push_triangle(false, vertex(c1[static_cast<std::size_t>(j)], nn, col1, eid), m.part_at(i));
                push_triangle(false, vertex(c0[static_cast<std::size_t>(k)], nn, col0, eid), m.part_at(i));
                push_triangle(false, vertex(c1[static_cast<std::size_t>(j)], nn, col1, eid), m.part_at(i));
                push_triangle(false, vertex(c1[static_cast<std::size_t>(k)], nn, col1, eid), m.part_at(i));
              }
              quad(c0[0], c0[3], c0[2], c0[1], col0, eid, m.part_at(i));
              quad(c1[0], c1[1], c1[2], c1[3], col1, eid, m.part_at(i));
            }
            if (edges_on && mesh_edges)
              for (int k = 0; k < 4; ++k) {
                const int j = (k + 1) % 4;
                line(c0[static_cast<std::size_t>(k)], c0[static_cast<std::size_t>(j)]), line(c1[static_cast<std::size_t>(k)], c1[static_cast<std::size_t>(j)]);
                line(c0[static_cast<std::size_t>(k)], c1[static_cast<std::size_t>(k)]);
              }
          }
        } else {
          // 선 요소(보·트러스)를 선으로: 파트 색(컨투어가 있으면 노드 값의 색). 모서리색(짙은 회색)은 어두운 배경에서 안 보인다
          b.scene.lines.push_back(vertex(shrink_point(p, i), {0, 0, 0}, color_of(ids[0], base, i), eid));
          b.scene.lines.push_back(vertex(shrink_point(q, i), {0, 0, 0}, color_of(ids[1], base, i), eid));
        }
        // 1축 방향 표식(PRP-07): 요소 중앙에서 단면 1축 쪽으로 주황 선. 단면을 알면 높이의 반, 모르면 요소 길이의 15%(방향은 프로퍼티·요소별 지정, 없으면 솔버 기본 (0,0,-1))
        if (beam_axes && norm(sub(q, p)) > 0) {
          const V3 mid = mul(add(p, q), 0.5);
          double len = 0.15 * norm(sub(q, p));
          V3 a1m;
          if (beam_section(i, axis, rects, a1, a2, shift)) {
            double H = 0;
            for (const BeamRect& r : rects) H = std::max(H, 2 * (std::fabs(r.c1) + r.t1 / 2));
            if (H > 0) len = 0.5 * H;
            a1m = a1;
          } else {
            V3 dir{0, 0, -1};
            auto bd = beam_dir.find(m.element_ids()[i]);
            if (bd != beam_dir.end()) dir = bd->second;
            a1m = sub(dir, mul(axis, dot(dir, axis)));
            if (norm(a1m) < 1e-9) continue;  // 축과 나란: 표식 없음(case.check 가 오류로 잡는다)
            a1m = unit(a1m);
          }
          const std::array<std::uint8_t, 3> orange{235, 140, 40};
          b.scene.lines.push_back(vertex(mid, {0, 0, 0}, orange, 0));
          b.scene.lines.push_back(vertex(add(mid, mul(a1m, len)), {0, 0, 0}, orange, 0));
        }
      }
    }
    // 축대칭 전개(RES-56): 2D 요소(x 반지름, y 축 — 솔버의 규약)의 자유 변을 y 축 둘레로 돌려 겉면을 만든다. 색은 노드 값을 그대로 따른다
    if (!vs.expand.is_null() && vs.expand.value("kind", std::string()) == "axisymmetric") {
      const int segs = std::max(3, vs.expand.value("segments", 36));
      const double total = vs.expand.value("angle", 360.0) * 3.14159265358979323846 / 180.0;
      struct EdgeUse {
        int count = 0;
        std::size_t elem = 0;
        Id u = 0, v = 0;  // 요소의 꼭짓점 순서대로(바깥 법선이 일정하게 나오도록)
      };
      std::map<std::pair<Id, Id>, EdgeUse> edge_count;  // 정렬한 변 → 쓰임
      for (std::size_t i = 0; i < m.element_count(); ++i) {
        const ShapeInfo& info = shape_info(m.shape_at(i));
        if (info.dim != 2 || hidden_elem(i)) continue;
        const Id* n = m.nodes_at(i);
        const std::vector<int> corners = corner_positions(info.shape);
        for (std::size_t k = 0; k < corners.size(); ++k) {
          const Id u = n[corners[k]], v = n[corners[(k + 1) % corners.size()]];
          auto& e = edge_count[{std::min(u, v), std::max(u, v)}];
          ++e.count, e.elem = i, e.u = u, e.v = v;
        }
      }
      const V3 axis_pt{0, 0, 0}, axis_dir{0, 1, 0};
      for (const auto& [key, ce] : edge_count) {
        if (ce.count != 1) continue;  // 자유 변만
        const Id u = ce.v, v = ce.u;  // 그림으로 확인: 요소 순서 그대로 돌리면 법선이 안쪽을 향해 뒷면 색이 나온다 → 뒤집는다
        (void)key;
        const std::array<std::uint8_t, 3> cu = color_of(u, base_color(ce.elem), ce.elem), cv = color_of(v, base_color(ce.elem), ce.elem);
        const V3 pu = pos(u), pv = pos(v);
        for (int k = 0; k < segs; ++k) {
          const double a0 = total * k / segs, a1 = total * (k + 1) / segs;
          const V3 q00 = rotate_about(pu, axis_pt, axis_dir, a0), q10 = rotate_about(pv, axis_pt, axis_dir, a0);
          const V3 q01 = rotate_about(pu, axis_pt, axis_dir, a1), q11 = rotate_about(pv, axis_pt, axis_dir, a1);
          V3 nrm = unit(cross(sub(q10, q00), sub(q01, q00)));
          if (norm(sub(q10, q00)) < 1e-12 || norm(sub(q01, q00)) < 1e-12) nrm = unit(cross(sub(q11, q10), sub(q01, q10)));
          const std::uint32_t id = pick_id({"element", m.element_ids()[ce.elem], 0});
          b.scene.triangles.push_back(vertex(q00, nrm, cu, id)), b.scene.triangles.push_back(vertex(q10, nrm, cv, id)), b.scene.triangles.push_back(vertex(q11, nrm, cv, id));
          b.scene.triangles.push_back(vertex(q00, nrm, cu, id)), b.scene.triangles.push_back(vertex(q11, nrm, cv, id)), b.scene.triangles.push_back(vertex(q01, nrm, cu, id));
          b.bounds.add(q01), b.bounds.add(q11);
          if (edges_on && (k % std::max(1, segs / 12)) == 0) line(q00, q10);
        }
      }
      // 부분 전개(각도 < 360)면 끝 각도에 단면(2D 메시를 돌린 것)을 덧대 안이 비어 보이지 않게 한다. 바깥(회전 접선 쪽)을 향하게 감는다
      if (total < 2.0 * 3.14159265358979323846 - 1e-9) {
        for (std::size_t i = 0; i < m.element_count(); ++i) {
          const ShapeInfo& info = shape_info(m.shape_at(i));
          if (info.dim != 2 || hidden_elem(i)) continue;
          const Id* n = m.nodes_at(i);
          const std::vector<int> corners = corner_positions(info.shape);
          std::vector<V3> p;
          std::vector<std::array<std::uint8_t, 3>> col;
          for (int k : corners) p.push_back(rotate_about(pos(n[k]), axis_pt, axis_dir, total)), col.push_back(color_of(n[k], base_color(i), i));
          V3 nrm = unit(cross(sub(p[1], p[0]), sub(p[2], p[0])));
          const V3 tangent = cross(axis_dir, sub(p[0], axis_pt));  // 회전 접선(+θ 쪽)
          const bool flip = dot(nrm, tangent) < 0;
          if (flip) nrm = mul(nrm, -1.0);
          const std::uint32_t id = pick_id({"element", m.element_ids()[i], 0});
          for (std::size_t k = 1; k + 1 < p.size(); ++k) {
            const std::size_t a_ = 0, b_ = flip ? k + 1 : k, c_ = flip ? k : k + 1;
            b.scene.triangles.push_back(vertex(p[a_], nrm, col[a_], id)), b.scene.triangles.push_back(vertex(p[b_], nrm, col[b_], id)), b.scene.triangles.push_back(vertex(p[c_], nrm, col[c_], id));
          }
          if (edges_on)
            for (std::size_t k = 0; k < p.size(); ++k) line(p[k], p[(k + 1) % p.size()]);
        }
      }
    }
    // 보 단면력 선도(RES-55): 보 노드의 단면력 값을 direction 쪽으로 scale 배 띄워 꺾은선으로 그린다
    if (!vs.beam_diagram.is_null() && !vs.result.is_null()) {
      ResultFile& file = results(a).get(vs.result["result"].get<Id>());
      ResultFrame& frame = file.frame(vs.result["frame"].get<int>());
      static const std::map<std::string, std::string> comp = {{"shear_1", "SXX"}, {"shear_2", "SYY"}, {"normal_force", "SZZ"},
                                                             {"torque", "SXY"}, {"moment_2", "SZX"}, {"moment_1", "SYZ"}};
      const std::string q = vs.beam_diagram.value("quantity", std::string("moment_1"));
      const ResultField& sf = file.field(frame, "STRESS");
      std::vector<double> v;
      {
        const auto it = std::find(sf.components.begin(), sf.components.end(), comp.at(q));
        if (it == sf.components.end()) throw Error("not_found", "STRESS 에 없는 성분입니다: " + comp.at(q), {{"param", "quantity"}});
        const std::size_t ci = static_cast<std::size_t>(it - sf.components.begin()), nc = sf.components.size();
        v.resize(sf.count);
        for (std::size_t i = 0; i < sf.count; ++i) v[i] = sf.data[i * nc + ci];
      }
      std::unordered_map<Id, double> at;
      for (std::size_t i = 0; i < sf.count; ++i) at[sf.ids[i]] = v[i];
      double vmax = 0;
      for (const auto& [n, x] : at) vmax = std::max(vmax, std::fabs(x));
      const double scale = has(vs.beam_diagram, "scale") ? vs.beam_diagram["scale"].get<double>() : (vmax > 0 ? 0.15 * b.bounds.radius() / vmax : 0.0);
      const V3 dir = has(vs.beam_diagram, "direction") ? unit(v3(vs.beam_diagram["direction"])) : V3{0, 0, 1};
      const std::array<std::uint8_t, 3> col{40, 120, 200};
      for (std::size_t i = 0; i < m.element_count(); ++i) {
        const ShapeInfo& info = shape_info(m.shape_at(i));
        if (info.dim != 1 || hidden_elem(i)) continue;
        const Id* n = m.nodes_at(i);
        const Id u = n[0], w = n[1];
        auto iu = at.find(u), iw = at.find(w);
        if (iu == at.end() || iw == at.end()) continue;
        const V3 pu = pos(u), pw = pos(w), ou = add(pu, mul(dir, iu->second * scale)), ow = add(pw, mul(dir, iw->second * scale));
        b.scene.lines.push_back(vertex(pu, {0, 0, 0}, col, 0)), b.scene.lines.push_back(vertex(ou, {0, 0, 0}, col, 0));
        b.scene.lines.push_back(vertex(pw, {0, 0, 0}, col, 0)), b.scene.lines.push_back(vertex(ow, {0, 0, 0}, col, 0));
        b.scene.lines.push_back(vertex(ou, {0, 0, 0}, col, 0)), b.scene.lines.push_back(vertex(ow, {0, 0, 0}, col, 0));
        b.bounds.add(ou), b.bounds.add(ow);
      }
    }
  }
  // 순환대칭 전개(RES-50): 메시로 그린 것(면·선·투명)을 축 둘레로 섹터 수만큼 복사한다(복사본의 픽 ID 는 원본과 같다)
  if (!vs.expand.is_null() && vs.expand.value("kind", std::string()) == "cyclic") {
    const int sectors = std::max(2, vs.expand.value("sectors", 2));
    const V3 pt = v3(vs.expand["point"]), ax = unit(v3(vs.expand["axis"]));
    const double step = vs.expand.value("angle", 360.0 / sectors) * 3.14159265358979323846 / 180.0;
    const std::size_t tri1 = b.scene.triangles.size(), line1 = b.scene.lines.size(), tr1 = b.scene.transparent.size();
    for (int k = 1; k < sectors; ++k) {
      const double ang = step * k;
      auto copy = [&](std::vector<RenderVertex>& list, std::size_t from, std::size_t to) {
        for (std::size_t i = from; i < to; ++i) {
          RenderVertex v = list[i];
          const V3 p = rotate_about({v.pos[0], v.pos[1], v.pos[2]}, pt, ax, ang), n = rotate_dir({v.normal[0], v.normal[1], v.normal[2]}, ax, ang);
          for (std::size_t c = 0; c < 3; ++c) v.pos[c] = static_cast<float>(p[c]), v.normal[c] = static_cast<float>(n[c]);
          list.push_back(v);
          b.bounds.add(p);
        }
      };
      copy(b.scene.triangles, mesh_tri0, tri1), copy(b.scene.lines, mesh_line0, line1), copy(b.scene.transparent, mesh_tr0, tr1);
    }
  }
  // 스케치 표시(RND-38, 1차): 억제되지 않은 스케치의 요소를 평면 위 꺾은선으로 그린다(sketch.tessellate). 참조 요소는 연하게. 평면 원점에 u(빨강)·v(초록) 축 표식.
  // 그리는 중 고무줄·구속 기호·치수 글자는 아직 없다. 숨긴 파트의 스케치는 그리지 않는다.
  if (a.commands().count("sketch.tessellate")) {
    const std::array<std::uint8_t, 3> ink{235, 140, 40}, faint{150, 120, 90}, ured{200, 60, 60}, vgreen{60, 170, 60};
    for (const Object* sk : a.model().by_kind("sketch")) {
      if (sk->suppressed || vs.hidden.count(sk->parent)) continue;
      Json t;
      try {
        t = a.commands().at("sketch.tessellate").fn(a, Json{{"id", sk->id}});
      } catch (const Error&) {
        continue;  // 평면이 깨진 스케치는 건너뛴다
      }
      auto v3 = [](const Json& q) { return V3{q[0].get<double>(), q[1].get<double>(), q[2].get<double>()}; };
      double extent = 1.0;
      for (const Json& e : t["entities"]) {
        const auto& color = e.value("reference", false) ? faint : ink;
        for (const Json& pl : e["polylines"]) {
          if (pl.size() == 1) {  // 점: 작은 십자
            const V3 c = v3(pl[0]);
            b.bounds.add(c);
            continue;
          }
          for (std::size_t i = 0; i + 1 < pl.size(); ++i) {
            const V3 p0 = v3(pl[i]), p1 = v3(pl[i + 1]);
            b.scene.lines.push_back(vertex(p0, {0, 0, 0}, color, 0)), b.scene.lines.push_back(vertex(p1, {0, 0, 0}, color, 0));
            b.bounds.add(p0), b.bounds.add(p1);
          }
        }
      }
      const Json& bo = t["bounds"];
      extent = std::max({extent, bo["u"][1].get<double>() - bo["u"][0].get<double>(), bo["v"][1].get<double>() - bo["v"][0].get<double>()});
      const V3 o = v3(t["frame"]["origin"]), u = v3(t["frame"]["u"]), v = v3(t["frame"]["v"]);
      const double len = 0.15 * extent;
      b.scene.lines.push_back(vertex(o, {0, 0, 0}, ured, 0)), b.scene.lines.push_back(vertex(add(o, mul(u, len)), {0, 0, 0}, ured, 0));
      b.scene.lines.push_back(vertex(o, {0, 0, 0}, vgreen, 0)), b.scene.lines.push_back(vertex(add(o, mul(v, len)), {0, 0, 0}, vgreen, 0));
      b.bounds.add(o);
    }
  }
  if (!vs.symbols.is_null() && m.node_count() > 0) add_symbols(a, vs, b);
  if (has(vs.overlay, "background")) {
    const Json& bg = vs.overlay["background"];
    for (std::size_t k = 0; k < 3 && k < bg.size(); ++k) b.scene.background[k] = static_cast<float>(bg[k].get<double>() / 255.0);
  }
  // 결과 표시 옵션(RES-07, RES-10, RND-30, RND-32): 미변형 윤곽(변형 표시 중일 때 원래 자리의 바깥 모서리를 연한 회색 선으로), 벡터 화살표
  if (!vs.result.is_null() && m.element_count() > 0 && vs.show != "geometry") {
    const std::array<std::uint8_t, 3> ghost{175, 175, 175};
    if (vs.result_options.value("undeformed", false) && vs.result.value("deform_scale", 0.0) != 0.0) {
      std::set<std::pair<Id, Id>> drawn;
      auto ghost_line = [&](Id u, Id v) {
        if (!drawn.insert({std::min(u, v), std::max(u, v)}).second) return;
        b.scene.lines.push_back(vertex(m.node(u), {0, 0, 0}, ghost, 0));
        b.scene.lines.push_back(vertex(m.node(v), {0, 0, 0}, ghost, 0));
      };
      for (std::size_t i = 0; i < m.element_count(); ++i) {
        const ShapeInfo& info = shape_info(m.shape_at(i));
        if (info.dim == 0 || vs.hidden.count(m.part_at(i))) continue;
        const Id* n = m.nodes_at(i);
        for (const auto& ed : shape_edges(info.shape))
          if (n[ed[0]] && n[ed[1]]) ghost_line(n[ed[0]], n[ed[1]]);
      }
    }
    if (has(vs.result_options, "vectors")) {
      const Json& vo = vs.result_options["vectors"];
      ResultFile& file = results(a).get(vs.result["result"].get<Id>());
      ResultFrame& frame = file.frame(vs.result["frame"].get<int>());
      const ResultField& f = file.field(frame, vo["field"].get<std::string>());
      if (f.components.size() >= 3) {
        const std::size_t nc = f.components.size();
        double vmax = 0;
        for (std::size_t i = 0; i < f.count; ++i)
          vmax = std::max(vmax, std::sqrt(f.data[i * nc] * f.data[i * nc] + f.data[i * nc + 1] * f.data[i * nc + 1] + f.data[i * nc + 2] * f.data[i * nc + 2]));
        const double scale = has(vo, "scale") ? vo["scale"].get<double>() : (vmax > 0 ? 0.15 * b.bounds.radius() / vmax : 0.0);
        const std::array<std::uint8_t, 3> arrow{200, 40, 160};
        const double shift_scale = vs.result.value("deform_scale", 0.0);
        const ResultField* disp = shift_scale != 0.0 ? &file.field(frame, "DISP") : nullptr;
        std::unordered_map<Id, V3> dshift;
        if (disp)
          for (std::size_t i = 0; i < disp->count; ++i)
            dshift[disp->ids[i]] = {shift_scale * disp->data[3 * i], shift_scale * disp->data[3 * i + 1], shift_scale * disp->data[3 * i + 2]};
        for (std::size_t i = 0; i < f.count; ++i) {
          const Id n = f.ids[i];
          if (!m.has_node(n)) continue;
          V3 p = m.node(n);
          if (auto it = dshift.find(n); it != dshift.end()) p = add(p, it->second);
          const V3 v{f.data[i * nc] * scale, f.data[i * nc + 1] * scale, f.data[i * nc + 2] * scale};
          if (norm(v) <= 0) continue;
          const V3 q = add(p, v);
          b.scene.lines.push_back(vertex(p, {0, 0, 0}, arrow, 0));
          b.scene.lines.push_back(vertex(q, {0, 0, 0}, arrow, 0));
          // 화살촉: 끝에서 뒤로 짧은 두 선(방향에 수직한 임의 축)
          const V3 d = unit(v);
          V3 side = cross(d, std::fabs(d[2]) < 0.9 ? V3{0, 0, 1} : V3{1, 0, 0});
          side = mul(unit(side), 0.25 * norm(v));
          const V3 back = mul(d, -0.25 * norm(v));
          for (double sgn : {1.0, -1.0}) {
            b.scene.lines.push_back(vertex(q, {0, 0, 0}, arrow, 0));
            b.scene.lines.push_back(vertex(add(add(q, back), mul(side, sgn)), {0, 0, 0}, arrow, 0));
          }
        }
      }
    }
  }
  return b;
}

// --- 행렬(열 우선). Vulkan 의 클립 공간: y 는 아래, 깊이 0~1.
V3 screen_right(const Camera& c) {
  const V3 f = unit(sub(c.target, c.eye));
  V3 s = cross(f, c.up);
  if (norm(s) < 1e-12) s = cross(f, std::fabs(f[2]) < 0.9 ? V3{0, 0, 1} : V3{0, 1, 0});
  return unit(s);
}
void look_at(const Camera& c, float out[16]) {
  const V3 f = unit(sub(c.target, c.eye)), s = screen_right(c);
  const V3 u = cross(s, f);
  const double m[16] = {s[0], u[0], -f[0], 0, s[1], u[1], -f[1], 0, s[2], u[2], -f[2], 0, -dot(s, c.eye), -dot(u, c.eye), dot(f, c.eye), 1};
  for (int i = 0; i < 16; ++i) out[i] = static_cast<float>(m[i]);
}
void projection(const Camera& c, double aspect, double znear, double zfar, float out[16]) {
  double m[16] = {0};
  if (c.ortho) {
    const double hh = 0.5 * c.height, hw = hh * aspect;
    m[0] = 1.0 / hw, m[5] = -1.0 / hh, m[10] = 1.0 / (znear - zfar), m[14] = znear / (znear - zfar), m[15] = 1.0;
  } else {
    const double f = 1.0 / std::tan(0.5 * c.fov * 3.14159265358979323846 / 180.0);
    m[0] = f / aspect, m[5] = -f, m[10] = zfar / (znear - zfar), m[11] = -1.0, m[14] = znear * zfar / (znear - zfar);
  }
  for (int i = 0; i < 16; ++i) out[i] = static_cast<float>(m[i]);
}
void multiply(const float a[16], const float b[16], float out[16]) {
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r) {
      float s = 0;
      for (int k = 0; k < 4; ++k) s += a[k * 4 + r] * b[c * 4 + k];
      out[c * 4 + r] = s;
    }
}

// 카메라를 방향은 그대로 두고 범위 전체가 들어오게 맞춘다(RND-10).
void fit(Camera& c, const Bounds& bounds) {
  const V3 dir = unit(sub(c.target, c.eye));
  const double r = bounds.radius();
  c.target = bounds.empty() ? V3{0, 0, 0} : bounds.center();
  c.pivot = c.target;
  const double fov = c.fov * 3.14159265358979323846 / 180.0;
  const double dist = c.ortho ? 3.0 * r : r / std::sin(0.5 * fov);
  c.eye = sub(c.target, mul(dir, dist));
  c.height = 2.0 * r * 1.05;
}

Json camera_json(const Camera& c) {
  return Json{{"eye", c.eye}, {"target", c.target}, {"up", c.up}, {"projection", c.ortho ? "orthographic" : "perspective"},
              {"fov", c.fov}, {"height", c.height}, {"pivot", c.pivot}};
}

Camera standard_camera(Camera cam, const Bounds& bounds, const std::string& name) {
  const auto target = render_detail::navigation_cube_target(name);
  cam.target = {0, 0, 0}, cam.eye = target.direction, cam.up = target.up;
  fit(cam, bounds);
  return cam;
}

using Quaternion = std::array<double, 4>;  // w,x,y,z
Quaternion camera_orientation(const Camera& c) {
  const V3 right = screen_right(c), back = unit(sub(c.eye, c.target)), up = cross(back, right);
  const double m[3][3] = {{right[0], up[0], back[0]}, {right[1], up[1], back[1]}, {right[2], up[2], back[2]}};
  Quaternion q{};
  const double trace = m[0][0] + m[1][1] + m[2][2];
  if (trace > 0) {
    const double s = 2 * std::sqrt(1 + trace);
    q = {s / 4, (m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s};
  } else {
    int i = 0;
    if (m[1][1] > m[i][i]) i = 1;
    if (m[2][2] > m[i][i]) i = 2;
    const int j = (i + 1) % 3, k = (i + 2) % 3;
    const double s = 2 * std::sqrt(1 + m[i][i] - m[j][j] - m[k][k]);
    q[0] = (m[k][j] - m[j][k]) / s, q[i + 1] = s / 4;
    q[j + 1] = (m[j][i] + m[i][j]) / s, q[k + 1] = (m[k][i] + m[i][k]) / s;
  }
  return q;
}
Camera interpolate_camera(const Camera& from, const Camera& to, double progress) {
  if (progress <= 0) return from;
  if (progress >= 1) return to;
  const double t = progress * progress * progress * (progress * (progress * 6 - 15) + 10);  // 출발·도착 속도와 가속도 0
  Quaternion a = camera_orientation(from), b = camera_orientation(to), q{};
  double cosine = 0;
  for (int i = 0; i < 4; ++i) cosine += a[i] * b[i];
  if (cosine < 0) { for (double& v : b) v = -v; cosine = -cosine; }  // 최단 회전 경로
  double wa = 1 - t, wb = t;
  if (cosine < 0.9995) {
    const double angle = std::acos(std::clamp(cosine, 0.0, 1.0));
    wa = std::sin((1 - t) * angle) / std::sin(angle), wb = std::sin(t * angle) / std::sin(angle);
  }
  double length = 0;
  for (int i = 0; i < 4; ++i) q[i] = wa * a[i] + wb * b[i], length += q[i] * q[i];
  for (double& v : q) v /= std::sqrt(length);
  const V3 vector{q[1], q[2], q[3]};
  auto rotate = [&](const V3& v) { const V3 twice = mul(cross(vector, v), 2); return add(v, add(mul(twice, q[0]), cross(vector, twice))); };
  auto blend = [&](const V3& x, const V3& y) { return add(mul(x, 1 - t), mul(y, t)); };
  auto positive_blend = [&](double x, double y) { return std::exp((1 - t) * std::log(x) + t * std::log(y)); };
  Camera cam = from;
  cam.target = blend(from.target, to.target), cam.pivot = blend(from.pivot, to.pivot);
  cam.eye = add(cam.target, mul(rotate({0, 0, 1}), positive_blend(norm(sub(from.eye, from.target)), norm(sub(to.eye, to.target)))));
  cam.up = rotate({0, 1, 0});
  cam.height = positive_blend(from.height, to.height), cam.fov = (1 - t) * from.fov + t * to.fov;
  return cam;
}

std::string scene_key(App& a) {
  const ViewState& vs = state(a);
  std::string hidden;
  for (Id h : vs.hidden) hidden += std::to_string(h) + ",";
  return a.digest().dump() + "|" + vs.mode + "|" + vs.show + "|" + vs.result.dump() + "|" + vs.symbols.dump() + "|" + vs.transparency.dump() +
         "|" + vs.legend_opts.dump() + "|" + vs.result_filter.dump() + "|" + vs.appearance.dump() + "|" + vs.mesh_options.dump() + "|" + std::to_string(vs.result_phase) +
         "|" + vs.overlay.dump() + "|" + vs.result_options.dump() + "|" + vs.expand.dump() + "|" + vs.beam_diagram.dump() +
         "|" + vs.section.dump() + "|" + vs.iso.dump() + "|" + vs.streamlines.dump() +
         "|" + hidden + "|" + vs.color_by + "|" + std::to_string(vs.tess_deflection) + "," + std::to_string(vs.tess_angle);
}
std::shared_ptr<Built> cached_scene(App& a) {
  ViewState& vs = state(a);
  const std::string key = scene_key(a);
  if (!vs.scene || vs.scene_key != key) {
    vs.scene = std::make_shared<Built>(build_scene(a));
    vs.scene_key = key;
  }
  return vs.scene;
}

std::array<double, 2> depth_range(const Camera& camera, const Built& built) {
  const double radius = built.bounds.radius();
  const V3 center = built.bounds.empty() ? camera.target : built.bounds.center();
  const double d = dot(sub(center, camera.eye), unit(sub(camera.target, camera.eye)));
  double znear = d - 1.01 * radius, zfar = d + 1.01 * radius;
  if (!camera.ortho) znear = std::max(znear, 1e-3 * std::max(zfar, radius));
  if (zfar <= znear) zfar = znear + std::max(radius, 1.0);
  return {znear, zfar};
}

// 카메라·장면에서 행렬을 만든다.
void matrices(ViewState& vs, const Built& built, int width, int height, float mvp[16], float view[16]) {
  if (!vs.camera_set) fit(vs.camera, built.bounds);
  const auto [znear, zfar] = depth_range(vs.camera, built);
  float proj[16];
  look_at(vs.camera, view);
  projection(vs.camera, static_cast<double>(width) / std::max(height, 1), znear, zfar, proj);
  multiply(proj, view, mvp);
  view[12] = view[13] = view[14] = 0.0f, view[15] = 1.0f;
}

// 클리핑 평면(RND-19)을 장면에 싣는다: 켜진 평면마다 (n, -n·p). 법선이 가리키는 쪽을 남긴다
void apply_clips(const ViewState& vs, RenderScene& scene) {
  scene.clip_planes.clear();
  for (const Json& c : vs.clip) {
    if (!c.value("enabled", true)) continue;
    const V3 n = unit(v3(c["normal"])), p = v3(c["point"]);
    scene.clip_planes.push_back({static_cast<float>(n[0]), static_cast<float>(n[1]), static_cast<float>(n[2]), static_cast<float>(-dot(n, p))});
    if (static_cast<int>(scene.clip_planes.size()) >= kMaxClipPlanes) break;
  }
}

V3 vertex_position(const Built& built, Id node) {
  auto it = built.node_pos.find(node);
  return it == built.node_pos.end() ? V3{std::nan(""), 0, 0} : it->second;
}

// 강조 표시(RND-27, RND-49): 강조 대상의 삼각형을 밝은 색으로 위에 덧그린다.
void add_overlay(ViewState& vs, const Built& built, RenderScene& scene) {
  scene.overlay.clear();
  std::set<std::uint32_t> ids;
  if (vs.hover) ids.insert(vs.hover);
  for (const PickRecord& h : vs.highlighted)
    for (std::size_t i = 0; i < built.picks.size(); ++i)
      if (built.picks[i].kind == h.kind && built.picks[i].a == h.a && built.picks[i].b == h.b) ids.insert(static_cast<std::uint32_t>(i + 1));
  // 적용 대상의 노드 표식(WT-24): 노드마다 작은 팔면체
  if (!vs.target_nodes.empty()) {
    const double r = std::max(1e-9, 0.008 * built.bounds.radius());
    const std::array<std::uint8_t, 3> c{255, 150, 0};
    for (Id n : vs.target_nodes) {
      const V3 p = vertex_position(built, n);
      if (std::isnan(p[0])) continue;
      const V3 axes[3] = {{r, 0, 0}, {0, r, 0}, {0, 0, r}};
      for (int sx : {1, -1})
        for (int sy : {1, -1})
          for (int sz : {1, -1}) {
            scene.overlay.push_back(vertex(add(p, mul(axes[0], sx)), {0, 0, 0}, c, 0));
            scene.overlay.push_back(vertex(add(p, mul(axes[1], sy)), {0, 0, 0}, c, 0));
            scene.overlay.push_back(vertex(add(p, mul(axes[2], sz)), {0, 0, 0}, c, 0));
          }
    }
  }
  if (ids.empty()) return;
  for (std::size_t k = 0; k + 2 < built.scene.triangles.size(); k += 3) {
    const std::uint32_t id = built.scene.triangles[k].id;
    if (!ids.count(id)) continue;
    const bool hovering = id == vs.hover;
    for (std::size_t c = 0; c < 3; ++c) {
      RenderVertex v = built.scene.triangles[k + c];
      v.normal[0] = v.normal[1] = v.normal[2] = 0.0f;  // 조명 없이, 카메라 쪽으로 당겨 그린다
      v.color[0] = 255, v.color[1] = hovering ? 220 : 140, v.color[2] = hovering ? 80 : 0;
      scene.overlay.push_back(v);
    }
  }
}

// 고른 구속(WT-22 의 선택 객체 가운데 구속 또는 구속 셋). 비어 있으면 모두 보통으로, 있으면 고른 것만 진하게 그린다
std::set<Id> focused_bcs(const ViewState& vs, const Built& built) {
  std::set<Id> out;
  for (const BcMark& mark : built.bc_marks)
    if (std::any_of(vs.selected_objects.begin(), vs.selected_objects.end(), [&](Id s) { return s == mark.id || s == mark.set; })) out.insert(mark.id);
  return out;
}

// 구속 표시(BC-13): 적용 영역을 구속의 색으로 옅게 칠하고, 고른 구속에는 자유도마다 축 기호를 화면 고정 크기로 그린다
// (병진 = 노드에 끝이 닿는 원뿔, 회전 = 그 축에 꿴 원판). 이름표는 add_hud 가 그린다.
void add_bc_marks(const ViewState& vs, const Built& built, RenderScene& scene, int height) {
  if (built.bc_marks.empty()) return;
  const Camera& cam = vs.camera;
  const V3 forward = unit(sub(cam.target, cam.eye));
  auto world_per_pixel = [&](const V3& p) {
    const double visible = cam.ortho ? cam.height : 2.0 * std::max(dot(sub(p, cam.eye), forward), 1e-9) * std::tan(0.5 * cam.fov * kPi / 180.0);
    return visible / std::max(height, 1);
  };
  // 조명 없이 그리는 오버레이라 면의 기울기에 따른 밝기를 여기서 넣는다
  auto solid = [&](const V3& p0, const V3& p1, const V3& p2, const std::array<std::uint8_t, 3>& c) {
    const double shade = 0.5 + 0.5 * std::fabs(dot(unit(cross(sub(p1, p0), sub(p2, p0))), forward));
    const std::array<std::uint8_t, 3> lit{static_cast<std::uint8_t>(c[0] * shade), static_cast<std::uint8_t>(c[1] * shade), static_cast<std::uint8_t>(c[2] * shade)};
    for (const V3& p : {p0, p1, p2}) scene.overlay.push_back(vertex(p, {0, 0, 0}, lit, 0));
  };
  const int segments = 12;
  auto ring = [&](const V3& center, const V3& axis, double radius) {
    const V3 u = unit(cross(axis, std::fabs(axis[2]) < 0.9 ? V3{0, 0, 1} : V3{1, 0, 0})), v = cross(axis, u);
    std::vector<V3> out;
    for (int i = 0; i <= segments; ++i) {
      const double ang = 2.0 * kPi * i / segments;
      out.push_back(add(center, add(mul(u, radius * std::cos(ang)), mul(v, radius * std::sin(ang)))));
    }
    return out;
  };
  const std::set<Id> focus = focused_bcs(vs, built);
  for (const BcMark& mark : built.bc_marks) {
    const bool focused = focus.count(mark.id) > 0, dim = !focus.empty() && !focused;
    const std::uint8_t alpha = focused ? 190 : dim ? 55 : 135;
    for (std::size_t k : mark.triangles)
      for (std::size_t c = 0; c < 3; ++c) {
        RenderVertex v = built.scene.triangles[k + c];
        v.normal[0] = v.normal[1] = v.normal[2] = 0.0f;  // 조명 없이, 면보다 카메라 쪽으로 당겨 그린다
        v.color[0] = mark.color[0], v.color[1] = mark.color[1], v.color[2] = mark.color[2], v.color[3] = alpha;
        v.id = 0;
        scene.transparent.push_back(v);
      }
    if (!dim)
      for (const V3& p : mark.nodes) {  // 면이 없는 대상: 노드마다 작은 팔면체
        const double r = 4.0 * world_per_pixel(p);
        const V3 axes[3] = {{r, 0, 0}, {0, r, 0}, {0, 0, r}};
        for (int sx : {1, -1})
          for (int sy : {1, -1})
            for (int sz : {1, -1}) solid(add(p, mul(axes[0], sx)), add(p, mul(axes[1], sy)), add(p, mul(axes[2], sz)), mark.color);
      }
    if (!focused) continue;
    for (const V3& p : mark.points) {
      const double len = 26.0 * world_per_pixel(p);
      if (mark.dofs.size() == 6) {  // 완전 고정: 축마다 그리지 않고 노드에 상자 하나
        const double r = 0.3 * len;
        auto corner = [&](int i) { return add(p, V3{(i & 1 ? r : -r), (i & 2 ? r : -r), (i & 4 ? r : -r)}); };
        static const int quads[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
        for (const auto& f : quads) solid(corner(f[0]), corner(f[1]), corner(f[2]), mark.color), solid(corner(f[0]), corner(f[2]), corner(f[3]), mark.color);
        continue;
      }
      for (int dof : mark.dofs) {
        // 기호는 축의 두 쪽 가운데 카메라를 향한 쪽에 둔다(입체 속에 묻히지 않게)
        const std::size_t k = static_cast<std::size_t>((dof - 1) % 3);
        V3 axis{0, 0, 0};
        axis[k] = forward[k] > 0 ? -1.0 : 1.0;
        if (dof <= 3) {
          const V3 base = add(p, mul(axis, len));
          const std::vector<V3> rim = ring(base, axis, 0.32 * len);
          for (int i = 0; i < segments; ++i) solid(p, rim[static_cast<std::size_t>(i)], rim[static_cast<std::size_t>(i + 1)], mark.color), solid(base, rim[static_cast<std::size_t>(i)], rim[static_cast<std::size_t>(i + 1)], mark.color);
        } else {
          const bool on_cone = std::find(mark.dofs.begin(), mark.dofs.end(), dof - 3) != mark.dofs.end();
          const V3 c0 = add(p, mul(axis, 1.0 * len)), c1 = add(p, mul(axis, 1.12 * len));
          const std::vector<V3> r0 = ring(c0, axis, 0.45 * len), r1 = ring(c1, axis, 0.45 * len);
          for (int i = 0; i < segments; ++i) {
            const std::size_t j = static_cast<std::size_t>(i);
            solid(c0, r0[j], r0[j + 1], mark.color), solid(c1, r1[j], r1[j + 1], mark.color);
            solid(r0[j], r0[j + 1], r1[j + 1], mark.color), solid(r0[j], r1[j + 1], r1[j], mark.color);
          }
          if (!on_cone) {  // 같은 축의 병진 구속이 없으면 원판을 노드에 잇는 대
            scene.lines.push_back(vertex(p, {0, 0, 0}, mark.color, 0)), scene.lines.push_back(vertex(c0, {0, 0, 0}, mark.color, 0));
          }
        }
      }
    }
  }
}

// ------------------------------------------------------------ 화면 고정 요소(RND-35~37): 좌표축·글자·범례
// 글자는 5×7 점 글꼴을 사각형으로 그린다(외부 글꼴 라이브러리 없이). 대문자·숫자·몇 가지 기호만 있다.
using render_detail::Hud;
render_detail::CubeOrientation cube_orientation(const Camera& camera) {
  const V3 right = screen_right(camera), back = unit(sub(camera.eye, camera.target));
  return {right, cross(back, right), back};
}
bool cube_visible(const ViewState& vs) { return vs.hud.value("triad", true) && vs.hud.value("navigation_cube", false); }
Json cube_pick(const ViewState& vs, double x, double y, int width, int height) {
  return cube_visible(vs) ? render_detail::pick_navigation_cube(cube_orientation(vs.camera), x, y, width, height) : Json{{"hit", false}};
}

std::string format_value(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, std::fabs(v) >= 1e4 || (std::fabs(v) < 1e-2 && v != 0.0) ? "%.3e" : "%.4g", v);
  return buf;
}

// 좌표축(RND-36): 왼쪽 아래에 카메라를 따라 도는 x·y·z 축. 글자·범례(RND-35, RND-37)도 여기서 그린다.
void add_hud(App& a, const ViewState& vs, const Built& built, RenderScene& scene, int width, int height, const float view[16], const float mvp[16]) {
  scene.hud_triangles.clear(), scene.hud_lines.clear();
  if (width < 1 || height < 1) return;
  Hud h{scene, width, height};
  const bool dark_background = 0.2126f * scene.background[0] + 0.7152f * scene.background[1] + 0.0722f * scene.background[2] < 0.45f;
  const std::array<std::uint8_t, 3> text_color = dark_background
      ? std::array<std::uint8_t, 3>{220, 220, 220} : std::array<std::uint8_t, 3>{40, 40, 40};
  // 노드·요소 라벨(RND-36): 3D 위치를 화면에 투영해 글자를 둔다(화면 밖·카메라 뒤는 뺀다)
  if (vs.labels.is_object() && !vs.labels.empty()) {
    const Mesh& m = a.mesh();
    auto project = [&](const V3& p, double& px, double& py) {
      double c[4] = {0, 0, 0, 0};
      for (int r = 0; r < 4; ++r) c[r] = mvp[r] * p[0] + mvp[4 + r] * p[1] + mvp[8 + r] * p[2] + mvp[12 + r];
      if (c[3] <= 1e-12) return false;
      px = (c[0] / c[3] * 0.5 + 0.5) * width, py = (c[1] / c[3] * 0.5 + 0.5) * height;
      return px >= -50 && px <= width + 50 && py >= -20 && py <= height + 20;
    };
    const std::array<std::uint8_t, 3> node_color = dark_background
        ? std::array<std::uint8_t, 3>{117, 190, 255} : std::array<std::uint8_t, 3>{20, 60, 160};
    const std::array<std::uint8_t, 3> elem_color = dark_background
        ? std::array<std::uint8_t, 3>{255, 150, 140} : std::array<std::uint8_t, 3>{140, 40, 40};
    const bool all_nodes = vs.labels.value("all_nodes", false), all_elements = vs.labels.value("all_elements", false);
    const bool have_nodes = all_nodes || !vs.labels.value("nodes", Json::array()).empty();
    const bool have_elements = all_elements || !vs.labels.value("elements", Json::array()).empty();
    // 함께 켰을 때도 두 종류가 모두 보이도록 전체 5000개 한도를 나눈다.
    std::size_t budget = have_elements ? 2500 : 5000;
    auto draw_node = [&](Id id) {
      double px, py;
      if (!budget || !m.has_node(id) || !project(m.node(id), px, py)) return;
      h.text(px + 3.0, py - 7.0, std::to_string(id), node_color, 1.5), --budget;
    };
    if (all_nodes) {
      for (Id id : m.node_ids()) { if (!budget) break; draw_node(id); }
    } else {
      for (const Json& id : vs.labels.value("nodes", Json::array())) { if (!budget) break; draw_node(id.get<Id>()); }
    }
    budget = have_nodes ? 2500 : 5000;
    auto draw_element = [&](Id id) {
      if (!budget || !m.has_element(id)) return;
      V3 c{0, 0, 0};
      std::size_t cnt = 0;
      for (Id n : m.element(id).nodes)
        if (n) c = add(c, m.node(n)), ++cnt;
      double px, py;
      if (!cnt || !project(mul(c, 1.0 / static_cast<double>(cnt)), px, py)) return;
      const std::string s = std::to_string(id);
      h.text(px - 0.5 * Hud::text_width(s, 1.5), py - 5.0, s, elem_color, 1.5), --budget;
    };
    if (all_elements) {
      for (Id id : m.element_ids()) { if (!budget) break; draw_element(id); }
    } else {
      for (const Json& id : vs.labels.value("elements", Json::array())) { if (!budget) break; draw_element(id.get<Id>()); }
    }
  }
  // 구속 이름표(BC-13): 구속마다 하나, 대상 가운데의 노드에서 지시선을 뽑아 색 바탕에 흰 글자로. 겹치면 아래로 민다
  if (!built.bc_marks.empty()) {
    const std::set<Id> focus = focused_bcs(vs, built);
    std::vector<std::array<double, 4>> placed;
    for (const BcMark& mark : built.bc_marks) {
      const V3& p = mark.anchor;
      double c[4] = {0, 0, 0, 0};
      for (int r = 0; r < 4; ++r) c[r] = mvp[r] * p[0] + mvp[4 + r] * p[1] + mvp[8 + r] * p[2] + mvp[12 + r];
      if (c[3] <= 1e-12) continue;
      const double px = (c[0] / c[3] * 0.5 + 0.5) * width, py = (c[1] / c[3] * 0.5 + 0.5) * height;
      if (px < 0 || px > width || py < 0 || py > height) continue;
      const bool focused = focus.count(mark.id) > 0, dim = !focus.empty() && !focused;
      const double scale = dim ? 1.5 : 2.0, pad = 4.0;
      const double w = Hud::text_width(mark.text, scale) - scale + 2 * pad, tag_h = 7 * scale + 2 * pad;
      double x = std::clamp(px + 16.0, 2.0, std::max(2.0, width - w - 2.0)), y = std::clamp(py - 16.0 - tag_h, 2.0, std::max(2.0, height - tag_h - 2.0));
      for (int tries = 0; tries < 12; ++tries) {
        const bool overlap = std::any_of(placed.begin(), placed.end(), [&](const std::array<double, 4>& q) { return x < q[2] && q[0] < x + w && y < q[3] && q[1] < y + tag_h; });
        if (!overlap) break;
        y += tag_h + 3.0;
      }
      placed.push_back({x, y, x + w, y + tag_h});
      std::array<std::uint8_t, 3> color = mark.color;
      if (dim)  // 고르지 않은 구속은 바탕색 쪽으로 흐리게
        for (std::size_t k = 0; k < 3; ++k) color[k] = static_cast<std::uint8_t>(0.45 * color[k] + 0.55 * 255.0 * scene.background[k]);
      h.line(px, py, x, y + tag_h, color);
      h.rect(px - 2.5, py - 2.5, px + 2.5, py + 2.5, color);
      if (focused) h.rect(x - 2.0, y - 2.0, x + w + 2.0, y + tag_h + 2.0, text_color, 0.03f);  // 고른 구속은 테두리
      h.rect(x, y, x + w, y + tag_h, color, 0.02f);
      h.text(x + pad, y + pad, mark.text, {255, 255, 255}, scale);
    }
  }
  if (cube_visible(vs)) {
    render_detail::draw_navigation_cube(h, cube_orientation(vs.camera), vs.cube_hover);
  } else if (vs.hud.value("triad", true)) {
    const double len = 42.0, ox = 60.0, oy = height - 60.0;
    const std::array<std::array<std::uint8_t, 3>, 3> colors = dark_background
        ? std::array<std::array<std::uint8_t, 3>, 3>{{{245, 100, 100}, {110, 205, 110}, {100, 160, 255}}}
        : std::array<std::array<std::uint8_t, 3>, 3>{{{200, 40, 40}, {40, 160, 40}, {40, 80, 220}}};
    const char* names[3] = {"X", "Y", "Z"};
    // 뷰 행렬(열 우선)의 3×3 회전: 세계의 축 k 는 (view[k*4+0], view[k*4+1], view[k*4+2]) 로 간다. 화면의 y 는 아래가 양.
    std::array<std::pair<double, int>, 3> order;
    for (int k = 0; k < 3; ++k) order[static_cast<std::size_t>(k)] = {view[k * 4 + 2], k};
    std::sort(order.begin(), order.end());  // 멀리 있는(앞으로 향한) 축부터 그린다
    for (const auto& [depth, k] : order) {
      const double dx = view[k * 4 + 0] * len, dy = -view[k * 4 + 1] * len;
      const auto& c = colors[static_cast<std::size_t>(k)];
      h.line(ox, oy, ox + dx, oy + dy, c);
      // 화살촉: 축 방향의 작은 삼각형
      const double l = std::sqrt(dx * dx + dy * dy);
      if (l > 1.0) {
        const double ux = dx / l, uy = dy / l, px = -uy, py = ux, hs = 8.0, hw = 3.5;
        auto& t = scene.hud_triangles;
        t.push_back(h.v(ox + dx, oy + dy, c));
        t.push_back(h.v(ox + dx - ux * hs + px * hw, oy + dy - uy * hs + py * hw, c));
        t.push_back(h.v(ox + dx - ux * hs - px * hw, oy + dy - uy * hs - py * hw, c));
      }
      h.text(ox + dx * 1.25 - 5.0 + (dx >= 0 ? 4.0 : -4.0), oy + dy * 1.25 - 7.0, names[k], c);
    }
  }
  if (vs.hud.value("legend", true) && !built.legend.is_null()) {
    // 범례: 오른쪽에 세로 색띠, 눈금 5개, 제목(필드와 성분)
    const double bar_w = 18.0, bar_h = std::max(80.0, 0.45 * height), x1 = width - 110.0, x0 = x1 - bar_w;
    const double y0 = 0.5 * height - 0.5 * bar_h, y1 = y0 + bar_h;
    const int steps = 32;
    for (int i = 0; i < steps; ++i) {
      const double t0 = static_cast<double>(i) / steps, t1 = static_cast<double>(i + 1) / steps;
      h.rect(x0, y1 - t1 * bar_h, x1, y1 - t0 * bar_h, colormap(0.5 * (t0 + t1), built.legend.value("colormap", std::string("rainbow")), built.legend.value("levels", 0)));
    }
    h.line(x0, y0, x1, y0, text_color), h.line(x0, y1, x1, y1, text_color), h.line(x0, y0, x0, y1, text_color), h.line(x1, y0, x1, y1, text_color);
    const double vmin = built.legend.value("min", 0.0), vmax = built.legend.value("max", 0.0);
    for (int i = 0; i <= 4; ++i) {
      const double t = i / 4.0, y = y1 - t * bar_h;
      h.line(x1, y, x1 + 5.0, y, text_color);
      h.text(x1 + 8.0, y - 7.0, format_value(vmin + t * (vmax - vmin)), text_color);
    }
    // 제목·프레임: 색띠 위아래에, 오른쪽 가장자리를 넘지 않게
    const std::string title = built.legend.value("field", std::string()) + " " + built.legend.value("component", std::string());
    h.text(std::min(x0 - 4.0, width - 6.0 - Hud::text_width(title)), y0 - 26.0, title, text_color);
    if (built.legend.contains("frame")) {
      const std::string frame = "FRAME " + std::to_string(built.legend["frame"].get<int>());
      h.text(std::min(x0 - 4.0, width - 6.0 - Hud::text_width(frame)), y1 + 12.0, frame, text_color);
    }
  }
  if (vs.overlay.value("ruler", false) && !built.bounds.empty()) {
    // 눈금자: 장면 중심에서 화면 가로 방향 단위 길이가 몇 픽셀인지 재어 1·2·5×10^n 가운데 100~250 픽셀이 되는 길이를 고른다
    const V3 c = built.bounds.center();
    auto project = [&](const V3& p, double& px, double& py) {
      double q[4] = {0, 0, 0, 0};
      for (int r = 0; r < 4; ++r) q[r] = mvp[r] * p[0] + mvp[4 + r] * p[1] + mvp[8 + r] * p[2] + mvp[12 + r];
      if (q[3] <= 1e-12) return false;
      px = (q[0] / q[3] * 0.5 + 0.5) * width, py = (q[1] / q[3] * 0.5 + 0.5) * height;
      return true;
    };
    // 화면 x 방향의 세계 벡터: 뷰 행렬의 첫 행(열 우선 → view[0], view[4], view[8])
    const V3 sx{view[0], view[4], view[8]};
    double x0, y0, x1, y1;
    if (norm(sx) > 0 && project(c, x0, y0) && project(add(c, unit(sx)), x1, y1)) {
      const double px_per_unit = std::hypot(x1 - x0, y1 - y0);
      if (px_per_unit > 1e-9) {
        double len = std::pow(10.0, std::floor(std::log10(150.0 / px_per_unit)));
        for (double mult : {1.0, 2.0, 5.0, 10.0})
          if (len * mult * px_per_unit >= 100.0) { len *= mult; break; }
        const double w = len * px_per_unit, bx = 0.5 * width - 0.5 * w, by = height - 24.0;
        h.line(bx, by, bx + w, by, text_color), h.line(bx, by - 6.0, bx, by + 6.0, text_color), h.line(bx + w, by - 6.0, bx + w, by + 6.0, text_color);
        const std::string label = format_value(len);
        h.text(bx + 0.5 * w - 0.5 * Hud::text_width(label), by - 20.0, label, text_color);
      }
    }
  }
  for (const Json& label : vs.hud.value("labels", Json::array())) {  // 사용자가 둔 글자(픽셀 위치)
    if (!label.contains("text")) continue;
    h.text(label.value("x", 10.0), label.value("y", 10.0), label["text"].get<std::string>(), text_color, label.value("scale", 2.0));
  }
}

Json pick_json(const ViewState& vs, std::uint32_t id) {
  if (id == 0 || id > vs.picks.size()) return Json{{"hit", false}};
  const PickRecord& rec = vs.picks[id - 1];
  if (!vs.pick_filter.empty() && !vs.pick_filter.count(rec.kind)) return Json{{"hit", false}, {"filtered", rec.kind}};  // 선택 필터(RND-26)
  Json out{{"hit", true}, {"kind", rec.kind}};
  if (rec.kind == "face") out["part"] = rec.a, out["index"] = rec.b;
  else if (rec.kind == "element_face") out["element"] = rec.a, out["face"] = rec.b;
  else out["element"] = rec.a;
  return out;
}

struct Rendered {
  RenderImage image;
  Built built;
};

// 투명 면 정렬(RND-18, quality.transparency = sorted): 삼각형을 카메라에서 먼 것부터 그리도록 뷰 공간 깊이로 정렬한다
void sort_transparent(RenderScene& scene, const float view[16]) {
  auto& t = scene.transparent;
  const std::size_t n = t.size() / 3;
  std::vector<std::pair<float, std::size_t>> order(n);
  for (std::size_t i = 0; i < n; ++i) {
    float z = 0;
    for (std::size_t c = 0; c < 3; ++c) {
      const RenderVertex& v = t[3 * i + c];
      z += view[2] * v.pos[0] + view[6] * v.pos[1] + view[10] * v.pos[2] + view[14];
    }
    order[i] = {z, i};  // 뷰 공간 z 는 카메라 앞이 음수: 작은(먼) 것부터
  }
  std::sort(order.begin(), order.end());
  std::vector<RenderVertex> sorted;
  sorted.reserve(t.size());
  for (const auto& [z, i] : order)
    for (std::size_t c = 0; c < 3; ++c) sorted.push_back(t[3 * i + c]);
  t.swap(sorted);
}

Rendered render_one(App& a, int width, int height, const Json* background);

// 뷰포트 분할(RND-03): 칸마다 표준 뷰 또는 저장한 뷰를 그려 한 이미지에 붙인다(1픽셀 회색 경계). ID 는 칸의 것을 그대로 둔다
Rendered render(App& a, int width, int height, const Json* background = nullptr) {
  ViewState& vs = state(a);
  if (vs.layout.is_null()) return render_one(a, width, height, background);
  const int rows = vs.layout.value("rows", 1), cols = vs.layout.value("cols", 1);
  const Json cells = vs.layout.value("cells", Json::array());
  Rendered out;
  out.image.width = width, out.image.height = height;
  out.image.rgba.assign(static_cast<std::size_t>(width) * height * 4, 128);
  out.image.ids.assign(static_cast<std::size_t>(width) * height, 0);
  const Camera saved_cam = vs.camera;
  const bool saved_set = vs.camera_set;
  const Json saved_views = vs.saved_views;
  for (int r = 0; r < rows; ++r)
    for (int c = 0; c < cols; ++c) {
      const int x0 = c * width / cols, x1 = (c + 1) * width / cols - (c + 1 < cols ? 1 : 0);
      const int y0 = r * height / rows, y1 = (r + 1) * height / rows - (r + 1 < rows ? 1 : 0);
      const int cw = x1 - x0, ch = y1 - y0;
      if (cw < 1 || ch < 1) continue;
      const std::size_t k = static_cast<std::size_t>(r * cols + c);
      const std::string view = k < cells.size() ? cells[k].value("view", std::string()) : std::string();
      if (!view.empty()) {
        if (vs.saved_views.contains(view)) a.commands().at("view.restore").fn(a, Json{{"name", view}});
        else a.commands().at("view.standard").fn(a, Json{{"name", view}});
      } else {
        vs.camera = saved_cam, vs.camera_set = saved_set;
      }
      const Rendered cell = render_one(a, cw, ch, background);
      for (int y = 0; y < ch; ++y) {
        std::memcpy(&out.image.rgba[(static_cast<std::size_t>(y0 + y) * width + x0) * 4], &cell.image.rgba[static_cast<std::size_t>(y) * cw * 4], static_cast<std::size_t>(cw) * 4);
        std::memcpy(&out.image.ids[static_cast<std::size_t>(y0 + y) * width + x0], &cell.image.ids[static_cast<std::size_t>(y) * cw], static_cast<std::size_t>(cw) * 4);
      }
      if (k == 0) out.built = cell.built;
    }
  vs.camera = saved_cam, vs.camera_set = saved_set, vs.saved_views = saved_views;
  return out;
}

Rendered render_one(App& a, int width, int height, const Json* background) {
  ViewState& vs = state(a);
  Rendered r;
  r.built = *cached_scene(a);
  if (background)
    for (std::size_t k = 0; k < 4; ++k) r.built.scene.background[k] = (*background)[k].get<float>();
  // 안티에일리어싱(RND-18): 두 배 크기로 그려 2×2 평균으로 줄인다.
  const int ss = vs.quality.value("antialiasing", std::string("none")) == "ssaa2" ? 2 : 1;
  const int rw = width * ss, rh = height * ss;
  r.built.scene.pixel_scale = ss;
  float view[16], mvp[16];
  matrices(vs, r.built, rw, rh, mvp, view);  // 깊이 범위도 장면에 맞춘다(RND-11)
  add_overlay(vs, r.built, r.built.scene);
  add_bc_marks(vs, r.built, r.built.scene, height);
  apply_clips(vs, r.built.scene);
  if (vs.quality.value("transparency", std::string("unsorted")) == "sorted") sort_transparent(r.built.scene, view);
  add_hud(a, vs, r.built, r.built.scene, width, height, view, mvp);
  r.image = renderer(a).render(r.built.scene, mvp, view, rw, rh);
  if (ss > 1) {
    RenderImage small;
    small.width = width, small.height = height;
    small.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    small.ids.resize(static_cast<std::size_t>(width) * height);
    for (int y = 0; y < height; ++y)
      for (int x = 0; x < width; ++x) {
        for (int c = 0; c < 4; ++c) {
          int sum = 0;
          for (int dy = 0; dy < ss; ++dy)
            for (int dx = 0; dx < ss; ++dx) sum += r.image.rgba[(static_cast<std::size_t>(y * ss + dy) * rw + (x * ss + dx)) * 4 + c];
          small.rgba[(static_cast<std::size_t>(y) * width + x) * 4 + c] = static_cast<std::uint8_t>((sum + ss * ss / 2) / (ss * ss));
        }
        small.ids[static_cast<std::size_t>(y) * width + x] = r.image.ids[static_cast<std::size_t>(y * ss) * rw + x * ss];  // ID 는 표본 하나
      }
    r.image = std::move(small);
  }
  vs.picks = r.built.picks;
  vs.legend = r.built.legend;
  return r;
}

// --- PNG(압축하지 않는 저장 블록으로 쓴다: 외부 라이브러리 없이)
std::uint32_t crc32(const std::uint8_t* data, std::size_t n, std::uint32_t crc = 0) {
  static std::uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    ready = true;
  }
  crc = ~crc;
  for (std::size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}
void write_png(const std::string& path, const RenderImage& img) {
  std::vector<std::uint8_t> raw;  // 줄마다 필터 바이트(0) + RGBA
  raw.reserve(static_cast<std::size_t>(img.height) * (static_cast<std::size_t>(img.width) * 4 + 1));
  for (int y = 0; y < img.height; ++y) {
    raw.push_back(0);
    const std::uint8_t* row = img.rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(img.width) * 4;
    raw.insert(raw.end(), row, row + static_cast<std::size_t>(img.width) * 4);
  }
  std::vector<std::uint8_t> z = {0x78, 0x01};
  std::uint32_t s1 = 1, s2 = 0;
  for (std::size_t pos = 0; pos < raw.size(); pos += 65535) {
    const std::size_t n = std::min<std::size_t>(65535, raw.size() - pos);
    z.push_back(pos + n >= raw.size() ? 1 : 0);  // 마지막 블록 표시
    z.push_back(static_cast<std::uint8_t>(n & 0xFF)), z.push_back(static_cast<std::uint8_t>(n >> 8));
    z.push_back(static_cast<std::uint8_t>(~n & 0xFF)), z.push_back(static_cast<std::uint8_t>((~n >> 8) & 0xFF));
    for (std::size_t i = 0; i < n; ++i) {
      z.push_back(raw[pos + i]);
      s1 = (s1 + raw[pos + i]) % 65521, s2 = (s2 + s1) % 65521;
    }
  }
  const std::uint32_t adler = (s2 << 16) | s1;
  for (int k = 3; k >= 0; --k) z.push_back(static_cast<std::uint8_t>(adler >> (8 * k)));

  std::ofstream f(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
  if (!f) throw Error("io_error", "파일을 쓸 수 없습니다: " + path, {{"path", path}});
  auto be32 = [](std::uint32_t v) { return std::array<std::uint8_t, 4>{std::uint8_t(v >> 24), std::uint8_t(v >> 16), std::uint8_t(v >> 8), std::uint8_t(v)}; };
  auto chunk = [&](const char* type, const std::vector<std::uint8_t>& data) {
    const auto len = be32(static_cast<std::uint32_t>(data.size()));
    f.write(reinterpret_cast<const char*>(len.data()), 4);
    std::vector<std::uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    f.write(reinterpret_cast<const char*>(body.data()), static_cast<std::streamsize>(body.size()));
    const auto crc = be32(crc32(body.data(), body.size()));
    f.write(reinterpret_cast<const char*>(crc.data()), 4);
  };
  const std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  f.write(reinterpret_cast<const char*>(sig), 8);
  std::vector<std::uint8_t> ihdr;
  for (std::uint8_t x : be32(static_cast<std::uint32_t>(img.width))) ihdr.push_back(x);
  for (std::uint8_t x : be32(static_cast<std::uint32_t>(img.height))) ihdr.push_back(x);
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8비트 RGBA
  chunk("IHDR", ihdr);
  chunk("IDAT", z);
  chunk("IEND", {});
  if (!f) throw Error("io_error", "파일을 쓰다가 실패했습니다: " + path, {{"path", path}});
}

CommandSpec base(const std::string& name, char kind, const std::string& desc, const std::string& features) {
  CommandSpec c;
  c.name = name, c.kind = kind, c.undoable = false, c.desc = desc, c.features = features;
  return c;
}

void size_of(const App& a, const std::string& command, const Json& p, int& w, int& h) {
  for (const FieldSpec& f : a.commands().at(command).params)
    if ((f.name == "width" || f.name == "height") && has(p, f.name.c_str())) check_value(f, p[f.name], nullptr);
  w = p.value("width", 800), h = p.value("height", 600);
}
F width_param() { return F("width", "integer", "이미지 너비(픽셀, 기본 800)").ge(1).le(16384); }
F height_param() { return F("height", "integer", "이미지 높이(픽셀, 기본 600)").ge(1).le(16384); }

}  // namespace

RenderImage render_view(App& app, int width, int height) { return render(app, width, height).image; }

void view_attach_window(App& app, void* native_handle) { renderer(app).attach_window(native_handle); }
void view_detach_window(App& app) {
  if (state(app).renderer) state(app).renderer->detach_window();
}

std::array<int, 2> view_present(App& app) {
  ViewState& vs = state(app);
  Renderer& r = renderer(app);
  const int pixel_scale = vs.quality.value("antialiasing", std::string("none")) == "ssaa2" ? 2 : 1;
  const std::array<int, 2> size = r.prepare_window(pixel_scale);
  if (size[0] == 0 || size[1] == 0) return size;
  const auto built = cached_scene(app);
  float view[16], mvp[16];
  matrices(vs, *built, size[0], size[1], mvp, view);
  RenderScene scene = built->scene;  // 강조 오버레이·화면 고정 요소를 더한 사본(캐시는 그대로)
  scene.pixel_scale = pixel_scale;
  add_overlay(vs, *built, scene);
  add_bc_marks(vs, *built, scene, size[1]);
  apply_clips(vs, scene);
  if (vs.quality.value("transparency", std::string("unsorted")) == "sorted") sort_transparent(scene, view);
  add_hud(app, vs, *built, scene, size[0], size[1], view, mvp);
  const std::array<int, 2> drawn = r.present(scene, mvp, view);
  if (drawn[0] != size[0] || drawn[1] != size[1]) {  // 준비와 출력 사이에 외부에서 크기가 바뀐 경우만 재출력
    if (drawn[0] == 0) return drawn;
    matrices(vs, *built, drawn[0], drawn[1], mvp, view);
    add_hud(app, vs, *built, scene, drawn[0], drawn[1], view, mvp);
    r.present(scene, mvp, view);
  }
  vs.picks = built->picks, vs.legend = built->legend, vs.window_size = r.window_size();
  return vs.window_size;
}

Json view_hover(App& app, int x, int y) {
  ViewState& vs = state(app);
  if (!renderer(app).has_window()) throw Error("invalid_state", "창이 붙어 있지 않습니다");
  const std::uint32_t id = renderer(app).pick_window(x, y);
  const bool changed = id != vs.hover;
  vs.hover = id;
  Json out = pick_json(vs, id);
  out["changed"] = changed;
  return out;
}

Json view_pick_window(App& app, int x, int y) {
  if (!renderer(app).has_window()) throw Error("invalid_state", "창이 붙어 있지 않습니다");
  return pick_json(state(app), renderer(app).pick_window(x, y));
}

// --- 마우스 조작(RND-09). 픽셀 이동을 창 크기에 대한 비율로 바꿔 쓴다.
Json view_orbit_begin(App& app, double x, double y, int width, int height) {
  if (x < 0 || y < 0 || x >= width || y >= height)
    throw Error("out_of_range", "회전 시작점은 뷰포트 안에 있어야 합니다");
  ViewState& vs = state(app);
  // 와이어프레임도 숨김·변형 설정을 적용한 모델 표면에서 중심을 고른다. 표시 캐시는 바꾸지 않는다.
  const auto built = vs.mode == "wireframe" ? std::make_shared<Built>(build_scene(app, true)) : cached_scene(app);
  Camera& c = vs.camera;
  if (!vs.camera_set) fit(c, built->bounds), vs.camera_set = true;
  const V3 f = unit(sub(c.target, c.eye)), right = screen_right(c), up = cross(right, f);
  const double nx = 2.0 * x / width - 1.0, ny = 1.0 - 2.0 * y / height;
  const double half = c.ortho ? c.height * 0.5 : std::tan(c.fov * kPi / 360.0);
  const V3 offset = add(mul(right, nx * half * width / height), mul(up, ny * half));
  const V3 origin = c.ortho ? add(c.eye, offset) : c.eye;
  const V3 ray = c.ortho ? f : add(f, offset);  // dot(ray, f) = 1: t 는 뷰 깊이
  const auto [near_depth, far_depth] = depth_range(c, *built);
  double nearest = far_depth;
  bool hit = false;
  RenderScene clips;
  apply_clips(vs, clips);
  auto intersect = [&](const std::vector<RenderVertex>& triangles) {
    for (std::size_t i = 0; i + 2 < triangles.size(); i += 3) {
      // HUD·하중 심볼은 회전 중심 후보가 아니다. 투명도 0 인 면도 제외한다.
      if (!triangles[i].id || !triangles[i].color[3]) continue;
      auto position = [&](std::size_t j) -> V3 { const auto& p = triangles[j].pos; return {p[0], p[1], p[2]}; };
      const V3 a = position(i), e1 = sub(position(i + 1), a), e2 = sub(position(i + 2), a);
      const V3 p = cross(ray, e2);
      const double det = dot(e1, p);
      if (std::fabs(det) <= 1e-12 * norm(e1) * norm(e2) * norm(ray)) continue;
      const V3 s = sub(origin, a), q = cross(s, e1);
      const double u = dot(s, p) / det, v = dot(ray, q) / det;
      if (u < -1e-10 || v < -1e-10 || u + v > 1.0 + 1e-10) continue;
      const double t = dot(e2, q) / det;
      if (t < near_depth || t > nearest) continue;
      const V3 point = add(origin, mul(ray, t));
      bool clipped = false;
      for (const auto& plane : clips.clip_planes)
        if (plane[0] * point[0] + plane[1] * point[1] + plane[2] * point[2] + plane[3] < 0) { clipped = true; break; }
      if (clipped) continue;
      nearest = t, c.pivot = point, hit = true;
    }
  };
  intersect(built->scene.triangles);
  intersect(built->scene.transparent);
  return Json{{"hit", hit}, {"pivot", c.pivot}};
}

Json view_orbit(App& app, double dx, double dy, double height) {
  ViewState& vs = state(app);
  Camera& c = vs.camera;
  if (!vs.camera_set) fit(c, cached_scene(app)->bounds), vs.camera_set = true;
  const double h = height > 0 ? height : (vs.window_size[1] > 0 ? vs.window_size[1] : 600);
  const double distance = std::hypot(dx, dy);
  if (distance == 0) return camera_json(c);
  const V3 right = screen_right(c), up = cross(right, unit(sub(c.target, c.eye)));
  const V3 axis = add(mul(up, -dx / distance), mul(right, -dy / distance));
  const double angle = distance / h * kPi;
  auto rotate = [](const V3& v, const V3& axis, double ang) {  // 로드리게스 회전
    const V3 k = unit(axis);
    const double cs = std::cos(ang), sn = std::sin(ang);
    return add(add(mul(v, cs), mul(cross(k, v), sn)), mul(k, dot(k, v) * (1 - cs)));
  };
  c.eye = add(c.pivot, rotate(sub(c.eye, c.pivot), axis, angle));
  c.target = add(c.pivot, rotate(sub(c.target, c.pivot), axis, angle));
  c.up = rotate(up, axis, angle);
  c.up = unit(cross(screen_right(c), unit(sub(c.target, c.eye))));
  return camera_json(c);
}

Json view_pan(App& app, double dx, double dy) {
  ViewState& vs = state(app);
  Camera& c = vs.camera;
  if (!vs.camera_set) fit(c, cached_scene(app)->bounds), vs.camera_set = true;
  const double h = std::max(vs.window_size[1], 1);
  const double dist = norm(sub(c.target, c.eye));
  const double world_per_pixel = (c.ortho ? c.height : 2.0 * dist * std::tan(0.5 * c.fov * 3.14159265358979323846 / 180.0)) / h;
  const V3 f = unit(sub(c.target, c.eye));
  const V3 right = unit(cross(f, c.up)), up = unit(cross(right, f));
  const V3 shift = add(mul(right, -dx * world_per_pixel), mul(up, dy * world_per_pixel));
  c.eye = add(c.eye, shift), c.target = add(c.target, shift);
  return camera_json(c);
}

Json view_zoom(App& app, double factor, double, double) {
  ViewState& vs = state(app);
  Camera& c = vs.camera;
  if (!vs.camera_set) fit(c, cached_scene(app)->bounds), vs.camera_set = true;
  if (factor <= 0) factor = 1.0;
  if (c.ortho) {
    c.height /= factor;
  } else {
    const V3 rel = sub(c.eye, c.target);
    c.eye = add(c.target, mul(rel, 1.0 / factor));
  }
  return camera_json(c);
}

// 영역 확대(RND-09): 화면의 사각형(픽셀 x0,y0 ~ x1,y1, 뷰포트 w×h)이 화면에 꽉 차도록 카메라를 옮기고 당긴다.
// 사각형의 가운데를 화면 가운데로(카메라를 화면 축으로 평행 이동), 사각형이 가로·세로 모두 들어가는 배율로 확대. 깊이(시점-목표 거리)는 평면 이동이라 변하지 않는다
Json view_zoom_region(App& app, double x0, double y0, double x1, double y1, int width, int height) {
  ViewState& vs = state(app);
  Camera& c = vs.camera;
  if (!vs.camera_set) fit(c, cached_scene(app)->bounds), vs.camera_set = true;
  const double w = width > 0 ? width : std::max(vs.window_size[0], 1), h = height > 0 ? height : std::max(vs.window_size[1], 1);
  const double rw = std::max(std::fabs(x1 - x0), 1.0), rh = std::max(std::fabs(y1 - y0), 1.0);
  const double cx = 0.5 * (x0 + x1), cy = 0.5 * (y0 + y1);
  const double dist = norm(sub(c.target, c.eye));
  const double visible_h = c.ortho ? c.height : 2.0 * dist * std::tan(0.5 * c.fov * 3.14159265358979323846 / 180.0);
  const double world_per_pixel = visible_h / h;
  const V3 f = unit(sub(c.target, c.eye));
  const V3 right = unit(cross(f, c.up)), up = unit(cross(right, f));
  // 사각형 가운데가 화면 가운데로 오게 평행 이동(오른쪽 +, 아래쪽 + 인 픽셀 좌표)
  const V3 shift = add(mul(right, (cx - 0.5 * w) * world_per_pixel), mul(up, -(cy - 0.5 * h) * world_per_pixel));
  c.eye = add(c.eye, shift), c.target = add(c.target, shift), c.pivot = c.target;
  // 사각형이 세로(rh)로도 가로(rw, 화면 비율 보정)로도 들어가는 배율
  const double factor = h / std::max(rh, rw * h / w);
  if (c.ortho) c.height /= factor;
  else c.eye = add(c.target, mul(sub(c.eye, c.target), 1.0 / factor));
  return camera_json(c);
}

// 프로젝트가 새로 열리면(project.new·open) 모델에 매인 뷰 상태를 비운다: 이전 모델의 결과 표시·심볼·선택·숨김·클리핑 등이
// 새 모델에 그대로 입혀지면 노드 번호가 맞지 않아 엉뚱한 그림이 된다. 렌더러·창 연결·표시 모드·품질·HUD 설정은 남긴다.
void reset_model_bound_state(App& a) {
  ViewState& vs = state(a);
  vs.camera_transition.reset();
  vs.cube_hover.clear();
  vs.camera_set = false;
  vs.result = nullptr, vs.result_phase = 0.0, vs.legend = nullptr, vs.result_filter = nullptr, vs.result_options = Json::object();
  vs.symbols = nullptr, vs.highlighted.clear(), vs.target_nodes.clear(), vs.selected_objects.clear(), vs.selection = Json::array();
  vs.hidden.clear(), vs.appearance = Json::object(), vs.labels = Json::object(), vs.transparency = nullptr;
  vs.clip = Json::array(), vs.next_clip = 1, vs.animation = nullptr, vs.expand = nullptr, vs.beam_diagram = nullptr;
  vs.section = nullptr, vs.iso = nullptr, vs.streamlines = nullptr, vs.saved_views = Json::object(), vs.tree_state = Json::object();
  vs.picks.clear(), vs.hover = 0, vs.scene_key.clear(), vs.scene.reset();
}

void register_view_commands(App& app) {
  app.subscribe([&app](const Json& ev) {
    if (ev.value("source", "") == "reset") reset_model_bound_state(app);
  });
  {
    CommandSpec c = base("view.camera_get", 'V', "카메라 위치·방향·투영을 읽는다", "RND-08, RND-09, CMN-10, API-16");
    c.fn = [](App& a, const Json&) { return camera_json(state(a).camera); };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.camera_set", 'V', "카메라 위치·방향·투영을 지정한다", "RND-08, RND-09, CMN-10, API-16");
    c.params = {F("eye", "vector3", "카메라 위치"), F("target", "vector3", "바라보는 점"), F("up", "vector3", "위쪽 방향"),
                F("pivot", "vector3", "회전 중심(eye·target 지정 시 생략하면 target)"),
                F("projection", "string", "투영").one_of({"orthographic", "perspective"}),
                F("fov", "number", "원근 투영의 세로 시야각(도)").gt(0).lt(180), F("height", "number", "직교 투영에서 화면에 담기는 세로 길이").gt(0)};
    c.fn = [](App& a, const Json& p) {
      for (const FieldSpec& f : a.commands().at("view.camera_set").params)
        if (has(p, f.name.c_str())) check_value(f, p[f.name], nullptr);
      Camera cam = state(a).camera;
      if (has(p, "eye")) cam.eye = v3(p["eye"]);
      if (has(p, "target")) cam.target = v3(p["target"]);
      if (has(p, "up")) cam.up = v3(p["up"]);
      if (has(p, "eye") || has(p, "target")) cam.pivot = cam.target;
      if (has(p, "pivot")) cam.pivot = v3(p["pivot"]);
      if (has(p, "projection")) cam.ortho = p["projection"] == "orthographic";
      if (has(p, "fov")) cam.fov = p["fov"].get<double>();
      if (has(p, "height")) cam.height = p["height"].get<double>();
      if (norm(sub(cam.target, cam.eye)) == 0.0) throw Error("out_of_range", "카메라 위치와 바라보는 점이 같습니다", {{"param", "eye"}});
      state(a).camera = cam, state(a).camera_set = true;
      return camera_json(cam);
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.orbit_begin", 'V', "화면 좌표 아래 가장 가까운 모델 표면을 회전 중심으로 정한다. 빈 곳이면 기존 중심 유지", "RND-09");
    c.params = {F("x", "number", "왼쪽부터의 연속 화면 좌표").call_req().ge(0), F("y", "number", "위쪽부터의 연속 화면 좌표").call_req().ge(0),
                F("width", "integer", "좌표와 같은 단위의 뷰포트 너비(기본 창 너비, 창 없으면 800)").ge(1).le(16384),
                F("height", "integer", "좌표와 같은 단위의 뷰포트 높이(기본 창 높이, 창 없으면 600)").ge(1).le(16384)};
    c.fn = [](App& a, const Json& p) {
      for (const FieldSpec& f : a.commands().at("view.orbit_begin").params)
        if (has(p, f.name.c_str())) check_value(f, p[f.name], nullptr);
      const auto size = state(a).window_size;
      return view_orbit_begin(a, p["x"].get<double>(), p["y"].get<double>(),
                              p.value("width", size[0] > 0 ? size[0] : 800), p.value("height", size[1] > 0 ? size[1] : 600));
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.orbit", 'V', "고정된 회전 중심 둘레에서 화면 축으로 자유 회전한다(높이만큼 끌면 180도)", "RND-09");
    c.params = {F("dx", "number", "오른쪽 방향 이동량").call_req(), F("dy", "number", "아래쪽 방향 이동량").call_req(),
                F("height", "integer", "이동량과 같은 단위의 뷰포트 높이(기본 창 높이, 창 없으면 600)").ge(1).le(16384)};
    c.fn = [](App& a, const Json& p) {
      for (const FieldSpec& f : a.commands().at("view.orbit").params)
        if (has(p, f.name.c_str())) check_value(f, p[f.name], nullptr);
      return view_orbit(a, p["dx"].get<double>(), p["dy"].get<double>(), p.value("height", 0));
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.zoom_region", 'V', "화면의 사각형 영역(픽셀 x0,y0~x1,y1)이 뷰포트에 꽉 차게 확대한다(영역 확대: 버튼을 누르고 끌어 고른 영역)", "RND-09");
    c.params = {F("x0", "number", "사각형 한 꼭짓점 x(픽셀, 왼쪽 0)").call_req().ex(100.0), F("y0", "number", "그 y(픽셀, 위쪽 0)").call_req().ex(100.0),
                F("x1", "number", "맞은편 꼭짓점 x").call_req().ex(300.0), F("y1", "number", "맞은편 꼭짓점 y").call_req().ex(250.0),
                F("width", "integer", "픽셀 좌표와 같은 단위의 뷰포트 너비(기본 창 너비)").ge(1).le(16384),
                F("height", "integer", "뷰포트 높이(기본 창 높이)").ge(1).le(16384)};
    c.fn = [](App& a, const Json& p) {
      for (const FieldSpec& f : a.commands().at("view.zoom_region").params)
        if (has(p, f.name.c_str())) check_value(f, p[f.name], nullptr);
      return view_zoom_region(a, p["x0"].get<double>(), p["y0"].get<double>(), p["x1"].get<double>(), p["y1"].get<double>(),
                              p.value("width", 0), p.value("height", 0));
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.standard", 'V', "표준 뷰로 바꾸고 전체에 맞춘다", "RND-10");
    c.params = {F("name", "string", "뷰 이름").call_req().one_of({"front", "back", "left", "right", "top", "bottom", "iso"}).ex("iso")};
    c.fn = [](App& a, const Json& p) {
      check_value(a.commands().at("view.standard").params[0], p["name"], nullptr);
      Camera& cam = state(a).camera;
      cam = standard_camera(cam, build_scene(a).bounds, p["name"].get<std::string>());
      state(a).camera_set = true;
      return camera_json(cam);
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.transition_begin", 'V', "현재 카메라에서 표준 뷰로 부드러운 전환을 준비한다. 시작 시 화면은 이동하지 않는다", "RND-09, RND-10");
    c.params = {F("name", "string", "목표 방향(6면·12에지·8꼭짓점과 iso)").call_req().one_of(render_detail::navigation_cube_views())};
    c.fn = [](App& a, const Json& p) {
      check_value(a.commands().at("view.transition_begin").params[0], p["name"], nullptr);
      ViewState& vs = state(a);
      const auto built = cached_scene(a);
      if (!vs.camera_set) fit(vs.camera, built->bounds), vs.camera_set = true;
      const Camera target = standard_camera(vs.camera, built->bounds, p["name"].get<std::string>());
      const bool active = camera_json(vs.camera) != camera_json(target);
      vs.camera_transition.reset();
      if (active) vs.camera_transition = CameraTransition{vs.camera, target, vs.camera};
      return Json{{"active", active}, {"camera", camera_json(vs.camera)}, {"target", camera_json(target)}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.transition_step", 'V', "전환 진행률을 적용한다. 방향은 최단 경로 구면 보간, 출발·도착은 가감속한다", "RND-09, RND-10");
    c.params = {F("progress", "number", "전체 진행률(0~1), 1이면 목표에 정확히 도착하고 종료").call_req().ge(0).le(1)};
    c.fn = [](App& a, const Json& p) {
      check_value(a.commands().at("view.transition_step").params[0], p["progress"], nullptr);
      ViewState& vs = state(a);
      // 수동 조작·외부 API 가 카메라를 바꿨으면 그 값을 오래된 전환으로 덮지 않는다.
      if (vs.camera_transition && camera_json(vs.camera) != camera_json(vs.camera_transition->last)) vs.camera_transition.reset();
      if (vs.camera_transition) {
        const double t = p["progress"].get<double>();
        vs.camera = interpolate_camera(vs.camera_transition->from, vs.camera_transition->to, t);
        vs.camera_transition->last = vs.camera;
        if (t >= 1) vs.camera_transition.reset();
      }
      return Json{{"active", vs.camera_transition.has_value()}, {"camera", camera_json(vs.camera)}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.transition_cancel", 'V', "전환을 현재 위치에서 멈춘다", "RND-09, RND-10");
    c.fn = [](App& a, const Json&) {
      ViewState& vs = state(a);
      vs.camera_transition.reset();
      return Json{{"active", false}, {"camera", camera_json(vs.camera)}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.fit", 'V', "보는 방향은 그대로 두고 전체가 들어오게 맞춘다", "RND-10");
    c.fn = [](App& a, const Json&) {
      fit(state(a).camera, build_scene(a).bounds);
      state(a).camera_set = true;
      return camera_json(state(a).camera);
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.display_mode", 'V', "표시 모드(음영·음영+경계선·와이어프레임)와 무엇을 보일지(형상·메시)를 지정한다", "RND-15");
    c.params = {F("mode", "string", "표시 모드").one_of({"shaded", "shaded_edges", "wireframe"}),
                F("show", "string", "auto = 메시가 있으면 메시, 없으면 형상").one_of({"auto", "geometry", "mesh", "both"})};
    c.fn = [](App& a, const Json& p) {
      for (const FieldSpec& f : a.commands().at("view.display_mode").params)
        if (has(p, f.name.c_str())) check_value(f, p[f.name], nullptr);
      if (has(p, "mode")) state(a).mode = p["mode"].get<std::string>();
      if (has(p, "show")) state(a).show = p["show"].get<std::string>();
      return Json{{"mode", state(a).mode}, {"show", state(a).show}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.result_show", 'V', "메시에 입힐 결과(프레임·종류·성분)와 변형 배율을 지정한다. result 를 비우면 결과 표시를 끈다",
                         "RES-04, RES-05, RES-08, RES-54, RES-59, WT-26, RND-28, RND-29, RND-30");
    c.params = {F("result", "integer", "결과(result.open 이 돌려준 번호). 없으면 끈다").ge(1), F("frame", "integer", "프레임 번호").ge(1),
                F("field", "string", "결과 종류(예: STRESS)"), F("component", "string", "성분 또는 파생량(예: mises)"),
                F("deform_scale", "number", "변위에 곱할 배율(0 = 변형 없이)"), F("min", "number", "범례의 최솟값(없으면 자동)"),
                F("max", "number", "범례의 최댓값(없으면 자동)"),
                F("location", "string", "값 위치: nodal(절점 값을 요소 안에서 보간, 기본) | element(요소마다 꼭짓점 평균 한 색)").one_of({"nodal", "element"}),
                F("shell_face", "string", "전개된 쉘 결과의 면(top | bottom | mid): 모델 쉘 노드 기준 그 면의 값을 입힌다(RES-54·RES-08)").one_of({"top", "bottom", "mid"})};
    c.fn = [](App& a, const Json& p) {
      if (!has(p, "result")) {
        state(a).result = nullptr;
        return Json{{"shown", false}};
      }
      ResultFile& file = results(a).get(p["result"].get<Id>());
      Json r{{"result", p["result"]}, {"frame", p.value("frame", static_cast<int>(file.frames.size()))}};
      ResultFrame& frame = file.frame(r["frame"].get<int>());
      if (has(p, "field")) {
        const ResultField& f = file.field(frame, p["field"].get<std::string>());
        if (!has(p, "component")) throw Error("missing_param", "필수 매개변수가 없습니다: component", {{"param", "component"}});
        const std::string comp = p["component"].get<std::string>();
        if (std::find(f.components.begin(), f.components.end(), comp) == f.components.end()) derived_values(f, comp);  // 없으면 Error
        r["field"] = p["field"], r["component"] = comp;
      }
      for (const char* k : {"deform_scale", "min", "max", "location", "shell_face"})
        if (has(p, k)) r[k] = p[k];
      for (const FieldSpec& fs : a.commands().at("view.result_show").params)
        if ((fs.name == "location" || fs.name == "shell_face") && has(p, fs.name.c_str())) check_value(fs, p[fs.name], nullptr);
      if (has(p, "shell_face")) {  // 전개된 쉘 결과여야 한다
        if (!has(p, "field")) throw Error("missing_param", "shell_face 에는 field 와 component 가 필요합니다", {{"param", "field"}});
        shell_face_values(a, file, file.field(frame, p["field"].get<std::string>()), p["component"].get<std::string>(), p["shell_face"].get<std::string>());
      }
      if (r.value("deform_scale", 0.0) != 0.0) file.field(frame, "DISP");  // 변위 결과가 있어야 한다
      state(a).result = r;
      return Json{{"shown", true}, {"settings", r}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.result_state", 'Q', "지금 화면에 입힌 결과(결과 번호·프레임·종류·성분·배율 등)를 조회한다. 없으면 shown=false", "WT-26, RES-04");
    c.fn = [](App& a, const Json&) {
      const ViewState& vs = state(a);
      if (vs.result.is_null()) return Json{{"shown", false}};
      Json out{{"shown", true}, {"settings", vs.result}};
      ResultFile& file = results(a).get(vs.result["result"].get<Id>());
      const ResultFrame& fr = file.frame(vs.result["frame"].get<int>());
      out["step"] = fr.step, out["increment"] = fr.increment, out["value"] = fr.value, out["case"] = file.case_id ? Json(file.case_id) : Json();
      if (vs.result_phase != 0.0) out["phase"] = vs.result_phase;
      return out;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.screenshot", 'J', "현재 뷰를 지정한 크기의 이미지(PNG)로 저장한다. 화면 크기와 무관하다", "RND-45, RES-25");
    c.params = {F("path", "string", "파일 경로(.png)").call_req().ex("view.png"), width_param(), height_param(),
                F("transparent", "bool", "배경을 투명하게")};
    c.fn = [](App& a, const Json& p) {
      int w, h;
      size_of(a, "view.screenshot", p, w, h);
      const Json bg = Json::array({1.0, 1.0, 1.0, p.value("transparent", false) ? 0.0 : 1.0});
      const Rendered r = render(a, w, h, &bg);
      write_png(p["path"].get<std::string>(), r.image);
      return Json{{"path", p["path"]}, {"width", w}, {"height", h}, {"triangles", r.built.scene.triangles.size() / 3},
                  {"lines", r.built.scene.lines.size() / 2}, {"legend", r.built.legend}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.pick", 'Q', "화면 좌표에 있는 객체(형상의 면, 요소면, 요소)를 조회한다", "RND-24, RND-48");
    c.params = {F("x", "integer", "가로 위치(픽셀, 왼쪽이 0)").call_req().ge(0).ex(400),
                F("y", "integer", "세로 위치(픽셀, 위가 0)").call_req().ge(0).ex(300), width_param(), height_param()};
    c.fn = [](App& a, const Json& p) {
      int w, h;
      size_of(a, "view.pick", p, w, h);
      const int x = p["x"].get<int>(), y = p["y"].get<int>();
      if (x < 0 || y < 0 || x >= w || y >= h) throw Error("out_of_range", "화면 밖의 좌표입니다", {{"param", "x"}});
      if (!has(p, "width") && !has(p, "height") && renderer(a).has_window()) return view_pick_window(a, x, y);
      const Rendered r = render(a, w, h);
      return pick_json(state(a), r.image.ids[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)]);
    };
    app.register_command(std::move(c));
  }
  {
    // 선택(WT-14, RND-26): 픽 기록의 목록. 강조 표시와 함께 간다. 필터는 픽·마우스 오버가 돌려주는 종류를 제한한다
    auto to_record = [](const Json& item) {
      PickRecord rec;
      rec.kind = item.value("kind", std::string());
      if (rec.kind == "face") rec.a = item.value("part", Id(0)), rec.b = item.value("index", 0);
      else if (rec.kind == "element_face") rec.a = item.value("element", Id(0)), rec.b = item.value("face", 0);
      else if (rec.kind == "element") rec.a = item.value("element", Id(0)), rec.b = 0;
      else throw Error("invalid_param", "선택 항목의 kind 는 face, element_face, element 가운데 하나여야 합니다", {{"param", "items"}});
      if (!rec.a) throw Error("missing_param", "선택 항목에 part 또는 element 가 없습니다", {{"param", "items"}});
      return rec;
    };
    CommandSpec g = base("selection.get", 'Q', "현재 선택 목록과 필터를 조회한다", "WT-14, RND-26");
    g.fn = [](App& a, const Json&) {
      const ViewState& vs = state(a);
      return Json{{"items", vs.selection}, {"filter", std::vector<std::string>(vs.pick_filter.begin(), vs.pick_filter.end())}};
    };
    app.register_command(std::move(g));
    CommandSpec s = base("selection.set", 'V',
                         "선택 목록을 바꾼다(add 면 더한다). 선택된 것은 강조 표시된다. 트리 항목(kind=object, id)을 넣으면 그 객체의 적용 대상(하중·경계조건·프로퍼티·셋·메시 파트 등)이 "
                         "view.show_targets 처럼 강조된다(WT-22)",
                         "WT-14, WT-22, RND-27");
    s.params = {F("items", "object_list", "선택 항목(view.pick 이 돌려준 형식, 또는 {kind: object, id})").call_req()
                    .of({F("kind", "string", "종류").call_req().one_of({"face", "element_face", "element", "object"}).ex("element"),
                         F("part", "ref", "형상 파트(face)").ref("part"), F("index", "integer", "면 번호(face)").ge(1),
                         F("element", "integer", "요소(element_face·element)").ge(1), F("face", "integer", "요소면 번호(element_face)").ge(1),
                         F("hit", "bool", "view.pick 의 적중 표시(무시)"), F("filtered", "string", "view.pick 의 필터 표시(무시)"),
                         F("id", "ref", "모델 객체(object)")}),
                F("add", "bool", "기존 선택에 더한다")};
    s.fn = [to_record](App& a, const Json& p) {
      ViewState& vs = state(a);
      if (!p.value("add", false)) vs.selection = Json::array(), vs.highlighted.clear(), vs.selected_objects.clear();
      for (const Json& item : p["items"]) {
        if (item.value("kind", std::string()) == "object") {
          if (!has(item, "id")) throw Error("missing_param", "object 항목에는 id 가 필요합니다", {{"param", "items"}});
          const Id id = item["id"].get<Id>();
          if (!a.model().find(id)) throw Error("not_found", "없는 객체입니다: " + std::to_string(id), {{"param", "items"}, {"object", id}});
          if (std::find(vs.selected_objects.begin(), vs.selected_objects.end(), id) == vs.selected_objects.end()) {
            vs.selected_objects.push_back(id);
            vs.selection.push_back(Json{{"kind", "object"}, {"id", id}, {"object_kind", a.model().get(id).kind}, {"name", a.model().get(id).name}});
          }
          continue;
        }
        const PickRecord rec = to_record(item);
        if (!vs.pick_filter.empty() && !vs.pick_filter.count(rec.kind)) continue;
        const bool dup = std::any_of(vs.highlighted.begin(), vs.highlighted.end(), [&](const PickRecord& h) { return h.kind == rec.kind && h.a == rec.a && h.b == rec.b; });
        if (dup) continue;
        vs.highlighted.push_back(rec);
        Json copy = item;
        vs.selection.push_back(copy);
      }
      // 객체 항목의 적용 대상을 강조한다(없으면 해제)
      if (a.commands().count("view.show_targets")) {
        Json ids = Json::array();
        for (Id id : vs.selected_objects) ids.push_back(id);
        const std::vector<PickRecord> keep = vs.highlighted;  // show_targets 는 강조를 지우므로 픽 항목의 강조는 되살린다
        a.commands().at("view.show_targets").fn(a, ids.empty() ? Json::object() : Json{{"ids", ids}});
        for (const PickRecord& r : keep)
          if (std::none_of(vs.highlighted.begin(), vs.highlighted.end(), [&](const PickRecord& h) { return h.kind == r.kind && h.a == r.a && h.b == r.b; }))
            vs.highlighted.push_back(r);
      }
      return Json{{"items", vs.selection}, {"count", vs.selection.size()}};
    };
    app.register_command(std::move(s));
    CommandSpec c = base("selection.clear", 'V', "선택을 모두 해제한다", "WT-14");
    c.fn = [](App& a, const Json&) {
      state(a).selection = Json::array(), state(a).highlighted.clear(), state(a).selected_objects.clear(), state(a).target_nodes.clear();
      return Json{{"count", 0}};
    };
    app.register_command(std::move(c));
    CommandSpec f = base("selection.set_filter", 'V', "선택 가능한 종류를 제한한다(비우면 전부). 픽·마우스 오버가 다른 종류를 돌려주지 않는다", "RND-26");
    f.params = {F("kinds", "string_list", "허용할 종류(face, element_face, element)").ex({"element"})};
    f.fn = [](App& a, const Json& p) {
      ViewState& vs = state(a);
      vs.pick_filter.clear();
      for (const Json& k : p.value("kinds", Json::array())) {
        const std::string kind = k.get<std::string>();
        if (kind != "face" && kind != "element_face" && kind != "element") throw Error("invalid_param", "알 수 없는 종류: " + kind, {{"param", "kinds"}});
        vs.pick_filter.insert(kind);
      }
      return Json{{"filter", std::vector<std::string>(vs.pick_filter.begin(), vs.pick_filter.end())}};
    };
    app.register_command(std::move(f));
  }
  {
    CommandSpec c = base("view.show", 'V', "숨긴 객체(형상 파트·메시 파트)를 다시 보인다", "WT-20, RND-23");
    c.params = {F("ids", "integer_list", "객체 ID").call_req().ex({1})};
    c.fn = [](App& a, const Json& p) {
      for (const Json& i : p["ids"]) state(a).hidden.erase(i.get<Id>());
      return Json{{"hidden", std::vector<Id>(state(a).hidden.begin(), state(a).hidden.end())}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.hide", 'V', "객체를 숨긴다: 형상 파트·메시 파트는 형상·요소를, 하중 셋·구속 셋은 그 항목의 심볼을 그리지 않는다", "WT-20, RND-23, RND-34");
    c.params = {F("ids", "integer_list", "객체 ID").call_req().ex({1})};
    c.fn = [](App& a, const Json& p) {
      for (const Json& i : p["ids"]) {
        const Object& o = a.model().get(i.get<Id>());
        if (o.kind != "part" && o.kind != "mesh_part" && o.kind != "load_set" && o.kind != "bc_set")
          throw Error("wrong_kind", "형상 파트·메시 파트·하중 셋·구속 셋만 숨길 수 있습니다", {{"object", o.id}, {"kind", o.kind}});
        state(a).hidden.insert(o.id);
      }
      return Json{{"hidden", std::vector<Id>(state(a).hidden.begin(), state(a).hidden.end())}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.isolate", 'V', "지정한 객체만 보이고 나머지 파트는 숨긴다", "WT-21, RND-23");
    c.params = {F("ids", "integer_list", "보일 객체 ID").call_req().ex({1})};
    c.fn = [](App& a, const Json& p) {
      std::set<Id> keep;
      for (const Json& i : p["ids"]) keep.insert(a.model().get(i.get<Id>()).id);
      ViewState& vs = state(a);
      vs.hidden.clear();
      for (const char* kind : {"part", "mesh_part"})
        for (const Object* o : a.model().by_kind(kind))
          if (!keep.count(o->id)) vs.hidden.insert(o->id);
      return Json{{"hidden", std::vector<Id>(vs.hidden.begin(), vs.hidden.end())}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.show_all", 'V', "숨긴 것을 모두 보인다", "WT-20, RND-23");
    c.fn = [](App& a, const Json&) {
      state(a).hidden.clear();
      return Json{{"hidden", Json::array()}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.color_by", 'V', "메시 색 기준을 지정한다: part(메시 파트), property(프로퍼티 할당), material(재료). 할당 없는 요소는 회색", "MSH-24, PRP-13");
    c.params = {F("by", "string", "색 기준").call_req().one_of({"part", "property", "material"}).ex("property")};
    c.fn = [](App& a, const Json& p) {
      check_value(a.commands().at("view.color_by").params[0], p["by"], nullptr);
      state(a).color_by = p["by"].get<std::string>();
      return Json{{"by", state(a).color_by}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.object_colors", 'V', "메시 파트·재료·프로퍼티·형상 파트의 표시 색과 지금 색 기준(color_by)을 조회한다(트리 아이콘·범례용, 화면과 같은 색)", "MSH-24, PRP-13, WT-25");
    c.fn = [](App& a, const Json&) {
      const ViewState& vs = state(a);
      Json out{{"color_by", vs.color_by}};
      for (const char* kind : {"mesh_part", "material", "property", "part"}) {
        Json arr = Json::array();
        for (const Object* o : a.model().by_kind(kind)) {
          const auto c3 = object_color(a, vs.appearance, kind, o->id);
          const bool custom = vs.appearance.contains(std::to_string(o->id)) && vs.appearance[std::to_string(o->id)].contains("color");
          arr.push_back(Json{{"id", o->id}, {"name", o->name}, {"color", {c3[0], c3[1], c3[2]}}, {"custom", custom}});
        }
        out[kind] = arr;
      }
      return out;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.labels", 'V', "노드·요소 번호 라벨을 화면에 표시한다(3D 위치에 투영). 매개변수 없이 부르면 지운다. 최대 5000개", "RND-36");
    c.params = {F("nodes", "integer_list", "라벨을 붙일 노드").ex({1, 2}), F("elements", "integer_list", "라벨을 붙일 요소").ex({1}),
                F("all_nodes", "bool", "모든 절점 번호 표시 토글(해당 종류만 변경)").ex(true),
                F("all_elements", "bool", "모든 요소 번호 표시 토글(해당 종류만 변경)").ex(true)};
    c.fn = [](App& a, const Json& p) {
      // 목록 지정/무인자는 기존처럼 전체 교체, 토글만 지정하면 다른 종류는 유지한다.
      Json l = (has(p, "all_nodes") || has(p, "all_elements")) && !has(p, "nodes") && !has(p, "elements")
          ? state(a).labels : Json::object();
      if (has(p, "nodes")) l["nodes"] = p["nodes"];
      if (has(p, "elements")) l["elements"] = p["elements"];
      for (const char* kind : {"nodes", "elements"}) {
        const std::string key = std::string("all_") + kind;
        if (has(p, key.c_str())) {
          l[key] = p[key];
          l.erase(kind);
        }
      }
      state(a).labels = l;
      return Json{{"nodes", l.value("all_nodes", false) ? a.mesh().node_count() : l.value("nodes", Json::array()).size()},
                  {"elements", l.value("all_elements", false) ? a.mesh().element_count() : l.value("elements", Json::array()).size()}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.labels_get", 'Q', "절점·요소 번호의 표시 여부와 대상 개수를 조회한다(배열 복사 없음)", "RND-36");
    c.fn = [](App& a, const Json&) {
      const Json& l = state(a).labels;
      const bool all_nodes = l.value("all_nodes", false), all_elements = l.value("all_elements", false);
      const std::size_t nodes = all_nodes ? a.mesh().node_count() : (l.contains("nodes") ? l["nodes"].size() : 0);
      const std::size_t elements = all_elements ? a.mesh().element_count() : (l.contains("elements") ? l["elements"].size() : 0);
      return Json{{"nodes", all_nodes || nodes > 0}, {"elements", all_elements || elements > 0},
                  {"node_count", nodes}, {"element_count", elements}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("geometry.set_tessellation", 'V', "표시용 삼각화의 정밀도를 바꾼다(0 = 기본: 경계 상자 대각선의 1/1000, 각 20도)", "GEO-06");
    c.params = {F("deflection", "number", "현 편차(길이)").ge(0).unit("length").ex(0.1), F("angle", "number", "각 편차(도)").ge(0).ex(20.0)};
    c.fn = [](App& a, const Json& p) {
      for (const FieldSpec& f : a.commands().at("geometry.set_tessellation").params)
        if (has(p, f.name.c_str())) check_value(f, p[f.name], nullptr);
      if (has(p, "deflection")) state(a).tess_deflection = p["deflection"].get<double>();
      if (has(p, "angle")) state(a).tess_angle = p["angle"].get<double>();
      return Json{{"deflection", state(a).tess_deflection}, {"angle", state(a).tess_angle}};
    };
    app.register_command(std::move(c));
  }
  {
    // 뷰 저장·복원(RND-10): 카메라·표시 모드·결과 표시·클리핑·투명·숨김·색 기준
    auto snapshot = [](const ViewState& vs) {
      return Json{{"camera", camera_json(vs.camera)}, {"camera_set", vs.camera_set}, {"mode", vs.mode}, {"show", vs.show}, {"result", vs.result},
                  {"clip", vs.clip}, {"transparency", vs.transparency}, {"hidden", std::vector<Id>(vs.hidden.begin(), vs.hidden.end())},
                  {"color_by", vs.color_by}, {"symbols", vs.symbols}};
    };
    CommandSpec c = base("view.save", 'V', "현재 뷰(카메라·표시 상태)를 이름으로 저장한다(실행 중 상태)", "RND-10");
    c.params = {F("name", "string", "뷰 이름").call_req().ex("front_contour")};
    c.fn = [snapshot](App& a, const Json& p) {
      ViewState& vs = state(a);
      vs.saved_views[p["name"].get<std::string>()] = snapshot(vs);
      Json names = Json::array();
      for (auto& [k, v] : vs.saved_views.items()) names.push_back(k);
      return Json{{"name", p["name"]}, {"saved", names}};
    };
    app.register_command(std::move(c));
    CommandSpec r = base("view.restore", 'V', "저장한 뷰를 복원한다(이름을 비우면 저장한 이름 목록)", "RND-10");
    r.params = {F("name", "string", "뷰 이름").ex("front_contour")};
    r.fn = [](App& a, const Json& p) {
      ViewState& vs = state(a);
      if (!has(p, "name")) {
        Json names = Json::array();
        for (auto& [k, v] : vs.saved_views.items()) names.push_back(k);
        return Json{{"saved", names}};
      }
      const std::string name = p["name"].get<std::string>();
      if (!vs.saved_views.contains(name)) throw Error("not_found", "저장한 뷰가 없습니다: " + name, {{"param", "name"}});
      const Json& s = vs.saved_views[name];
      const Json& cam = s["camera"];
      vs.camera.eye = v3(cam["eye"]), vs.camera.target = v3(cam["target"]), vs.camera.up = v3(cam["up"]);
      vs.camera.pivot = v3(cam.value("pivot", cam["target"]));
      vs.camera.ortho = cam["projection"] == "orthographic", vs.camera.fov = cam["fov"], vs.camera.height = cam["height"];
      vs.camera_set = s.value("camera_set", true);
      vs.mode = s["mode"], vs.show = s["show"], vs.result = s["result"], vs.transparency = s["transparency"];
      vs.clip = s["clip"].is_array() ? s["clip"] : s["clip"].is_object() ? Json::array({Json{{"id", 1}, {"point", s["clip"]["point"]}, {"normal", s["clip"]["normal"]}, {"enabled", true}}}) : Json::array();
      vs.color_by = s["color_by"], vs.symbols = s["symbols"];
      vs.hidden.clear();
      for (const Json& h : s["hidden"]) vs.hidden.insert(h.get<Id>());
      return Json{{"name", name}, {"restored", s}};
    };
    app.register_command(std::move(r));
  }
  {
    // 애니메이션(RND-31, RES-11): 결과 프레임 목록을 차례로 보인다. 창 없는 실행에서는 advance 로 한 프레임씩 넘기고,
    // UI 는 타이머로 advance 를 부른다. 프레임을 비우면 결과의 모든 프레임.
    CommandSpec c = base("view.animate", 'V',
                         "결과 프레임 애니메이션을 설정(frames·interval_ms·loop)·시작·정지하거나 한 프레임 넘긴다(advance). "
                         "위상 애니메이션(RES-48): phase_steps 를 주면 현재 프레임(모드 형상)을 0~360° 로 흔든다(변형 배율 × cos(위상)); phase 로 한 위상을 바로 보인다",
                         "RES-11, RES-48, RND-31");
    c.params = {F("frames", "integer_list", "보일 프레임 번호(비우면 전부)").ex({1, 2, 3}), F("interval_ms", "integer", "프레임 간격(ms, 기본 200)").ge(1),
                F("loop", "bool", "반복(기본 켬)"), F("advance", "bool", "다음 프레임으로 넘긴다"), F("stop", "bool", "멈춘다"),
                F("deform_scale", "number", "변형 배율(모드 형상은 ± 로 흔든다)"),
                F("phase_steps", "integer", "위상 애니메이션: 한 주기(0~360°)를 나누는 수").ge(2).ex(12),
                F("phase", "number", "이 위상(도)의 모양을 바로 보인다(0 = 변형 배율 그대로, 180 = 반대 부호)")};
    c.fn = [](App& a, const Json& p) {
      ViewState& vs = state(a);
      if (p.value("stop", false)) {
        if (!vs.animation.is_null()) vs.animation["playing"] = false;
        vs.result_phase = 0.0;
        return vs.animation.is_null() ? Json{{"playing", false}} : vs.animation;
      }
      if (has(p, "phase") || has(p, "phase_steps")) {
        if (vs.result.is_null()) throw Error("invalid_state", "먼저 view.result_show 로 결과를 보인다");
        if (has(p, "deform_scale")) vs.result["deform_scale"] = p["deform_scale"];
        const double base_scale = vs.result.value("deform_scale", 0.0);
        if (has(p, "phase")) {
          vs.result_phase = p["phase"].get<double>();
          return Json{{"phase", vs.result_phase}, {"deform_scale", base_scale}, {"deform_scale_effective", base_scale * std::cos(vs.result_phase * kPi / 180.0)},
                      {"frame", vs.result["frame"]}};
        }
        const int steps = p["phase_steps"].get<int>();
        Json phases = Json::array();
        for (int k = 0; k < steps; ++k) phases.push_back(360.0 * k / steps);
        vs.animation = Json{{"frames", Json::array({vs.result["frame"]})}, {"phases", phases}, {"index", 0}, {"interval_ms", p.value("interval_ms", 200)},
                            {"loop", p.value("loop", true)}, {"playing", true}, {"deform_scale", base_scale}};
        vs.result_phase = 0.0;
        return vs.animation;
      }
      if (!p.value("advance", false)) {
        if (vs.result.is_null()) throw Error("invalid_state", "먼저 view.result_show 로 결과를 보인다");
        Json frames = p.value("frames", Json::array());
        if (frames.empty()) {
          ResultFile& file = results(a).get(vs.result["result"].get<Id>());
          for (const ResultFrame& fr : file.frames) frames.push_back(fr.index);
        }
        vs.animation = Json{{"frames", frames}, {"index", 0}, {"interval_ms", p.value("interval_ms", 200)}, {"loop", p.value("loop", true)},
                            {"playing", true}, {"deform_scale", p.value("deform_scale", vs.result.value("deform_scale", 0.0))}};
        vs.result["frame"] = frames[0];
        if (has(p, "deform_scale")) vs.result["deform_scale"] = p["deform_scale"];
        return vs.animation;
      }
      if (vs.animation.is_null() || !vs.animation["playing"].get<bool>()) throw Error("invalid_state", "애니메이션이 돌고 있지 않습니다");
      if (vs.animation.contains("phases")) {  // 위상 애니메이션: 위상만 돈다
        const Json& phases = vs.animation["phases"];
        int index = vs.animation["index"].get<int>() + 1;
        if (index >= static_cast<int>(phases.size())) {
          if (!vs.animation["loop"].get<bool>()) {
            vs.animation["playing"] = false;
            return vs.animation;
          }
          index = 0;
        }
        vs.animation["index"] = index;
        vs.result_phase = phases[static_cast<std::size_t>(index)].get<double>();
        return vs.animation;
      }
      const Json& frames = vs.animation["frames"];
      int index = vs.animation["index"].get<int>() + 1;
      if (index >= static_cast<int>(frames.size())) {
        if (!vs.animation["loop"].get<bool>()) {
          vs.animation["playing"] = false;
          return vs.animation;
        }
        index = 0;
      }
      vs.animation["index"] = index;
      vs.result["frame"] = frames[static_cast<std::size_t>(index)];
      return vs.animation;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.export_animation", 'J', "결과 프레임마다 화면을 이미지로 저장한다(<pattern> 의 {frame} 자리에 프레임 번호)", "RND-46, RES-25");
    c.params = {F("pattern", "string", "파일 경로 패턴(예: out/frame_{frame}.png)").call_req().ex("frames/f_{frame}.png"),
                F("frames", "integer_list", "프레임 번호(비우면 전부)").ex({1, 2}), width_param(), height_param(),
                F("phase_steps", "integer", "위상 애니메이션(RES-48): 현재 프레임을 0~360° 로 나눠 이 수만큼 저장한다({frame} 자리에 1부터 번호)").ge(2)};
    c.fn = [](App& a, const Json& p) {
      ViewState& vs = state(a);
      if (vs.result.is_null()) throw Error("invalid_state", "먼저 view.result_show 로 결과를 보인다");
      int w, h;
      size_of(a, "view.export_animation", p, w, h);
      if (has(p, "phase_steps")) {
        const int steps = p["phase_steps"].get<int>();
        const std::string pattern = p["pattern"].get<std::string>();
        if (pattern.find("{frame}") == std::string::npos) throw Error("invalid_param", "pattern 에 {frame} 이 있어야 합니다", {{"param", "pattern"}});
        const double saved = vs.result_phase;
        Json files = Json::array();
        for (int k = 0; k < steps; ++k) {
          vs.result_phase = 360.0 * k / steps;
          std::string path = pattern;
          path.replace(path.find("{frame}"), 7, std::to_string(k + 1));
          write_png(path, render(a, w, h).image);
          files.push_back(path);
        }
        vs.result_phase = saved;
        return Json{{"files", files}, {"count", files.size()}, {"width", w}, {"height", h}, {"phase_steps", steps}};
      }
      Json frames = p.value("frames", Json::array());
      if (frames.empty()) {
        ResultFile& file = results(a).get(vs.result["result"].get<Id>());
        for (const ResultFrame& fr : file.frames) frames.push_back(fr.index);
      }
      const std::string pattern = p["pattern"].get<std::string>();
      if (pattern.find("{frame}") == std::string::npos) throw Error("invalid_param", "pattern 에 {frame} 이 있어야 합니다", {{"param", "pattern"}});
      const Json saved_frame = vs.result["frame"];
      Json files = Json::array();
      for (const Json& fr : frames) {
        vs.result["frame"] = fr;
        std::string path = pattern;
        path.replace(path.find("{frame}"), 7, std::to_string(fr.get<int>()));
        write_png(path, render(a, w, h).image);
        files.push_back(path);
      }
      vs.result["frame"] = saved_frame;
      return Json{{"files", files}, {"count", files.size()}, {"width", w}, {"height", h}};
    };
    app.register_command(std::move(c));
  }
  {
    // 보고서(RES-26): report 객체의 항목을 모아 HTML 하나로 쓴다. 뷰는 PNG 를 base64 로 품고, 그래프는 값 표와 간단한 SVG 선 그래프,
    // table 은 조회 명령의 결과를 표로 적는다.
    CommandSpec c = base("report.generate", 'J', "보고서 객체의 항목(글·뷰·그래프·표)을 모아 HTML 파일로 만든다", "RES-26");
    c.target = "report";
    c.params = {F("id", "ref", "보고서 객체").ref("report").call_req(), F("path", "string", "저장할 파일(.html)").call_req().ex("report.html"),
                F("format", "string", "형식(기본 html). 확장이 ext.register_report 로 등록한 이름이면 그 명령에 id·path 를 넘긴다").ex("html")};
    c.fn = [](App& a, const Json& p) {
      const Object& rep = a.model().get(p["id"].get<Id>());
      if (rep.kind != "report") throw Error("wrong_kind", "보고서 객체가 아닙니다", {{"object", rep.id}, {"expected", "report"}});
      const std::string format = p.value("format", std::string("html"));
      if (format != "html") {
        // 확장이 등록한 보고서 형식(API-21)
        std::string command;
        if (a.commands().count("ext.registrations"))
          for (const Json& r : a.commands().at("ext.registrations").fn(a, Json::object()).value("report_formats", Json::array()))
            if (r.value("name", std::string()) == format) command = r.value("command", std::string());
        if (command.empty()) throw Error("not_found", "등록되지 않은 보고서 형식: " + format, {{"param", "format"}});
        Json r = a.execute(command, Json{{"id", rep.id}, {"path", p["path"]}});
        if (r.is_object()) r["format"] = format;
        return r;
      }
      auto esc = [](const std::string& s) {
        std::string o;
        for (char ch : s) o += ch == '<' ? "&lt;" : ch == '>' ? "&gt;" : ch == '&' ? "&amp;" : std::string(1, ch);
        return o;
      };
      auto b64 = [](const std::vector<std::uint8_t>& data) {
        static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string o;
        for (std::size_t i = 0; i < data.size(); i += 3) {
          const std::uint32_t v = (data[i] << 16) | (i + 1 < data.size() ? data[i + 1] << 8 : 0) | (i + 2 < data.size() ? data[i + 2] : 0);
          o += tbl[(v >> 18) & 63], o += tbl[(v >> 12) & 63];
          o += i + 1 < data.size() ? tbl[(v >> 6) & 63] : '=';
          o += i + 2 < data.size() ? tbl[v & 63] : '=';
        }
        return o;
      };
      auto json_table = [&](const Json& v) {
        std::string h;
        if (v.is_array() && !v.empty() && v[0].is_object()) {  // 객체 배열 → 열 = 키
          std::vector<std::string> keys;
          for (auto& [k, _] : v[0].items()) keys.push_back(k);
          h += "<table><tr>";
          for (const std::string& k : keys) h += "<th>" + esc(k) + "</th>";
          h += "</tr>";
          for (const Json& row : v) {
            h += "<tr>";
            for (const std::string& k : keys) h += "<td>" + esc(row.contains(k) ? (row[k].is_string() ? row[k].get<std::string>() : row[k].dump()) : "") + "</td>";
            h += "</tr>";
          }
          h += "</table>";
        } else if (v.is_object()) {
          h += "<table>";
          for (auto& [k, val] : v.items()) h += "<tr><th>" + esc(k) + "</th><td>" + esc(val.is_string() ? val.get<std::string>() : val.dump()) + "</td></tr>";
          h += "</table>";
        } else {
          h += "<pre>" + esc(v.dump(1)) + "</pre>";
        }
        return h;
      };
      std::string html = "<!doctype html><html><head><meta charset=\"utf-8\"><title>" + esc(rep.props.value("title", rep.name)) + "</title>"
                         "<style>body{font-family:sans-serif;margin:24px}table{border-collapse:collapse}td,th{border:1px solid #999;padding:2px 6px;font-size:13px}"
                         "img{border:1px solid #ccc}h2{margin-top:28px}</style></head><body>";
      html += "<h1>" + esc(rep.props.value("title", rep.name)) + "</h1><p>NASA-95 " + App::version() + "</p>";
      Json made = Json::array();
      ViewState& vs = state(a);
      for (const Json& item : rep.props.value("items", Json::array())) {
        const std::string kind = item.value("kind", std::string());
        if (item.contains("title")) html += "<h2>" + esc(item["title"].get<std::string>()) + "</h2>";
        if (kind == "text") {
          html += "<p>" + esc(item.value("text", std::string())) + "</p>";
        } else if (kind == "view") {
          const Json saved = Json{{"camera", camera_json(vs.camera)}, {"camera_set", vs.camera_set}, {"mode", vs.mode}, {"show", vs.show}, {"result", vs.result},
                                  {"clip", vs.clip}, {"transparency", vs.transparency}, {"hidden", std::vector<Id>(vs.hidden.begin(), vs.hidden.end())},
                                  {"color_by", vs.color_by}, {"symbols", vs.symbols}};
          if (item.contains("view") && !item["view"].get<std::string>().empty())
            a.commands().at("view.restore").fn(a, Json{{"name", item["view"]}});
          const int w = item.value("width", 800), h = item.value("height", 600);
          const RenderImage img = render(a, w, h).image;
          // PNG 를 메모리에 만든다: write_png 는 파일에 쓰므로 임시 파일을 거친다
          const std::filesystem::path tmp = std::filesystem::temp_directory_path() / ("nasa95_report_" + std::to_string(rep.id) + "_" + std::to_string(made.size()) + ".png");
          const std::u8string tmp_u8 = tmp.u8string();
          write_png(std::string(tmp_u8.begin(), tmp_u8.end()), img);
          std::ifstream f(tmp, std::ios::binary);
          std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
          f.close();
          std::error_code ec;
          std::filesystem::remove(tmp, ec);
          html += "<p><img width=\"" + std::to_string(w) + "\" src=\"data:image/png;base64," + b64(bytes) + "\"></p>";
          if (!vs.legend.is_null()) html += "<p>" + esc(vs.legend.dump()) + "</p>";
          // 뷰 상태를 되돌린다
          vs.saved_views["__report_tmp"] = saved;
          a.commands().at("view.restore").fn(a, Json{{"name", "__report_tmp"}});
          vs.saved_views.erase("__report_tmp");
        } else if (kind == "plot") {
          const Json d = a.commands().at("plot.data").fn(a, Json{{"id", item["plot"]}});
          // 간단한 SVG 선 그래프
          const Json& x = d["x"];
          const Json& y = d["y"];
          if (x.size() >= 2) {
            double xmin = 1e300, xmax = -1e300, ymin = 1e300, ymax = -1e300;
            for (std::size_t i = 0; i < x.size(); ++i) {
              xmin = std::min(xmin, x[i].get<double>()), xmax = std::max(xmax, x[i].get<double>());
              ymin = std::min(ymin, y[i].get<double>()), ymax = std::max(ymax, y[i].get<double>());
            }
            if (ymax == ymin) ymax = ymin + 1;
            if (xmax == xmin) xmax = xmin + 1;
            std::string pts;
            for (std::size_t i = 0; i < x.size(); ++i) {
              const double px = 50 + 500 * (x[i].get<double>() - xmin) / (xmax - xmin), py = 250 - 200 * (y[i].get<double>() - ymin) / (ymax - ymin);
              pts += std::to_string(px) + "," + std::to_string(py) + " ";
            }
            html += "<svg width=\"600\" height=\"300\" style=\"border:1px solid #ccc\"><polyline fill=\"none\" stroke=\"#1f4e9c\" stroke-width=\"2\" points=\"" + pts + "\"/>"
                    "<text x=\"300\" y=\"290\" font-size=\"12\" text-anchor=\"middle\">" + esc(d["x_label"].get<std::string>()) + " [" + format_value(xmin) + " .. " + format_value(xmax) + "]</text>"
                    "<text x=\"12\" y=\"150\" font-size=\"12\" transform=\"rotate(-90 12 150)\" text-anchor=\"middle\">" + esc(d["y_label"].get<std::string>()) + " [" + format_value(ymin) + " .. " + format_value(ymax) + "]</text></svg>";
          }
          html += json_table(d["rows"]);
        } else if (kind == "table") {
          const std::string cmd = item.value("command", std::string());
          if (!a.commands().count(cmd)) throw Error("unknown_command", "등록되지 않은 명령: " + cmd, {{"param", "command"}});
          if (a.commands().at(cmd).kind != 'Q') throw Error("invalid_param", "table 항목에는 조회(Q) 명령만 쓴다: " + cmd, {{"param", "command"}});
          html += json_table(a.commands().at(cmd).fn(a, item.value("params", Json::object())));
        }
        made.push_back(kind);
      }
      html += "</body></html>\n";
      const std::string path = p["path"].get<std::string>();
      std::ofstream out(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
      if (!out) throw Error("io_error", "파일을 쓸 수 없습니다: " + path, {{"path", path}});
      out << html;
      return Json{{"path", path}, {"items", made}, {"bytes", html.size()}};
    };
    app.register_command(std::move(c));
  }
  {
    // 클리핑 평면(RND-19): view.clip 은 평면 하나로 바꾸는(또는 모두 끄는) 지름길, clip_add/update/remove 는 여러 평면(최대 8개) 관리
    auto plane_params = [] {
      return Fields{F("point", "vector3", "평면 위의 점").unit("length").ex({0.0, 0.0, 0.0}), F("normal", "vector3", "평면의 법선(남길 쪽)").ex({0.0, 0.0, 1.0})};
    };
    auto check_plane = [](App& a, const char* cmd, const Json& p, bool required) {
      if (required && (!has(p, "point") || !has(p, "normal"))) throw Error("missing_param", "point 와 normal 을 함께 적는다", {{"param", "normal"}});
      for (const FieldSpec& f : a.commands().at(cmd).params)
        if (has(p, f.name.c_str()) && (f.name == "point" || f.name == "normal")) check_value(f, p[f.name], nullptr);
      if (has(p, "normal") && norm(v3(p["normal"])) == 0.0) throw Error("out_of_range", "법선의 길이가 0 입니다", {{"param", "normal"}});
    };
    auto find_plane = [](ViewState& vs, int id) -> Json& {
      for (Json& c : vs.clip)
        if (c["id"].get<int>() == id) return c;
      throw Error("not_found", "없는 클리핑 평면 번호입니다: " + std::to_string(id), {{"param", "id"}});
    };
    auto summary = [](const ViewState& vs) {
      int enabled = 0;
      for (const Json& c : vs.clip) enabled += c.value("enabled", true) ? 1 : 0;
      return Json{{"enabled", enabled > 0}, {"count", vs.clip.size()}, {"active", enabled}, {"planes", vs.clip}};
    };
    {
      CommandSpec c = base("view.clip", 'V', "클리핑 평면을 하나만 둔다(법선이 가리키는 쪽을 남긴다. 있던 평면은 모두 지운다). 매개변수 없이 부르면 모두 끈다", "RND-19");
      c.params = plane_params();
      c.fn = [check_plane, summary](App& a, const Json& p) {
        ViewState& vs = state(a);
        if (!has(p, "point") && !has(p, "normal")) {
          vs.clip = Json::array();
          return summary(vs);
        }
        check_plane(a, "view.clip", p, true);
        vs.clip = Json::array({Json{{"id", vs.next_clip++}, {"point", p["point"]}, {"normal", p["normal"]}, {"enabled", true}}});
        Json r = summary(vs);
        r["plane"] = vs.clip[0];
        return r;
      };
      app.register_command(std::move(c));
    }
    {
      CommandSpec c = base("view.clip_add", 'V', "클리핑 평면을 더한다(최대 8개). 돌려주는 id 로 고치거나 지운다", "RND-19");
      c.params = plane_params();
      c.fn = [check_plane, summary](App& a, const Json& p) {
        ViewState& vs = state(a);
        check_plane(a, "view.clip_add", p, true);
        if (static_cast<int>(vs.clip.size()) >= kMaxClipPlanes)
          throw Error("out_of_range", "클리핑 평면은 최대 " + std::to_string(kMaxClipPlanes) + "개입니다", {{"count", vs.clip.size()}});
        const int id = vs.next_clip++;
        vs.clip.push_back(Json{{"id", id}, {"point", p["point"]}, {"normal", p["normal"]}, {"enabled", true}});
        Json r = summary(vs);
        r["id"] = id;
        return r;
      };
      app.register_command(std::move(c));
    }
    {
      CommandSpec c = base("view.clip_update", 'V', "클리핑 평면의 점·법선을 바꾸거나 켜고 끈다(enabled)", "RND-19");
      c.params = plane_params();
      c.params.insert(c.params.begin(), F("id", "integer", "평면 번호").call_req().ge(1).ex(1));
      c.params.push_back(F("enabled", "bool", "켬·끔"));
      c.fn = [check_plane, find_plane, summary](App& a, const Json& p) {
        ViewState& vs = state(a);
        check_plane(a, "view.clip_update", p, false);
        Json& plane = find_plane(vs, p["id"].get<int>());
        for (const char* key : {"point", "normal", "enabled"})
          if (has(p, key)) plane[key] = p[key];
        Json r = summary(vs);
        r["plane"] = plane;
        return r;
      };
      app.register_command(std::move(c));
    }
    {
      CommandSpec c = base("view.clip_remove", 'V', "클리핑 평면을 지운다(id 가 없으면 모두)", "RND-19");
      c.params = {F("id", "integer", "평면 번호").ge(1).ex(1)};
      c.fn = [find_plane, summary](App& a, const Json& p) {
        ViewState& vs = state(a);
        if (!has(p, "id")) {
          vs.clip = Json::array();
          return summary(vs);
        }
        find_plane(vs, p["id"].get<int>());
        Json kept = Json::array();
        for (const Json& c : vs.clip)
          if (c["id"] != p["id"]) kept.push_back(c);
        vs.clip = kept;
        return summary(vs);
      };
      app.register_command(std::move(c));
    }
  }
  {
    // 범례(RES-14): 범위(min·max 는 결과 표시 설정에 둔다)·단계 수·색상표·범위 밖 처리
    CommandSpec c = base("view.legend", 'V', "범례의 범위(min·max, 비우면 자동)·단계 수(levels, 0 = 연속)·색상표(colormap)·범위 밖 색(out_of_range: clamp|gray)을 지정한다. 매개변수 없이 부르면 기본으로", "RES-14");
    c.params = {F("min", "number", "최솟값(없으면 자동)"), F("max", "number", "최댓값(없으면 자동)"), F("levels", "integer", "단계 수(0 = 연속)").ge(0).le(64),
                F("colormap", "string", "색상표").one_of(kColormaps), F("out_of_range", "string", "범위 밖 값의 색").one_of({"clamp", "gray"})};
    c.fn = [](App& a, const Json& p) {
      ViewState& vs = state(a);
      for (const FieldSpec& f : a.commands().at("view.legend").params)
        if (has(p, f.name.c_str())) check_value(f, p[f.name], nullptr);
      if (!has(p, "min") && !has(p, "max") && !has(p, "levels") && !has(p, "colormap") && !has(p, "out_of_range")) {
        vs.legend_opts = Json::object();
        if (!vs.result.is_null()) vs.result.erase("min"), vs.result.erase("max");
      }
      for (const char* key : {"min", "max"})
        if (p.contains(key)) {
          if (vs.result.is_null()) throw Error("invalid_state", "결과 표시(view.result_show)가 켜져 있어야 범위를 둘 수 있습니다", {{"param", key}});
          if (p[key].is_null()) vs.result.erase(key);
          else vs.result[key] = p[key];
        }
      if (has(vs.result, "min") && has(vs.result, "max") && vs.result["min"].get<double>() > vs.result["max"].get<double>())
        throw Error("out_of_range", "min 이 max 보다 큽니다", {{"param", "min"}});
      for (const char* key : {"levels", "colormap", "out_of_range"})
        if (p.contains(key)) {
          if (p[key].is_null()) vs.legend_opts.erase(key);
          else vs.legend_opts[key] = p[key];
        }
      Json r = vs.legend_opts;
      r["min"] = vs.result.is_null() ? Json() : vs.result.value("min", Json());
      r["max"] = vs.result.is_null() ? Json() : vs.result.value("max", Json());
      r["colormaps"] = kColormaps;
      return r;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.result_filter", 'V', "결과 표시 영역을 제한한다: target 의 요소에만 컨투어를 입히고(밖은 바탕색), 값이 min~max 밖인 곳은 회색. 매개변수 없이 부르면 끈다", "RES-16");
    c.params = {F("target", "target", "컨투어를 입힐 요소").one_of({"elements", "set", "parts", "geometry"}), F("min", "number", "이 값보다 작으면 회색"),
                F("max", "number", "이 값보다 크면 회색")};
    c.fn = [](App& a, const Json& p) {
      ViewState& vs = state(a);
      if (!has(p, "target") && !has(p, "min") && !has(p, "max")) {
        vs.result_filter = nullptr;
        return Json{{"enabled", false}};
      }
      if (has(p, "target")) resolve_target(a, p["target"], "elements");  // 대상이 풀리는지 지금 확인한다
      Json f = Json::object();
      for (const char* key : {"target", "min", "max"})
        if (has(p, key)) f[key] = p[key];
      vs.result_filter = f;
      Json r = f;
      r["enabled"] = true;
      return r;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.set_appearance", 'V', "객체(형상 파트·메시 파트·재료·프로퍼티)의 색(0~255 RGB)과 불투명도(alpha 0~1, 파트만)를 지정한다. 둘 다 비우면 그 객체의 지정을 지운다. 재료·프로퍼티 색은 view.color_by material·property 에서 쓴다. 컨투어 색은 그대로다", "WT-25, RND-16, MSH-24, PRP-13");
    c.params = {F("id", "ref", "객체(part·mesh_part·material·property)").call_req(), F("color", "integer_list", "[R, G, B] (0~255)").ge(0).le(255).ex({200, 60, 60}),
                F("alpha", "number", "불투명도(0~1)").gt(0).le(1)};
    c.fn = [](App& a, const Json& p) {
      ViewState& vs = state(a);
      const Object& o = a.model().get(p["id"].get<Id>());
      if (o.kind != "part" && o.kind != "mesh_part" && o.kind != "material" && o.kind != "property")
        throw Error("wrong_kind", "형상 파트·메시 파트·재료·프로퍼티만 모양을 지정할 수 있습니다", {{"object", o.id}, {"kind", o.kind}});
      if ((o.kind == "material" || o.kind == "property") && has(p, "alpha"))
        throw Error("invalid_param", "재료·프로퍼티에는 색만 지정합니다(불투명도는 형상·메시 파트에)", {{"param", "alpha"}});
      const std::string key = std::to_string(o.id);
      if (!has(p, "color") && !has(p, "alpha")) {
        vs.appearance.erase(key);
        return Json{{"id", o.id}, {"cleared", true}};
      }
      Json ap = vs.appearance.value(key, Json::object());
      if (has(p, "color")) {
        if (p["color"].size() != 3) throw Error("invalid_param", "색은 [R, G, B] 세 값입니다", {{"param", "color"}});
        check_value(a.commands().at("view.set_appearance").params[1], p["color"], nullptr);
        ap["color"] = p["color"];
      }
      if (has(p, "alpha")) check_value(a.commands().at("view.set_appearance").params[2], p["alpha"], nullptr), ap["alpha"] = p["alpha"];
      vs.appearance[key] = ap;
      return Json{{"id", o.id}, {"appearance", ap}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.mesh_options", 'V',
                         "메시 표시 옵션: 요소 경계선(edges), 요소 축소(shrink 0~0.9), 1D/2D 입체 표시(solid_1d_2d: 쉘·멤브레인·복합재는 프로퍼티 두께(오프셋 반영)의 프리즘, "
                         "보·트러스는 단면 치수(rect·circ·box 는 a×b, pipe 는 지름 상자, 트러스는 √A 정사각형)의 상자. 단면을 모르면 선). 매개변수 없이 부르면 기본으로. 2차 곡면 표시는 미구현",
                         "RND-20, RND-21, RND-22");
    c.params = {F("edges", "bool", "요소 경계선을 그린다(기본 켬)"), F("shrink", "number", "요소를 중심 쪽으로 줄이는 비율(0 = 그대로)").ge(0).le(0.9),
                F("solid_1d_2d", "bool", "보·쉘을 두께·단면이 있는 입체로 그린다(기본 끔)"),
                F("beam_axes", "bool", "보 요소마다 단면 1축 방향 표식(요소 중앙에서 1축 쪽 주황 선, 길이 = 단면 높이의 반 또는 요소 길이의 15%)을 그린다(기본 끔, PRP-07)")};
    auto options = [](const ViewState& vs) {
      return Json{{"edges", vs.mesh_options.value("edges", true)}, {"shrink", vs.mesh_options.value("shrink", 0.0)},
                  {"solid_1d_2d", vs.mesh_options.value("solid_1d_2d", false)}, {"beam_axes", vs.mesh_options.value("beam_axes", false)}};
    };
    c.fn = [options](App& a, const Json& p) {
      ViewState& vs = state(a);
      if (!has(p, "edges") && !has(p, "shrink") && !has(p, "solid_1d_2d") && !has(p, "beam_axes")) vs.mesh_options = Json::object();
      for (const FieldSpec& f : a.commands().at("view.mesh_options").params)
        if (has(p, f.name.c_str())) check_value(f, p[f.name], nullptr), vs.mesh_options[f.name] = p[f.name];
      return options(vs);
    };
    app.register_command(std::move(c));
    CommandSpec g = base("view.mesh_options_get", 'Q', "메시 표시 옵션을 초기화하지 않고 조회한다", "RND-20, RND-21, RND-22");
    g.fn = [options](App& a, const Json&) { return options(state(a)); };
    app.register_command(std::move(g));
  }
  {
    CommandSpec g = base("view.tree_state_get", 'V', "워크 트리의 펼침·접힘 상태(UI 가 저장한 값)를 읽는다", "WT-36");
    g.fn = [](App& a, const Json&) { return state(a).tree_state; };
    app.register_command(std::move(g));
    CommandSpec c = base("view.tree_state_set", 'V', "워크 트리의 펼침·접힘 상태를 저장한다(가지·객체 이름 → 펼침 여부). UI 가 창을 다시 열 때 복원한다", "WT-36");
    c.params = {F("state", "object", "펼침 상태(이름 → bool)").call_req().ex(Json{{"parts", true}})};
    c.fn = [](App& a, const Json& p) {
      if (!p["state"].is_object()) throw Error("invalid_param_type", "state 는 객체여야 합니다", {{"param", "state"}});
      state(a).tree_state = p["state"];
      return state(a).tree_state;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.overlay", 'V', "화면 요소: 좌표축(triad), 눈금자(ruler: 장면 크기에 맞춘 1·2·5 단위 길이 막대), 배경색(background [R,G,B] 0~255). 매개변수 없이 부르면 기본으로", "RND-37");
    c.params = {F("triad", "bool", "좌표축(기본 켬)"), F("ruler", "bool", "눈금자(기본 끔)"), F("background", "integer_list", "배경색 [R, G, B]").ge(0).le(255).ex({255, 255, 255})};
    c.fn = [](App& a, const Json& p) {
      ViewState& vs = state(a);
      if (!has(p, "triad") && !has(p, "ruler") && !has(p, "background")) vs.overlay = Json::object();  // 좌표축 설정(view.hud)은 그대로
      if (has(p, "triad")) vs.hud["triad"] = p["triad"];
      if (has(p, "ruler")) vs.overlay["ruler"] = p["ruler"];
      if (has(p, "background")) {
        if (p["background"].size() != 3) throw Error("invalid_param", "배경색은 [R, G, B] 세 값입니다", {{"param", "background"}});
        check_value(a.commands().at("view.overlay").params[2], p["background"], nullptr);
        vs.overlay["background"] = p["background"];
      }
      return Json{{"triad", vs.hud.value("triad", true)}, {"ruler", vs.overlay.value("ruler", false)},
                  {"background", vs.overlay.value("background", Json::array({255, 255, 255}))}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.quality", 'V',
                         "그리기 품질: antialiasing(none | ssaa2 — 창과 저장 이미지를 두 배로 그려 줄임), transparency(unsorted | sorted — 투명 면을 먼 것부터), "
                         "simplify_during_interaction(UI 가 끌기 중에 와이어프레임으로 그림), gpu_memory_limit(바이트). 매개변수 없이 부르면 기본으로",
                         "RND-17, RND-18, RND-42, RND-44");
    c.params = {F("antialiasing", "string", "안티에일리어싱").one_of({"none", "ssaa2"}), F("transparency", "string", "투명 합성").one_of({"unsorted", "sorted"}),
                F("simplify_during_interaction", "bool", "조작 중 간소화"),
                F("gpu_memory_limit", "integer", "GPU 메모리 한도(바이트). view.diagnostics 의 gpu_memory 에 near_limit(80%)·over_limit 로 드러난다").ge(0)};
    c.fn = [](App& a, const Json& p) {
      ViewState& vs = state(a);
      if (!has(p, "antialiasing") && !has(p, "transparency") && !has(p, "simplify_during_interaction") && !has(p, "gpu_memory_limit")) vs.quality = Json::object();
      for (const FieldSpec& f : a.commands().at("view.quality").params)
        if (has(p, f.name.c_str())) check_value(f, p[f.name], nullptr), vs.quality[f.name] = p[f.name];
      return a.commands().at("view.quality_get").fn(a, Json::object());
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.quality_get", 'Q', "현재 렌더링 품질 설정을 변경 없이 조회한다", "RND-17, RND-18, RND-42, RND-44");
    c.fn = [](App& a, const Json&) {
      const ViewState& vs = state(a);
      Json out{{"antialiasing", vs.quality.value("antialiasing", std::string("none"))},
               {"transparency", vs.quality.value("transparency", std::string("unsorted"))},
               {"simplify_during_interaction", vs.quality.value("simplify_during_interaction", false)}};
      if (vs.quality.contains("gpu_memory_limit")) out["gpu_memory_limit"] = vs.quality["gpu_memory_limit"];
      return out;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.result_options", 'V',
                         "결과 표시 옵션: undeformed(변형 표시 중에 원래 자리의 요소 모서리를 연한 회색으로), vectors(벡터 결과를 화살표로: field, scale — 없으면 장면 크기의 15%). "
                         "매개변수 없이 부르면 기본으로. 평균화·쉘/보 입체 표시는 미구현(frd 값은 이미 절점 값)",
                         "RES-07, RES-10, RND-30, RND-32");
    c.params = {F("undeformed", "bool", "미변형 윤곽"), F("vectors", "string", "화살표로 그릴 벡터 결과 종류(예: DISP). 비우면 끔"),
                F("vector_scale", "number", "화살표 배율(없으면 자동)").gt(0)};
    c.fn = [](App& a, const Json& p) {
      ViewState& vs = state(a);
      if (!has(p, "undeformed") && !p.contains("vectors") && !has(p, "vector_scale")) vs.result_options = Json::object();
      if (has(p, "undeformed")) vs.result_options["undeformed"] = p["undeformed"];
      if (p.contains("vectors")) {
        if (p["vectors"].is_null() || p["vectors"].get<std::string>().empty()) {
          vs.result_options.erase("vectors");
        } else {
          if (vs.result.is_null()) throw Error("invalid_state", "결과 표시(view.result_show)가 켜져 있어야 벡터를 그릴 수 있습니다", {{"param", "vectors"}});
          ResultFile& file = results(a).get(vs.result["result"].get<Id>());
          const ResultField& f = file.field(file.frame(vs.result["frame"].get<int>()), p["vectors"].get<std::string>());
          if (f.components.size() < 3) throw Error("invalid_param", "벡터 결과가 아닙니다: " + f.name, {{"param", "vectors"}});
          Json v{{"field", p["vectors"]}};
          if (has(p, "vector_scale")) v["scale"] = p["vector_scale"];
          vs.result_options["vectors"] = v;
        }
      } else if (has(p, "vector_scale") && has(vs.result_options, "vectors")) {
        vs.result_options["vectors"]["scale"] = p["vector_scale"];
      }
      return vs.result_options;
    };
    app.register_command(std::move(c));
  }
  {
    // 적용 대상 표시(WT-24): 하중·경계조건·프로퍼티·구속·접촉의 대상(면·요소·노드)을 강조한다
    CommandSpec c = base("view.show_targets", 'V', "객체(하중·경계조건·프로퍼티·구속·접촉 쌍 등)의 적용 대상을 강조한다: 면·요소는 밝은 색으로 덧그리고 노드는 표식으로. ids 를 비우면 해제", "WT-24");
    c.params = {F("ids", "ref_list", "대상을 보일 객체들").ex({1})};
    c.fn = [](App& a, const Json& p) {
      ViewState& vs = state(a);
      vs.highlighted.clear(), vs.target_nodes.clear();
      Json shown = Json::array();
      if (!has(p, "ids") || p["ids"].empty()) return Json{{"faces", 0}, {"nodes", 0}, {"objects", shown}};
      const Mesh& m = a.mesh();
      std::set<std::pair<Id, int>> faces;
      std::set<Id> nodes;
      for (const Json& idj : p["ids"]) {
        const Object& o = a.model().get(idj.get<Id>());
        for (const char* key : {"target", "slave", "master", "surface"}) {
          if (!has(o.props, key) || !o.props[key].is_object()) continue;
          const Json& t = o.props[key];
          bool done = false;
          for (const char* what : {"faces", "elements", "nodes"}) {
            Json r;
            try {
              r = resolve_target(a, t, what);
            } catch (const Error&) {
              continue;
            }
            if (std::string(what) == "faces") {
              for (const Json& f : r) faces.insert({f[0].get<Id>(), f[1].get<int>()});
            } else if (std::string(what) == "elements") {
              // 요소: 바깥 면(요소 하나만 가진 면)을 강조한다. 2D 요소는 요소 자체
              std::map<std::vector<Id>, std::pair<Id, int>> seen;
              std::set<std::vector<Id>> dup;
              for (const Json& e : r) {
                const Element el = m.element(e.get<Id>());
                if (shape_info(el.shape).dim == 2) faces.insert({el.id, 0});
                if (shape_info(el.shape).dim != 3) continue;
                const auto& table = shape_faces(el.shape);
                for (std::size_t f = 0; f < table.size(); ++f) {
                  std::vector<Id> key;
                  for (int k : table[f]) key.push_back(el.nodes[static_cast<std::size_t>(k)]);
                  std::sort(key.begin(), key.end());
                  if (seen.count(key)) dup.insert(key);
                  else seen[key] = {el.id, static_cast<int>(f) + 1};
                }
              }
              for (const auto& [key, ef] : seen)
                if (!dup.count(key)) faces.insert(ef);
            } else {
              for (const Json& n : r) nodes.insert(n.get<Id>());
            }
            done = true;
            break;
          }
          if (done) shown.push_back(Json{{"id", o.id}, {"field", key}});
        }
      }
      for (const auto& [e, f] : faces) {
        PickRecord rec;
        rec.kind = f == 0 ? "element" : "element_face", rec.a = e, rec.b = f;
        vs.highlighted.push_back(rec);
      }
      vs.target_nodes.assign(nodes.begin(), nodes.end());
      return Json{{"faces", faces.size()}, {"nodes", nodes.size()}, {"objects", shown}};
    };
    app.register_command(std::move(c));
  }
  {
    // 영역 선택(RND-25): 박스·다각형·원 안의 객체. visible 은 그린 ID 버퍼에서, all 은 대표점을 투영해(가려진 것 포함) 고른다
    CommandSpec c = base("view.pick_region", 'Q',
                         "화면 영역(box: [x0,y0,x1,y1], circle: [cx,cy,r], polygon: [[x,y],…] 픽셀) 안의 객체를 조회한다. mode=visible 은 보이는 것만(ID 버퍼), "
                         "all 은 가려진 것도(요소 중심·면 중심을 투영). 선택 필터를 따른다",
                         "RND-25");
    c.params = {F("shape", "string", "영역 모양").call_req().one_of({"box", "circle", "polygon"}).ex("box"),
                F("points", "number_list", "box: [x0, y0, x1, y1], circle: [cx, cy, r], polygon: [x0, y0, x1, y1, …]").call_req().ex({0.0, 0.0, 100.0, 100.0}),
                F("mode", "string", "visible(기본) | all").one_of({"visible", "all"}), width_param(), height_param()};
    c.fn = [](App& a, const Json& p) {
      ViewState& vs = state(a);
      int w, h;
      size_of(a, "view.pick_region", p, w, h);
      check_value(a.commands().at("view.pick_region").params[0], p["shape"], nullptr);
      const std::string shape = p["shape"].get<std::string>();
      std::vector<double> pts;
      for (const Json& v : p["points"]) pts.push_back(v.get<double>());
      if ((shape == "box" && pts.size() != 4) || (shape == "circle" && pts.size() != 3) || (shape == "polygon" && (pts.size() < 6 || pts.size() % 2)))
        throw Error("invalid_param", "points 의 개수가 모양에 맞지 않습니다", {{"param", "points"}, {"shape", shape}});
      auto inside = [&](double x, double y) {
        if (shape == "box") return x >= std::min(pts[0], pts[2]) && x <= std::max(pts[0], pts[2]) && y >= std::min(pts[1], pts[3]) && y <= std::max(pts[1], pts[3]);
        if (shape == "circle") return std::hypot(x - pts[0], y - pts[1]) <= pts[2];
        bool in = false;  // 짝수-홀수 규칙
        const std::size_t n = pts.size() / 2;
        for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
          const double xi = pts[2 * i], yi = pts[2 * i + 1], xj = pts[2 * j], yj = pts[2 * j + 1];
          if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) in = !in;
        }
        return in;
      };
      const Rendered r = render(a, w, h);
      std::set<std::uint32_t> ids;
      if (p.value("mode", std::string("visible")) == "visible") {
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x) {
            const std::uint32_t id = r.image.ids[static_cast<std::size_t>(y) * w + x];
            if (id && inside(x + 0.5, y + 0.5)) ids.insert(id);
          }
      } else {
        // 대표점: 요소 중심, 요소면 중심, 형상 면은 그 면 삼각형 꼭짓점의 평균. 그린 좌표(변형 표시 반영)
        float view[16], mvp[16];
        matrices(vs, r.built, w, h, mvp, view);
        std::unordered_map<std::uint32_t, std::pair<V3, int>> acc;
        for (std::size_t k = 0; k < r.built.scene.triangles.size(); ++k) {
          const RenderVertex& v = r.built.scene.triangles[k];
          if (!v.id) continue;
          auto& e = acc[v.id];
          e.first = add(e.first, V3{v.pos[0], v.pos[1], v.pos[2]}), ++e.second;
        }
        for (std::size_t k = 0; k < r.built.scene.transparent.size(); ++k) {
          const RenderVertex& v = r.built.scene.transparent[k];
          if (!v.id) continue;
          auto& e = acc[v.id];
          e.first = add(e.first, V3{v.pos[0], v.pos[1], v.pos[2]}), ++e.second;
        }
        for (const auto& [id, e] : acc) {
          const V3 c = mul(e.first, 1.0 / e.second);
          double q[4] = {0, 0, 0, 0};
          for (int row = 0; row < 4; ++row) q[row] = mvp[row] * c[0] + mvp[4 + row] * c[1] + mvp[8 + row] * c[2] + mvp[12 + row];
          if (q[3] <= 1e-12) continue;
          if (inside((q[0] / q[3] * 0.5 + 0.5) * w, (q[1] / q[3] * 0.5 + 0.5) * h)) ids.insert(id);
        }
      }
      Json hits = Json::array();
      for (std::uint32_t id : ids) {
        Json j = pick_json(vs, id);
        if (j.value("hit", false)) j.erase("hit"), hits.push_back(j);
      }
      return Json{{"count", hits.size()}, {"hits", hits}, {"mode", p.value("mode", std::string("visible"))}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.layout", 'V', "뷰포트 분할(오프스크린 이미지 합성): rows×cols 칸마다 표준 뷰 이름(front·iso …) 또는 저장한 뷰 이름을 둔다(없으면 현재 카메라). 매개변수 없이 부르면 하나로. 창 표시는 첫 칸(현재 뷰)만 그린다", "RND-03");
    c.params = {F("rows", "integer", "행 수").ge(1).le(4).ex(1), F("cols", "integer", "열 수").ge(1).le(4).ex(2),
                F("views", "string_list", "칸마다의 뷰 이름(행 우선, 빈 글은 현재 카메라)").ex({"front", "iso"})};
    c.fn = [](App& a, const Json& p) {
      ViewState& vs = state(a);
      if (!has(p, "rows") && !has(p, "cols") && !has(p, "views")) {
        vs.layout = nullptr;
        return Json{{"rows", 1}, {"cols", 1}, {"cells", Json::array()}};
      }
      for (const FieldSpec& f : a.commands().at("view.layout").params)
        if (has(p, f.name.c_str())) check_value(f, p[f.name], nullptr);
      const int rows = p.value("rows", 1), cols = p.value("cols", 1);
      Json cells = Json::array();
      static const std::set<std::string> standard = {"front", "back", "left", "right", "top", "bottom", "iso"};
      for (const Json& v : p.value("views", Json::array())) {
        const std::string name = v.get<std::string>();
        if (!name.empty() && !standard.count(name) && !vs.saved_views.contains(name))
          throw Error("not_found", "표준 뷰도 저장한 뷰도 아닙니다: " + name, {{"param", "views"}});
        cells.push_back(Json{{"view", name}});
      }
      if (static_cast<int>(cells.size()) > rows * cols) throw Error("out_of_range", "칸 수보다 뷰가 많습니다", {{"param", "views"}});
      vs.layout = Json{{"rows", rows}, {"cols", cols}, {"cells", cells}};
      return vs.layout;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.beam_diagram", 'V', "보 단면력 선도: 결과 표시 중인 프레임의 STRESS(SECTION FORCES)에서 quantity 를 읽어 direction 쪽으로 scale 배 띄운 꺾은선을 그린다. 매개변수 없이 끈다", "RES-55");
    c.params = {F("quantity", "string", "단면력").one_of({"normal_force", "shear_1", "shear_2", "torque", "moment_1", "moment_2"}).ex("moment_1"),
                F("scale", "number", "값에 곱할 길이 배율(없으면 장면 크기의 15%)").gt(0), F("direction", "vector3", "선도를 띄우는 방향(기본 z)").ex({0.0, 0.0, 1.0})};
    c.fn = [](App& a, const Json& p) {
      ViewState& vs = state(a);
      if (!has(p, "quantity")) {
        vs.beam_diagram = nullptr;
        return Json{{"enabled", false}};
      }
      if (vs.result.is_null()) throw Error("invalid_state", "결과 표시(view.result_show)가 켜져 있어야 선도를 그릴 수 있습니다", {{"param", "quantity"}});
      for (const FieldSpec& f : a.commands().at("view.beam_diagram").params)
        if (has(p, f.name.c_str())) check_value(f, p[f.name], nullptr);
      ResultFile& file = results(a).get(vs.result["result"].get<Id>());
      file.field(file.frame(vs.result["frame"].get<int>()), "STRESS");  // 없으면 Error
      Json d{{"quantity", p["quantity"]}};
      if (has(p, "scale")) d["scale"] = p["scale"];
      if (has(p, "direction")) {
        if (norm(v3(p["direction"])) == 0.0) throw Error("out_of_range", "방향의 길이가 0 입니다", {{"param", "direction"}});
        d["direction"] = p["direction"];
      }
      vs.beam_diagram = d;
      Json r = d;
      r["enabled"] = true;
      return r;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.expand_cyclic", 'V', "순환대칭 전개 표시: 메시(와 결과 색)를 축(point·axis) 둘레로 sectors 개 복사해 보인다. constraint 로 순환대칭 구속 객체를 주면 그 축·섹터 수를 쓴다. 매개변수 없이 끈다", "RES-50");
    c.params = {F("sectors", "integer", "섹터 수").ge(2).ex(12), F("point", "vector3", "축 위의 점").ex({0.0, 0.0, 0.0}), F("axis", "vector3", "축 방향").ex({0.0, 0.0, 1.0}),
                F("constraint", "ref", "순환대칭 구속(cyclic_symmetry)").ref("constraint")};
    c.fn = [](App& a, const Json& p) {
      ViewState& vs = state(a);
      if (!has(p, "sectors") && !has(p, "constraint")) {
        vs.expand = nullptr;
        return Json{{"enabled", false}};
      }
      Json e{{"kind", "cyclic"}};
      if (has(p, "constraint")) {
        const Object& o = a.model().get(p["constraint"].get<Id>());
        if (o.kind != "constraint" || o.props.value("type", std::string()) != "cyclic_symmetry")
          throw Error("wrong_kind", "순환대칭 구속이 아닙니다", {{"object", o.id}});
        e["sectors"] = o.props.value("sectors", 2);
        const V3 pa = v3(o.props["axis_point_a"]), pb = v3(o.props["axis_point_b"]);
        e["point"] = o.props["axis_point_a"], e["axis"] = Json::array({pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]});
      }
      for (const char* key : {"sectors", "point", "axis"})
        if (has(p, key)) e[key] = p[key];
      if (!has(e, "point") || !has(e, "axis")) throw Error("missing_param", "축(point·axis)이 필요합니다", {{"param", "axis"}});
      if (norm(v3(e["axis"])) == 0.0) throw Error("out_of_range", "축 방향의 길이가 0 입니다", {{"param", "axis"}});
      vs.expand = e;
      Json r = e;
      r["enabled"] = true;
      return r;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.expand_axisymmetric", 'V', "축대칭 전개 표시: 2D 요소(x 반지름, y 축 — 솔버 규약)의 자유 변을 y 축 둘레로 segments 개로 돌려 겉면을 그린다(각도 angle, 기본 360). 매개변수 없이 끈다", "RES-56");
    c.params = {F("segments", "integer", "둘레 분할 수(기본 36)").ge(3).ex(36), F("angle", "number", "전개 각도(도)").gt(0).le(360).ex(360.0)};
    c.fn = [](App& a, const Json& p) {
      ViewState& vs = state(a);
      if (!has(p, "segments") && !has(p, "angle")) {
        vs.expand = nullptr;
        return Json{{"enabled", false}};
      }
      for (const FieldSpec& f : a.commands().at("view.expand_axisymmetric").params)
        if (has(p, f.name.c_str())) check_value(f, p[f.name], nullptr);
      Json e{{"kind", "axisymmetric"}, {"segments", p.value("segments", 36)}, {"angle", p.value("angle", 360.0)}};
      vs.expand = e;
      Json r = e;
      r["enabled"] = true;
      return r;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.section_result", 'V', "단면 결과: 3D 요소를 평면(point·normal)으로 자른 단면 다각형을 결과 색(결과 표시가 없으면 밝은 회색)으로 그린다. 클리핑과 함께 쓰면 잘린 면이 채워진다. 매개변수 없이 끈다", "RES-15");
    c.params = {F("point", "vector3", "평면 위의 점").unit("length").ex({0.0, 0.0, 0.0}), F("normal", "vector3", "평면의 법선").ex({1.0, 0.0, 0.0})};
    c.fn = [](App& a, const Json& p) {
      ViewState& vs = state(a);
      if (!has(p, "point") && !has(p, "normal")) {
        vs.section = nullptr;
        return Json{{"enabled", false}};
      }
      if (!has(p, "point") || !has(p, "normal")) throw Error("missing_param", "point 와 normal 을 함께 적는다", {{"param", "normal"}});
      if (norm(v3(p["normal"])) == 0.0) throw Error("out_of_range", "법선의 길이가 0 입니다", {{"param", "normal"}});
      vs.section = Json{{"point", p["point"]}, {"normal", p["normal"]}};
      return Json{{"enabled", true}, {"point", p["point"]}, {"normal", p["normal"]}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.iso_surface", 'V', "등가면: 결과 표시 중인 성분이 values 인 면을 사면체 분할 마칭으로 그린다(색은 그 값의 범례 색). 매개변수 없이 끈다", "RES-41");
    c.params = {F("values", "number_list", "등가면 값(여러 개)").ex({1.0})};
    c.fn = [](App& a, const Json& p) {
      ViewState& vs = state(a);
      if (!has(p, "values") || p["values"].empty()) {
        vs.iso = nullptr;
        return Json{{"enabled", false}};
      }
      if (vs.result.is_null() || !has(vs.result, "field")) throw Error("invalid_state", "결과 성분 표시(view.result_show field·component)가 켜져 있어야 등가면을 그릴 수 있습니다", {{"param", "values"}});
      vs.iso = Json{{"values", p["values"]}};
      return Json{{"enabled", true}, {"values", p["values"]}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.streamlines", 'V', "유선: 결과 표시 중인 프레임의 벡터 결과(field, 기본 DISP)를 seeds 에서 시작해 RK2 로 적분한 꺾은선(크기로 색). 매개변수 없이 끈다", "RND-33");
    c.params = {F("seeds", "table", "시작점들 [x, y, z]").columns(3).unit("length").ex(Json::array({Json::array({0.0, 0.0, 0.0})})),
                F("field", "string", "벡터 결과 종류(기본 DISP)").ex("DISP"), F("steps", "integer", "최대 걸음 수(기본 200)").ge(1).le(100000),
                F("step_size", "number", "걸음 길이(없으면 장면 크기의 1%)").gt(0).unit("length"),
                F("direction", "string", "적분 방향(기본 forward)").one_of({"forward", "backward", "both"})};
    c.fn = [](App& a, const Json& p) {
      ViewState& vs = state(a);
      if (!has(p, "seeds") || p["seeds"].empty()) {
        vs.streamlines = nullptr;
        return Json{{"enabled", false}};
      }
      if (vs.result.is_null()) throw Error("invalid_state", "결과 표시(view.result_show)가 켜져 있어야 유선을 그릴 수 있습니다", {{"param", "seeds"}});
      for (const FieldSpec& f : a.commands().at("view.streamlines").params)
        if (has(p, f.name.c_str())) check_value(f, p[f.name], nullptr);
      ResultFile& file = results(a).get(vs.result["result"].get<Id>());
      const ResultField& f = file.field(file.frame(vs.result["frame"].get<int>()), p.value("field", std::string("DISP")));
      if (f.components.size() < 3) throw Error("invalid_param", "벡터 결과가 아닙니다: " + f.name, {{"param", "field"}});
      Json st{{"seeds", p["seeds"]}, {"field", f.name}};
      for (const char* key : {"steps", "step_size", "direction"})
        if (has(p, key)) st[key] = p[key];
      vs.streamlines = st;
      Json r = st;
      r["enabled"] = true;
      return r;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.transparency", 'V', "면을 투명하게 그린다(형상·메시·전부). what 을 비우면 불투명으로 돌아간다", "RND-17, RND-15");
    c.params = {F("what", "string", "투명하게 그릴 것").one_of({"geometry", "mesh", "all"}), F("alpha", "number", "불투명도(0~1, 기본 0.3)").gt(0).lt(1)};
    c.fn = [](App& a, const Json& p) {
      for (const FieldSpec& f : a.commands().at("view.transparency").params)
        if (has(p, f.name.c_str())) check_value(f, p[f.name], nullptr);
      if (!has(p, "what")) {
        state(a).transparency = nullptr;
        return Json{{"enabled", false}};
      }
      Json t{{"what", p["what"]}, {"alpha", p.value("alpha", 0.3)}};
      state(a).transparency = t;
      return Json{{"enabled", true}, {"settings", t}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.cube_pick", 'Q', "방향 큐브의 면·에지·꼭짓점(26방향)을 조회한다. 반환한 view 이름은 view.transition_begin 에 넘긴다", "RND-10, RND-37");
    c.params = {F("x", "number", "화면 픽셀 x").call_req(), F("y", "number", "화면 픽셀 y").call_req(), width_param(), height_param()};
    c.fn = [](App& a, const Json& p) {
      for (const FieldSpec& f : a.commands().at("view.cube_pick").params)
        if (has(p, f.name.c_str())) check_value(f, p[f.name], nullptr);
      const auto size = state(a).window_size;
      return cube_pick(state(a), p["x"].get<double>(), p["y"].get<double>(),
                       p.value("width", size[0] > 0 ? size[0] : 800), p.value("height", size[1] > 0 ? size[1] : 600));
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.cube_hover", 'V', "방향 큐브의 마우스 아래 면·에지·꼭짓점을 강조한다. x,y 를 모두 비우면 해제", "RND-10, RND-37, RND-49");
    c.params = {F("x", "number", "화면 픽셀 x"), F("y", "number", "화면 픽셀 y"), width_param(), height_param()};
    c.fn = [](App& a, const Json& p) {
      for (const FieldSpec& f : a.commands().at("view.cube_hover").params)
        if (has(p, f.name.c_str())) check_value(f, p[f.name], nullptr);
      if (has(p, "x") != has(p, "y")) throw Error("missing_param", "x,y 를 함께 지정해야 합니다");
      ViewState& vs = state(a);
      const auto size = vs.window_size;
      Json hit = has(p, "x") ? cube_pick(vs, p["x"].get<double>(), p["y"].get<double>(),
                 p.value("width", size[0] > 0 ? size[0] : 800), p.value("height", size[1] > 0 ? size[1] : 600)) : Json{{"hit", false}};
      const std::string hovered = hit.value("view", std::string());
      hit["changed"] = hovered != vs.cube_hover;
      vs.cube_hover = hovered;
      return hit;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.hud", 'V', "화면 고정 요소를 지정한다: 좌표축·방향 큐브(왼쪽 아래), 결과 범례(오른쪽), 글자(픽셀 위치)", "RND-35, RND-36, RND-37");
    c.params = {F("triad", "bool", "좌표축 표시(기본 켬)"), F("navigation_cube", "bool", "좌표축을 면 이름이 있는 방향 큐브로 표시(API 기본 끔, UI 켬)"), F("legend", "bool", "결과 범례 표시(기본 켬)"),
                F("labels", "object_list", "화면에 둘 글자(비우면 지운다)")
                    .of({F("text", "string", "글(대문자·숫자·기호)").call_req().ex("CASE 1"), F("x", "number", "왼쪽에서 픽셀").ex(10.0),
                         F("y", "number", "위에서 픽셀").ex(10.0), F("scale", "number", "점 하나의 픽셀 크기(기본 2)").gt(0).ex(2.0)})};
    c.fn = [](App& a, const Json& p) {
      for (const FieldSpec& f : a.commands().at("view.hud").params)
        if (has(p, f.name.c_str())) check_value(f, p[f.name], nullptr);
      Json& hud = state(a).hud;
      for (const char* key : {"triad", "navigation_cube", "legend", "labels"})
        if (has(p, key)) hud[key] = p[key];
      if (!cube_visible(state(a))) state(a).cube_hover.clear();
      return hud;
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.symbols", 'V', "하중·경계조건 심볼 표시를 켜고 끈다: step 이면 그 스텝에서 유효한 것, sets 면 그 하중 셋·구속 셋의 항목을 바로 그린다. 둘 다 없으면 끈다. view.hide 로 숨긴 셋의 항목은 그리지 않는다", "RND-34, RND-35, LOD-16, BC-13");
    c.params = {F("step", "ref", "스텝(sets 도 없으면 끈다)").ref("step"), F("sets", "integer_list", "바로 그릴 하중 셋·구속 셋 ID(스텝 대신)").ge(1),
                F("size", "number", "심볼 크기(없으면 모델 크기의 6%)").gt(0).unit("length")};
    c.fn = [](App& a, const Json& p) {
      if (has(p, "sets")) {
        for (const Json& sid : p["sets"]) {
          const Object& o = a.model().get(sid.get<Id>());
          if (o.kind != "load_set" && o.kind != "bc_set") throw Error("wrong_kind", "하중 셋·구속 셋이 아닙니다", {{"object", o.id}, {"kind", o.kind}});
        }
        Json s{{"sets", p["sets"]}};
        if (has(p, "size")) s["size"] = p["size"];
        state(a).symbols = s;
        return Json{{"shown", true}, {"sets", p["sets"]}};
      }
      if (!has(p, "step")) {
        state(a).symbols = nullptr;
        return Json{{"shown", false}};
      }
      const Object& s = a.model().get(p["step"].get<Id>());
      if (s.kind != "step") throw Error("wrong_kind", "스텝이 아닙니다", {{"object", s.id}, {"expected", "step"}});
      Json st{{"step", s.id}};
      if (has(p, "size")) st["size"] = p["size"];
      state(a).symbols = st;
      return Json{{"shown", true}, {"settings", st}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.highlight", 'V', "객체를 강조 표시하거나(요소·요소면·형상의 면) 모두 해제한다", "RND-27");
    c.params = {F("kind", "string", "대상 종류").one_of({"face", "element_face", "element"}),
                F("part", "ref", "형상 파트(kind=face)").ref("part"), F("index", "integer", "면 번호(kind=face)").ge(1),
                F("element", "integer", "요소(kind=element_face·element)").ge(1), F("face", "integer", "요소면 번호(kind=element_face)").ge(1),
                F("add", "bool", "기존 강조에 더한다(없으면 바꾼다)"), F("clear", "bool", "모두 해제한다")};
    c.fn = [](App& a, const Json& p) {
      ViewState& vs = state(a);
      if (p.value("clear", false) || !has(p, "kind")) vs.highlighted.clear();
      if (has(p, "kind")) {
        check_value(a.commands().at("view.highlight").params[0], p["kind"], nullptr);
        PickRecord rec;
        rec.kind = p["kind"].get<std::string>();
        if (rec.kind == "face") {
          if (!has(p, "part") || !has(p, "index")) throw Error("missing_param", "필수 매개변수가 없습니다: part, index", {{"param", "part"}});
          rec.a = p["part"].get<Id>(), rec.b = p["index"].get<int>();
        } else {
          if (!has(p, "element")) throw Error("missing_param", "필수 매개변수가 없습니다: element", {{"param", "element"}});
          rec.a = p["element"].get<Id>(), rec.b = rec.kind == "element_face" ? p.value("face", 0) : 0;
        }
        if (!p.value("add", false)) vs.highlighted.clear();
        const bool dup = std::any_of(vs.highlighted.begin(), vs.highlighted.end(),
                                     [&](const PickRecord& h) { return h.kind == rec.kind && h.a == rec.a && h.b == rec.b; });
        if (!dup) vs.highlighted.push_back(rec);
      }
      return Json{{"count", vs.highlighted.size()}};
    };
    app.register_command(std::move(c));
  }
  {
    CommandSpec c = base("view.diagnostics", 'Q', "GPU 와 마지막 프레임의 통계(삼각형·선·드로우 수)를 조회한다", "RND-44, RND-47");
    c.fn = [](App& a, const Json&) {
      Json info = renderer(a).info();
      info["legend"] = state(a).legend;
      info["hidden"] = std::vector<Id>(state(a).hidden.begin(), state(a).hidden.end());
      info["symbols"] = !state(a).symbols.is_null();  // 하중·구속 심볼이 켜져 있는가(창의 토글 버튼이 비춘다)
      // 마지막으로 만든 장면의 구속 표시(BC-13): 이름표 글, 색, 칠한 면(삼각형 수)·노드 표식 수, 고른 구속인가
      Json marks = Json::array();
      if (state(a).scene && !state(a).symbols.is_null()) {
        const std::set<Id> focus = focused_bcs(state(a), *state(a).scene);
        for (const BcMark& mark : state(a).scene->bc_marks)
          marks.push_back(Json{{"id", mark.id}, {"label", mark.text}, {"color", mark.color}, {"triangles", mark.triangles.size()},
                               {"nodes", mark.nodes.size()}, {"glyph_points", mark.points.size()}, {"focused", focus.count(mark.id) > 0}});
      }
      info["bc_marks"] = marks;
      // GPU 메모리 한도(RND-44): view.quality gpu_memory_limit(바이트). 마지막 프레임 + 남아 있는 할당이 한도의 80% 를 넘으면 near_limit, 넘으면 over_limit.
      Json& g = info["gpu_memory"];
      const std::uint64_t used = g.value("last_frame", std::uint64_t(0)) + g.value("persistent", std::uint64_t(0));
      g["used"] = used;
      if (state(a).quality.contains("gpu_memory_limit")) {
        const std::uint64_t limit = state(a).quality["gpu_memory_limit"].get<std::uint64_t>();
        g["limit"] = limit, g["near_limit"] = used >= limit * 8 / 10, g["over_limit"] = used > limit;
      }
      return info;
    };
    app.register_command(std::move(c));
  }
}

}  // namespace nasa95
