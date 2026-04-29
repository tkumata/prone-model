# current task

## objective

- Codex CLI `Stop` と Copilot CLI `agentStop` の両方で ESP-IDF build を実行する hook ハーネスを導入する
- 失敗時にビルドエラー要約と修正指示をエージェントへ返す
- 実装はユーザー承認後に行う

## planner steps

- `docs/PLAN.md` を作成する
- `docs/REQUIREMENTS.md` に hook ハーネス要件を追記する
- `docs/SPECIFICATIONS.md` に hook 入出力と終了コード仕様を追記する
- `docs/DESIGN.md` に共有ハーネス設計を追記する
- `docs/ADR/202604290959.md` を作成する
- 既存 `docs/TASK/current-task.md` を退避し、新しい current task を作成する
- ユーザー承認を待つ

## coder steps

- 完了: `.agent-hooks/build.sh` に共有ハーネスを実装する
- 完了: `.codex/hooks.json` に Codex CLI `Stop` hook を定義する
- 完了: `.github/hooks/hooks.json` に Copilot CLI `agentStop` hook を定義する
- 完了: 失敗時出力を確認する
- 完了: ESP-IDF build 成功を確認する

## acceptance

- 完了: hook 設定は共有ハーネスを呼ぶだけである
- 完了: build 成功時は hook が成功終了する
- 完了: build 失敗時は hook が非ゼロ終了し、修正指示を返す
- 完了: ログは `.agent-hooks/logs/` から確認できる
- 完了: 関連ドキュメントが実装内容と同期している
