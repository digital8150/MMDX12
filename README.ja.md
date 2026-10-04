<div align="center">
  <h1>MMDX12</h1>
  <p><a href="README.md">English</a> · <a href="README.ko.md">한국어</a> · <b>日本語</b> · <a href="README.zh.md">简体中文</a></p>
  <p>手持ちの MMD モデルとダンスのファイルを入れて再生を押すだけで、レイトレーシングのライティングですぐに見られます。<br>もっときれいに出したいときは、アプリの中から 4K の写真やミュージックビデオまでレンダリングできます。</p>
  <p>
    <img src="https://img.shields.io/badge/Platform-Windows%2010%2F11-39C5BB" alt="Platform">
    <img src="https://img.shields.io/badge/Graphics-Direct3D%2012%20%2B%20DXR%201.1-39C5BB" alt="Graphics">
    <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-39C5BB" alt="License"></a>
    <a href="https://youtu.be/j8nnveh7Bug"><img src="https://img.shields.io/badge/YouTube-Introduction-39C5BB?logo=youtube&logoColor=white" alt="YouTube"></a>
    <a href="#支援"><img src="https://img.shields.io/badge/%E6%94%AF%E6%8F%B4-Bitcoin-0f3b21?logo=bitcoin&logoColor=white" alt="支援"></a>
  </p>
  <a href="https://youtu.be/j8nnveh7Bug">
    <img src="docs/media/promo-preview.webp" width="100%" alt="MMDX12 プレビュー">
  </a>
  <br>
  ▶ YouTube: <a href="https://youtu.be/j8nnveh7Bug">フル紹介動画</a>
  <p>
    <a href="#ギャラリー">ギャラリー</a> ·
    <a href="#できること">できること</a> ·
    <a href="#はじめかた">はじめかた</a> ·
    <a href="#ベンチマーク">ベンチマーク</a>
  </p>
</div>

## ギャラリー

内蔵のレンダラーで出したそのままの画像です。補正はしていません。

<table>
  <tr>
    <td>
      <img src="docs/media/gi-open-arms.jpg" width="100%">
      <br><i>やわらかいスタジオ照明</i>
    </td>
    <td>
      <img src="docs/media/gi-point.jpg" width="100%">
      <br><i>手前が大きくボケたクローズアップ</i>
    </td>
  </tr>
  <tr>
    <td>
      <img src="docs/media/gi-stage.jpg" width="100%">
      <br><i>夜のステージ</i>
    </td>
    <td>
      <img src="docs/media/gi-render-bench.jpg" width="100%">
      <br><i>ベンチマークのシーン: ガラスのキューブを通って屈折する光</i>
    </td>
  </tr>
</table>

## 動画

MMDX12 の動画モードで 1 曲まるごとレンダリングして、そのままアップロードしました。

<ul>
  <li><a href="https://youtu.be/tng53F--jEM">モニタリング (Best Friend Remix)</a> (パストレーシング 4K 60FPS)</li>
  <li><a href="https://youtu.be/WqZzpxBWhj4">Cute Medley: Idol Sounds</a> (パストレーシング QHD 60FPS)</li>
  <li><a href="https://youtu.be/6JerS9iSD50">World is Mine</a> (パストレーシング QHD 24FPS)</li>
</ul>

## できること

- **ファイルは放り込むだけ。** フォルダを整理しなくても、キャラクター、ステージ、ダンスを自動で見分けます。
- **リアルタイム再生。** おなじみの MMD のトゥーン風のまま見ることも、レイトレーシングやパストレーシングをオンにして、リアルな光と反射で見ることもできます。
- **髪やスカートが揺れます。** モデルに入っている物理設定をそのまま使います。
- **DLSS、FSR、XeSS** に対応。フレームレートが物足りないときに試してみてください。
- **フォトモード**: P キーひとつで 4K の写真。
- **ビデオモード**: 曲を丸ごと、音楽つきの MP4 に最大 4K で書き出せます。始める前に、どれくらいかかるかも教えてくれます。
- **ベンチマーク**とオンラインのランキング。ちょっとした遊びです。

<img src="docs/media/ui-library.jpg" width="100%" alt="ライブラリ画面">
<br><sub>ライブラリ画面</sub>

## アセットはご自身で用意してください

> [!IMPORTANT]
> MMDX12 には、モデル・ダンス・ステージ・音楽がひとつも入っていません。MMD のアセットは作った方々のものなので、一緒に配布できないためです。お手持ちのものを使い、アセットごとの規約(商用利用の禁止や、動画を公開するときのクレジット表記など)は必ず守ってください。ここに載せている画像や動画は、私個人のライブラリで作ったものです。

`MMDX12.exe` の隣にある `library` フォルダに入れるか、アプリで好きなフォルダを選んでください。曲のフォルダには、ダンスのモーション(`.vmd`)と音楽ファイルだけあれば大丈夫です。カメラモーションがあれば、自動でいっしょに使います。

## はじめかた

1. [Releases](https://github.com/digital8150/MMDX12/releases/latest) から `MMDX12-<バージョン>-win64.zip` をダウンロードして、好きな場所に展開します。
2. `library` フォルダに MMD のファイルを入れます。
3. `MMDX12.exe` を起動します。署名のない実行ファイルなので、最初は SmartScreen の警告が出ることがあります。「詳細情報」→「実行」を押してください。

**必要なもの:** Windows 10/11、DirectX 12 対応のグラフィックスカード。レイトレーシング、パストレーシング、高品質レンダラーにはレイトレーシング対応のグラフィックスカード(NVIDIA RTX、AMD RX 6000 以降、Intel Arc など)が必要で、DLSS は NVIDIA RTX 専用です。

### 操作方法

| キー | 機能 |
|---|---|
| Space | 再生 / 一時停止 |
| ← / → | 5秒戻る / 進む |
| C | ダンスに入っているカメラ ↔ フリーカメラ |
| L | ライティングの切り替え |
| P | 4K の写真を撮る(`ピクチャ\MMDX12` に保存) |
| F1 | UI を隠す |
| マウスドラッグ / 右ドラッグ / ホイール | 回転 / 移動 / ズーム |
| Esc | 戻る |

ビデオは **プレイ** の隣の **ビデオレンダー** ボタンで作れて、`ビデオ\MMDX12` に保存されます。最高画質で回すとかなり時間がかかります。ノート PC の RTX 3060 だと 4K の 1 フレームに 10 秒ほどなので、曲 1 曲ぶんだと一晩以上かかります。途中で Esc で止めても、そこまでレンダリングした分は残ります。

## ベンチマーク

1080p と 4K のリアルタイムテスト、そして 4K の写真 1 枚を描くのにかかる時間を測る、シネベンチのようなテストがあります。結果画面からオンラインのランキングに登録できます。

アセットを同梱できないので、ベンチマークのシーンも各自のライブラリから作ります(いちばん重いモデルと、ちょうどいい長さの曲を選んで)。そのため、ライブラリが近い人どうしでしか正しく比較できません。ランキングは遊びとして眺めてください。

## ソースからビルドする

<details>
<summary>開発者向け</summary>

Visual Studio 2026 (v18) と Windows SDK、そして CMake + Ninja(`pip install cmake ninja`)が必要です。

```text
build.cmd
```

成果物は `build\bin\MMDX12.exe` です。DLSS / FSR / XeSS のライブラリは別途ダウンロードが必要で、なければアップスケーラーだけが無効になります。

```powershell
powershell -ExecutionPolicy Bypass -File tools/fetch_sdks.ps1
```

</details>

## サードパーティ

| 依存ライブラリ | ライセンス |
|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT |
| [stb](https://github.com/nothings/stb) | MIT / パブリックドメイン |
| [miniaudio](https://github.com/mackron/miniaudio) | MIT-0 / パブリックドメイン |
| [DirectX-Headers](https://github.com/microsoft/DirectX-Headers) | MIT |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT |
| [Bullet Physics 3.25 subset](https://github.com/bulletphysics/bullet3) | zlib |
| [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) | NVIDIA RTX SDK license |
| [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) | MIT |
| [Intel XeSS SDK](https://github.com/intel/xess) | Intel Simplified Software License |
| [Pretendard](https://github.com/orioncactus/pretendard) | SIL OFL 1.1 |
| [Phosphor Icons](https://github.com/phosphor-icons/core) | MIT |

*「MikuMikuDance」は樋口優さんによる作品で、初音ミクはクリプトン・フューチャー・メディアのキャラクターです。MMDX12 は非公式のファンプロジェクトで、どちらとも関係ありません。*

## 支援

MMDX12 を気に入っていただけたら、ビットコインでコーヒーをごちそうしてください:

<p>
  <a href="https://pay.digitalism.site/api/v1/invoices?storeId=63rqo9gJMgSoEn8N59SfUmBLrU6KFeLrXVAxtJQh6fgT&price=5000&currency=KRW"><img src="https://img.shields.io/badge/%E6%94%AF%E6%8F%B4%20%E2%82%A95%2C000-0f3b21?style=for-the-badge&logo=bitcoin&logoColor=white" alt="支援 ₩5,000"></a>
  <a href="https://pay.digitalism.site/api/v1/invoices?storeId=63rqo9gJMgSoEn8N59SfUmBLrU6KFeLrXVAxtJQh6fgT&price=10000&currency=KRW"><img src="https://img.shields.io/badge/%E6%94%AF%E6%8F%B4%20%E2%82%A910%2C000-0f3b21?style=for-the-badge&logo=bitcoin&logoColor=white" alt="支援 ₩10,000"></a>
  <a href="https://pay.digitalism.site/api/v1/invoices?storeId=63rqo9gJMgSoEn8N59SfUmBLrU6KFeLrXVAxtJQh6fgT&price=30000&currency=KRW"><img src="https://img.shields.io/badge/%E6%94%AF%E6%8F%B4%20%E2%82%A930%2C000-0f3b21?style=for-the-badge&logo=bitcoin&logoColor=white" alt="支援 ₩30,000"></a>
</p>

## ライセンス

MMDX12 のコードは [MIT](LICENSE) です。サードパーティのコードはそれぞれのライセンスに従い、MMD のアセットは対象外です。

## 今後の予定

- マテリアルモーフ、SDEF スキニング、PMD モデル、VMD のライティングトラック。
- ライブラリの読み込みをもっと速く。
