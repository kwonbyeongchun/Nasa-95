#pragma once
#include <nlohmann/json.hpp>

namespace ofep {
// 키가 정렬되는 기본 json 을 쓴다(다이제스트·저장 결과가 입력 순서에 좌우되지 않게).
using Json = nlohmann::json;
}  // namespace ofep
