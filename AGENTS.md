# AGENTS.md

MMDX12: a Windows Direct3D 12 MikuMikuDance client (C++20). The full project guide (build, architecture,
conventions, tools) is CLAUDE.md: read it first; it applies to every agent. The CLI section below is duplicated here
because agents misuse it most; keep both copies identical when you change it.

## Running MMDX12.exe from the CLI (read this before EVERY launch)
Agents keep misusing the CLI and then blaming the app. These rules are verified (2026-10-12); follow them literally.
- **No `--autoplay` = no scene.** `--character/--song` only preselect cards in the lobby. Without `--autoplay` (or when
  either name does not resolve) the app sits in the lobby, and `--frames` never counts there (it counts only inside a
  scene or with `--screen`), so the run never quits. Every scene capture needs `--character <s> --song <s> --autoplay`.
- **Always wrap a run in a timeout and read `<build>/bin/mmdx12.log` afterwards** (overwritten every run;
  `mmdx12.previous.log` keeps the one before); `tools/run_capture.py` does both for you (it is the only way to launch).
  Scene reached = `motion bound` + `STARTUP first frame` lines; neither =
  it never left the lobby (bad `--character/--song`, multi-part song without `--formation`, or a cold library scan in a
  fresh build dir: the first run of a build dir rescans the library, ~25 s). Grep `[E]`/`[W]` after every capture;
  unknown arguments are only a `[W] unknown command line argument` line, never an error.
- **Never delete `<build>/bin/cache/library_result.json`** to force a rescan (a worker did, 2026-10-27, and every
  capture was then refused). A scan cancelled by the app quitting is never saved, so a short `--frames` run cannot
  rebuild it: run `python tools/run_capture.py --timeout 300 -- --screen select --frames 30000` once, which also
  picks up new `<exe>/stages` folders.
- **Name matching** = case-insensitive substring of the library id (the relative path, e.g.
  `model/Sekai Model Pack v4/LeoNeed/Ichika/01_9001.pmx`) or the display name; the FIRST match wins, so use a
  distinctive fragment (`01_9001`, `LeoNeed/Ichika`). `--stage none` = no stage. Songs with >= 2 dance parts open the
  formation modal (except in the studio, which loads its own models): pass `--formation` (see below) or pick a
  one-part song. Ids: `build/bin/cache/library_result.json`.
- **Same frame across runs (A/B comparisons): `--seek <sec> --paused --no-physics`.**
  `--seek` alone keeps playing: the capture happens `--frames` frames later in wall-clock time, so two identical runs
  differ (measured: 113k of 518k pixels). `--paused` holds the scene at the seek time (residual 1.2k px = physics
  settling); adding `--no-physics` makes depth, velocity and the lit scene match between runs, but not always the final
  image: the half-res SSR / SSAO passes fall into two states per run (neon_plaza: 15.5k px, max 25 levels, sparse X marks
  in the floor reflection, cause not found; spot pick, camera, bones, constants, bytecode, frame index and uv all ruled
  out, 2026-11-07), so diff against both states before calling a difference real. Use `--frames 60` or more so TAA
  converges. `--seek`, `--autoplay` and `--paused` are one-shot: consumed when the scene loads.
- **Settings come from `<build dir>/bin/mmdx12.ini`, one per build dir**, and they differ (window size, lighting preset,
  DoF, effect stack, shader packs per character). An A/B between two build dirs must either copy the same ini into both
  (back it up and restore it) or pin everything on the command line: `--width W --height H --render raster|rt|pt
  --upscaler none --lighting <n> --dof 0|1 --effect none --shader-pack none|<id>` (+ `--camera tx,ty,tz,yawDeg,pitchDeg,dist`
  for a fixed free camera; it implies `--free-camera`). A HEAD baseline = `git stash`, build another dir, `git stash pop`.
- **One window at a time.** Every run opens a visible window: never launch runs in parallel, tell the user before a
  batch. A running exe locks its build dir (`LNK1168`): when a worker is building in `build/`, run another build dir.
- **Agents and workers launch the app ONLY through `tools/run_capture.py`** (never `MMDX12.exe` directly, never
  `timeout ...`): it refuses argument lists that cannot reach a scene or cannot quit by themselves (one window at a
  time, a real `--character/--song/--stage` from `<exe dir>/cache/library_result.json`, `--formation` for multi-part
  songs, a quit condition), defaults `--width 1600 --height 900`, kills the process tree on a missing `STARTUP first
  frame` / `motion bound` / an unknown-argument line / `--timeout`, and prints the exit reason, elapsed time, the
  `[E]`/`[W]` lines and the capture path + size. Exit 0 = quit by itself, target reached, capture present; 2 = refused.
  `python tools/run_capture.py --selftest` checks the pre-flight logic without launching.
  - Play capture: `python tools/run_capture.py --character 01_9001 --stage none --song Evergreen --autoplay --seek 20 --paused --no-physics --frames 60 --effect none --shader-pack none --capture out.png`
  - Studio ui-script capture (scripts use absolute pixel coordinates: pin the window size the script was authored for,
    e.g. 1920 x 1080 overrides the 1600 x 900 default):
    `python tools/run_capture.py --exe build_dev/bin/MMDX12.exe --character 01_9001 --screen studio --width 1920 --height 1080 --ui-script captures/studio/m2_a.txt --frames 120 --capture captures/out.png`
  - Wrapper options go before the app's own arguments (`--exe`, `--timeout`, `--first-frame-timeout`,
    `--allow-gui` for a deliberate interactive run, `--allow-library`); `--` is optional.
- Canonical A/B capture (run once per build, then diff the PNGs):
  `python tools/run_capture.py --exe <build>/bin/MMDX12.exe --character 01_9001 --stage none --song Evergreen --autoplay --seek 20 --paused --no-physics --frames 60 --effect none --shader-pack none --capture out.png`
