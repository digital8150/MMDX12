# Design task: studio lighting redesign (investigate, read-only)

## Goal
Design the redesign of the Studio lighting feature of MMDX12 (C++20, Direct3D 12 MikuMikuDance client; the working directory is the project root). Do not implement. Produce a concrete, file-level design that the architect (Claude) can review and then build in milestones. You may challenge any proposal below, but give evidence.

Read-only: do not create, modify or delete any file in the project. Do not build, do not run the app, do not use browser or image tools.

## What the user wants (requirements, in the user's order)
R1. The outliner object currently named "카메라 · 조명" becomes "카메라 VMD" (the camera VMD object). It keeps the camera VMD's light track (the light keys stored in the camera VMD). The user accepts that part.
R2. Presets must not live under the camera VMD object. Preset lights become real objects in the scene hierarchy, in a "조명" group. Today the preset and rig lighting hide behind a source switch inside the camera panel; that is the problem.
R3. Lights are edited in the viewport with gizmo handles, not only through inspector fields. Showing the light direction is a bonus.
R4. A preset is a "set to this preset" action: it replaces the current light set with the preset's light objects. Afterwards the preset's lights are ordinary objects in the outliner, editable like any other.
R5. Light object types as in Cinema 4D / Unreal / Unity: point light (radiates from one point), sun-like directional light (the user's word "엠비언트" seems to mean this sun), spot light. An ambient/sky light is acceptable if it fits the renderer.
R6. Example: choosing the "콘서트" preset creates several spot objects in the outliner; each has colour, position and direction editable in the inspector. Spots also get a target character (aim at a performer; the studio can hold several characters) and automatic direction (auto-aim).
R7. Spots get a viewport gizmo for direction, position and beam width (cone angle).
R8. Applying a preset discards the current light setup, so it asks first ("프리셋으로 재설정됩니다. 계속하시겠어요?") and is undoable with Ctrl+Z.

Implicit requirements: light values are keyframeable on the timeline (today's spot key rows); .mmdxproj files saved with today's "lighting" object must still load; play-mode lighting must not change; studio stills and videos must show the new lights; UI strings use Tr() with translations in src/core/I18nEn.cpp, I18nJa.cpp, I18nZh.cpp.

## Facts already checked (re-verify before relying on them)
- src/render/RenderTypes.h:101-124: PunctualLight (point when spotCosOuter <= -1, otherwise spot). LightParams has one sun (direction, color, sunIntensity), sky terms (skyZenith, skyHorizon, groundColor, hemiStrength), rim (rimStrength, rimColor) and a punctual list. src/render/Renderer.h:24: kMaxPunctualLights = 16.
- src/app/Lighting.cpp:40-114 BuildLighting: four presets (Studio, Sunset, Night, Concert). Concert = six animated spots (SwayTarget, positions relative to the performer centre, cone 0.24 / inner 0.15 rad, range 140) plus one fill point light. Lighting.cpp:116-172 BuildStudioLighting is the studio path today.
- src/studio/LightRig.h and LightRig.cpp: today's rig (source enum, KeyOverride, SpotLight with SpotKf keys, SampleSpotKeys).
- src/studio/StudioMotion.h: LightKf (VMD light key, linear), ShadowKf (stepped), FindKey / UpsertKey / EraseKeyFrames / MoveKeyFrames templates.
- src/studio/StudioDoc.h: RowKind enum (~63), TrackState and TrackEditCommand (~178-240), VecSpotCommand (~199-220), StudioDoc::lighting, autoKey (~127), selectedModel = -1 means the camera (~130).
- src/studio/CommandStack.h: Push() calls Do() immediately; Undo/Redo; Bytes() is the undo budget.
- src/studio/Gizmo.h:17-160: ViewProj (Project, PixelWorldSize, ortho flag), GizmoFrame, GizmoStyle, GizmoHitTest, DrawGizmo, BeginGizmoDrag, GizmoDragTranslation, GizmoDragAngle. Bone gizmo drag flow: src/app/UiStudioPose.cpp:400-560 (studioViewDrag_ state codes). Camera eye/target handles: UiStudioPose.cpp:580-655.
- Auto-key: a finished edit keys the value at the playhead when StudioDoc::autoKey is on (rig edit path: src/app/UiStudioCamera.cpp:169-194).
- Confirm dialog pattern: src/app/UiStudioProject.cpp:820-848 ("##removemodel"); StudioModal() guard in src/app/App.h:413.
- Outliner: src/app/UiStudio.cpp:1600-1741 (row lambda; model index -1 = camera, -2 = audio). Inspector: UiStudio.cpp:1743. Timeline rows for the camera group: UiStudio.cpp:1160-1190 (one RowKind::Spot per spot). Key helpers StudioTrackOfRow, StudioInsertKeys, StudioDeleteSelected, StudioSelectAll and the copy / move code: UiStudio.cpp:547-1140.
- Spot aim focus and head: UiStudio.cpp:504-541 (performer centre and head bones).
- Project file: src/studio/StudioProject.h:19 kProjectFormatVersion = 1; lighting written at src/studio/StudioProject.cpp:318-362, read at :560-620 (the legacy useLightTrack bool is read too). Tests: tools/studio_project_test.cpp (lighting tests ~166-370), CMakeLists.txt:177-184.
- Scripted verification: src/app/UiScript.cpp:298 (studiolight), :331 (studiokeyspot). Bone gizmo scripting goes through StudioScriptGizmoPoint.

## Decided (do not re-research)
D1. Reuse src/studio/Gizmo.h for translate and rotate handles. Only the cone and range handles need new solver math.
D2. Undo goes through CommandStack and Command subclasses. No second undo mechanism.
D3. Play mode, lobby and benchmark keep BuildLighting unchanged.
D4. The project file stays UTF-8 JSON (nlohmann) next to the standard VMD files.
D5. Object names are saved data. Default Korean names: 메인 조명 (sun), 환경광 (ambient), 점광원 N, 스팟 N, 채움광 (fill).
D6. Camera VMD export and import (camera.vmd, light keys included) keep their current format.

## Proposal to challenge
P1. Lights are objects in one flat list in the studio document. Each has a kind, a name, an enabled flag, base values, optional keys (the same idea as today's spot keys) and kind-specific fields.
P2. At most one sun and one ambient per scene, because the renderer has one sun term and one sky term. Check whether several suns are cheap in the shader; if not, keep the limit.
P3. The camera VMD light keys drive the sun while the sun's "VMD link" flag is on and the VMD track has keys. Otherwise the sun uses its own values.
P4. Preset spot positions are absolute world coordinates computed from the performer centre at apply time. Concert sway = aim base + time-based sway offset, chosen so the look matches today's concert preset.
P5. Applying a preset is one undoable command that swaps the light list.

## Start here
src/app/Lighting.cpp, src/app/Lighting.h, src/studio/LightRig.h, src/studio/LightRig.cpp, src/studio/StudioMotion.h, src/studio/StudioDoc.h, src/studio/CommandStack.h, src/studio/Gizmo.h, src/app/UiStudioCamera.cpp, src/app/UiStudio.cpp, src/app/UiStudioPose.cpp, src/app/UiStudioProject.cpp, src/studio/StudioProject.cpp, src/studio/StudioProject.h, src/app/UiScript.cpp, src/render/RenderTypes.h, src/render/Renderer.h, tools/studio_project_test.cpp, CMakeLists.txt.

## Off limits
external/, library/, library.zip, captures/, videos/, dist/, build/, build_au/, build_dev/, build_release/, CLAUDE.local.md. Do not read or list them.

## Your final message
Use these headings, in this order:
- SUMMARY (15 lines at most)
- DATA MODEL: fields per kind, keyable fields, the VMD link, signatures only, no bodies
- PRESET MAPPING: each of the four presets as an object list with values; the concert formulas mapped to fields
- RENDER MAPPING: object list to LightParams and punctual lights, including no sun, no ambient, and more than 16 lights
- EDIT AND AUTO-KEY RULES
- SELECTION: how "a light is selected" fits next to selectedModel = -1 for the camera; list the call sites that change
- OUTLINER AND TIMELINE: rows, RowKind values, TrackState / clipboard / StudioTrackOfRow identity (uid or name)
- GIZMOS: per light kind, which handles and which solver; where the drag state goes in UiStudioPose.cpp; ortho views
- UNDO: command types, granularity, what Ctrl+Z restores
- PRESET CONFIRMATION: modal flow, Enter and Esc
- PERSISTENCE: schema; whether to bump kProjectFormatVersion and what older builds do; migrating the version-1 lighting object; what cannot migrate exactly; tests to update
- SCRIPTED VERIFICATION: ui-script commands to add so the feature can be driven headlessly
- MILESTONES: ordered; each step compiles and can be tested; files per step; the riskiest step
- RISKS
- OPEN QUESTIONS: real product decisions for the user only
- EVIDENCE: file:line for every claim about the current code
- NOT VERIFIED: assumptions you did not confirm in the code

Write the report in English and quote Korean UI strings as they are. Keep the whole message under about 500 lines. Use tables and bullet lists; no function bodies.
