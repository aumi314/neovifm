# ADR 0002：Windows 安全文件操作边界

- 状态：Accepted
- 日期：2026-08-10
- 决策人：NeoVifm 项目

## 背景

NeoVifm protocol v3 已定义 `file-actions-v1`，但 Windows 不能直接复用 POSIX 的 parent-FD-relative 操作，也不能用窄字符 CRT API 满足中文、UNC 和长路径要求。移动和删除尤其不能接受覆盖目标、跨卷 copy-delete 或静默永久删除。

## 决策

1. NeoVifm core/TUI 的最低 Windows 运行基线是 Windows 10。经典 `vifm.exe` 的兼容实现不随本 ADR 重写。低于 Windows 10 时不发布 `file-actions-v1`。
2. protocol、schema 和 DTO 保持不变。Windows 将 `device`、`inode`、`ctime_unix_ns` 分别映射为 volume serial、64 位 file index 和 creation `FILETIME`；完整 identity 由显式 `nv_fs_identity_t` 返回，不塞入容量不足的 `struct stat`。
3. 所有操作路径在 Win32 边界转换为 UTF-16 extended-length absolute path。父目录、源对象和目标对象通过 no-follow handle 固定并复核；reparse point 不递归跟随。
4. copy 以 source/destination handle 读写，目标使用 `CREATE_NEW`；目录只做受控递归。失败清理只能触碰本次创建且仍能安全确认的对象。
5. move 使用 source handle 的 `SetFileInformationByHandle(FileRenameInfo)`，禁止覆盖。目标绝对路径从已固定的 destination directory handle 派生；不同 volume serial 返回 `EXDEV`，不回退为 copy-delete。移动前以发布 identity 防 stale，移动后比较仍持有的 source handle 与 destination handle，避免 creation time 在 rename 后变化导致误判。
6. mkdir 使用 Unicode Win32 API，创建前后复核父目录，并打开新目录确认结果。
7. delete 先以 handle rename 将对象无覆盖移入同目录私有隔离目录，再由 STA COM `IFileOperation` 配合 `FOFX_RECYCLEONDELETE` 送入 Recycle Bin。失败时只在原名称空闲时恢复，不直接永久删除用户原对象。
8. `NEOVIFM_TRASH_EXECUTABLE` 只作为自动化测试注入口；默认产品路径始终使用 Recycle Bin。
9. copy、move、mkdir 接入现有 undo bridge。delete 依赖 Recycle Bin，不增加协议级 delete undo。

## 结果

- Windows 10+ 可以发布 `file-actions-v1`，TUI 才显示对应操作入口。
- 中文、UNC 和超过 `MAX_PATH` 的路径不再依赖进程 locale。
- move 和 delete 的失败模式可预测：不覆盖、不跨卷 copy-delete、不绕过回收站。
- watcher、默认 Win32 opener、安装包和 release 仍是独立工作，不因本 ADR 自动完成。

## 被拒绝的方案

### CRT `rename()`、`remove()` 和窄字符路径

拒绝原因：不能稳定覆盖 Unicode/长路径，也无法固定 parent/entry handle 或表达 no-follow 边界。

### 跨卷 move 时复制后删除

拒绝原因：把一个原子动作变成两个可部分完成动作，源对象可能在复制和删除之间被替换。

### Recycle Bin 失败后永久删除

拒绝原因：违反用户对 delete 可恢复性的直接预期；失败应恢复或明确留下隔离对象，而不是扩大损失。

## 复审条件

- Windows 需要支持低于 Windows 10 的 NeoVifm core/TUI；
- Win32 提供可替代现有 rename/recycle 方案且能保留相同安全语义的新 API；
- 协议新增可审计的 delete undo，而不再依赖平台回收站。
