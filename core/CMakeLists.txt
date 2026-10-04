add_library(ofep_core STATIC
  src/model.cpp
  src/schema.cpp
  src/kinds.cpp
  src/kinds_common.cpp
  src/kinds_material.cpp
  src/kinds_property.cpp
  src/kinds_case.cpp
  src/kinds_load.cpp
  src/app.cpp
  src/commands_object.cpp
  src/commands_material.cpp
  src/commands_system.cpp
  src/commands_model.cpp
  src/commands_mesh.cpp
  src/commands_mesh_ops.cpp
  src/commands_solver.cpp
  src/commands_run.cpp
  src/commands_geometry.cpp
  src/sketch_solver.cpp
  src/commands_meshgen.cpp
  src/mesher_netgen.cpp
  src/kinds_geometry.cpp
  src/commands_result.cpp
  src/results_frd.cpp
  src/process.cpp
  src/deck_reader.cpp
  src/deck_writer.cpp
  src/mesh.cpp
  src/mesh_shapes.cpp
  src/expr.cpp
)
target_include_directories(ofep_core PUBLIC include)
target_link_libraries(ofep_core PUBLIC nlohmann_json::nlohmann_json)

# OpenCASCADE (LGPL 2.1 + 예외, 동적 링크) — 형상 커널. 없으면 형상 명령 없이 빌드한다.
set(OFEP_OCCT_DIR "${CMAKE_SOURCE_DIR}/../third_party/occt/8.0.1/opencascade-8.0.1-vc14-64" CACHE PATH "OpenCASCADE 설치 폴더")
if(EXISTS "${OFEP_OCCT_DIR}/cmake/OpenCASCADEConfig.cmake")
  find_package(OpenCASCADE CONFIG REQUIRED PATHS "${OFEP_OCCT_DIR}/cmake" NO_DEFAULT_PATH)
  message(STATUS "OpenCASCADE ${OpenCASCADE_VERSION}: ${OFEP_OCCT_DIR}")
  target_compile_definitions(ofep_core PUBLIC OFEP_WITH_OCCT)
  target_include_directories(ofep_core SYSTEM PRIVATE ${OpenCASCADE_INCLUDE_DIR})
  target_link_libraries(ofep_core PUBLIC
    TKernel TKMath TKG2d TKG3d TKGeomBase TKBRep TKGeomAlgo TKTopAlgo TKPrim TKBO TKShHealing TKBool
    TKFillet TKOffset TKMesh TKDE TKXSBase TKDESTEP TKDEIGES TKCDF TKLCAF TKXCAF)  # XCAF: 어셈블리·이름·색(XDE, GEO-03)
  # 실행할 때 필요한 DLL 폴더(파이썬 패키지가 읽어 DLL 검색 경로에 더한다)
  get_filename_component(_occt_3rd "${OFEP_OCCT_DIR}/../3rdparty-vc14-64" ABSOLUTE)
  # 공식 빌드의 데이터 교환 툴킷은 시각화 툴킷(TKService)까지 끌고 오므로 그 의존 DLL 폴더도 넣는다.
  # 배포용으로는 시각화 모듈을 뺀 OpenCASCADE 를 직접 빌드해 쓴다(.agent/proj-dev-direction.md).
  get_filename_component(_occt_bin "${OFEP_OCCT_DIR}/win64/vc14/bin" ABSOLUTE)
  file(GLOB _3rd_bins "${_occt_3rd}/*/bin" "${_occt_3rd}/*/bin/win64")
  set(OFEP_DLL_DIRS "${_occt_bin}" ${_3rd_bins} CACHE INTERNAL "")

  # Netgen (LGPL 2.1, 동적 링크) — 자동 메셔. OpenCASCADE 가 있어야 쓴다. 없으면 자동 메싱 없이 빌드한다.
  set(OFEP_NETGEN_DIR "${CMAKE_SOURCE_DIR}/../third_party/netgen/install" CACHE PATH "Netgen 설치 폴더")
  if(EXISTS "${OFEP_NETGEN_DIR}/cmake/NetgenConfig.cmake")
    find_package(Netgen CONFIG REQUIRED PATHS "${OFEP_NETGEN_DIR}/cmake" NO_DEFAULT_PATH)
    message(STATUS "Netgen ${NETGEN_VERSION}: ${OFEP_NETGEN_DIR}")
    target_compile_definitions(ofep_core PUBLIC OFEP_WITH_NETGEN)
    target_include_directories(ofep_core SYSTEM PRIVATE "${OFEP_NETGEN_DIR}/include" "${OFEP_NETGEN_DIR}/include/include")
    target_link_libraries(ofep_core PUBLIC nglib ngcore)
    get_filename_component(_netgen_bin "${OFEP_NETGEN_DIR}/bin" ABSOLUTE)
    file(GLOB _zlib_bin "${_occt_3rd}/zlib-*/bin")
    set(OFEP_DLL_DIRS ${OFEP_DLL_DIRS} "${_netgen_bin}" ${_zlib_bin} CACHE INTERNAL "")
  else()
    message(STATUS "Netgen 없음: 자동 메싱 없이 빌드한다 (${OFEP_NETGEN_DIR})")
  endif()
else()
  message(STATUS "OpenCASCADE 없음: 형상 기능 없이 빌드한다 (${OFEP_OCCT_DIR})")
  set(OFEP_DLL_DIRS "" CACHE INTERNAL "")
endif()