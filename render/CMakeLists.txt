# 렌더러: Vulkan 을 직접 쓴다(렌더링 라이브러리 없음). 셰이더는 빌드할 때 SPIR-V 로 바꿔 코드에 넣는다.
set(_sdk "${OFEP_VULKAN_SDK}")
find_program(OFEP_GLSLC glslc PATHS "${_sdk}/Bin" NO_DEFAULT_PATH REQUIRED)

set(_shader_out "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${_shader_out}")
set(_shader_incs)
foreach(_name mesh.vert mesh.frag)
  set(_src "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_name}")
  set(_inc "${_shader_out}/${_name}.inc")
  add_custom_command(OUTPUT "${_inc}"
    COMMAND "${OFEP_GLSLC}" -O -mfmt=num -o "${_inc}" "${_src}"
    DEPENDS "${_src}" VERBATIM)
  list(APPEND _shader_incs "${_inc}")
endforeach()

add_library(ofep_render STATIC src/vk_renderer.cpp src/view.cpp src/navigation_cube.cpp ${_shader_incs})
target_include_directories(ofep_render PUBLIC include PRIVATE "${_shader_out}")
target_include_directories(ofep_render SYSTEM PRIVATE "${_sdk}/Include")
target_link_libraries(ofep_render PUBLIC ofep_core PRIVATE "${_sdk}/Lib/vulkan-1.lib")
target_compile_definitions(ofep_render PUBLIC OFEP_WITH_RENDER)
