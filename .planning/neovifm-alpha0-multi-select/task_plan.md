# Task Plan: NeoVifm Alpha 0 多选 + 批量操作（multi-select）

## 目标

把文件操作从"光标下单个"升级为"选择集批量"，补齐 Vifm/TC 文件管理器的核心交互。用户对选择集执行 copy/move/delete 的路径已经端到端存在（`actionTargets()` 已发送 selection），本切片补的是缺口：visual 模式、select-all/clear-selection、批量 rename。

## 现状事实（2026-09-06 调查）

- copy/move/delete 多 target 已在 core parse/prepare/worker/事件/TUI 全链路打通；targets 上限 64（`NV_SESSION_MAX_ACTION_PATHS`）。
- selection 状态由 C core 持有（`pane_snapshot.h:72/88`），随 workspace-snapshot 发布（`entries[].selected`、`selection_count`），refresh/enter/parent 后按 name 保留。
- 现有选择命令只有 `toggle-selection`（键 `t`，不移动光标）和 `select-entry`（鼠标定点）。没有 select-all / clear-selection / visual。
- rename 被刻意堵在单 target：`core_session.c:750`（parse）、`workspace_session.c:1549-1555`（prepare）、`app.tsx:1343`（TUI 提示 "Batch rename is not supported yet"）。
- 键位空位：`v`/`V` 完全空闲；`Ctrl+A` 空闲；空格=quick-view、Tab=切 pane、`t`=toggle-selection，都已占用不动。

## 设计决策

1. **批量 rename 不改 core 协议**。TUI 在选择集非空时进入"rename 队列"：逐个弹出单目标 rename 对话框（预填当前名），提交时按 path 从最新 workspace 重解析 identity（复用单 rename 的防 stale 机制）。`Enter` 应用该个并前进，`Esc` 中止剩余队列（已应用的可 `u` 逐个撤销）。理由：协议保持单 target 简化校验与 undo 粒度；C 侧零改动；批量冲突/非法名的错误处理全部复用。
2. **新增两个 core 命令**：`select-all {pane}`、`clear-selection {pane}`。enum + parse + 执行 + schema + README + C 测试。这两个状态下 core 是正确的事实源，不能靠客户端循环 toggle 模拟（不可重入、慢、刷新中途与 watcher 冲突）。
3. **Visual 模式为客户端结构**：`v`/`V` 进入 visual-line（anchor=当前行并选中），`j`/k/方向键移动 = 发送 `move` 后紧跟 `toggle-selection`（core 顺序执行命令，天然等价于 vim visual-line 的扩展/收缩）。`v`/`Esc` 退出 visual，选择集保留（Vifm 语义）。h/l/Enter/gg/G 等导航键在 visual 中先退 visual 再执行原行为——不为视觉态发明跨目录语义。
4. **键位**：`Ctrl+A` = select-all（active pane）；`Esc`（normal 模式、非对话框时）= clear-selection（无选择时 noop）。这些都是新键位，不碰既有绑定。
5. cw 在 visual 中同样走 rename 队列；队列完成后保留 `Esc` 清理选择的常规途径（不自动清，和 Vifm 一致）。

## 明确不做

- 跨 pane selection、批量 rename 模板/正则/编号（vimv 外编辑器方案留到后续真实需求驱动）。
- `yy/p/dd` 对选择集的行为不加新语义（已存在）。
- 多 target rename 协议形态（单 target 上限保留，是刻意的协议约束）。
- 熔断：rename 队列中途 core 拒绝（如 stale/conflict）→ 该条失败任务照常展示，队列继续下一项，不回滚已成功的。

## 执行步骤（TDD）

1. [x] C focused 测试先行：`tests/neovifm_snapshot/session_selection.c`（新文件，规避 stic 800 行上限）覆盖 select-all/clear-selection 的状态、对 watcher 刷新的持久性、无效 pane/未知命令拒绝。
2. [x] core 实现：enum (`workspace_session.h`)、parse (`core_session.c`)、exec (`workspace_session.c`)、schema (`protocol/neovifm-core-v3.schema.json`)、`protocol/README.md`。
3. [x] TUI 单测先行：keymap（`v`/`V`/`Ctrl+A`/`Esc`）、visual 进入/扩展/退出状态转移、rename 队列（含 Esc 中止、identity 重解析、cW 保留扩展名逐个生效）。
4. [x] TUI 实现：keymap + app.tsx visual state machine + rename 队列对话框 + select-all/clear-selection 命令发送。
5. [x] 真实 session integration：rename-session.test 加批量用例（选 3 个文件 cw 逐个改名 + undo 逐个还原 + Esc 中止）；新文件 `visual-session.test.tsx` 覆盖 visual j/k 选中/收缩与 Ctrl+A/Esc。
6. [x] 更新 `docs/NEOVIFM_KEYMAP_MATRIX.md`（v/V/Ctrl+A/Esc/cw 批量）、`protocol/README.md`、本计划；CURRENT_STATE 不动（尚在"完整 Vifm keymap 未完成"表述内）。
7. [x] 全套验收（见progress.md 2026-09-06 条目）。

## 实施偏差记录（2026-09-06 实现时核实后调整）

- **visual 扩展/收缩的命令顺序需要 anchor 感知**。原方案"移动即 move + toggle"在收缩时会选中错行（离开后留下的行保持选中，中间出现空洞）。实现改为：app 记录 visual 起点的 anchor index，远离 anchor（扩展）发 `move`+`toggle-selection`（新行选中），朝向 anchor（收缩）发 `toggle-selection`+`move`（离开的行取消）。core 命令串行 FIFO 语义不变，真实 vim visual-line 收缩语义正确。
- **z-order 意识**：Escape/其它导航键在 visual 中先退模式再执行原命令，通过 keymap `#handleVisual` 的递归重派实现；`Ctrl+A` 在两种模式下都直接全选。
- **opentui Escape 有 ~20ms Alt 前缀消歧缓冲**：测试里按完 Escape 必须 sleep ≥30ms 或经 waitFor 轮询再按下一个键，否则会和后续键合成 meta 组合键（integration 里 `Esc` 紧跟 `u` 曾因此变成 Meta+U 丢键）。
- **未知/无效 pane**：parse 阶段已拒绝（`pane_from_string`），apply 层仍带 `valid_pane` 双保险；C 测试覆盖 apply 层，协议层用真实 binary 冒烟验证 `clear-selection {"pane":"middle"}` 返回 `invalid-command`。

## 验收

- `env -u VIFM -u MYVIFMRC make -C tests neovifm_snapshot`（串行）通过。
- `clients/tui`：test:coverage、typecheck、audit、test:integration 全绿。
- 串行 `env -u VIFM -u MYVIFMRC make check`。
- `git diff --check` 干净。
- 经典 `vifm` 行为不回归：全部改动落在 core-session / TUI / 协议扩展，不碰 classic UI。
- 推送 fork 后 PR CI + Preview 全绿。

## 风险提示

- visual 模式的"移动即发送 toggle"依赖命令顺序语义，core queue 是串行 FIFO（已验证），但 integration 要真按键盘测一遍扩展/收缩。
- rename 队列期间 watcher 可能刷新 workspace：队列项存的必须是 path identity 而不是 index——复用单 rename 的提交时重解析，天然满足。
- 失败即停 vs 继续：action_task worker 对多 target 是失败即停带 partial；但批量 rename 我们是逐条命令，单条失败不回滚之前已成功的，用户可用 u 逐个撤销——语义写进文档，避免惊喜。
