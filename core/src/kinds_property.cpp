// 프로퍼티(섹션), 구속·연결, 접촉.
// 속성은 CalculiX 의 섹션·구속 키워드가 받는 값을 솔버 중립 이름으로 둔 것이다.
#include "kinds.hpp"
#include "nasa95/error.hpp"

#include <algorithm>
#include <cmath>

namespace nasa95::kinds {

namespace {

F material() { return F("material", "ref", "재료").ref("material").req(); }
F orientation() { return F("orientation", "ref", "방향").ref("orientation"); }
F elements() { return target("target", "할당할 요소", kElements); }

void check_beam(const Model&, const Object& o) {
  if (o.props.value("type", std::string()) != "beam") return;
  if (!o.props.contains("section") || !o.props.contains("dimensions")) return;
  const std::string section = o.props["section"].get<std::string>();
  const int n = beam_section_dims(section);
  if (static_cast<int>(o.props["dimensions"].size()) != n)
    throw Error("invalid_param_type", "이 단면 종류의 치수는 " + std::to_string(n) + "개여야 합니다",
                {{"param", "dimensions"}, {"object", o.id}});
  if (beam_section_composite(section)) {
    // 형강 치수 h, b, tw, tf: 두께가 바깥 치수 안에 들어가야 부분 단면이 겹치지 않는다
    const std::vector<double> d = o.props["dimensions"].get<std::vector<double>>();
    const bool two_flanges = section == "I" || section == "C";
    if (d[2] >= d[1] || (two_flanges ? 2 * d[3] : d[3]) >= d[0])
      throw Error("invalid_param_type", "형강 치수가 맞지 않습니다: 웨브 두께(tw) < 폭(b), 플랜지 두께(tf)" + std::string(two_flanges ? "×2" : "") + " < 높이(h)",
                  {{"param", "dimensions"}, {"object", o.id}});
  }
}

}  // namespace

void register_property(Schema& s) {
  {
    KindSpec k;
    k.kind = "property", k.label = "프로퍼티", k.collection = "properties";
    k.features = "PRP-01~10, PRP-15~25";
    k.solver_name = true;
    k.fields = {elements(),
                F("nodal_thickness_values", "table", "절점별 두께 [노드, 두께]").columns(2).node_col(0).ex(rows({{1, 2.0}, {2, 1.5}}))};
    k.subtypes = {
        {"solid",  // *SOLID SECTION (3D·평면·축대칭·트러스)
         {material(), orientation(),
          F("thickness", "number", "두께(평면 요소) 또는 단면적(트러스)").gt(0).unit("length"),
          F("nodal_thickness", "bool", "절점 두께를 쓴다")}},
        {"shell",  // *SHELL SECTION
         {material(), orientation(), F("thickness", "number", "두께").req().gt(0).unit("length"),
          F("offset", "number", "기준면에서 중립면까지의 오프셋(두께 단위)"),
          F("nodal_thickness", "bool", "절점 두께를 쓴다")}},
        {"composite",  // *SHELL SECTION, COMPOSITE
         {orientation(), F("offset", "number", "오프셋(두께 단위)"),
          F("layers", "object_list", "층(아래에서 위로)")
              .req()
              .of({F("thickness", "number", "층 두께").call_req().gt(0).unit("length"),
                   F("material", "ref", "층 재료").ref("material").call_req(),
                   F("orientation", "ref", "층 방향").ref("orientation")})}},
        {"membrane",  // *MEMBRANE SECTION
         {material(), orientation(), F("thickness", "number", "두께").req().gt(0).unit("length"),
          F("offset", "number", "오프셋(두께 단위)")}},
        {"beam",  // *BEAM SECTION
         {material(), orientation(),
          F("section", "string", "단면 종류(I=H 형강·T·L·C 는 직사각형 부분 단면의 합성보로 덱에 나간다)").req().one_of({"rect", "circ", "pipe", "box", "general", "I", "T", "L", "C"}).ex("rect"),
          F("dimensions", "number_list", "단면 치수(종류에 따라 개수가 다르다: rect·circ 2, pipe 2, box 6, general 5, I·T·L·C 4 = h, b, tw, tf)").req().gt(0).unit("length").ex({20.0, 10.0}),
          F("direction", "vector3", "단면 1축 방향(형강은 웨브·높이 방향. 솔버 기본 (0,0,-1))").ex({0.0, 0.0, 1.0}),
          F("offset1", "number", "1축 방향 오프셋(두께 단위, 형강은 높이 h 단위)"), F("offset2", "number", "2축 방향 오프셋(두께 단위, 형강은 폭 b 단위)")}},
        {"truss", {material(), F("area", "number", "단면적").req().gt(0).unit("area")}},
        {"spring",  // *SPRING
         {F("stiffness", "number", "강성(선형)").unit("stiffness"),
          F("table", "table", "비선형: [힘, 변위] (변위 오름차순, 온도 의존이면 끝에 온도)").ex(rows({{0.0, 0.0}, {10.0, 1.0}})),
          F("dof1", "integer", "첫 노드의 자유도").ge(1).le(6), F("dof2", "integer", "둘째 노드의 자유도").ge(1).le(6),
          orientation()}},
        {"dashpot",  // *DASHPOT
         {F("coefficient", "number", "감쇠 상수").ge(0),
          F("table", "table", "[감쇠 상수, 주파수, (온도)]").ex(rows({{0.5, 10.0}, {0.4, 100.0}})),
          F("dof1", "integer", "첫 노드의 자유도").ge(1).le(6), F("dof2", "integer", "둘째 노드의 자유도").ge(1).le(6)}},
        {"gap",  // *GAP
         {F("clearance", "number", "초기 간극").req().unit("length"),
          F("direction", "vector3", "닫히는 방향").req().ex({1.0, 0.0, 0.0}),
          F("stiffness", "number", "닫힌 뒤의 강성").gt(0).unit("stiffness"), F("tension_force", "number", "간극 무한대에서의 인장력").unit("force")}},
        {"mass", {F("mass", "number", "질량").req().ge(0).unit("mass")}},
        {"fluid",  // *FLUID SECTION
         {material(), F("section_type", "string", "네트워크 요소 종류(솔버의 TYPE)").req().ex("ORIFICE CD1"),
          F("constants", "number_list", "종류별 상수").ex({1.0, 0.5}), F("oil", "string", "오일 이름")}},
        {"user",  // *USER SECTION
         {material(), F("constants", "number_list", "요소 상수").req().ex({1.0, 2.0, 3.0})}},
        {"substructure",  // *MATRIX ASSEMBLE
         {F("stiffness_file", "string", "강성 행렬 파일").req().ex("part.sti"),
          F("mass_file", "string", "질량 행렬 파일")}},
    };
    k.check = check_beam;
    s.add(std::move(k));
  }
  {
    KindSpec k;
    k.kind = "constraint", k.label = "구속", k.collection = "constraints";
    k.features = "BC-07~09, BC-16~21";
    k.subtypes = {
        {"equation",  // *EQUATION: 첫 항이 종속 자유도
         {F("terms", "table", "[노드, 자유도, 계수] (첫 행이 종속 항)").columns(3).req().node_col(0).ex(rows({{3, 1, 1.0}, {4, 1, -1.0}}))}},
        {"mpc_plane", {F("nodes", "integer_list", "평면을 유지할 노드(앞의 3개가 평면을 정한다)").req().ge(1).nodes().ex(ints({1, 2, 3, 4}))}},
        {"mpc_straight", {F("nodes", "integer_list", "직선을 유지할 노드(앞의 2개가 직선을 정한다)").req().ge(1).nodes().ex(ints({1, 2, 3}))}},
        {"mpc_beam", {F("nodes", "integer_list", "거리를 유지할 두 노드").req().ge(1).nodes().ex(ints({1, 2}))}},
        {"mpc_meanrot",
         {F("nodes", "integer_list", "평균 회전을 정할 노드").req().ge(1).nodes().ex(ints({1, 2, 3})),
          F("pilot_node", "integer", "회전을 받는 노드").req().ge(1).nodes().ex(4)}},
        {"mpc_dist",
         {F("nodes", "integer_list", "최대 거리를 제한할 두 노드").req().ge(1).nodes().ex(ints({1, 2})),
          F("pilot_node", "integer", "거리 값을 주는 노드").req().ge(1).nodes().ex(3)}},
        {"rigid_body",  // *RIGID BODY
         {target("target", "강체로 만들 노드 또는 요소", {"nodes", "elements", "set", "geometry"}).req(),
          F("ref_node", "integer", "기준 노드(병진)").ge(1).nodes(), F("rot_node", "integer", "회전 노드").ge(1).nodes()}},
        {"coupling_kinematic",  // *COUPLING + *KINEMATIC
         {target("surface", "묶을 면", kFaces).req(), F("ref_node", "integer", "기준 노드").req().ge(1).nodes().ex(100),
          F("dofs", "integer_list", "묶을 자유도").req().ge(1).le(6).ex(ints({1, 2, 3})), orientation()}},
        {"coupling_distributing",  // *COUPLING + *DISTRIBUTING
         {target("surface", "하중을 분배할 면", kFaces).req(), F("ref_node", "integer", "기준 노드").req().ge(1).nodes().ex(100),
          F("dofs", "integer_list", "분배할 자유도").req().ge(1).le(6).ex(ints({1, 2, 3})), orientation()}},
        {"tie",  // *TIE
         {target("slave", "종속 면", kNodesOrFaces).req(), target("master", "주 면", kFaces).req(),
          F("position_tolerance", "number", "위치 허용 오차").ge(0).unit("length"),
          F("adjust", "bool", "종속 노드를 주 면 위로 옮긴다(기본 켬)")}},
        {"cyclic_symmetry",  // *TIE, CYCLIC SYMMETRY + *CYCLIC SYMMETRY MODEL
         {target("slave", "섹터의 한쪽 면", kNodesOrFaces).req(), target("master", "섹터의 반대쪽 면", kNodesOrFaces).req(),
          F("sectors", "integer", "섹터 수").req().ge(2).ex(12),
          F("axis_point_a", "vector3", "회전축 위의 점 a").req().ex({0.0, 0.0, 0.0}),
          F("axis_point_b", "vector3", "회전축 위의 점 b").req().ex({0.0, 0.0, 1.0}),
          F("ngraph", "integer", "결과에 전개할 섹터 수").ge(1),
          F("position_tolerance", "number", "위치 허용 오차").ge(0).unit("length"),
          target("elements", "이 순환대칭에 속한 요소", kElements)}},
        {"multistage",  // *TIE, MULTISTAGE
         {target("slave", "한 단의 노드 면", {"nodes", "set"}).req(), target("master", "다른 단의 노드 면", {"nodes", "set"}).req(),
          F("position_tolerance", "number", "위치 허용 오차").ge(0).unit("length")}},
    };
    s.add(std::move(k));
  }
  {
    KindSpec k;  // *SURFACE INTERACTION 과 그 아래 키워드
    k.kind = "contact_property", k.label = "접촉 속성", k.collection = "contact_properties";
    k.features = "BC-23~27";
    k.solver_name = true;
    k.fields = {
        F("pressure_overclosure", "string", "압력-침투 관계").one_of({"hard", "linear", "exponential", "tabular", "tied"}),
        F("slope", "number", "기울기 K (linear, tied)").gt(0),
        F("sigma_inf", "number", "큰 간극에서의 인장 응력(linear, 노드-면)").gt(0),
        F("c0", "number", "간극 기준 거리(linear, exponential)").gt(0).unit("length"),
        F("p0", "number", "간극 0 에서의 압력(exponential)").gt(0).unit("pressure"),
        F("table", "table", "[압력, 침투량] (tabular)").columns(2).ex(rows({{0.0, 0.0}, {100.0, 0.01}})),
        F("friction_coefficient", "number", "마찰계수").ge(0),
        F("stick_slope", "number", "고착 구간 기울기").gt(0),
        F("damping", "number", "접촉 감쇠 계수").ge(0),
        F("damping_tangent_fraction", "number", "접선 방향 감쇠 비율").ge(0),
        F("conductance", "table", "간극 전도 [전도, 압력, (온도)]").ex(rows({{100.0, 0.0}, {200.0, 10.0}})),
        F("heat_conversion", "number", "마찰 일이 열로 바뀌는 비율").ge(0).le(1),
        F("heat_slave_fraction", "number", "종속 면으로 가는 열의 비율").ge(0).le(1),
        F("sliding_velocity", "number", "미끄럼 속도(고정값으로 줄 때)"),
    };
    s.add(std::move(k));
  }
  {
    KindSpec k;  // *CONTACT PAIR
    k.kind = "contact_pair", k.label = "접촉 쌍", k.collection = "contact_pairs";
    k.features = "BC-10, BC-25";
    k.fields = {target("slave", "종속 면", kNodesOrFaces).req(), target("master", "주 면", kFaces).req(),
                F("interaction", "ref", "접촉 속성").ref("contact_property").req(),
                F("small_sliding", "bool", "미소 미끄럼"),
                F("adjust", "number", "이 거리 안의 종속 노드를 주 면 위로 옮긴다").ge(0).unit("length"),
                F("clearance", "number", "초기 간극 값을 덮어쓴다").unit("length")};
    s.add(std::move(k));
  }
}

}  // namespace nasa95::kinds

namespace nasa95 {

int beam_section_dims(const std::string& section) {
  if (section == "rect" || section == "circ") return 2;  // 두 방향 치수(원형은 두 주축 길이)
  if (section == "pipe") return 2;                        // 외경 반지름, 두께
  if (section == "box") return 6;                         // a, b, t1, t2, t3, t4
  if (beam_section_composite(section)) return 4;          // h, b, tw, tf
  return 5;                                               // general: A, I11, I12, I22, 전단 계수
}

bool beam_section_composite(const std::string& section) { return section == "I" || section == "T" || section == "L" || section == "C"; }

std::vector<BeamRect> beam_section_rects(const std::string& section, const std::vector<double>& dims) {
  std::vector<BeamRect> r;
  if (section == "rect" || section == "circ") {
    if (dims.size() >= 2) r.push_back({dims[0], dims[1], 0, 0});
  } else if (section == "box") {  // 속 빈 각형: 벽 4개(a, b, t1 +1축 벽, t2 +2축 벽, t3 -1축 벽, t4 -2축 벽 — 매뉴얼 6.3.2). 두께 0 인 벽은 없다(U·C 형)
    if (dims.size() >= 6) {
      const double a = dims[0], b = dims[1], t1 = dims[2], t2 = dims[3], t3 = dims[4], t4 = dims[5];
      if (t1 > 0) r.push_back({t1, b, (a - t1) / 2, 0});
      if (t3 > 0) r.push_back({t3, b, -(a - t3) / 2, 0});
      const double inner = a - std::max(t1, 0.0) - std::max(t3, 0.0);
      if (t2 > 0 && inner > 0) r.push_back({inner, t2, (std::max(t3, 0.0) - std::max(t1, 0.0)) / 2, (b - t2) / 2});
      if (t4 > 0 && inner > 0) r.push_back({inner, t4, (std::max(t3, 0.0) - std::max(t1, 0.0)) / 2, -(b - t4) / 2});
    }
  } else if (section == "pipe") {
    if (!dims.empty()) r.push_back({2 * dims[0], 2 * dims[0], 0, 0});  // 솔버가 펼치는 외접 정사각형
  } else if (dims.size() >= 4) {
    const double h = dims[0], b = dims[1], tw = dims[2], tf = dims[3];
    if (section == "I") {  // 플랜지 둘(폭 b) + 웨브(높이 h - 2tf)
      r.push_back({tf, b, (h - tf) / 2, 0});
      r.push_back({tf, b, -(h - tf) / 2, 0});
      r.push_back({h - 2 * tf, tw, 0, 0});
    } else if (section == "T") {  // 플랜지가 +1축 쪽
      r.push_back({tf, b, (h - tf) / 2, 0});
      r.push_back({h - tf, tw, -tf / 2, 0});
    } else if (section == "L") {  // 세로 다리(1축, 두께 tw)가 -2축 쪽, 가로 다리(두께 tf)가 -1축 쪽
      r.push_back({h, tw, 0, -(b - tw) / 2});
      r.push_back({tf, b - tw, -(h - tf) / 2, tw / 2});
    } else if (section == "C") {  // 웨브(높이 h)가 -2축 쪽, 플랜지 둘이 +2축으로 뻗는다
      r.push_back({h, tw, 0, -(b - tw) / 2});
      r.push_back({tf, b - tw, (h - tf) / 2, tw / 2});
      r.push_back({tf, b - tw, -(h - tf) / 2, tw / 2});
    }
  }
  return r;
}

std::pair<double, double> beam_section_extent(const std::string& section, const std::vector<double>& dims) {
  double e1 = 0, e2 = 0;
  for (const BeamRect& q : beam_section_rects(section, dims)) {
    e1 = std::max(e1, 2 * (std::fabs(q.c1) + q.t1 / 2));
    e2 = std::max(e2, 2 * (std::fabs(q.c2) + q.t2 / 2));
  }
  return {e1, e2};
}

}  // namespace nasa95
