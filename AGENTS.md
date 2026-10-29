# NeoVifm 项目协作约定

## 项目目标

NeoVifm 是一款以 Vifm 为成熟底座的新一代终端文件工作台，而不是主题包、插件合集或一次性重写。

产品方向：

- 保留 Vifm 的 Vim 语义、双栏工作流、命令系统、文件操作、undo、跨平台能力和 Lua 扩展基础。
- 吸收 btop 的高信息密度、自适应布局、实时状态和主题体验。
- 吸收 OpenCode 的会话、命令入口、任务生命周期、权限边界和可观察事件流。
- 吸收 Yazi 的非阻塞任务、预览/预加载、虚拟文件系统和并发插件思路。
- 吸收 lsd 的图标、颜色、树形布局和结构化文件字段表达。

开始工作前先阅读：

1. `docs/NEOVIFM_ARCHITECTURE.md`
2. `HACKING.md`
3. 与改动相关的源码和测试

## 当前阶段

当前代码以 Vifm 0.15 开发版为兼容基线。经典二进制、配置目录和大部分公开 API 暂时仍使用 `vifm`，不得为了品牌切换进行全仓机械重命名。

当前里程碑是 **Workbench Alpha 0 (unreleased)**：C core probe/session 通过 protocol v3 发布不可变 workspace、preview、task 和 resource 事件，TypeScript/Bun/OpenTUI 客户端负责交互与渲染。Hybrid M0 是已经完成的历史切片，不再代表当前功能范围。当前平台能力与已知问题以 `docs/CURRENT_STATE.md` 为准，架构决策见 `docs/adr/0001-hybrid-core-opentui.md`。

## 架构约束

- 演进式改造，不进行全量重写。已有成熟行为默认保持兼容。
- 新 UI 不得直接把可变的 `view_t` 当作长期数据模型；通过不可变 snapshot 读取 pane 状态。
- 新异步任务必须显式携带工作目录、pane、selection 和取消上下文，不得依赖进程级 `chdir()` 传递状态。
- 阻塞 I/O、预览解码、目录递归、外部命令和昂贵元数据计算不得占用 UI 主循环。
- 后台线程不得直接操作 ncurses 或可变 UI 状态；结果通过事件/回调回到主线程提交。
- 文件操作核心返回结构化结果和事件；状态栏、reload、redraw 由上层协调。
- 新扩展点必须经过版本化 capability 接口，不能让插件直接依赖内部结构体。
- 终端能力必须可降级：无 Nerd Font、无 truecolor、无图像协议时仍可完整操作。
- 文件系统、操作、undo、Vim 语义和 Lua 兼容层继续使用 C；新 TUI 使用 TypeScript、Bun、OpenTUI 和 SolidJS。
- C/TypeScript 边界只能传递版本化、不可变 DTO；客户端不得依赖 `view_t` 等内部结构。
- core 的 stdout 只传协议，stderr 只传诊断；客户端必须校验协议版本、字段类型、记录顺序和输入大小。
- 引入 Rust、C++、常驻 daemon、网络协议或替换当前 TUI runtime 需要单独 ADR 和可量化收益。
- 默认只借鉴第三方设计，不复制实现；导入第三方代码前必须完成许可证、来源、NOTICE 和 GPL 兼容审查。

## 实现纪律

- 复杂改动先写计划，明确兼容面、失败模式和验收路径。
- 功能和 bug 修复执行 TDD：先写失败测试，再写最小实现，最后重构。
- 新增或修改模块的覆盖率目标不低于 80%；不能用删除有效断言来提高通过率。
- 优先复用 `engine/`、`io/`、`ops`、`filelist`、`background` 和 Lua API 的现有能力。
- 保持函数聚焦、错误显式、输入在边界校验；不吞掉错误。
- 数据更新优先创建新值或 snapshot，避免跨模块共享可变对象。
- 不顺手清理无关代码，不覆盖用户已有的未跟踪研究和计划文件。

## macOS 基线

本机可靠构建与测试命令：

```bash
scripts/fix-timestamps
CFLAGS='-Wno-error=gnu-folding-constant' \
  ./configure --enable-developer --without-glib
env -u VIFM -u MYVIFMRC make check
```

约束：

- 测试阶段串行执行 `make check`，不要使用 `make -jN check`；多个 suite 会竞争共享路径。
- 清除 `VIFM` 和 `MYVIFMRC`，防止用户配置污染测试。
- Apple Clang 21 仅对 `-Wgnu-folding-constant` 取消 `-Werror`，其余开发警告继续视为错误。
- macOS 基线使用 `--without-glib`，避免 GIO 返回 UTI 导致 MIME 断言不稳定。

## 变更验收

每次交付至少完成：

1. 相关单元测试和集成测试。
2. 串行 `make check`。
3. `clients/tui` 的测试、覆盖率、typecheck 和依赖审计。
4. `git diff --check`。
5. 检查默认经典视图行为未回归。
6. 涉及 TUI 时验证窄终端、宽终端、无 Nerd Font 和低色彩降级。
7. 涉及文件操作时验证取消、失败、部分完成、undo 和双 pane 刷新。
8. 涉及第三方代码时验证许可证兼容并保留来源记录。

## Git 与上游

- 主线仓库是 `aumi314/neovifm`（本地 remote `fork`），其 `master` 是唯一日常集成分支；提交、CI 与预览包都以它为准。
- `origin` 指向 `Rex-Diego/neovifm`，只作为评审和向上回馈的通道，不作为主线。
- `upstream` 只读指向官方 `vifm/vifm`，push URL 必须保持禁用。
- 上游同步提交与 NeoVifm 功能提交保持可区分，尽量让底层修复可回馈上游。
- 新功能走短期分支 + PR + `CI / gate`，合并进主线后即结束该层依赖，不长期堆叠。
- 未经明确要求不提交、不推送、不改写历史。
