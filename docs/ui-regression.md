# 回归验证

备份核心和页面分别由 `zc_backup_tests`、`zc_tests` 驱动，CTest 注册为 `backup_core`、`regression`。两套测试使用独立临时目录、隔离 Git 配置、本地 bare 远程和模拟 AI，不读取真实备份或凭据，不访问外部 Git/AI 服务，不写右键菜单或自启动设置。

## 覆盖范围

| 场景 | 回归证据 |
| --- | --- |
| 默认文件范围 | 重构前先运行行为基线：build 单独变化不触发；其他变化触发时复制 build；隐藏文件处理、Git ignore 与副本/提交区别保持 |
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

通知复制测试会保存并恢复剪贴板格式。只有测试窗口短暂显示以验证焦点；正常应用不用于回归。大列表使用展示模型 fixture，业务测试始终使用合法的隔离仓库和记录。

## 构建与运行

参见 [开发指南](../CONTRIBUTING.md)。本次 Windows 工作目录使用 Visual Studio 多配置构建：

```powershell
& 'S:/Qt/Tools/CMake_64/bin/cmake.exe' --build build/backup-core --config Release --parallel 4
& 'S:/Qt/Tools/CMake_64/bin/ctest.exe' --test-dir build/backup-core -C Release --output-on-failure
# 两套测试彼此隔离，也可加 --parallel 2。
git diff --check
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

## 本次验证边界

2026-09-25 实际执行 Windows / Qt 6.8.3 / MSVC 2022 x64 Release 完整构建、全部隔离 CTest 和 `git diff --check`，均通过。`zc_tests` 为 23 passed，`zc_backup_tests` 为 55 passed，均为 0 failed、0 skipped；Qt Test 计数包含初始化和清理。两套 CTest 并行执行总用时 116.06 秒，详细结果保留在上述日志。

旧 UI 阶段的安装包哈希和本地构建记录已从当前验证说明中移除，避免被误认为本次产物。

未启动读取真实备份的正常应用，未安装或发布。现有 macOS CI 在手动发布工作流中配置了同一套构建和 CTest，目标为 arm64、最低部署版本 12.0，并同时归档两份测试日志。本轮没有执行 macOS 构建或触发该工作流；这些配置不代表 macOS 已通过验收。后续需增加独立的 Windows/macOS PR 构建测试，并补齐大小写规则和可执行权限的专项回归。

以下仍需对应平台或隔离账户验证：真实远程认证与网络故障、Explorer/Finder 入口、自启动、托盘、安装/卸载、原生文件选择框和多显示器 DPI。当前流程不承诺自动崩溃重放，Ignore 产品设计和大目录增量扫描独立跟进，详见 [备份架构](backup-architecture.md)。
