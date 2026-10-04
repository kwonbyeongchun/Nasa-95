#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "ofep/app.hpp"

namespace ofep {

// 형상(Geometry). OpenCASCADE 는 이 계층 뒤(C++ 코어)에서만 쓴다 — 헤더에는 OpenCASCADE 타입이 나오지 않는다.
// 파트의 형상은 피처 객체에서 계산한 파생 상태이고, 피처가 바뀌면 다음 조회 때 다시 계산한다.

bool geometry_available();  // OpenCASCADE 없이 빌드했으면 false (형상 명령은 not_available 오류)

// 표시용 삼각화(GEO-06). 면마다 따로 삼각화한 것을 이어 붙인다.
struct Tessellation {
  std::vector<double> points;               // 3 × 점 수
  std::vector<double> normals;              // 3 × 점 수
  std::vector<std::int64_t> triangles;      // 3 × 삼각형 수 (점의 위치, 0 부터)
  std::vector<std::int64_t> triangle_face;  // 삼각형이 속한 면 번호(1 부터)
  std::vector<double> edge_points;          // 3 × 모서리 점 수 (모서리를 꺾은선으로)
  std::vector<std::int64_t> edge_offsets;   // 모서리마다 시작 위치(+ 끝)
};
Tessellation geometry_tessellation(App& app, Id part, double deflection, double angle_deg);

// 파트의 형상이 바뀌었는지 알아보는 요약값(피처에서 계산). 메시가 최신인지 판단하는 데 쓴다.
std::string geometry_digest(App& app, Id part);
// 엔티티 개수 {solids, faces, edges, vertices} 와 꼭짓점 좌표(3 × 꼭짓점 수, 번호 순).
Json geometry_counts(App& app, Id part);
// 영속 엔티티 이름표(GEO-05, D9: 자체 이름 부여). 피처를 적용하며 새로 생긴 엔티티에 "f<피처 ID>:<종류>:<순번>" 을 붙이고,
// 앞 피처에서 살아남은(IsSame) 엔티티와 같은 기하 서명의 엔티티는 이름을 이어받는다. 재생성 뒤에도 같은 규칙이라 이름이 유지된다.
// 번호(1 부터) → 이름. type 은 solid|face|edge|vertex. 형상이 없으면 빈 문자열
std::string geometry_entity_name(App& app, Id part, const std::string& type, int index);
// 이름 → 지금 번호(없으면 0)
int geometry_entity_index(App& app, Id part, const std::string& type, const std::string& name);
std::vector<double> geometry_vertices(App& app, Id part);
// 점(3 × 점 수)마다 가장 가까운 모서리의 번호(1 부터).
std::vector<int> geometry_nearest_edges(App& app, Id part, const std::vector<double>& points);

// --- 매핑(육면체) 메싱용(MSH-07): 솔리드가 육면체 위상(면 6, 모서리 12, 꼭짓점 8)인지 보고 번호를 정리한다.
struct BlockTopology {
  bool ok = false;
  std::string reason;                       // ok 가 아니면 사유
  std::array<int, 8> corners{};             // 꼭짓점 번호(1 부터), 육면체 요소의 꼭짓점 순서(아랫면 4개 → 윗면 4개, 오른손)
  std::array<std::array<int, 3>, 12> edges{};  // {모서리 번호, 꼭짓점 자리 a, 꼭짓점 자리 b}
  std::array<std::array<int, 5>, 6> faces{};   // {면 번호, 꼭짓점 자리 4개}
};
BlockTopology geometry_block_topology(App& app, Id part, int solid);
// 모서리 위의 점 n+1 개(호 길이로 고르게). from 에 가까운 끝에서 시작한다. 3 × (n+1).
std::vector<double> geometry_edge_points(App& app, Id part, int edge, const std::array<double, 3>& from, int n);
// 호 길이 비율(0~1, 오름차순) 자리의 점들. from 에 가까운 끝이 0 이다. 3 × 비율 수.
std::vector<double> geometry_edge_points_at(App& app, Id part, int edge, const std::array<double, 3>& from, const std::vector<double>& fractions);
// 점들을 면(모서리) 위로 투영한다(제자리에서 바꾼다).
void geometry_project_to_face(App& app, Id part, int face, std::vector<double>& xyz);
void geometry_project_to_edge(App& app, Id part, int edge, std::vector<double>& xyz);

void register_geometry_commands(App& app);

}  // namespace ofep
