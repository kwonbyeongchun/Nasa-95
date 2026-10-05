// 내장 객체 종류의 등록 순서. 워크 트리의 가지 순서이기도 하다.
// 형상·메시·결과에 딸린 종류(파트, 피처, 스케치, 메시 파트, 결과 파일 …)는 그 영역을 구현할 때 추가한다.
#include "kinds.hpp"

namespace nasa95 {

void register_builtin_kinds(Schema& s) {
  kinds::register_geometry(s);  // 파트·피처
  kinds::register_common(s);
  kinds::register_material(s);
  kinds::register_property(s);
  kinds::register_case(s);  // 케이스·스텝이 하중·BC 의 상위 객체다
  kinds::register_load(s);
}

}  // namespace nasa95
