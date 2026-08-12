# ADR 0004：便携预览包

## 状态

Accepted for Workbench Alpha 0 (unreleased), 2026-08-12.

## 背景

此前新 TUI 只能从源码目录运行：试用者需要 Bun、npm dependencies、C toolchain 和已编译 core。这个门槛使界面反馈与开发环境问题混在一起，也无法证明 runtime 能脱离 checkout 工作。

## 决策

- 使用固定 Bun 1.3.10 和 OpenTUI Solid build plugin，把 TUI 编译为本平台单文件 executable；目标机器不安装 Bun或 `node_modules`。
- standalone runtime 只从 executable sibling 查找默认 core；显式 `NEOVIFM_CORE_SESSION` 和兼容变量 `NEOVIFM_CORE_PROBE` 仍可覆盖。源码开发模式保留源码相对默认值。
- runtime 禁止从启动 cwd 自动加载 `.env` 与 `bunfig.toml`。
- core、Windows opener 和经递归依赖审计确认的 runtime DLL 与 TUI 一起分发；`neovifm-win-open.exe` 仍是内部依赖。
- 每个平台只在原生 GitHub runner 构建，不交叉编译 OpenTUI native runtime。
- archive 必须包含确定性的 build metadata、checksums、许可证/notices 和精确 source archive，不写构建时间。
- Windows/macOS preview 保持未签名。项目说明系统警告，但不提供绕过 SmartScreen 或 Gatekeeper 的脚本。

## 验证边界

archive 在带空格和中文的全新目录重新解压，并从包外 cwd 运行 `--version`、`--help`、`--check`。测试必须证明 sibling core 缺失时失败、恢复后成功；Unix 还通过真实 PTY 启动完整 TUI 并以 F10 退出。Windows 用真实 `--check` 覆盖 standalone OpenTUI runtime、sibling core 和 protocol v3；已有 CI 继续覆盖交互行为。

## 后果

便携 archive 可以用于有限试用，但不是安装器、release 或稳定分发承诺。用户仍自行管理 PATH、签名警告、session 文件和可选 preview/mount helper。公开 release 需要另行解决签名、安装位置、升级、卸载和配置迁移。
