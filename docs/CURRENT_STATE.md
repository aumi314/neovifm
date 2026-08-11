# NeoVifm 当前状态

最后核对：2026-08-12

## 阶段

NeoVifm 当前处于 **Workbench Alpha 0 (unreleased)**。

它已经越过只读 Hybrid M0：C core session 使用 protocol v3 发布双 pane、tab、排序、搜索、预览、任务和资源事件，OpenTUI 客户端负责交互与渲染。它仍不是可安装产品，没有 release、安装包或稳定兼容承诺。

经典 `vifm` 继续作为兼容入口和行为基准。当前阶段不进行品牌机械重命名，也不移除经典视图。

## 架构边界

- C 保留文件系统、Vifm 语义、undo、配置与 Lua 兼容基础。
- TypeScript/Bun/OpenTUI/SolidJS 负责新 TUI。
- 两端只通过版本化、不可变 JSONL DTO 通信。
- stdout 只传协议，stderr 只传诊断。
- OpenTUI 是建设中的产品主线，不代表已经达到经典 Vifm 的行为覆盖。

## Protocol v3 capability

`neovifm-core-session` 当前 hello 记录发布：

| Capability | macOS | Linux | Windows | 说明 |
|---|---:|---:|---:|---|
| `preview-session-v3` | 是 | 是 | 是 | 双 pane snapshot、命令确认、preview/task 事件 |
| `workspace-sort-v1` | 是 | 是 | 是 | core-owned 排序 |
| `pane-tabs-v1` | 是 | 是 | 是 | pane tab 新建、切换、关闭和顺序 |
| `open-v1` | 是 | 是 | 是 | 结构化 argv；Windows 通过内部 `neovifm-win-open.exe` 调用系统默认关联 |
| `resource-tasks-v1` | 是 | 是 | 是 | 协议入口存在；真实 mount 仍依赖平台 helper |
| `file-actions-v1` | 是 | 是 | 是（Windows 10+） | copy/move/mkdir/delete 与 copy/move/mkdir undo；Windows delete 依赖 Recycle Bin，不提供协议级 delete undo |

不要把“capability 被发布”写成“所有 helper 和 E2E 都已经完成”。ZIP/SSH 的真实挂载、取消和恢复仍需要单独平台验收。

## 平台基线

### macOS

- 当前功能和导师验收的主要基线。
- 发布 `file-actions-v1`，使用 kqueue watcher。
- kqueue 同时监听活动 tab 的目录与当前预览文件；外部目录项变化和已选文件内容变化都会发布 `trigger: "watch"` 并刷新预览。
- developer 构建使用 Apple Clang 的单项 warning 兼容 flag。
- 文件操作、undo、watcher 和 session 行为仍需 CI 持续证明。

### Linux

- 是干净构建、focused C、完整 C 回归和 TUI integration 的 CI 基线。
- protocol v3、导航、tabs、搜索、排序、预览、结构化 open 和 `file-actions-v1` 可构建和测试。
- copy/move/mkdir/delete 使用 parent-FD-relative、no-follow、no-overwrite 规则；move 使用 `renameat2(RENAME_NOREPLACE)`，delete 默认调用 `/usr/bin/gio trash`。
- undo 会刷新来源和目标 pane/tab；Trash helper 失败时保留原对象并恢复隔离文件。
- session 复用 Vifm 的 filesystem watcher，以 50 ms 轮询窗口合并活动 tab 的外部变化；inactive tab 在切回活动状态后重新绑定并读取最新目录。

### Windows

- NeoVifm core/TUI 的最低运行基线是 Windows 10；经典 `vifm.exe` 的兼容范围没有随之改写。低于 Windows 10 时 core 不发布 `file-actions-v1`。
- MinGW64 能构建经典 `vifm`、两个 NeoVifm core 和内部 `neovifm-win-open.exe`，并真实运行现有 Windows C tests 和 13 个 NeoVifm focused fixtures。
- 真实 core/TUI integration 已验证 Unicode 目录与文件名、无输入时的初始 preview、导航、pane 切换、tabs、搜索、排序和 refresh。
- session state 路径优先级是 `NEOVIFM_SESSION_STATE`、`%LOCALAPPDATA%\neovifm\session.json`、`%USERPROFILE%\AppData\Local\neovifm\session.json`。
- 状态文件和目录使用 Unicode Win32 路径；正常退出能创建、替换并跨进程恢复双 pane、tabs、排序和光标现场。
- Windows 10+ 发布 `file-actions-v1`。路径在 Win32 边界使用 UTF-16 extended-length absolute path；copy/move/mkdir/delete 使用 handle identity、no-follow 和 no-overwrite 复核。
- move 只允许同卷 handle rename，跨卷返回 `EXDEV`，不做 copy-delete；delete 先进入同目录私有隔离目录，再强制交给 Recycle Bin，失败时无覆盖恢复。
- copy、move、mkdir 可以 undo；delete 依赖 Recycle Bin 自身恢复，不新增协议级 delete undo。
- `open-v1` 优先使用显式 Vifm association；没有匹配项时，core 发布相邻 `neovifm-win-open.exe` 的绝对 argv，由 helper 通过 Unicode `ShellExecuteExW` 调用系统默认关联。
- helper 是内部运行时依赖，不是稳定 CLI；缺失或系统关联失败时明确报错，不退化到 `explorer.exe` 或 shell。
- session 使用 overlapped `ReadDirectoryChangesW` directory handle 监听活动 tab；支持中文和 extended-length 路径，外部目录变化会自动刷新 workspace 与当前预览。

## 当前能做什么

- 双 pane 导航、tab、选择、排序和当前目录文件名搜索。
- 不改变目标 pane 状态的快速预览。
- 文本、目录、图片、PDF、音频、视频和 archive 的有界预览或 metadata 降级；效果依赖可用 helper。
- 有界 Vifm `filetype`/`filextype`/`fileviewer` association 解析和结构化 open 结果。
- task center、action/resource task 事件、取消和历史展示。
- 正常退出时保存并恢复 workspace session；POSIX/macOS 和 Windows 均有自动化证据。

## 尚未完成

- 完整 Vifm keymap、marks、registers、visual、history 和命令语义。
- 文件操作与 Vifm `ops`/`background`/`undo` 的最终收口。
- ZIP/SSH 跨平台真实挂载 E2E。
- Kitty/Sixel 等原生图形协议、音频封面和完整媒体体验。
- 安装器、发布包、稳定配置迁移和公开 release。
- 插件 SDK 和 agent session。

## 验证入口

Unix developer baseline：

```bash
scripts/fix-timestamps
./configure --enable-developer --without-glib
make -C src neovifm-core-session
env -u VIFM -u MYVIFMRC make -C tests neovifm_snapshot

cd clients/tui
bun install --frozen-lockfile
bun run test:coverage
bun run typecheck
bun audit
bun run test:integration

cd ../..
env -u VIFM -u MYVIFMRC make check
git diff --check
```

macOS configure 时增加：

```bash
CFLAGS='-Wno-error=gnu-folding-constant' \
  ./configure --enable-developer --without-glib
```

Windows 本地基线在 MSYS2/MINGW64 中执行：

```bash
bash scripts/appveyor/win/build-deps
bash scripts/appveyor/win/build
bash scripts/appveyor/win/test
```

随后在 PowerShell 中执行真实 core integration：

```powershell
cd clients/tui
bun install --frozen-lockfile
$env:NEOVIFM_CORE_PROBE = (Resolve-Path '..\..\src\neovifm-core-probe.exe')
$env:NEOVIFM_CORE_SESSION = (Resolve-Path '..\..\src\neovifm-core-session.exe')
bun test ./integration/core-probe.test.tsx ./integration/windows-baseline.test.ts
bun test ./integration/windows-actions.test.ts
bun test ./integration/windows-opener.test.ts
bun test ./integration/cross-platform-watcher.test.ts
```

三平台最终门槛见 `.github/workflows/ci.yml` 的 `CI / gate`。B1 合并提交 `f7eccff37` 的导师仓库 run `31349657903` 全绿。B2a 重排后 run `31352874801` 全绿，并已创建 Ready PR #3。B2b 本地 Windows real-core 为 `8 pass / 1 cross-volume skip`，另以本机 C/D 两个真实卷单独验证跨卷 move 为 `1 pass`；最终 B2b run `31353418721` 全绿。C1 最终 run `31358472484` 的 Linux、macOS、Windows 与 `CI / gate` 全绿；Windows opener integration 为 `2 pass / 0 fail`，真实验证系统默认关联、Unicode、空格和超过 260 字符的路径。C2 实现 run `31513223577` 三平台与 gate 全绿；Windows watcher integration 为 `1 pass / 0 fail / 5 expects`，覆盖中文、extended-length path、外部目录变化、预览更新和导航后重新绑定。

## 文档优先级

1. `AGENTS.md`：协作和架构约束。
2. 本文件：当前阶段、平台能力和已知问题。
3. `protocol/README.md` 与 schema：协议契约。
4. `.planning/`：实施历史与未完成任务，不作为当前能力声明。

功能变化如果让本文件失真，必须在同一提交中更新；历史 planning 不回写成“早就完成”。
