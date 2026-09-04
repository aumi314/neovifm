# Progress

## 2026-09-04

- 接手项目并选定方向：Alpha 0 已收尾，按"用户体验最高准则"选择 Vifm keymap 兼容缺口作为下一切片。
- 调查确认权威语义（`src/modes/normal.c`）：`yy`/`Y` yank、`p` put-copy、`P` put-move、`dd` remove；`p`/`d` 当时是 TC 风格即时动作，与 Vifm 寄存器模型冲突。
- 确认协议零变更可行：`copy`/`move-files` 命令已携带双 pane identity，yank buffer 纯客户端持有，put 时按 `path_bytes_hex` 从最新 workspace 重解析 identity。
- 确认 `DD`（永久删除）无 core 能力，放弃映射——避免把 trash 删除伪装成永久删除。
- TDD 执行：
  - `test/keymap.test.ts`：改写别名测试 + 新增 yy/Y/p/P、dd 前缀、y 前缀拒绝、count 清理 4 条。
  - `test/yank.test.ts`（新）：9 条覆盖 yankFromSnapshot 与 resolvePutSource 全分支。
  - `src/keymap.ts`：FunctionAction 增加 `yank`/`put-copy`/`put-move`，`d`/`y` 前缀。
  - `src/yank.ts`（新）：yank buffer 与 put 源解析。
  - `src/app.tsx`：dispatchFunction 新增 yank/put 分支。
  - `test/app.test.tsx`：6 条集成测试（yank→put-copy、选择集 put-move、watcher 刷新后 identity 重解析、空 buffer、源不可见、dd 删除确认）。
  - `integration/yank-put.test.tsx`（新）：真实 core session 全链路——yy 零命令、p put-copy、u undo、P put-move、源目录离开 pane 后 put 拒绝。
- 验收结果（WSL Linux）：
  - `bun run typecheck` 干净。
  - 单测 `177 pass / 0 fail`；functions `92.90%`、lines `97.24%`（基线 92.20%/96.97%，未降级）；`yank.ts`、`keymap.ts` 均 100%。
  - `bun run test:integration`：`23 pass / 10 skip / 0 fail`（skip 均为 Windows/macOS 专属，比 C3 的 22 pass 多出的 1 条即本切片测试）。
  - `git diff --check` 干净；未触碰 `src/` C 代码，无需 `make check`。
  - `bun audit`：本机 WSL 无直连，经宿主 clash 网关 `172.29.208.1:7897` 跑通；2 个 high 漏洞均位于 `browserslist` 传递依赖（@babel 构建链），非本切片引入，记录待单独处理。

## 教训

- 同一文件的多个 Edit 调用必须串行发送；并行 Edit 会互相覆盖（本次丢失 FunctionAction 类型行，typecheck 兜底发现）。
- `.planning/.active_plan` 等单值文件用 `printf` 写入，PowerShell `echo` 会带 CRLF 导致 `git diff --check` trailing whitespace。
- WSL2 的 `127.0.0.1` 不是宿主；访问 clash 代理用默认网关 IP + 7897 端口。
- 100 列终端下 statusbar notice 会截断，集成测试断言用稳定前缀。
