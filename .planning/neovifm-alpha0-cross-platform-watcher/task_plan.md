# NeoVifm Cross-platform Watcher Baseline

## 目标

在 C1 Windows opener 之上，让 macOS、Linux 和 Windows 的 Workbench Alpha 0 session 都能自动刷新活动 tab 与当前预览，同时保持 protocol v3、schema、DTO 和 capability 不变。

## 范围

- [x] macOS 保留 kqueue，并补充当前预览文件 vnode 监听。
- [x] Linux 复用 Vifm filesystem watcher。
- [x] Windows 使用 Unicode overlapped directory handle，覆盖 extended-length path。
- [x] watcher 只绑定两个 pane 的活动 tab，导航和 tab 激活后重新绑定。
- [x] 50 ms polling/coalescing，action queue 忙碌时暂缓刷新。
- [x] focused C 与 real-core integration 覆盖创建、修改、重命名、删除、preview 和 navigation rebind。
- [x] 最终三平台 CI 和 `CI / gate` 全绿。

## 不在范围

- protocol/schema/DTO/capability 变化。
- inactive tab 常驻 watcher、安装器、资源挂载扩展、release/tag。
- 经典 `vifm.exe` watcher 语义重构。

## 验收

- 外部文件变化不需要先按键或手动 refresh。
- watcher workspace 保留最后确认的 `command_sequence`，并使用既有 `trigger: "watch"`。
- 中文和 Windows 超过 260 字符的目录能监听并预览。
- watcher 失败只影响对应 pane，session stdin 和另一 pane 继续运行。
- Linux、macOS、Windows 完整回归及覆盖率门槛不降级。
