#pragma once
#include <string>

#include "nasa95/app.hpp"

namespace nasa95 {

// CalculiX 입력 파일(덱) 쓰기의 결과(CAS-10).
//  text     덱 본문(ASCII)
//  warnings 덱은 썼지만 확인이 필요한 것: {code, message, object?}
//  skipped  덱에 쓰지 못한 객체: {object, kind, name, type, reason}. 조용히 빠뜨리지 않고 모두 여기에 알린다.
struct DeckResult {
  std::string text;
  std::string mesh_text;  // text 가운데 노드·요소·셋 부분(CAS-40 분할 출력용. text 는 머리 + mesh_text + model_text)
  std::string model_text;
  Json warnings = Json::array();
  Json skipped = Json::array();
};

struct DeckOptions {
  bool no_analysis = false;  // 스텝마다 *NO ANALYSIS 를 넣는다(입력 검사만)
  // 재시작 덱(CAS-38): 0 보다 크면 `*RESTART, READ, STEP=n` 으로 시작하고 모델 정의 없이 셋과 n 번째 뒤의 스텝만 쓴다
  // (모델은 <job>.rin 에서 읽는다. ccx 2.22 매뉴얼 7.110). 스텝 번호는 덱에 쓰인 순서(억제된 스텝 제외)다.
  int restart_from_step = 0;
};

// 해석 케이스 하나를 덱으로 쓴다. 모델을 바꾸지 않는다. 케이스의 solver 로 백엔드(solver.hpp 등록부)를 골라 그 작성기를 부른다(D16·D17).
DeckResult write_deck(const App& app, const Object& analysis_case, const DeckOptions& options = {});
// CalculiX 입력(*.inp)
DeckResult write_calculix_deck(const App& app, const Object& analysis_case, const DeckOptions& options = {});
// 케이스의 솔버(없으면 calculix).
std::string case_solver(const Object& analysis_case);
// OpenSees 입력(Tcl 스크립트). 스크립트가 결과를 <job>.frd 로, 진행을 <job>.sta 로 쓴다. no_analysis 면 모델만 만들고 끝낸다.
DeckResult write_opensees_deck(const App& app, const Object& analysis_case, const DeckOptions& options = {});
// 메시와 셋만 쓴다(메시 내보내기).
DeckResult write_mesh_deck(const App& app);

// 입력 파일에서 메시와 셋을 읽어 모델에 더한다(MSH-02). 기록 중인 명령 안에서 부른다.
// 돌려주는 값: 읽은 개수, 만든 파트·셋, 번호를 민 양, 건너뛴 카드(종류별 개수), 읽지 못한 요소 타입.
// merge: 번호를 밀지 않고 합친다(이미 있는 노드는 그대로, 요소 번호는 새것이어야 한다) — 번호를 이어 쓴 파일(솔버의 세분화 메시)용.
Json import_mesh_deck(App& app, const std::string& path, bool merge = false);

// 입력 파일을 읽어 메시·재료·섹션·스텝·하중·경계조건이 든 케이스 하나를 만든다(CAS-11).
// 해석하지 못한 카드는 deck_block 으로 보존한다(CAS-12). 메시가 빈 모델에서만 한다.
Json import_deck(App& app, const std::string& path);

}  // namespace nasa95
