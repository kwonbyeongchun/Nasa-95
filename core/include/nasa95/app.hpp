#pragma once
#include <any>
#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "nasa95/json.hpp"
#include "nasa95/mesh.hpp"
#include "nasa95/model.hpp"
#include "nasa95/schema.hpp"

namespace nasa95 {

class App;

// 명령 등록부의 항목(API-01). Python 래퍼·REST 경로·문서·입력 검증이 모두 이 정의에서 나온다.
//  kind: C 명령(모델 변경) / Q 조회 / J 작업 / V 뷰 / S 시스템
struct CommandSpec {
  std::string name;
  char kind = 'C';
  std::string desc;
  Fields params;
  bool undoable = false;   // 변경을 기록해 Undo 이력에 넣는다
  bool journaled = false;  // 저널에 남긴다(undoable 이면 항상 남긴다)
  // 다른 명령들을 불러 일하는 명령(확장·스크립트). 안에서 실행한 명령들이 Undo 한 단계로 묶인다.
  bool composite = false;
  bool external = false;   // 코어 밖(Python)에서 등록한 명령. 등록 해제할 수 있다
  std::string target;      // 대상 객체 종류(객체 명령일 때)
  std::string features;    // 대응 기능 ID
  std::function<Json(App&, const Json&)> fn;

  Json to_json() const;
};

// 명령에 JSON 과 따로 넘기는 대용량 배열(API-09). 저널·매크로에는 JSON 목록으로 풀어 적는다.
struct Arrays {
  std::map<std::string, std::vector<double>> doubles;
  std::map<std::string, std::vector<std::int64_t>> ints;
  std::map<std::string, int> cols;  // 2차원 배열이었으면 행의 길이(저널에 표로 적기 위해)
};

class App {
 public:
  using Listener = std::function<void(const Json&)>;

  App();

  // 모든 입구(UI·Python·REST)가 거치는 단일 실행 경로.
  Json execute(const std::string& name, const Json& params = Json::object(), const Arrays* arrays = nullptr);
  // 실행 중인 명령이 받은 배열(없으면 nullptr).
  const Arrays* arrays() const { return arrays_; }

  // 실행 중인 명령 안에서 다른 명령의 처리기를 부른다(입력 검증은 하고, 같은 Undo 단계에 들어간다).
  Json invoke(const std::string& name, const Json& params);
  // 기본 메시 파트(이름 MESH, 형상 없음). 없으면 만든다 — 파트 없이 요소를 만드는 명령이 쓴다(D19)
  Id default_mesh_part();

  void register_command(CommandSpec spec);
  void unregister_command(const std::string& name);  // external 명령만
  const std::map<std::string, CommandSpec>& commands() const { return commands_; }

  // 매크로 기록(API-13): 실행된 명령을 Python 코드로 모은다.
  void macro_start();
  std::string macro_stop();
  bool macro_recording() const { return macro_on_; }

  // 모델에 하나만 있는 설정 객체(단위계·물리 상수). 없으면 만든다(기록 중이어야 한다).
  Object settings();
  const Object* find_settings() const;

  Model& model() { return model_; }
  const Model& model() const { return model_; }
  Mesh& mesh() { return mesh_; }
  const Mesh& mesh() const { return mesh_; }
  Json digest() const;  // 객체와 메시를 합친 상태 요약값(API-39)
  Schema& schema() { return schema_; }
  const Schema& schema() const { return schema_; }

  // Undo/Redo (CMN-08, CMN-14~17)
  Json undo();
  Json redo();
  Json history() const;
  void history_goto(std::uint64_t serial);  // 0 = 맨 처음
  void set_history_limit(std::size_t limit);

  // 묶음 실행(API-04)
  void transaction_begin(const std::string& name);
  void transaction_commit();
  void transaction_rollback();
  bool in_transaction() const { return group_.has_value(); }
  Json transaction_changes() const;  // 열린 묶음이 지금까지 모은 변경의 요약(created·updated·deleted·mesh). 미리보기(RND-39)용

  // 프로젝트(CMN-09, CMN-20)
  void new_project();
  void save(const std::string& path);
  void open(const std::string& path);
  Json info() const;

  // 저널(API-08)
  void journal_start(const std::string& path);
  void journal_stop();
  Json journal_replay(const std::string& path);

  // 변경 통지(API-06). 구독자의 오류는 명령을 실패시키지 않고 여기에 남는다(마지막 것).
  int subscribe(Listener fn);
  void unsubscribe(int token);
  const Json& last_listener_error() const { return last_listener_error_; }

  // 모델에 속하지 않는 실행 중 상태(솔버 실행 등). 저장·Undo 대상이 아니다.
  std::any& runtime(const std::string& key) { return runtime_[key]; }

  static const char* version() { return "0.2.0"; }
  static const char* api_version() { return "0.1"; }

 private:
  struct Txn {
    std::uint64_t serial = 0;
    std::string name;
    ChangeSet changes;
    MeshLog mesh;
    bool empty() const { return changes.empty() && mesh.empty(); }
  };

  Json run_recorded(const CommandSpec& spec, const Json& params);
  // 불변식(D19, 2026-10-06): 요소는 반드시 메시 파트에 속한다. 명령이 끝날 때 파트 없는(또는 없어진 파트의) 요소를 기본 메시 파트에 넣는다
  void ensure_mesh_parts();
  Json run_readonly(const CommandSpec& spec, const Json& params);
  void push_txn(Txn t);
  void emit(const ChangeSet& cs, const MeshLog& mesh, bool inverse, const std::string& source, const std::string& command);
  void revert(const Txn& t);  // 객체와 메시의 변경을 되돌린다
  void reapply(const Txn& t);
  void journal_write(const std::string& name, const Json& params);
  std::uint64_t state_token() const;
  void reset_history();

  Model model_;
  Mesh mesh_;
  const Arrays* arrays_ = nullptr;
  Schema schema_;
  std::map<std::string, CommandSpec> commands_;

  std::vector<Txn> undo_, redo_;
  std::optional<Txn> group_;
  std::uint64_t next_serial_ = 1;
  std::uint64_t base_token_ = 0;   // 이력 맨 아래 상태의 토큰
  std::uint64_t saved_token_ = 0;  // 마지막 저장 시점의 토큰
  std::size_t history_limit_ = 200;

  std::string path_;
  std::ofstream journal_;
  bool replaying_ = false;
  bool macro_on_ = false;
  std::vector<std::string> macro_lines_;

  std::map<std::string, std::any> runtime_;
  std::map<int, Listener> listeners_;
  int next_listener_ = 1;
  Json last_listener_error_;
};

// 명령 등록(각 commands_*.cpp)
void register_object_commands(App& app);
// 프로그램 설정 값(app.settings_*; 없으면 null). 모델이 아니라 사용자 환경이다(CAS-41).
Json program_setting(App& app, const std::string& key);
void register_material_commands(App& app);
void register_section_commands(App& app);  // 표준 형강 목록 (commands_sections.cpp)
// 보 프로퍼티의 단면 상수 {area, i11, i22, j, ...}(property.section_values 와 같다). 다른 솔버의 덱 작성기(PBAR 등)가 쓴다
Json beam_section_properties(const Object& property);
// 좌표계 객체의 점 x 에서의 축(직교: 고정, 원통: 반경·접선·축). ax[0..2] 에 채운다
void csys_axes_at(const Object& csys, const Vec3& x, Vec3 ax[3]);
void register_system_commands(App& app);
void register_model_commands(App& app);
void register_mesh_commands(App& app);   // 메시 (commands_mesh.cpp)
void register_mesh_op_commands(App& app);  // 메시 연산·대상 전개·재번호 (commands_mesh_ops.cpp)

// 적용 대상을 메시 번호로 푼다. what: "nodes" | "elements" | "faces". 풀 수 없으면 Error.
Json resolve_target(const App& app, const Json& target, const std::string& what);
// 단위계 환산 계수(CMN-11·CMN-13): from 단위계의 값에 곱하면 to 단위계의 값. dim 은 length·force·pressure 등. 모르는 단위계·차원이면 Error
double unit_factor(const std::string& from, const std::string& to, const std::string& dim);
bool known_unit_system(const std::string& system);
// 재료 구성 모델 표를 from → to 단위계로 환산한다(열마다 차원). 돌려주는 값은 바꾼 값의 수. 라이브러리 가져오기·모델 단위계 변환이 함께 쓴다
int convert_material_behaviors(Json& behaviors, const std::string& from, const std::string& to);
// 단위계의 물리량별 단위 기호(길이 mm, 압력 MPa …). UI 입력 상자의 단위 표시용(CMN-12)
Json unit_symbols(const std::string& system);
void register_run_commands(App& app);      // 솔버 실행·상태·중지 (commands_run.cpp)
void register_solver_commands(App& app);   // 덱 출력·메시 내보내기 (commands_solver.cpp)

// 객체 명령에서 함께 쓰는 진단(프로젝트 전체 검증에서도 쓴다)
Json diagnose_object(const App& app, const Object& o);

// 스텝에 직접 둔 하중·경계조건 + 스텝이 참조하는 하중 셋(`load_sets` [{set, factor}])·구속 셋(`bc_sets`)의 것(D14).
// 셋의 하중은 계수를 곱한 사본(id·parent 는 원본 그대로, props 의 크기 값만 배수). 억제된 것은 뺀다. kind: "load" | "bc"
std::vector<Object> step_entries(const App& app, const Object& step, const std::string& kind);
// 보 프로퍼티의 단면 상수 {area, i11(1축 둘레), i22(2축 둘레), …}. 단면 종류·치수가 없으면 Error
Json beam_section_constants(const Object& beam_property);
// 스텝 전용 셋(하중은 반드시 하중 셋 안에, 구속은 구속 셋 안에 — 사용자 결정 2026-10-04): 그 스텝을 가리키는 load_set/bc_set 을 찾고,
// 없으면 "<스텝 이름> 하중/구속" 으로 만들어 스텝의 load_sets/bc_sets 에 넣는다. kind: "load" | "bc". 돌려주는 값은 셋 ID
Id step_own_set(App& app, const std::string& kind, Id step);

}  // namespace nasa95
