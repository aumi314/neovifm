# Progress

## 2026-08-12

- 从 C2 `7e3e65d96` 创建并推送 `codex/neovifm-alpha0-portable-preview`。
- TDD 增加 CLI parsing、override precedence、standalone path 和 `--check` timeout/failure tests；TUI suite 为 `158 pass / 0 fail`，functions `91.95%`，lines `96.97%`。
- 真实 Linux standalone 首轮证明旧 runtime marker 无效，修为 executable basename detection。
- 完成 deterministic staging、source/license/notices/SHA256、依赖 metadata 和 archive verifier。
- 真实 PTY 首轮发现 standalone 缺少 Solid compile transform；显式接入 OpenTUI build plugin 后，中文/空格目录的 help/version/check、core 缺失/恢复与 F10 退出全部通过。
- `actionlint` 通过；`macos-15-intel` 是 GitHub 当前官方 runner label，但 actionlint 1.7.7 的内置 label 表滞后，因此只忽略该条 unknown-label 诊断。
## 2026-08-21

- 将 C3 推送到个人 fork，并创建 Ready PR `aumi314/neovifm#4`，base 为 C2 watcher 分支；未向导师仓库创建新的堆叠 PR。
- 第一轮远端 Preview 暴露 Windows MSYS2 工具路径假设和 macOS Intel PTY 退出问题；后续证明裸 PTY 无法稳定模拟 OpenTUI 终端能力协商，改用 tmux 等待真实 workspace 后发送 F10。
- Windows 远端和本机 Bun 1.3.10 证明 compiled entry 没有触发 `import.meta.main`；加入 standalone compile flag 后，普通路径与中文路径的 `--version` 均输出正确并退出 0。
- Windows PE 审计改为从 MSYS2 动态取得 `objdump`/MinGW 路径，source tar 校验改为 Unicode cwd 加 ASCII basename。
- 实现验证提交 `b035f42d2`：CI run `32484040086` 三平台与 `CI / gate` 全绿；Preview run `32484040073` 四个平台与 `Preview / gate` 全绿。
- 最终 TUI coverage 为 `159 pass / 0 fail`，functions `92.20%`、lines `96.97%`；Linux real-core integration 为 `22 pass / 0 fail`。
- 四个 artifact 对应 merge source `f891c8dedde8`，分别为 Windows x64、Linux x64、macOS arm64 和 macOS x64，保留 14 天。
