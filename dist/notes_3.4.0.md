# MMDX12 3.4.0 — Stages on the Beat, Glass Reflections and Studio Physics Settings

> 🌐 **Language:** [한국어](#한국어) · [English](#english) · [日本語](#日本語) · [简体中文](#简体中文)

## 한국어
### ✨ 새 기능
- **스테이지가 음악 박자에 반응해요**: MMDX 스테이지가 조명·재질·장치를 곡의 박자, 마디, 음량에 맞춰 스스로 움직일 수 있어요(stage.json의 `drivers`). 월드 이펙트와 스테이지 스크린 팩도 같은 오디오 입력을 쓸 수 있어요.
- **RT 렌더러에서 유리에 장면이 비쳐요**: 레이 트레이싱 경로에서 유리(투과 재질)가 실내와 캐릭터를 반사해요. 밤의 통유리 창처럼 정면으로 보는 유리에도 반사가 나와요. 패스 트레이싱과 GI 렌더는 원래 지원했어요.
- **젖은 바닥이 캐릭터를 제대로 비춰요**: PBR 재질의 반사가 하늘만 비추고 캐릭터와 무대는 희미하게만 나오던 문제를 고쳤어요(RT 반사와 화면 공간 반사 모두).
- **스튜디오: 중력과 바람 설정**: 재생 바의 물리 팝업에서 중력의 세기·방향과 바람(세기, 돌풍)을 바꿀 수 있고, 프로젝트에 저장돼요. 뷰포트·렌더·베이크가 같은 돌풍 타이밍을 써요.
- **스튜디오: 오디오 위치 잠금**: 타임라인의 오디오 레인을 잠가서 실수로 음악 위치가 밀리지 않게 할 수 있어요.
- **렌더가 끝나면 PC를 끌 수 있어요**: 영상 렌더 창의 "완료 후 시스템 종료"를 켜면 긴 렌더가 끝난 뒤 자동으로 종료해요(취소할 수 있는 카운트다운이 먼저 떠요).
- **나이틀리 채널**: 최신 개발 빌드를 따로 받아 볼 수 있어요. 정식 버전과 다른 폴더에 설치해서 쓰세요.

### 🐞 수정 및 개선
- **무거운 스테이지가 훨씬 가벼워졌어요**: 카메라 화면이 여러 개인 스테이지는 같은 카메라를 한 번만 그리고, 큰 스테이지는 보이는 구역만 그려요(예: 네온 플라자 45 → 21 ms, RTX 4070 Ti).
- **스팟 조명 그림자가 필요한 곳에 쓰여요**: 캐릭터에 닿지 않는 스팟은 그림자 맵을 차지하지 않아서, 조명이 많은 스테이지에서 중요한 조명의 그림자가 빠지지 않아요.
- **스튜디오 입력 칸의 실행 취소가 정확해요**: 키·포즈·변환 칸에 값을 직접 입력하면 실행 취소 한 번으로 되돌아가요. 카메라 칸은 트랙의 값을 보여 주고, 자동 키는 기본이 꺼짐이며 마지막 설정을 기억해요.
- **스튜디오에서 스테이지의 이미지 하늘이 불러와져요**: 스테이지 폴더 기준의 하늘 이미지 경로를 찾지 못하던 문제를 고쳤어요.
- **스테이지 제작: 하늘 밝기 바인딩**: `lights.<이름>.sky.intensity` 대상이 오류로 거부되던 문제를 고쳤어요.
- **업데이트가 더 안전해요**: 업데이트 정보에 서명을 붙이고, 서명이 맞지 않으면 앱이 받지 않아요. 선택 구성 요소도 해시를 확인해요.

### 🙏 기여
- 중력·바람 설정, 입력 칸 실행 취소 등 스튜디오 개선은 quasiquiescent 님의 기여예요. 감사합니다!

### 📦 안내
- 자동 업데이트로 받을 수 있어요(변경된 파일만 내려받아요).
- 업데이트 직후 첫 실행은 셰이더를 다시 컴파일해서 평소보다 오래 걸려요.

## English
### ✨ What's New
- **Stages react to the beat**: an MMDX stage can drive its lights, materials and devices from the song's beat, bar and energy by itself (`drivers` in stage.json). World effects and stage screen packs can read the same audio inputs.
- **Glass reflects the scene in the RT renderer**: on the ray-traced path, glass (transmissive materials) now reflects the room and the performers, including glass seen head-on such as a window wall at night. Path tracing and GI renders already did.
- **Wet floors mirror the performers**: reflections on PBR materials showed mostly the sky and only a faint trace of characters and the stage. Fixed for ray-traced and screen-space reflections.
- **Studio: gravity and wind**: the physics popup on the transport bar sets gravity strength and direction and the wind (strength, gusts), saved in the project. The viewport, renders and bakes share the same gust timing.
- **Studio: lock the audio position**: lock the timeline's audio lane so the music cannot be moved by accident.
- **Shut the PC down after a render**: turn on "shut down when finished" in the video render dialog for long renders (a countdown you can cancel comes first).
- **Nightly channel**: the latest development builds are available separately. Install them in a different folder from the stable version.

### 🐞 Fixes and improvements
- **Heavy stages are much lighter**: stages with several camera screens render each camera once, and large stages draw only the visible cells (e.g. Neon Plaza 45 → 21 ms on an RTX 4070 Ti).
- **Spot shadows go where they matter**: a spot that cannot reach a character no longer takes a shadow map, so stages with many lights keep the shadows of the important ones.
- **Studio fields undo correctly**: a value typed into a key, pose or transform field is one undo step. Camera fields show the track's value, and auto-key is off by default and remembered.
- **Stage image skies load in the Studio**: a sky image path relative to the stage folder was not found.
- **Stage authoring: sky brightness binding**: the `lights.<name>.sky.intensity` target was rejected with an error.
- **Safer updates**: the update feed is signed and the app refuses one that does not verify; optional components are hash-checked too.

### 🙏 Thanks
- The Studio's gravity and wind settings, the field undo fix and more were contributed by quasiquiescent. Thank you!

### 📦 Notes
- Available through the automatic updater (only changed files are downloaded).
- The first start after the update compiles the shaders again and takes longer than usual.

## 日本語
### ✨ 新機能
- **ステージが曲のビートに反応します**: MMDX ステージが照明・マテリアル・装置を曲のビート、小節、音量に合わせて自分で動かせます（stage.json の `drivers`）。ワールドエフェクトとステージスクリーンのパックも同じオーディオ入力を使えます。
- **RT レンダラーでガラスにシーンが映ります**: レイトレーシングでガラス（透過マテリアル）が室内とキャラクターを反射します。夜の窓のように正面から見るガラスにも映り込みが出ます。パストレーシングと GI レンダーは以前から対応しています。
- **濡れた床がキャラクターをきちんと映します**: PBR マテリアルの反射が空ばかりを映し、キャラクターやステージがわずかにしか出ない問題を修正しました（RT 反射とスクリーンスペース反射の両方）。
- **スタジオ: 重力と風の設定**: 再生バーの物理ポップアップで重力の強さ・向きと風（強さ、突風）を変更でき、プロジェクトに保存されます。ビューポート・レンダー・ベイクで突風のタイミングが揃います。
- **スタジオ: オーディオ位置のロック**: タイムラインのオーディオレーンをロックして、音楽の位置を誤って動かさないようにできます。
- **レンダー完了後に PC をシャットダウン**: 動画レンダー画面の「完了後にシステムを終了」をオンにすると、長いレンダーの後に自動で終了します（キャンセルできるカウントダウンが先に出ます）。
- **ナイトリーチャンネル**: 最新の開発ビルドを別に入手できます。安定版とは別のフォルダーに入れて使ってください。

### 🐞 修正と改善
- **重いステージが大幅に軽くなりました**: カメラ画面が複数あるステージは同じカメラを一度だけ描画し、大きなステージは見えている区画だけを描画します（例: ネオンプラザ 45 → 21 ms、RTX 4070 Ti）。
- **スポットライトの影が必要な場所に使われます**: キャラクターに届かないスポットはシャドウマップを使わないため、照明の多いステージでも重要な照明の影が抜けません。
- **スタジオの入力欄の取り消しが正確になりました**: キー・ポーズ・変形の欄に値を直接入力すると、取り消し 1 回で戻ります。カメラの欄はトラックの値を表示し、オートキーは既定でオフ、最後の設定を記憶します。
- **スタジオでステージの画像スカイが読み込まれます**: ステージフォルダー基準のスカイ画像のパスが見つからない問題を修正しました。
- **ステージ制作: 空の明るさのバインド**: `lights.<名前>.sky.intensity` がエラーで拒否される問題を修正しました。
- **アップデートがより安全になりました**: アップデート情報に署名が付き、検証できない場合はアプリが受け取りません。オプションコンポーネントもハッシュを確認します。

### 🙏 貢献
- 重力・風の設定や入力欄の取り消しなど、スタジオの改善は quasiquiescent さんの貢献です。ありがとうございます！

### 📦 お知らせ
- 自動アップデートで入手できます（変更されたファイルのみダウンロード）。
- アップデート直後の初回起動はシェーダーを再コンパイルするため、いつもより時間がかかります。

## 简体中文
### ✨ 新功能
- **舞台会跟随音乐节拍**：MMDX 舞台可以根据歌曲的节拍、小节和音量自行驱动灯光、材质和装置（stage.json 的 `drivers`）。世界特效和舞台屏幕包也可以使用同样的音频输入。
- **RT 渲染器中玻璃会映出场景**：在光线追踪路径下，玻璃（透射材质）会反射室内和角色，包括夜晚落地窗这类正面观看的玻璃。路径追踪和 GI 渲染原本就支持。
- **湿地面能正确映出角色**：PBR 材质的反射主要只映出天空，角色和舞台只有很淡的影子。已修复（光线追踪反射和屏幕空间反射）。
- **工作室：重力与风的设置**：在播放栏的物理弹窗中可以调整重力的强度、方向和风（强度、阵风），并保存到工程中。视口、渲染和烘焙使用相同的阵风时序。
- **工作室：锁定音频位置**：可以锁定时间轴的音频轨，避免误拖动音乐位置。
- **渲染完成后关机**：在视频渲染窗口开启“完成后关闭系统”，长时间渲染结束后会自动关机（会先显示可取消的倒计时）。
- **Nightly 频道**：可以单独获取最新的开发版本。请安装在与正式版不同的文件夹中。

### 🐞 修复与改进
- **大型舞台明显更流畅**：有多个摄像机屏幕的舞台对同一摄像机只渲染一次，大型舞台只绘制可见区域（例如霓虹广场 45 → 21 ms，RTX 4070 Ti）。
- **聚光灯阴影用在需要的地方**：照不到角色的聚光灯不再占用阴影贴图，灯光很多的舞台也不会丢失重要灯光的阴影。
- **工作室输入框的撤销更准确**：在关键帧、姿势、变换输入框中直接输入数值，只需撤销一次即可还原。摄像机输入框显示轨道的值，自动关键帧默认关闭并会记住上次设置。
- **工作室可以加载舞台的图像天空**：修复了找不到相对于舞台文件夹的天空图像路径的问题。
- **舞台制作：天空亮度绑定**：修复了 `lights.<名称>.sky.intensity` 目标被报错拒绝的问题。
- **更新更安全**：更新信息带有签名，验证不通过时应用不会接受；可选组件也会校验哈希。

### 🙏 贡献
- 重力与风的设置、输入框撤销等工作室改进由 quasiquiescent 贡献。感谢！

### 📦 说明
- 可通过自动更新获取（只下载有变化的文件）。
- 更新后的首次启动会重新编译着色器，比平时更久。
