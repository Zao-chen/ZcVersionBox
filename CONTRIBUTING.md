# 开发指南

## 环境

- Qt 6.8.3：Widgets、Network、Svg；运行回归测试还需要 Qt Test。
- Windows：Visual Studio 2022 的 MSVC x64 工具链。
- macOS：Xcode Command Line Tools，最低部署版本 12.0；仓库自带的 AI SDK 仅支持 arm64。
- CMake 3.21 或更新版本、Git。使用 Ninja 生成器时另需 Ninja。

Qlementine v1.4.2 源码已固定纳入 `3rdparty/qlementine`，提交为 `13f72eb8b53bafd9ac24e5562d8ddc28d5440469`，通过静态库链接，配置阶段不下载依赖。来源、MIT 许可证和字体许可证见其 `UPSTREAM.md`、`LICENSE`、`LICENSES`。`ZcAILib` 保留为仓库内的预编译 AI SDK，不使用项目父目录中的依赖。

## 构建与测试

Windows 在 x64 Native Tools Command Prompt for VS 2022 中运行。Qt 路径请改为实际安装位置：

```bat
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=S:/Qt/6.8.3/msvc2022_64 -DBUILD_TESTING=ON
cmake --build build/release --parallel 4
ctest --test-dir build/release --output-on-failure
```

也可使用 Visual Studio 生成器：

```bat
cmake -S . -B build/msvc -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=S:/Qt/6.8.3/msvc2022_64 -DBUILD_TESTING=ON
cmake --build build/msvc --config Release --parallel 4
ctest --test-dir build/msvc -C Release --output-on-failure
```

如果本地化 MSVC 配合 Ninja 出现大量未被过滤的 `/showIncludes` 输出，应检查 CMake 检测的包含文件前缀和终端编码。头文件依赖未正确识别时，使用干净构建目录或 Visual Studio 生成器验证，不应信任增量构建结果。

macOS：

```bash
cmake -S . -B build/release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DCMAKE_PREFIX_PATH="$HOME/Qt/6.8.3/macos" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build/release --parallel 4
ctest --test-dir build/release --output-on-failure
```

CMake 会用 `lipo` 验证 AI SDK 包含全部指定架构。没有匹配 SDK 时，不能仅修改 `CMAKE_OSX_ARCHITECTURES` 就生成 x86_64 或通用包。应用包内 SDK 文件名为 `libZcAiLib.1.dylib`，与其加载标识一致。

只构建应用可设置 `-DBUILD_TESTING=OFF`。发布使用 Release；当前仓库提供的 AI SDK 二进制也是 Release。

## 打包

打包目录必须不存在，脚本不会从旧 `dist` 目录混入残留运行库。先完成上述构建和测试，再执行：

```powershell
# 在 VS 2022 开发者 PowerShell 中运行，windeployqt 必须在 PATH。
# 创建 setup.exe 还需要 Inno Setup 6。
./scripts/package-windows.ps1 -BuildDirectory build/release -OutputDirectory dist/windows -Tag v0.1.0
# 只准备可运行目录，可增加 -SkipInstaller。
```

```bash
# macdeployqt 必须在 PATH。
bash scripts/package-macos.sh build/release dist/macos v0.1.0 arm64
```

Windows 显式复制 `ZcAiLib.dll`，并要求部署 `vc_redist.x64.exe`；安装器在首次启动应用前安装 Visual C++ 运行库。使用可运行目录时，也需要先安装该运行库。macOS 显式复制 `libZcAiLib.1.dylib` 后再部署 Qt。脚本包含依赖检查、许可证复制；macOS 还执行 bundle 路径检查、架构校验和 ad-hoc 签名。签名不等同于 Developer ID 公证。

三套手动发布工作流共用平台构建 action，固定 Qt 6.8.3、MSVC 2022、macOS arm64/12.0，测试通过后生成安装包。发布资产只取 `pkg-*`，测试日志单独上传。

## 修改 UI

```text
ui/                   窗口、路由、独立页面与 Designer 表单
ui/components/        主题、导航展示、通知与确认对话框
services/             备份、应用级监控、设置、AI 传输
utils/                现有文件、恢复与 AI 辅助函数
3rdparty/qlementine/   固定版本的 Qlementine 源码
3rdparty/ZcAILib/      保留的 AI SDK
tests/                临时仓库与模拟 AI 的 Qt Test
scripts/              共用打包脚本
```

表单中的 Qlementine `Label` 使用 Designer 的提升控件机制，头文件为 `oclero/qlementine/widgets/Label.hpp`。不需要额外 Designer 插件即可编辑布局；实际样式由运行时统一应用。

Git、文件和配置操作应放进服务，通过结果/信号反馈给页面。新增页面不得拥有备份扫描定时器，不通过父对象层级或显示文案寻找其他页面。

详细边界和保留的 Git 限制见 [UI 架构](docs/ui-architecture.md)。测试覆盖、截图方式和平台验收见 [回归验证](docs/ui-regression.md)。回归请使用测试程序：正常启动应用会读取“文档”目录下的真实备份并启动监控。
