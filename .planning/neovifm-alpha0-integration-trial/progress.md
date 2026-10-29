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
- 当时 pending：本轮各层远端 CI、导师独立 Windows actions PR、最终 Windows 便携包校验与原生测试；完成证据见下文。
- 仍 pending：Windows 图形终端人工体验；自动化结果不能替代主观体验。
- out-of-scope：新功能、上游同步、release/tag/installer、自动合并、分支保护和备份清场。
- 记忆只读；备份与复核现场保留，不清理。

## 2026-10-16：审计阻塞修复

- 第一轮顶层 Preview `38674572145` 四平台通过；CI `38674572197` 未通过。Linux/macOS 在 audit 报警；Windows 在 core-client 的 arrival-order fixture 超时，尚不能宣称本轮全绿。
- B2b 第一轮 `38674316158` 确认同样被 audit 阻塞。补入独立安全提交 `f8f7c9694`：`brace-expansion@5.0.12`、`seroval@1.6.3`、`browserslist@4.28.9`，锁文件同步更新。依据 [brace-expansion advisory](https://github.com/advisories/GHSA-q2hr-2g5m-vwhr) 与 [Seroval advisory](https://github.com/advisories/GHSA-jp82-f5mq-hwhp)，首个补丁仍有其他公告时继续升到覆盖全部已知公告的补丁，不忽略 severity。
- B2b 现在是原 3 个 Windows 提交加 1 个独立依赖安全修复。后续五层再次 rebase 继承修复；rename 旧 browserslist 补丁与新锁文件冲突，保留已修复锁文件，只保留旧提交的历史说明。顶层与上一轮仅差依赖文件及 B2b 验证记录，产品文件无变化。
- 本地 B2b：Bun 1.3.10 frozen install、audit 零告警、145 单测和 typecheck 通过。
- Bun 1.3.10 经代理请求 registry cached manifest 的 304 时停滞；用 `--no-cache` 重新获取生成锁文件后正常。没有改全局代理或关闭 TLS 校验。

## 2026-10-16：实现验收与交付

| 层 | 重新整理后的 CI run | 结果 |
|---|---|---|
| Windows actions | [38840152564](https://github.com/aumi314/neovifm/actions/runs/38840152564) | Linux/macOS/Windows/gate 全绿 |
| Windows opener | [38840152037](https://github.com/aumi314/neovifm/actions/runs/38840152037) | 三平台/gate 全绿 |
| watcher | [38840152752](https://github.com/aumi314/neovifm/actions/runs/38840152752) | 三平台/gate，以远端最终结果为准 |
| portable preview | [38840153390](https://github.com/aumi314/neovifm/actions/runs/38840153390) | 三平台/gate 全绿；Preview 38840153446 四包全绿 |
| rename | [38840153204](https://github.com/aumi314/neovifm/actions/runs/38840153204) | 三平台/gate 全绿；Preview 38840153178 四包全绿 |
| multi-select | [38840152651](https://github.com/aumi314/neovifm/actions/runs/38840152651) | 三平台/gate 全绿；[Preview 38840152609](https://github.com/aumi314/neovifm/actions/runs/38840152609) 四包全绿 |

- 上表顶层 head 为 `6c0d47416c9df592d7ba6dfe4e6e436fbe98da50`，PR merge build/source 为 `f3f85f7461a5f267964b981df0c07e7e420ec26d`。之后的文档凭证提交不改变 runtime，单独保留其检查结果。
- 导师独立 Ready [PR #4](https://github.com/Rex-Diego/neovifm/pull/4)：base `5db377e4e`、head `f8f7c9694`、4 commits、可合并；PR body 请求将 `CI / gate` 设为 required check。未自动合并，也未创建其他重复导师 PR。
- 本地 Linux：focused C `106 tests / 10084 checks / 0 skipped`；TUI `192 pass / 0 fail / 709 expects`、函数/行 `92.93% / 97.27%`；integration `26 pass / 10 Windows-only skips / 0 fail`；typecheck、audit 和串行完整 `make check` 通过。
- 本地 Windows：Bun 1.3.10，TUI `192 pass / 0 fail / 709 expects`，报告总计函数/行 `94.74% / 98.48%`（Windows 报告同时列出测试文件）；core-client 单独 `19 pass`，未再复现第一轮超时，未改产品 deadline 或测试 timeout。
- 本机测试环境最初缺少 schema 目录和 `cat`，另一次 `bun run` 从 PATH 找到本机 Bun 1.4.2；这两次不算有效验收。补齐归档内 protocol fixtures、临时添加 Git usr/bin，并直接调用独立 Bun 1.3.10 后全过；没有修改全局 PATH。
- 包下载并解压到 `C:\Users\aumif\Documents\NeoVifm 试用\2026-10-16\validated-extracted\neovifm-workbench-alpha0-windows-x64-f3f85f7461a5\`。archive SHA256：`f0c7d6339cf8128634eb64c797efa138a6910986c5f908462d8e6104db960fb6`。
- 原生执行 `scripts/preview/verify.ts` 通过：包白名单、每个文件 SHA256、源码 tarball、许可证、依赖报告、version/help/check、移走 sibling core 的明确失败及恢复。额外只保留 System32 PATH、清除两个 core override 后 `--check` 成功，目标程序不依赖 Bun 或源码目录。
- 对这个包内的真实 core：actions/opener/watcher/rename/visual `10 pass / 0 fail / 70 expects`，包含 C/D 两个真实卷 move 拒绝、Recycle Bin、失败恢复、长中文路径和 no-overwrite；session persistence 单独 `1 pass / 0 fail / 5 expects`，probe 用例刻意未执行（便携包不含 probe）。新建测试目录之外的用户文件未参与操作。
- 原生 ConPTY 启动这个包显示双 pane，能进入 visual、退出并 F10；单独父进程 harness 读取实际 TUI exit code `0`，session 文件生成。工具外层 PowerShell 的 PTY 退出状态曾为 1，不能拿它替代子进程结果；独立证据 `native-pty-exit.json` 已区分二者。
- 最终包、测试日志、独立 workspace 和复核 harness 留在上述试用目录；Git bundle 与 backup refs 保留。原生桌面窗口的字体、鼠标和长时间人体工学体验仍待用户试用，不宣称已经人工验收。

## 知识收尾状态

- code/runtime：实现门槛及以上自动化路径 verified-current；真实人工试用 pending。
- docs：CURRENT_STATE、README、handoff 与 active plan changed-and-verified；协议/schema 未改。
- rules：项目 AGENTS、架构与 HACKING 已核对；命令沿用项目串行测试要求，未改规则。
- memory：generated-read-only，未写入；工作区备份与唯一未合并分支均保留。
- out-of-scope：导师是否合并、分支保护、release/签名/安装器、新功能和清场。
