MMDX12 library
==============

[English]
Put your assets in these folders (MMDX12 ships no assets; follow each asset's terms of use):
  characters/  one folder per model with its textures. PMX, VRM, glTF/GLB, FBX
               (humanoid rigs: MMD, VRoid/VRM, Mixamo, Ready Player Me, Unity/Blender/UE rigs).
  stages/      one folder per stage. All PMX files in a folder are drawn together;
               every glTF/GLB/FBX/OBJ file is a stage of its own.
  songs/       one folder per song: dance .vmd, camera .vmd, audio (.wav/.mp3/.ogg/.flac).
You may use any layout you like. Folder names decide the type (characters/models/..., stages/...,
songs/motions/...); without them MMDX12 guesses from the content. Several songs in one folder are
told apart by length and file names.

To decide yourself, put an mmdx.json in a folder (paths are relative to that folder):
  {"type": "character", "model": "body.fbx", "name": "My avatar"}
  {"type": "stage", "parts": ["stage.pmx", "sky.pmx"]}
  {"type": "song", "dance": "motion.vmd", "camera": "cam.vmd", "audio": "song.mp3", "extra": ["face.vmd"]}
  {"type": "ignore"}                       (hide this folder)
  [ {...}, {...} ]                         (several entries in one file)
A type without files ({"type": "stage"}) applies to everything below that folder.
In the app, right-click a card to use it as a character or a stage, or to hide it.

[한국어]
이 폴더들에 에셋을 넣으세요 (MMDX12에는 에셋이 없습니다. 에셋마다 이용 규약을 지켜 주세요).
  characters/  모델마다 폴더 하나 (텍스처 포함). PMX, VRM, glTF/GLB, FBX 휴머노이드 모델.
  stages/      스테이지마다 폴더 하나. 폴더 안의 PMX는 함께 그려지고, glTF/FBX/OBJ 파일은 각각 하나의 스테이지입니다.
  songs/       곡마다 폴더 하나: 댄스 VMD, 카메라 VMD, 음원(.wav/.mp3/.ogg/.flac).
폴더 구조는 자유입니다. 폴더 이름(characters/models, stages, songs/motions 등)으로 종류를 정하고,
없으면 내용으로 추측합니다. 직접 정하려면 폴더에 mmdx.json을 두세요 (위 영어 예시 참고).
앱에서 카드를 우클릭하면 캐릭터/스테이지로 쓰거나 숨길 수 있습니다.

[日本語]
このフォルダに素材を入れてください (MMDX12に素材は含まれません。各素材の利用規約を守ってください)。
  characters/  モデルごとにフォルダ一つ (テクスチャ込み)。PMX / VRM / glTF / FBX の人型モデル。
  stages/      ステージごとにフォルダ一つ。フォルダ内のPMXは一緒に描画、glTF/FBX/OBJは1ファイル1ステージ。
  songs/       曲ごとにフォルダ一つ: ダンスVMD・カメラVMD・音源。
フォルダ構成は自由です。フォルダ名で種類が決まり、なければ内容から推測します。
自分で決めるときはフォルダに mmdx.json を置いてください (上の英語の例を参照)。
アプリでカードを右クリックすると、キャラクター/ステージとして使う・非表示にするを選べます。

[简体中文]
请把素材放进这些文件夹 (MMDX12 不附带素材，请遵守各素材的使用规约)：
  characters/ 角色模型 (PMX / VRM / glTF / FBX 人形模型)，stages/ 场景，songs/ 每首歌一个文件夹 (舞蹈VMD、镜头VMD、音频)。
文件夹结构自由；也可以在文件夹里放 mmdx.json 自行指定 (见上方英文示例)。
