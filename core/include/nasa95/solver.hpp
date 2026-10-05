// 솔버 백엔드 등록부(D16·D17, 2026-10-05): 모델은 솔버 중립이고, 케이스의 `solver` 가 어느 백엔드로 풀지 정한다.
// 백엔드마다 덱 쓰기·실행 인자·결과 변환·지원 범위(기능 표)를 한 곳에 둔다. CalculiX·OpenSees·MyStran 이 여기 등록되며,
// 뒤에 Nastran·ANSYS 등을 더할 때도 이 표에 한 항목을 더하는 것으로 시작한다. 솔버 프로그램은 모두 별도 실행 파일로만 쓴다(규칙 11).
#pragma once

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "nasa95/deck.hpp"
#include "nasa95/model.hpp"

namespace nasa95 {

class App;

struct SolverSpec {
  std::string name;              // calculix | opensees | mystran
  std::string label;
  std::string env_var;           // 실행 파일 환경 변수(NASA95_CCX …)
  std::string setting_key;       // 프로그램 설정 키(app.settings_set)
  std::string deck_extension;    // inp | tcl | bdf
  std::string license;           // 라이선스 메모(배포 가능 여부)
  bool restart = false;          // 재시작 덱 지원
  bool all = false;              // 지원 범위를 검사하지 않는다(CalculiX: 모델 정의가 이 솔버 기준)
  std::set<std::string> step_types;          // 지원 스텝 종류
  std::set<std::string> element_shapes;      // 지원 요소 형상(line2·quad4 …)
  std::set<std::string> load_types, bc_types, property_types, material_behaviors, constraint_types;
  bool contact = false;
  bool nlgeom = false;
  std::function<std::vector<std::string>(const std::string& job)> run_args;                     // 실행 인자
  std::function<DeckResult(const App&, const Object& analysis_case, const DeckOptions&)> write;  // 덱 쓰기
  // 실행이 끝난 뒤(결과를 frd 로 바꾸는 등). work 는 작업 폴더, job 은 작업 이름. 없으면 솔버가 <job>.frd 를 직접 낸다
  std::function<void(const App&, const Object& analysis_case, const std::string& work, const std::string& job)> after_run;
};

const std::vector<SolverSpec>& solver_specs();
const SolverSpec& solver_spec(const std::string& name);  // 모르는 이름이면 Error("not_supported")
Json solver_capabilities(const SolverSpec& spec);          // 기능 표(JSON)
// 케이스가 쓰는 정의 가운데 그 솔버가 지원하지 않는 것(case.check 의 "unsupported_by_solver" 오류 목록)
Json solver_support_issues(const App& app, const Object& analysis_case);

// 백엔드 구현(각 파일)
DeckResult write_mystran_deck(const App& app, const Object& analysis_case, const DeckOptions& options = {});
// 구속에 국부 좌표계(csys)가 있는 절점 → 좌표계 ID(GRID CD). 결과 변환기가 국부 성분을 전역으로 돌릴 때 같이 쓴다
std::map<Id, Id> mystran_node_csys(const App& app, const Object& analysis_case);
// MyStran 의 F06 을 읽어 <job>.frd 로 쓴다(결과 읽기·표시가 CalculiX 와 같은 경로를 탄다). 돌려주는 값: 읽은 블록 수
int convert_f06_to_frd(const App& app, const Object& analysis_case, const std::string& work, const std::string& job);

}  // namespace nasa95
