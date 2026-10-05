// 초기 조건, 하중, 경계조건, 스텝 중 변경, 출력 요청.
// 하중·경계조건은 스텝이 소유한다(스텝 아래에 직접 둔다). 초기 조건은 스텝과 무관한 모델 정의다.
#include "kinds.hpp"

namespace nasa95::kinds {

namespace {

F amplitude() { return F("amplitude", "ref", "시간(주파수) 함수").ref("function"); }
F time_delay() { return F("time_delay", "number", "함수를 시간 축으로 미는 양"); }
F csys() { return F("csys", "ref", "방향의 기준 좌표계").ref("csys"); }
F value(const std::string& desc) { return F("value", "number", desc).req().ex(1.0); }
// 계수를 곱해 조합할 수 있는 하중 크기
F mag(const std::string& desc) { return value(desc).scales(); }
F magvec(const std::string& name, const std::string& desc) { return F(name, "vector3", desc).req().scales().ex({0.0, 0.0, -1.0}); }
F vec(const std::string& name, const std::string& desc) { return F(name, "vector3", desc).req().ex({0.0, 0.0, -1.0}); }

const std::vector<std::string> kSurface = {"faces", "set", "geometry"};
const std::vector<std::string> kBody = {"elements", "set", "parts", "geometry"};

// 요소면에 거는 하중의 공통 속성: 좌표의 함수로 주는 공간 분포
F distribution() { return F("distribution", "ref", "공간 분포 함수(좌표의 수식)").ref("function"); }

}  // namespace

void register_load(Schema& s) {
  {
    KindSpec k;  // 하중 셋(D14): 하중을 모아 두는 최상위 그릇. 스텝의 load_sets [{set, factor}] 로 참조해 여러 케이스에서 재사용·조합한다
    k.kind = "load_set", k.label = "하중 셋", k.collection = "load_sets";
    k.features = "LOD-01, CAS-05";
    k.fields = {F("description", "string", "설명").ex("자중 + 적재"), F("step", "ref", "스텝 전용 셋이면 그 스텝(스텝을 부모로 만든 하중이 들어간다)").ref("step")};
    s.add(std::move(k));
  }
  {
    KindSpec k;  // 구속 셋(D14): 경계조건을 모아 두는 최상위 그릇. 스텝의 bc_sets 로 참조한다
    k.kind = "bc_set", k.label = "구속 셋", k.collection = "bc_sets";
    k.features = "BC-01, CAS-05";
    k.fields = {F("description", "string", "설명").ex("양단 핀"), F("step", "ref", "스텝 전용 셋이면 그 스텝").ref("step")};
    s.add(std::move(k));
  }
  {
    KindSpec k;  // *INITIAL CONDITIONS, *INITIAL STRAIN INCREASE
    k.kind = "initial_condition", k.label = "초기 조건", k.collection = "initial_conditions";
    k.features = "LOD-09, LOD-30, LOD-31, LOD-39";
    k.subtypes = {
        {"temperature",
         {target("target", "대상 노드", kNodes).req(), value("온도").unit("temperature"),
          F("gradient", "number", "두께 방향 온도 구배(쉘·보)")}},
        {"velocity", {target("target", "대상 노드", kNodes).req(), vec("components", "속도 성분")}},
        {"displacement", {target("target", "대상 노드", kNodes).req(), vec("components", "변위 성분").unit("length")}},
        {"stress",
         {target("target", "대상 요소", kElements).req(),
          F("components", "number_list", "응력 성분 [xx, yy, zz, xy, xz, yz]").req().unit("pressure").ex({1.0, 0.0, 0.0, 0.0, 0.0, 0.0}),
          F("user", "bool", "사용자 서브루틴으로 준다")}},
        {"plastic_strain",
         {target("target", "대상 요소", kElements).req(),
          F("components", "number_list", "소성 변형률 성분 [xx, yy, zz, xy, xz, yz]").req().ex({0.01, 0.0, 0.0, 0.0, 0.0, 0.0})}},
        {"solution",
         {target("target", "대상 요소", kElements).req(), F("values", "number_list", "내부 상태변수 값").req().ex({0.0, 0.0}),
          F("user", "bool", "사용자 서브루틴으로 준다")}},
        {"mass_flow", {target("target", "네트워크 노드", kNodes).req(), value("질량유량")}},
        {"total_pressure", {target("target", "네트워크 노드", kNodes).req(), value("전압력").unit("pressure")}},
        {"fluid_velocity", {target("target", "유체 노드", kNodes).req(), vec("components", "유체 속도 성분")}},
        {"pressure", {target("target", "유체 노드", kNodes).req(), value("정압").unit("pressure")}},
        {"turbulence", {target("target", "유체 노드", kNodes).req(), F("values", "number_list", "난류 변수").req().ex({1.0, 1.0})}},
        {"strain_increase", {target("target", "대상 요소", kElements).req()}},
    };
    s.add(std::move(k));
  }
  {
    KindSpec k;
    k.kind = "load", k.label = "하중", k.collection = "loads";
    k.features = "LOD-01";
    k.parents = {"load_set"};  // 하중은 반드시 하중 셋 안에(사용자 결정 2026-10-04). parent 로 스텝을 주면 그 스텝 전용 셋에 들어간다(step_own_set)
    k.fields = {amplitude(), time_delay(),
                F("phase", "string", "정상상태 동해석: 실수(동위상) / 허수(90° 위상)").one_of({"real", "imaginary"}),
                F("sector", "integer", "순환대칭: 하중을 줄 섹터 번호").ge(1)};
    k.subtypes = {
        // --- 구조
        {"force", {target("target", "대상 노드", kNodes).req(), magvec("components", "힘 성분").unit("force"), csys()}},
        {"moment", {target("target", "대상 노드", kNodes).req(), magvec("components", "모멘트 성분").unit("moment"), csys()}},
        {"pressure",  // 솔리드는 요소면에, 쉘은 요소(면 전체)에 건다
         {target("target", "대상 면(쉘은 요소)", {"faces", "elements", "parts", "set", "geometry"}).req(),
          mag("압력(솔리드: 면을 누르는 쪽이 양. 쉘: 요소 법선 방향으로 미는 쪽이 양)").unit("pressure"), distribution()}},
        {"traction",
         {target("target", "대상 면", kSurface).req(), magvec("components", "단위 면적당 힘 성분").unit("pressure"), csys(), distribution()}},
        {"edge_load", {target("target", "대상 쉘 edge(요소면)", kSurface).req(), mag("단위 길이당 힘(변에 수직, 변을 누르는 쪽이 양)").unit("force_per_length")}},
        {"line_load", {target("target", "대상 보 요소", kBody).req(), magvec("components", "단위 길이당 힘 성분").unit("force_per_length"), csys()}},
        {"gravity",
         {target("target", "대상 요소", kBody).req(), mag("가속도 크기").unit("acceleration"), vec("direction", "방향")}},
        {"acceleration",
         {target("target", "대상 요소", kBody).req(), mag("가속도 크기").unit("acceleration"), vec("direction", "방향")}},
        {"centrifugal",
         {target("target", "대상 요소", kBody).req(), F("omega", "number", "각속도(rad/시간)").req().ex(100.0),
          F("axis_point", "vector3", "회전축 위의 점").req().unit("length").ex({0.0, 0.0, 0.0}),
          vec("axis_direction", "회전축 방향")}},
        {"newton_gravity", {target("target", "서로 끌어당길 요소", kBody).req()}},
        {"temperature", {target("target", "대상 노드", kNodes).req(), mag("온도").unit("temperature")}},
        {"temperature_gradient",
         {target("target", "대상 노드", kNodes).req(), value("기준면 온도").unit("temperature"),
          F("gradient", "number", "두께 방향 온도 구배").req().ex(10.0)}},
        {"temperature_from_file",
         {F("file", "string", "온도가 든 결과 파일").req().ex("thermal.frd"),
          F("begin_step", "integer", "읽기 시작할 스텝").ge(1), target("target", "대상 노드(없으면 전체)", kNodes)}},
        {"remote_force",
         {target("target", "하중을 받을 면", kSurface).req(), F("point", "vector3", "하중 작용점").req().unit("length").ex({0.0, 0.0, 10.0}),
          magvec("force", "힘 성분").unit("force"), F("moment", "vector3", "모멘트 성분").unit("moment").scales().ex({0.0, 0.0, 0.0})}},
        {"total_force", {target("target", "대상 면", kSurface).req(), magvec("components", "합력 성분").unit("force"), csys()}},
        {"pretension",  // *PRE-TENSION SECTION
         {target("target", "볼트 단면(면)", kSurface).req(), F("node", "integer", "프리텐션 노드").req().ge(1).nodes().ex(1000),
          F("direction", "vector3", "단면 법선").ex({0.0, 0.0, 1.0}), F("force", "number", "체결력").unit("force").scales().ex(1000.0),
          F("fixed", "bool", "현재 체결 길이를 고정한다")}},
        // --- 열
        {"concentrated_flux", {target("target", "대상 노드", kNodes).req(), mag("열량(단위 시간당)").unit("power")}},
        {"surface_flux", {target("target", "대상 면", kSurface).req(), mag("단위 면적당 열유속(들어오는 쪽이 양)").unit("heat_flux"), distribution()}},
        {"body_flux", {target("target", "대상 요소", kBody).req(), mag("단위 체적당 발열").unit("power_per_volume")}},
        {"film",
         {target("target", "대상 면", kSurface).req(), F("coefficient", "number", "대류 계수").req().ge(0).unit("film_coefficient").ex(10.0),
          F("sink_temperature", "number", "주변 온도").req().unit("temperature").ex(293.0),
          F("coefficient_amplitude", "ref", "대류 계수의 시간 함수").ref("function")}},
        {"forced_convection",
         {target("target", "대상 면", kSurface).req(), F("coefficient", "number", "대류 계수").req().ge(0).ex(10.0),
          F("fluid_node", "integer", "열을 주고받는 네트워크 노드").req().ge(1).nodes().ex(1)}},
        {"radiation",
         {target("target", "대상 면", kSurface).req(), F("emissivity", "number", "방사율").req().ge(0).le(1).ex(0.8),
          F("sink_temperature", "number", "주변 온도").unit("temperature").ex(293.0),
          F("sink_node", "integer", "주변 온도를 주는 노드").ge(1).nodes()}},
        {"cavity_radiation",
         {target("target", "대상 면", kSurface).req(), F("emissivity", "number", "방사율").req().ge(0).le(1).ex(0.8),
          F("sink_temperature", "number", "보이지 않는 부분의 주변 온도").unit("temperature").ex(293.0),
          F("cavity", "string", "공동 이름(3자 이하)").ex("C1")}},
        // --- 연계·특수
        {"network_pressure",
         {target("target", "대상 면", kSurface).req(), F("fluid_node", "integer", "압력을 주는 네트워크 노드").req().ge(1).nodes().ex(1)}},
        {"submodel_traction",
         {target("target", "서브모델 경계 면", kSurface).req(), F("global_file", "string", "전역 모델 결과 파일").req().ex("global.frd"),
          F("global_step", "integer", "전역 모델의 스텝").ge(1), target("global_elements", "전역 모델에서 쓸 요소", kElements)}},
        {"submodel_force",
         {target("target", "서브모델 경계 노드", kNodes).req(), F("global_file", "string", "전역 모델 결과 파일").req().ex("global.frd"),
          F("global_step", "integer", "전역 모델의 스텝").ge(1)}},
        {"coil_current",
         {target("target", "코일 도체의 노드", kNodes).req(), value("전류"), F("frequency", "number", "주파수").gt(0)}},
        {"user",  // 사용자 서브루틴 하중(라벨에 NU 가 붙는다)
         {target("target", "대상", {"nodes", "elements", "faces", "set", "geometry"}).req(),
          F("base", "string", "기본 종류").req().one_of({"pressure", "surface_flux", "body_flux", "film", "radiation", "force", "temperature"}).ex("pressure"),
          F("label", "string", "분포를 구분하는 라벨 접미사").ex("1")}},
        {"mapped_field",
         {target("target", "대상 면 또는 노드", {"nodes", "faces", "set", "geometry"}).req(),
          F("quantity", "string", "물리량").req().one_of({"pressure", "temperature", "surface_flux"}).ex("pressure"),
          F("points", "table", "[x, y, z, 값] 점 자료").columns(4).req().ex(rows({{0.0, 0.0, 0.0, 1.0}, {1.0, 0.0, 0.0, 2.0}})),
          F("method", "string", "보간: nearest(가장 가까운 점) 또는 idw(역거리 가중, 기본)").one_of({"nearest", "idw"}),
          F("power", "number", "idw 의 거리 거듭제곱(기본 2)").gt(0), F("radius", "number", "idw 에서 쓸 점의 반지름(없으면 모든 점)").gt(0).unit("length")}},
    };
    s.add(std::move(k));
  }
  {
    KindSpec k;  // *BOUNDARY, *BASE MOTION
    k.kind = "bc", k.label = "경계조건", k.collection = "bcs";
    k.features = "BC-01";
    k.parents = {"bc_set"};  // 구속은 반드시 구속 셋 안에. parent 로 스텝을 주면 그 스텝 전용 셋에
    k.fields = {amplitude(), time_delay()};
    k.subtypes = {
        {"displacement",
         {target("target", "대상 노드", kNodes).req(),
          F("dofs", "integer_list", "구속할 자유도(1~3 병진, 4~6 회전)").req().ge(1).le(6).ex(ints({1, 2, 3})),
          F("values", "number_list", "자유도별 값(없으면 0)").ex({0.0, 0.0, 0.0}), csys()}},
        {"symmetry",
         {target("target", "대칭면 위의 노드", kNodes).req(), F("normal", "string", "대칭면의 법선 축").req().one_of({"x", "y", "z"}).ex("x"),
          csys()}},
        {"antisymmetry",
         {target("target", "반대칭면 위의 노드", kNodes).req(), F("normal", "string", "반대칭면의 법선 축").req().one_of({"x", "y", "z"}).ex("x"),
          csys()}},
        {"fixed_current",  // *BOUNDARY, FIXED
         {target("target", "대상 노드", kNodes).req(),
          F("dofs", "integer_list", "현재 값으로 고정할 자유도").req().ge(1).le(6).ex(ints({1, 2, 3}))}},
        {"temperature", {target("target", "대상 노드", kNodes).req(), value("온도").unit("temperature")}},
        {"base_motion",  // *BASE MOTION
         {F("dof", "integer", "가진 자유도").req().ge(1).le(3).ex(1),
          F("motion", "string", "가진 종류").one_of({"displacement", "acceleration"})}},
        {"submodel",  // *BOUNDARY, SUBMODEL
         {target("target", "서브모델 경계 노드", kNodes).req(),
          F("dofs", "integer_list", "보간해 받을 자유도(11 = 온도)").req().ge(1).le(11).ex(ints({1, 2, 3})),
          F("global_file", "string", "전역 모델 결과 파일").req().ex("global.frd"),
          F("global_step", "integer", "전역 모델의 스텝").ge(1), target("global_elements", "전역 모델에서 쓸 요소", kElements)}},
        {"network",
         {target("target", "네트워크 노드", kNodes).req(),
          F("quantity", "string", "지정할 양").req().one_of({"mass_flow", "total_pressure", "static_pressure", "depth", "temperature"}).ex("total_pressure"),
          value("값")}},
        {"electromagnetic", {target("target", "대상 노드", kNodes).req(), value("전위")}},
        {"fluid",
         {target("target", "유체 경계 노드", kNodes).req(),
          F("quantity", "string", "지정할 양").req().one_of({"velocity", "pressure", "temperature", "turbulence"}).ex("velocity"),
          F("values", "number_list", "값(속도는 3성분)").req().ex({0.0, 0.0, 0.0})}},
        {"user",  // *BOUNDARY, USER
         {target("target", "대상 노드", kNodes).req(),
          F("dofs", "integer_list", "사용자 서브루틴이 값을 줄 자유도").req().ge(1).le(11).ex(ints({1}))}},
    };
    s.add(std::move(k));
  }
  {
    KindSpec k;  // 스텝에서 모델을 바꾸는 키워드: *MODEL CHANGE, *CHANGE ...
    k.kind = "step_change", k.label = "스텝 중 변경", k.collection = "changes";
    k.features = "BC-28, BC-35, MAT-26, PRP-26";
    k.parents = {"step"};
    k.subtypes = {
        {"model_change_element",
         {target("target", "대상 요소", kElements).req(), F("action", "string", "추가/제거").req().one_of({"add", "remove"}).ex("remove")}},
        {"model_change_contact",
         {F("pair", "ref", "접촉 쌍").ref("contact_pair").req(), F("action", "string", "추가/제거").req().one_of({"add", "remove"}).ex("remove")}},
        {"change_friction",
         {F("interaction", "ref", "접촉 속성").ref("contact_property").req(),
          F("friction_coefficient", "number", "마찰계수").req().ge(0).ex(0.2), F("stick_slope", "number", "고착 구간 기울기").gt(0)}},
        {"change_surface_behavior",
         {F("interaction", "ref", "접촉 속성").ref("contact_property").req(),
          F("pressure_overclosure", "string", "압력-침투 관계").req().one_of({"hard", "linear", "exponential", "tabular", "tied"}).ex("linear"),
          F("slope", "number", "기울기 K").gt(0), F("sigma_inf", "number", "큰 간극에서의 인장 응력").gt(0),
          F("c0", "number", "간극 기준 거리").gt(0), F("p0", "number", "간극 0 에서의 압력").gt(0),
          F("table", "table", "[압력, 침투량]").columns(2).ex(rows({{0.0, 0.0}, {100.0, 0.01}}))}},
        {"change_contact_type",
         {F("method", "string", "접촉 방식").req().one_of({"node_to_surface", "surface_to_surface", "mortar", "massless"}).ex("surface_to_surface")}},
        {"change_material",  // *CHANGE MATERIAL + *CHANGE PLASTIC (소성 데이터만 바꿀 수 있다)
         {F("material", "ref", "재료").ref("material").req(),
          F("hardening", "string", "경화 종류").one_of({"isotropic", "kinematic"}),
          F("data", "table", "새 경화 곡선 [응력, 등가 소성 변형률, (온도)]").req().ex(rows({{300.0, 0.0}, {350.0, 0.1}}))}},
        {"change_section",  // *CHANGE SOLID SECTION
         {target("target", "대상 요소", kElements).req(), F("material", "ref", "새 재료").ref("material").req(),
          F("orientation", "ref", "방향").ref("orientation")}},
    };
    s.add(std::move(k));
  }
  {
    KindSpec k;  // 출력 요청: *NODE FILE, *EL FILE, *CONTACT FILE, *NODE PRINT, *EL PRINT, *CONTACT PRINT, *SECTION PRINT
    k.kind = "output_request", k.label = "출력 요청", k.collection = "outputs";
    k.features = "CAS-07, CAS-20";
    k.parents = {"step"};
    k.fields = {F("variables", "string_list", "출력 변수(솔버의 이름. 예: U, S, RF)").req().ex({"U", "RF"}),
                F("frequency", "integer", "몇 증분마다 출력할지").ge(1),
                F("frequency_f", "integer", "3D 유체: 몇 유체 증분마다 출력할지(FREQUENCYF)").ge(1),
                F("time_points", "ref", "출력 시점 목록").ref("time_points")};
    // 솔버로 확인: *NODE PRINT 는 GLOBAL 을 적지 않으면 *TRANSFORM 이 걸린 노드를 국부 좌표계로(줄 끝에 L) 찍는다.
    const F global("global", "bool", "전역 좌표계로 출력(끄면 좌표 변환이 걸린 노드는 국부 좌표계로)");
    const F binary("binary", "bool", "이진 형식으로 출력");
    const F expand = F("expand", "string", "쉘·보의 출력 형태").one_of({"2d", "3d"});
    const F totals = F("totals", "string", "합계 출력").one_of({"yes", "no", "only"});
    // 발산 진단(RES-31): LAST ITERATIONS 는 마지막 증분의 반복별 변위를 ResultsForLastIterations.frd 에, CONTACT ELEMENTS 는 반복별 접촉 요소를 <job>.cel 에 쓴다
    const F last_it("last_iterations", "bool", "마지막 증분의 반복마다 변위를 ResultsForLastIterations.frd 에 쓴다(발산 진단)");
    const F contact_el("contact_elements", "bool", "반복마다 생긴 접촉 요소를 <job>.cel 에 쓴다(발산 진단)");
    k.subtypes = {
        {"node_file", {target("target", "출력할 노드(없으면 전체)", kNodes), global, binary, expand, last_it, contact_el}},
        {"element_file",
         {target("target", "출력할 노드(없으면 전체)", kNodes), global, binary, expand, F("section_forces", "bool", "보 단면력을 출력"), last_it, contact_el}},
        {"contact_file", {binary}},
        {"node_print", {target("target", "출력할 노드", kNodes).req(), global, totals}},
        {"element_print", {target("target", "출력할 요소", kElements).req(), global, totals}},
        {"contact_print",
         {F("pair", "ref", "접촉 쌍(접촉력 합계용)").ref("contact_pair"), totals}},
        {"section_print", {target("target", "출력할 단면(면)", kSurface).req(), F("label", "string", "출력 이름").req().ex("section1")}},
    };
    s.add(std::move(k));
  }
}

}  // namespace nasa95::kinds
