# Progress

## 2026-09-05

- 选定切片：rename（`cw`/`cW`）——Alpha 0 唯一的文件操作能力缺口，无兜底路径。
- 调查确认最小路径：`nv_fs_move` 的 destination 是完整路径，rename = 同目录 + 新 basename 的 move，零新 fs 原语；undo 复用 `record_move_group`。
- 协议扩展：v3 schema 加 `rename` 命令（单 target + `name`），hello 新增 `file-rename-v1`（随 `file-actions-v1` 发布），action-task 事件枚举加 `rename`。
- core 实现：`NV_SESSION_RENAME` 贯穿 parse → prepare（valid_name + 单 target + 同名拒绝 + 替换 basename）→ execute（nv_fs_move）→ undo 记录 → 事件序列化。
- 客户端实现：`c` 前缀（cw/cW）、rename 对话框（预填、校验、cW 保留扩展名）、提交时按 path 重解析 identity。
- 集成测试抓到并修复两个真实 bug：
  1. `core_session.c` 终态处理漏把 RENAME 记入 undo 栈 → `undo-empty`（手动协议复现确认）。
  2. 对话框持有打开时的 identity，输入期间 watcher 刷新会导致 stale 拒绝 → 改为提交时重解析（与 yank/put 同一模式）。
- 验收（WSL Linux）：
  - `make -C tests neovifm_snapshot`：103 tests 全绿。
  - 串行 `make check`：全量回归 FAIL 0 / ERROR 0。
  - 单测 183 pass（51 app + keymap/schema/yank 等）；coverage functions 92.72% / lines 97.24%。
  - `bun audit` 首次 CI/本地校验报 `browserslist <= 4.28.6` 两个 high；用 `overrides` 固定到 4.28.9 后无漏洞。
  - integration 24 pass / 10 skip / 0 fail（新增 rename-session）。
  - `git diff --check` 干净。
  - typecheck 干净。
  - PR #5 远端验收：`CI / gate` run `33974625832` 全绿（Linux/macOS/Windows）；`Preview / gate` run `33974625827` 全绿（四平台包）。

## 教训

- stic 测试框架单文件 800 行上限（`STIC_MAX_LINES`），session.c 已满；新测试文件按通配符自动进入构建，无需改 Makefile。
- 带输入框的命令不要在打开时固化 identity——真实 session 里 watcher 随时可能刷新 workspace，提交时按 path 重解析才对（yank/put 已验证同一模式）。
- 手动协议复现（stdin 注入命令 + 观察 JSONL 输出）是定位 core/客户端分界问题的最快手段。
- 跨平台 integration 不要断言 statusbar 短文案；渲染时序和截断会制造 macOS flake，该行为由单元测试覆盖，integration 只验证 core/command/文件系统结果。
