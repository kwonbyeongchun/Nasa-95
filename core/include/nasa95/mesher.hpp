#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "nasa95/app.hpp"

namespace nasa95 {

// 자동 메셔의 인터페이스(MSH-05~08). 메셔 라이브러리(Netgen)는 이 뒤에 있고, 다른 메셔로 바꾸거나 더할 수 있다.
struct MesherRequest {
  int dimension = 3;        // 3 = 솔리드를 사면체로, 2 = 면을 삼각형으로
  int order = 1;            // 1차 / 2차(중간 절점은 형상 위에 놓는다)
  double max_size = 0;      // 요소 크기의 상한
  double min_size = 0;      // 하한(0 = 제한 없음)
  double grading = 0.3;     // 크기가 변하는 빠르기(0 에 가까울수록 고르게)
  std::vector<std::pair<int, double>> face_sizes;    // (면 번호, 크기)
  std::vector<std::pair<int, double>> edge_sizes;    // (모서리 번호, 크기)
  std::vector<std::pair<int, double>> vertex_sizes;  // (꼭짓점 번호, 크기)
  double curvature_safety = 0;  // 곡률 반지름당 요소 수(0 = 메셔 기본값)
};

// 메셔의 결과. 절점은 0 부터 센 위치로 가리킨다. 2차 요소는 꼭짓점 뒤에 중간 절점이 온다(순서는 메셔의 것 그대로).
struct MesherBlock {
  int nodes_per = 0;
  std::vector<std::int64_t> conn;
  std::vector<int> tag;  // 요소가 속한 형상 엔티티 번호(1 부터): 솔리드 / 면 / 모서리
};
struct MesherOutput {
  std::vector<double> xyz;
  MesherBlock volume, surface, edge;
};

bool mesher_available();
std::string mesher_name();

// 메싱할 형상. 주 스레드에서 만들고(형상 캐시를 읽는다), 그 뒤에는 어느 스레드에서나 메싱할 수 있다.
// 형상 커널의 타입은 밖으로 내지 않는다.
struct MesherGeometry {
  std::shared_ptr<const void> shape;
};
MesherGeometry prepare_mesher_geometry(App& app, Id part);
// 형상을 메싱한다. 실패하면 Error("mesh_failed"), 중간에 멈추면 Error("cancelled"). 메셔는 전역 상태를 쓰므로 한 번에 하나만 돈다.
MesherOutput run_mesher(const MesherGeometry& geometry, const MesherRequest& request);
MesherOutput run_mesher(App& app, Id part, const MesherRequest& request);
// 돌고 있는 메싱의 진행률(0~100)과 하는 일. 돌고 있지 않으면 {0, ""}.
struct MesherProgress {
  double percent = 0;
  std::string task;
  bool running = false;
};
MesherProgress mesher_progress();
// 돌고 있는 메싱을 멈추게 한다(멈추기까지 조금 걸린다).
void mesher_cancel();

void register_meshgen_commands(App& app);
// 형상 대상([파트, 종류, 번호]의 목록)을 메시 번호로 푼다. what: nodes | elements | faces.
Json resolve_geometry_target(const App& app, const Json& ids, const std::string& what);

}  // namespace nasa95
