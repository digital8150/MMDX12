# Contributing to MMDX12

Bug fixes, small improvements and translations are welcome as pull requests. For a large feature, open an issue first so we can agree on the approach before you spend time on it.

Some ideas fit better as an extension than as a change to the app. Shader packs and stages can be built and shared without touching the engine: see [`docs/shader_effect_api.md`](docs/shader_effect_api.md), [`docs/shader_world_api.md`](docs/shader_world_api.md), [`docs/stage_format.md`](docs/stage_format.md) and the templates in `shaders/pack_template` and `shaders/pack_template_effect`.

## Build

You need Visual Studio 2026 (v18) with the Windows SDK, plus CMake and Ninja (`pip install cmake ninja`).

```text
build.cmd
```

The result is `build\bin\MMDX12.exe`. To enable the upscalers, fetch their libraries once and rebuild:

```powershell
powershell -ExecutionPolicy Bypass -File tools/fetch_sdks.ps1
```

[`CLAUDE.md`](CLAUDE.md) is the detailed project guide: architecture, conventions, command-line options and test tools.

## Pull requests

- Branch from `master` and keep each pull request to one topic. `master` only accepts changes through pull requests.
- The `windows` CI check must pass. It builds the project, runs the tests that need no GPU or assets, and compiles the built-in shader packs.
- Fill in the pull request template, including what you could not test.

### Rendering changes

The CI runners have no GPU, so a green check says nothing about the image. If your change can affect rendering, compare before and after on your own machine and say which render paths you checked (raster, ray traced, path traced, offline GI).

To get the same frame in both runs, build the base commit and your branch in separate build directories, then run this once per build:

```text
python tools/run_capture.py --exe <build dir>/bin/MMDX12.exe --character <model> --stage none --song <song> --autoplay --seek 20 --paused --no-physics --frames 60 --effect none --shader-pack none --capture out.png
```

Use the same `mmdx12.ini` for both builds, because each build directory keeps its own settings. Replace `<model>` and `<song>` with part of a name from your library. Always launch the app through `tools/run_capture.py` for scripted runs: it refuses arguments that could never quit by themselves and reports the warnings and errors from the log.

## Rules

- **Do not commit assets.** That covers models, motions, stages, songs, game textures and anything else you have no right to redistribute. Only post screenshots of assets whose terms allow it.
- **Translate UI text.** Korean is the source language of the UI. Every UI string goes through `Tr("...")` and needs English, Japanese and Chinese entries. The build runs `tools/i18n_check.py`, which fails on missing keys, duplicates and printf mismatches.
- **Leave `external/` alone.** It is vendored third-party code.
- **Keep private details out.** Check that commits contain no local user paths, server names, tokens or keys.

## Reporting bugs and security issues

Use the [issue forms](https://github.com/digital8150/MMDX12/issues/new/choose) for bugs and feature requests. Report security problems privately as described in [`SECURITY.md`](SECURITY.md).

## License

By contributing, you agree that your contribution is released under the project's [MIT license](LICENSE).
