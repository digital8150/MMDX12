cmake_minimum_required(VERSION 3.24)
project(MMDX12 VERSION 3.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(MSVC)
  add_compile_options(/utf-8 /W3 /MP /permissive- /Zc:__cplusplus)
  # Stated explicitly: a CMake cache whose CMAKE_CXX_FLAGS* came up empty once produced a release exe without
  # /O2, NDEBUG and /EHsc (physics ran at 2-10 fps). These duplicate the defaults harmlessly.
  add_compile_options(/EHsc "$<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:/O2>")
  add_compile_definitions("$<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:NDEBUG>")
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
# Widget registry for automation (MCP ui_items / ui_click) through imgui's test-engine hooks; the hook
# implementation lives in src/ui_probe so external/ stays untouched.
target_sources(imgui PRIVATE src/ui_probe/UiProbe.cpp)
target_compile_definitions(imgui PUBLIC IMGUI_ENABLE_TEST_ENGINE)
target_include_directories(imgui PRIVATE src)
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
# The scene light core (studio/SceneLight.h: kind / aim / shadow / falloff names, key sampling, presets) is pure data
# and pure math, and the asset layer needs it: an MMDX stage.json's "lights" use the project file's schema
# (asset/SceneLightJson), so asset/StageManifest calls into it. It lives in mmdx_core once; mmdx_app links it from here.
target_sources(mmdx_core PRIVATE src/studio/SceneLight.cpp)
# Media Foundation: asset/VideoReader (IMFSourceReader) and the RIFF AVI reader. mfreadwrite pulls the source
# reader in; the app already links these too (VideoEncoder).
target_link_libraries(mmdx_core PRIVATE ufbx mfplat mfreadwrite mfuuid)

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
target_link_libraries(mmdx_render PUBLIC mmdx_core Microsoft::DirectX-Headers Microsoft::DirectX-Guids d3d12 dxgi d3dcompiler version)
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
# studio/SceneLight.* is pure data + math and lives in mmdx_core (the asset layer's StageManifest uses it): never
# compile it twice.
list(REMOVE_ITEM APP_SRC ${CMAKE_SOURCE_DIR}/src/studio/SceneLight.cpp)
if(NOT APP_SRC)
  set(APP_SRC ${CMAKE_SOURCE_DIR}/cmake/empty.cpp)
endif()
add_library(mmdx_app STATIC ${APP_SRC})
target_include_directories(mmdx_app PUBLIC ${EXT}/miniaudio)
target_link_libraries(mmdx_app PUBLIC mmdx_core mmdx_anim mmdx_render imgui winhttp mfplat mfreadwrite mfuuid)

if(EXISTS ${CMAKE_SOURCE_DIR}/src/main.cpp)
  add_executable(MMDX12 WIN32 src/main.cpp)
  # dbghelp: the crash handler in main.cpp (SymInitialize / StackWalk64 / SymFromAddr).
  target_link_libraries(MMDX12 PRIVATE mmdx_app dbghelp)
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
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/pack_suggest_test.cpp)
  add_executable(pack_suggest_test tools/pack_suggest_test.cpp)
  # ShaderPack manifest parsing lives in mmdx_render.
  target_link_libraries(pack_suggest_test PRIVATE mmdx_render)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/render_smoke.cpp)
  add_executable(render_smoke tools/render_smoke.cpp)
  target_link_libraries(render_smoke PRIVATE mmdx_render)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/vmd_roundtrip.cpp)
  add_executable(vmd_roundtrip tools/vmd_roundtrip.cpp)
  target_link_libraries(vmd_roundtrip PRIVATE mmdx_core)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/pmm_probe.cpp)
  add_executable(pmm_probe tools/pmm_probe.cpp)
  target_link_libraries(pmm_probe PRIVATE mmdx_core)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/pack_check.cpp)
  add_executable(pack_check tools/pack_check.cpp)
  # CompileShaderDxc comes from mmdx_render (dxcompiler / dxcapi); the manifest parsing from mmdx_core (json, NetUtil).
  target_link_libraries(pack_check PRIVATE mmdx_render)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/stage_check.cpp)
  # The MMDX stage format (asset/StageManifest, asset/StageEval) and the model load it validates; the same entry
  # point registers this file as define-able from tools (the manifest parser itself is mmdx_core).
  add_executable(stage_check tools/stage_check.cpp)
  target_link_libraries(stage_check PRIVATE mmdx_core)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/shader_choice_test.cpp)
  add_executable(shader_choice_test tools/shader_choice_test.cpp src/app/Settings.cpp)
  target_include_directories(shader_choice_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  # Settings.cpp now needs imgui.h for the rebindable key defaults (AppSettings::KeyFor).
  target_link_libraries(shader_choice_test PRIVATE mmdx_render imgui)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/rest_pose_test.cpp)
  add_executable(rest_pose_test tools/rest_pose_test.cpp)
  target_include_directories(rest_pose_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(rest_pose_test PRIVATE mmdx_core)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/raymat_test.cpp)
  add_executable(raymat_test tools/raymat_test.cpp)
  target_link_libraries(raymat_test PRIVATE mmdx_core)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/gltf_pbr_test.cpp)
  add_executable(gltf_pbr_test tools/gltf_pbr_test.cpp)
  target_link_libraries(gltf_pbr_test PRIVATE mmdx_core)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/raymat_probe.cpp)
  add_executable(raymat_probe tools/raymat_probe.cpp)
  target_link_libraries(raymat_probe PRIVATE mmdx_core)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/pbr_tex_probe.cpp)
  add_executable(pbr_tex_probe tools/pbr_tex_probe.cpp)
  target_link_libraries(pbr_tex_probe PRIVATE mmdx_core)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/mme_parse_test.cpp)
  add_executable(mme_parse_test tools/mme_parse_test.cpp)
  target_link_libraries(mme_parse_test PRIVATE mmdx_core)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/mme_compile_test.cpp)
  add_executable(mme_compile_test tools/mme_compile_test.cpp)
  target_link_libraries(mme_compile_test PRIVATE mmdx_render)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/mme_script_test.cpp)
  add_executable(mme_script_test tools/mme_script_test.cpp)
  target_link_libraries(mme_script_test PRIVATE mmdx_render)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/dds_test.cpp)
  add_executable(dds_test tools/dds_test.cpp)
  target_link_libraries(dds_test PRIVATE mmdx_core)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/anim_texture_test.cpp)
  add_executable(anim_texture_test tools/anim_texture_test.cpp)
  target_link_libraries(anim_texture_test PRIVATE mmdx_core)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/anim_probe.cpp)
  add_executable(anim_probe tools/anim_probe.cpp)
  target_link_libraries(anim_probe PRIVATE mmdx_anim)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/beat_probe.cpp)
  add_executable(beat_probe tools/beat_probe.cpp)
  target_link_libraries(beat_probe PRIVATE mmdx_app)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/studio_edit_test.cpp)
  add_executable(studio_edit_test tools/studio_edit_test.cpp)
  target_link_libraries(studio_edit_test PRIVATE mmdx_app)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/studio_light_test.cpp)
  add_executable(studio_light_test tools/studio_light_test.cpp)
  target_link_libraries(studio_light_test PRIVATE mmdx_app)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/restpose_retarget_test.cpp)
  add_executable(restpose_retarget_test tools/restpose_retarget_test.cpp)
  target_link_libraries(restpose_retarget_test PRIVATE mmdx_app)
endif()
# Pose-editing unit tests build only the module under test (not the whole app).
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/studio_gizmo_test.cpp)
  add_executable(studio_gizmo_test tools/studio_gizmo_test.cpp src/studio/Gizmo.cpp)
  target_include_directories(studio_gizmo_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(studio_gizmo_test PRIVATE mmdx_anim imgui)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/studio_pose_test.cpp)
  add_executable(studio_pose_test tools/studio_pose_test.cpp src/studio/StudioPose.cpp src/studio/AutoIk.cpp)
  target_include_directories(studio_pose_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(studio_pose_test PRIVATE mmdx_core)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/studio_project_test.cpp)
  # SceneLight.cpp comes from mmdx_core (see above), never listed twice.
  add_executable(studio_project_test tools/studio_project_test.cpp src/studio/StudioProject.cpp src/studio/StudioMotion.cpp
                                     src/studio/StudioEffects.cpp)
  target_include_directories(studio_project_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  # StudioMotion.cpp needs Bezier (src/anim/Motion.cpp and its ModelInstance/Physics chain),
  # which mmdx_anim builds; mmdx_core carries core/asset and the json include dir.
  target_link_libraries(studio_project_test PRIVATE mmdx_core mmdx_anim)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/gallery_index_test.cpp)
  # The gallery index parser (app/ShaderPackStore.cpp) without the whole app; ShaderPackRegistry comes from
  # mmdx_render, and ShaderPackStore.cpp's NetUtil pulls WinHTTP.
  add_executable(gallery_index_test tools/gallery_index_test.cpp src/app/ShaderPackStore.cpp)
  target_include_directories(gallery_index_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(gallery_index_test PRIVATE mmdx_render winhttp)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/stage_store_test.cpp)
  # The stage gallery data layer + install core (app/StageStore.cpp) without the whole app; NetUtil pulls WinHTTP and
  # the real tar.exe extractor, the asset layer parses stage.json.
  add_executable(stage_store_test tools/stage_store_test.cpp src/app/StageStore.cpp)
  target_include_directories(stage_store_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(stage_store_test PRIVATE mmdx_core winhttp)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/pack_guard_test.cpp)
  add_executable(pack_guard_test tools/pack_guard_test.cpp src/render/PackCrashGuard.cpp)
  target_include_directories(pack_guard_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(pack_guard_test PRIVATE mmdx_core)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/look_preset_test.cpp)
  # The Look data layer (render) and its settings mapping (app), without the whole app.
  add_executable(look_preset_test tools/look_preset_test.cpp src/render/LookPreset.cpp src/app/LookApply.cpp)
  target_include_directories(look_preset_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  # app/Settings.h reaches render/Dx12Context.h, whose d3dx12.h comes from DirectX-Headers.
  target_link_libraries(look_preset_test PRIVATE mmdx_core Microsoft::DirectX-Headers)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/world_track_test.cpp)
  # The collider track sampler (render/WorldTypes.cpp), without the whole renderer.
  add_executable(world_track_test tools/world_track_test.cpp src/render/WorldTypes.cpp)
  target_include_directories(world_track_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(world_track_test PRIVATE Microsoft::DirectX-Headers)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/spot_shadow_pick_test.cpp)
  add_executable(spot_shadow_pick_test tools/spot_shadow_pick_test.cpp src/render/SpotShadowPick.cpp)
  target_include_directories(spot_shadow_pick_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(spot_shadow_pick_test PRIVATE Microsoft::DirectX-Headers)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/studio_focus_test.cpp)
  add_executable(studio_focus_test tools/studio_focus_test.cpp src/studio/StudioFocus.cpp src/studio/StudioProject.cpp
                                   src/studio/StudioMotion.cpp src/studio/StudioEffects.cpp)
  target_include_directories(studio_focus_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(studio_focus_test PRIVATE mmdx_core mmdx_anim)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/bake_test.cpp)
  # The physics bake core plus the VMD evaluation it drives (Bezier lives in mmdx_anim).
  add_executable(bake_test tools/bake_test.cpp src/studio/PhysicsBake.cpp src/studio/StudioMotion.cpp)
  target_include_directories(bake_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(bake_test PRIVATE mmdx_core mmdx_anim)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/mmdx12_mcp.cpp)
  add_executable(mmdx12_mcp tools/mmdx12_mcp.cpp)
  target_include_directories(mmdx12_mcp PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(mmdx12_mcp PRIVATE mmdx_core)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/video_probe.cpp)
  # asset/VideoReader (mmdx_core) + the PNG writer (ImageLoader).
  add_executable(video_probe tools/video_probe.cpp)
  target_link_libraries(video_probe PRIVATE mmdx_core)
endif()

# ---- i18n check ------------------------------------------------------------
# Every Tr() / TrKey() key needs En / Ja / Zh entries: the build fails on a missing key (tools/i18n_check.py).
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_custom_target(i18n_check ALL
    COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/i18n_check.py --root ${CMAKE_SOURCE_DIR}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    VERBATIM)
else()
  message(STATUS "Python 3 not found: i18n_check skipped")
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tools/mvd_test.cpp)
  add_executable(mvd_test tools/mvd_test.cpp)
  target_link_libraries(mvd_test PRIVATE mmdx_core)
endif()
