# prone-model

`Freenove ESP32-S3 WROOM CAM` を使い、乳幼児の `うつ伏せ / 非うつ伏せ` 学習用写真データを収集するための ESP-IDF プロジェクトです。

既存 Wi-Fi AP に `STA` 接続し、ESP32 上で Web サーバを起動します。ブラウザからライブストリームを確認し、ラベルを選んで撮影し、SD カードへ保存します。保存済みデータは PC へ分割エクスポートでき、必要に応じて SD カード内データを全削除できます。

## 現在のスコープ

- `Freenove ESP32-S3 WROOM CAM` でカメラ映像を取得する
- `sdkconfig` に保存した `SSID` / `PASSWORD` で既存 AP に接続する
- Web UI でストリーミング映像を表示する
- Web UI で `うつ伏せ / 非うつ伏せ` を選択して撮影する
- JPEG 画像と学習用メタデータを SD カードへ保存する
- 保存済みデータを PC 側へ分割ダウンロードする
- SD カード内のデータセットを初期化する

## Web UI

ルート画面 `/` には以下を配置します。

- カメラストリーミング
- 撮影ボタン
- `うつ伏せ / 非うつ伏せ` ラジオボタン
- `エクスポート` ボタン
- `SDカードリセット` ボタン
- `subject_id` 入力欄
- `session_id` 入力欄
- `location_id` 入力欄
- `lighting_id` 入力欄
- `camera_position_id` 入力欄
- `annotator_id` 入力欄
- `学習利用可否` 入力欄
- `除外理由` 入力欄
- `notes` 入力欄

## 保存形式

SD カード上には `dataset/` ディレクトリを作成し、以下を保存します。

- `dataset/images/*.jpg`
  撮影した JPEG 画像
- `dataset/metadata.csv`
  全画像に対するメタデータ一覧

### `metadata.csv` の基本列

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

## 学習データとして追加で残すべき情報

画像だけでは後で偏りや品質問題を潰しにくいので、以下を必須で残します。

- `subject_id`
  被写体単位分割を行うための最重要キー
- `session_id`
  同一環境・同一時間帯・同一被写体群を識別する
- `location_id`
  撮影場所の偏りを監査する
- `lighting_id`
  照明条件の偏りを監査する
- `camera_position_id`
  画角と設置位置の偏りを監査する
- `annotator_id`
  注釈担当差分を監査する
- `timestamp_ms`
  時系列の偏り、連写由来のリーク確認に使う
- `is_usable_for_training`
  曖昧サンプルを学習集合から外す
- `exclude_reason`
  学習利用不可理由を保存する
- `notes`
  後から判断保留や注意点を監査する
- `frame_width` / `frame_height`
  前処理条件を再現する
- `jpeg_quality`
  圧縮率差異を追跡する
- `board_name`
  取得元ハードを識別する
- `image_bytes`
  異常に小さい破損画像や露光失敗を検知する

## エクスポート

エクスポートは巨大アーカイブを 1 回で返さず、以下に分割します。

- `/api/export/metadata`
  `metadata.csv` を取得する
- `/api/export/manifest`
  画像一覧をページ単位で取得する
- `/api/export/image?capture_id=...`
  画像を 1 件ずつ取得する

通信断があっても、未取得画像だけ再試行できます。

## 設定

Wi-Fi 認証情報は `sdkconfig` に保持します。

- `CONFIG_WIFI_SSID`
- `CONFIG_WIFI_PASSWORD`

リポジトリへ実値をコミットしない前提です。

## ドキュメント方針

本リポジトリでは、実装より先に学習データ仕様を閉じます。特に以下を絶対要件とします。

- `subject_id` 必須
- 画像本体保存必須
- `is_usable_for_training` と `exclude_reason` による曖昧サンプル除外
- `subject_id` 単位分割

## ビルド

```bash
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py -p <PORT> flash monitor
```

## ドキュメント

- [docs/REQUIREMENTS.md](/Users/kumata/Developer/prone-model/docs/REQUIREMENTS.md)
- [docs/DESIGN.md](/Users/kumata/Developer/prone-model/docs/DESIGN.md)
- [docs/SPECIFICATIONS.md](/Users/kumata/Developer/prone-model/docs/SPECIFICATIONS.md)
- [docs/COLLECTION_POLICY.md](/Users/kumata/Developer/prone-model/docs/COLLECTION_POLICY.md)
- [docs/LABELING_GUIDELINES.md](/Users/kumata/Developer/prone-model/docs/LABELING_GUIDELINES.md)
- [docs/DATA_QUALITY.md](/Users/kumata/Developer/prone-model/docs/DATA_QUALITY.md)
- [docs/NAMING_RULES.md](/Users/kumata/Developer/prone-model/docs/NAMING_RULES.md)
- [docs/ACCEPTANCE_CRITERIA.md](/Users/kumata/Developer/prone-model/docs/ACCEPTANCE_CRITERIA.md)
- [docs/TODO.md](/Users/kumata/Developer/prone-model/docs/TODO.md)
