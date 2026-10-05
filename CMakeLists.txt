cmake_minimum_required(VERSION 3.24)
project(MMDX12 VERSION 1.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(MSVC)
  add_compile_options(/utf-8 /W3 /MP /permissive- /Zc:__cplusplus)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
endif()

# the app version (logged at start; release zips are named after it)
add_compile_definitions(MMDX12_VERSION="${PROJECT_VERSION}")

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

# Bullet Physics (LinearMath + Collision + Dynamics only). Not built from its *All.cpp unity
# files: btVector3.cpp defines BT_USE_SSE_IN_API, which breaks the files after it.
file(GLOB_RECURSE BULLET_SRC CONFIGURE_DEPENDS
  ${EXT}/bullet/src/LinearMath/*.cpp
  ${EXT}/bullet/src/BulletCollision/*.cpp
  ${EXT}/bullet/src/BulletDynamics/*.cpp)
add_library(bullet STATIC ${BULLET_SRC})
target_include_directories(bullet SYSTEM PUBLIC ${EXT}/bullet/src)
if(MSVC)
  target_compile_options(bullet PRIVATE /W0)
endif()

# ---- engine modules --------------------------------------------------------
# core + asset: file formats, text conversion, library scanning (no GPU)
file(GLOB CORE_SRC CONFIGURE_DEPENDS src/core/*.cpp src/asset/*.cpp)
if(NOT CORE_SRC)
  set(CORE_SRC ${CMAKE_SOURCE_DIR}/cmake/empty.cpp)
endif()
# ufbx (FBX/OBJ import) is plain C; cgltf is header-only (implemented in GltfImport.cpp).
add_library(ufbx STATIC ${EXT}/ufbx/ufbx.c)
target_include_directories(ufbx SYSTEM PUBLIC ${EXT}/ufbx)
if(MSVC)
  target_compile_options(ufbx PRIVATE /W0)
endif()
add_library(mmdx_core STATIC ${CORE_SRC})
target_include_directories(mmdx_core PUBLIC src ${EXT}/stb ${EXT}/json ${EXT}/miniaudio)
target_include_directories(mmdx_core SYSTEM PRIVATE ${EXT}/cgltf)
target_link_libraries(mmdx_core PRIVATE ufbx)

# anim: skeleton pose, IK, morphs, VMD evaluation (no GPU)
file(GLOB ANIM_SRC CONFIGURE_DEPENDS src/anim/*.cpp)
if(NOT ANIM_SRC)
  set(ANIM_SRC ${CMAKE_SOURCE_DIR}/cmake/empty.cpp)
endif()
add_library(mmdx_anim STATIC ${ANIM_SRC})
target_link_libraries(mmdx_anim PUBLIC mmdx_core PRIVATE bullet)

# render: D3D12 device, GPU resources, render passes
file(GLOB RENDER_SRC CONFIGURE_DEPENDS src/render/*.cpp)
if(NOT RENDER_SRC)
  set(RENDER_SRC ${CMAKE_SOURCE_DIR}/cmake/empty.cpp)
endif()
add_library(mmdx_render STATIC ${RENDER_SRC})
target_link_libraries(mmdx_render PUBLIC mmdx_core Microsoft::DirectX-Headers Microsoft::DirectX-Guids d3d12 dxgi d3dcompiler)
# Upscaler SDKs (headers + import libs vendored; runtime DLLs fetched by tools/fetch_sdks.ps1).
# DLSS: static NGX loader (loads nvngx_dlss.dll itself). XeSS: delay-loaded so a missing
# libxess.dll only disables XeSS. FSR: amd_fidelityfx_loader_dx12.dll via LoadLibrary.
target_include_directories(mmdx_render PRIVATE ${EXT}/dlss/include ${EXT}/xess/include ${EXT}/ffx/api/include ${EXT}/ffx/upscalers/include)
target_link_libraries(mmdx_render PUBLIC
  $<IF:$<CONFIG:Debug>,${EXT}/dlss/lib/nvsdk_ngx_d_dbg.lib,${EXT}/dlss/lib/nvsdk_ngx_d.lib>
  ${EXT}/xess/lib/libxess.lib delayimp)
target_link_options(mmdx_render INTERFACE /DELAYLOAD:libxess.dll)

# app: window, screens, audio, benchmark, leaderboard
file(GLOB APP_SRC CONFIGURE_DEPENDS src/app/*.cpp src/audio/*.cpp src/studio/*.cpp)
if(NOT APP_SRC)
  set(APP_SRC ${CMAKE_SOURCE_DIR}/cmake/empty.cpp)
endif()
add_library(mmdx_app STATIC ${APP_SRC})
target_include_directories(mmdx_app PUBLIC ${EXT}/miniaudio)
target_link_libraries(mmdx_app PUBLIC mmdx_core mmdx_anim mmdx_render imgui winhttp mfplat mfreadwrite mfuuid)

if(EXISTS ${CMAKE_SOURCE_DIR}/src/main.cpp)
  add_executable(MMDX12 WIN32 src/main.cpp)
  target_link_libraries(MMDX12 PRIVATE mmdx_app)
  # Shaders are compiled at runtime; keep the copy next to the exe in sync on every build.
  add_custom_target(copy_shaders ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/shaders ${CMAKE_BINARY_DIR}/bin/shaders)
  add_custom_target(copy_assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets ${CMAKE_BINARY_DIR}/bin/assets)
  add_dependencies(MMDX12 copy_shaders copy_assets)

  # Runtime DLLs next to the exe: DXC (shader model 6.x, from the Windows SDK) and the
  # upscaler runtimes that tools/fetch_sdks.ps1 put into external/*/bin.
  set(RUNTIME_DLLS "")
  file(TO_CMAKE_PATH "$ENV{WindowsSdkVerBinPath}" SDK_BIN)
  if(NOT SDK_BIN)
    file(GLOB SDK_BINS "C:/Program Files (x86)/Windows Kits/10/bin/10.*")
    list(SORT SDK_BINS)
    list(POP_BACK SDK_BINS SDK_BIN)
  endif()
  foreach(dll dxcompiler.dll dxil.dll)
    if(EXISTS "${SDK_BIN}/x64/${dll}")
      list(APPEND RUNTIME_DLLS "${SDK_BIN}/x64/${dll}")
    else()
      message(WARNING "${dll} not found in ${SDK_BIN}/x64: ray tracing will be unavailable")
    endif()
  endforeach()
  file(GLOB SDK_DLLS CONFIGURE_DEPENDS ${EXT}/dlss/bin/*.dll ${EXT}/xess/bin/*.dll ${EXT}/ffx/bin/*.dll)
  list(APPEND RUNTIME_DLLS ${SDK_DLLS})
  add_custom_target(copy_runtime_dlls ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RUNTIME_DLLS} ${CMAKE_BINARY_DIR}/bin)
  add_dependencies(MMDX12 copy_runtime_dlls)
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
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/vmd_roundtrip.cpp)
  add_executable(vmd_roundtrip tools/vmd_roundtrip.cpp)
  target_link_libraries(vmd_roundtrip PRIVATE mmdx_core)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/anim_probe.cpp)
  add_executable(anim_probe tools/anim_probe.cpp)
  target_link_libraries(anim_probe PRIVATE mmdx_anim)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/studio_edit_test.cpp)
  add_executable(studio_edit_test tools/studio_edit_test.cpp)
  target_link_libraries(studio_edit_test PRIVATE mmdx_app)
endif()
# Pose-editing unit tests build only the module under test (not the whole app).
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/studio_gizmo_test.cpp)
  add_executable(studio_gizmo_test tools/studio_gizmo_test.cpp src/studio/Gizmo.cpp)
  target_include_directories(studio_gizmo_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(studio_gizmo_test PRIVATE mmdx_anim imgui)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/studio_pose_test.cpp)
  add_executable(studio_pose_test tools/studio_pose_test.cpp src/studio/StudioPose.cpp)
  target_include_directories(studio_pose_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(studio_pose_test PRIVATE mmdx_core)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/studio_project_test.cpp)
  add_executable(studio_project_test tools/studio_project_test.cpp src/studio/StudioProject.cpp src/studio/StudioMotion.cpp)
  target_include_directories(studio_project_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  # StudioMotion.cpp needs Bezier (src/anim/Motion.cpp and its ModelInstance/Physics chain),
  # which mmdx_anim builds; mmdx_core carries core/asset and the json include dir.
  target_link_libraries(studio_project_test PRIVATE mmdx_core mmdx_anim)
endif()
