# UI 迁移验证

## 自动化范围

`tests/regression.cpp` 由 Qt Test 驱动，CTest 注册为 `regression`。测试使用独立 `QTemporaryDir`、临时 Git 配置、本地 bare 远程仓库和模拟 AI 回调；不读取用户 Git 配置，不调用系统右键菜单/自启动设置，不使用真实备份或凭据。

| 场景 | 回归证据 |
| --- | --- |
| 添加本地文件；自动备份；恢复及只读预览 | 临时中文/空格文件名，历史数量、恢复内容和 HEAD 保持情况断言 |
| 目录、隐藏文件、嵌套文件和二进制 | 实际快照、恢复内容及二进制 Diff 断言 |
| 首提交、按文件 Diff | 空树对比及未转义的中文文件路径读取断言 |
| pull / push、地址开关、重建 | 仅使用本地 bare 远程，检查版本数和地址保留 |
| 云端导入、取消、删除 | 暂存导入不进入监控列表；内容与临时目录清理断言 |
| AI 自动提交及失败回退 | 注入提交说明生成函数，验证 AI 说明、时间戳回退和旧仓库结果失效 |
| 刷新/销毁页面 | 监控实例数量和指纹持续有效，删除后清除监控 |
| 千条历史记录与说明编辑反馈 | 1000 次 fast-import 提交，持续使用相同模型，失败编辑回填原说明 |
| 同名文件、修改中文文案 | 卡片点击仍发出正确的备份 ID |
| AI 配置兼容、过期模型请求 | 旧配置迁移、切换服务商、修改 Key、手动模型及旧响应丢弃 |
| Diff 页面请求上下文 | 离开页面、删除并重新添加对象后，旧 AI 分析不能覆盖页面 |
| 设置回填及折叠 | 构造/刷新设置页不触发保存；展开收起不增删远程配置 |
| 键盘与窗口控件 | Space 控制远程开关；侧栏、前进后退和置顶状态断言 |
| 页面渲染 | 浅色/深色各页面、760×520 窗口、展开配置及空列表截图 |

Windows 无界面 CTest 显式指定 Qt offscreen 插件目录，避免 `windeployqt` 部署后的 Qt 安装路径影响测试。截图检查使用 Windows 平台插件及 `WA_DontShowOnScreen`，这样可使用真实系统字体而无需显示窗口。

## 复现

先按贡献指南构建并执行 CTest。以下 PowerShell 命令仅运行测试程序：

```powershell
$env:PATH = 'S:\Qt\6.8.3\msvc2022_64\bin;' + $env:PATH
$env:QT_QPA_PLATFORM = 'windows'
$env:QT_SCALE_FACTOR = '1.5' # 另测 1、2
$env:ZC_TEST_SCREENSHOTS = Join-Path $PWD 'build/screenshots-150'
& ./build/release/zc_tests.exe renderAllPages -o ./build/render-150.txt,txt
```

测试目录与系统 Qt 路径应按本机配置调整。截图渲染期间没有启动自动监控；不要用正常应用代替测试程序做截图或回归，它会读取默认备份目录并启动扫描。

## 本地验证记录（2026-09-25）

最终源码从 Git 暂存区导出至独立目录后，以 Qt 6.8.3 / MSVC 2022 x64 执行 Release 干净构建。已核对导出的 223 个文件与工作区内容一致（忽略换行符差异），没有遗漏的未跟踪依赖。

| 检查 | 结果 |
| --- | --- |
| 干净源码构建 | 应用、Qlementine 静态库及测试程序全部链接成功 |
| CTest `regression` | 通过；Qt Test 共 13 项通过、0 失败、0 跳过，包含初始化和清理 |
| Windows 页面渲染 | 100%、150%、200% 缩放全部通过；检查浅深色页面、760×520 窗口及长路径字段 |
| 长路径交互 | 只读字段可完整选择、复制并水平滚动；页面不产生横向滚动条 |
| Windows 部署 | 应用与干净构建产物 SHA-256 一致；包含 Qt、AI、Visual C++ 运行库及许可证 |
| 依赖清理 | Git 暂存区无旧 UI 目录；应用和 AI DLL 的导入表不含旧框架，部署目录无旧框架运行库 |
| 安装器编译 | Inno Setup 6 成功生成 `dist/ZcVersionBox-v0.1.0-setup.exe`；没有执行安装或启动正常应用 |

本地构建与测试记录位于 `build/migration/clean-build`，三个缩放比例的截图位于 `build/migration/accepted-100`、`accepted-150`、`accepted-200`，安装包构建日志为 `build/migration/accepted-package.log`。这些是本地产物，不纳入版本控制。

offscreen 回归日志中的字体目录和 `propagateSizeHints` 警告来自无界面平台插件；Windows 原生平台插件渲染没有这些警告。安装、卸载和系统集成的实际验证范围仍以下方清单为准。

## 平台验收

Windows Release 已在 Qt 6.8.3 / MSVC 2022 上完成干净构建和临时仓库回归。部署脚本只从新目录打包，显式包含 `ZcAiLib.dll`，检查 Qt Widgets、Network、Svg 及 Windows 平台插件。

三套发布入口共用 `.github/actions/build-windows` 和 `.github/actions/build-macos`，固定 Qt 6.8.3，测试失败即停止打包。macOS 构建目标为 12.0，打包前验证 AI SDK 架构，部署后检查 dylib 路径和签名。当前仓库的 AI SDK 只提供 arm64，选择其他架构会明确失败。

尚需在对应平台的隔离账户执行下列集成验收；本机 Windows 自动化结果不替代这些检查：

- Windows：Explorer 文件/目录/背景右键入口，启用/停用自启动，托盘关闭隐藏、恢复、退出，安装器安装/卸载。
- macOS arm64：Release 构建、DMG 部署、Finder 服务、LaunchAgent 自启动、原生窗口和托盘行为；在最低支持系统上启动检查。
- 使用键盘逐项检查文件选择与确认对话框、焦点可见性；确认删除和重建默认停留在取消操作。

没有向远程 Git 仓库或 AI 服务发送测试数据，也没有触发 GitHub 发布流程。
