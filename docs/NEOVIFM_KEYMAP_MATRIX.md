# NeoVifm Keymap Matrix

状态：Phase 1 基线矩阵
日期：2026-07-30

本文档只覆盖当前 Hybrid TUI 的键盘映射边界，并用 classic Vifm 与 ViATc/Total Commander 作为语义参照。目标不是复制 ViATc 的 AutoHotkey 实现，而是建立可验收的交互矩阵。

## 状态定义

- `supported`：当前 `clients/tui/src/keymap.ts` 已映射，且已有单元或集成测试覆盖。
- `mapped`：当前已映射，但测试覆盖仍应补强或需要和 core capability 联动验收。
- `conflict`：当前有刻意偏离 classic Vifm/TC 语义的设计，需要在计划中显式承认。
- `deferred`：classic Vifm 或 TC/ViATc 有成熟语义，但当前 Hybrid TUI 未映射。
- `not-applicable`：ViATc/TC 的窗口壳层语义，不适合直接迁入 NeoVifm。

## 设计边界

- 主导航以 Vifm 语义为准：`hjkl`、`gg/G`、`Ctrl-W`、`gt/gT` 是主线。
- `F3-F10` 保留 Total Commander 功能键语义，作为 Hybrid 的共享 action dispatcher。
- ViATc 只提供“动作清单”和“快捷键编排”参考，不作为实现来源；其 AHK 窗口类匹配、发送键、宏系统不纳入本矩阵的实现目标。

## Normal / Workspace

| 按键/序列 | 模式 | 目标动作 | 当前状态 | 来源文件 | 测试建议 |
| --- | --- | --- | --- | --- | --- |
| `j` / `Down` | Normal | 光标下移一项 | `supported` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:127), [`src/modes/normal.c`](/Users/rex/soft/neovifm/src/modes/normal.c:381) | 保持 `keymap.test.ts` 与真实 session 的 cursor 断言 |
| `k` / `Up` | Normal | 光标上移一项 | `supported` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:128), [`src/modes/normal.c`](/Users/rex/soft/neovifm/src/modes/normal.c:382) | 同上，覆盖顶部边界 |
| `h` / `Backspace` | Normal | 返回父目录 | `supported` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:126), [`src/modes/normal.c`](/Users/rex/soft/neovifm/src/modes/normal.c:379) | 用真实目录进入/返回验证 cwd 变化 |
| `l` / `Enter` | Normal | 进入目录；对文件走 viewer 入口 | `conflict` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:129), [`clients/tui/src/app.tsx`](/Users/rex/soft/neovifm/clients/tui/src/app.tsx:829), [`src/modes/normal.c`](/Users/rex/soft/neovifm/src/modes/normal.c:383) | 分目录/文件两组验收；文件应打开 F3 viewer，而非外部执行 |
| `gg` | Normal | 跳到第一项 | `supported` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:53), [`src/modes/normal.c`](/Users/rex/soft/neovifm/src/modes/normal.c:362) | 已有真实 session 测试，继续保留 |
| `G` / `End` | Normal | 跳到最后一项 | `supported` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:114), [`src/modes/normal.c`](/Users/rex/soft/neovifm/src/modes/normal.c:334) | 覆盖空列表与长列表 |
| `Home` | Normal | 跳到第一项 | `supported` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:115) | 保持单测 |
| `Space` | Normal | 在对面 pane 临时预览当前项，不改变目标 pane 状态 | `supported` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:132), [`clients/tui/src/app.tsx`](/Users/rex/soft/neovifm/clients/tui/src/app.tsx:1380) | 验证 source/target identity、Esc/Tab 关闭和窄终端 F3 fallback |
| `Tab` | Normal | 切换活动 pane | `supported` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:136), [`src/modes/normal.c`](/Users/rex/soft/neovifm/src/modes/normal.c:318) | 真实 session 验证 `active_pane` 切换 |
| `/` | Normal | 正向按名称搜索并打开查询输入 | `supported` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:122), [`src/neovifm/workspace_session.c`](/Users/rex/soft/neovifm/src/neovifm/workspace_session.c:1100) | 覆盖大小写、不命中、换向和查询长度边界 |
| `?` | Normal | 反向按名称搜索并打开查询输入 | `supported` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:123), [`src/neovifm/workspace_session.c`](/Users/rex/soft/neovifm/src/neovifm/workspace_session.c:1100) | 与 `/` 共用查询对话框，验证反向首个匹配 |
| `n` / `N` | Normal | 沿最近一次查询向前/向后循环 | `supported` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:119), [`src/neovifm/workspace_session.c`](/Users/rex/soft/neovifm/src/neovifm/workspace_session.c:1100) | 验证 wraparound、无历史查询和 pane 隔离 |
| `t` | Normal | 切换当前条目选中态 | `mapped` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:133), [`src/modes/normal.c`](/Users/rex/soft/neovifm/src/modes/normal.c:392) | 补一条键盘选择集成测试，和鼠标右键选择对齐 |
| `Ctrl-N` | Normal | 下移一项 | `supported` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:87), [`src/modes/view.c`](/Users/rex/soft/neovifm/src/modes/view.c:228) | 单测已覆盖，建议补 session 级验证 |
| `Ctrl-P` | Normal | 上移一项 | `supported` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:88), [`src/modes/view.c`](/Users/rex/soft/neovifm/src/modes/view.c:229) | 同上 |
| `Ctrl-L` | Normal | 刷新 workspace | `supported` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:86) | 增加 command 发送断言或 session 回包断言 |
| `Left` | Normal | 返回父目录 | `supported` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:130) | 与 `h` 共用目录返回和 cursor 恢复路径 |
| `Right` | Normal | 进入目录；对文件走 viewer 入口 | `supported` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:131) | 与 `l` 共用目录/文件分流 |

## Prefix / Pane / Tabs

| 按键/序列 | 模式 | 目标动作 | 当前状态 | 来源文件 | 测试建议 |
| --- | --- | --- | --- | --- | --- |
| `Ctrl-W w` / `Ctrl-W p` | Normal | 切换 pane | `supported` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:75), [`src/modes/normal.c`](/Users/rex/soft/neovifm/src/modes/normal.c:301) | 补充 prefix 真实输入测试 |
| `Ctrl-W h` | Normal | 聚焦左 pane | `supported` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:76), [`src/modes/normal.c`](/Users/rex/soft/neovifm/src/modes/normal.c:284) | 验证在右 pane 时回到左侧 |
| `Ctrl-W l` | Normal | 聚焦右 pane | `supported` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:77), [`src/modes/normal.c`](/Users/rex/soft/neovifm/src/modes/normal.c:290) | 验证在左 pane 时切到右侧 |
| `Ctrl-W j` / `Ctrl-W k` | Normal | 上下 pane 导航 | `deferred` | [`src/modes/normal.c`](/Users/rex/soft/neovifm/src/modes/normal.c:286), [`src/modes/normal.c`](/Users/rex/soft/neovifm/src/modes/normal.c:288) | 等多行/纵向布局进入主线后补映射 |
| `gt` | Normal | 下一个标签或第 n 个标签 | `supported` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:48), [`src/modes/normal.c`](/Users/rex/soft/neovifm/src/modes/normal.c:368) | 保持已有 tab 集成测试 |
| `gT` | Normal | 上一个标签或前 n 个标签 | `supported` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:44), [`src/modes/normal.c`](/Users/rex/soft/neovifm/src/modes/normal.c:369) | 保持 count 反向跳转测试 |
| `2gt` 等数字前缀 | Normal | 直接激活第 n 个标签 | `supported` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:48), [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:94) | 已有 `tab-index` 单测和集成测试 |
| `q` 前缀 | Normal | classic Vifm 的 `q:` / `q/` / `q?` / `q=` 编辑入口 | `deferred` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:61), [`src/modes/normal.c`](/Users/rex/soft/neovifm/src/modes/normal.c:388) | 后续若引入命令行/搜索编辑器，再补映射与测试 |
| `ZZ` / `ZQ` | Normal | 退出 | `supported` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:66), [`src/modes/normal.c`](/Users/rex/soft/neovifm/src/modes/normal.c:342) | 保持取消路径测试，区分 `quit` 与进程 EOF |

## Function Keys / Total Commander 对齐

## File Action Aliases

这些别名只复用现有的 F5/F6/F8 action dispatcher，因此不会在客户端重新实现一套文件操作语义；删除仍由同一个确认对话框和 core snapshot 校验保护。只有 core hello 发布 `file-actions-v1` 时，F5--F8 和这些别名才可用；不具备 capability 的平台不会显示成可执行动作。

| 按键/序列 | 模式 | 目标动作 | 当前状态 | 来源文件 | 测试建议 |
| --- | --- | --- | --- | --- | --- |
| `p` | Normal | 复制当前项/选择集到另一 pane | `mapped` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:117), [`src/modes/normal.c`](/Users/rex/soft/neovifm/src/modes/normal.c:398) | `keymap.test.ts` 别名映射；继续用 F5 的真实 session 验证 |
| `P` | Normal | 移动当前项/选择集到另一 pane | `mapped` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:117), [`src/modes/normal.c`](/Users/rex/soft/neovifm/src/modes/normal.c:399) | `keymap.test.ts` 别名映射；继续用 F6 的真实 session 验证 |
| `d` / `D` | Normal | 删除当前项/选择集 | `mapped` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:118), [`src/modes/normal.c`](/Users/rex/soft/neovifm/src/modes/normal.c:406) | `keymap.test.ts` 别名映射；继续复用 F8 确认/取消/失败路径 |

| 按键/序列 | 模式 | 目标动作 | 当前状态 | 来源文件 | 测试建议 |
| --- | --- | --- | --- | --- | --- |
| `F3` | Normal | 查看当前项 | `supported` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:104), [`clients/tui/src/app.tsx`](/Users/rex/soft/neovifm/clients/tui/src/app.tsx:651), [`/Users/rex/soft/_refs/neovifm/viatc/actions/TCCOMMAND.ahk`](/Users/rex/soft/_refs/neovifm/viatc/actions/TCCOMMAND.ahk:74) | 保持 viewer 打开/关闭、stale preview、鼠标 fallback 测试 |
| `F4` | Normal | 编辑当前文件 | `supported` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:105), [`clients/tui/src/app.tsx`](/Users/rex/soft/neovifm/clients/tui/src/app.tsx:656), [`/Users/rex/soft/_refs/neovifm/viatc/actions/TCCOMMAND.ahk`](/Users/rex/soft/_refs/neovifm/viatc/actions/TCCOMMAND.ahk:76) | 保持 UTF-8/非 UTF-8 路径分支测试 |
| `F5` | Normal | 复制到另一 pane | `supported` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:106), [`clients/tui/src/app.tsx`](/Users/rex/soft/neovifm/clients/tui/src/app.tsx:723), [`/Users/rex/soft/_refs/neovifm/viatc/actions/TCCOMMAND.ahk`](/Users/rex/soft/_refs/neovifm/viatc/actions/TCCOMMAND.ahk:77) | 保持真实 session 文件复制测试；增加 queue busy 拒绝场景 |
| `F6` | Normal | 移动到另一 pane | `supported` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:107), [`clients/tui/src/app.tsx`](/Users/rex/soft/neovifm/clients/tui/src/app.tsx:742), [`/Users/rex/soft/_refs/neovifm/viatc/actions/TCCOMMAND.ahk`](/Users/rex/soft/_refs/neovifm/viatc/actions/TCCOMMAND.ahk:80) | 保持真实 session move 测试；补失败/partial 提示验证 |
| `F7` | Normal | 新建目录 | `supported` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:108), [`clients/tui/src/app.tsx`](/Users/rex/soft/neovifm/clients/tui/src/app.tsx:680), [`/Users/rex/soft/_refs/neovifm/viatc/actions/TCCOMMAND.ahk`](/Users/rex/soft/_refs/neovifm/viatc/actions/TCCOMMAND.ahk:81) | 保持对话框提交测试；补非法名字边界 |
| `F8` | Normal | 删除当前项/选择集 | `supported` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:109), [`clients/tui/src/app.tsx`](/Users/rex/soft/neovifm/clients/tui/src/app.tsx:696), [`/Users/rex/soft/_refs/neovifm/viatc/actions/TCCOMMAND.ahk`](/Users/rex/soft/_refs/neovifm/viatc/actions/TCCOMMAND.ahk:82) | 保持确认对话框测试；补取消和 queue full 场景 |
| `F10` | Normal | 退出 | `supported` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:103), [`/Users/rex/soft/_refs/neovifm/viatc/actions/TCCOMMAND.ahk`](/Users/rex/soft/_refs/neovifm/viatc/actions/TCCOMMAND.ahk:260) | 增加 `F10` 与 `ZZ` 行为一致性检查 |

## Preview / View Mode 参照

| 按键/序列 | 模式 | 目标动作 | 当前状态 | 来源文件 | 测试建议 |
| --- | --- | --- | --- | --- | --- |
| `Esc` | Viewer overlay | 关闭 F3 viewer / 取消对话框 | `supported` | [`clients/tui/src/app.tsx`](/Users/rex/soft/neovifm/clients/tui/src/app.tsx:794), [`clients/tui/src/app.tsx`](/Users/rex/soft/neovifm/clients/tui/src/app.tsx:783) | 保持 viewer 与 dialog 两条关闭路径测试 |
| `a` / `A` | Classic view mode | 切换下一个/上一个 viewer | `deferred` | [`src/modes/view.c`](/Users/rex/soft/neovifm/src/modes/view.c:279), [`src/modes/view.c`](/Users/rex/soft/neovifm/src/modes/view.c:288) | 等多 renderer preview 进入后补 |
| `i` | Classic view mode | raw preview 切换 | `deferred` | [`src/modes/view.c`](/Users/rex/soft/neovifm/src/modes/view.c:294) | 图片/PDF/Markdown 预览进入后补 |
| `P` | Classic view mode | 持久化当前 viewer 选择 | `deferred` | [`src/modes/view.c`](/Users/rex/soft/neovifm/src/modes/view.c:284) | 与 `:fileviewer` / preview cache 联动验收 |
| `q` | Classic view mode | 退出 view mode | `deferred` | [`src/modes/view.c`](/Users/rex/soft/neovifm/src/modes/view.c:283), [`src/modes/view.c`](/Users/rex/soft/neovifm/src/modes/view.c:299) | 若 Hybrid viewer 引入独立 mode，再补 |

## Deferred: Archive / Remote / Explore

| 按键/序列 | 模式 | 目标动作 | 当前状态 | 来源文件 | 测试建议 |
| --- | --- | --- | --- | --- | --- |
| `e` | Normal | 像目录一样探索文件内容 | `deferred` | [`src/modes/normal.c`](/Users/rex/soft/neovifm/src/modes/normal.c:356) | 以 zip/archive provider 进入为首个验收切片 |
| `g l` | Normal | 打开当前项族 | `mapped` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:57), [`src/modes/normal.c`](/Users/rex/soft/neovifm/src/modes/normal.c:366) | 增加 `g l` 对文件和目录的行为测试 |
| `g h` | Normal | 父目录 | `mapped` | [`clients/tui/src/keymap.ts`](/Users/rex/soft/neovifm/clients/tui/src/keymap.ts:54), [`src/modes/normal.c`](/Users/rex/soft/neovifm/src/modes/normal.c:363) | 增加 prefix 组合测试 |
| `g f` / `g F` | Normal | 跳到链接目标 / 最终目标 | `deferred` | [`src/modes/normal.c`](/Users/rex/soft/neovifm/src/modes/normal.c:360), [`src/modes/normal.c`](/Users/rex/soft/neovifm/src/modes/normal.c:361), [`src/running.c`](/Users/rex/soft/neovifm/src/running.c:658) | 以 symlink 导航作为独立切片 |
| `archive://` 进入键位 | Normal | 压缩包当目录打开 | `deferred` | [`docs/NEOVIFM_ARCHITECTURE.md`](/Users/rex/soft/neovifm/docs/NEOVIFM_ARCHITECTURE.md:87), [`docs/NEOVIFM_ARCHITECTURE.md`](/Users/rex/soft/neovifm/docs/NEOVIFM_ARCHITECTURE.md:194) | 先实现 provider，再决定沿用 `e` 还是 `Enter/l` |
| `sshfs`/remote 进入键位 | Normal | 远程目录像本地目录打开 | `deferred` | [`src/int/fuse.c`](/Users/rex/soft/neovifm/src/int/fuse.c:323), [`tests/utils/get_command_name.c`](/Users/rex/soft/neovifm/tests/utils/get_command_name.c:74) | 先做 provider/FUSE 验收，再确定键位入口 |

## Not Applicable: ViATc 壳层特性

| 按键/序列 | 模式 | 目标动作 | 当前状态 | 来源文件 | 测试建议 |
| --- | --- | --- | --- | --- | --- |
| AHK `Class` 级热键作用域 | Global shell hook | 仅在指定窗口类下转发按键 | `not-applicable` | [`/Users/rex/soft/_refs/neovifm/viatc/vimcore.0.2.1.ahk`](/Users/rex/soft/_refs/neovifm/viatc/vimcore.0.2.1.ahk:67) | 不纳入 NeoVifm |
| `Send` 文本/键串 | Global shell hook | 向其他程序发送原始按键 | `not-applicable` | [`/Users/rex/soft/_refs/neovifm/viatc/vimcore.0.2.1.ahk`](/Users/rex/soft/_refs/neovifm/viatc/vimcore.0.2.1.ahk:121), [`/Users/rex/soft/_refs/neovifm/viatc/vimcore.0.2.1.ahk`](/Users/rex/soft/_refs/neovifm/viatc/vimcore.0.2.1.ahk:200) | 不纳入 NeoVifm |
| ViATc 宏/运行外部程序 | Global shell hook | 宏动作或运行外部程序 | `not-applicable` | [`/Users/rex/soft/_refs/neovifm/viatc/vimcore.0.2.1.ahk`](/Users/rex/soft/_refs/neovifm/viatc/vimcore.0.2.1.ahk:206), [`/Users/rex/soft/_refs/neovifm/viatc/vimcore.0.2.1.ahk`](/Users/rex/soft/_refs/neovifm/viatc/vimcore.0.2.1.ahk:218) | 不纳入 NeoVifm |
| TC 面板按钮栏/驱动器栏显隐 | GUI shell | 控制 Total Commander 外壳组件 | `not-applicable` | [`/Users/rex/soft/_refs/neovifm/viatc/actions/TCCOMMAND.ahk`](/Users/rex/soft/_refs/neovifm/viatc/actions/TCCOMMAND.ahk:278) | 不纳入 NeoVifm |

## 当前可验收重点

1. 已实现主线应继续锁定：`hjkl`、`gg/G`、`Ctrl-W w/h/l`、`gt/gT`、`F3-F10`。
2. 当前最重要的显式偏差是：`l`/`Right` 对普通文件进入 F3 viewer；排序切换由排序控件和命令入口完成，不占用方向键。
3. 下一批最适合进入执行阶段的键位扩展，不是新增大量快捷键，而是让既有 `F3`、`e`、`l/Enter` 承接 archive、image、pdf、markdown、remote provider。
