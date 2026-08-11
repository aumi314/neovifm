# Findings

## Watcher boundary

- session watcher 只持有左右 pane 当前活动 tab；inactive tab 不常驻占用系统 watcher，切换后依据不可变 snapshot 重新绑定。
- watcher 事件继续使用现有 `trigger: "watch"` 和已确认 `command_sequence`，因此不需要协议或 schema 变化。
- action queue 执行期间不消费 watcher refresh；action terminal 已负责刷新 pane，随后 watcher 再同步新 snapshot。

## Platform evidence

- Linux 的既有 `fswatch_nix` 能覆盖目录项和文件内容变化，50 ms poll 只负责从非阻塞 watcher 汇总事件。
- 第一轮 run `31510042822` 证明 macOS 目录 kqueue 不保证在现有文件内容写入时通知目录 vnode；补充当前预览文件 vnode 后，第二轮 macOS 全绿。
- 同一轮 Windows classic C tests 暴露 short path 经过 extended prefix 后的 change-notification 回归；普通路径继续保持 classic 表达。
- 第二轮 run `31510719033` 证明 Windows classic watcher 回归已修复，但长路径 preview worker 仍缺少 extended prefix。
- 第三轮 run `31511554012` 证明长路径初始 preview 已修复；失败进一步收敛到 `FindFirstChangeNotificationW` 对 extended path 的通知不可靠。
- Windows watcher 最终改为 overlapped `ReadDirectoryChangesW` directory handle，同时保留 no-follow、Unicode 和长路径边界。

## Verification

- Linux focused C：`101 tests / 9950 checks`。
- Linux real integration：`22 pass / 10 platform skip`，watcher case `1 pass / 5 expects`。
- TUI coverage：`147 pass / 0 fail`，functions `92.23%`，lines `97.12%`；typecheck 与 audit 通过。
- `session_watcher.c`、`fswatch_win.c` 与 Windows preview boundary 通过 MinGW64 `-Wall -Werror` 交叉编译。
- 最终实现 run `31515109163` 的 Linux、macOS、Windows 与 `CI / gate` 全绿；Windows watcher integration 为 `1 pass / 0 fail / 5 expects`。
