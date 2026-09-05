# NeoVifm Alpha 0: Vim Verbs (yank/put + 键位语义对齐)

## 目标

让 Hybrid TUI 的文件动词对齐经典 Vifm 语义：`yy`/`Y` yank、`p`/`P` put-copy/put-move、`dd` 删除。零协议变更、零 C core 改动，全部在 `clients/tui` 内完成。

动机：Alpha 0 已具备文件操作与 undo 能力，但键位是 TC 风格即时动作（`p`=复制到对面），Vim 用户肌肉记忆里的寄存器模型（先 yank 后 put）完全缺失。这是 OpenTUI 界面"日常可用"的最大键盘缺口。

## 权威语义依据（src/modes/normal.c）

| 键位 | 经典 Vifm 语义 | 当前 Hybrid TUI | 迁移 |
|---|---|---|---|
| `yy` / `Y` | yank 文件到寄存器 | 无 | 新增 |
| `p` | put files **by copying**（从寄存器粘贴副本） | 复制到对面 pane（TC 风格） | **改语义** |
| `P` | put files **by moving**（从寄存器粘贴移动） | 移动到对面 pane（TC 风格） | **改语义** |
| `dd` | remove files（trash） | `d` 单键删除 | **改为双键** |
| `DD` | remove files permanently | `D` 单键删除 | 暂映射为同 `dd`（core 无永久删除），矩阵标 deferred |

复制/移动到对面 pane 的 TC 语义继续由 `F5`/`F6` 承担，不丢失。

## 设计

### Yank buffer（客户端持有，不动协议）

- `clients/tui` 新增 yank 状态：源 pane + 源 cwd path + entry 的 `path_bytes_hex` 列表 + yank 时间戳。纯路径引用，不存 identity。
- `yy`/`Y`：有选择集时 yank 选择集，否则 yank 当前项。statusbar 反馈 "N item(s) yanked"。
- **不**实现命名寄存器（`"a` 等），单个默认 buffer，对齐 `def_reg()` 行为。

### Put（`p` / `P`）

- 目标：活动 pane 当前 tab 的 cwd（Vifm 语义，不是对面 pane）。
- 从最新 workspace 状态按 `path_bytes_hex` 重新解析每个源 entry 的当前 identity（device/inode/ctime），再走现有 `copy` / `move-files` 命令路径（复用确认-free 直接执行 + task center 事件 + undo）。
- 源已失效（被外部删除/改名/移动）：整条命令不发送，statusbar 报具体失效路径数；部分失效时报告并中止（不静默跳过，避免用户以为全部粘贴成功）。
- 空 buffer 时 `p`/`P` 给明确提示，不发命令。
- watcher 刷新后 identity 变化不影响 buffer 有效性，因为按 path 重新解析。

### 键位迁移

- `keymap.ts`：`p`/`P` 从 function action copy/move 改为 put 语义；`d` 从单键 delete 变为前缀（`dd` → delete）；`D` 同。
- 删除确认对话框、undo 行为不变，仍走现有 delete 路径。
- `docs/NEOVIFM_KEYMAP_MATRIX.md` 同步更新：`yy`/`Y`/`dd` 标记为 supported，`p`/`P` 记录语义迁移决策（从 TC 别名到 Vifm put），`DD` 标 deferred（永久删除）。

## 明确不做

- rename（`cw`/`cW`）：需要协议新增 action（move-files 无法指定新文件名），独立切片。
- visual mode（`v`/`V`/`gv`）、marks（`m`/`'`）、redo（`Ctrl-R`，core 无 redo）、registers（`"a`）、`C` clone、永久删除。
- 协议、schema、C core 任何改动。

## 执行步骤（TDD）

1. [x] keymap 单测先行：`yy`/`Y`/`p`/`P`/`dd` 序列解析、count 前缀交互、`d` 前缀 pending、与现有前缀无冲突。
2. [x] yank buffer 模块单测：yank 当前项/选择集、put 时 identity 重解析、源失效检测、空 buffer。
3. [x] 最小实现通过上述测试。
4. [x] 真实 session 集成测试：yank → 切 pane → put-copy → 目标 pane 出现副本；put-move → 源消失目标出现；undo 回滚 put；watcher 刷新后 put 重解析 identity；源目录离开 pane 后 put 拒绝。
5. [x] 更新 KEYMAP_MATRIX.md 与本计划状态。

## 状态：Complete（2026-09-04）

验收记录见 `progress.md`。实施中的一处计划修正：`DD` 未按原计划映射为同 `dd` 的删除——core 只有 trash 删除，映射会伪装"永久删除"语义，改为不映射并标 deferred。

## 验收

- `bun run test:coverage` 全绿，functions/lines 覆盖率不低于当前基线（92.20% / 96.97%）。
- `bun run typecheck`、`bun audit` 通过。
- Linux real-core integration 全绿。
- `git diff --check` 干净。
- 经典 `vifm` 零改动（本切片不碰 src/），无需 `make check`；若意外触碰则补跑。

## 远端/发布

- 沿用个人 fork PR 流程；本切片结束后由用户决定是否堆叠新 PR。
