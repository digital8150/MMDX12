<div align="center">
  <h1>MMDX12</h1>
  <p><a href="README.md">English</a> · <a href="README.ko.md">한국어</a> · <a href="README.ja.md">日本語</a> · <b>简体中文</b></p>
  <p>放入你自己的 MMD 模型和舞蹈文件,点一下播放,就能用光线追踪的光照直接观看。<br>想要更好的画质时,还可以在软件里直接渲染 4K 照片,甚至整支音乐视频。</p>
  <p>
    <img src="https://img.shields.io/badge/Platform-Windows%2010%2F11-39C5BB" alt="Platform">
    <img src="https://img.shields.io/badge/Graphics-Direct3D%2012%20%2B%20DXR%201.1-39C5BB" alt="Graphics">
    <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-39C5BB" alt="License"></a>
    <a href="https://www.bilibili.com/video/BV1m8Hr6JEP3/?"><img src="https://img.shields.io/badge/Bilibili-%E4%BB%8B%E7%BB%8D%E8%A7%86%E9%A2%91-39C5BB?logo=bilibili&logoColor=white" alt="Bilibili"></a>
    <a href="#支持"><img src="https://img.shields.io/badge/%E6%94%AF%E6%8C%81-Bitcoin-0f3b21?logo=bitcoin&logoColor=white" alt="支持"></a>
  </p>
  <a href="https://www.bilibili.com/video/BV1m8Hr6JEP3/?">
    <img src="docs/media/promo-preview.webp" width="100%" alt="MMDX12 预览">
  </a>
  <br>
  ▶ <a href="https://www.bilibili.com/video/BV1m8Hr6JEP3/">Bilibili 完整介绍视频</a>
  <p>
    <a href="#画廊">画廊</a> ·
    <a href="#功能">功能</a> ·
    <a href="#开始使用">开始使用</a> ·
    <a href="#影棚">影棚</a> ·
    <a href="#基准测试">基准测试</a>
  </p>
</div>

## 画廊

下面都是内置渲染器直接输出的原图,没有做任何后期修饰。

<table>
  <tr>
    <td>
      <img src="docs/media/gi-open-arms.jpg" width="100%">
      <br><i>柔和的影棚灯光</i>
    </td>
    <td>
      <img src="docs/media/gi-point.jpg" width="100%">
      <br><i>前景大幅虚化的特写</i>
    </td>
  </tr>
  <tr>
    <td>
      <img src="docs/media/gi-stage.jpg" width="100%">
      <br><i>夜晚的舞台</i>
    </td>
    <td>
      <img src="docs/media/gi-render-bench.jpg" width="100%">
      <br><i>基准测试场景:光线穿过玻璃立方体时发生折射</i>
    </td>
  </tr>
</table>

## 视频

用 MMDX12 的视频模式整首歌渲染后,原样上传。

<table>
  <tr>
    <td align="center" width="50%">
      <a href="https://youtu.be/tng53F--jEM"><img src="docs/media/video-monitoring.webp" width="100%" alt="モニタリング (Best Friend Remix)"></a>
      <br><b>モニタリング (Best Friend Remix)</b>
      <br><sub>路径追踪 4K 60FPS</sub>
    </td>
    <td align="center" width="50%">
      <a href="https://youtu.be/WqZzpxBWhj4"><img src="docs/media/video-cute-medley.webp" width="100%" alt="Cute Medley: Idol Sounds"></a>
      <br><b>Cute Medley: Idol Sounds</b>
      <br><sub>路径追踪 QHD 60FPS</sub>
    </td>
  </tr>
  <tr>
    <td align="center" width="50%">
      <a href="https://youtu.be/6JerS9iSD50"><img src="docs/media/video-world-is-mine.webp" width="100%" alt="World is Mine"></a>
      <br><b>World is Mine</b>
      <br><sub>路径追踪 QHD 24FPS</sub>
    </td>
    <td align="center" width="50%">
      <a href="https://youtu.be/xYYAZFU4Yz8"><img src="docs/media/video-catch-the-wave.webp" width="100%" alt="Catch the Wave"></a>
      <br><b>Catch the Wave</b>
      <br><sub>路径追踪 4K 60FPS</sub>
    </td>
  </tr>
</table>

## 功能

- **PMX、VRM、glTF、FBX。** VRoid、Mixamo、Ready Player Me 的模型也能跟着 MMD 动作跳舞,glTF/FBX/OBJ 场景可以当舞台。
- **随意整理。** 分到 `characters`、`stages`、`songs` 文件夹最可靠,不分也会按内容识别;认错了可以在软件里改。
- **实时播放。** 可以保持熟悉的 MMD 卡通风格,也可以打开光线追踪或路径追踪,看到真实的光影和反射。
- **头发和裙子会摆动。** 直接使用模型自带的物理设置。
- 支持 **DLSS、FSR、XeSS**。帧率不够时可以试试。
- **拍照模式**:按一下 P 键,输出 4K 照片。
- **视频模式**:整首歌连同音乐一起导出为 MP4,最高 4K。开始之前还会告诉你大概要花多久。
- **影棚**:MMD 风格的关键帧编辑器,可以自己编辑舞蹈,并保存为 MMD 能够打开的标准 VMD/VPD 文件。
- **基准测试**和在线排行榜,纯属好玩。

<img src="docs/media/ui-library.jpg" width="100%" alt="素材库界面">
<br><sub>素材库界面</sub>

## 素材请自行准备

> [!IMPORTANT]
> MMDX12 本身不含任何模型、舞蹈、舞台和音乐。MMD 素材归创作者所有,无法随软件一起分发。请使用你自己拥有的素材,并务必遵守各素材的使用规约(例如禁止商用、发布视频时需要标注署名等)。这里展示的图片和视频,都是用我个人的素材库制作的。

把素材放进 `MMDX12.exe` 旁边的 `library` 文件夹(也可以在软件里选择其他文件夹)。里面自带三个文件夹:

| 文件夹 | 放什么 | 格式 |
|---|---|---|
| `characters/` | 每个模型一个文件夹(含贴图) | PMX, VRM, glTF/GLB, FBX |
| `stages/` | 每个舞台一个文件夹(其中的 PMX 一起绘制) | PMX, glTF/GLB, FBX, OBJ |
| `songs/` | 每首歌一个文件夹:舞蹈动作、镜头动作、音乐 | VMD, WAV/MP3/OGG/FLAC |

其他整理方式也可以:能识别 `models`、`motions` 之类的文件夹名,没有的话就按内容判断。判断错了可以右键点击卡片改为角色/舞台或隐藏,也可以在文件夹里放 `mmdx.json` 自行指定(例如 `{"type": "song", "dance": "dance.vmd", "camera": "cam.vmd", "audio": "song.mp3"}`)。PMX 以外的模型需要人形骨骼(VRM、VRoid、Mixamo、Ready Player Me、Unity/Blender/UE 骨架),读取时会转换为 MMD 骨骼,暂不支持头发和裙子的物理。

## 开始使用

1. 从 [Releases](https://github.com/digital8150/MMDX12/releases/latest) 下载 `MMDX12-<版本>-win64.zip`,解压到任意位置。
2. 把 MMD 文件放进 `library` 文件夹。
3. 运行 `MMDX12.exe`。由于是未签名的可执行文件,第一次运行时可能会弹出 SmartScreen 警告,点击"更多信息"→"仍要运行"即可。

**系统要求:** Windows 10/11,支持 DirectX 12 的显卡。光线追踪、路径追踪和高质量渲染器需要支持光线追踪的显卡(NVIDIA RTX、AMD RX 6000 及以上、Intel Arc 等),DLSS 仅限 NVIDIA RTX。

### 操作方式

| 按键 | 功能 |
|---|---|
| Space | 播放 / 暂停 |
| ← / → | 后退 / 前进 5 秒 |
| C | 舞蹈自带镜头 ↔ 自由镜头 |
| L | 切换灯光 |
| P | 拍摄 4K 照片(保存到 `图片\MMDX12`) |
| F1 | 隐藏界面 |
| 鼠标拖动 / 右键拖动 / 滚轮 | 旋转 / 平移 / 缩放 |
| Esc | 返回 |

视频通过 **Play** 旁边的 **Render video** 按钮制作,保存到 `视频\MMDX12`。用最高画质渲染会花不少时间:在笔记本 RTX 3060 上,4K 每帧大约 10 秒,所以一首歌要跑一整夜以上。中途按 Esc 停止,已渲染的部分也会保留。

## 影棚

<img src="docs/media/studio.jpg" width="100%" alt="Studio">

在素材库界面中，未选择任何内容时点击 **新建影棚项目** 可打开空项目；选中角色或曲目后点击 **在影棚中编辑**，则会带入预设场景开始。旁边的 `...` 菜单可打开已有项目或最近的项目。

画面左侧的场景列表用于管理镜头、角色、配件、舞台和音频，点击 `+` 按钮可从资料库或文件添加模型（PMX、VRM、glTF、FBX）。中间是 3D 视图，右侧是检查器（关键帧、骨骼、表情标签页，选中镜头时包含镜头、照明和自阴影设置），底部则是按模型显示框分组的骨骼时间轴与贝塞尔插值曲线编辑器。

在 3D 视图中点击骨骼，可通过旋转/移动操纵器（局部/全局轴）调整姿势。移动 IK 骨骼时会自动联动骨骼链。与 MMD 一样，按下登记关键帧（`I`）之前编辑的姿势会一直保持，还支持表情滑块调节、左右镜像以及 VPD 姿势文件的导入与保存。时间轴支持添加、移动、删除、复制、剪切、粘贴关键帧，仅粘贴插值曲线，曲线预设，类似 MMD 的插入与删除帧，区间选择，伴随音乐循环播放，以及开关物理。镜头、照明与自阴影轨道同样可以编辑，支持"将当前视角登记为关键帧"，并在自由镜头下显示镜头运动路径。

项目保存为 `.mmdxproj` 文件，各动作文件保存在其旁边，为 MMD 可打开的常规 VMD 文件。支持崩溃后的自动保存恢复、音频起始偏移调整，以及将配件附加到其他模型骨骼上的摆放功能。在影棚内可直接调用与素材库相同的渲染对话框（光栅化、实时光线追踪、实时路径追踪、离线 GI），通过镜头动作配合音乐将时间轴区间或整个项目导出为视频或高质量静态图。与 MMD 的兼容性通过标准文件（VMD、VPD）实现，不支持 PMM 工程文件。

| 按键 | 功能 |
|---|---|
| Space | 播放 / 暂停 |
| ← / → | 上一帧 / 下一帧 |
| Ctrl+← / → | 上一关键帧 / 下一关键帧 |
| Home / End | 回到开头 / 跳到结尾 |
| I | 登记姿势 (Ctrl+I: 登记所有骨骼) |
| Delete | 删除选中的关键帧 |
| Ctrl+C / X / V | 复制 / 剪切 / 粘贴关键帧 (Ctrl+Shift+V: 仅插值曲线) |
| Ctrl+A | 选择所有关键帧 |
| Ctrl+Z / Ctrl+Y | 撤销 / 重做 |
| E / W / L | 旋转工具 / 移动工具 / 局部 ↔ 全局轴 |
| Ctrl+S / Ctrl+Shift+S | 保存 / 另存为 |
| Ctrl+O / Ctrl+N | 打开 / 新建项目 |
| ? / F1 | 快捷键列表 |
| 3D 视图鼠标操作 | 左键拖动: 旋转视角 (或操纵器 / 选取骨骼) · 右键或中键拖动: 平移 · 滚轮: 缩放 |
| 时间轴鼠标操作 | 滚轮: 滚动 · Ctrl+滚轮: 缩放 · Shift+滚轮: 横向滚动 · 标尺上 Shift+拖动: 设置区间 |

## 基准测试

内置了类似 Cinebench 的测试,会测量 1080p 和 4K 实时渲染,以及渲染一张 4K 照片所需的时间。在结果界面可以把成绩提交到在线排行榜。

由于无法附带素材,基准测试的场景也是由每个人自己的素材库生成的(自动选取最重的模型和长度合适的歌曲)。所以只有素材库相近的人之间才能正确比较,排行榜就当娱乐看看吧。

## 从源码构建

<details>
<summary>开发者向</summary>

需要 Visual Studio 2026 (v18)、Windows SDK,以及 CMake + Ninja(`pip install cmake ninja`)。

```text
build.cmd
```

产物是 `build\bin\MMDX12.exe`。DLSS / FSR / XeSS 的库需要另行下载,缺少时只会禁用对应的超分功能。

```powershell
powershell -ExecutionPolicy Bypass -File tools/fetch_sdks.ps1
```

</details>

## 第三方组件

| 依赖 | 许可证 |
|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT |
| [stb](https://github.com/nothings/stb) | MIT / 公有领域 |
| [miniaudio](https://github.com/mackron/miniaudio) | MIT-0 / 公有领域 |
| [DirectX-Headers](https://github.com/microsoft/DirectX-Headers) | MIT |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT |
| [cgltf](https://github.com/jkuhlmann/cgltf) | MIT |
| [ufbx](https://github.com/ufbx/ufbx) | MIT / Public domain |
| [Bullet Physics 3.25 subset](https://github.com/bulletphysics/bullet3) | zlib |
| [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) | NVIDIA RTX SDK license |
| [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) | MIT |
| [Intel XeSS SDK](https://github.com/intel/xess) | Intel Simplified Software License |
| [Pretendard](https://github.com/orioncactus/pretendard) | SIL OFL 1.1 |
| [Phosphor Icons](https://github.com/phosphor-icons/core) | MIT |

*"MikuMikuDance"是樋口优先生的作品,初音未来是 Crypton Future Media 的角色。MMDX12 是非官方的粉丝项目,与两者均无关系。*

## 支持

如果你喜欢 MMDX12,欢迎用比特币请我喝杯咖啡:

<p>
  <a href="https://pay.digitalism.site/api/v1/invoices?storeId=63rqo9gJMgSoEn8N59SfUmBLrU6KFeLrXVAxtJQh6fgT&price=5000&currency=KRW"><img src="https://img.shields.io/badge/%E6%94%AF%E6%8C%81%20%E2%82%A95%2C000-0f3b21?style=for-the-badge&logo=bitcoin&logoColor=white" alt="支持 ₩5,000"></a>
  <a href="https://pay.digitalism.site/api/v1/invoices?storeId=63rqo9gJMgSoEn8N59SfUmBLrU6KFeLrXVAxtJQh6fgT&price=10000&currency=KRW"><img src="https://img.shields.io/badge/%E6%94%AF%E6%8C%81%20%E2%82%A910%2C000-0f3b21?style=for-the-badge&logo=bitcoin&logoColor=white" alt="支持 ₩10,000"></a>
  <a href="https://pay.digitalism.site/api/v1/invoices?storeId=63rqo9gJMgSoEn8N59SfUmBLrU6KFeLrXVAxtJQh6fgT&price=30000&currency=KRW"><img src="https://img.shields.io/badge/%E6%94%AF%E6%8C%81%20%E2%82%A930%2C000-0f3b21?style=for-the-badge&logo=bitcoin&logoColor=white" alt="支持 ₩30,000"></a>
</p>

## 许可证

MMDX12 的代码采用 [MIT](LICENSE) 许可证。第三方代码遵循各自的许可证,MMD 素材不适用。

## 后续计划

- 材质变形、SDEF 蒙皮、PMD 模型、播放界面的 VMD 灯光轨道。
- 更快的素材库加载。
- 影棚后续: 配件(.x)、PMM 导入。
