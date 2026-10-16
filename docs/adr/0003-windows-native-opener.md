# ADR 0003：Windows 原生默认 opener 边界

- 状态：Accepted
- 日期：2026-08-10
- 决策人：NeoVifm 项目

## 背景

`open-v1` 由 C core 校验目标并解析 Vifm association，再向 OpenTUI 发布结构化 argv；实际进程启动属于客户端。Windows 没有 `open`/`xdg-open` 等可安全表达为 argv 的系统命令，`explorer.exe <path>` 也不等于可靠地执行用户的默认文件关联。

## 决策

1. protocol v3、schema、DTO 和 `open-v1` capability 保持不变。
2. Windows core 在没有显式 association 时，以 `GetModuleFileNameW` 找到 core 所在目录，并发布相邻内部 helper 的绝对 argv：`neovifm-win-open.exe <target>`。
3. helper 使用 Unicode `wmain`、STA COM 和 `ShellExecuteExW` 的默认动作。它不调用 shell、不等待被打开的 GUI 应用退出，也不显示系统错误 UI。
4. helper 失败时通过有界 stderr 和非零退出码返回诊断；OpenTUI 最多读取 4 KiB，清理控制字符后显示。
5. 显式 `filetype`/`filextype` association 始终优先并继续直接执行结构化 argv。
6. `neovifm-win-open.exe` 是 Windows 内部运行时依赖，不是稳定用户 CLI；未来安装包必须与两个 core executable 放在同一目录。

## 结果

- Windows 10+ 的普通文件 Enter 可以走用户系统默认关联。
- core 继续拥有目标身份校验和 association 解析，TUI 继续只执行已解析 argv。
- 缺失 helper 或系统没有可用关联时明确失败，不退化为 `explorer.exe` 或 shell 字符串。
- watcher、安装器和公开 release 仍是独立工作。

## 被拒绝的方案

### `cmd.exe /c start` 或 PowerShell

拒绝原因：重新引入 shell quoting、控制字符和注入边界。

### `explorer.exe <path>`

拒绝原因：行为随目标类型变化，不能可靠表达“使用默认关联打开普通文件”。

### core session 直接调用 `ShellExecuteExW`

拒绝原因：破坏“core 解析、客户端启动”的现有 `open-v1` 职责，并让协议成功记录无法继续描述实际启动请求。

## 复审条件

- protocol 引入版本化的原生 launch intent，不再要求所有 opener 表达为 argv；
- Bun/OpenTUI 提供稳定、可审计且无需额外二进制的 Win32 ShellExecute 边界；
- Windows 安装布局不再保证 helper 与 core 位于同一目录。
