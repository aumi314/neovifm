# Progress

## 2026-08-12

- 从 C2 `7e3e65d96` 创建并推送 `codex/neovifm-alpha0-portable-preview`。
- TDD 增加 CLI parsing、override precedence、standalone path 和 `--check` timeout/failure tests；TUI suite 为 `158 pass / 0 fail`，functions `91.95%`，lines `96.97%`。
- 真实 Linux standalone 首轮证明旧 runtime marker 无效，修为 executable basename detection。
- 完成 deterministic staging、source/license/notices/SHA256、依赖 metadata 和 archive verifier。
- 真实 PTY 首轮发现 standalone 缺少 Solid compile transform；显式接入 OpenTUI build plugin 后，中文/空格目录的 help/version/check、core 缺失/恢复与 F10 退出全部通过。
- `actionlint` 通过；`macos-15-intel` 是 GitHub 当前官方 runner label，但 actionlint 1.7.7 的内置 label 表滞后，因此只忽略该条 unknown-label 诊断。
- 待记录个人 fork CI run、四个平台 artifact 和 Ready PR。
