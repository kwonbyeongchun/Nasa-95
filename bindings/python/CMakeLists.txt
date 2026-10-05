pybind11_add_module(_nasa95 module.cpp)
target_link_libraries(_nasa95 PRIVATE nasa95_core)
if(TARGET nasa95_render)
  target_link_libraries(_nasa95 PRIVATE nasa95_render)
endif()

# 개발 중에는 빌드한 모듈을 python/nasa95 에 바로 둔다(PYTHONPATH=python 으로 사용).
set(_nasa95_out "${CMAKE_SOURCE_DIR}/python/nasa95")
set_target_properties(_nasa95 PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${_nasa95_out}"
  RUNTIME_OUTPUT_DIRECTORY "${_nasa95_out}")
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set_target_properties(_nasa95 PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY_${cfg} "${_nasa95_out}"
    RUNTIME_OUTPUT_DIRECTORY_${cfg} "${_nasa95_out}")
endforeach()

# 실행할 때 필요한 DLL 폴더 목록(한 줄에 하나)
string(REPLACE ";" "\n" _dll_dirs "${NASA95_DLL_DIRS}")
file(WRITE "${_nasa95_out}/_dll_dirs.txt" "${_dll_dirs}\n")