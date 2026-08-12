# NeoVifm Portable Preview Baseline

## 目标

在 C2 watcher 基线上生成四个平台可下载、解压即用的 Workbench Alpha 0 预览 archive，不改变 protocol、schema、DTO 或 capability。

## 范围

- [x] standalone CLI、sibling core 和 `--check`。
- [x] Bun 1.3.10 单文件 TUI，关闭 runtime dotenv/bunfig 自动加载。
- [x] 固定 archive 结构、source archive、checksums、许可证和依赖 metadata。
- [x] Windows PE recursion audit，Unix ldd/otool audit。
- [x] 中文/空格目录解压验证、core 缺失反向验证和 Unix PTY/F10 smoke。
- [x] Windows x64、Linux x64、macOS arm64、macOS x64 原生 jobs 与 `Preview / gate`。
- [ ] 个人 fork 远端 `CI / gate` 与 `Preview / gate` 最终证据。

## 不在范围

- tag、GitHub Release、安装器、签名、notarization、自动更新。
- watcher、marks、插件、资源挂载扩展。
- protocol/schema/DTO/capability 变化。

## 验收

- 四个平台 archive 不包含 Bun、`node_modules`、classic `vifm` 或 `neovifm-core-probe`。
- 从包外 cwd 运行 help/version/check，无源码树依赖。
- 许可证、source archive、SHA256 和动态依赖审计通过。
- 原有三平台 CI 不降级，工作树干净。
