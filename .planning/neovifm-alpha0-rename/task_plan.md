# NeoVifm Alpha 0: Rename（cw/cW）

## 目标

让 Hybrid TUI 支持单文件/目录重命名：`cw` 改完整名字、`cW` 只改 root（保留扩展名），对齐经典 Vifm 语义。这是当前唯一的文件操作能力缺口（copy/move/mkdir/delete/undo 已有）。

## 权威语义依据（src/modes/normal.c）

- `cw` → "rename files"（`fops_rename_current`），`cW` → "rename root of current file"（不含扩展名部分）。
- 选择集批量 rename（`fops_rename(curr_view, NULL, 0, 0)`）**不在本切片**：选择集非空时明确提示。

## 协议扩展（v3 同版本兼容）

- 新命令 `rename`：source identity（pane/cwd_bytes_hex/snapshot_revision/cwd_device/inode/ctime）+ `targets`（复用 actionTarget 形状，**恰好 1 项**）+ `name`（新 basename，UTF-8 string，1-255 字符）。不携带 destination 字段——destination 恒等于源目录。
- hello 新增 capability `file-rename-v1`；客户端仅在发布时启用 cw/cW。旧 core + 新客户端安全降级，旧客户端不发 rename。
- action-task 事件 action 枚举增加 `"rename"`。
- 同版本加 action 枚举值符合协议兼容规则（新增可选能力，不改已有字段语义）。

## core 设计（复用 move 执行路径，无新 fs 原语）

- `workspace_session.h`：新增 `NV_SESSION_RENAME`。
- `core_session.c` 解析：`rename` 分支 = `parse_action_identity(payload, command, 0, 1)` + `name` 字段（非空、≤255 字节）。
- `workspace_session.c` prepare：
  - `valid_name(name)` 校验（复用 mkdir 规则：非空、非 `.`/`..`、无 `/` `\`）。
  - 恰好 1 个 target；新名字 ≠ 当前 basename（相同报 `invalid-name`）。
  - `destination_directory` = 源目录副本，destination identity = 源 identity。
  - 校验通过后把 `targets[0].name` 替换为新名字副本——execute/undo 现成逻辑自动把 `old_path → 同目录/新名`。
- `command_is_action`、`pending_action_context_prepare`（clone + has_destination，destination pane/tab = 源 pane/tab）、`record_action_undo`（走 `record_move_group`，source/dest parent 相同）均加入 RENAME。
- `action_task.c` execute：RENAME 走 `nv_fs_move` 分支；`failure_code` 增加 `rename-failed`。
- `snapshot_json.c`：action 字符串映射增加 `rename`；hello capabilities 增加 `file-rename-v1`。
- no-overwrite 由 `nv_fs_move` 现有 `RENAME_NOREPLACE` 语义保证；重名目标 → `destination-exists`。

## 客户端设计

- `core-client.ts`：CoreSessionCommand 增加 `rename` 变体。
- `protocol.ts`：ActionTaskPayload action 枚举增加 `"rename"`。
- `keymap.ts`：新增 `c` 前缀——`cw` → `rename`，`cW`（c 后 Shift+W）→ `rename-root`；其余 `c?` unhandled。
- `app.tsx`：
  - `canRename()` = capabilities 包含 `file-rename-v1`。
  - DialogState 增加 rename 形态：预填当前名（cw 全名 / cW root 部分），提交校验对齐 mkdir（非空、≤255 字节、非 `.`/`..`、无 `/` `\` `\0`）；新名 = 旧名直接关闭（无操作）；cW 提交时拼回原扩展名（无扩展名或点文件按全名处理）。
  - 选择集非空时提示 `Batch rename is not supported yet`，不开对话框。
  - 发送 rename 命令（单 target + identity），notice 风格对齐现有 `Rename requested`。

## 执行步骤（TDD）

1. [x] C focused 测试先行：`tests/neovifm_snapshot/session_rename.c`（独立文件——stic 单文件 800 行上限，session.c 已满）覆盖执行、destination-exists、invalid-name、unchanged、多 target、stale identity。
2. [x] core 实现至 C 测试通过（103 tests）。
3. [x] schema 更新 + `protocol-schema.test.ts` 断言新形状。
4. [x] 客户端 keymap/对话框/命令构造测试先行，实现至通过。
5. [x] 真实 session 集成：cw 改名 → 文件系统确认 → u undo 还原；重名目标 → destination-exists 失败可见。
6. [x] 更新 KEYMAP_MATRIX.md、CURRENT_STATE.md、protocol/README.md 与本计划。

## 状态：Complete（2026-09-05）

验收记录见 `progress.md`。实施中的两处计划外修正：
- `stic` 单文件 800 行上限 → C 测试独立为 `session_rename.c`（suite 源按通配符自动收集，无需改构建）。
- 集成测试抓到两处真实 bug 并修复：core_session.c 终态处理漏把 RENAME 记入 undo 栈（`undo-empty`）；rename 对话框改为提交时按路径重解析 identity，避免输入期间 watcher 刷新导致 stale 拒绝。

## 验收

- `env -u VIFM -u MYVIFMRC make -C tests neovifm_snapshot` 通过（串行）。
- `clients/tui`：test:coverage、typecheck、audit、test:integration 全绿。
- `git diff --check` 干净。
- 经典 `vifm` 行为不回归：rename 能力只加到 `neovifm-core-session`，不碰 classic UI 键位。

## 明确不做

- 批量 rename（选择集）、registers、永久删除、`:rename` 命令行形态。
- preview/open 对改名后文件的缓存失效策略（现有 watcher 刷新已覆盖）。
