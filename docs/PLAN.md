# 計画

## 1. 目的

組み込み C 言語開発で、エージェントが作業を停止するたびに ESP-IDF ビルドを自動実行するハーネスを導入する。

対象イベントは次とする。

- Codex CLI の `Stop`
- Copilot CLI の `agentStop`

ハーネスは `source "$IDF_PATH/export.sh" && idf.py build` 相当の処理を実行し、失敗した場合はビルドエラーの要点と修正指示をエージェントへ返す。

## 2. 実装対象

実装対象ディレクトリは次とする。

- `.agent-hooks/`
- `.codex/`
- `.github/hooks/`

`.agent-hooks/` に共有ハーネスを置き、CLI 固有の hook 定義は共有ハーネスを呼び出すだけにする。

## 3. 方針

- hook ごとの処理差分を最小化する
- ESP-IDF v6.0 を前提にする
- `IDF_PATH` が設定されている場合はそれを優先する
- `IDF_PATH` が未設定の場合は `$HOME/.espressif/v6.0/esp-idf` を候補にする
- 既存 build tree がある場合は構成済み Python 環境を優先する
- 作業ディレクトリはリポジトリルートに固定する
- ビルドログは保存し、失敗時だけ要約して返す
- secret や不要なローカル環境情報をエージェント出力へ過度に含めない

## 4. 実装順序

1. Planner が本計画、要件、仕様、設計、ADR、task を更新する
2. ユーザー承認を待つ
3. Coder が `.agent-hooks/` に共有ハーネスを実装する
4. Coder が `.codex/hooks.json` に Codex CLI 用 `Stop` hook を定義する
5. Coder が `.github/hooks/hooks.json` に Copilot CLI 用 `agentStop` hook を定義する
6. 意図的に失敗する条件でエラー返却形式を確認する
7. 通常の ESP-IDF build が成功することを確認する

## 5. 完了条件

- Codex CLI の `Stop` から共有ハーネスが呼ばれる
- Copilot CLI の `agentStop` から共有ハーネスが呼ばれる
- ビルド成功時は hook が成功終了する
- ビルド失敗時は hook が非ゼロ終了し、修正に必要なエラー要約を返す
- 既存の ESP-IDF v6.0 開発環境と矛盾しない
- 関連ドキュメントが実装内容と同期している
