// 재료와 구성 모델.
//
// 재료는 구성 모델의 묶음이다(props.behaviors). 온도 의존이 가능한 구성 모델은 `data` 표로 값을 준다:
// 한 행이 한 온도의 상수 묶음이며 마지막 열이 온도다. 온도 의존이 없으면 온도 열 없이 한 행만 준다.
// 행의 상수 개수는 구성 모델과 그 종류에 따라 정해진다(아래 row_size).
// 열 순서는 CalculiX 입력 파일의 데이터 줄 순서와 같다(CalculiX 2.22 매뉴얼 7장).
#include "kinds.hpp"
#include "nasa95/error.hpp"

namespace nasa95::kinds {

namespace {

F data(const std::string& desc, Json example) {
  return F("data", "table", desc + " 온도 의존이면 행마다 끝에 온도를 붙인다").call_req().ex(std::move(example));
}

std::string str(const Json& b, const char* key, const char* def) { return b.value(key, std::string(def)); }

// 구성 모델의 선행 조건과 물리적 범위를 진단한다(MAT-12, MAT-28).
void diagnose_material(const Model&, const Object& o, Json& issues) {
  auto bit = o.props.find("behaviors");
  if (bit == o.props.end()) return;
  auto add = [&](const char* code, const std::string& field, const std::string& msg) {
    issues.push_back(Json{{"severity", "error"}, {"code", code}, {"object", o.id}, {"kind", o.kind},
                          {"name", o.name}, {"field", field}, {"message", msg}});
  };
  for (const BehaviorSpec& b : material_behaviors()) {
    auto it = bit->find(b.name);
    if (it == bit->end()) continue;
    for (const std::string& need : b.requires_all)
      if (!bit->contains(need))
        add("missing_behavior", b.name, b.label + " 에는 구성 모델 " + need + " 이 함께 있어야 합니다");
    if (b.diagnose)
      b.diagnose(*it, [&](const std::string& field, const std::string& msg) {
        add("out_of_range", b.name + "." + field, msg);
      });
  }
}

}  // namespace

void register_material(Schema& s) {
  KindSpec k;
  k.kind = "material", k.label = "재료", k.collection = "materials", k.features = "MAT-01";
  k.solver_name = true;
  k.fields = {F("description", "string", "설명").ex("구조용 강"),
              F("orientation", "ref", "재료 방향").ref("orientation")};
  k.diagnose = diagnose_material;
  s.add(std::move(k));
}

}  // namespace nasa95::kinds

namespace nasa95 {

using kinds::F;

const std::vector<BehaviorSpec>& material_behaviors() {
  using kinds::data;
  using kinds::rows;
  using kinds::str;
  static const std::vector<BehaviorSpec> v = [] {
    std::vector<BehaviorSpec> b;
    auto add = [&](BehaviorSpec spec) -> BehaviorSpec& {
      b.push_back(std::move(spec));
      return b.back();
    };

    {  // *ELASTIC
      BehaviorSpec& x = add({"elastic", "탄성", {}, "MAT-02, MAT-03, MAT-13", {}});
      x.fields = {F("type", "string", "형식").one_of({"iso", "ortho", "engineering_constants", "aniso"}),
                  data("iso: [E, ν]. ortho: 강성 계수 9개. engineering_constants: [E1,E2,E3,ν12,ν13,ν23,G12,G13,G23]. "
                       "aniso: 강성 계수 21개.",
                       rows({{210000.0, 0.3}}))};
      x.row_size = [](const Json& p) {
        const std::string t = str(p, "type", "iso");
        return t == "iso" ? 2 : t == "aniso" ? 21 : 9;
      };
      x.column_dims = [](const Json& p) -> std::vector<std::string> {
        const std::string t = str(p, "type", "iso");
        if (t == "iso") return {"pressure", ""};
        if (t == "engineering_constants") return {"pressure", "pressure", "pressure", "", "", "", "pressure", "pressure", "pressure"};
        return std::vector<std::string>(t == "aniso" ? 21 : 9, "pressure");  // 강성 계수
      };
      x.diagnose = [](const Json& p, const BehaviorSpec::Report& report) {
        if (str(p, "type", "iso") != "iso") return;
        for (const Json& row : p["data"]) {
          if (row[0].get<double>() <= 0) report("data", "탄성계수는 0 보다 커야 합니다");
          const double nu = row[1].get<double>();
          if (nu <= -1.0 || nu >= 0.5) report("data", "포아송비는 -1 보다 크고 0.5 보다 작아야 합니다");
        }
      };
    }
    {  // *DENSITY
      BehaviorSpec& x = add({"density", "밀도", {data("[밀도].", rows({{7.85e-9}}))}, "MAT-02", {}});
      x.row_size = [](const Json&) { return 1; };
      x.column_dims = [](const Json&) { return std::vector<std::string>{"density"}; };
      x.diagnose = [](const Json& p, const BehaviorSpec::Report& report) {
        for (const Json& row : p["data"])
          if (row[0].get<double>() <= 0) report("data", "밀도는 0 보다 커야 합니다");
      };
    }
    {  // *PLASTIC
      BehaviorSpec& x = add({"plastic", "소성", {}, "MAT-04, MAT-16, MAT-17", {"elastic"}});
      x.fields = {F("hardening", "string", "경화 종류")
                      .one_of({"isotropic", "kinematic", "combined", "user", "johnson_cook"}),
                  F("data", "table",
                    "경화 곡선 [응력, 등가 소성 변형률] (온도 의존이면 끝에 온도). "
                    "johnson_cook 은 [A, B, n, m, Tm, T0] 한 행. user 는 주지 않는다")
                      .ex(rows({{250.0, 0.0}, {300.0, 0.1}}))};
      x.row_size = [](const Json& p) {
        const std::string h = str(p, "hardening", "isotropic");
        return h == "johnson_cook" ? 6 : h == "user" ? 0 : 2;
      };
      x.column_dims = [](const Json& p) -> std::vector<std::string> {
        const std::string h = str(p, "hardening", "isotropic");
        if (h == "johnson_cook") return {"pressure", "pressure", "", "", "temperature", "temperature"};
        if (h == "user") return {};
        return {"pressure", ""};
      };
      x.curve = true;  // 온도별 곡선: 같은 온도 안에서 둘째 열(소성 변형률)이 오름차순
      x.diagnose = [](const Json& p, const BehaviorSpec::Report& report) {
        const std::string h = str(p, "hardening", "isotropic");
        if (h == "johnson_cook" || h == "user" || !p.contains("data") || p["data"].empty()) return;
        if (p["data"][0][1].get<double>() != 0.0)
          report("data", "경화 곡선의 첫 점은 소성 변형률 0 이어야 합니다");
      };
    }
    {  // *CYCLIC HARDENING
      BehaviorSpec& x = add({"cyclic_hardening", "복합 경화의 등방 경화 곡선",
                             {data("[응력, 등가 소성 변형률].", rows({{250.0, 0.0}, {280.0, 0.1}}))}, "MAT-16", {"plastic"}});
      x.row_size = [](const Json&) { return 2; };
      x.curve = true;
    }
    {  // *RATE DEPENDENT
      add({"rate_dependent", "변형률 속도 의존(Johnson-Cook)",
           {F("c", "number", "C").call_req().ex(0.01), F("eps0", "number", "기준 변형률 속도").call_req().gt(0).ex(1.0)},
           "MAT-17", {"plastic"}});
    }
    {  // *DEFORMATION PLASTICITY
      BehaviorSpec& x = add({"deformation_plasticity", "변형 소성(Ramberg-Osgood)",
                             {data("[E, ν, 항복 응력, 지수 n, 항복 오프셋 α].", rows({{210000.0, 0.3, 800.0, 12.0, 0.4}}))},
                             "MAT-18", {}});
      x.row_size = [](const Json&) { return 5; };
    }
    {  // *CREEP
      BehaviorSpec& x = add({"creep", "크리프", {}, "MAT-05, MAT-19", {"elastic"}});
      x.fields = {F("law", "string", "법칙").one_of({"norton", "user"}),
                  F("data", "table", "Norton 법칙 [A, n, m] (온도 의존이면 끝에 온도). user 는 주지 않는다")
                      .ex(rows({{1e-10, 5.0, 0.0}}))};
      x.row_size = [](const Json& p) { return str(p, "law", "norton") == "user" ? 0 : 3; };
    }
    {  // *HYPERELASTIC
      BehaviorSpec& x = add({"hyperelastic", "초탄성", {}, "MAT-05, MAT-14", {}});
      x.fields = {F("model", "string", "모델")
                      .call_req()
                      .one_of({"arruda_boyce", "mooney_rivlin", "neo_hooke", "ogden", "polynomial",
                               "reduced_polynomial", "yeoh"})
                      .ex("neo_hooke"),
                  F("n", "integer", "차수(ogden, polynomial, reduced_polynomial)").ge(1).le(3).ex(1),
                  data("모델의 상수(매뉴얼의 순서).", rows({{0.5, 0.001}}))};
      x.row_size = [](const Json& p) {
        const std::string m = str(p, "model", "");
        const int n = p.value("n", 1);
        if (m == "arruda_boyce" || m == "mooney_rivlin") return 3;
        if (m == "neo_hooke") return 2;
        if (m == "ogden") return 3 * n;
        if (m == "polynomial") return n == 1 ? 3 : n == 2 ? 7 : 12;
        if (m == "reduced_polynomial") return 2 * n;
        return 6;  // yeoh
      };
    }
    {  // *HYPERFOAM
      BehaviorSpec& x = add({"hyperfoam", "초탄성 폼", {}, "MAT-15", {}});
      x.fields = {F("n", "integer", "차수").ge(1).le(3).ex(1),
                  data("[μ1, α1, …, ν1, …].", rows({{0.164861, 8.88413, 0.0}}))};
      x.row_size = [](const Json& p) { return 3 * p.value("n", 1); };
    }
    {  // *MOHR COULOMB
      BehaviorSpec& x = add({"mohr_coulomb", "Mohr-Coulomb",
                             {data("[마찰각(도), 팽창각(도)].", rows({{20.0, 10.0}}))}, "MAT-20",
                             {"elastic"}});
      x.row_size = [](const Json&) { return 2; };
    }
    {  // *MOHR COULOMB HARDENING
      BehaviorSpec& x = add({"mohr_coulomb_hardening", "Mohr-Coulomb 경화",
                             {data("[점착력, 등가 소성 변형률].", rows({{10.0, 0.0}, {100.0, 1.0}}))}, "MAT-20",
                             {"mohr_coulomb"}});
      x.row_size = [](const Json&) { return 2; };
      x.curve = true;
    }
    {  // *USER MATERIAL, *DEPVAR
      BehaviorSpec& x = add({"user", "사용자 재료", {}, "MAT-22", {}});
      x.fields = {F("type", "string", "종류").one_of({"mechanical", "thermal"}),
                  F("constants", "integer", "상수 개수").call_req().ge(1).ex(2),
                  data("상수 묶음(행마다 상수 개수만큼).", rows({{1.0, 2.0}}))};
      x.row_size = [](const Json& p) { return p.value("constants", 0); };
      add({"depvar", "내부 상태변수", {F("count", "integer", "상태변수 개수").call_req().ge(1).ex(3)}, "MAT-22", {}});
    }
    {  // 내장 특수 재료(사용자 재료의 예약 이름으로 출력된다)
      BehaviorSpec& x = add({"special", "내장 특수 재료", {}, "MAT-21", {}});
      x.fields = {F("model", "string", "모델")
                      .call_req()
                      .one_of({"tension_only", "compression_only", "fiber", "single_crystal",
                               "single_crystal_creep", "aniso_plasticity", "aniso_creep", "ideal_gas",
                               "ciarlet", "small_strain_rotation_insensitive"})
                      .ex("tension_only"),
                  F("constants", "table", "모델의 상수(행 = 온도별)").call_req().ex(rows({{210000.0, 1e-3}}))};
    }
    {  // *EXPANSION
      BehaviorSpec& x = add({"expansion", "열팽창", {}, "MAT-06, MAT-23", {}});
      x.fields = {F("type", "string", "형식").one_of({"iso", "ortho", "aniso"}),
                  F("zero", "number", "기준 온도").unit("temperature"),
                  data("iso: [α]. ortho: [α11, α22, α33]. aniso: 6개.", rows({{1.2e-5}}))};
      x.row_size = [](const Json& p) {
        const std::string t = str(p, "type", "iso");
        return t == "iso" ? 1 : t == "ortho" ? 3 : 6;
      };
      x.column_dims = [](const Json& p) {
        const std::string t = str(p, "type", "iso");
        return std::vector<std::string>(t == "iso" ? 1 : t == "ortho" ? 3 : 6, "thermal_expansion");
      };
    }
    {  // *CONDUCTIVITY
      BehaviorSpec& x = add({"conductivity", "열전도", {}, "MAT-06, MAT-23", {}});
      x.fields = {F("type", "string", "형식").one_of({"iso", "ortho", "aniso"}),
                  data("iso: [k]. ortho: [k11, k22, k33]. aniso: 6개.", rows({{50.0}}))};
      x.row_size = [](const Json& p) {
        const std::string t = str(p, "type", "iso");
        return t == "iso" ? 1 : t == "ortho" ? 3 : 6;
      };
      x.column_dims = [](const Json& p) {
        const std::string t = str(p, "type", "iso");
        return std::vector<std::string>(t == "iso" ? 1 : t == "ortho" ? 3 : 6, "conductivity");
      };
      x.diagnose = [](const Json& p, const BehaviorSpec::Report& report) {
        if (str(p, "type", "iso") != "iso") return;
        for (const Json& row : p["data"])
          if (row[0].get<double>() <= 0) report("data", "열전도율은 0 보다 커야 합니다");
      };
    }
    {  // *SPECIFIC HEAT
      BehaviorSpec& x = add({"specific_heat", "비열", {data("[비열].", rows({{4.46e8}}))}, "MAT-06", {}});
      x.row_size = [](const Json&) { return 1; };
      x.column_dims = [](const Json&) { return std::vector<std::string>{"specific_heat"}; };
      x.diagnose = [](const Json& p, const BehaviorSpec::Report& report) {
        for (const Json& row : p["data"])
          if (row[0].get<double>() <= 0) report("data", "비열은 0 보다 커야 합니다");
      };
    }
    {  // *DAMPING, STRUCTURAL  (레일리 감쇠는 재료가 아니라 케이스에 둔다)
      add({"structural_damping", "구조 감쇠", {F("value", "number", "구조 감쇠 계수").call_req().ge(0).ex(0.03)},
           "MAT-08", {}});
    }
    {  // *ELECTRICAL CONDUCTIVITY
      BehaviorSpec& x = add({"electrical_conductivity", "전기 전도도", {data("[전도도].", rows({{5.96e7}}))}, "MAT-24", {}});
      x.row_size = [](const Json&) { return 1; };
    }
    {  // *MAGNETIC PERMEABILITY
      BehaviorSpec& x = add({"magnetic_permeability", "투자율",
                             {data("[투자율, 영역 번호].", rows({{1.255987e-6, 2.0}}))}, "MAT-24", {}});
      x.row_size = [](const Json&) { return 2; };
    }
    {  // *FLUID CONSTANTS
      BehaviorSpec& x = add({"fluid_constants", "유체 물성", {data("[정압 비열, 점성].", rows({{1.032e9, 71.1e-13}}))},
                             "MAT-25", {}});
      x.row_size = [](const Json&) { return 2; };
    }
    {  // *SPECIFIC GAS CONSTANT
      add({"specific_gas_constant", "기체 상수", {F("value", "number", "기체 상수").call_req().gt(0).ex(287.0)},
           "MAT-25", {}});
    }
    {  // 허용치(결과의 안전율 계산용. 솔버로는 나가지 않는다)
      add({"allowable", "허용치",
           {F("yield", "number", "항복 강도").gt(0).unit("pressure").ex(250.0),
            F("ultimate", "number", "인장 강도").gt(0).unit("pressure").ex(400.0)},
           "MAT-09", {}});
    }
    return b;
  }();
  return v;
}

}  // namespace nasa95
