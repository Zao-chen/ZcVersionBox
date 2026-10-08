# CI 范围与运行方式

日常提交在 Linux 验证业务回归，在 Windows/macOS 验证构建和关键平台行为；压力测试、截图遍历和发布打包独立运行。所有现有 Qt Test 用例保留，默认本地配置仍为 `full`。

## 调整依据

2026-10-08 调查了以下独立 Qt 桌面项目的实际工作流：

- [qView 单元测试](https://github.com/jurplel/qView/blob/main/.github/workflows/test.yml) 在 Ubuntu 执行，[多平台构建](https://github.com/jurplel/qView/blob/main/.github/workflows/build.yml) 独立组织。
- ksnip 的 [Windows](https://github.com/ksnip/ksnip/blob/master/.github/workflows/windows.yml)、[Linux](https://github.com/ksnip/ksnip/blob/master/.github/workflows/linux.yml)、[macOS](https://github.com/ksnip/ksnip/blob/master/.github/workflows/macos.yml) 将测试与打包分开，打包限定为 push。它仍执行跨平台测试。
- [Crow Translate](https://github.com/crow-translate/crow-translate/blob/master/.github/workflows/main.yml) 排除文档、翻译等路径，并将格式、元数据和构建检查分开。

这些项目没有统一的矩阵模板。这里采用适合 ZcVersionBox 的范围：保留三平台编译、备份恢复与文件监听检查，将重复的完整集成回归集中在较快的 Linux；Universal 架构与安装包验证留在发布流程。

旧 CI 的实测证据来自 PR #30 的 [第二轮运行](https://github.com/Zao-chen/ZcVersionBox/actions/runs/37745107340)：Windows 作业 29 分 23 秒，其中 `backup_core` 1068 秒、`regression` 431 秒；macOS 13 分 53 秒，Linux 3 分 32 秒。Qt 缓存命中也无法减少这些测试进程的耗时。新配置的实际结果以最新运行及作业摘要为准，首次建立编译缓存仍需完整编译。

2026-10-08 新配置首次云端验证：[PR #30 运行](https://github.com/Zao-chen/ZcVersionBox/actions/runs/37754093242) 三平台及汇总 `CI` 均通过。Windows 作业 4 分 09 秒（UI 10 个用例 51.6 秒，核心 9 个用例 30.4 秒）；macOS 作业 3 分 35 秒（UI 10 个用例 23.8 秒，核心 9 个用例 11.7 秒）；Linux 作业 3 分 52 秒，standard 有 55 个 UI 用例、156 个核心用例及 5 个更新用例，总计约 61 秒，并通过 X11/Wayland 各 4 个原生检查。Linux 相比旧运行的 3 分 32 秒多 20 秒，但覆盖了完整日常回归及桌面烟测；首轮缓存冷启动用来建立后续编译缓存，不据此宣称缓存已降低首次总耗时。

## 三种范围

| 场景 | Linux | Windows | macOS | 打包 |
| --- | --- | --- | --- | --- |
| PR / main 的代码改动 | standard + X11/Wayland | smoke | smoke，arm64 | 否 |
| 仅 Markdown、docs、LICENSE 改动 | 跳过 Qt | 跳过 Qt | 跳过 Qt | 否 |
| 手动 CI，scope=full | full + X11/Wayland | full | full，arm64 | 否 |
| 手动 Release | full + 桌面/安装包验证 | full + 安装器验证 | full，Universal + DMG 验证 | 是 |

纯文档改动仍执行轻量报告工具检查，最终 `CI` 检查正常结束，避免 required check 因 workflow 整体跳过而一直 pending。未知文件类型、工作流和测试修改均按代码改动处理。手动 CI 可以单选平台。

`standard` 通过 Qt Test 的 `-functions` 自动发现测试，新函数自动进入日常回归；只排除以下五个函数，数据驱动测试的全部行继续执行：

- `largeProjectMonitorBoundsEventStorms`：一万文件、两千次写入的监听压力场景。
- `manyRevisionsAndEditFeedback`：大历史列表与编辑压力场景。
- `renderAllPages`、`renderImportantVersions`、`renderConflictPages`：专门的截图遍历。

`smoke` 的明确函数名单在 `tests/CMakeLists.txt`。UI 覆盖运行时、关闭窗口、备份恢复/Diff、新建并切换方案的两条事件路径、编辑与恢复；核心覆盖 Windows 原子替换、目录监听、方案切换时保存工作及忽略文件、失效确认、恢复回滚、递归存储/链接拒绝、跨进程写锁。`update_core` 在三种范围均完整执行。

`full` 不传函数筛选，执行测试程序的全部用例，包括上述压力和截图函数。平台不适用的既有跳过规则保留。macOS offscreen 无法替代有桌面环境的 Cocoa 焦点验收；Windows/macOS smoke 也不能替代全部平台故障回归，需要时手动运行 full。

## 本地与云端

复用已有构建目录，显式选择范围；不需要为切换范围重新编译业务代码：

```bash
cmake -S . -B build -DZCVERSIONBOX_TEST_PROFILE=standard
cmake --build build --parallel 4
ctest --test-dir build --parallel 2 --output-on-failure

# 需要完整回归时，显式切回 full；CMake 缓存会记住上次选择。
cmake -S . -B build -DZCVERSIONBOX_TEST_PROFILE=full
ctest --test-dir build --parallel 2 --output-on-failure
```

GitHub Actions → Desktop CI → Run workflow：默认 `scope=standard` 使用日常矩阵，选择 `scope=full` 执行完整回归；`platform` 可选择 all/windows/macos/linux。Release 始终显式使用 full，不能因上一次选择 smoke 而缩小发布验证。

## 缓存、超时和诊断

- Qt 安装缓存继续使用；非打包构建增加 [sccache](https://github.com/mozilla-actions/sccache-action) 编译缓存，通过 GitHub Actions cache 后端复用编译产物。每次配置仍使用干净构建目录；发布构建不启用编译缓存。
- `ctest --parallel 2` 并行不同测试程序的独立 fixture；每组内部顺序执行，只有 `zc_tests` 使用界面和剪贴板。
- smoke 的 UI/core 各 180 秒；standard/full 的 UI 在 Linux/macOS 为 300 秒、Windows 600 秒，core 为 600/1200 秒；更新测试 60 秒。CTest 外层预算额外留 30 秒输出诊断，超时仍使作业失败。
- 配置、编译、回归、Linux 原生桌面检查和打包步骤分开，失败可定位到具体阶段。日常不构建安装包、不运行安装升级容器。
- Qt 同时输出文本与 JUnit XML；`scripts/ci_report.py` 在作业摘要展示数量、失败、跳过、整组时间和最慢十个用例。失败和超时仍上传日志，残缺 XML 会明确标记；测试日志保留 7 天。
- 同一分支的新提交取消过时日常运行。最终 `CI` 作业汇总已选检查，失败或取消不会被当作成功。仓库如配置必需检查，可将稳定的 `CI` 用作合并门槛。

本次调整不改变生产备份逻辑，不删测试，也不缩短压力数据集或隐藏断言失败。回归仍仅使用隔离测试程序，不运行读取真实备份的应用。
