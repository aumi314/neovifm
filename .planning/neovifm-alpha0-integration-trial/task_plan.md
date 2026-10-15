# Alpha 0 分支整合与 Windows 试用

## 目标

在不增加产品功能的前提下，把已完成的六层工作接到导师已合并 Linux actions 的 master，保留可恢复历史，交付独立 Windows actions PR 和包含多选的最新便携试用包。

## 顺序与验收

1. 备份全部 Git refs 与 bundle；核对工作树干净和远端状态。
2. 依次整理 Windows actions、Windows opener、watcher、portable preview、rename、multi-select；逐层验证 rebase 前后 tree 一致。
3. 使用明确旧 SHA 的 force-with-lease 更新个人 fork；Windows actions PR 接到 master，其余保持原堆叠关系。
4. 三平台 CI 验证 Windows actions 后，向导师创建独立 Ready PR；不自动合并。
5. 更新当前状态、handoff 和 active plan，记录 rename、多选及真实平台边界。
6. 当前顶层分支 CI / gate、Preview / gate 全绿，下载 Windows archive 到源码目录之外，验证 checksums、CLI、session 和文件任务。
7. 仅在新建测试目录中执行文件操作，保留可复用试用目录和证据；不操作用户原有文件。

## 边界

- 不添加新功能，不同步本阶段无关的 Vifm 更新，不创建 tag/release/安装器。
- 不合并个人或导师 PR；分支保护规则不修改。
- 图形终端手动体验与自动化验证分别记录，未观察的行为不标记通过。
