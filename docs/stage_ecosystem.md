# MMDX stages: format, studio editing, scripting, gallery

Status: direction v1 (2026-10-25). Architect: Claude; decisions by the owner. Each milestone below is built by a worker
run and marked done here with its date. This is a direction document; the format reference lives in
`docs/stage_format.md` (written with milestone S1a and kept current by every later milestone).

## 0. Goal

An MMDX-native stage ("MMDX 스테이지") that carries real PBR materials, a designed light rig, screens and scripted
content, downloaded complete from an online stage gallery and dropped into the studio or play mode. The showcase is
offline GI / PT video quality that the MMD ecosystem cannot reach without Blender. The engine stays generic: it exposes
contracts (screens, cameras, sockets, params, packs); every concrete look or behaviour lives in the stage.

Reference stages (content, built later on these contracts): (4) dance-studio cyclorama with colour variants,
(1) dance practice room with a mirror wall, (2) arena stage with moving spots, haze and LED screens fed by cameras /
videos, (3) a Times Square-like plaza with many LED billboards and building fireworks. Real brands / logos are never used.

## 1. Decisions

- **Container**: a stage is a folder (zip when distributed) with `stage.json` (`"format": "mmdx-stage"`). Geometry is
  glTF 2.0 (`.glb` / `.gltf`, preferred: Blender exports it with standard PBR extensions) or PMX (lets existing MMD stages
  be upgraded by wrapping them). A stage may have several model parts.
- **Detection by content**: any library folder holding a valid `stage.json` is an MMDX stage (never by folder name).
  Installed stages (gallery downloads, tests) live in `<exe>/stages/<id>/` (like `<exe>/shader_packs`), a second scan
  root next to the user's library; the user's library is never written to.
  Its model files are not also listed as plain stages.
- **Editing = overrides**: the studio never edits the installed stage. Exposed params, variant, light / screen / part
  overrides are stored in the `.mmdxproj` and keyable on the timeline. Mesh work stays in Blender.
- **Scripting layers** (every layer is a pure function of time + params + audio analysis, so seeks, reverse playback,
  video and GI shutter samples agree):
  - L0 declarative (`stage.json`): params, variants (= named param value sets), bindings (param -> target with
    scale / offset / curve / gain param), cue tracks, and procedural **drivers** (a param modulated by the beat / bar /
    onset / energy / section or the clock: docs/stage_format.md "drivers").
  - L1 GPU code through the existing pack system, bundled inside the stage: surface packs on stage materials, world
    packs anchored to stage sockets, and the new **screen packs** (`"type": "screen"`, a pixel shader that draws LED
    content from time / audio / params).
  - L2 a CPU script VM: deferred. If ever built: sandboxed, no file / network access, pure `evaluate(t)`. Native code
    from a stage is never loaded.
- **Gallery**: a top-level "갤러리" screen (online stages / effects / looks), separate from the installed-pack manager.
  Stages have their own index (`stages/index.json`) with the same source / verified / revoked / SHA-256 rules as
  shader packs (`app/ShaderPackStore`), absolute download URLs (big zips live on an external host, never the home
  server), resumable downloads, install into the library.

## 2. Engine gaps and milestones

Order = dependency order. Every milestone keeps the ecosystem rules (docs/effect_ecosystem.md §0): every render path,
zero cost when unused, failures stay local, no library-layout dependence.

| # | Milestone | Status |
|---|---|---|
| S1a | Format core (asset layer): `stage.json` parser, param / variant / binding evaluator, library detection, `stage_check`, procedural test stages (`tools/stagegen`) | done 2026-10-25 |
| S1b | App integration: play + studio load MMDX stages, manifest lights as "스테이지 조명", material overrides, lobby variant picker | done 2026-10-25 |
| S2a | glTF material / light import: transmission, ior, volume, emissive_strength, clearcoat (approx.), texture_transform, KHR_lights_punctual | done 2026-10-25 |
| S2b | Mesh dielectrics: transmissive materials refract in RT / PT / offline GI (thin + volume), raster approximation | done 2026-10-26 |
| S3 | Studio stage entity: outliner group, overrides saved in the project, keyable stage params (timeline rows) | done 2026-10-26 |
| S4 | Camera entities: N cameras with own tracks, an active-camera switch track, VMD export flattens cuts | done 2026-10-26 |
| S5 | Screens: N screen surfaces, sources none / image / video / camera feed, multi-video decode, feed render pass | done 2026-10-26 |
| S6 | Stage-bundled packs: screen packs (new type), surface packs on stage materials, world packs on sockets | done 2026-10-26 |
| S7a | Many lights, part 1: punctual cap 16 -> 256, the 16 most important spots get shadow slices (importance + hysteresis), PT / GI light sampling by a power CDF, volumetric march over (up to 64) lights | done 2026-10-26 |
| S7c | Many lights, part 2: clustered forward lighting (Forward+ with depth slices; no deferred: shader packs, MSAA, translucent hair and MME object effects need forward), the three Punctual* loops iterate a froxel's light list | done 2026-10-26 |
| S7b | Spot cookies / gobos: a projected texture per spot (stage-bundled PNGs, rotatable, keyable) in raster / RT / PT / GI and the volumetric beams | done 2026-10-26 |
| S8 | Emissive lights: emissive-triangle NEE in PT / GI (screens and LED walls light the scene), raster screen lights | done 2026-10-26 (S8a PT / GI emissive NEE + MIS; S8b raster / RT screen area lights) |
| S9 | Haze without heavy fog: per-light volumetric scattering + anisotropy, local fog volumes, stage `environment.fog` / `environment.fogVolumes` | done 2026-10-26 |
| S9c | Fully path-traced haze in offline GI: the medium (height fog + fog volumes) inside every path segment, multiple scattering, scattering-event NEE to sun / punctual lights, medium transmittance on surface shadow rays, fogged irradiance cache (done 2026-10-26) | |
| S9c-2 | Less noise in path-traced haze: equiangular sampling of punctual / spot lights at medium events (MIS with distance sampling), all-light NEE when few lights | done 2026-10-26 |
| S9d | Real-time froxel volumetric fog (after S7): a camera-aligned 3D grid (froxels, the S7 cluster layout) injects medium density (height fog + fog volumes) and lighting from every light via the S7 cluster lists (shadow maps / ray-query shadows), temporal reprojection over frames, front-to-back integration; an approximate multiple-scattering term; replaces the per-pixel ray march in raster / RT / PT, same medium parameters as S9 / S9c (done 2026-10-26) | |
| S9b-1 | Physical Sky light (C4D-like): `LightKind::Sky`, procedural sky (sun position / time of day, turbidity, horizon haze, ground) as the background, the fog colour and the sun colour, every render path | done 2026-10-27 |
| S9b-2 | Sky lighting: the sky as a light (SH irradiance for the raster / RT ambient, importance-sampled with MIS in PT / GI) | done 2026-10-27 |
| S9b-3 | Image skies: HDRI (.hdr / .exr equirect) and MME / Ray-MMD sky domes (DDS cubemap) as the sky light's map, rotation + intensity | done 2026-10-27 (.hdr / png / jpg / RGBA8 dds incl. Ray-MMD RGBT and cube maps; no EXR / BC6H) |
| S9b-4 | Clouds: procedural cloud layer on the physical sky (coverage, height, wind; keyable), lit by the sun | done 2026-10-27 |
| S10 | Planar mirrors in raster (RT / PT / GI reflect natively) | done 2026-10-26 |
| S11 | Compressed textures: DDS BCn (+ glTF `MSFT_texture_dds`), VRAM budget report | done 2026-10-26 |
| S12 | Gallery screen + stage store (index, resumable download, install, update, revoke) | done 2026-10-26 |
| S13 | Website stage gallery (MMDX12_Web): index + pages; deploy only after the owner says so | built 2026-10-26 (MMDX12_Web: build-stages.mjs, /<lang>/stages/ pages, creator doc); not deployed, zips not uploaded (STAGE_CDN_BASE unset) |
| U1 | (issue #5) Settings window: camera controls (invert horizontal / vertical, sensitivity) + English bone / morph names toggle (timeline, bone / morph lists) | |
| U2 | (issue #5) Studio timeline audio waveform (the project audio under the frame ruler, zoom-aware) | |
| U3 | (issue #5) Lobby model selection: list view next to the card grid; hide (exists: right-click) made discoverable + undoable (a hidden-models filter) | |
| U4 | (issue #5) Pose editing: `Ctrl` fine drag, click cycles through overlapping bones, MMD-style on-screen bone controllers (bottom-left of the viewport) | |
| U5 | (issue #5) UI scaling: LUT and other buttons off-screen on 3:2 / 2K displays; check 3:2, 16:10, 2K, 4K at 100-200 % | |
| U6 | (issue #5) Wind physics: keyable wind (direction, strength, gusts) on the Bullet bodies, play + studio | |
| U7 | (issue #5) Beamman MME effects: add to the compatibility list, fix what breaks | |
| U8 | (issue #5) Rigify-like control rig: auto-generated (editable) FK / IK controls saved as a project JSON sidecar, never touching the model file | |

## 3. Runtime model (shared by all milestones)

- `StageManifest` (asset layer, pure data) = parsed `stage.json`. `StageState` = resolved param values for one time
  (variant + project overrides + keyed params), produced by a pure evaluator; bindings turn it into concrete targets
  (`lights.<name>.<field>`, `materials.<name>.<field>`, `screens.<id>.<field>`, `packs.<n>.params.<key>`,
  `environment.<field>`).
- Play mode: the lobby picks stage + variant (select-screen panel, saved per stage); params use their defaults / the
  variant; the lighting picker gains "스테이지 조명" (default when the stage has lights).
- Studio: one stage entity per MMDX stage (its parts stay models internally); its params are timeline rows.
- Sockets: named transforms (a glTF node or an explicit transform) that world packs, cameras and props attach to.
