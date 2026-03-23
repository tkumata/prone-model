# 仕様

## 1. 概要

本システムは `Freenove ESP32-S3 WROOM CAM` を用いて、うつ伏せ検知モデル学習用の画像データセットを収集する。

単なる画像保存ではなく、以下を満たすことを仕様とする。

- 被写体単位分割に必要なメタデータを必ず保存する
- 曖昧サンプルを学習対象から外せる
- 画像とメタデータの整合を維持できる
- PC 上で `ESP-DL` 用モデル生成に進める

## 2. ハードウェア仕様

- ボード: `Freenove ESP32-S3 WROOM CAM`
- 保存先: microSD
- 画像形式: JPEG
- 初期フレームサイズ: `320 x 240`
- `board_name` 保存値: `Freenove ESP32-S3 WROOM CAM`

## 3. PC モデル生成前提仕様

- モデル生成は PC 上で行う
- 学習元は `Freenove ESP32-S3 WROOM CAM` で収集した JPEG と `metadata.csv` のみとする
- 学習対象行は `is_usable_for_training=1` かつ `exclude_reason=""` に限定する
- 入力画像は RGB 3 チャンネルへ変換する
- 入力サイズは `96 x 96`
- 出力は 2 クラス分類とし、添字 `0=non_prone`, `1=prone` で固定する
- 学習/検証/評価分割は `subject_id` 単位で固定する
- 分割比率既定値は `train=0.70`, `val=0.15`, `test=0.15`
- 分割乱数シード既定値は `42`
- 初期学習条件は `epoch=20`, `batch_size=32`, `learning_rate=0.001`
- 初期判定閾値は `0.50`
- PC 側では `float` 学習済みモデル、`ONNX`、`ESP-DL` 形式モデルを順に生成可能でなければならない
- 検証データで決めた判定閾値を `.espdl` 評価と実機評価で固定利用する
- `float` モデル、量子化参照、実機結果は同一評価集合で比較可能でなければならない

### 3.1 PC 実行環境仕様

- 想定 OS は `macOS`
- 想定実行系は `python3`
- 必須依存は `torch`, `torchvision`, `pillow`, `onnx`
- 依存不足時は不足パッケージ一覧を表示して終了する

### 3.2 PC 成果物仕様

- 実行ごとに `artifacts/pc_pipeline/<run_name>/` を作成する
- `config.json` に入力引数、前処理条件、分割条件、出力順、閾値を保存する
- `dataset_audit.json` に監査結果を保存する
- `splits/train.csv`, `splits/val.csv`, `splits/test.csv` を保存する
- `checkpoints/best_model.pt` に最良 `float` モデルを保存する
- `onnx/model.onnx` に `ONNX` を保存する
- `reports/metrics.json` に各分割の指標を保存する
- `reports/threshold.json` に固定閾値を保存する
- `espdl/model.espdl` は変換コマンド成功時のみ保存する

### 3.3 PC エラー仕様

- `metadata.csv` 不在時は即座に終了する
- 学習対象行が 0 件なら即座に終了する
- 学習対象 `subject_id` が 3 件未満なら即座に終了する
- 分割結果で `val` または `test` が 0 件なら即座に終了する
- 画像欠損または破損が 1 件でもあれば `dataset_audit.json` に記録し、既定では終了する
- `ESP-DL` 変換コマンド未指定時は `float` モデルと `ONNX` 出力後に正常終了できる

## 4. 実機推論成立仕様

- 推論対象は `Freenove ESP32-S3 WROOM CAM` のライブ入力とする
- 実機入力前処理は PC 学習前処理と同一とする
- 実機出力クラス順は `0=non_prone`, `1=prone`
- 実機推論の受け入れ判定は評価データと別の実機確認用集合でも行う
- 実機で使う閾値は検証データで一度だけ決め、評価集合と実機確認用集合では変更しない
- 実機受け入れ時は PC 量子化参照とのクラス一致率を確認する

## 5. Wi-Fi 仕様

- モード: `STA`
- 接続先: 既存 AP
- 認証情報保存先: `sdkconfig`

使用設定値:

- `CONFIG_WIFI_SSID`
- `CONFIG_WIFI_PASSWORD`

## 6. UI 仕様

### 6.1 `GET /`

返却: HTML

必須 UI:

- カメラストリーミング
- 撮影ボタン
- `うつ伏せ / 非うつ伏せ` ラジオボタン
- エクスポートボタン
- SD カードリセットボタン
- `subject_id` 入力欄
- `session_id` 入力欄
- `location_id` 入力欄
- `lighting_id` 入力欄
- `camera_position_id` 入力欄
- `annotator_id` 入力欄
- 学習利用可否入力
- 除外理由入力
- `notes` 入力欄

## 7. API 仕様

### 7.1 `GET /stream`

目的:

- MJPEG ストリームを返す

レスポンス:

- `200 OK`
- `Content-Type: multipart/x-mixed-replace;boundary=frame`

### 7.2 `GET /api/status`

目的:

- 現在状態と集計を返す

レスポンス例:

```json
{
  "wifi": "connected",
  "camera": "ok",
  "sdcard": "ok",
  "sample_count": 240,
  "usable_count": 210,
  "excluded_count": 30,
  "last_capture_ms": 1710000000000
}
```

### 7.3 `POST /api/capture`

目的:

- 画像と必須メタデータを保存する

リクエスト:

```json
{
  "subject_id": "baby_001",
  "session_id": "baby_001_20260315_am",
  "location_id": "loc_01",
  "lighting_id": "light_day",
  "camera_position_id": "campos_top",
  "annotator_id": "ann_a01",
  "label": 1,
  "is_usable_for_training": 1,
  "exclude_reason": "",
  "notes": ""
}
```

必須制約:

- `subject_id` は必須
- `session_id` は必須
- `location_id` は必須
- `lighting_id` は必須
- `camera_position_id` は必須
- `annotator_id` は必須
- `label` は `0` または `1`
- `is_usable_for_training` は `0` または `1`
- `is_usable_for_training = 1` の場合、`exclude_reason` は空
- `is_usable_for_training = 0` の場合、`exclude_reason` は必須

成功レスポンス例:

```json
{
  "saved": true,
  "capture_id": "1710000000000",
  "image_path": "images/1710000000000.jpg"
}
```

### 7.4 `GET /api/export/manifest`

目的:

- エクスポート対象一覧をページ単位で返す

クエリ:

- `page`
- `page_size`

制約:

- `page_size` の既定値は `50`
- `page_size` の最大値は `100`

レスポンス例:

```json
{
  "total_samples": 240,
  "page": 1,
  "page_size": 50,
  "has_next": true,
  "items": [
    {
      "capture_id": "1710000000000",
      "image_path": "images/1710000000000.jpg",
      "timestamp_ms": 1710000000000,
      "label": 1
    }
  ]
}
```

### 7.5 `GET /api/export/metadata`

目的:

- `metadata.csv` を単独で返す

レスポンス:

- `200 OK`
- `Content-Type: text/csv`

### 7.6 `GET /api/export/image`

目的:

- 指定した 1 画像を返す

クエリ:

- `capture_id`

レスポンス:

- `200 OK`
- `Content-Type: image/jpeg`

失敗条件:

- 該当画像なし: `404 Not Found`

### 7.7 `POST /api/reset`

目的:

- データセット全体を削除する

リクエスト:

```json
{
  "confirm": "RESET"
}
```

## 8. 保存仕様

### 8.1 保存先

- 画像: `dataset/images/<capture_id>.jpg`
- メタデータ: `dataset/metadata.csv`

### 8.2 `metadata.csv` 列

以下を必須列とする。

- `capture_id`
- `timestamp_ms`
- `subject_id`
- `session_id`
- `location_id`
- `lighting_id`
- `camera_position_id`
- `annotator_id`
- `label`
- `label_name`
- `is_usable_for_training`
- `exclude_reason`
- `notes`
- `image_path`
- `image_bytes`
- `frame_width`
- `frame_height`
- `pixel_format`
- `jpeg_quality`
- `board_name`

### 8.3 列定義

- `capture_id`
  一意な撮影 ID
- `timestamp_ms`
  保存時刻
- `subject_id`
  被写体識別子
- `session_id`
  連続収集識別子
- `location_id`
  撮影場所識別子
- `lighting_id`
  照明条件識別子
- `camera_position_id`
  カメラ設置条件識別子
- `annotator_id`
  注釈担当者識別子
- `label`
  `1=prone`, `0=non_prone`
- `label_name`
  可読名
- `is_usable_for_training`
  学習利用可否
- `exclude_reason`
  学習除外理由
- `notes`
  自由記述メモ
- `image_path`
  相対画像パス
- `image_bytes`
  JPEG サイズ
- `frame_width`
  画像幅
- `frame_height`
  画像高
- `pixel_format`
  画像形式
- `jpeg_quality`
  JPEG 品質値
- `board_name`
  取得ハード。値は `Freenove ESP32-S3 WROOM CAM` 固定

### 8.4 後方互換仕様

- 現行の20列を `metadata.csv` 版 `1.0` とする
- `1.x` 系では既存列の削除、改名、順序変更、意味変更を禁止する
- 列追加は末尾追加のみ許可する
- PC 側処理は既知の必須列がそろっていれば追加列を無視してよい
- `major` 更新を伴う非互換変更時は旧版入力受け入れを停止する

## 9. 語彙仕様

### 9.1 `label_name`

- `1 -> prone`
- `0 -> non_prone`

### 9.2 `exclude_reason`

許可値:

- `ambiguous_pose`
- `face_not_visible`
- `subject_missing`
- `multiple_subjects`
- `motion_blur`
- `poor_lighting`
- `annotation_error`
- `other`

## 10. 保存ロジック仕様

保存成功条件:

1. 入力メタデータがすべて妥当
2. JPEG 保存成功
3. `metadata.csv` 追記成功

失敗時:

- JPEG 保存失敗なら CSV を追記しない
- CSV 追記失敗なら JPEG を削除する

## 11. エクスポートロジック仕様

- `metadata.csv` は 1 リクエストで返す
- 画像一覧は `/api/export/manifest` でページ単位に返す
- 画像本体は `/api/export/image?capture_id=...` で 1 件ずつ返す
- サーバ側で巨大アーカイブを生成しない
- 通信断後は未取得画像だけ再取得できる
- 画像順序は `timestamp_ms` 昇順を基本とする
- PC 側で `subject_id` 単位分割と学習前処理を再現できる列を欠落させない

## 12. 分割仕様

- 学習/検証/評価分割は `subject_id` 単位
- `subject_id` 欠損サンプルは学習対象外
- `is_usable_for_training=0` は学習対象外
- `exclude_reason != ""` は監査対象として保持する
- `board_name != "Freenove ESP32-S3 WROOM CAM"` は本モデル生成対象外
- 閾値調整に使う集合と最終評価集合を分離する
- 実機確認用集合は学習/検証/評価のいずれとも分離管理する
- 実機確認用集合の `session_id` は台帳上 `dataset_role=device_validation` で固定する

## 13. PC 前処理仕様

### 13.1 学習対象フィルタ

- `label` は `0` または `1`
- `is_usable_for_training=1`
- `exclude_reason` は空文字
- `subject_id` 欠損は除外
- `image_path` 欠損は除外
- `board_name` は `Freenove ESP32-S3 WROOM CAM`

### 13.2 画像前処理

- 読み込み元は JPEG
- 色空間は RGB
- 出力サイズは `96 x 96`
- 画素値スケール方法は学習設定として保存し、量子化時も同一条件を使う
- 前処理条件は評価結果と一緒に保存する
- チャンネル順は `RGB`
- 画素並び順はモデル入力定義と一致させる
- 画像回転方向は収集ボードの実画像向きに合わせて固定する
- 学習時と実機時で別の補間方法を使ってはならない

### 13.3 学習利用可画像の最低条件

- 被写体の体幹が視認できる
- 被写体主要部位が画面外へ大きく欠けていない
- 学習利用可画像では被写体短辺サイズが画像短辺の 30% 以上
- 姿勢判定に必要な輪郭がブレや露光失敗で失われていない

### 13.4 学習分割成果物

- `train`
- `val`
- `test`

各成果物は少なくとも以下を持つ。

- `image_path`
- `label`
- `subject_id`
- `session_id`
- `board_name`

### 13.5 モデル成果物

- 学習済み `float` モデル
- `ONNX`
- `ESP-DL` 形式モデル
- 分割定義
- 評価結果
- 量子化設定
- 生成元データ識別情報
- 固定済み閾値
- PC 量子化参照の推論結果

### 13.6 モデル合格基準

- `float` モデルの評価集合における `prone` 再現率は `0.92` 以上
- `float` モデルの評価集合における `prone` 適合率は `0.90` 以上
- `float` モデルの評価集合における全体正解率は `0.90` 以上
- 量子化後評価で全体正解率低下は `0.03` 以内
- 量子化後評価で `prone` 再現率低下は `0.03` 以内
- PC 量子化参照と `.espdl` 実機推論のクラス一致率は `0.98` 以上

## 14. 実機検証仕様

### 14.1 入力

- `Freenove ESP32-S3 WROOM CAM` のライブフレーム
- 実機確認用集合に含まれる画像または再現可能な姿勢シーン

### 14.2 記録項目

- 推論時刻
- 入力識別子
- 予測クラス
- `prone` スコア
- 判定閾値
- 正解ラベル
- PC 側量子化参照クラス

### 14.3 実機合格基準

- 連続 300 フレーム相当の試験で異常終了しない
- 実機確認用集合で `prone` 再現率 `0.90` 以上
- 実機確認用集合で `non_prone` 適合率 `0.90` 以上
- PC 量子化参照との差分原因を説明できない不一致が 2% を超えない
- PC 側量子化参照とのクラス一致率 `0.98` 以上

## 15. エラー仕様

- 必須項目欠損: `400 Bad Request`
- 不正ラベル: `400 Bad Request`
- 不正除外理由: `400 Bad Request`
- 不正な `location_id`, `lighting_id`, `camera_position_id`, `annotator_id`: `400 Bad Request`
- カメラ異常: `500` または `503`
- SD カード異常: `500`
- エクスポート対象画像なし: `404 Not Found`
- 確認トークン不一致: `409 Conflict`
- PC 側で必須列不足が見つかった場合はモデル生成を開始してはならない
- PC 側で `subject_id` 重複跨ぎ分割が検出された場合はその分割を無効とする
- `float` モデルが合格基準未達の場合は量子化工程へ進めない
- 量子化後モデルが合格基準未達の場合は `.espdl` 実機受け入れへ進めない
- 実機検証で合格基準未達の場合はモデル採用を禁止する

## 16. 成立条件

本仕様は、以下を満たすときに学習データ仕様として成立する。

- `subject_id` 単位で分割できる
- 曖昧サンプルを自動除外できる
- 人手で画像監査できる
- 画像とメタデータの対応が壊れない
- 分割エクスポートを途中再試行しても全件取得できる
- PC 上で同一前処理条件から `float` モデル、`ONNX`、`ESP-DL` 形式モデルを再生成できる
- 実機確認用集合で `Freenove ESP32-S3 WROOM CAM` 上の `prone / non_prone` 検知が合格基準を満たす
