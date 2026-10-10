# Design

The MMDX12 visual system, implemented in `src/app/UiKit.{h,cpp}`. Native Win32 + Dear ImGui:
every widget is drawn by hand from these tokens, never with stock ImGui styling.

## Direction

A light (or dark: see below), quiet app shell around a loud subject. The dance (and the character portraits) carry
the colour; the chrome is cool off-white, near-black ink and one accent: Hatsune Miku's teal.
Modes: the library and benchmark are Operate surfaces (scan, pick, compare); playback is
Experience (the UI slides away and the scene owns the screen).

## Color (sRGB)

| Token | Hex | Use |
|---|---|---|
| bg | #F3F5F7 | screen background |
| surface | #FFFFFF | panels, cards, app bar |
| sunken | #EBEEF1 | tracks, segmented background, search, skeletons |
| line / lineStrong | #E2E6EA / #CBD2D9 | hairlines, input borders |
| ink / ink2 / ink3 | #13171C / #4C5560 / #7D8792 | text, secondary, tertiary |
| accent | #39C5BB | primary fills, selection rings, progress, switches |
| accentHover / accentPress | #2FB5AB / #26A198 | primary states |
| accentInk | #0A7F78 | accent as text/icon on light (4.8:1) |
| accentSoft | #E2F5F3 | selected backgrounds, portrait backdrop |
| onAccent | #052B28 | label on accent fill (8.7:1) |
| danger / warn (+Soft) | #C7383F / #9A6400 | errors, missing-asset badges |

Shadows are tinted toward teal-grey (`rgb(18,52,58)`), layered (7 steps) and offset downward.
The single accent is never decorative: it marks the primary action, the current selection
and progress.

## Dark theme

Same hue family, surfaces and ink swapped (`MakeDarkPalette` in UiKit.cpp; `P()` is the live palette, `ui::SetTheme`).

| Token | Dark |
|---|---|
| bg / surface / sunken | #0F1316 / #181D22 / #0C0F12 (sunken stays the recessed tone) |
| line / lineStrong | #262D34 / #3A444E |
| ink / ink2 / ink3 | #E8ECEF / #A9B3BC / #78838E |
| accent / accentInk / accentSoft | #39C5BB / #5ED8CE (text) / #143331 |
| danger / warn | #F06A71 / #E3AB45 |

Shadows turn plain black and denser, the frosted veil's white hairline drops to 9 %, tooltips become a raised
surface (not an inverted bubble). `AppSettings::theme` (0 system, 1 light, 2 dark; `--theme auto|light|dark`
overrides one run); system = Windows' app mode, re-read on `WM_SETTINGCHANGE`. The title bar follows
(`DWMWA_USE_IMMERSIVE_DARK_MODE`), the splash is themed. The theme button (sun / moon) sits in the library app bar and
the studio top bar. Scene-like drawings (stage placeholders, viewport overlays, gizmos) are not themed.

## Type

Pretendard (Regular / SemiBold / Bold), Phosphor icons merged into each face (filled set in Bold),
YuGothic / Microsoft YaHei fallbacks for asset names. Scale (px at 100% DPI): Caption 12.5,
Small 13.5, Body 14.5, Title 17, Heading 22, Hero 30, Display 64 (benchmark score).
The wordmark is "MMDX" in ink plus "12" in accentInk.

## Shape and space

Radii: controls 10, chips/pills full, cards 14, panels 18-22. Spacing on a 4 px grid; screen
padding 28, card gap 16, panel padding 20-32. App bar 64 high. Elevation is a soft shadow plus a
6%-ink hairline; cards lift 3 px on hover.

## Components

Button (Primary, Secondary, Ghost, Danger), IconButton (round, tooltip), Segmented (pill with a
sliding thumb), Switch, SliderRow, SearchField, TextField (label above), Chip, Badge,
ProgressBar, Skeleton (shimmer), Panel, FrostedPanel. Every interactive item supports hover,
pressed, disabled and keyboard-nav focus.

## Surfaces

- Library: tabs (characters / stages / songs) with counts, search, a card grid (3:4 portraits,
  16:10 stages) or song rows with feature badges; the right panel composes the chosen character
  over the chosen stage, then quality, lighting, detailed settings and the Play button.
- Play: frosted panels (the blurred scene from the renderer's UI backdrop pass under a 68-72%
  white veil) for the title, the perf pill and the control bar. They slide out after 3 s
  without mouse movement.
- Benchmark: resolution cards, the measured scene, nickname, start; leaderboard table with
  skeleton, error and empty states; result with the score in Display type and a stats grid.

## Motion

About 120-150 ms exponential easing (`ui::Anim`) for hover, selection, switch knobs and the
segmented thumb; the play overlay eases in and out with a cubic curve. Motion only conveys
state.
