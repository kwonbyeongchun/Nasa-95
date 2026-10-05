// 해석 케이스와 스텝.
// 스텝의 하위 종류는 CalculiX 의 해석 절차 키워드와 1:1 이다(`*STATIC`, `*FREQUENCY` …).
// 열전달 유사 해석(음향·전기장 등)·네트워크·개수로는 케이스의 해석 분야로 지정하고 스텝은 heat_transfer 를 쓴다.
#include "kinds.hpp"

namespace nasa95::kinds {

namespace {

const std::vector<std::string> kSolvers = {"default", "pastix", "pardiso", "spooles", "taucs", "iterative_scaling",
                                           "iterative_cholesky"};

// 모든 스텝이 갖는 속성 (*STEP 과 절차 키워드의 공통 부분)
Fields step_common() {
  return {
      F("nlgeom", "bool", "기하 비선형"),
      F("perturbation", "bool", "섭동 스텝(직전 정적 스텝을 기준 상태로)"),
      F("max_increments", "integer", "최대 증분 수").ge(1),
      F("max_fluid_increments", "integer", "3D 유체 계산의 최대 증분 수(*STEP, INCF — 기본 10000)").ge(1),
      F("load_application", "string", "하중 적용 방식").one_of({"ramp", "step"}),
      F("loads_inheritance", "string", "앞 스텝 하중: keep = 유지, new = 모두 새로").one_of({"keep", "new"}),
      F("bcs_inheritance", "string", "앞 스텝 경계조건: keep = 유지, new = 모두 새로").one_of({"keep", "new"}),
      // 하중 셋·구속 셋 참조(D14): 셋의 하중·경계조건이 이 스텝의 것으로 들어온다(스텝 직속과 함께). 계수는 조합 하중(1.2D + 1.6L)
      F("load_sets", "object_list", "이 스텝에 넣을 하중 셋과 계수")
          .of({F("set", "ref", "하중 셋").ref("load_set").call_req(), F("factor", "number", "계수(기본 1)").ex(1.5)}),
      F("bc_sets", "ref_list", "이 스텝에 넣을 구속 셋").ref("bc_set"),
      F("controls", "object_list", "수렴 제어(*CONTROLS)")
          .of({F("parameters", "string", "대상").call_req().one_of(
                   {"time_incrementation", "field", "line_search", "network", "cfd", "contact"}),
               F("values", "table", "데이터 줄(솔버 형식 그대로)").call_req().ex(rows({{4.0, 8.0, 9.0, 16.0, 10.0, 4.0}}))}),
      F("controls_reset", "bool", "수렴 제어를 기본값으로 되돌린다"),
      // 재시작 파일 쓰기(CAS-38, *RESTART, WRITE): 이 스텝부터 <job>.rout 에 결과를 쌓는다. 빈도 0 이면 더 쓰지 않는다
      F("restart_write", "bool", "이 스텝부터 재시작 파일(<job>.rout)을 쓴다"),
      F("restart_frequency", "integer", "재시작 파일을 쓸 스텝 간격(기본 1, 0 = 그만 씀)").ge(0),
      F("restart_overlay", "bool", "재시작 파일에 마지막 스텝만 남긴다"),
  };
}

// 시간 증분을 쓰는 절차의 공통 속성
Fields time_fields() {
  return {F("initial_increment", "number", "초기 시간 증분").gt(0), F("period", "number", "스텝 시간").gt(0),
          F("min_increment", "number", "최소 시간 증분").gt(0), F("max_increment", "number", "최대 시간 증분").gt(0),
          F("direct", "bool", "증분 크기를 고정한다"), F("time_reset", "bool", "스텝 시작 시 시간을 0 으로"),
          F("total_time_at_start", "number", "스텝 시작 시의 전체 시간")};
}

F solver() { return F("solver", "string", "방정식 솔버").one_of(kSolvers); }

Fields join(std::initializer_list<Fields> parts) {
  Fields out;
  for (const Fields& p : parts) out.insert(out.end(), p.begin(), p.end());
  return out;
}

}  // namespace

void register_case(Schema& s) {
  {
    KindSpec k;
    k.kind = "case", k.label = "해석 케이스", k.collection = "cases";
    k.features = "CAS-01";
    k.fields = {
        F("physics", "string", "해석 분야")
            .one_of({"structural", "thermal", "thermo_mechanical", "acoustic", "electrostatic", "groundwater",
                     "diffusion", "lubrication", "shallow_water", "irrotational_flow", "gas_network",
                     "liquid_network", "channel_network", "fluid", "electromagnetic"}),
        F("description", "string", "설명").ex("외팔보 정적 해석"),
        // 이 케이스를 풀 솔버(D16). opensees 면 덱이 OpenSees 의 Tcl 스크립트로 나가고 OpenSees 실행 파일로 돌린다
        F("solver", "string", "솔버(없으면 calculix. 지원 범위는 solver.list)").one_of({"calculix", "opensees", "mystran"}),
        target("scope", "해석에 포함할 요소(없으면 전체)", kElements),
        F("contact_method", "string", "접촉 방식(케이스의 모든 접촉 쌍에 적용)")
            .one_of({"node_to_surface", "surface_to_surface", "mortar", "massless"}),
        F("rayleigh_alpha", "number", "레일리 감쇠 α(질량 비례)").ge(0),
        F("rayleigh_beta", "number", "레일리 감쇠 β(강성 비례)").ge(0),
        F("links", "ref_list", "산출물을 가져오는 선행 케이스").ref("case"),
        F("solver_executable", "string", "솔버 실행 파일 경로").ex("ccx.exe"),
        F("threads", "integer", "병렬 스레드 수").ge(1),
        F("work_directory", "string", "작업 폴더").ex("work"),
        F("user_subroutines", "bool", "사용자 서브루틴이 든 전용 실행 파일을 쓴다"),
        F("design_variable_type", "string", "민감도 해석의 설계 변수 종류").one_of({"coordinate", "orientation"}),
        target("design_nodes", "설계 변수로 쓸 표면 노드", {"nodes", "set"}),
        // 3D 유체(*VALUES AT INFINITY, 매뉴얼 7.139): 압력 계수 CP 와 난류 자유류 조건에 쓴다
        F("values_at_infinity", "number_list", "무한원 값 [정온도, 속도 크기, 정압, 밀도, 계산 영역 길이]").ex({40.0, 1.0, 11.428571, 1.0, 40.0}),
    };
    s.add(std::move(k));
  }
  {
    KindSpec k;
    k.kind = "step", k.label = "스텝", k.collection = "steps";
    k.features = "CAS-02, CAS-03";
    k.parents = {"case"};
    k.fields = step_common();
    const Fields t = time_fields();
    k.subtypes = {
        {"static", join({t, {solver()}})},
        {"frequency",
         {F("num_modes", "integer", "구할 모드 수").req().ge(1).ex(10), F("freq_min", "number", "진동수 하한").ge(0),
          F("freq_max", "number", "진동수 상한").gt(0), F("storage", "bool", "후속 해석용으로 고유치를 저장한다(.eig)"),
          F("global", "bool", "전역 좌표계로 저장"), F("cycmpc", "string", "순환대칭 구속 처리").one_of({"active", "inactive"}),
          F("cyclic_mode_min", "integer", "순환대칭 모드(절직경) 하한(*SELECT CYCLIC SYMMETRY MODES, NMIN)").ge(0),
          F("cyclic_mode_max", "integer", "순환대칭 모드(절직경) 상한(NMAX. 둘 중 하나라도 주면 순환대칭 고유치 해석)").ge(0),
          solver()}},
        {"complex_frequency",
         {F("num_modes", "integer", "구할 모드 수").req().ge(1).ex(10), F("coriolis", "bool", "코리올리 효과")}},
        {"buckle",
         {F("num_modes", "integer", "구할 좌굴 모드 수").req().ge(1).ex(5), F("accuracy", "number", "정확도").gt(0), solver()}},
        {"modal_dynamic",
         join({t,
               {F("steady_state", "bool", "정상 상태에 이를 때까지 계산"),
                F("damping_type", "string", "모드 감쇠 종류").one_of({"direct", "rayleigh"}),
                F("modal_damping", "table", "direct: [첫 모드, 끝 모드, 감쇠비]. rayleigh: [α, β] 한 행")
                    .ex(rows({{1.0, 10.0, 0.02}})),
                solver()}})},
        {"steady_state_dynamics",
         {F("freq_min", "number", "주파수 하한").req().ge(0).ex(1.0), F("freq_max", "number", "주파수 상한").req().gt(0).ex(1000.0),
          F("points", "integer", "계산 점 수").ge(2), F("bias", "number", "점 분포 편향").ge(1),
          F("harmonic", "bool", "조화 하중(끄면 비조화 주기 하중)"),
          F("fourier_terms", "integer", "비조화: 푸리에 항 수").ge(1),
          F("period_start", "number", "비조화: 주기 시작 시각"), F("period_end", "number", "비조화: 주기 끝 시각"),
          F("damping_type", "string", "모드 감쇠 종류").one_of({"direct", "rayleigh"}),
          F("modal_damping", "table", "direct: [첫 모드, 끝 모드, 감쇠비]. rayleigh: [α, β] 한 행").ex(rows({{1.0, 10.0, 0.02}})),
          solver()}},
        {"dynamic",
         join({t,
               {F("alpha", "number", "수치 감쇠 계수(-1/3 ~ 0)").ge(-1.0 / 3.0).le(0),
                F("explicit_scheme", "string", "적분 방식")
                    .one_of({"implicit", "explicit"}),
                F("relative_to_absolute", "bool", "상대 운동을 절대 운동으로"), solver()}})},
        {"heat_transfer",
         join({t,
               {F("steady_state", "bool", "정상 해석"), F("deltmx", "number", "증분당 최대 온도 변화").gt(0),
                F("eigenmodes", "bool", "열 고유치를 구한다"), F("num_modes", "integer", "열 고유 모드 수").ge(1),
                F("modal", "bool", "모드 기반 열해석"), F("storage", "bool", "고유치를 저장한다(.eig)"), solver()}})},
        {"coupled_temperature_displacement",
         join({t,
               {F("steady_state", "bool", "정상 해석"), F("deltmx", "number", "증분당 최대 온도 변화").gt(0),
                F("alpha", "number", "수치 감쇠 계수").ge(-1.0 / 3.0).le(0), solver()}})},
        {"uncoupled_temperature_displacement",
         join({t,
               {F("steady_state", "bool", "정상 해석"), F("deltmx", "number", "증분당 최대 온도 변화").gt(0),
                F("alpha", "number", "수치 감쇠 계수").ge(-1.0 / 3.0).le(0), solver()}})},
        {"visco", join({t, {F("cetol", "number", "증분 내 점성 변형률 허용 차").req().gt(0).ex(1e-4), solver()}})},
        {"electromagnetics",
         join({t,
               {F("magnetostatics", "bool", "정자기 해석"), F("frequency_domain", "bool", "주파수 영역 해석"),
                F("omega", "number", "전류의 주파수").gt(0), F("no_heat_transfer", "bool", "열전달 연성을 끈다"),
                F("deltmx", "number", "증분당 최대 온도 변화").gt(0), solver()}})},
        {"cfd",
         join({t,
               {F("steady_state", "bool", "정상 해석"), F("compressible", "bool", "압축성"),
                F("turbulence_model", "string", "난류 모델").one_of({"none", "k-epsilon", "k-omega", "sst"}),
                F("shallow_water", "bool", "천수 해석")}})},
        {"substructure_generate",
         {F("stiffness", "bool", "강성 행렬을 낸다"), F("mass", "bool", "질량 행렬을 낸다"),
          F("output_file", "string", "행렬 파일 이름").ex("substructure"),
          F("retained_dofs", "object_list", "유지할 노드 자유도(*RETAINED NODAL DOFS)")
              .of({target("target", "노드", {"nodes", "set"}).call_req(),
                   F("first_dof", "integer", "첫 자유도").call_req().ge(1).le(6).ex(1),
                   F("last_dof", "integer", "끝 자유도").ge(1).le(6)}),
          solver()}},
        {"sensitivity",  // *SENSITIVITY, *DESIGN RESPONSE, *FILTER
         {F("design_responses", "object_list", "설계 응답")
              .req()
              .of({F("name", "string", "이름").call_req().ex("resp1"),
                   F("type", "string", "응답 종류(솔버의 이름. 예: ALL-DISP, MASS, STRAIN ENERGY, STRESS)").call_req().ex("MASS"),
                   target("target", "대상 노드·요소", {"nodes", "elements", "parts", "set"}),
                   F("values", "number_list", "응답 종류별 추가 값").ex({10.0, 100.0})}),
          F("filter_type", "string", "민감도 필터").one_of({"explicit", "implicit"}),
          F("filter_radius", "number", "필터 반경").gt(0).unit("length"),
          F("boundary_weighting", "bool", "경계 가중"), F("edge_preservation", "bool", "모서리 보존"),
          F("direction_weighting", "bool", "방향 가중")}},
        {"feasible_direction",  // *FEASIBLE DIRECTION(매뉴얼 7.56), *OBJECTIVE(7.101), *CONSTRAINT(7.18), *GEOMETRIC CONSTRAINT(7.67)
         {F("method", "string", "제약 처리 방법(METHOD. 기본 gradient_descent)").one_of({"gradient_descent", "gradient_projection"}),
          F("step_size", "number", "메시 수정 크기(둘째 줄)").gt(0).unit("length").ex(0.1),
          F("objective", "string", "목적 함수로 쓸 설계 응답의 이름").req().ex("resp1"),
          F("objective_target", "string", "최소화/최대화(TARGET)").one_of({"min", "max"}),
          F("constraints", "object_list", "설계 응답에 대한 제약(*CONSTRAINT: 이름, LE|GE, 상대값, 절대값)")
              .of({F("response", "string", "설계 응답의 이름").call_req().ex("resp2"),
                   F("relation", "string", "부등호").call_req().one_of({"le", "ge"}).ex("le"),
                   F("relative_value", "number", "기준값 대비 비율"), F("absolute_value", "number", "절대값")}),
          F("geometric_constraints", "object_list", "기하 제약(*GEOMETRIC CONSTRAINT: 종류, 노드 셋, 반대쪽·경계 노드 셋, 값)")
              .of({F("type", "string", "제약 종류(솔버의 이름)").call_req()
                       .one_of({"MAXSHRINKAGE", "MAXGROWTH", "PACKAGING", "MAXMEMBERSIZE", "MINMEMBERSIZE"}).ex("MAXGROWTH"),
                   target("target", "대상 노드", {"nodes", "set"}).call_req(),
                   target("other_target", "반대쪽 노드(MAXMEMBERSIZE·MINMEMBERSIZE) 또는 경계 노드(PACKAGING)", {"nodes", "set"}),
                   F("value", "number", "허용 값(MAXMEMBERSIZE·MINMEMBERSIZE·MAXGROWTH·MAXSHRINKAGE)").unit("length")})}},
        {"robust_design",  // *ROBUST DESIGN(매뉴얼 7.111), *GEOMETRIC TOLERANCES(7.68), *CORRELATION LENGTH(7.25)
         {F("random_field_only", "bool", "랜덤 필드만 만든다(RANDOM FIELD ONLY — 지금은 이것만 가능)"),
          F("accuracy", "number", "요구 정확도(0~1, 둘째 줄)").req().gt(0).lt(1).ex(0.99),
          F("correlation_length", "number", "상관 길이(*CORRELATION LENGTH)").req().gt(0).unit("length").ex(2.0),
          F("constrained", "bool", "공차 노드와 나머지 노드 사이를 부드럽게 잇는다(CONSTRAINED)"),
          F("tolerances", "object_list", "기하 공차(*GEOMETRIC TOLERANCES, TYPE=NORMAL: 노드(셋), 평균, 표준편차)")
              .req()
              .of({target("target", "대상 노드", {"nodes", "set"}).call_req(),
                   F("mean", "number", "평균").call_req().unit("length").ex(0.0),
                   F("deviation", "number", "표준편차").call_req().ge(0).unit("length").ex(0.1)})}},
        {"green",
         {F("num_modes", "integer", "구할 모드 수").req().ge(1).ex(10), F("storage", "bool", "결과를 저장한다(.eig)"), solver()}},
        {"crack_propagation",  // *CRACK PROPAGATION(매뉴얼 7.26): INPUT·MATERIAL 필수, LENGTH 는 균열 길이 계산 방법, 데이터 줄은 최대 진전 길이·최대 꺾임각
         {F("input_file", "string", "균열 없는 구조의 결과 파일").req().ex("uncracked.frd"),
          F("material", "ref", "균열 진전 재료").ref("material").req(),
          F("length_method", "string", "균열 길이 계산 방법(LENGTH)").one_of({"cumulative", "intersection"}),
          F("max_increment", "number", "증분당 최대 균열 진전 길이").gt(0).unit("length").ex(0.05),
          F("max_angle", "number", "증분당 최대 꺾임각(도)").gt(0).ex(10.0),
          F("hcf_input_file", "string", "고주기 피로: 모드 결과 파일").ex("modes.frd"),
          F("hcf_mode", "integer", "고주기 피로: 모드 번호").ge(1),
          F("hcf_mission_step", "integer", "고주기 피로: 적용할 미션 스텝").ge(1),
          F("hcf_max_cycle", "number", "고주기 피로: 최대 사이클 수").gt(0),
          F("hcf_scaling", "number", "고주기 피로: 모드 배율")}},
    };
    s.add(std::move(k));
  }
  {
    KindSpec k;  // 가져온 덱에서 해석하지 못한 내용. 덱을 쓸 때 그대로 다시 나간다(CAS-12)
    k.kind = "deck_block", k.label = "보존한 덱 내용", k.collection = "deck_blocks";
    k.features = "CAS-12";
    k.parents = {"case", "step"};
    k.fields = {F("text", "string", "덱에 그대로 쓸 내용(키워드 줄과 데이터 줄)").call_req().ex("*NO ANALYSIS"),
                F("source", "string", "가져온 곳(파일:줄)").ex("job.inp:120"),
                // 케이스 아래의 블록이 나갈 자리. model = 스텝들보다 앞(모델 정의), steps = 스텝들 사이(after 스텝의 바로 뒤)
                F("place", "string", "케이스 아래 블록의 자리: model = 모델 정의, steps = 스텝 순서 안").one_of({"model", "steps"}),
                F("after", "ref", "place 가 steps 일 때: 이 스텝의 바로 뒤(없으면 첫 스텝 앞)").ref("step")};
    s.add(std::move(k));
  }
  {
    KindSpec k;  // 그래프(RES-17, RES-18, RES-49): 결과 파일의 이력·경로·주파수 응답 정의. 값은 plot.data 로 계산한다
    k.kind = "plot", k.label = "그래프", k.collection = "plots";
    k.features = "RES-17, RES-18, RES-49";
    k.suppressible = false;  // 해석 대상이 아니다
    const F file = F("result_file", "ref", "결과 파일").ref("result_file").req();
    const F field = F("field", "string", "결과 종류").req().ex("DISP");
    const F comp = F("component", "string", "성분 또는 파생량").req().ex("D3");
    k.subtypes = {
        {"history", {file, field, comp, F("node", "integer", "노드").req().ge(1).ex(1), F("step", "integer", "이 스텝의 프레임만").ge(1)}},
        {"path",
         {file, field, comp, F("frame", "integer", "프레임").req().ge(1).ex(1),
          F("points", "table", "경로의 꼭짓점").columns(3).req().unit("length").ex(rows({{0.0, 0.0, 0.0}, {100.0, 0.0, 0.0}})),
          F("samples", "integer", "표본 수(기본 50)").ge(2)}},
        {"frequency_response", {file, field, comp, F("node", "integer", "노드").req().ge(1).ex(1), F("step", "integer", "정상 상태 동해석 스텝").ge(1)}},
    };
    s.add(std::move(k));
  }
  {
    KindSpec k;  // 보고서(RES-26): 뷰·표·그래프·글을 모아 HTML 로 만든다(report.generate)
    k.kind = "report", k.label = "보고서", k.collection = "reports";
    k.features = "RES-26";
    k.suppressible = false;
    k.fields = {F("title", "string", "제목").ex("해석 보고서"),
                F("items", "object_list", "항목(순서대로): text / view(저장한 뷰 또는 현재 뷰) / plot / table(조회 명령의 결과)")
                    .of({F("kind", "string", "종류").call_req().one_of({"text", "view", "plot", "table"}).ex("text"),
                         F("title", "string", "항목 제목").ex("최대 응력"), F("text", "string", "text: 본문").ex("설명"),
                         F("view", "string", "view: 저장한 뷰 이름(없으면 현재 뷰)").ex("iso_contour"),
                         F("width", "integer", "view: 이미지 너비").ge(16), F("height", "integer", "view: 이미지 높이").ge(16),
                         F("plot", "ref", "plot: 그래프 객체").ref("plot"), F("command", "string", "table: 조회 명령 이름").ex("result.minmax"),
                         F("params", "object", "table: 명령 매개변수").ex(Json::object())})};
    s.add(std::move(k));
  }
  {
    KindSpec k;  // 폴더(WT-07): 같은 종류의 항목을 사용자가 묶어 정리한다. 상위 객체는 묶는 항목의 상위 객체와 같다(최상위면 없음).
    // 항목은 <종류>.move folder=<폴더> 로 넣고, 그 항목의 props.folder 에 남는다. 트리에서만 쓰이고 해석에는 영향이 없다.
    k.kind = "folder", k.label = "폴더", k.collection = "folders";
    k.features = "WT-07";
    k.parents = {"case", "step", "part", "result_file", "load_set", "bc_set", ""};  // 하위 항목을 가진 객체 아래(하중·구속 폴더는 셋 아래), 또는 최상위("")
    k.fields = {F("kind", "string", "묶는 항목의 종류(예: load, material)").call_req().ex("material"),
                F("description", "string", "설명").ex("볼트 재료")};
    s.add(std::move(k));
  }
  {
    KindSpec k;  // 프로젝트에 결과 파일을 연결해 둔다(RES-01). 열린 결과(result.open)는 실행 중 상태이고, 이 객체는 그 경로의 기억이다
    k.kind = "result_file", k.label = "결과 파일", k.collection = "results";
    k.features = "RES-01";
    k.fields = {F("path", "string", "결과 파일(frd) 경로. case 를 주면 비워 둘 수 있다(실행 폴더에서 찾는다)").ex("job.frd"),
                F("case", "ref", "이 결과를 만든 해석 케이스").ref("case"), F("description", "string", "설명").ex("하중 케이스 1")};
    s.add(std::move(k));
  }
  {
    KindSpec k;  // 파생 결과: 수식·조합·포락(RES-19~21). 값은 result.derived_values 로 계산한다(파일에 저장하지 않는다)
    k.kind = "derived_result", k.label = "파생 결과", k.collection = "derived";
    k.features = "RES-19, RES-20, RES-21";
    k.parents = {"result_file"};
    k.subtypes = {
        {"expression",  // 노드마다 수식을 계산한다. 변수는 "결과.성분"(예: STRESS.mises, DISP.D3)과 매개변수
         {F("expression", "string", "수식. 변수 = 결과종류.성분(예: STRESS.mises / 250)").req().ex("250 / max(STRESS.mises, 1e-9)"),
          F("frame", "integer", "프레임(없으면 계산할 때 지정)").ge(1)}},
        {"combination",  // 같은 결과 종류·성분을 프레임(또는 다른 결과 파일)마다 계수를 곱해 더한다
         {F("field", "string", "결과 종류").req().ex("DISP"), F("component", "string", "성분 또는 파생량").req().ex("D3"),
          F("terms", "object_list", "항: 결과 파일(없으면 이 파일)·프레임·계수").req()
              .of({F("result_file", "ref", "결과 파일").ref("result_file"), F("frame", "integer", "프레임").call_req().ge(1).ex(1),
                   F("factor", "number", "계수").call_req().ex(1.0)})}},
        {"envelope",  // 여러 프레임에서 노드마다 최대·최소와 그 출처(프레임)
         {F("field", "string", "결과 종류").req().ex("STRESS"), F("component", "string", "성분 또는 파생량").req().ex("mises"),
          F("frames", "integer_list", "프레임 목록(없으면 모든 프레임)").ex({1, 2, 3}),
          F("kind", "string", "최대·최소·절대값 최대").one_of({"max", "min", "absmax"})}},
        {"custom",  // 확장이 ext.register_derived_result 로 등록한 계산(API-21): 이름으로 그 명령을 부른다
         {F("calculation", "string", "등록된 파생 결과 계산 이름").req().ex("safety_factor"),
          F("frame", "integer", "프레임(없으면 조회할 때 지정)").ge(1).ex(1),
          F("params", "object", "계산 명령에 더할 매개변수").ex(Json::object())}},
    };
    s.add(std::move(k));
  }
}

}  // namespace nasa95::kinds
