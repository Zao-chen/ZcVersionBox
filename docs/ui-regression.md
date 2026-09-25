# UI 现代化重构验证

本轮在已有 Ela → Qlementine 迁移基础上完成窗口、Sidebar、备份列表、历史、概览、Diff、设置和通知的重组。结构继续使用标准 Qt Widgets 与 Designer，视觉由 QlementineStyle 统一，Switch、Expander、LoadingSpinner、Popover 按交互用途保留。

## 自动化范围

`tests/regression.cpp` 由 Qt Test 驱动，CTest 注册为 `regression`。测试使用独立 QTemporaryDir、临时 Git 配置、本地 bare 远程仓库和模拟 AI 回调；不读取真实备份和凭据，不写系统右键菜单或自启动配置，不向外部 Git / AI 服务发送测试数据。

| 场景 | 回归证据 |
| --- | --- |
| 本地文件备份、恢复、只读预览 | 临时中文/空格文件名，历史数量、恢复内容和 HEAD 保持断言 |
| 目录、隐藏文件、嵌套文件和二进制 | 实际快照、恢复内容及二进制 Diff 断言 |
| 首提交、按文件 Diff | 空树对比及中文文件路径读取断言 |
| pull / push、远程开关、重建 | 本地 bare 远程；检查版本数、地址保留和移除语义 |
| 云端导入、取消、删除 | 暂存导入不进入监控；内容与临时目录清理断言 |
| 自动备份和 AI 提交说明 | 应用级监控保持、模拟成功/失败回退、删除或重建后的旧结果失效 |
| 对象 Sidebar 与路由 | 同名不同 ID、点击默认进入历史、前后退、独立筛选代理；筛选隐藏当前对象时保留上下文，清除后恢复选择 |
| 0 / 1 / 50 / 500 个备份 | QAbstractItemModelTester、按 ID 增删移动、行数变化；没有按行堆叠 QWidget |
| 1000 条历史与说明编辑 | fast-import 构造 1001 个版本；表格 QWidget 数量与单版本时一致，滚动到末尾仍可直接对比；持久模型、F2 编辑、失败反馈及说明回填 |
| 版本行内操作 | 未预先选中时直接预览/对比，预览内容正确且源文件/HEAD 不变；不同图标释放、移出视口、模型重置取消旧按下；双击/Enter、Alt+P、Shift+F10 / Menu、F2 与菜单编辑均保留 |
| 版本菜单与恢复确认 | 菜单绑定原版本，不随选择或刷新改变；取消默认，确认期间切换对象、重建、删除重加后拒绝旧操作；空白区右键无版本菜单 |
| 提交时间与选择保持 | NUL 分隔日志、空 subject、本机时区显示；新增提交保持选择和滚动，编辑中延后监控刷新 |
| 危险操作确认 | 取消默认、对象 ID 与仓库代次捕获；旧确认不能操作新上下文 |
| 概览与远程展开 | 无边框路径可完整选择/复制/水平滚动；程序回填 Switch 与切换 Expander 不写配置 |
| Diff 与 AI 分析 | 原始文本、选择及滚动在主题切换后保持；成功、失败、离开、失效时停止 Spinner，旧 AI 响应不能覆盖新对象 |
| 紧凑 Diff 布局 | 内容宽度不足 640 时改为上下布局；AI Expander 内容和宿主同时限高至正文宿主的三分之一且不超过 200 |
| AI 设置与模型 | 配置兼容、服务商/Key/URL 切换后的请求失效；单个可编辑筛选框、手工模型、即时保存与获取进度 |
| 设置侧栏、搜索与返回 | 专用分类导航、完整设置名称/多词/大小写匹配、空状态与清除；Ctrl+, / Ctrl+F / Escape / 前后退；返回恢复版本选择、备份筛选与侧栏状态，删除/重建同步修正返回目标，浏览和搜索不写配置 |
| 通知与 Popover | 浮层不改变页面布局；长消息完整复制、详情暂停计时、关闭恢复计时、新通知关闭旧详情、Escape/外部点击及焦点返回 |
| 字体与窗口 | 字体角色、浅深色一致；原生窗口标志、1080×740 默认、760×520 最小、Sidebar 200–280 调整/收起、置顶及导航状态 |
| 密度与页面渲染 | 默认尺寸至少 8 个备份或 10 条历史；紧凑页面不产生整体横向滚动，长中文名称、超长路径、多行消息、空状态与加载状态截图 |

通知复制测试会保存并恢复原剪贴板格式。Windows 无界面 CTest 显式指定 Qt offscreen 插件目录。原生平台截图使用 Windows 平台插件及 WA_DontShowOnScreen；快捷键回归短暂显示使用临时数据的测试窗口来验证真实焦点。所有场景均不启动正常应用；截图为客户区，原生窗口标志另有断言。

## 复现

按 CONTRIBUTING.md 配置 Release 构建。以下是本机最终构建目录；Qt 和工具路径应按实际环境调整。

```powershell
& 'S:/Qt/Tools/CMake_64/bin/ctest.exe' --test-dir build/history-release --output-on-failure

$env:PATH = 'S:/Qt/6.8.3/msvc2022_64/bin;' + $env:PATH
$env:QT_QPA_PLATFORM = 'windows'
$env:QT_SCALE_FACTOR = '1'
$env:ZC_TEST_SCREENSHOTS = Join-Path $PWD 'build/history-actions/final-100'
& ./build/history-release/zc_tests.exe -o ./build/history-actions/final-native.txt,txt

# 分别将 scale 改为 1.5、2，输出目录改为 final-150、final-200。
$env:QT_SCALE_FACTOR = '1.5'
$env:ZC_TEST_SCREENSHOTS = Join-Path $PWD 'build/history-actions/final-150'
& ./build/history-release/zc_tests.exe renderAllPages -o ./build/history-actions/render-150.txt,txt
```

依次运行 UI 测试进程，避免剪贴板和焦点测试互相干扰。三个缩放比例通过 QT_SCALE_FACTOR 验证逻辑尺寸与渲染，不修改 Windows 显示设置。截图在控件短动画完成后采集。

不要用正常应用代替测试程序做回归：它会读取默认“文档”备份目录并启动扫描。截图场景只创建临时仓库，合成的大列表对象不创建监控。

## UI 现代化阶段验证记录（2026-09-25）

使用 Qt 6.8.3 / MSVC 2022 x64 / Ninja，在 build/modernization-final 中完成 Release 干净重建。该阶段构建包括新增的展示模型、QAction、图标与项目主题；其产物和检查记录保留如下。随后恢复工程命名和目录约定的验证见下一节。

| 检查 | 结果 |
| --- | --- |
| Release 干净重建 | 93 个构建步骤完成，应用、Qlementine 静态库和测试程序全部链接成功 |
| CTest regression | 1/1 测试套件通过；内部 Qt Test 18 项通过、0 失败、0 跳过，包含初始化和清理 |
| Windows 原生平台完整回归 | Qt Test 18 项通过、0 失败、0 跳过，包含初始化和清理 |
| 100% / 150% / 200% 页面渲染 | 浅深色七个页面、1080×740 / 760×520、长路径、空状态、加载、展开、通知和菜单均有截图 |
| 源码检查 | 没有应用 QSS、逐行 setIndexWidget 或装饰性 QGroupBox；上游 Qlementine 源码未修改 |
| Windows 部署目录 | 应用 SHA-256 与最终构建一致；包含 Qt / AI / Visual C++ 运行库、项目和 Qlementine/字体许可证 |
| 运行库边界 | 应用和 AI DLL 导入表无旧 UI 框架；33 个部署文件中没有 Ela、ZcWidgetTools、Qlementine 动态库或测试程序 |
| 安装器编译 | Inno Setup 6 成功生成本地安装器；没有执行安装、启动正常应用或发布 |

本地产物不纳入版本控制：

- 构建：`build/modernization-final`；干净重建日志：`build/modernization/clean-final-build.log`。
- 回归：`build/modernization/final-ctest.log`、`build/modernization-final/tests/regression.txt`、`build/modernization/native-regression.txt`。
- 截图：`build/modernization/accepted-100`、`accepted-150`、`accepted-200`；各目录含页面、紧凑布局、通知/Popover、菜单与加载状态。
- 打包日志与校验：`build/modernization/final-package.log`、`package-verification.json`、`package-imports.txt`。
- 部署目录：`dist/modernization/windows-v0.1.0`。
- 安装器：`dist/modernization/ZcVersionBox-v0.1.0-setup.exe`，47,454,886 字节。

该阶段应用 SHA-256：`8F6D002CA56E79D2F2463713AF1B74BFBDA95FCD3FA678958C77237EB82AB652`。

该阶段安装器 SHA-256：`1A711DF3EA6FD5AE815FB5633FD20A6B57877F9D65E2A6EE9BBFAFB90A404FAB`。

offscreen 平台插件可能输出字体目录和 propagateSizeHints 警告；它不替代 Windows 系统字体渲染检查。原生平台日志确认正文解析为 Microsoft YaHei UI、正常尺寸为 14 逻辑像素。

## 恢复工程约定后的复核（2026-09-25）

42 个源文件和表单归回 `windows/`、`windows/mainwindow_child/` 和 `utils/`，沿用小写文件名及 `homepage_page_*` 等原有前缀。HomePage、SettingPage 和对应 Designer 类恢复一致命名；布局、模型、服务职责与业务流程保留。GlobalConstants.h 与 HEAD 中的原文件 Git blob 一致，AppPaths 继续支持测试目录注入。

- 使用独立的 `build/conventions-release` 完成 93 个步骤的 Release 干净构建。
- CTest 与 Windows 原生平台完整回归均为 18 项通过、0 失败、0 跳过，包含初始化和清理。
- 核对 96 处项目 include / 自动生成表单引用，未发现路径大小写问题；应用 C++ / UI 文件名遵循原有小写约定。
- Windows 原生平台重新渲染七个页面及加载、展开、通知、菜单和紧凑状态，并复核 100% 截图。本次为命名与目录调整，150% / 200% 的视觉记录沿用上一阶段。
- 本地安装器重新编译；33 个部署文件通过运行库检查，包内程序与该次构建的 SHA-256 一致。
- 原有暂存区保留；相关源码、路径映射和暂存区快照位于 `build/conventions/before`。工程约定记录在根目录 AGENTS.md 与 CONTRIBUTING.md。

本次构建、回归、打包日志分别为 `build/conventions/build.log`、`ctest.log`、`native-regression.txt`、`package.log`；截图位于 `build/conventions/screenshots-100`，校验记录为 `build/conventions/package-verification.json`。

该阶段安装器：`dist/conventions/ZcVersionBox-v0.1.0-setup.exe`，47,454,166 字节；部署目录为 `dist/conventions/windows-v0.1.0`。

该阶段应用 SHA-256：`98A447EE70F716256BDC3E981DF973151C8F8CB706CF58578D8E040E8CBF6EA3`。

该阶段安装器 SHA-256：`C113F0E416F25773AB3198421F5985BCDCB7B4F0C0E4A3868BC421D7D8FDFF6F`。

## 设置结构调整后的复核（2026-09-25）

按照 Codex 设置页的结构，使用独立设置侧栏、搜索和返回入口，三类内容均采用居中、限宽的阅读区域。常规与 AI 设置按“名称和说明 / 控件”排成设置行，窄窗口将 AI 字段移到说明下方；保留 Switch、LoadingSpinner、密码输入、手工模型和即时保存。修改位于现有文件中，文件名、模块目录和业务服务保持原有约定。

- 在独立的 `build/settings-release` 中构建 Release；构建记录位于 `build/settings-layout/build.log`、`rebuild.log` 和 `final-build.log`。
- 最终 CTest 与 Windows 原生完整回归均为 19 项通过、0 失败、0 跳过，包含初始化和清理。日志为 `build/settings-layout/final-ctest.log`、`final-native.txt`。
- 新增设置导航回归覆盖完整名称搜索、大小写及多词查询、空结果、清除、键盘快捷键、前后退、历史选择恢复、折叠侧栏恢复，以及设置打开期间删除或重建对象。检查设置文件字节与 changed 信号，确认浏览/搜索不保存设置。
- 搜索隐藏 AI 内容时只停止 Spinner 展示，清除后恢复真实请求状态；模拟失败结束加载。现有服务商/密钥失效与手工模型回归继续通过。
- 设置额外覆盖 1440×920 居中限宽、1080×740 和 760×520；自定义 Base URL、搜索空状态与窄窗口滚动到末尾的操作均有截图。全部尺寸断言无整页水平滚动。
- 100% / 150% / 200% Windows 原生渲染均通过；截图位于 `build/settings-layout/final-100`、`final-150`、`final-200`，高 DPI 日志为 `render-150.txt`、`render-200.txt`。复核了浅深色、设置行对齐、完整 Custom 配置和窄窗口末尾的模型操作。
- 暂存区与 `build/settings-layout/before/git-index` 的 SHA-256 一致。没有新增应用 QSS、修改上游源码或重组源文件。

本地安装器为 `dist/settings-layout/ZcVersionBox-v0.1.0-setup.exe`，47,460,260 字节；部署目录为 `dist/settings-layout/windows-v0.1.0`。包内程序与验证过的构建 SHA-256 一致，33 个部署文件通过运行库检查。打包日志及校验记录位于 `build/settings-layout/package.log`、`package-verification.json`。

应用 SHA-256：`DB9C01126AB27779CB935AA7660EF99075DC957FD26157FAC55131B4B79CB6EE`。

安装器 SHA-256：`69ABCFCECE8D6F1C9E5DA5A16A1156D5410D515AD61D3B27565326FDD4CDB798`。

## 历史版本行内操作后的复核（2026-09-25）

版本操作移到历史行右侧，悬停或键盘焦点时显示预览、对比和更多。恢复与编辑说明位于版本菜单；顶部操作区保留刷新和对象菜单。实现继续使用原有 `homepage_page_backup.*`、QTableView、持久模型与私有 delegate，文件命名和目录结构沿用工程约定。

- 在独立的 `build/history-release` 中完成 Release 构建，日志为 `build/history-actions/build.log`、`final-build.log`。
- 最终 CTest 与 Windows 原生完整回归均为 21 项通过、0 失败、0 跳过，包含初始化和清理。日志为 `build/history-actions/final-ctest.log`、`final-native.txt`。
- 行内预览和对比直接使用所点版本；验证预览内容、源文件与 HEAD 保持、不同图标释放、移出视口和模型刷新时取消按下，以及双击、Enter、Alt+P、Shift+F10、Menu、F2 和菜单编辑。
- 菜单打开后改变选择或新增版本，操作仍指向原提交。恢复确认默认取消；确认期间切换对象、重建、删除并重加对象，旧操作均被拒绝。
- fast-import 构造 1001 条历史，表格 QWidget 数量与单版本时一致，滚动到最后一行仍可直接对比；原有说明编辑失败反馈、回填及选择保持回归继续通过。
- 100% / 150% / 200% 原生平台渲染通过，截图位于 `build/history-actions/final-100`、`final-150`、`final-200`，高 DPI 日志为 `render-150.txt`、`render-200.txt`。复核浅深色悬停、键盘焦点、版本菜单和 760×520 紧凑布局；字体宽度向上取整，修正分数缩放下时间末位被省略的问题。
- 隐藏渲染窗口通过 Qt 鼠标事件构造悬停状态，实际鼠标及键盘操作由原生测试窗口覆盖。图标缓存并随主题更新，绘制与命中检测不查询 Git，也不创建逐行按钮或应用 QSS。
- 七个已有页面、测试及文档文件的差异通过空白检查；暂存区与 `build/history-actions/before/git-index` 的 SHA-256 一致。

本地安装器为 `dist/history-actions/ZcVersionBox-v0.1.0-setup.exe`，47,465,827 字节；最终部署目录为 `dist/history-actions/windows-final-v0.1.0`。包内程序与最终测试构建的 SHA-256 一致，33 个部署文件通过运行库检查。打包与校验记录为 `build/history-actions/final-package.log`、`package-verification.json`。

应用 SHA-256：`744B64EC1BFEC95F438C53AC4432976232F2A53FAB58B89DE17C3DAD2B02500C`。

安装器 SHA-256：`7295C2AC01191BBDE5B99B07367FA3879376C6AFD54B380F6382641B9DE24619`。

## 平台验收边界

Windows 已完成本地构建、临时仓库自动化、原生平台客户区渲染和安装器编译。本轮没有安装/卸载应用、写入注册表/自启动设置或操作真实备份。

以下仍需在对应平台的隔离账户验证，本机自动化不能替代：

- Windows：实际安装/卸载，Explorer 文件/目录/背景右键入口，自启动启停，托盘关闭隐藏/恢复/退出，以及真实多显示器 DPI 和系统放大字号交互。
- Windows/macOS：真实原生标题栏、文件选择与确认对话框的完整键盘流程和焦点可见性。
- macOS arm64：Release 构建、DMG 部署、Finder 服务、LaunchAgent 自启动、窗口和托盘行为，以及最低支持系统上的启动。当前环境没有执行 macOS 构建或部署。

现有三套发布入口继续共用平台构建 action，固定 Qt 6.8.3。macOS 最低部署版本为 12.0，AI SDK 仍只提供 arm64；没有升级依赖、重写备份引擎、迁移真实数据、修改历史改写规则或触发发布流程。
