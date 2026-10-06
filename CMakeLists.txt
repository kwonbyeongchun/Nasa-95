cmake_minimum_required(VERSION 3.24)
project(open_fep VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
  add_compile_options(/utf-8 /W4 /permissive- /MP)
endif()

option(NASA95_BUILD_PYTHON "Python 모듈(_nasa95)을 빌드한다" ON)

include(FetchContent)
# nlohmann/json (MIT) — 명령 매개변수·모델 직렬화
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d)
FetchContent_MakeAvailable(nlohmann_json)

add_subdirectory(core)

# Vulkan SDK (헤더·로더 라이브러리·셰이더 컴파일러). 없으면 렌더러 없이 빌드한다.
set(NASA95_VULKAN_SDK "${CMAKE_SOURCE_DIR}/../third_party/vulkan/sdk" CACHE PATH "Vulkan SDK 폴더")
if(EXISTS "${NASA95_VULKAN_SDK}/Include/vulkan/vulkan.h")
  message(STATUS "Vulkan SDK: ${NASA95_VULKAN_SDK}")
  add_subdirectory(render)
else()
  message(STATUS "Vulkan SDK 없음: 렌더러 없이 빌드한다 (${NASA95_VULKAN_SDK})")
endif()

if(NASA95_BUILD_PYTHON)
  set(PYBIND11_FINDPYTHON ON)
  find_package(pybind11 CONFIG REQUIRED)
  add_subdirectory(bindings/python)
endif()
