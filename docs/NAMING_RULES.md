# 命名規則

## 1. 目的

本規則は、収集時の入力ゆれを防ぎ、後段の自動集計と分割を安定化させるための命名規則を定める。

## 2. 共通ルール

- 使用文字は英小文字、数字、アンダースコアのみ
- 空白は禁止
- ハイフンは禁止
- 大文字は禁止

## 3. `subject_id`

形式:

```text
baby_<3桁以上の連番>
```

例:

- `baby_001`
- `baby_012`

ルール:

- 同一被写体に複数 ID を振らない
- 他人と共有する一覧表で管理する

## 4. `session_id`

形式:

```text
<subject_id>_<yyyymmdd>_<time_block>_<setup_id>
```

例:

- `baby_001_20260315_am_setup01`
- `baby_001_20260315_pm_setup02`

`time_block` の許可値:

- `am`
- `pm`
- `night`

## 5. `location_id`

形式:

```text
loc_<2桁以上の連番>
```

例:

- `loc_01`
- `loc_02`

## 6. `lighting_id`

形式:

```text
light_<条件名>
```

例:

- `light_day`
- `light_room`
- `light_night`

## 7. `camera_position_id`

形式:

```text
campos_<条件名>
```

例:

- `campos_top`
- `campos_diag_left`
- `campos_diag_right`

## 8. `annotator_id`

形式:

```text
ann_<識別子>
```

例:

- `ann_a01`
- `ann_k02`

## 9. `capture_id`

形式:

- ミリ秒タイムスタンプ文字列

例:

- `1710000000000`

ルール:

- システムが自動生成する
- 手入力しない
