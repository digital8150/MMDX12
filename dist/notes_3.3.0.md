# MMDX12 3.3.0 — MikuMikuMoving Motions, Steadier RT AO and Studio Render Fix

> 🌐 **Language:** [한국어](#한국어) · [English](#english) · [日本語](#日本語) · [简体中文](#简体中文)

## 한국어
### ✨ 새 기능
- **MikuMikuMoving 모션(.mvd)을 불러올 수 있어요**: 라이브러리, 드래그 앤 드롭, 스튜디오의 모션 불러오기에서 VMD처럼 .mvd 파일을 쓸 수 있어요. 본·표정·카메라·IK·조명 키를 읽어요(첫 번째 레이어). 게임에서 변환한 댄스처럼 IK 키 없이 다리를 FK로만 움직이는 모션은 다리 IK를 자동으로 꺼서 다리가 꼬이지 않아요.

### 🐞 수정 및 개선
- **스튜디오 영상 렌더가 뷰포트와 같은 효과로 나와요(#12)**: 스튜디오에서 영상을 렌더하면 효과 패널 설정 대신 로비 렌더 창의 효과 설정을 써서, 뷰포트보다 어둡게(그림자·앰비언트 오클루전이 항상 켜짐) 나오거나 피사계 심도가 빠졌어요. 이제 스튜디오의 모든 렌더(영상·스틸, 모든 렌더러)가 효과 패널 설정을 그대로 따르고, 스튜디오에서 연 렌더 창에는 효과 스위치 대신 "효과 패널 열기" 버튼이 보여요.
- **RT 앰비언트 오클루전이 움직일 때도 안정적이에요**: 광선 분포를 고르게 바꾸고 이전 프레임을 더 정확하게 이어 써서, 카메라나 캐릭터가 움직일 때의 얼룩과 깜빡임이 줄었어요. 영상 렌더는 화질에 따라 광선을 더 많이 써요(8~48개). 로비의 RT 설정에서 광선 수(4 / 8 / 16)를 고를 수 있어요.
- **스테이지 스크린이 영상에서 어두워지지 않아요**: 멀리 있는 스크린이 앰비언트 오클루전의 잡음 때문에 영상 렌더에서 통째로 어두워지던 문제를 고쳤어요.
- **스튜디오 카메라의 화각이 소수점까지 저장돼요**: 게임에서 추출한 카메라처럼 화각이 조금씩 바뀌는 카메라가 1도 단위 계단처럼 움직였어요. 이제 정확한 값을 프로젝트에 저장해요(VMD로 내보낼 때는 MMD 규격대로 정수).

### 🙏 기여
- 스튜디오 렌더 문제는 zhangyan453 님의 자세한 보고(#12) 덕분에 찾을 수 있었어요. 감사합니다!

### 📦 안내
- 자동 업데이트로 받을 수 있어요(변경된 파일만 내려받아요).
- 업데이트 직후 첫 실행은 셰이더를 다시 컴파일해서 평소보다 오래 걸려요.

## English
### ✨ What's New
- **MikuMikuMoving motions (.mvd)**: .mvd files now work like VMDs in the library, drag and drop, and the Studio's motion import. Bone, morph, camera, IK and light keys are read (first layer). Dances that move the legs with FK only and carry no IK keys, as many game-converted dances do, get their leg IK switched off automatically, so the legs no longer twist.

### 🐞 Fixes and improvements
- **Studio video renders match the viewport (#12)**: a Studio video took the lobby render dialog's effect settings instead of the effects panel, so it came out darker than the viewport (shadows and ambient occlusion were always on) or lost its depth of field. Every Studio render (videos and stills, every renderer) now follows the effects panel, and the render dialog opened from the Studio shows an "Open effects panel" button instead of its own effect switches.
- **Steadier ray-traced ambient occlusion in motion**: better distributed rays and a more accurate reuse of the previous frames reduce the blotches and flicker when the camera or the characters move. Video renders use more rays depending on the quality (8 to 48), and the lobby's RT settings let you pick the ray count (4 / 8 / 16).
- **Stage screens no longer go dark in videos**: distant screens were dimmed as a whole by ambient-occlusion noise in video renders.
- **Studio camera field of view keeps its fraction**: cameras whose field of view changes slowly, such as game-extracted ones, moved in 1-degree steps. The exact value is now saved in the project (VMD export still rounds it, as the MMD format requires).

### 🙏 Thanks
- The Studio render issue was found thanks to zhangyan453's detailed report (#12). Thank you!

### 📦 Notes
- Available through the automatic updater (only changed files are downloaded).
- The first start after the update compiles the shaders again and takes longer than usual.

## 日本語
### ✨ 新機能
- **MikuMikuMoving のモーション (.mvd) を読み込めます**: ライブラリ、ドラッグ＆ドロップ、スタジオのモーション読み込みで、.mvd ファイルを VMD と同じように使えます。ボーン・表情・カメラ・IK・照明のキーを読み込みます（最初のレイヤー）。ゲームから変換したダンスのように IK キーがなく足を FK だけで動かすモーションは、足 IK を自動でオフにするので足がねじれません。

### 🐞 修正と改善
- **スタジオの動画レンダーがビューポートと同じエフェクトになります (#12)**: スタジオで動画をレンダーすると、エフェクトパネルではなくロビーのレンダー画面のエフェクト設定が使われ、ビューポートより暗く（影とアンビエントオクルージョンが常にオン）なったり、被写界深度が抜けたりしていました。スタジオのすべてのレンダー（動画・静止画、すべてのレンダラー）がエフェクトパネルの設定に従い、スタジオから開いたレンダー画面には独自のエフェクトスイッチの代わりに「エフェクトパネルを開く」ボタンが表示されます。
- **RT アンビエントオクルージョンが動きの中でも安定しました**: レイの分布を均一にし、前のフレームをより正確に引き継ぐことで、カメラやキャラクターが動くときのムラやちらつきが減りました。動画レンダーは画質に応じてより多くのレイ（8〜48）を使い、ロビーの RT 設定でレイ数（4 / 8 / 16）を選べます。
- **ステージのスクリーンが動画で暗くなりません**: 遠くのスクリーンが動画レンダーでアンビエントオクルージョンのノイズにより丸ごと暗くなる問題を修正しました。
- **スタジオのカメラの画角が小数まで保存されます**: ゲームから抽出したカメラのように画角が少しずつ変わるカメラが 1 度刻みの階段状に動いていました。正確な値をプロジェクトに保存します（VMD への書き出しは MMD の仕様どおり整数）。

### 🙏 貢献
- スタジオのレンダーの問題は zhangyan453 さんの詳しい報告（#12）のおかげで見つけられました。ありがとうございます！

### 📦 お知らせ
- 自動アップデートで入手できます（変更されたファイルのみダウンロード）。
- アップデート直後の初回起動はシェーダーを再コンパイルするため、いつもより時間がかかります。

## 简体中文
### ✨ 新功能
- **可以导入 MikuMikuMoving 动作 (.mvd)**：在资源库、拖放和工作室的动作导入中，.mvd 文件可以像 VMD 一样使用。会读取骨骼、表情、镜头、IK 和灯光关键帧（第一层）。像从游戏转换的舞蹈那样没有 IK 关键帧、只用 FK 移动腿部的动作，会自动关闭腿部 IK，腿不再扭曲。

### 🐞 修复与改进
- **工作室视频渲染与视口效果一致 (#12)**：在工作室渲染视频时，使用的是大厅渲染窗口的效果设置而不是效果面板，因此画面比视口更暗（阴影和环境光遮蔽始终开启），或者丢失景深。现在工作室的所有渲染（视频和静帧、所有渲染器）都沿用效果面板的设置，从工作室打开的渲染窗口不再显示单独的效果开关，而是显示“打开效果面板”按钮。
- **RT 环境光遮蔽在运动中更稳定**：光线分布更均匀，并更准确地复用之前的帧，镜头或角色移动时的斑块和闪烁减少了。视频渲染会根据画质使用更多光线（8 到 48 条），大厅的 RT 设置中可以选择光线数（4 / 8 / 16）。
- **舞台屏幕在视频中不再变暗**：修复了远处的屏幕在视频渲染中因环境光遮蔽噪点而整体变暗的问题。
- **工作室镜头的视角保留小数**：像从游戏提取的镜头那样视角缓慢变化的镜头，过去会以 1 度为单位阶梯式变化。现在精确值会保存在项目中（导出 VMD 时按 MMD 规格取整）。

### 🙏 贡献
- 工作室渲染问题多亏了 zhangyan453 的详细报告（#12）才得以发现。谢谢！

### 📦 说明
- 可通过自动更新获取（只下载变更的文件）。
- 更新后的首次启动会重新编译着色器，耗时比平时长。
