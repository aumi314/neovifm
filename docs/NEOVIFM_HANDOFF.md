# NeoVifm 交接说明

## 当前基线

NeoVifm 当前阶段是 **Workbench Alpha 0 (unreleased)**。C core 负责文件系统、Vifm 语义、任务和 protocol v3；`clients/tui` 使用 TypeScript/Bun/OpenTUI/SolidJS 负责交互与渲染。经典 Vifm 继续作为兼容入口。

平台事实、capability 和已知问题统一见 [`CURRENT_STATE.md`](CURRENT_STATE.md)，不要从旧 planning 的阶段名称推断当前能力。

Git 远端约定：

- `origin`：`Rex-Diego/neovifm`。
- `upstream`：只读 `vifm/vifm`，push URL 必须为 `DISABLED`。
- 上游同步使用独立 merge commit，不与 NeoVifm 功能提交混合。

Phase A 建立了三平台构建与 CI 基线。Phase B1 已将 Vifm 上游同步到 `f5d60eaa`，并让 Windows focused C、真实 core/TUI session、Unicode preview 和 session persistence 获得远端证据。`CI / gate` 只有三平台全部成功才通过。

## 启动与验证

macOS：

```bash
scripts/fix-timestamps
CFLAGS='-Wno-error=gnu-folding-constant' \
  ./configure --enable-developer --without-glib
make -C src neovifm-core-session

cd clients/tui
bun install --frozen-lockfile
bun run dev
```

Linux 使用同一流程，但 configure 不需要 Apple Clang flag。完整验证命令见 `docs/CURRENT_STATE.md`。

Windows 使用 `scripts/appveyor/win/` 构建和运行 C tests；真实 core integration 命令见 `docs/CURRENT_STATE.md`。Windows 10+ 已能运行 Alpha 0 会话和安全文件操作，但仍没有 watcher、安装器和默认 opener。

## 当前功能

- 双 pane、pane tabs、排序、选择和当前目录文件名搜索。
- protocol v3 preview/task/resource event。
- 快速对面 pane 预览、task center 和结构化 open。
- POSIX/macOS 和 Windows 正常退出 session 保存和恢复。
- macOS、Linux 和 Windows 10+ 的 `file-actions-v1` 文件任务和 undo bridge。

## 已知边界

- Windows 10+ 使用 Win32 handle identity 和 extended-length Unicode path；move 不跨卷 copy-delete，delete 必须经过同目录隔离和 Recycle Bin。
- Linux move 使用 `renameat2(RENAME_NOREPLACE)`，delete 默认通过 `/usr/bin/gio trash`，测试可注入 `NEOVIFM_TRASH_EXECUTABLE`。
- Windows 没有 watcher 和默认 Win32 opener。
- ZIP/SSH 真实挂载依赖 helper，跨平台 E2E 未完成。
- Vifm marks、registers、完整 visual/history、批量重命名、compare/sync 和完整 background facade 未完成。
- 安装、发布、插件 SDK、agent session 和全仓品牌重命名不属于 Alpha 0 基线。

接手顺序：`AGENTS.md` → `docs/CURRENT_STATE.md` → `docs/NEOVIFM_ARCHITECTURE.md` → `protocol/README.md` → 当前 `.planning/` 计划。
