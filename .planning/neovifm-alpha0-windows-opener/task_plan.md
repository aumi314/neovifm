# NeoVifm Windows Default Opener Baseline

## 目标

在 B2b Windows actions 之上，让 Windows 10+ 通过系统默认关联打开普通文件，同时保持 Workbench Alpha 0 (unreleased) 和 protocol v3 不变。

## 范围

- [x] 个人 fork `master` 快进到导师 B1，并建立 B2b 内部 stacked PR。
- [x] Windows core 将默认 open 解析为相邻 `neovifm-win-open.exe` 的结构化 argv。
- [x] helper 使用 Unicode `ShellExecuteExW`，不经过 shell，也不等待 GUI 应用退出。
- [x] TUI 移除 `explorer.exe` fallback，并有界显示 opener stderr。
- [x] focused C、TUI unit 和真实 Windows 默认关联 integration。
- [x] C1 最终三平台 CI 和 `CI / gate` 全绿。

## 不在范围

- Windows/Linux watcher、安装器、release/tag。
- protocol/schema/DTO/capability 变化。
- 经典 `vifm.exe` 的 Windows launcher 重构。

## 验收

- Windows 中文、空格和超过 260 字符的文件路径到达临时注册的默认应用时保持完整。
- 显式 Vifm association 继续优先，缺失 helper 或 ShellExecute 失败时不退化为其他命令。
- Linux/macOS fallback、focused C、TUI coverage、integration 和串行 `make check` 不降级。
