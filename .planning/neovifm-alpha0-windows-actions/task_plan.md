# NeoVifm Windows File Actions Baseline

## 目标

在 B2a Linux actions 分支之上，让 Windows 10+ session 发布并真实执行 `file-actions-v1`，保持 Workbench Alpha 0 (unreleased) 和 protocol v3 不变。

## 范围

- [x] Windows 10 runtime capability gate。
- [x] Win32 handle identity、UTF-16 extended-length path 和 no-follow directory snapshot。
- [x] no-overwrite copy、同卷 move、mkdir、隔离后 Recycle Bin delete。
- [x] copy/move/mkdir undo bridge。
- [x] Windows focused C fixtures 不再排除 action/undo。
- [x] 真实 core integration 覆盖 Unicode、长路径、递归 copy、junction、Recycle Bin、失败恢复和 C/D 跨卷 move。
- [x] 个人 fork 三平台 CI 和 `CI / gate` 全绿。

## 不在范围

- watcher、默认 Win32 opener、安装器、release/tag。
- protocol/schema/DTO/public API 变化。
- protocol 级 delete undo。

## 验收

- Windows 本地 real-core：8 pass；C/D 真实跨卷 move：1 pass。
- Windows focused C 由远端 MSYS2 job 构建并运行 13 个 fixtures。
- Linux/macOS 原有 focused C、real-core、coverage、audit 和 serial `make check` 不降级。
- 重排后的 B2b 远端证据：GitHub Actions run `31352896921`，Linux、macOS、Windows 和 `CI / gate` 全绿。
