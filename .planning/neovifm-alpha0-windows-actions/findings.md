# Findings

## Win32 identity

- `struct stat` 不能完整承载 volume serial、64 位 file index 和 creation `FILETIME`；Windows filesystem helper 改为显式返回 `nv_fs_identity_t`，POSIX 映射不变。
- creation `FILETIME` 在同卷 rename 后可能变化。移动前仍用三元 identity 防 stale；移动后比较仍持有的 source handle 与 destination handle，确认对象连续性。

## Rename and delete

- `FileRenameInfo.RootDirectory + relative name` 在实机上出现过拒绝或非预期目标解析。当前目标路径从已固定的 destination directory handle 取得，再由 source handle 执行 no-replace rename。
- 默认 delete 使用 STA COM `IFileOperation` 和 `FOFX_RECYCLEONDELETE`。helper 失败时以隔离后的 handle identity 恢复，避免 rename 后 creation time 变化导致合法恢复被拒绝。

## Path and reparse boundary

- Win32 边界统一使用 UTF-16 extended-length absolute path，真实测试覆盖中文和超过 260 字符路径。
- junction/reparse point 作为最终 entry 时拒绝复制，不递归跟随。

## CI registration

- B2a 起初没有远端 CI，是因为个人 fork `master` 没有 workflow。将 fork `master` fast-forward 到导师 `58f26509` 后，workflow 注册成功。
- fork master run `31349058418` 与 B2a run `31349327088` 均为 Linux、macOS、Windows、`CI / gate` 全绿。
