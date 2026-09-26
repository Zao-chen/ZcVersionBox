# 回归验证

备份核心和页面分别由 `zc_backup_tests`、`zc_tests` 驱动，CTest 注册为 `backup_core`、`regression`。两套测试使用独立临时目录、隔离 Git 配置、本地 bare 远程和模拟 AI，不读取真实备份或凭据，不访问外部 Git/AI 服务。Linux 桌面入口测试将 XDG 路径注入临时目录，不写用户实际的右键菜单或自启动设置。

## 覆盖范围

| 场景 | 回归证据 |
| --- | --- |
| 默认文件范围 | 重构前先运行行为基线：build 单独变化不触发；其他变化触发时复制 build；隐藏文件处理、Git ignore 与副本/提交区别保持 |
| 监控调度 | 注入时钟验证 500 ms 静默、5 s 最大合并、30 s 完整校验；执行中再次变化、失效快照、轮转、单扫描/单自动备份上限、退避和取消 |
| 原生文件事件 | 临时目录中的嵌套写入、原子保存、新目录、源目录改名及重建、共享监听、无关/build 事件过滤；登记失败和静默遗漏均由完整校验补偿 |
| 监控生命周期 | 默认停止、幂等启动/停止、停止后重启、取消 AI 等待、保留手工任务、删除重加旧结果失效、服务与监控分别销毁 |
| 服务调度和目录 | 同级 FIFO、前台优先、后台等待期间每 8 个前台任务让行；加载状态共享、加载中多个变化合为一次重载、30 s 外部 Git 审计、200 次原子记录保存 |
| 一万文件 | 100 个目录、10,000 个文件；真实空闲扫描次数、扫描阻塞期间 2,000 次写入、后续自动备份请求数和最大未完成请求数 |
| 文件、目录、中文、空格、二进制 | 真实临时 Git 仓库，历史、初始 Diff、预览、源恢复与 HEAD 断言 |
| pull 持久暂停 | 排队备份、源继续编辑、重启、修改地址、重复 pull、取消确认均不能覆盖拉取版本 |
| 两种解决方式 | 应用到源不额外提交；保留源创建普通后继版本，远端提交保持祖先关系；相同内容不产生空提交 |
| 确认上下文 | 源内容、对象、仓库代次或待处理版本变化时拒绝旧请求；UI 取消默认与两种操作均有交互回归 |
| pull 失败 | 保存暂停失败不拉取；无内容变化不推进源指纹；实际 upstream、不同分支、分叉与不确定结果 |
| 映射失效 | 远端删除追踪路径时明确失败，源仍保留；普通恢复和重建不能绕过暂停 |
| 文件/Git/记录故障 | 目录枚举、复制、替换、暂存、提交、初始/最终记录保存失败；基线不误推进，安全回滚，无法回滚保留具体路径 |
| 源与外部变化 | 源消失再出现、失败退避、同大小同修改时间的内容变化；安装后立即编辑源/副本不能推进基线，暂存和提交瞬间的外部暂存内容保持原样 |
| AI 与队列 | 等待时主事件循环响应；超时回退、取消、迟到回调、服务销毁与等待期间的外部修改 |
| 并发和异常退出 | 隔离子进程竞争存储锁；中止已准备备份的子进程，重载发现持久标记并暂停；监控不循环重载自己的锁 |
| 导入 | 普通 Git、多顶层内容、根/子目录/单文件选择、目标改名后恢复原路径历史；失败恢复原目标 |
| 路径与删除安全 | 递归存储、符号链接/联接及祖先链接拒绝；添加 .git 指针/目录/内部文件在运行 Git 前失败；导入不能覆盖源 Git 元数据；恢复保留仅含 .git 的目录，旧格式数据不自动删除 |
| 替换与回滚完整性 | 安装内容对照候选预期指纹；目录中途替换失败能恢复原内容；保留的旧副本被外部编辑时停止回滚并保留现场 |
| 重建与编辑 | 忽略规则下的已追踪树保留、对象哈希格式不变、准备时外部提交不会丢失；编辑最新说明不夹带暂存内容 |
| 列表与导航 | 同名不同 ID、独立筛选、0/1/50/500 项模型、前后退、删除/重建失效、返回应用上下文 |
| 历史长列表与行操作 | fast-import 构造 1001 版本，无逐行 QWidget；预览/对比/菜单/快捷键、完整 OID、空说明、时间和选择保持 |
| 异步页面与编辑 | 切换对象、重建、删除重加后的旧响应失效；编辑期间延后刷新，失败反馈和回填 |
| 设置、主题与通知 | 设置搜索/返回、即时保存、服务商/Key/URL 的请求失效；Diff 原文/滚动保持，Spinner、Expander、Popover 与焦点回归 |
| 页面渲染 | 浅深色、1080×740/760×520、窄窗口、超长路径、空态、加载和通知；无整页横向滚动 |
| Linux 文件语义 | 大小写不同的文件并存、仅大小写改名、执行位触发原生事件与备份、恢复 `100644/100755`、扫描期间 chmod、真实不可读目录、旧指纹刷新无空提交 |
| Linux 桌面入口 | 原子注册、重复启停、保留不属于应用的文件、目录写入失败不保存启用状态、多选本地 URI 及特殊字符、localhost 归一化、拒绝远程 URI |
| 无托盘及运行库 | 关闭窗口退出、Xvfb/XCB 与 headless Weston/Wayland 窗口暴露和截图、安装后的系统图标解析、SVG 和 TLS 插件 |

通知复制测试会保存并恢复剪贴板格式。只有测试窗口短暂显示以验证焦点；正常应用不用于回归。大列表使用展示模型 fixture，业务测试始终使用合法的隔离仓库和记录。

## 构建与运行

参见 [开发指南](../CONTRIBUTING.md)。本次 Windows 工作目录使用 Visual Studio 多配置构建：

```powershell
& 'S:/Qt/Tools/CMake_64/bin/cmake.exe' --build build/backup-core --config Release --parallel 4
& 'S:/Qt/Tools/CMake_64/bin/ctest.exe' --test-dir build/backup-core -C Release --output-on-failure
# 两套测试彼此隔离，也可加 --parallel 2。
git diff --check
```

BackupMonitor 重构的约定范围是 `backup_core` 整组，以及 `regression` 中相关测试函数：

```powershell
& 'S:/Qt/Tools/CMake_64/bin/cmake.exe' --build build/backup-core --config Release --target zc_backup_tests zc_tests ZcVersionBox --parallel 4
& 'S:/Qt/Tools/CMake_64/bin/ctest.exe' --test-dir build/backup-core -C Release -R '^backup_core$' --output-on-failure
$env:PATH = 'S:/Qt/6.8.3/msvc2022_64/bin;' + $env:PATH
$env:QT_QPA_PLATFORM = 'offscreen'
$env:QT_QPA_PLATFORM_PLUGIN_PATH = 'S:/Qt/6.8.3/msvc2022_64/plugins/platforms'
& ./build/backup-core/Release/zc_tests.exe monitorSurvivesPageRefresh defaultFileSelectionKeepsBuildAndGitIgnoreSemantics automaticAiMessagesAndDeletedContext pullStateActionsRespectConfirmation confirmationsRespectObjectContext asyncControlsAndThemePreserveContext -o ./build/backup-core/monitor-ui-regression.txt,txt
```

`backup_core` 包含一万文件的完整备份事务，CTest 超时为 600 秒；小范围调试可指定 Qt Test 函数，避免每次运行压力用例。所有时间窗口策略用注入时钟验证；真实文件事件测试只缩短合并时间，不将平台事件延迟当作严格定时保证。原子保存测试在 Windows 使用 `ReplaceFileW`，其他平台使用 QSaveFile；另有持续 QSaveFile 清单保存测试，覆盖监听对应用自有记录的影响。

macOS 在已有 Qt 6.8.3 / arm64 构建目录运行同组 `ctest -R '^backup_core$'`，文件事件及压力用例不以平台条件跳过。可单独验收监听行为：

```bash
cmake --build build/release --target zc_backup_tests zc_tests --parallel 4
QT_QPA_PLATFORM=offscreen ./build/release/zc_backup_tests \
  sourceEventsHandleAtomicSavesNewDirectoriesAndRecreation \
  sourceWatchesSharePathsAndFilterUnrelatedChanges \
  rejectedSourceWatchesFallBackToPeriodicContentChecks \
  missingEventsAreRecoveredAtThePeriodicDeadline \
  catalogWatchesAllowAtomicRecordReplacement \
  catalogChangesDuringReloadAreCoalesced \
  monitorStopRestartDrainsTheOldScan \
  largeProjectMonitorBoundsEventStorms -o build/release/monitor-macos.txt,txt
```

CTest 使用 offscreen 平台运行 UI 回归，显式设置 Qt 插件路径并保留文本结果：

- `build/backup-core/tests/regression.txt`
- `build/backup-core/tests/backup_core.txt`
- `build/backup-core/Testing/Temporary/LastTest.log`

单独运行 Qt Test 时应配置 Qt DLL 和插件路径。需要原生渲染时仍使用测试程序：

```powershell
$env:PATH = 'S:/Qt/6.8.3/msvc2022_64/bin;' + $env:PATH
$env:QT_QPA_PLATFORM = 'windows'
$env:ZC_TEST_SCREENSHOTS = Join-Path $PWD 'build/backup-core/screenshots'
& ./build/backup-core/Release/zc_tests.exe renderAllPages -o ./build/backup-core/render.txt,txt
```

不同 UI 测试进程应依次运行，避免焦点和剪贴板相互干扰。`QT_SCALE_FACTOR=1.5` 或 `2` 可用于额外 DPI 检查，不改变系统显示设置。

## BackupMonitor 验收步骤

1. 在 `feature/backup-monitor-refactor` 上运行上述约定范围，查看 `backup_core.txt` 和 `monitor-ui-regression.txt`；失败时使用日志中的函数名单独复现。
2. 以 `sourceEventsHandleAtomicSavesNewDirectoriesAndRecreation` 核对嵌套修改、原子保存、新增目录、源改名及原路径重建；对照仓库内容，旧源目录的内容应保持。
3. 以 `monitorStopRestartDrainsTheOldScan`、`monitorStopCancelsItsBackupButKeepsManualWork` 和 `monitorCancelsQueuedBackupWhenPullPausesTarget` 核对停止、重启、AI 取消和暂停；迟到结果不能推进旧基线。
4. 以 `missingEventsAreRecoveredAtThePeriodicDeadline`、`periodicMonitorAuditFindsExternalGitChanges` 核对事件遗漏后的 30 s 完整校验和外部 Git 审计。
5. 查看 `largeProjectMonitorBoundsEventStorms` 的计数输出：空闲期间无额外扫描，风暴期间不扩张任务队列，全局最多一个尚未完成的自动备份。此用例保留完整备份安全检查，耗时也包含这些检查。
6. 在 macOS 执行相同隔离用例并保留日志，再验收平台行为。当前功能分支仅本地提交；验收通过前不推送或合并。

## Linux 兼容升级的验证

2026-09-26 的 Linux 移植使用 ZcAILib 0.2.0 固定源码、Qt 6.8.3 和普通用户的 Linux 文件系统。Ubuntu 22.04 构建的同一 `.deb` 在无 Qt 开发环境的 Ubuntu 22.04 和 24.04 容器各运行 `regression`（29 passed）及 `backup_core`（84 passed），均无失败或跳过。两个系统的 XCB 和 Wayland 各通过 `platformRuntimeSmoke`、`closingWithoutTrayExitsWindow`，并验证安装、升级和 purge 保留用户目录。日志、安装步骤、构建方式及真实 GNOME 待验收项见 [Linux 兼容说明](linux-compatibility.md)。

Windows 复用 `build/backup-core`，应用与两个测试程序增量构建成功；`regression` 为 28 passed / 1 Linux-only skipped，`backup_core` 为 79 passed / 5 Linux-only skipped。后续 URI 边界修正单独运行 `nautilusSelectionPreservesPaths`。未因文档或脚本调整重复无关业务测试。

## BackupMonitor 重构的历史验证

以下记录为 Linux 移植前的 BackupMonitor 重构验收范围。2026-09-26 在 Windows 11 / Qt 6.8.3 / MSVC 2022 x64 Release 上复用 `build/backup-core` 增量构建 `zc_backup_tests`、`zc_tests`、`ZcVersionBox`，均成功。

| 实际执行范围 | 结果 | 日志 |
| --- | --- | --- |
| `backup_core` 整组 | 79 passed、0 failed、0 skipped；CTest 300.12 s | `build/backup-core/tests/backup_core.txt` |
| 上述 6 个 UI 回归函数 | 8 passed、0 failed、0 skipped；22.035 s | `build/backup-core/monitor-ui-regression.txt` |
| 一万文件 / 2,000 次写入 | 空闲 2.1 s 内 0 次额外扫描；风暴产生 2 次扫描、1 个自动备份，最多 1 个未完成备份 | `backup_core.txt` 中的 `largeProjectMonitorBoundsEventStorms` 输出 |
| 差异及依赖来源检查 | `git diff --check` 通过；上游源码比对仅 Windows 回调文件有记录在案的修改，另新增 `UPSTREAM.md` | 本地差异及 `3rdparty/efsw/UPSTREAM.md` |

Qt Test 的 passed 计数包含初始化和清理。未运行 `regression` 的无关函数。压力用例从释放扫描到完整备份及后续校验结束为 115.448 s，包含复用引擎的完整复制、SHA-256 和事务安全检查；该数字不是事件监听延迟或吞吐承诺。500 ms、5 s、30 s 的期限另由注入时钟的确定性测试验证。

构建期间 windeployqt 提示当前 shell 未设置 `VCINSTALLDIR`，构建及测试正常完成；本轮没有制作或验证安装包。UI 的 offscreen 插件产生字体目录及 `propagateSizeHints` 提示，相关函数断言全部通过。

该次重构未启动读取真实备份的正常应用，未安装、推送或合并；仅包含 Windows 执行结果。后续 Linux 移植已补齐源码 SDK、大小写与执行位回归及安装包验证，见上节。macOS CI 目标为 arm64、最低部署版本 12.0，但尚无本轮实际运行结果，不能据此宣称 macOS 已通过验收。

以下仍需对应平台或隔离账户验证：真实远程认证与网络故障、Explorer/Finder 入口、自启动、托盘、安装/卸载、原生文件选择框和多显示器 DPI。当前流程不承诺自动崩溃重放，Ignore 产品设计和大目录增量扫描独立跟进，详见 [备份架构](backup-architecture.md)。
