#pragma once
#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

#include "nasa95/app.hpp"

namespace nasa95 {

// 결과 데이터 모델(RES-02): 결과 파일 → 프레임(스텝·증분) → 결과 종류(필드) → 성분.
// 무거운 배열은 여기(C++)에 두고, 필요한 필드만 파일에서 읽어 올린다(RES-03).
struct ResultField {
  std::string name;                     // 솔버의 이름: DISP, STRESS, FORC, NDTEMP …
  std::vector<std::string> components;  // 파일에 값이 있는 성분: D1, D2, D3 / SXX …
  int entity = 1;                       // 1 스칼라, 2 벡터, 4 텐서(대칭 6성분)
  std::string location = "node";        // frd 의 결과는 모두 절점 값(요소 결과는 솔버가 절점으로 외삽한 값)

  // 지연 로딩
  long long offset = 0;                 // 파일에서 값이 시작하는 위치
  std::size_t count = 0;                // 절점 수
  int format = 1;                       // 0 짧은 형식, 1 긴 형식
  bool loaded = false;
  std::vector<Id> ids;                  // 절점 번호(파일에 적힌 순서)
  std::vector<double> data;             // count × components.size()
};

struct ResultFrame {
  int index = 0;       // 1 부터
  int step = 0;        // 스텝 번호
  int increment = 0;   // 스텝 안의 증분(모드 해석이면 모드 번호)
  double value = 0;    // 시간 / 진동수 / 좌굴 계수 …
  std::string type;    // static | time | frequency | load_step | user
  std::map<std::string, std::string> attributes;  // 1P 줄(STEP, MODE, GM …)
  std::vector<ResultField> fields;
};

struct ResultElement {
  Id id = 0;
  int type = 0;  // frd 의 요소 타입 번호(1 he8, 2 pe6, 3 tet4, 4 he20, 5 pe15, 6 tet10, 7 tr3, 8 tr6, 9 qu4, 10 qu8, 11 be2, 12 be3)
  std::vector<Id> nodes;  // frd 의 절점 순서(입력 파일의 순서와 다를 수 있다)
};

struct ResultFile {
  Id id = 0;
  std::string path;
  Id case_id = 0;
  Id file_id = 0;  // 연 result_file 객체(없으면 0)
  std::vector<Id> node_ids;
  std::vector<double> node_xyz;
  std::vector<ResultElement> elements;
  std::vector<ResultFrame> frames;
  std::map<std::string, std::string> header;  // 1U 줄
  std::string unit_system;                    // 파일의 단위계(비면 모델과 같다). 값은 읽을 때 모델 단위계로 환산한다(CMN-13)
  std::string model_digest;                   // 이 결과가 나온 모델·메시의 다이제스트(케이스 실행 때 것, 없으면 연 때의 것). 결과가 모델보다 오래됐는지 판정(D13)
  std::map<std::string, double> unit_factors; // 차원 → 환산 계수(unit_system 이 있을 때)

  ResultFrame& frame(int index);                                  // 없으면 Error
  ResultField& field(ResultFrame& frame, const std::string& name);  // 값을 읽어 올린다. 없으면 Error
};

// 실행 중 상태로만 둔다(저장·Undo 대상이 아님).
class ResultStore {
 public:
  std::shared_ptr<ResultFile> open(const std::string& path);  // frd 를 훑어 목록만 만든다
  ResultFile& get(Id id);
  void close(Id id);
  const std::map<Id, std::shared_ptr<ResultFile>>& files() const { return files_; }

 private:
  std::map<Id, std::shared_ptr<ResultFile>> files_;
  Id next_ = 1;
};

ResultStore& results(App& app);

// 파생 스칼라(RES-06). name: magnitude(벡터) / mises, tresca, p1, p2, p3, pressure(텐서).
std::vector<std::string> derived_names(const ResultField& f);
std::vector<double> derived_values(const ResultField& f, const std::string& name);

// 전개된 쉘·보 결과(RES-53·54·08): 결과에만 있는 노드(솔버가 두께·단면 방향으로 펼친 노드)를 모델의 쉘 노드에 대응시킨다.
struct ShellExpansion {
  std::map<Id, std::vector<Id>> top, bottom, mid;  // 모델 노드 → 결과 노드(법선 양쪽 / 음쪽 / 중립면)
  std::map<Id, double> thickness;                  // 모델 노드에서 잰 전개 두께(양쪽 끝 사이 거리)
  std::size_t expanded = 0;                        // 대응된 결과 노드 수
};
ShellExpansion shell_expansion(const App& app, const ResultFile& f);
// 면(top | bottom | mid)으로 고른 값: 모델 노드 → 값(top/bottom 은 그 노드들의 평균, mid 는 중립면 노드가 있으면 그 값, 없으면 양쪽 평균)
std::map<Id, double> shell_face_values(const App& app, const ResultFile& f, const ResultField& field, const std::string& component, const std::string& face);
// 스텝에서 제거된 요소(RES-59): 케이스의 그 스텝까지 누적된 *MODEL CHANGE 로 빠진 요소. 결과 프레임의 스텝 번호로 찾는다
std::set<Id> removed_elements_in_step(const App& app, Id case_id, int step);

void register_result_commands(App& app);

}  // namespace nasa95
