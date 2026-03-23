# 運用固定

## 1. 目的

本書は、`metadata.csv` の後方互換、`subject_id` と `session_id` の台帳運用、ラベル監査、学習利用可否の変更履歴、実機確認用集合の分離、および実機合格基準の計測手順を固定する。

フェーズ2では、本書に定める運用を暫定運用ではなく標準運用として扱う。

## 2. `metadata.csv` の後方互換ルール

### 2.1 基本方針

- 現行列順は固定し、既存列の削除、改名、意味変更を禁止する
- 新しい列を追加する場合は末尾追加のみを許可する
- 既存列が空文字許容であっても、意味を変える既定値の変更は行わない
- PC 側処理は既知の必須列がそろっていることを前提にし、未知の追加列は無視してよい
- `metadata.csv` の互換性は `major.minor` 形式の内部版番号で管理する

### 2.2 版番号運用

- 列追加のみで既存列の意味と順序が変わらない変更は `minor` を加算する
- 必須列の削除、改名、列順変更、意味変更は `major` を加算し、同時に旧版入力の受け入れを停止する
- 初期固定版は `1.0` とする

### 2.3 互換判定

- `1.x` 系では、先頭から `board_name` までの既存20列が同一順序で存在する限り後方互換ありとみなす
- 旧版 `metadata.csv` を取り込む PC 側処理では、必須列不足があれば停止する
- 新版 `metadata.csv` を旧版処理へ渡す場合、追加列の存在のみで失敗してはならない
- `label`、`label_name`、`is_usable_for_training`、`exclude_reason`、`board_name` の意味変更は禁止する

### 2.4 変更手続き

1. 変更理由を本書の変更履歴へ記録する
2. `REQUIREMENTS.md`、`SPECIFICATIONS.md`、`README.md` の関連箇所を同時更新する
3. PC 側検証で必須列監査が通ることを確認する
4. `docs/TODO.md` に未完了の移行作業があれば追記する

## 3. `subject_id` と `session_id` の運用台帳

### 3.1 台帳ファイル

- `artifacts/ledger/subject_registry.csv`
- `artifacts/ledger/session_registry.csv`

これらは Git 管理対象外の運用台帳とし、更新責任者は収集担当者とする。

### 3.2 `subject_registry.csv` 必須項目

- `subject_id`
- `subject_status`
- `first_session_id`
- `latest_session_id`
- `notes`

### 3.3 `subject_status` 許可値

- `active`
- `hold`
- `closed`

### 3.4 `session_registry.csv` 必須項目

- `session_id`
- `subject_id`
- `session_date`
- `time_block`
- `setup_id`
- `location_id`
- `lighting_id`
- `camera_position_id`
- `annotator_id`
- `dataset_role`
- `review_status`
- `notes`

### 3.5 `dataset_role` 許可値

- `train_candidate`
- `validation_candidate`
- `test_candidate`
- `device_validation`
- `excluded`

### 3.6 `review_status` 許可値

- `open`
- `reviewed`
- `approved`

### 3.7 運用ルール

- 新しい `subject_id` を使う前に `subject_registry.csv` へ登録する
- 新しい `session_id` を使う前に `session_registry.csv` へ登録する
- 1 つの `session_id` は 1 つの `subject_id` にのみ紐付ける
- `dataset_role=device_validation` の `session_id` は学習、検証、評価へ再利用してはならない
- `subject_status=closed` の被写体へ新規 `session_id` を追加してはならない
- `review_status=approved` になるまで、そのセッションは最終学習投入候補に昇格させない

## 4. ラベル監査フロー

### 4.1 対象

- `is_usable_for_training=1` の全件
- `annotation_error` へ変更された件
- 実機確認用集合へ割り当てる件

### 4.2 手順

1. 収集担当者が保存直後に入力値の誤りを確認する
2. 別のレビュー担当者が `metadata.csv` と画像を見てラベル妥当性を確認する
3. 誤りがあれば `is_usable_for_training=0` とし、`exclude_reason=annotation_error` へ更新する
4. 判定が難しい場合は `exclude_reason=ambiguous_pose` を優先する
5. 監査後に `session_registry.csv` の `review_status` を更新する

### 4.3 抜取率

- 学習利用可サンプルが 200 件未満のセッションは 100% 監査する
- 200 件以上のセッションは各ラベル少なくとも 50 件、かつ全体の 20% 以上を監査する
- 実機確認用集合へ入れる候補は 100% 監査する

### 4.4 差し戻し条件

- 誤ラベル率が 2% を超えたセッション
- `subject_id` または `session_id` の入力誤りが 1 件でも見つかったセッション
- `is_usable_for_training` と `exclude_reason` の矛盾が 1 件でも見つかったセッション

差し戻し時は当該セッション全件を再監査する。

## 5. 学習利用可否の変更履歴管理

### 5.1 変更履歴ファイル

- `artifacts/ledger/training_eligibility_history.jsonl`

1 行 1 変更として追記し、過去行の上書きは禁止する。

### 5.2 記録項目

- `changed_at`
- `capture_id`
- `subject_id`
- `session_id`
- `before_is_usable_for_training`
- `after_is_usable_for_training`
- `before_exclude_reason`
- `after_exclude_reason`
- `changed_by`
- `change_reason`

### 5.3 変更ルール

- `is_usable_for_training` または `exclude_reason` を変えた場合は必ず同時記録する
- `change_reason` は空欄禁止とする
- 学習実行後に可否変更が発生した場合は、同じ `run_name` の成果物を再利用せず再学習する
- `annotation_error` への変更はレビュー起因として扱い、`changed_by` にレビュー担当者を入れる

## 6. 実機確認用集合の分離ルール

### 6.1 基本原則

- 実機確認用集合は、学習、閾値調整、最終評価から分離する
- 同一 `session_id` の画像を、学習系分割と実機確認用集合へ混在させてはならない
- 可能な限り、学習系へ未使用の `subject_id` を優先して割り当てる

### 6.2 割り当て優先順

1. 学習系未使用の `subject_id`
2. 学習系既使用だが未使用 `session_id`
3. 上記が不足するときのみ、学習系既使用 `session_id` と時刻的に十分離れた別セッション

### 6.3 台帳記録

`session_registry.csv` の `dataset_role` により固定する。

- `device_validation` にしたセッションは学習、検証、評価へ使わない
- `train_candidate`、`validation_candidate`、`test_candidate` のセッションを実機確認用へ転用する場合は、新しい別セッションを採り直す

### 6.4 量的目安

- 実機確認用集合は各クラス 200 件以上を目標とする
- 少なくとも `lighting_id` 3 条件、`camera_position_id` 3 条件を含める
- `prone` への遷移直後など紛らわしい姿勢を両クラス合計の 10% 以上含める

## 7. 実機合格基準の計測手順

### 7.1 前提

- 評価対象モデルは固定済み閾値を持つ
- 比較対象として PC 側量子化参照結果を保存済みである
- 実機確認用集合の正解ラベルは監査済みである

### 7.2 計測準備

1. 対象 `.espdl`、`reports/threshold.json`、PC 側量子化参照結果を同一 `run_name` から用意する
2. 実機確認用集合の入力識別子一覧を確定する
3. 推論ログ保存先を `artifacts/device_validation/<run_name>/` とする

### 7.3 実行手順

1. 実機で 300 フレーム相当以上の連続推論を実施する
2. 各入力について `推論時刻`, `入力識別子`, `予測クラス`, `prone` スコア, `判定閾値`, `正解ラベル` を記録する
3. 同一入力識別子に対する PC 側量子化参照結果と突合する
4. `prone` 再現率、`non_prone` 適合率、クラス一致率、不一致件数を算出する

### 7.4 合格判定

- 異常終了 0 件
- `prone` 再現率 `0.90` 以上
- `non_prone` 適合率 `0.90` 以上
- PC 側量子化参照とのクラス一致率 `0.98` 以上
- 説明不能な不一致率 `0.02` 以下

### 7.5 不合格時の扱い

- 閾値未固定、前処理不一致、クラス順不一致のいずれかが見つかった場合は即不合格とする
- 説明不能な不一致が基準超過した場合は `.espdl` 採用を禁止する
- 学習利用可否の変更履歴が計測対象に対して発生していた場合は、台帳更新後に計測をやり直す

## 8. 変更履歴

- `2026-03-23`: フェーズ2の運用固定版として初版作成。`metadata.csv` 後方互換、台帳、監査、変更履歴、実機確認用集合分離、計測手順を定義した
