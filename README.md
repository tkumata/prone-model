# prone-model

乳幼児のうつ伏せ状態を、顔検出から得られる数値情報だけで判定するためのプロジェクトです。
最終的には、PC 側で学習したモデルを `ESP-DL` で扱える `.espdl` 形式へ変換し、`Freenove ESP32-S3 WROOM CAM` 上で利用することを目標にしています。

現時点では、マイコン上でカメラ映像を取得し、顔検出と顔特徴点の抽出を行い、Web 画面からラベル付きで顔情報を CSV に収集できる段階まで進んでいます。

## 目的

- 乳幼児のうつ伏せ状態を判定する軽量モデルを作る
- 入力は画像そのものではなく、顔検出から得られる数値情報に限定する
- 学習済みモデルを `.espdl` 形式へ変換し、マイコンで扱えるようにする

## 現状できること

- `Freenove ESP32-S3 WROOM CAM` のカメラ映像を取得する
- Wi-Fi `STA` モードで既存アクセスポイントへ接続する
- `ESP-DL` の顔検出モデルを使って顔を検出する
- 顔枠と顔特徴点を抽出する
- ブラウザから映像ストリームを確認する
- Web 画面から `うつ伏せ` / `非うつ伏せ` のラベルを付けて 1 サンプルずつ保存する
- 保存済み CSV をエクスポートする
- CSV をリセットする

## 収集できる顔情報

保存先は `/sdcard/prone_samples.csv` です。

CSV には、少なくとも以下の情報を保存します。

- `timestamp`
- `session_id`
- `label`
- `face_detected`
- `face_score`
- `face_count`
- `bbox_x`
- `bbox_y`
- `bbox_w`
- `bbox_h`
- `left_eye_x`
- `left_eye_y`
- `left_eye_visible`
- `right_eye_x`
- `right_eye_y`
- `right_eye_visible`
- `nose_x`
- `nose_y`
- `nose_visible`
- `left_mouth_x`
- `left_mouth_y`
- `left_mouth_visible`
- `right_mouth_x`
- `right_mouth_y`
- `right_mouth_visible`

顔特徴点は次の 5 点を扱います。

- 左目
- 右目
- 鼻
- 左口角
- 右口角

座標は顔枠 (バウンティボックス) 基準で正規化して保存します。顔が未検出の場合や特徴点が使えない場合は、番兵値 `-1.0` と可視フラグ `0` を保存します。

## 画面と API

マイコン起動後、同一ネットワーク内のブラウザからアクセスできます。

- `GET /`
  Web 画面
- `GET /stream`
  映像ストリーム
- `GET /api/status`
  最新の検出状態
- `POST /api/sample`
  ラベル付きで 1 サンプル保存
- `GET /api/export`
  CSV ダウンロード
- `POST /api/reset`
  CSV リセット

Web 画面では、映像プレビュー、顔枠表示、検出状態、`session_id` 入力、保存ボタン、CSV 操作を提供します。

## 動作環境

- ボード: `Freenove ESP32-S3 WROOM CAM`
- フレーム解像度: `320 x 240`
- Wi-Fi: `STA` モード
- 保存先: microSD
- 推論基盤: `ESP-DL`
- ビルド環境: `ESP-IDF`

Wi-Fi の `SSID` とパスフレーズは `menuconfig` から設定する前提です。

## 現時点の位置づけ

このリポジトリは、最終的なうつ伏せ判定モデルの完成版ではありません。
現在は、学習用データを安定して収集するための基盤を中心に実装しています。

現段階で到達している主な範囲は次のとおりです。

- 顔検出
- 顔特徴点の取得
- 顔情報の可視化
- ラベル付き CSV 収集

未着手または今後の主対象は次のとおりです。

- PC 側のデータ検証
- 前処理
- 学習
- 評価
- `.espdl` 変換
- うつ伏せ判定モデルの組み込み

## 参考ドキュメント

詳細は `docs` 配下を参照してください。

- `docs/REQUIREMENTS.md`
- `docs/DESIGN.md`
- `docs/SPECIFICATIONS.md`
- `docs/TODO.md`
