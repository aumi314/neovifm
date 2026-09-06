# Progress

## 2026-09-06

- 切片立项：多选 + 批量操作。调查确认 copy/move/delete 多 target 链路已存在；缺口为 visual 模式、select-all/clear-selection、批量 rename。
- 设计定案：批量 rename 不改协议——TUI 把选择集拆成逐个单目标 rename（复用提交时 identity 重解析），core 只新增 `select-all` / `clear-selection` 两个命令；visual 为客户端状态机，移动 = move + toggle-selection 顺序命令。
- 计划见 `task_plan.md`。

## 2026-09-06（实现完成）

- 全部按 `task_plan.md` 执行完毕，计划内偏差都记录在 task_plan.md 的「实施偏差记录」节（visual 收缩命令顺序、Esc 消歧缓冲两点是真实发现的坑）。
- C core：`select-all` / `clear-selection` 两条命令落地（enum/parse/exec/schema/README），payload 可选 `pane`，空 pane no-op、无效 pane 拒绝。
- TUI：`v`/`V` visual-line（anchor 感知的扩展/收缩命令对）、`Ctrl+A` 全选、`Esc` 清选（空选择不发命令）、cw/cW 选择集 rename 队列（Enter 前进、Esc 中止、按 path 重解析 identity）、状态栏 VISUAL badge。
- 验收结果：
  - `make -C tests neovifm_snapshot`：106 tests / 10084 checks 全过（新增 3 个 C 测试）。
  - `bun run test:coverage`：192 pass / 0 fail；行覆盖 97.27%，keymap.ts 100%。
  - `bun run typecheck`：干净；`bun audit`：无漏洞。
  - `bun run test:integration`：26 pass / 10 skip（Windows-only）/ 0 fail。
  - `env -u VIFM -u MYVIFMRC make check`（串行）：PASS。
  - `git diff --check`：干净。
- 未做（按纪律）：未 commit/push；协议 rename 仍单 target（刻意保留 `core_session.c` 与 `workspace_session.c` 的两道限制）。
