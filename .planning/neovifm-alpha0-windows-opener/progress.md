# Progress

## 2026-08-10

- 个人 fork `master` fast-forward 到导师 `origin/master@f7eccff37`；B2b 建立个人 fork Ready PR #1，没有修改导师仓库。
- 从 B2b `8b610b933` 创建并推送 `codex/neovifm-alpha0-windows-opener`。
- TDD 红灯确认 Windows resolver 返回 `unsupported-platform`、TUI 仍调用 `explorer.exe` 且丢失 stderr；实现后对应测试转绿。
- 新增 Windows-only `neovifm-win-open.exe`；使用 Unicode `wmain`、STA COM 和 `ShellExecuteExW`，不经过 shell。
- Linux 干净 clone focused C：`97 tests / 9880 checks` 全绿。
- TUI：`146 pass`，函数覆盖率 `92.23%`、行覆盖率 `97.02%`，typecheck 和 audit 通过。
- `win_open_helper.c` 与 `open_resolver.c` 通过 MinGW64 `-Wall -Werror` 交叉编译。
- C1 实现/测试远端 run：`31357048648`。
- 第一轮 run 的 Windows opener integration 因测试直接比较 `/` 与 `\\` 路径分隔符失败；helper invalid/missing-target 测试已通过，产品代码未失败。
- 第二轮 run `31357478237` 进入真实 helper 后，长路径被 `ShellExecuteExW` 以 error 2 拒绝；补入 wide separator normalization 与 `longPathAware` manifest。
- 第三轮 run `31357977382` 中 ShellExecute 已成功；测试 capture handler 因环境依赖没有写 marker，改为全参数 fixture 后重跑。

## 2026-08-11

- 第四轮 run `31358472484` 全绿：Linux、macOS、Windows 和 `CI / gate` 全部通过。
- Windows TUI coverage 为 `147 pass / 0 fail`；Windows baseline integration `5 pass / 0 fail`，actions integration `4 pass / 0 fail`，opener integration `2 pass / 0 fail`。
- opener integration 使用唯一的临时 `HKCU\\Software\\Classes` 关联验证中文、空格和超过 260 字符的目标路径，并验证 helper 对无效或不存在目标返回诊断。
