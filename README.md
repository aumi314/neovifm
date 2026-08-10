# NeoVifm

NeoVifm 是一款正在建设中的终端文件工作台，以 Vifm 的 Vim 语义、双栏工作流和成熟文件操作为基础，吸收 btop 的高信息密度 TUI、OpenCode 的会话与命令工作流、Yazi 的异步与预览体系，以及 lsd 的结构化目录表达。

本项目是独立社区 fork，与 Vifm、btop、OpenCode、Yazi、lsd 的维护团队没有隶属或官方合作关系。

## 当前状态

NeoVifm 的主线入口是 OpenTUI，当前阶段是 **Workbench Alpha 0 (unreleased)**，尚未发布可安装版本。这里的 Hybrid 是“Vifm C core 负责文件系统、操作和兼容语义，OpenTUI 负责渲染与交互”的架构边界，不再是只读 M0 的阶段名称；经典 `vifm` 继续作为兼容入口。

- 当前代码基线：Vifm 0.15 开发版。
- 经典二进制、配置目录和 Lua API 仍使用 `vifm`，用于保持兼容。
- `neovifm-core-probe` 发布无用户配置污染的目录快照；`neovifm-core-session` 为 OpenTUI 提供版本化 JSONL 会话协议。
- `clients/tui` 已接入双 pane、pane tab、Vifm 风格键位基础、搜索、排序、任务中心、资源任务和 F3/Space 文本与媒体预览。
- 当前仍未完成：完整 Vifm filetype/fileviewer/running 语义、真实 ZIP/SSH 挂载 E2E、图形图片/PDF/视频/音频渲染和完整低色彩/终端尺寸验收。
- `file-actions-v1` 当前在 macOS、Linux 和 Windows 10+ 提供；macOS 使用 kqueue watcher，Linux/Windows session 暂无 watcher。Windows 文件操作使用 Win32 handle identity、no-overwrite/no-follow、同卷 move 和 Recycle Bin delete；默认文件关联通过内部 `neovifm-win-open.exe` 和 Unicode `ShellExecuteExW` 打开。
- 经典 Vifm 默认行为不被替换；OpenTUI 缺少 capability 时必须降级为可读的文本或结构化错误。
- 当前平台和能力事实见 [CURRENT_STATE](docs/CURRENT_STATE.md)，混合架构见 [ADR 0001](docs/adr/0001-hybrid-core-opentui.md)，Windows 安全操作与 opener 见 [ADR 0002](docs/adr/0002-windows-safe-file-actions.md) 和 [ADR 0003](docs/adr/0003-windows-native-opener.md)，协议见 [NeoVifm Core Protocol](protocol/README.md)。

## 产品方向

- **Vifm**：Vim 模式、映射、命令、寄存器、双栏、文件操作、undo 和 Lua。
- **btop**：紧凑、清晰、自适应的终端信息设计。
- **OpenCode**：任务/会话生命周期、统一命令入口、事件流和权限边界。
- **Yazi**：非阻塞任务、可取消预览、预加载、VFS 和终端图像适配。
- **lsd**：图标、颜色、树和结构化文件字段。

详细边界和阶段计划见 [NeoVifm 产品与架构基线](docs/NEOVIFM_ARCHITECTURE.md)。参与开发前请阅读 [项目协作约定](AGENTS.md) 和 [Vifm 的开发说明](HACKING.md)。

## 安装依赖

### 必需依赖

Unix core 构建需要 Autotools 与 curses 开发库，OpenTUI 需要 Bun 1.3 或更高版本。macOS 已安装依赖时无需重复执行：

```bash
brew install autoconf automake bun
```

确认环境：

```bash
autoconf --version | head -1
automake --version | head -1
bun --version
```

### 可选预览与挂载 helper

这些工具不是 core 编译依赖；缺少它们时，NeoVifm 使用内建的文本、metadata 或结构化错误降级。命令会使用绝对路径和受限参数，不会把 shell 命令拼进协议。

| helper | 用途 | macOS 安装方式 |
| --- | --- | --- |
| `unzip`、`bsdtar` | ZIP/tar 清单预览 | 系统自带 |
| `sshfs`、`umount` | F9 SSH 目录生命周期 | `brew install sshfs` 或 macFUSE 发行包；`umount` 系统自带 |
| `pdftoppm` | PDF 首页栅格化，随后交给 `chafa` 做终端可读预览 | `brew install poppler` |
| `pdftotext` | PDF 文本降级预览 | `brew install poppler` |
| `mutool` | PDF/文档诊断与后续页面渲染 | `brew install mupdf` |
| `ffmpeg` | 视频首帧提取，随后交给 `chafa` 做终端可读预览 | `brew install ffmpeg` |
| `ffprobe` | 媒体探测与调试 | `brew install ffmpeg` |
| `chafa` | 图像 Unicode block symbols 降级预览（不传递 ANSI/Kitty/Sixel 原始序列） | `brew install chafa` |
| `archivemount` 或 `fuse-zip` | ZIP 作为目录挂载 | macOS 需要 macFUSE/FUSE-T；Homebrew core 中这两个 formula 当前标记为 Linux-only，不能直接安装 |

本项目会按 `/usr/local/bin`、`/opt/homebrew/bin` 和系统路径探测 helper。macOS 上要启用真实 archive mount，需要先安装一个 FUSE runtime。任选其一：

如需测试或指定其他安装位置，可将以下变量设为绝对路径；变量只改变 helper 选择，不会改变参数校验或通过 shell 执行：

```text
NEOVIFM_CHAFA_EXECUTABLE
NEOVIFM_PDF_RENDER_EXECUTABLE
NEOVIFM_PDF_TEXT_EXECUTABLE
NEOVIFM_FFMPEG_EXECUTABLE
NEOVIFM_FFPROBE_EXECUTABLE
```

```bash
brew install --cask macfuse
# 或：brew install --cask fuse-t
```

macFUSE 安装需要管理员密码，并可能需要在“系统设置 -> 隐私与安全性”批准系统扩展；FUSE-T 不使用 kext，但安装器仍需要管理员权限。然后可构建当前上游 `archivemount-ng`（Homebrew formula 在 macOS 上不能直接用）：

```bash
brew install libarchive pkgconf
archive_build_dir=$(mktemp -d /tmp/neovifm-archivemount.XXXXXX)
curl -fL 'https://git.sr.ht/~nabijaczleweli/archivemount-ng/archive/1b.tar.gz' \
  -o "$archive_build_dir/source.tar.gz"
tar -xzf "$archive_build_dir/source.tar.gz" -C "$archive_build_dir"
cd "$archive_build_dir/archivemount-ng-1b"
PKG_CONFIG_PATH="$(brew --prefix libarchive)/lib/pkgconfig:$(brew --prefix)/lib/pkgconfig" \
  make FUSES=fuse VERSION=1b
install -m 755 archivemount "$(brew --prefix)/bin/archivemount"
archivemount --version
```

仅安装 `unzip` 只能提供清单预览，不能让 ZIP 变成可进入的目录。没有管理员密码、系统扩展未批准或 helper 不存在时，OpenTUI 会返回结构化的 mount-unavailable 错误，不会把 ZIP 交给桌面 opener。

检查当前环境：

```bash
for tool in sshfs umount unzip bsdtar pdftoppm pdftotext mutool ffmpeg ffprobe chafa archivemount fuse-zip; do
  printf '%-12s' "$tool"
  command -v "$tool" || printf '%s' 'missing'
  printf '\n'
done
```

### 卸载可选依赖

只卸载你通过 Homebrew 安装且不再被其他项目使用的 helper。系统自带的 `/usr/bin/unzip`、`/usr/bin/bsdtar` 和 `/sbin/umount` 不要删除。

```bash
brew uninstall chafa
brew uninstall sshfs
brew uninstall poppler       # pdftotext
brew uninstall mupdf         # mutool
brew uninstall ffmpeg        # ffprobe
brew uninstall --cask macfuse
brew uninstall --cask fuse-t
```

`archivemount`/`fuse-zip` 若是手工构建，请只删除当时使用的明确文件，例如：

```bash
rm -f "$(brew --prefix)/bin/archivemount"
```

不要递归删除整个 `/usr/local`。卸载 macFUSE/FUSE-T 前先退出 NeoVifm 并卸载所有仍在使用的 FUSE 挂载。

`autoconf`、`automake`、`bun`、`libarchive` 和 `pkgconf` 通常会被其他项目共用，除非确认没有其他使用者，否则不要卸载它们。

## 原型运行

NeoVifm core 沿用 Vifm 的 Autotools 构建。通用 C 依赖和平台说明见 [INSTALL](INSTALL)。

构建 core probe：

```bash
scripts/fix-timestamps
CFLAGS='-Wno-error=gnu-folding-constant' \
  ./configure --enable-developer --without-glib
make -C src neovifm-core-probe
```

启动新 TUI：

```bash
cd clients/tui
bun install --frozen-lockfile
bun run dev ../.. /tmp
```

前两个路径参数分别是左、右 pane；省略右路径时会复制左路径。`q` 或 `Ctrl-C` 退出，`Tab` 切换当前 pane。也可通过 `NEOVIFM_CORE_PROBE=/path/to/probe` 指定 core probe。

不传路径启动时，OpenTUI 会在正常退出后恢复上次现场：包括左右 pane 路径、每个 pane 的 tab 顺序与活动 tab、活动 pane 和各目录中的光标目标。在 POSIX 环境，状态默认保存到 `$XDG_STATE_HOME/neovifm/session.json`，未设置时使用 `~/.local/state/neovifm/session.json`。Windows 默认使用 `%LOCALAPPDATA%\neovifm\session.json`，缺失时回退到 `%USERPROFILE%\AppData\Local\neovifm\session.json`。所有平台都可用 `NEOVIFM_SESSION_STATE` 覆盖；Windows 中文用户名和目录、已有状态替换及跨进程恢复已经过真实 core/TUI integration 验证。已挂载的 ZIP/SSH 资源不会被伪造为普通本地目录恢复。

方向键遵循 Vifm 直觉：`↑/↓/←/→` 分别等同于 `k/j/h/l`；排序切换保留在排序控件和命令入口，不再占用左右方向键。

## 测试

```bash
env -u VIFM -u MYVIFMRC make -C tests neovifm_snapshot

cd clients/tui
bun run test:coverage
bun run typecheck
bun audit
bun run test:integration

cd ../..
env -u VIFM -u MYVIFMRC make check
```

测试必须串行执行。现有 suite 会使用共享相对路径，`make -jN check` 存在竞态。

Windows 的 MSYS2/MINGW64 构建、focused C 和真实 core integration 命令见 [CURRENT_STATE](docs/CURRENT_STATE.md)。

Windows 构建会同时生成内部 `neovifm-win-open.exe`。它必须与 `neovifm-core-session.exe` 位于同一目录，不是面向用户的稳定命令，也不能单独代替未来安装包。

## 上游同步

- `origin`：NeoVifm fork。
- `upstream`：只读的 [vifm/vifm](https://github.com/vifm/vifm)。
- Vifm 的缺陷和 NeoVifm 的缺陷必须分别提交到对应项目；不要把 NeoVifm 功能请求提交给 Vifm 上游。

## 许可证与来源

Vifm 基线按 GNU GPL v2 或更高版本分发，详见 [COPYING](COPYING) 和现有源码头。

NeoVifm 默认只借鉴其他项目的公开设计，不复制实现。任何第三方代码导入都必须先记录来源、许可证和 NOTICE 要求；特别是 Apache-2.0 代码不能在未完成兼容审查时直接合入。若未来导入 Apache-2.0 实现，组合发行需要采用兼容的 GPLv3 路径并保留相应声明。

Vifm 的原作者和贡献者信息见 [AUTHORS](AUTHORS) 与 [THANKS](THANKS)。
