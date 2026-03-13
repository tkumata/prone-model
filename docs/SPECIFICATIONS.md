# 仕様

## 1. マイコン側仕様

### 1.1 動作モード

- Wi-Fi は `STA` モードで起動する
- 起動後にカメラ、SD カード、顔関連モデル、HTTP サーバを初期化する
- Web 画面は同一ネットワーク内のブラウザからアクセスする

### 1.2 HTTP エンドポイント

- `GET /`
  Web 画面を返す
- `GET /stream`
  映像ストリームを返す
- `GET /api/status`
  最新検出状態を JSON で返す
- `POST /api/sample`
  指定ラベルで1サンプル保存する
- `GET /api/export`
  CSV をダウンロードさせる
- `POST /api/reset`
  CSV を空にする

### 1.3 `POST /api/sample` の要求仕様

要求 JSON は以下とする。

```json
{
  "label": 1,
  "session_id": "20260313_a01"
}
```

- `label` は `0` または `1` のみ許可する
- `session_id` は空文字不可とする
- 顔が複数検出されている場合は保存失敗とする

### 1.4 `GET /api/status` の応答仕様

応答 JSON は以下を含む。

```json
{
  "face_detected": 1,
  "face_score": 0.82,
  "face_count": 1,
  "bbox": {
    "x": 48,
    "y": 36,
    "w": 102,
    "h": 118
  }
}
```

## 2. Web 画面仕様

- 映像プレビューを表示する
- 顔枠を重ねて表示する
- 現在の検出状態を表示する
- `うつ伏せ保存` と `非うつ伏せ保存` の2ボタンを表示する
- `CSV エクスポート` ボタンを表示する
- `CSV リセット` ボタンを表示する
- 保存成功、保存失敗、理由を表示する

## 3. CSV 仕様

### 3.1 保存先

- 保存先は `/sdcard/prone_samples.csv` とする

### 3.2 ヘッダ

```csv
timestamp,session_id,label,face_detected,face_score,face_count,bbox_x,bbox_y,bbox_w,bbox_h,left_eye_x,left_eye_y,left_eye_visible,right_eye_x,right_eye_y,right_eye_visible,nose_x,nose_y,nose_visible,left_mouth_x,left_mouth_y,left_mouth_visible,right_mouth_x,right_mouth_y,right_mouth_visible
```

### 3.3 列定義

- `timestamp`
  保存時刻。ISO 形式または起動後ミリ秒
- `session_id`
  人手が指定する収集単位識別子
- `label`
  `1 = うつ伏せ` `0 = 非うつ伏せ`
- `face_detected`
  `1 = 顔検出あり` `0 = 顔検出なし`
- `face_score`
  顔検出信頼度
- `face_count`
  検出された顔数
- `bbox_x` `bbox_y` `bbox_w` `bbox_h`
  顔枠情報
- 各座標列
  顔枠基準で正規化した座標
- 各 `visible` 列
  特徴点の可視状態

### 3.4 欠損仕様

- `visible = 0` のとき座標には番兵値 `-1.0` を入れる
- `face_detected = 0` のとき全特徴点を番兵値 `-1.0` とする
- `face_detected = 0` のとき全 `visible` は `0` とする

## 4. 正規化仕様

- `norm_x = (point_x - bbox_x) / bbox_w`
- `norm_y = (point_y - bbox_y) / bbox_h`
- `bbox_w <= 0` または `bbox_h <= 0` の場合は正規化せず、顔未検出扱いとする
- 顔枠情報自体は画像サイズ基準で `0.0 - 1.0` に正規化して保存してよい

## 5. 保存可否仕様

- `label` が `0/1` 以外なら保存しない
- `session_id` が空なら保存しない
- `face_count > 1` なら保存しない
- SD カードが書き込み不可なら保存しない
- 保存失敗時は HTTP エラーと理由を返す

## 6. 学習データ構築仕様

### 6.1 採用条件

- `face_detected = 1`
- `face_count = 1`
- `label` が有効
- 必須列欠落なし

### 6.2 除外条件

- 欠損だらけで特徴量が成立しない
- 同一セッション内で不自然な重複がある
- 収集メモ上で無効と判断したセッション

## 7. データ分割仕様

- `train`
- `valid`
- `test`

分割比率の初期値は以下とする。

- `train = 70%`
- `valid = 15%`
- `test = 15%`

ただし、分割は行単位ではなく `session_id` 単位で実施する。

## 8. 学習仕様

### 8.1 入力次元

- `20` 次元

### 8.2 入力内容

- 5点座標 `10`
- 5点可視フラグ `5`
- 顔枠 `4`
- 顔検出信頼度 `1`

### 8.3 初期モデル

- 全結合 `20 -> 32 -> 16 -> 1`
- 2値分類
- 出力は `0.0 - 1.0` のスコア

### 8.4 初期判定閾値

- `0.50`

## 9. 評価仕様

- `うつ伏せ` 再現率
- `うつ伏せ` 適合率
- 全体正解率
- 混同行列

採用条件の初期値は以下とする。

- `うつ伏せ` 再現率 `0.90` 以上
- `うつ伏せ` 適合率 `0.80` 以上

## 10. `.espdl` 変換仕様

- 学習済みモデルを保存する
- 保存したモデルを `.espdl` 形式へ変換する
- 変換時は入力次元、前処理、出力順序を固定する
- 変換後ファイル名は `prone_face_classifier.espdl` とする

## 11. エラーコード仕様

### 11.1 `POST /api/sample`

- `200`
  保存成功
- `400`
  ラベル不正、`session_id` 不正、複数顔
- `500`
  SD カード書き込み失敗、内部例外

### 11.2 `POST /api/reset`

- `200`
  リセット成功
- `409`
  確認トークン不一致
- `500`
  ファイル操作失敗
