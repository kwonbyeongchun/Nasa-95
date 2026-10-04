pybind11_add_module(_ofep module.cpp)
target_link_libraries(_ofep PRIVATE ofep_core)
if(TARGET ofep_render)
  target_link_libraries(_ofep PRIVATE ofep_render)
endif()

# 개발 중에는 빌드한 모듈을 python/openfep 에 바로 둔다(PYTHONPATH=python 으로 사용).
set(_ofep_out "${CMAKE_SOURCE_DIR}/python/openfep")
set_target_properties(_ofep PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${_ofep_out}"
  RUNTIME_OUTPUT_DIRECTORY "${_ofep_out}")
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set_target_properties(_ofep PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY_${cfg} "${_ofep_out}"
    RUNTIME_OUTPUT_DIRECTORY_${cfg} "${_ofep_out}")
endforeach()

# 실행할 때 필요한 DLL 폴더 목록(한 줄에 하나)
string(REPLACE ";" "\n" _dll_dirs "${OFEP_DLL_DIRS}")
file(WRITE "${_ofep_out}/_dll_dirs.txt" "${_dll_dirs}\n")