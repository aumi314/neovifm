# 整合与试用记录

## 2026-10-15：备份与分支整理

- 导师 `origin/master@5db377e4e` 已包含 Linux actions，tree 与原 Linux 分支 `a28359dbd` 相同。
- 工作树干净后，建立全部 refs 的完整 bundle 并通过 `git bundle verify`：`/home/aumif/myproject/02_Projects/neovifm-project-review/2026-10-15/neovifm-before-integration-eab1a31c5.bundle`。
- 旧 refs 保留于本地 `refs/codex-backups/2026-10-15/`，未删除历史分支。
- 依次 rebase 后，每层 `git diff --exit-code <backup-ref> <new-branch>` 均通过。导师 master 是新顶层分支祖先。

| 分支尾名 | 旧 tip | rebase 后 tip |
|---|---|---|
| windows-actions | `8b610b933` | `7c2bb58d0` |
| windows-opener | `6a56c286e` | `6efe68c0a` |
| cross-platform-watcher | `7e3e65d96` | `8658619cf` |
| portable-preview | `84c83d779` | `854461627` |
| rename | `b1cb3d017` | `c713f5766` |
| multi-select | `eab1a31c5`（含本轮计划） | `ee8442e0e` |

- 使用一次 atomic push 和逐分支明确旧 SHA 的 `--force-with-lease` 更新六层个人分支；fork master 仅 fast-forward 到导师 master。个人 PR #1 的 base 已改为 master，确认只有 3 个 Windows 提交；其余 base 不变。
- GitHub API 发生间歇性 TLS timeout/EOF；未把失败请求当作已完成，重新请求并读取返回结果。

## 验证补缺与当前待办

- 发现 rename/visual real-core TUI integration 原来跳过 Windows；解除这两组纯跨平台用例的 skip，并在 Windows CI 增加独立日志步骤。未修改产品代码、protocol 或 schema。
- 本地 Bun 1.3.10：rename/visual `3 pass / 0 fail / 19 expects`；typecheck 通过。
- 当前状态与 handoff 修正多选、逐项改名和未合并边界；active plan 指向本目录。旧 planning 保留历史，不作为当前能力入口。
- pending：本轮各层远端 CI、导师独立 Windows actions PR、最终 Windows 便携包校验与原生测试。
- pending：Windows 图形终端人工体验；自动化结果不能替代主观体验。
- out-of-scope：新功能、上游同步、release/tag/installer、自动合并、分支保护和备份清场。
- 记忆只读；备份与复核现场保留，不清理。

## 2026-10-16：审计阻塞修复

- 第一轮顶层 Preview `38674572145` 四平台通过；CI `38674572197` 未通过。Linux/macOS 在 audit 报警；Windows 在 core-client 的 arrival-order fixture 超时，尚不能宣称本轮全绿。
- B2b 第一轮 `38674316158` 确认同样被 audit 阻塞。补入独立安全提交 `f8f7c9694`：`brace-expansion@5.0.12`、`seroval@1.6.3`、`browserslist@4.28.9`，锁文件同步更新。依据 [brace-expansion advisory](https://github.com/advisories/GHSA-q2hr-2g5m-vwhr) 与 [Seroval advisory](https://github.com/advisories/GHSA-jp82-f5mq-hwhp)，首个补丁仍有其他公告时继续升到覆盖全部已知公告的补丁，不忽略 severity。
- B2b 现在是原 3 个 Windows 提交加 1 个独立依赖安全修复。后续五层再次 rebase 继承修复；rename 旧 browserslist 补丁与新锁文件冲突，保留已修复锁文件，只保留旧提交的历史说明。顶层与上一轮仅差依赖文件及 B2b 验证记录，产品文件无变化。
- 本地 B2b：Bun 1.3.10 frozen install、audit 零告警、145 单测和 typecheck 通过。
- Bun 1.3.10 经代理请求 registry cached manifest 的 304 时停滞；用 `--no-cache` 重新获取生成锁文件后正常。没有改全局代理或关闭 TLS 校验。
