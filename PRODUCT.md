# Product

<!-- impeccable:product-schema 1 -->

## Platform

windows-desktop (native Win32 + Direct3D 12, UI drawn with Dear ImGui). Not web, not mobile.

## Users

MMD (MikuMikuDance) fans who already keep a folder of PMX models, stages and VMD dance
motions, and want to watch a dance play back with their music on a Windows PC. The same
people run the built-in benchmark to compare GPUs. (Inferred from README and the MVP
scope; not interviewed.)

## Product Purpose

Drop assets into a library folder, pick a character, stage and song, and watch the dance
play in sync with the audio at high quality. Success: the scene looks like a modern game
render rather than a flat MMD viewport, and choosing what to play takes seconds.

## Positioning

A native D3D12 MMD player with content-based library classification (any folder layout
works) and a fixed-workload benchmark scored with the web MikuMark formula.

## Operating Context

- Desktop window, mouse and keyboard, usually at 1080p or 4K.
- Playback is watched full screen; the play bar hides while the dance runs.
- The benchmark runs unattended at fixed resolution with vsync off.

## Capabilities and Constraints

- Assets: PMX 2.0/2.1, VMD dance/camera/facial motions, audio wav/mp3/flac/ogg.
- Rendering: rasterization only for this milestone; hardware ray tracing is out of scope.
- UI copy is Korean; asset names can be Japanese or Chinese.
- Leaderboard categories and scores are managed from the official release onward, with a
  hard reset then. Do not touch the server or submit scores before that.

## Brand Commitments

- Light theme by user preference.
- Accent: Hatsune Miku's signature teal-blue (#39C5BB family).
- Fonts: Pretendard (Korean UI) with system JP/CN fallbacks; Phosphor icons.

## Evidence on Hand

- Local test library in `library/` (30 characters, 8 stages, 14 songs). No user
  testimonials, metrics or press exist; do not invent them.

## Product Principles

1. The dance is the hero: UI recedes during playback.
2. Faithful to MMD materials (toon, sphere, edges) while adding modern lighting.
3. Choosing what to play is visual: show the character and stage, not file names.
4. Every graphics feature is measurable and switchable, so the benchmark stays honest.
