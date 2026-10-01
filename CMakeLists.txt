cmake_minimum_required(VERSION 3.24)
project(MMDX12 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(MSVC)
  add_compile_options(/utf-8 /W3 /MP /permissive- /Zc:__cplusplus)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
endif()

set(EXT ${CMAKE_SOURCE_DIR}/external)

# ---- third party -----------------------------------------------------------
set(DXHEADERS_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(DXHEADERS_BUILD_GOOGLE_TEST OFF CACHE BOOL "" FORCE)
set(DXHEADERS_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${EXT}/DirectX-Headers EXCLUDE_FROM_ALL)

add_library(imgui STATIC
  ${EXT}/imgui/imgui.cpp
  ${EXT}/imgui/imgui_draw.cpp
  ${EXT}/imgui/imgui_tables.cpp
  ${EXT}/imgui/imgui_widgets.cpp
  ${EXT}/imgui/imgui_demo.cpp
  ${EXT}/imgui/backends/imgui_impl_win32.cpp
  ${EXT}/imgui/backends/imgui_impl_dx12.cpp)
target_include_directories(imgui PUBLIC ${EXT}/imgui ${EXT}/imgui/backends)
target_link_libraries(imgui PUBLIC d3d12 dxgi)

# ---- engine modules --------------------------------------------------------
# core + asset: file formats, text conversion, library scanning (no GPU)
file(GLOB CORE_SRC CONFIGURE_DEPENDS src/core/*.cpp src/asset/*.cpp)
if(NOT CORE_SRC)
  set(CORE_SRC ${CMAKE_SOURCE_DIR}/cmake/empty.cpp)
endif()
add_library(mmdx_core STATIC ${CORE_SRC})
target_include_directories(mmdx_core PUBLIC src ${EXT}/stb ${EXT}/json)

# anim: skeleton pose, IK, morphs, VMD evaluation (no GPU)
file(GLOB ANIM_SRC CONFIGURE_DEPENDS src/anim/*.cpp)
if(NOT ANIM_SRC)
  set(ANIM_SRC ${CMAKE_SOURCE_DIR}/cmake/empty.cpp)
endif()
add_library(mmdx_anim STATIC ${ANIM_SRC})
target_link_libraries(mmdx_anim PUBLIC mmdx_core)

# render: D3D12 device, GPU resources, render passes
file(GLOB RENDER_SRC CONFIGURE_DEPENDS src/render/*.cpp)
if(NOT RENDER_SRC)
  set(RENDER_SRC ${CMAKE_SOURCE_DIR}/cmake/empty.cpp)
endif()
add_library(mmdx_render STATIC ${RENDER_SRC})
target_link_libraries(mmdx_render PUBLIC mmdx_core Microsoft::DirectX-Headers Microsoft::DirectX-Guids d3d12 dxgi d3dcompiler)

# app: window, screens, audio, benchmark, leaderboard
file(GLOB APP_SRC CONFIGURE_DEPENDS src/app/*.cpp src/audio/*.cpp)
if(NOT APP_SRC)
  set(APP_SRC ${CMAKE_SOURCE_DIR}/cmake/empty.cpp)
endif()
add_library(mmdx_app STATIC ${APP_SRC})
target_include_directories(mmdx_app PUBLIC ${EXT}/miniaudio)
target_link_libraries(mmdx_app PUBLIC mmdx_core mmdx_anim mmdx_render imgui winhttp)

if(EXISTS ${CMAKE_SOURCE_DIR}/src/main.cpp)
  add_executable(MMDX12 WIN32 src/main.cpp)
  target_link_libraries(MMDX12 PRIVATE mmdx_app)
  # Shaders are compiled at runtime; keep the copy next to the exe in sync on every build.
  add_custom_target(copy_shaders ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/shaders ${CMAKE_BINARY_DIR}/bin/shaders)
  add_dependencies(MMDX12 copy_shaders)
endif()

# ---- tools -----------------------------------------------------------------
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/asset_probe.cpp)
  add_executable(asset_probe tools/asset_probe.cpp)
  target_link_libraries(asset_probe PRIVATE mmdx_core)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/render_smoke.cpp)
  add_executable(render_smoke tools/render_smoke.cpp)
  target_link_libraries(render_smoke PRIVATE mmdx_render)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/anim_probe.cpp)
  add_executable(anim_probe tools/anim_probe.cpp)
  target_link_libraries(anim_probe PRIVATE mmdx_anim)
endif()
