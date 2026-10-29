# Alpha 0 切换到个人仓库主线

## 目标

把已验证的六层成果（Windows actions、opener、watcher、portable preview、rename、multi-select）合入 `aumi314/neovifm:master`，让个人仓库成为唯一日常集成分支，解除「等导师合并才能继续」的依赖。本阶段不新增产品功能。

## 顺序与验收

1. 备份全部 refs 与分支 bundle；确认工作树干净、旧 `refs/codex-backups/` 仍在。
2. 确认整合分支的树与此前 CI 全绿的 `b2a95ddb3` 逐文件一致。
3. 文档改为主线模型：AGENTS、`CURRENT_STATE.md`、`NEOVIFM_HANDOFF.md`、README 与 active plan 同源。
4. 推送整合分支并向个人仓库 `master` 开一个 Ready PR，`CI / gate` 三平台全绿后合并。
5. 合并后在 `master` 触发一次 `Preview` workflow，得到四平台主线包。
6. 关闭个人 fork 上以分支为 base 的堆叠 PR；导师侧 PR #4 不动。
7. 主线 Windows 包在源码目录外做一次 `--check` 与启动冒烟，证据写进 `progress.md`。

## 边界

- 不改 protocol、schema、DTO 和 capability；不加新功能、不同步 Vifm 上游（当前 `f5d60eaa` 已是祖先）。
- 不创建 release、tag、安装器，不做品牌重命名。
- 不删除历史分支和试用现场，清理需另行确认。
- 导师仓库仍是评审通道：不擅自合并导师 PR，不改其分支保护。
