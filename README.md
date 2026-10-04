<div align="center">
  <h1>MMDX12</h1>
  <p><b>English</b> · <a href="README.ko.md">한국어</a> · <a href="README.ja.md">日本語</a></p>
  <p>Drop in your MMD models and dances, hit play, and watch them with ray-traced lighting.<br>When you want something prettier, render a 4K still or a full music video right from the app.</p>
  <p>
    <img src="https://img.shields.io/badge/Platform-Windows%2010%2F11-39C5BB" alt="Platform">
    <img src="https://img.shields.io/badge/Graphics-Direct3D%2012%20%2B%20DXR%201.1-39C5BB" alt="Graphics">
    <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-39C5BB" alt="License"></a>
    <a href="https://youtu.be/7S7D670HbIM"><img src="https://img.shields.io/badge/YouTube-Introduction-39C5BB?logo=youtube&logoColor=white" alt="YouTube"></a>
    <a href="#support"><img src="https://img.shields.io/badge/Donate-Bitcoin-0f3b21?logo=bitcoin&logoColor=white" alt="Donate"></a>
  </p>
  <a href="https://youtu.be/vNztzqvVV4M">
    <img src="docs/media/promo-preview.webp" width="100%" alt="MMDX12 preview">
  </a>
  <br>
  ▶ YouTube: <a href="https://youtu.be/vNztzqvVV4M">58-second promo</a> · <a href="https://youtu.be/7S7D670HbIM">Full introduction</a>
  <p>
    <a href="#gallery">Gallery</a> ·
    <a href="#what-it-does">What it does</a> ·
    <a href="#getting-started">Getting started</a> ·
    <a href="#benchmark">Benchmark</a>
  </p>
</div>

## Gallery

Straight out of the built-in renderer, no touch-ups.

<table>
  <tr>
    <td>
      <img src="docs/media/gi-open-arms.jpg" width="100%">
      <br><i>Soft studio lighting</i>
    </td>
    <td>
      <img src="docs/media/gi-point.jpg" width="100%">
      <br><i>Close-up with a blurry foreground</i>
    </td>
  </tr>
  <tr>
    <td>
      <img src="docs/media/gi-stage.jpg" width="100%">
      <br><i>Night stage</i>
    </td>
    <td>
      <img src="docs/media/gi-render-bench.jpg" width="100%">
      <br><i>The benchmark scene: light bending through a glass cube</i>
    </td>
  </tr>
</table>

## What it does

- **Just throw your files in.** Any folder layout works. MMDX12 figures out which files are characters, stages and dances on its own.
- **Real-time playback** with the classic MMD toon look, or switch on ray tracing and path tracing for realistic light and reflections.
- **Hair and skirts move** with the model's own physics setup.
- **DLSS, FSR and XeSS** if you want more frames.
- **Photo mode**: press P for a 4K picture.
- **Video mode**: render a whole song to MP4, with the music, up to 4K. The app tells you how long it will take before you start.
- **A benchmark** with an online leaderboard, just for fun.

<img src="docs/media/ui-library.jpg" width="100%" alt="Library screen">
<br><sub>The library screen. The app's UI is in Korean.</sub>

## Bring your own assets

> [!IMPORTANT]
> MMDX12 doesn't come with any models, dances, stages or music. MMD assets belong to their creators and can't be redistributed, so you use your own collection. Please follow each asset's rules (many don't allow commercial use, or ask for credit when you post a video). Everything shown here was made with my own local library.

Put everything in a `library` folder next to `MMDX12.exe`, or pick a folder inside the app. A song folder just needs the dance motion (`.vmd`) and the music file. A camera motion is used automatically if there is one.

## Getting started

1. Download `MMDX12-<version>-win64.zip` from [Releases](https://github.com/digital8150/MMDX12/releases/latest) and unzip it anywhere.
2. Add your MMD files to the `library` folder.
3. Run `MMDX12.exe`. Windows may show a SmartScreen warning the first time because the exe isn't signed: click "More info", then "Run anyway".

**What you need:** Windows 10/11 and a DirectX 12 graphics card. Ray tracing, path tracing and the high-quality renderer need a ray-tracing capable GPU (e.g. NVIDIA RTX, AMD RX 6000+, Intel Arc). DLSS needs an NVIDIA RTX card.

### Controls

| Key | Action |
|---|---|
| Space | Play / pause |
| ← / → | Skip 5 seconds |
| C | Switch between the dance's camera and free camera |
| L | Change lighting |
| P | Take a 4K picture (saved to `Pictures\MMDX12`) |
| F1 | Hide the UI |
| Mouse drag / right drag / wheel | Rotate / move / zoom |
| Esc | Back |

Videos are made from the **영상 렌더** (Render video) button next to **플레이** (Play) and saved to `Videos\MMDX12`. The highest-quality mode takes a while: on a laptop RTX 3060 a 4K frame takes about 10 seconds, so a full song is an overnight-or-longer job. You can stop with Esc at any time and keep what's been rendered.

## Benchmark

There's a benchmark mode with real-time tests at 1080p and 4K, plus a Cinebench-style test that renders one 4K picture and times it. You can post your score to the online leaderboard from the result screen.

Since no assets ship with the app, the benchmark builds its scene from your own library (the heaviest models and a suitable song). That means scores only really compare between people with similar libraries, so treat the leaderboard as a toy.

## Building from source

<details>
<summary>For developers</summary>

You'll need Visual Studio 2026 (v18) with the Windows SDK, and CMake + Ninja (`pip install cmake ninja`).

```text
build.cmd
```

The result is `build\bin\MMDX12.exe`. The DLSS / FSR / XeSS libraries are downloaded separately; without them the upscalers are simply turned off:

```powershell
powershell -ExecutionPolicy Bypass -File tools/fetch_sdks.ps1
```

</details>

## Third-party

| Dependency | License |
|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT |
| [stb](https://github.com/nothings/stb) | MIT / Public domain |
| [miniaudio](https://github.com/mackron/miniaudio) | MIT-0 / Public domain |
| [DirectX-Headers](https://github.com/microsoft/DirectX-Headers) | MIT |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT |
| [Bullet Physics 3.25 subset](https://github.com/bulletphysics/bullet3) | zlib |
| [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) | NVIDIA RTX SDK license |
| [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) | MIT |
| [Intel XeSS SDK](https://github.com/intel/xess) | Intel Simplified Software License |
| [Pretendard](https://github.com/orioncactus/pretendard) | SIL OFL 1.1 |
| [Phosphor Icons](https://github.com/phosphor-icons/core) | MIT |

*"MikuMikuDance" was made by Yu Higuchi (樋口優), and Hatsune Miku is a Crypton Future Media character. MMDX12 is an unofficial fan project and isn't affiliated with either.*

## Support

If you enjoy MMDX12, you can buy me a coffee with Bitcoin:

<p>
  <a href="https://pay.digitalism.site/api/v1/invoices?storeId=63rqo9gJMgSoEn8N59SfUmBLrU6KFeLrXVAxtJQh6fgT&price=5&currency=USD"><img src="https://img.shields.io/badge/Donate%20%245-0f3b21?style=for-the-badge&logo=bitcoin&logoColor=white" alt="Donate $5"></a>
  <a href="https://pay.digitalism.site/api/v1/invoices?storeId=63rqo9gJMgSoEn8N59SfUmBLrU6KFeLrXVAxtJQh6fgT&price=10&currency=USD"><img src="https://img.shields.io/badge/Donate%20%2410-0f3b21?style=for-the-badge&logo=bitcoin&logoColor=white" alt="Donate $10"></a>
  <a href="https://pay.digitalism.site/api/v1/invoices?storeId=63rqo9gJMgSoEn8N59SfUmBLrU6KFeLrXVAxtJQh6fgT&price=25&currency=USD"><img src="https://img.shields.io/badge/Donate%20%2425-0f3b21?style=for-the-badge&logo=bitcoin&logoColor=white" alt="Donate $25"></a>
</p>

## License

MMDX12's own code is [MIT](LICENSE). Third-party code keeps its own license, and MMD assets aren't covered.

## What's next

- Material morphs, SDEF skinning, PMD models, and the VMD light track.
- Faster library loading.
