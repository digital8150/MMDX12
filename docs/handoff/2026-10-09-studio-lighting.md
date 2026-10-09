# 세션 핸드오프: 스튜디오 scene light 리디자인 (2026-10-09)

새 세션은 이 문서부터 읽고 시작하세요. 상태, 결정, 남은 일을 한곳에 모았습니다.

## 2026-10-09 마지막 갱신 (조명 리디자인 완료) — 새 세션은 여기부터 읽으세요

- **상태**: 계획한 마일스톤 전부 완료. `94ab1fc` 점광원 그림자 상한 제거 + 콘서트 채움광 NoCast(사용자 결정), `9e28a42` 4c 면광원,
  `1412da1` PT/GI 점광원 스페큘러 하이라이트, `4d05c59` 스폿 상한 8 -> 16(`kSceneCbSize` 2560), 감쇠 "없음" -> "기본", 면광원 PCSS 24탭.
  4e(태양 꺼짐 시 하늘 글로우)는 4a에서 처리됨. 자세한 검증 수치는 progress.md (15).
- **남은 것(선택)**: 면광원 넓은 반그림자의 고정 입자 노이즈, PT/GI 바닥 하이라이트(작고 선명) 대 래스터(넓고 흐림) 모양 차이,
  4c·스페큘러 이후 "면광원/점광원 없는 장면"의 기준 빌드 비교(코드상 새 경로를 건너뜀, GI 스페큘러 끔은 비트 동일 확인).
- **기준 worktree** `E:\repos\MMDX12_base`는 아직 `fa8f863`. 다음 비교 전에 기준 커밋으로 checkout 후 `build_base` 재빌드.
- **안티그래비티**: 4c는 잘함(19분, 범위 준수). 스페큘러 작업은 구현은 맞았으나 API 오류로 두 번 끊김. 판정은 계속 제 비교로.
- 검증 캡처: `build_dev\captures_cap`(점광원 6개), `captures_4c`(면광원, 스폿 10개), `captures_pt`(PT 속성, 스페큘러).

## 2026-10-09 후반 갱신 (4a, 4b 완료)

- **상태**: 4a `6a6dd25`(속성 렌더링), DoF 4분할 수정 `fa8f863`, 4b `ea2dc2b`(점광원 그림자: 래스터 + RT) 완료 및 커밋. 남은 것은 4c(면광원)와 아래 열린 항목.
- **결정 완료**: D-a 래스터 점광원 그림자 넣는다(완료). D-b 면광원 추가한다(4c, 남음). D-c 추천안(셀프 섀도 트랙은 켜짐/거리, 종류·부드러움·농도·색은 태양 인스펙터)으로 구현됨.
- **열린 항목**
  1. **점광원 그림자 상한 4개 제거**: 상한은 제가 임의로 둔 비용 예산이고 스튜디오에는 맞지 않아요(사용자 지적). RT는 맵이 필요 없으니 상한 불필요(지금은 슬라이스 인덱스를 플래그로 재사용해서 생긴 부산물). 래스터는 그림자 켠 점광원 수만큼 슬라이스를 늘려 할당하고(`kPointShadowLights`, `FillGpuLights`, `PointShadowCount`, `EnsurePointShadowMap`, DSV 힙 64), 해상도와 한도는 품질 설정으로 둔다. 오프라인 렌더는 한도 없이.
  2. **콘서트 채움광**: Hard(PT/GI 룩 유지, 래스터에 옅은 그림자가 새로 생김, 현재) 대 NoCast(래스터 룩 유지, PT/GI에서 그림자 사라짐). 사용자 결정 필요. 4a 이전에 저장된 프로젝트의 채움광은 NoCast로 저장돼 있고 이제 렌더러가 그 값을 따른다.
  3. **4c 면광원**: 모델(`SceneLight.h`), 저장 포맷(버전 2 확장), UI(`UiStudioLight.cpp`), 렌더링(PT/GI는 실제 면 샘플링, 실시간은 큰 반경의 소프트 그림자 근사).
  4. 미확인: 점광원 5개일 때 5번째가 그림자 없이 비추는지, 오프라인 PT 비디오에서 점광원 속성 변화 비교, 감쇠/디퓨즈/스페큘러를 제가 눈으로 확인하지 않음.
- **검증 방식 (사용자 지시, 중요)**: 판정은 워커 보고가 아니라 **제가 직접 돌린 기준 대비 비교**로 한다. 4a에서 워커의 PASS 뒤에 파라미터 인덱스 버그, GI Hard 룩 변경, SkyColor 변경이 숨어 있었고, 4b에서는 `studiorender`가 설정을 오염시켰다. 기존 렌더 룩과 셰이더 팩 룩을 오염시키면 안 된다(메모리 `feedback-no-look-contamination`).
  - 기준 빌드: `git worktree` `E:\repos\MMDX12_base` (현재 `fa8f863`). 새 비교 전 `git -C E:\repos\MMDX12_base checkout --detach <기준 커밋>` 후 `build_base`를 다시 빌드한다. 설정(`mmdx12.ini`)이 두 빌드 사이에 다르면 비교가 무너지니 diff로 확인한다(`renderPath` 오염 사례).
  - 재생 고정: `--autoplay --paused --seek 20` (`--autoplay` 단독은 실시간 시계라 비결정적). 같은 빌드를 두 번 돌려 노이즈 바닥을 먼저 잰다. 오프라인 GI는 결정적이라 비트 동일 비교가 된다.
  - 스튜디오 뷰포트는 항상 래스터(`UiStudio.cpp` ~1503)이고 `--render`는 무시된다. RT/PT는 `--project p.mmdxproj --offline-video out.mp4 --offline-range 20 20.1 --offline-renderer rt|pt --offline-size 960 540` 후 ffmpeg(`-vf "select=eq(n\,1)" -frames:v 1`)로 프레임을 뽑아 비교한다. ffmpeg는 WinGet(Gyan.FFmpeg)에 있다.
  - 플레이 모드의 `--lighting`은 효과가 없다. 점광원과 스폿은 스튜디오 `studiolightpreset 2` 또는 `studiolightadd`로 만든다.
  - 비교용 캡처: `build_dev\captures_cmp`(4a), `captures_dof`, `captures_p4b`(4b). 이미지 비교는 PIL만 있다(numpy 없음).
- **빌드 환경**: 이 셸에서 `build.cmd`가 `vswhere`와 `ninja`를 못 찾는다. PowerShell에서 PATH 앞에 `C:\Program Files (x86)\Microsoft Visual Studio\Installer`와 `C:\Users\admin\AppData\Roaming\Python\Python314\Scripts`를 붙인다. (CLAUDE.md는 miniconda3\Scripts라고 적혀 있어 실제와 다르다.)
- **테스트** (`build_dev\bin\`): `studio_project_test` 133, `studio_light_test` 92, `studio_edit_test` 15, `studio_gizmo_test` 19, `studio_pose_test` 38, `shader_choice_test`. 모두 통과.
- **워커 실행 기록**: 4a `20261009-132917`(세션 f06b9c5c-6b5a-4bd8-8e5e-8efd3e467d0a), 4b `20261009-141207`(세션 c69062af-a3be-4454-aae4-f884c5b2c0c7). 명세: `docs/handoff/specs/04a-renderer-light-properties.md`, `04b-point-light-shadows.md`.
- 아래 "한눈에"와 "4단계 계획"은 3단계 종료 시점의 기록이라 위 갱신이 우선합니다.

## 한눈에

- **상태**: 1단계, 2단계, 3단계 완료 및 커밋. 4단계(렌더러 반영)는 아직 시작 전이에요.
- **커밋**: `7b85d89` (1단계), `33d91da` (2단계), `a058d44` (3단계). 세션 마무리 커밋에 이 문서, `docs/handoff/specs/`, `progress.md` 항목이 들어가요.
- **작업 트리**: 마무리 시점에 깨끗했어요.
- **빌드**: `E:\repos\MMDX12\build.cmd build_dev`. 사용자의 `build_au`(셰이더 팩이 있는 곳)는 건드리지 않았어요.
- **테스트** (`build_dev\bin\`, 모두 통과): `studio_project_test.exe` 133, `studio_light_test.exe` 84, `studio_edit_test.exe` 15, `studio_gizmo_test.exe` 19, `studio_pose_test.exe` 38.

## 이어서 하는 법

1. 이 문서와 `git show --stat a058d44`를 확인하세요.
2. 빌드와 테스트를 한 번 돌려서 상태를 재확인하세요.
3. 아래 "열린 결정" D-a, D-b, D-c에 대한 사용자 답을 받으세요.
4. 4단계 명세를 쓰고 안티그래비티에 위임하세요 (아래 "진행 방식" 참고).

## 사용자 요구 (원문 취지)

- R1. 아웃라이너의 "카메라 · 조명"을 "카메라 VMD"로 바꾸고, 카메라 VMD 조명 트랙은 거기 둔다.
- R2. 프리셋은 카메라 VMD 밑에 두지 않는다. 프리셋의 조명은 "조명" 그룹 아래의 실제 객체가 된다.
- R3. 조명은 인스펙터만이 아니라 뷰포트 기즈모로 조작한다. 방향 시각화는 덤.
- R4. 프리셋은 "이 프리셋 값으로 맞추기" 동작이다. 기존 조명 목록을 프리셋 조명으로 교체한다.
- R5. 광원 종류: 점광원, 태양(디렉셔널), 스폿, 환경광(앰비언트).
- R6. "콘서트" 프리셋은 스폿 여러 개와 채움광을 만든다. 스폿은 색, 위치, 방향을 인스펙터에서 조절하고, 타겟 캐릭터와 자동 방향 조절을 쓸 수 있다.
- R7. 스폿 기즈모로 방향, 위치, 빔 너비(원뿔 각도)를 조절한다.
- R8. 프리셋 적용은 "프리셋으로 재설정됩니다. 계속하시겠어요?" 확인 창을 띄우고 Ctrl+Z로 되돌릴 수 있어야 한다.
- R9 (추가). C4D, 3ds Max, Blender 조명의 공통 속성을 갖춘다: 그림자 종류(없음 No Cast / 하드 / 소프트), 부드러움, 농도, 그림자 색, 감쇠(없음/선형/역제곱), 디퓨즈 영향, 스페큘러 영향, 뷰포트 표시.

## 확정된 설계 결정

| # | 내용 |
|---|---|
| D1 | 환경광 객체가 하늘 색, 지면 색, 힘(hemi)을 가진다. 환경광이 없으면 hemi는 0이고, 기본 하늘 색은 그대로 둔다(검정이 되지 않게). |
| D2 | 태양이 없으면 태양 색을 (0,0,0)으로, sunIntensity는 1로 둔다. "꺼짐"을 sunIntensity 0이나 하늘 색 0으로 표현하지 않는다. 셰이더가 하늘 배경, 안개, 스튜디오 바닥을 `gSunIntensity`로 곱하기 때문이다. |
| D3 | 태양은 장면당 최대 1개, 환경광도 최대 1개. 셰이더는 태양 벡터 하나만 받는다. |
| D4 | 키 편집 규칙: 재생 헤드에 키가 있으면 그 키를 고친다. 없고 자동 키가 켜져 있으면 키를 만든다. 아니면 기본값을 고친다. VMD 연결 중인 태양의 색과 방향은 읽기 전용이다. |
| D5 | 타겟과 자동 스윙 모드에서는 조준점 핸들을 숨긴다. 원뿔과 범위 핸들은 남긴다. |
| D6 | 카메라 VMD 조명 트랙이 전부 MMD 기본값 키이면 스튜디오에 들어올 때 버린다(플레이 경로와 같음). 적용 위치: 스튜디오 카메라 VMD 가져오기, 노래의 카메라 VMD, 프로젝트/패키지의 카메라 트랙. 이 결정은 설계 승인 때 추가된 것이다. |
| D7 | 프로젝트 포맷을 버전 2로 올렸다. 구버전 빌드는 버전 2를 명시적으로 거부한다. 버전 1 "lighting" 객체는 읽을 때 변환한다. |
| D8 | 광원 선택은 `selectedLightUid`를 쓰고 `selectedModel`은 -1로 둔다. **-2는 쓰지 않는다**: -2는 오디오 행과 빈 클립보드가 쓴다. |
| D9 | 광원 식별은 uid로 한다. 이름으로 식별하지 않는다. |
| D10 | 기본 이름(저장되는 데이터): "메인 조명"(태양), "환경광", "점광원 N", "스팟 N", "채움광"(채움용 점광원). |

## 구현 현황

### 1단계 (`7b85d89`): 데이터 모델, 저장, 렌더 매핑
- `src/studio/SceneLight.h/.cpp` (신규): 광원 모델. 종류, 조준 모드, 기본값, 키 샘플링, 프리셋 빌더 `PresetLights`. 그림자, 감쇠, 디퓨즈·스페큘러, 뷰포트 표시 필드를 포함한다(저장만 하고 아직 렌더링하지 않음).
- `src/app/Lighting.h/.cpp`: `BuildSceneLighting`(+ `LightAnchors`)이 `BuildStudioLighting`을 대체. 플레이 모드의 `BuildLighting`은 그대로 둠.
- `src/studio/StudioDoc.h/.cpp`: `lights`, `nextLightUid`, `selectedLightUid`, `LightsCommand`, `RowKind::SceneLight`(값 7).
- `src/studio/StudioProject.h/.cpp`: 버전 2 `"lights"` 배열, 버전 1 변환, 기본 트랙 필터.
- `src/app/UiStudio.cpp`, `UiStudioCamera.cpp`, `UiStudioProject.cpp`, `UiScript.cpp`, `App.h`: 호출처 이식. 구 rig UI와 스크립트 명령 제거.
- `src/core/I18nEn/Ja/Zh.cpp`, `CMakeLists.txt`.
- `tools/studio_project_test.cpp` 갱신, `tools/studio_light_test.cpp` 신규(프리셋 회귀 테스트: `BuildLighting`과 값 비교).
- 삭제: `src/studio/LightRig.h/.cpp`.

### 2단계 (`33d91da`): 편집 UI
- `src/app/UiStudioLight.cpp` (신규, 약 900줄): 아웃라이너 "조명" 그룹, 추가 메뉴(점광원, 스폿, 조건부 태양과 환경광), 프리셋 메뉴, 광원 행(아이콘, 이름, 켜기 토글), 선택, 종류별 인스펙터(태양, 점광원, 스폿, 환경광), 그림자 구역(종류, 부드러움, 농도, 그림자 색), 감쇠, 디퓨즈·스페큘러, 뷰포트 표시, 키 등록, 프리셋 적용 확인 창(`##lightpresetconfirm`), 이름 바꾸기, 켜기/끄기, 삭제. 모든 편집은 `LightsCommand` 또는 기존 트랙 명령으로 Ctrl+Z 가능.
- `App.h`, `UiStudio.cpp`(아웃라이너와 인스펙터 연결), `UiStudioCamera.cpp`, `UiStudioPose.cpp`(카메라 핸들이 광원 선택 중에는 꺼짐), `UiStudioProject.cpp`(`StudioSelectModel`이 `selectedLightUid`를 지움), I18n 3종(44개 문자열).

### 3단계 (`a058d44`): 뷰포트 기즈모와 스크립트 명령
- `src/studio/Gizmo.h/.cpp`: `BuildMouseRay`(원근과 직교), `IntersectRayPlane`, `SolveConeAngle`, `SolveRangeDistance`, `SolveSpotRange`. `studio_gizmo_test.cpp`에 테스트 4개 추가.
- `src/app/Lighting.h/.cpp`: `ResolveSpotAim`을 렌더러와 핸들이 공유.
- `src/app/UiStudioPose.cpp`: `StudioViewportLightHandles`(점광원 위치와 범위, 태양 회전 링과 방향 화살표, 스폿 위치, 조준(수동만), 원뿔, 범위), `StudioScriptLightGizmoPoint`.
- `src/app/UiStudio.cpp`: `StudioBuildLightAnchors`, 뷰포트에서 `StudioViewportLightHandles` 호출.
- `src/app/UiStudioLight.cpp`: `StudioDeleteLight`, `StudioApplyLightPreset`.
- `src/app/UiScript.cpp`: 스크립트 명령 `studiolightpreset`, `studiolightadd`, `studiolightdel`, `studiolightsel`, `studiolightkey`, `studiolightset`, `studiolightgizmo`. `studiostate`가 광원마다 `STUDIOLIGHT` 줄을 남김.
- **버그 수정(검토 중 발견)**: `StudioViewportPose`가 캐릭터나 스테이지가 선택되지 않은 상태(광원 선택 중에는 항상 이렇다)에서 조기 반환하면서 `studioViewDrag_ == 8`인 광원 드래그를 **다음 프레임에 끊고 있었어요**. 조기 반환 블록에서 그 분기를 뺐습니다. 수정 전 원뿔 드래그는 13.8°에서 0.1°만 바뀌었고, 수정 후에는 6.9°까지 줄었어요.

### 저장 포맷 (버전 2)
- 편집 객체의 `"lights"` 배열. 광원마다 `uid`, `kind`(sun/point/spot/ambient), `name`, `enabled`, `values`(position, aim, direction, color, intensity, range, coneOuter, coneInner), `sun`(vmdLink, rim), `spot`(aim, target, part, swayPhase), `sky`, `keys`, 그리고 최상위의 `shadow`, `falloff`, `affectDiffuse`, `affectSpecular`, `viewportVisible`.
- 타겟 캐릭터는 저장할 때 `모델 인덱스 + 1`로 쓰고, 불러올 때 uid로 되돌립니다(`UiStudioProject.cpp` 저장, `UiStudio.cpp` 155-158 로드).
- 버전 1의 `lighting`(소스 선택 + rig)과 `useLightTrack`은 `ConvertLegacyLighting`으로 변환합니다. VMD 소스는 태양을 VMD에 연결하고, 프리셋 소스는 키 오버라이드를 태양에 적용하고 연결을 끊고, 커스텀 소스는 스폿을 옮기고(auto는 Sway, center·head는 Target), 채움광을 추가합니다.

## 아직 렌더링되지 않는 것 (4단계)

지금 UI에서 설정할 수 있지만 렌더러가 무시하는 속성입니다.

| 속성 | 실시간 래스터 | 광선 추적 (RT, PT, 오프라인 GI) |
|---|---|---|
| 그림자 없음 (No Cast) | 쉬움. 스폿은 그림자 맵 슬롯을 안 씀. 태양은 `gCascadeSplits.w` 스위치 | 쉬움. 그림자 광선을 쏘지 않음 |
| 하드/소프트, 태양 | 쉬움. CSM 12탭 포아송 반경(`mmd.hlsl` ~109-126)을 광원 값으로 받게 함 | 쉬움. 이미 원뿔 샘플(`ShadowRt`) |
| 하드/소프트, 스폿 | 쉬움. 지금 4탭 PCF, 반경 0.75 텍셀 고정(`SpotShadow` ~157-178) | 중간. 광원 면에서 샘플링해야 함 |
| **점광원 그림자** | **없음.** 그림자 맵이 없음. 큐브 맵 필요 | RT는 쉬움. PT·오프라인 GI는 **이미 있음**(`pathtrace.hlsl` ~251-267, `offline_gi.hlsl` ~308-371) |
| 농도, 부드러움, 감쇠 | 쉬움. 감쇠 식 `PunctualDiffuse` ~180-198 | 쉬움 |
| 그림자 색 | 중간. `GpuLight`(`ShaderInterop.h` 80-87)의 여분 실수 3개는 색 하나에는 맞지만, 다른 광원 필드와 같이 넣으려면 구조체를 키워야 함 | 중간 |
| 디퓨즈·스페큘러 끄기 | 쉬움. `PunctualDiffuse`, `PunctualSpecular` 루프에 플래그 | 확인 필요 |

- **태양 그림자의 현재 주인**: 카메라 VMD의 셀프 섀도 트랙(`useShadowTrack`)과 렌더 설정. 광원의 그림자 종류는 아직 연결되지 않았습니다(D-c).
- **하늘 글로우 문제**: 태양이 꺼져도 `SkyColor`(`common.hlsli` 96-97)가 `gLightDir` 방향으로 글로우를 그립니다. 4단계에서 처리해야 합니다.

## 열린 결정 (사용자 답 필요)

- **D-a. 점광원 실시간 그림자**: 래스터에도 넣을까요? 추천: 아니오. 지금은 RT, PT, 오프라인 GI에서만 그림자를 내고, 인스펙터에 "실시간 미지원" 안내를 둡니다. 래스터에 넣으려면 점광원마다 6면 큐브 맵이 필요하고 비용이 큽니다.
- **D-b. 면광원(Area) 타입**: C4D, 3ds Max, Blender에 다 있으니 추가할까요? 추천: 4단계 뒤에 추가. PT와 GI는 실제 면에서 샘플링하고, 실시간은 큰 반경의 소프트 그림자로 근사합니다.
- **D-c. 태양 그림자와 셀프 섀도 트랙**: 추천: VMD 셀프 섀도 키가 있고 연결이 켜져 있으면 VMD가 그림자의 켜짐·꺼짐과 거리를 정하고, 종류·부드러움·농도는 태양 인스펙터에서 정합니다.

## 4단계 계획 (제안, 명세는 아직 없음)

- **4a**: 그림자 종류(없음/하드/소프트), 부드러움, 농도, 감쇠, 디퓨즈·스페큘러 플래그를 래스터와 광선 경로에 반영. 광원별 값은 `GpuLight`의 여분 실수로 전달하거나 구조체를 키웁니다(크기 변경 시 `static_assert`와 HLSL 구조체를 같이 고칠 것).
- **4b**: 점광원 그림자. RT는 광선 쿼리, PT와 GI는 부드러움에 따른 면 샘플링. 래스터 큐브 맵은 D-a 결정에 따라 결정.
- **4c**: 그림자 색(구조체 확장).
- **4d**: 면광원(D-b 결정에 따라).
- **4e**: 태양이 꺼졌을 때 하늘 글로우 정리.
- 각 항목은 하드, 소프트, 없음을 나란히 캡처해서 확인합니다.

## 진행 방식 (사용자 지시)

- 단계가 끝날 때마다 범위를 나눠 커밋합니다. 다른 단계 변경이 섞이지 않게 합니다.
- 2단계부터 안티그래비티에 위임합니다: `delegate.py --level high --review smoke`(구현 모드). 명세는 한 페이지짜리 큰 틀로 쓰고, 세부 설계는 워커에게 맡깁니다.
- 검토(smoke): 빌드와 테스트를 제가 직접 다시 돌리고, 보고서와 변경 목록을 대조하고, 의심스러운 부분은 깊게 봅니다. 워커의 "PASS"는 로그와 캡처로 확인합니다. 3단계 검토에서 워커가 0.1° 변화를 놓쳤던 것처럼요.
- 명세에 금지 규칙을 넣습니다: 실행 중인 프로세스를 중지하지 않는다(2단계 워커가 실행 중인 `MMDX12.exe`를 강제 종료한 적이 있음). 앱 실행 횟수는 4회 이하(3단계 워커는 5회 실행).
- 앱 실행은 한 번에 하나씩 하고, 창이 뜬다는 걸 사용자에게 미리 알립니다(`no-window-spam` 메모리 참고).
- **Sonnet 서브에이전트는 주간 사용 한도로 중단된 적이 있습니다** (2026-10-09). 한도 해제는 **Oct 11, 8am (Asia/Seoul)**입니다. 그 전에는 Sonnet 위임을 쓰지 마세요.

## 워커 실행 기록

모든 워커 실행 폴더는 `C:\Users\admin\AppData\Local\Temp\antigravity-delegate\<run>\`에 있습니다. `report.md`, `changes.patch`, `trace.md`, `live.log`가 들어 있어요.

| 단계 | run | 세션 ID | 결과 |
|---|---|---|---|
| 설계 조사 (investigate, read-only) | `20261009-035319` | `8ce9af40-8f8f-43c7-8d26-8229fccd5e35` | 설계 제안. 워커의 설계 중 세 부분을 검토에서 수정 |
| 2단계 구현 | `20261009-125142` | `1a89bb7c-d3e0-4259-a92e-0186c56c846a` | 8개 수정, 1개 추가 |
| 3단계 구현 | `20261009-130430` | `6e802521-7474-45e9-96fc-43b78a1f9b6b` | 10개 수정. 드래그 버그는 검토에서 수정 |

1단계 Sonnet 서브에이전트(`aea44cad32306501a`)는 한도 초과로 중단됐습니다. 남은 1단계는 제가 직접 마무리했습니다.

## 명세 파일

`docs/handoff/specs/` 아래 세 파일이 원본 명세입니다.
- `01-design-investigation.md`: 설계 조사 명세 (당시 결정 일부는 이 문서의 결정표가 우선)
- `02-phase2-editing-ui.md`: 2단계 명세
- `03-phase3-gizmos-scripts.md`: 3단계 명세

## 검증 결과 (마무리 시점)

- 빌드: `build_dev`, 에러 0.
- 테스트: 위 "한눈에" 목록 전부 통과.
- 캡처 (`build_dev\captures_phase3\`):
  - `a_concert_spot.png`: 콘서트 프리셋, 스폿 1 선택, 핸들과 원뿔 외곽선이 보임.
  - `b_drag_cone.png`: 수정 전 드래그 (각도가 거의 안 바뀜).
  - `b_fixed.png`: 수정 후 드래그 (원뿔이 줄어듦, 프레임 0에 키 생성).
  - `c_sun_selected.png`: 태양 선택, 회전 링과 방향 화살표가 보임.
  - `mmdx12_run_*.log`, `mmdx12.log`: `[E]` 줄 0개.

## 알려진 문제와 주의

- 위치, 조준, 범위 드래그는 별도 캡처로 확인하지 않았습니다. 같은 수정 경로를 쓰므로 4단계 전에 한 번 확인하는 게 좋습니다.
- 그림자, 감쇠, 디퓨즈·스페큘러, 그림자 색, 농도, 부드러움은 저장과 UI만 있고 렌더링에는 아직 영향이 없습니다. 사용자가 설정해도 화면이 안 바뀝니다.
- 빈 프로젝트(캐릭터, 스테이지 없음)에서 광원을 편집하는 것이 캡처 조건이었습니다. 캐릭터가 있는 장면에서의 광원 편집은 아직 캡처하지 않았습니다.
- 스튜디오 기본 문서(새 프로젝트)는 "스튜디오" 프리셋을 씁니다(태양 + 환경광).
- 버전 2 프로젝트는 구버전(1.5.0 등)에서 열리지 않습니다.
- 저장된 `.ini`의 실험 값이나 `mmdx12_layout.ini`가 UI 레이아웃을 바꿀 수 있습니다.

## 핵심 파일 위치

- 모델: `src/studio/SceneLight.h`, `src/studio/SceneLight.cpp`
- 문서와 명령: `src/studio/StudioDoc.h` (`LightsCommand`, `TrackState`, `RowKind`)
- 저장: `src/studio/StudioProject.cpp` (`SceneLightJson`, `ReadSceneLight`, `ConvertLegacyLighting`)
- 렌더 매핑: `src/app/Lighting.cpp` (`BuildSceneLighting`, `ResolveSpotAim`, `BuildLighting` 플레이 경로)
- 편집 UI: `src/app/UiStudioLight.cpp`
- 뷰포트 핸들: `src/app/UiStudioPose.cpp` (`StudioViewportLightHandles`, `StudioScriptLightGizmoPoint`)
- 기즈모 솔버: `src/studio/Gizmo.cpp` (`SolveConeAngle` 등)
- 스크립트: `src/app/UiScript.cpp` (`studiolight*`)
- 렌더러 (수정 대상, 4단계): `shaders/mmd.hlsl`(`ShadowCascade`, `Shadow`, `ShadowRt`, `SpotShadow`, `PunctualDiffuse`, `PunctualSpecular`, `SkyColor` 사용처), `shaders/common.hlsli`(`SkyColor`, `gCascadeSplits`), `src/render/ShaderInterop.h`(`GpuLight`, `SceneConstants`, `kSpotShadowSlices`), `src/render/Renderer.cpp`(`FillGpuLights`), `src/render/Renderer.h`(`kMaxPunctualLights = 16`), `shaders/pathtrace.hlsl`, `shaders/offline_gi.hlsl`
