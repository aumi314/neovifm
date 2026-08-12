# Findings

## Runtime

- Bun 1.3.10 standalone 没有可依赖的 `Bun.isStandaloneExecutable` runtime marker；当前使用 executable basename 区分 `bun` 开发进程与打包后的 `neovifm`。
- OpenTUI/Solid 不能只依靠开发态 `bunfig.toml` preload 进行 standalone 编译。第一次真实 PTY 运行出现 orphan text 并落入 debug console；构建改为显式 `@opentui/solid/bun-plugin` 后界面正常。
- `--check` 使用 `resume: false`、`persist: false`，因此不读取或写入用户 session。

## Distribution

- Linux 本地包约 135 MiB，standalone TUI 和 release core 都只依赖 glibc/system loader。
- npm/OpenTUI notices 从 frozen `node_modules` 收集；Bun 1.3.10 的完整 license 包含 JavaScriptCore LGPL 修改源码与 relink 说明。
- Windows dependency scanner 从三个 PE root 递归解析 imports，系统 DLL 只记录，非系统 DLL 进入 `runtime/` 和 metadata。

## Boundaries

- Preview artifact 只保留 14 天，不创建 release 或 tag。
- 未签名包只记录 SmartScreen/Gatekeeper 风险，不降低系统安全策略。
- `protocol/README.md` 未修改，因为 CLI 和 archive 不改变 wire contract。
