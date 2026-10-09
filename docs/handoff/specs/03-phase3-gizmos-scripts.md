# Phase 3: viewport gizmos, script commands and capture checks (MMDX12 studio lights)

## Goal
Make the scene lights editable in the viewport and scriptable, and check the result with captures. The model
(src/studio/SceneLight.h/.cpp), the save format and the editing UI (src/app/UiStudioLight.cpp, Phase 2) already exist and
must not change in their names or semantics. Renderer and shader changes are NOT part of this phase.

## Product requirements (decided by the user)
- Every light gets viewport handles, not only inspector fields.
  - Point light: a translate gizmo at its position; a range handle on a circle around it.
  - Sun: rotate rings around a pivot at the performer centre (or {0,10,0} when there is no performer), plus an arrow
    that shows the light direction. Read-only while the sun is VMD-linked and the camera VMD has light keys.
  - Spot: a translate gizmo at its position; an aim-point translate gizmo only in the 수동 (manual) aim mode, with a line
    from the position to the aim point; a cone handle on the rim at the aim distance that sets the outer cone angle; a
    range handle along the spot axis. Target and sway modes show no aim handle, but the cone and range handles remain.
  - Ambient: no handles.
- Direction visualisation is a bonus: spot cone outline, point range circle, sun arrow. Draw them only when
  viewportVisible is true. They never affect the render.
- Script commands for headless tests (replace nothing that exists; the old studiolight and studiokeyspot branches were
  removed in Phase 1): studiolightpreset <0-3>, studiolightadd <sun|point|spot|ambient>, studiolightdel <index>,
  studiolightsel <index>, studiolightset <index> <field> <args...>, studiolightkey <index> [frame], and
  studiolightgizmo <part> <dx> <dy> [steps] (drags one light gizmo part, like studiogizmo does for bones). studiostate logs
  one STUDIOLIGHT line per light.

## Architecture direction
- The gizmo drawing, hit tests and drag maths reuse src/studio/Gizmo.h (GizmoFrame, GizmoStyle, GizmoHitTest, DrawGizmo,
  BeginGizmoDrag, GizmoDragTranslation, GizmoDragAngle). Only the cone and range handles need new solver maths:
  build the mouse ray (perspective, and orthographic for the quad view via ViewProj::ortho), intersect it with a plane
  (the cone's cross-section plane at the aim distance; a plane facing the camera for the range), and derive the angle
  or distance. Put these helpers in src/studio/Gizmo.cpp and test them in tools/studio_gizmo_test.cpp.
- New viewport drag state: studioViewDrag_ = 8. Codes 0, 1, 2, 3, 4, 6 and 7 are in use; 5 is free but leave it alone.
  The light handle function sits next to StudioViewportCameraHandles in src/app/UiStudioPose.cpp.
- Each gesture is one undo step (LightsCommand, or the existing edit wrappers StudioBeginLightEdit / StudioEndLightEdit).
  Values update live during the drag and the step is pushed on release. Escape restores the values from before the drag.
- Use the same aim resolution as the renderer (BuildSceneLighting in src/app/Lighting.cpp: manual aim, character target,
  sway), so the handles sit where the light really points. Share the code; do not copy it.
- Keyed edit rule (unchanged from Phase 2): a key at the playhead is updated; else with auto-key on a key is inserted;
  else the base values change.
- Pick the light by clicking its handles or its icon in the viewport; a click on empty space keeps the selection.

## Constraints
- No renderer or shader change: nothing under render/ or shaders/. Play mode, lobby and benchmark are untouched.
- Do not change the save format, SceneLight field names, or BuildLighting.
- No new light types.
- Do not commit and do not change git state.
- NEVER stop, kill or close any running process (a previous run force-stopped a running MMDX12.exe). If a build or a run
  is blocked by a locked file, stop and report it instead.

## Non-goals
Renderer support for the shadow types, softness, density, falloff and affect switches (Phase 4). The sky-glow issue
(SkyColor glows toward the sun direction even when the sun is off) is also Phase 4.

## Verification
1. E:\repos\MMDX12\build.cmd build_dev must finish with 0 errors.
2. These must build and pass: build_dev\bin\studio_project_test.exe, studio_light_test.exe, studio_edit_test.exe,
   studio_gizmo_test.exe (with the new solver tests), studio_pose_test.exe.
3. Captures, strictly one app run at a time, at most 4 runs in total. Read the capture procedure in CLAUDE.md
   (Visual verification). Write PNGs and the log to build_dev\captures_phase3\. Then check build_dev\bin\mmdx12.log for
   [E] lines. The runs must show: (a) the concert preset with a spot selected, handles and cone outline visible;
   (b) a script that drags a cone handle, with a STUDIOLIGHT line in the log showing the new outer angle;
   (c) a sun selected, with its rotate rings and arrow visible. List every run (command line and result) in your report.

## Start here
src/studio/Gizmo.h, src/studio/Gizmo.cpp, tools/studio_gizmo_test.cpp, src/app/UiStudioPose.cpp (bone gizmo drag around
lines 400-560; StudioViewportCameraHandles around 580-655; StudioScriptGizmoPoint), src/app/UiStudio.cpp (DrawStudioViewport
and the viewport input order), src/app/UiStudioLight.cpp (Phase 2 light UI and StudioBeginLightEdit / StudioEndLightEdit),
src/app/App.h (studioViewDrag_ and the gizmo members), src/app/UiScript.cpp (studiogizmo, studiostate), src/app/Lighting.cpp
(BuildSceneLighting, the aim resolution), src/studio/SceneLight.h.

## Off limits
external/, library/, library.zip, captures/ (read only; you may read the example scripts in captures\studio), videos/,
dist/, build/, build_au/, build_release/, CLAUDE.local.md, render/, shaders/, progress.md, docs/. build_dev is allowed.

## Decided (do not re-research)
- Gizmo maths: reuse src/studio/Gizmo.h. New solvers only for the cone and range handles.
- Drag state: studioViewDrag_ = 8.
- Target and sway modes hide the aim-point handle.
- The sun gizmo is read-only while VMD-linked with camera light keys.
- One undo step per gesture; Escape cancels a drag.
