# 設計

## 1. 設計方針

本プロジェクトは「収集装置」ではあるが、単なる画像保存機ではない。設計上の主目的は、`うつ伏せ検知モデルの学習データを確実に作ること` である。

そのため、設計上の最重要点は次の 5 つとする。

- 画像本体を必ず保存する
- 被写体リーク防止のため `subject_id` を必須入力にする
- 曖昧サンプルを負例へ混入させないため `is_usable_for_training` と `exclude_reason` を持つ
- 環境偏り監査のため `location_id`, `lighting_id`, `camera_position_id`, `annotator_id` を必須入力にする
- 画像とメタデータの整合を壊さずエクスポートできる

加えて、本設計は `Freenove ESP32-S3 WROOM CAM` で収集した画像を PC 上へ持ち出し、`ESP-DL` 用モデル生成まで再現できることを前提にする。
さらに、最終成果物は PC 上の学習済みモデルではなく、同ボード上でライブカメラ入力に対して安定動作する `.espdl` 推論系とする。

## 2. 論理構成

### 2.1 `wifi_service`

- `sdkconfig` から `SSID` / `PASSWORD` を読む
- `STA` モードで既存 AP に接続する
- 切断時は自動再接続する

### 2.2 `camera_service`

- `Freenove ESP32-S3 WROOM CAM` のピン設定で初期化する
- JPEG フレームを取得する
- ストリーミングと静止画保存で同じカメラを利用する

### 2.3 `annotation_service`

- Web UI から `subject_id`, `session_id`, `location_id`, `lighting_id`, `camera_position_id`, `annotator_id`, `label`, `is_usable_for_training`, `exclude_reason`, `notes` を受け取る
- 必須項目の妥当性を検証する
- 学習利用不可サンプルが誤って通常ラベルに混ざらないよう整形する

### 2.4 `storage_service`

- SD カードをマウントする
- `dataset/images/` に JPEG を保存する
- `dataset/metadata.csv` にメタデータを追記する
- 画像保存成功後のみ CSV を追記する
- CSV 追記失敗時は画像をロールバックする
- 保存、エクスポート、リセットの排他制御を持つ
- エクスポート用に画像一覧をページ単位で列挙できる

### 2.5 `web_service`

- `/`
- `/stream`
- `/api/status`
- `/api/capture`
- `/api/export/manifest`
- `/api/export/metadata`
- `/api/export/image`
- `/api/reset`
  を提供する

### 2.6 `pc_pipeline`

- `Freenove ESP32-S3 WROOM CAM` 由来の `metadata.csv` と JPEG を取り込む
- `is_usable_for_training=1` かつ `exclude_reason=""` の行だけを学習候補にする
- `subject_id` 単位で `train / val / test` に分割する
- JPEG を RGB 化し、`96 x 96` へ正規化した学習入力を作る
- PC 上で学習済み `float` モデルを生成する
- 学習済みモデルを `ONNX` へ変換し、さらに `ESP-DL` 用成果物へ変換する
- 分割定義、評価結果、変換済みモデルを保存し、同条件で再生成できるようにする

### 2.7 `inference_service`

- `.espdl` を `Freenove ESP32-S3 WROOM CAM` 上で読み込む
- ライブフレームに PC 側と同一前処理を適用する
- `non_prone`, `prone` の 2 クラス出力を返す
- 検証時は PC 側量子化参照と一致比較できるログを出す

## 3. データモデル設計

### 3.1 ディレクトリ構成

```text
/sdcard/
  dataset/
    metadata.csv
    images/
      <capture_id>.jpg
```

### 3.2 1 レコードの責務

`metadata.csv` の 1 行は 1 枚の画像に対応する。

1 レコードが持つべき意味は以下とする。

- 何を撮ったか
- 誰を撮ったか
- どの収集単位で撮ったか
- どの環境で撮ったか
- 誰が注釈したか
- その画像を学習に使ってよいか
- 使えないならなぜ使えないか
- PC 側の `ESP-DL` モデル生成へ採用してよいか

## 4. ID 設計

### 4.1 `capture_id`

- 1 撮影ごとに一意
- 画像ファイル名と対応する

### 4.2 `subject_id`

- 被写体単位 ID
- 学習/検証/評価分割の最小単位
- 欠損を許可しない

### 4.3 `session_id`

- 同一被写体、同一撮影環境、同一時間帯の連続収集単位
- 1 つの `session_id` に異なる `subject_id` を混在させない

### 4.4 `board_name`

- 取得元ハード識別子
- 本プロジェクトでは `Freenove ESP32-S3 WROOM CAM` 固定とする
- PC 側で学習対象を他ボード収集データと混在させないために使う

## 5. 保存シーケンス

1. オペレータが `subject_id`, `session_id`, `label`, `is_usable_for_training`, `exclude_reason` を入力する
2. オペレータが `location_id`, `lighting_id`, `camera_position_id`, `annotator_id`, `notes` を入力する
3. `annotation_service` が入力妥当性を検証する
4. `camera_service` が現在フレームを JPEG として取得する
5. `storage_service` が `dataset/images/<capture_id>.jpg` を保存する
6. 画像保存成功後に `dataset/metadata.csv` へ 1 行追記する
7. CSV 追記に失敗した場合は画像を削除して失敗を返す

保存時点で、後段で必要になる次の再現情報が欠けてはならない。

- 元画像サイズ
- JPEG 圧縮設定
- 取得ボード識別子
- 収集日時
- 被写体識別子

## 6. 曖昧サンプル混入防止設計

姿勢ラベルと学習利用可否を分離する。

- `label`
  姿勢ラベル
- `is_usable_for_training`
  学習使用可否
- `exclude_reason`
  使用不可理由

これにより以下を防ぐ。

- 横向きや途中姿勢を `non_prone` に押し込む事故
- 被写体不在画像を負例として学習させる事故
- ブレ画像を正常サンプルとして混入させる事故

## 7. 分割リーク防止設計

データ分割は `subject_id` 単位で行うことを前提にする。

- `subject_id` がないサンプルは学習母集団に入れない
- `session_id` は時系列監査やセッション偏り分析に使う
- `location_id`, `lighting_id`, `camera_position_id` は環境偏り監査に使う
- `timestamp_ms` は連写偏りや時系列リーク検査に使う

## 8. エクスポート設計

エクスポートは「ESP32 側で途中エラーを起こしにくく、PC 側で再開可能に取り切れること」を目的とする。

含めるもの:

- `metadata.csv`
- `images/` 配下の全 JPEG

要件:

- `metadata.csv` は単独 API で取得する
- 画像一覧はマニフェスト API でページ単位に取得する
- 画像本体は 1 画像 1 リクエストで取得する
- 画像取得は `capture_id` 指定で再試行可能とする
- 展開後に `image_path` から画像へ到達できる
- CSV だけでもサンプル品質確認と分割判定ができる
- 画像を見ればラベル監査ができる
- 単一レスポンスで全画像を束ねる設計は禁止する

## 9. PC モデル生成設計

PC 側は次の段階で処理する。

1. 生データ監査
2. 学習対象フィルタ
3. `subject_id` 単位分割
4. 画像前処理固定化
5. `float` モデル学習
6. `ONNX` 変換
7. `ESP-DL` 用変換
8. 量子化後評価
9. 実機搭載前一致確認
10. 実機受け入れ試験

この段階で固定する事項は以下とする。

- 入力画像は RGB 3 チャンネル
- 入力サイズは `96 x 96`
- 出力は 2 要素で、順序は `non_prone`, `prone`
- 分割単位は `subject_id`
- 量子化後モデルは元の分割定義と評価結果に紐付ける
- 判定閾値は検証データで決めて固定する
- 実機前処理は PC 側評価コードと同一にする

PC 側成果物として最低でも以下を残す。

- 分割済み一覧
- 前処理条件
- 学習済み `float` モデル
- `ONNX`
- `ESP-DL` 形式モデル
- 評価指標
- 生成日時と元データ識別情報
- 固定済み判定閾値
- PC 量子化参照出力

## 10. 実機推論設計

実機推論では次を満たす。

- カメラ入力元は `Freenove ESP32-S3 WROOM CAM` のライブフレーム
- リサイズ、色変換、正規化、テンソル並び順は PC 側と一致
- 出力順は `non_prone`, `prone`
- 実機検証では同一入力に対する PC 側量子化参照との差分を確認する
- 判定閾値は検証データで決めた固定値を使う

実機受け入れで必要な確認項目は以下とする。

- モデル読み込み成功
- 連続推論中に停止しない
- `prone` の見逃しが許容値以内
- `non_prone` の誤検知が許容値以内
- 推論ログと正解ラベルを突合できる

## 11. リセット設計

- 対象は `dataset/images/` と `dataset/metadata.csv`
- 実行前に確認操作を必須とする
- 実行後はヘッダ付き空 CSV を再生成する

## 12. ステータス設計

`/api/status` では少なくとも以下を返す。

- Wi-Fi 状態
- カメラ状態
- SD カード状態
- 収集済みサンプル件数
- 学習利用可サンプル件数
- 学習除外サンプル件数
- 直近保存時刻

## 13. 運用設計

学習データを成立させるため、運用ルールも設計対象とする。

- `subject_id` 命名規則を固定する
- `session_id` 命名規則を固定する
- `location_id`, `lighting_id`, `camera_position_id`, `annotator_id` の命名規則を固定する
- `exclude_reason` の語彙を固定する
- 曖昧なら保存しない、または `is_usable_for_training=0` で保存する
- PC 側へエクスポートした後も、元の `metadata.csv` と画像一式は再現用に保持する
- `board_name=Freenove ESP32-S3 WROOM CAM` 以外の行は本モデル生成フローへ入れない
- 実機受け入れ試験に使う評価動画または評価画像集合は学習集合と分離する
- 閾値変更を行った場合は再度 PC 評価と実機評価をやり直す

## 14. この設計で担保すること

この設計は、次を保証するためのものとする。

- 画像監査可能
- 被写体リーク防止可能
- 曖昧サンプル除外可能
- 収集後に学習用フィルタを機械的に適用可能
- PC 上で `ONNX` および `ESP-DL` モデル生成条件を再現可能
- 最終的に `Freenove ESP32-S3 WROOM CAM` 上での検知成立条件を監査可能
