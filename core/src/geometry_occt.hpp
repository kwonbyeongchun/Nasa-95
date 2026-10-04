// OpenCASCADE 형상을 직접 다루는 파일들만 쓰는 내부 헤더(형상 명령, 메셔 연결).
// 이 헤더를 넣는 파일 밖으로 OpenCASCADE 타입이 나가지 않는다(아키텍처 규칙 2).
#pragma once
#ifdef OFEP_WITH_OCCT
#include <TopoDS_Shape.hxx>

#include "ofep/app.hpp"

namespace ofep {

// 파트의 현재 형상(피처를 적용한 결과). 형상이 없으면 Error("invalid_state").
const TopoDS_Shape& geometry_shape(App& app, Id part);

}  // namespace ofep
#endif
