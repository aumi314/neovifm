# Findings

## Open boundary

- `open-v1` 已把 resolver 结果固定为结构化 argv，core 不负责启动外部程序；直接在 session 中调用 `ShellExecuteExW` 会破坏现有职责。
- `explorer.exe <path>` 不能作为可靠的系统默认文件关联。Windows 使用相邻内部 helper 后，协议仍只传 argv，TUI 也不需要解释新的 DTO。
- 经典 Vifm 的 `running.c` 已证明 `ShellExecuteExW` 是现有平台做法，但 C1 使用独立 helper，避免改写经典 `vifm.exe` 的兼容路径。

## Build and tests

- `tests/Makefile` 会枚举 `src/neovifm/*.c` 作为测试链接对象；带 `wmain` 的 helper 必须像现有 `win_helper.c` 一样排除，否则 focused suite 会出现入口点链接错误。
- Windows helper 必须和 core session 位于同一目录。core 使用 `GetModuleFileNameW` 解析绝对 sibling path，安装器以后必须把三者作为一个运行时单元打包。
- 真实关联测试只创建唯一的 `HKCU\\Software\\Classes` 扩展名和 ProgID，并在 `finally` 中删除，不触碰用户已有文件关联。
- 第一轮远端测试在启动 helper 前发现 core 的 Windows UTF-8 canonical path 允许最后一段使用 `/`，而 Node 创建路径使用 `\\`。两者指向同一文件；断言改为 Windows path normalization 后再比较，没有修改 protocol identity 或产品路径。
- 第二轮证明 helper 能启动，但 `ShellExecuteExW` 对超过 260 字符且末段仍含 `/` 的路径返回 `ERROR_FILE_NOT_FOUND`。helper 因此在 UTF-16 边界统一分隔符，并嵌入 Windows 10 `longPathAware` manifest；不裁短路径，也不回退 shell。
- 第三轮中 `ShellExecuteExW` 已对同一长路径返回成功，但依赖继承环境变量的 Bun capture fixture 没有生成 marker。测试关联改为把 marker 与 target 都作为显式 argv 传给隐藏 PowerShell fixture；这不改变产品的无 shell 边界。
- 第四轮 run `31358472484` 验证修正后的真实默认关联路径：Windows opener integration `2 pass / 0 fail`，并且 Linux、macOS、Windows 与 `CI / gate` 全绿。
