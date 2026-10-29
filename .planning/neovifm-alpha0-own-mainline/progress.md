# 主线切换进度

## 2026-10-30 前置核对

- 导师 `origin/master` 仍是 `5db377e4e`（2026-08-27 合并 Linux actions PR #3），Windows actions [PR #4](https://github.com/Rex-Diego/neovifm/pull/4) 为 open、无评审；其 CI run `38840949224`（`f8f7c9694`）三平台与 gate 全绿。
- 个人 `fork/master` 也是 `5db377e4e`，六层成果此前只存在于分支与堆叠 PR #1–#6。
- 官方 `upstream/master` 仍是 `f5d60eaa`，`git rev-list --count HEAD..upstream/master` 为 0，本轮无需上游同步。
- 备份：`~/myproject/02_Projects/neovifm-project-review/2026-10-30/neovifm-before-own-mainline.bundle`，`git bundle verify` 通过（记录完整历史）；本地 `refs/codex-backups/` 15 条仍在，旧分支未删。
- 整合分支 `codex/neovifm-alpha0-own-mainline` = `b2a95ddb3`，与已全绿的 multi-select tip 树完全一致（`git diff --stat` 空），领先 `fork/master` 42 个提交，工作树干净。
- 合入前的远端证据（复核自 API，非记忆）：CI `38840152651`、Preview `38840152609`（`6c0d47416`）与 CI `38841150927`、Preview `38841150720`（`b2a95ddb3`）均为 success。

## 文档同源

- `AGENTS.md`、`docs/CURRENT_STATE.md`、`docs/NEOVIFM_HANDOFF.md`、`README.md` 改为个人仓库主线口径，删除「尚未合并到导师 master」「PR #2–#6 继续堆叠」等过期叙述；历史 planning 不回写。
- `.planning/.active_plan` 指向本目录。

## 整合与合并

- 整合 [PR #7](https://github.com/aumi314/neovifm/pull/7)：base `master`、head `codex/neovifm-alpha0-own-mainline`，44 个提交，`MERGEABLE`。
- 第一次 `CI` run `40915377789` 只有 `Audit TUI dependencies` 失败：新发布 5 个 seroval 公告（3 high / 2 moderate，影响 `<1.6.11`）。修法是独立提交 `219e15c35` 把 override 从 1.6.3 提到 1.6.11；本地 `bun audit` 变为 `No vulnerabilities found`，TUI 192 pass / 0 fail、typecheck 通过。
- 第二次 `CI` run [40917242184](https://github.com/aumi314/neovifm/actions/runs/40917242184)：Linux `2m17s`、Windows `8m30s`、macOS `2m15s`，`CI / gate` 通过。
- 第一次 macOS 两个 job 都以 15m1s 失败且 `runner` 为空、没有失败步骤，属于 hosted runner 未分配；重跑后 macOS 两次都正常完成。
- PR 的 `Preview` run `40917241926` 有三个包通过，`macos arm64` 长时间排队导致 `Preview / gate` 失败，同样属基础设施；以合并后的主线 `Preview` 为准。
- 合并提交 `4b2cc1314061f0182163a50189669e6713530c8f`（merge commit，保留 44 个提交的分组历史）。
- 个人仓库堆叠 PR #2–#6 已按“被 #7 取代”关闭，分支与备份保留；PR #1 由 GitHub 判定为 `MERGED`。导师侧 PR #4 未改动，仍开放。

## 主线验证

- 主线 `Preview` run [40921615010](https://github.com/aumi314/neovifm/actions/runs/40921615010)：Windows x64、Linux x64、macOS arm64、macOS x64 与 `Preview / gate` 全部 success。
- Windows 包 `neovifm-workbench-alpha0-windows-x64-4b2cc1314061` 下载到 `C:\Users\aumif\Documents\NeoVifm 试用\2026-10-30\`，48,525,743 字节，ZIP SHA256 `7b00985d7e72b305227eaa9e99840239cb18f9aa4597affd86d4fa743a9184a7`；包内 `source_commit` 与主线一致。
- 清空 `NEOVIFM_CORE_SESSION`/`NEOVIFM_CORE_PROBE` 并把 PATH 收窄到 `C:\Windows\System32` 后，`--version`、`--check`（`core neovifm-core-probe · protocol v3 · ok`）都返回 0，说明不依赖 Bun、源码目录或外部 core 覆盖。
- 真实 ConPTY 启动（独立 workspace 与独立 session 路径）：双栏正常渲染，F10 退出，子进程退出码 `0`（`pty-exit.json`），生成 854 字节、`version: 1` 的 session 文件。
- 本机没有重复跑 14 天前已完成的那 11 个原生集成用例：C core 与 TUI 代码自 `b2a95ddb3` 未变，期间只有文档与 seroval 依赖调整；主线 CI 已在 Windows runner 上重跑完整集成。

状态：代码、CI、四平台包与 Windows 冒烟均已验证；人工长时间试用仍 pending，release/安装器 out-of-scope。
