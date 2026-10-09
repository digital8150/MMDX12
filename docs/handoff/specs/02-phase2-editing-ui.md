# Phase 2: scene light editing UI (MMDX12 studio)

## Goal
Build the editing UI for the studio's scene lights. The model, saving and the preset builder already exist in the
working tree from Phase 1 (src/studio/SceneLight.h/.cpp, StudioDoc::lights, StudioDoc::selectedLightUid, LightsCommand,
project format version 2). Do not change those field names or the save format.

A user must be able to: see every light as an object in the outliner; select a light and edit it in the inspector;
add, delete, rename, enable and disable lights; apply a preset (it replaces the whole light list, asks first, and is
undoable with Ctrl+Z). Viewport gizmos, script commands and renderer changes are NOT part of this phase.

## Product requirements (decided by the user)
- Outliner: the camera row "카메라 VMD" stays. Below it a group "조명" with a "+" menu (점광원, 스팟; 메인 조명 only
  when no sun exists; 환경광 only when no ambient exists) and a preset menu (스튜디오, 노을, 콘서트, 밤). Each light is a
  row with a kind icon, its name and an enable eye toggle. Clicking the row selects the light.
- Selection: selecting a light sets selectedLightUid and keeps selectedModel = -1. Never use -2 (it is the audio row).
  Camera-only behaviour must not fire while a light is selected.
- Inspector for the selected light: rename (undoable), enable, delete (undoable), then by kind:
  - 태양 (sun): direction, colour with picker, intensity, rim strength and rim colour, the VMD link switch. While linked
    and the camera VMD has light keys, colour and direction are read-only and a note says so.
  - 점광원 (point): position, colour, intensity, range.
  - 스팟 (spot): aim mode 수동 / 타겟 / 자동 스윙. For 타겟: target character and part 중심 / 머리. The aim point is
    shown only for 수동. Then position, colour, intensity, range, cone outer and inner in degrees (inner not above outer).
  - 환경광 (ambient): sky zenith, horizon and ground colours, strength.
  - Shadow section (sun, point, spot): type 없음(No Cast) / 하드 / 소프트, softness (소프트 only), density 0-100 %,
    shadow colour. Point lights carry a note: the real-time raster path has no point-light shadows yet (the path tracer
    and offline GI do cast them).
  - Falloff 없음 / 선형 / 역제곱 (point and spot). Affect diffuse and affect specular switches. Viewport visible switch
    (editor only, no render effect).
  - Keys: the key count and a "현재 프레임에 키 등록" button.
- Keyed edit rule, for every edited value: if a key exists at the playhead, update it; else if StudioDoc::autoKey is on,
  insert a key at the playhead with the new value; else change the base values. One undo step per gesture.
- Preset apply: choosing a preset opens a confirmation built like the existing "##removemodel" popup
  (src/app/UiStudioProject.cpp). Title "조명 프리셋 적용". Body "프리셋으로 재설정됩니다. 계속하시겠어요?". Subtext
  "현재 조명 목록이 프리셋의 조명으로 교체됩니다. 실행 취소(Ctrl+Z)로 되돌릴 수 있습니다.". Buttons 취소 (Esc) and 적용
  (Enter). The popup must be included in StudioModal(). Applying replaces the whole light list in one LightsCommand with
  PresetLights(preset, focus, nextLightUid). focus = the performer centre as BuildStudioFrameView computes it, else {0,10,0}.
- Add defaults: 점광원 at focus + (0,40,-20), range 140, white, intensity 1. 스팟 at focus + (0,58,-18), aimed at focus,
  cone 0.24 / 0.15, intensity 2.6, range 140. Names 점광원 N and 스팟 N (first free N). 메인 조명 and 환경광 use the
  SceneLight defaults.
- Undo step names in Korean, for example 조명 추가, 조명 삭제, 조명 켜기, 조명 끄기, 조명 이름 바꾸기, 조명 편집, 프리셋 적용.
- Every new Tr() string needs an entry in src/core/I18nEn.cpp, I18nJa.cpp and I18nZh.cpp.

## Architecture direction
- Put the new UI in a new file src/app/UiStudioLight.cpp, with its App members and declarations in src/app/App.h.
  Keep the changes in UiStudio.cpp (outliner, inspector) to call-site dispatch.
- Reuse the studio UI kit (src/app/UiKit.h) and the existing key-edit pattern (StudioBeginKeyEdit / StudioEndKeyEdit)
  where it fits.
- Every edit goes through StudioDoc::history (LightsCommand, or the existing track commands). Add no new undo mechanism.
- Keep the Phase 1 model as it is. Helper functions may be added, but do not rename fields or change the save format.

## Constraints
- Do not change render/, shaders/, play mode, the lobby or the benchmark. BuildLighting and the play path stay as they are.
- Do not change the project file format.
- The camera VMD light track keeps its current behaviour.
- Use icons from src/app/Icons.h. Add a glyph there only if nothing fits.
- Do not commit and do not change git state.

## Non-goals
Viewport gizmos and viewport icons (Phase 3), script commands (Phase 3), renderer support for shadow types, softness,
falloff and affect switches (Phase 4), new light types.

## Verification
- E:\repos\MMDX12\build.cmd build_dev must finish with 0 errors.
- These must build and pass: build_dev\bin\studio_project_test.exe, studio_light_test.exe, studio_edit_test.exe,
  studio_gizmo_test.exe, studio_pose_test.exe.
- Do not launch MMDX12.exe.

## Start here
src/studio/SceneLight.h, src/studio/SceneLight.cpp, src/studio/StudioDoc.h (LightsCommand, selectedLightUid, lights),
src/app/UiStudio.cpp (DrawStudioOutliner, DrawStudioInspector, the timeline rows), src/app/UiStudioCamera.cpp
(DrawStudioCameraPanel, LightBall, DrawStudioCameraKeyFields), src/app/UiStudioProject.cpp (StudioSelectModel,
DrawStudioModelMenu, the ##removemodel popup), src/app/App.h (StudioModal, studio members), src/app/UiKit.h,
src/app/Icons.h.

## Off limits
external/, library/, library.zip, captures/, videos/, dist/, build/, build_au/, build_dev/ (except for the verification
build above), build_release/, CLAUDE.local.md, render/, shaders/.

## Decided (do not re-research)
- Light selection uses selectedLightUid with selectedModel = -1. Never -2.
- Undo goes through LightsCommand and the existing track commands only.
- The preset confirmation reuses the ##removemodel popup pattern.
- The keyed edit rule above is the rule for every light field.
