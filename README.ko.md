<div align="center">
  <h1>MMDX12</h1>
  <p><a href="README.md">English</a> · <b>한국어</b></p>
  <p>Direct3D 12 네이티브 MikuMikuDance 플레이어 — 실시간 레이 트레이싱과 패스 트레이싱, 그리고 블렌더급 MMD 영상을 만드는 오프라인 GI 렌더러.</p>
  <p>
    <img src="https://img.shields.io/badge/Platform-Windows%2010%2F11-39C5BB" alt="Platform">
    <img src="https://img.shields.io/badge/Graphics-Direct3D%2012%20%2B%20DXR%201.1-39C5BB" alt="Graphics">
    <img src="https://img.shields.io/badge/Language-C%2B%2B20-39C5BB" alt="Language">
    <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-39C5BB" alt="License"></a>
    <a href="https://youtu.be/7S7D670HbIM"><img src="https://img.shields.io/badge/YouTube-Introduction-39C5BB?logo=youtube&logoColor=white" alt="YouTube"></a>
  </p>
  <a href="https://youtu.be/vNztzqvVV4M">
    <img src="docs/media/promo-preview.webp" width="100%" alt="MMDX12 미리보기">
  </a>
  <br>
  ▶ YouTube: <a href="https://youtu.be/vNztzqvVV4M">58초 프로모</a> · <a href="https://youtu.be/7S7D670HbIM">전체 소개 영상</a>
  <p>
    <a href="#갤러리">갤러리</a> ·
    <a href="#주요-기능">주요 기능</a> ·
    <a href="#에셋은-직접-준비하세요">에셋</a> ·
    <a href="#시작하기">시작하기</a> ·
    <a href="#벤치마크-재미용">벤치마크</a>
  </p>
</div>

## 갤러리

내장 오프라인 GI 렌더러의 결과물을 보정 없이 그대로 실었습니다 (3840×2160 원본을 축소).

<table>
  <tr>
    <td>
      <img src="docs/media/gi-open-arms.jpg" width="100%">
      <br><i>부드러운 스튜디오 조명, 얇은 렌즈 피사계 심도</i>
    </td>
    <td>
      <img src="docs/media/gi-point.jpg" width="100%">
      <br><i>전경 보케가 들어간 클로즈업</i>
    </td>
  </tr>
  <tr>
    <td>
      <img src="docs/media/gi-stage.jpg" width="100%">
      <br><i>밤 무대, 피사계 심도와 보케</i>
    </td>
    <td>
      <img src="docs/media/gi-render-bench.jpg" width="100%">
      <br><i>GI 렌더 벤치마크: 유리 큐브의 굴절과 분산</i>
    </td>
  </tr>
</table>

## 주요 기능

- **아무렇게나 넣어도 되는 라이브러리**: 폴더 구조와 상관없이 캐릭터, 무대, 댄스 세트를 내용으로 분류합니다.
- **실시간 래스터**: MMD 툰 셰이딩, 캐스케이드 그림자, SSAO, SSR, TAA.
- **DXR 1.1 레이 트레이싱**: RT 그림자, RTAO, RT 반사.
- **패스 트레이싱**: 픽셀당 1–4 샘플, 시간 누적 + à-trous 디노이저.
- **업스케일러**: DLSS, FSR, XeSS.
- **포스트 효과**: 피사계 심도, 볼류메트릭 라이트, FFT 컨볼루션 블룸, 컬러 LUT(내장 룩 또는 `.cube` 파일).
- **물리**: PMX 강체와 조인트를 Bullet으로 시뮬레이션 (머리카락, 치마, 액세서리).
- **오프라인 GI 렌더러**: 이래디언스 캐시 + 패스 트레이싱에 피사계 심도, 모션 블러, 피부 표면하 산란까지. 4K 스틸과 MP4 영상을 만들고, 렌더 대화상자가 예상 시간을 직접 측정합니다.
- **벤치마크**: 실시간 및 GI 렌더 카테고리와 온라인 리더보드.

## 화면

<img src="docs/media/ui-library.jpg" width="100%" alt="라이브러리 화면">
<br><sub>라이브러리 화면. UI는 한국어입니다.</sub>

## 에셋은 직접 준비하세요

> [!IMPORTANT]
> MMDX12에는 모델, 모션, 무대, 음악이 **하나도 들어 있지 않습니다**. MMD 에셋은 제작자에게 권리가 있어 재배포할 수 없으므로, 각자의 라이브러리 폴더를 지정해서 쓰고 에셋마다 이용 규약을 지켜야 합니다 (예: 상업적 이용 금지, 영상 공개 시 크레딧 표기 등). 이 README의 이미지와 영상은 제작자의 로컬 라이브러리로 만들었습니다.

라이브러리 폴더는 `--library` → `mmdx12.ini` → `<exe>\library` → exe에서 위로 올라가며 처음 찾은 `library\` 폴더 순으로 정해집니다.

**라이브러리 분류 규칙**
- **캐릭터**: 사람형 본(`頭`, `左腕`, `左足`/`左ひざ`)이 있는 PMX. 단, `stage` 폴더 아래는 제외.
- **무대**: 그 밖의 PMX. 한 폴더의 PMX 파일은 모두 하나의 무대가 됩니다 (여러 파츠로 된 무대). 내용이 같은 중복 파일은 빠지고, 캐릭터 옆에 있는 사람형이 아닌 PMX는 액세서리로 보고 건너뜁니다.
- **곡**: 본 키가 있는 VMD가 든 폴더. 카메라 키가 있는 VMD가 카메라, 모프만 있는 VMD는 표정 레이어입니다. 음원은 같은 폴더나 상위 폴더의 wav/mp3/flac/ogg. 댄스 VMD가 여러 개면 음원 길이와 가장 잘 맞는 것을 씁니다.

## 시작하기

### 다운로드

[Releases](https://github.com/digital8150/MMDX12/releases/latest)에서 포터블 빌드(`MMDX12-<버전>-win64.zip`)를 받아 아무 곳에나 압축을 풀고, `MMDX12.exe` 옆의 `library` 폴더에 MMD 에셋을 넣은 뒤(또는 앱에서 폴더 선택) `MMDX12.exe`를 실행하세요. Windows 10/11 x64와 Direct3D 12 GPU가 필요하고, 레이 트레이싱·패스 트레이싱·GI 렌더러는 DXR 1.1, DLSS는 NVIDIA RTX GPU가 필요합니다. 실행 파일에 코드 서명이 없어 SmartScreen 경고가 한 번 뜰 수 있습니다 (추가 정보 → 실행).

### 소스에서 빌드

**요구 사항**
- Windows 10/11, D3D12 GPU.
- RT/PT/GI에는 DXR 1.1 필요.
- Visual Studio 2026 (v18, MSVC 14.5x) + Windows SDK.
- CMake + Ninja (`pip install cmake ninja`).

**빌드**
```text
build.cmd
build.cmd build --target asset_probe
```

**업스케일러 (선택)**
업스케일러 DLL을 받은 뒤 다시 빌드합니다 (DLL이 없으면 해당 업스케일러만 꺼집니다):
```powershell
powershell -ExecutionPolicy Bypass -File tools/fetch_sdks.ps1
```

**실행**
`build\bin\MMDX12.exe`

## 사용법

- **화면**: 라이브러리 → 플레이, 영상 렌더 대화상자, 벤치마크.
- **플레이 조작**: Space (재생/일시정지), ←/→ (±5초 이동), C (모션/자유 카메라), L (조명 프리셋), F1 (UI 숨기기), 마우스 드래그 (회전), 우클릭 드래그 (이동), 휠 (확대/축소), Esc (뒤로).
- **오프라인 렌더**:
  - P 키 / 카메라 버튼 = 4K PNG 스틸, `Pictures\MMDX12`에 저장 (RTX 3060 Laptop 기준 약 20초).
  - 영상 렌더 (플레이 옆 버튼) = H.264 + AAC MP4, `Videos\MMDX12`에 저장, 720p–4K, 24/30/60 fps. 렌더러는 래스터, RT, PT, 오프라인 GI 중 선택 (오프라인 GI 4K는 RTX 3060 Laptop 기준 프레임당 약 10초, 곡 하나에 하루 정도). Esc로 멈추면 그때까지의 프레임은 남습니다.

## 벤치마크 (재미용)

- 에셋을 함께 배포할 수 없어서 공식 벤치마크 장면이 없습니다. 장면은 라이브러리에서 자동으로 고릅니다: 정점이 가장 많은 캐릭터와 무대, 춤 길이가 2분 30초에 가장 가까운 곡 (카메라 모션이 있는 곡 우선).
- GI 렌더 벤치마크 (시네벤치 방식, `dx12-gi-render`): 3840×2160 이미지 한 장을 픽셀당 4096 샘플로 그리고 걸린 시간으로 점수를 매깁니다 (점수 = 6.25 × 초당 백만 샘플). 출연진은 라이브러리에서 정점이 가장 많은 캐릭터 3명이며, 각자 라이브러리의 춤에서 가져온 포즈를 취합니다 (같은 라이브러리면 항상 같은 장면). 분산이 있는 굴절 유리 큐브 뒤에 서고, 소프트박스와 컬러 림 스포트라이트로 비춥니다.

| 카테고리 | 해상도와 측정 방식 |
|---|---|
| `dx12-raster-fhd`<br>`dx12-rt-fhd`<br>`dx12-pt-fhd` | **1080p 실시간**: 워밍업 120 + 측정 3600 프레임, 프레임당 1/60초 고정 진행, 수직 동기화/업스케일러 끔, 래스터/RT는 MSAA 4x, PT는 1 spp · 3회 반사. |
| `dx12-raster-4k`<br>`dx12-rt-4k`<br>`dx12-pt-4k` | **4K 실시간**: 측정 방식은 1080p와 같습니다. |
| `dx12-gi-render` | **4K 오프라인 GI**: 3840×2160 이미지 한 장, 픽셀당 4096 샘플. |

- 점수는 사용한 라이브러리에 따라 달라지므로 리더보드는 재미용이지 기준이 아닙니다. 결과 화면에서 온라인 리더보드에 제출할 수 있습니다.
- 실시간 점수 공식: `(avg*0.6 + low1*0.4) × resolutionFactor × stabilityFactor × 100`, 등급 D…SSS.

## 명령줄

<details>
<summary>명령줄 옵션</summary>

```text
--library <dir>  --character <s> --stage <s|none> --song <s>  --autoplay  --seek <sec>
--benchmark dx12-raster-fhd|dx12-raster-4k|dx12-rt-fhd|dx12-rt-4k|dx12-pt-fhd|dx12-pt-4k|dx12-gi-render  --bench-frames <n>
--bench-spp <n>  (GI render benchmark testing: samples per pixel; with --offline-size the result cannot be submitted)
--frames <n>  --capture <out.png>  --width <w> --height <h>  --debug
--free-camera  --camera tx,ty,tz,yaw,pitch,dist  --paused  --lighting 0..3  --quality 0..3  --no-physics
--render raster|rt|pt  --upscaler none|dlss|fsr|xess  --upscale-quality native|quality|balanced|performance|ultra
--dof 0|1  --volumetric 0|1  --bloom-conv 0|1  --lut <name|none>
--offline-still <out.png> | --offline-video <out.mp4> [--offline-range <a> <b>]   (offline GI render, then quit)
--offline-fps 24|30|60  --offline-bitrate <mbps>  --offline-quality 0..3   (video format for this run; default: the select screen dialog's)
--offline-renderer raster|rt|pt|gi   (video renderer for this run; default: the dialog's; --dof/--volumetric/--bloom-conv also apply to the video)
--offline-probe   (the dialog's time measurement from a play scene; logs "VIDEO PROBE", then quits)
--offline-spp <n>  --offline-size <w> <h>   (testing: cap samples / override the output size)
--screen select|stages|songs|settings|video|bench|bench-gi   (UI capture testing; video = the render dialog, or with
                                                              --offline-video renders straight from the select screen)
```

</details>

## 구조

<details>
<summary>모듈 개요</summary>

```text
src/core    Log, text encodings (UTF-8 / UTF-16 / Shift-JIS)
src/asset   PMX 2.0/2.1 + VMD parsers, image decode (stb + WIC), library scanner
src/anim    ModelInstance (bone hierarchy, append bones, CCD IK with limits, vertex/bone/group morphs),
            PhysicsWorld (Bullet rigid bodies + 6DOF spring joints: hair, skirts), BoundMotion /
            CameraMotion (VMD bezier evaluation)
src/render  Dx12Context (device, swap chain, frame pacing, descriptor heaps, uploads, capture)
            Renderer = ordered IRenderPass list (Passes.h): Shadow (3 cascades) -> Scene (MSAA MRT:
            HDR colour, view normal + reflectivity, velocity) -> Resolve -> SSAO -> SSR -> Composite
            (AO, reflections, haze) -> Volumetric -> TAA (optional) -> Upscale -> DoF -> Bloom (mip
            chain or FFT convolution) -> Post (PBR Neutral tonemap, grade, LUT, vignette) -> UI
            backdrop blur -> Present
            GpuModel (GPU skinning via StructuredBuffer, morph delta stream), IUpscaler seam
            OfflineRenderer (offline GI: irradiance cache prepass + path tracing, DoF, motion blur, SSS, outlines)
src/app     App state machine + ImGui screens, VideoEncoder (Media Foundation MP4), scene loader (worker thread), benchmark, leaderboard (WinHTTP)
src/audio   miniaudio playback; audio cursor is the master clock
shaders     mmd.hlsl (MMD toon + shadows through the toon ramp, punctual lights, rim, sky, studio
            floor, shadow map), resolve/ssao/ssr/composite/taa/bloom/post/present.hlsl,
            dof, volumetric(_apply), bloom_fft (compute FFT), offline_gi/offline_post/offline_edge
tools       asset_probe, anim_probe, render_smoke (headless-ish self tests)
```

규칙: 어디서나 MMD 네이티브 좌표계(왼손 좌표계, +Y 위)를 씁니다. D3D와 같아서 축을 뒤집지 않습니다. DirectXMath 행 벡터(`v * M`), HLSL은 `pack_matrix(row_major)`.

</details>

## 서드파티

| 의존성 | 라이선스 |
|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT |
| [stb](https://github.com/nothings/stb) | MIT / 퍼블릭 도메인 |
| [miniaudio](https://github.com/mackron/miniaudio) | MIT-0 / 퍼블릭 도메인 |
| [DirectX-Headers](https://github.com/microsoft/DirectX-Headers) | MIT |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT |
| [Bullet Physics 3.25 subset](https://github.com/bulletphysics/bullet3) | zlib |
| [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) | NVIDIA RTX SDK license |
| [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) | MIT |
| [Intel XeSS SDK](https://github.com/intel/xess) | Intel Simplified Software License |
| [Pretendard](https://github.com/orioncactus/pretendard) | SIL OFL 1.1 |
| [Phosphor Icons](https://github.com/phosphor-icons/core) | MIT |

*"MikuMikuDance"는 樋口優(Yu Higuchi)가 만들었고, 하츠네 미쿠는 Crypton Future Media의 캐릭터입니다. MMDX12는 비공식 팬 프로젝트이며 어느 쪽과도 관련이 없습니다.*

## 라이선스

MMDX12 자체 코드는 [MIT](LICENSE), 서드파티 코드는 각자의 라이선스를 따릅니다. MMD 에셋은 포함되지 않습니다.

## 로드맵

- 재질 모프, SDEF 스키닝, PMD 지원, VMD 조명 트랙.
- 라이브러리 인덱스 캐시.
