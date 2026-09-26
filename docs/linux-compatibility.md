# Linux 兼容与 SDK 0.2.0

本轮目标为 Ubuntu 22.04/24.04 x86_64、X11 与 Wayland，使用 Qt 6.8.3。应用版本保持 0.1.0，ZcAILib 升至 0.2.0，共享库 ABI/SONAME 保持 1。两个仓库都保留在 `feature/linux-compat`，验收前不推送、合并或发布。

## 安装与日常使用

```bash
sudo apt update
sudo apt install ./ZcVersionBox-v0.1.0-linux-amd64.deb
zcversionbox
```

可从桌面应用列表启动 ZcVersionBox。包安装到 `/opt/zcversionbox`，启动包装脚本为 `/usr/bin/zcversionbox`，桌面标识为 `com.zc.versionbox`，图标位于标准 hicolor 目录。Qt、SDK 和 ICU 使用私有运行库及相对 RPATH；Git、证书、OpenSSL 和窗口系统库由 apt 安装。不需要另外安装 Qt 开发环境。

在常规设置启用右键入口后，Nautilus 菜单位置为“右键 → 脚本 → 添加到 ZcVersionBox”。支持多选本地文件/文件夹，保留中文、空格、引号和百分号；逐项顺序添加并汇总结果。脚本通过 `NAUTILUS_SCRIPT_SELECTED_URIS` 传递本地 URI，不拼接所选路径为 shell 命令。已有位置参数继续可用，例如 `zcversionbox "/tmp/a.txt" "/tmp/b.txt"`；前导 `--` 可用于结束选项。

自启动文件位于 `$XDG_CONFIG_HOME/autostart/com.zc.versionbox.desktop`，默认是 `~/.config/autostart/`；脚本位于 `$XDG_DATA_HOME/nautilus/scripts/添加到 ZcVersionBox`，默认是 `~/.local/share/nautilus/scripts/`。重复启停幂等，只修改带有应用所有权标记的文件，拒绝覆盖其他文件或符号链接。写入失败时设置不会记为已启用。

关闭窗口时以实际托盘可用性决定行为：有托盘时隐藏，没有托盘时退出。命令添加在托盘或通知不可用时使用可见对话框反馈。GNOME 的托盘扩展属于桌面环境配置，应用不自行安装扩展。

## 数据与文件权限

备份仍位于系统“文档”目录的 `ZcVersionBox/Backup`，设置仍为旁边的 `config.ini`。JSON 记录格式保持 `format: 1`。旧指纹正常读取，通过成功备份流程刷新；Git 树没有变化时不创建空提交。待处理拉取和异常暂停状态继续保护仓库与源文件。

Linux 指纹包含 Git 可执行模式，inotify 的 `IN_ATTRIB` 会触发检查，单独改变所有者执行位也会创建版本。恢复以 Git 的 `100644` / `100755` 为准；完整 POSIX 权限、ACL、用户/组、扩展属性和空目录不属于 Git 历史。符号链接/子模块仍沿用已有拒绝规则。

安装、升级和 `sudo apt purge zcversionbox` 都保留用户备份、配置和用户级入口。卸载前可在设置中停用自启动及 Nautilus 入口，避免留下指向已卸载程序的入口。安装器不启动应用，也不自动迁移或删除旧数据。

## SDK 接入

应用默认使用 `3rdparty/ZcAILib` 中的固定源码快照，来源提交为 `275abb447916cf25c0e99e391048b307ed803621`。不下载 SDK、不查找父目录、不依赖旧 DLL/dylib。开发时可显式配置：

```powershell
cmake -S . -B build/sdk-dev -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH=S:/Qt/6.8.3/msvc2022_64 `
  -DZCVERSIONBOX_AI_SDK_SOURCE_DIR=P:/Qt/Project/ZcAILib
```

SDK 保留 `AiProvider` 方法、信号和请求行为，公开包含名在源码和安装后均为 `AiProvider.h`。原 `ZcAiLib` target 和 `ZcAiLib::ZcAiLib` 均可使用；静态库 target 为 `ZcAiLib::ZcAiLibStatic`。独立发布包含共享库、静态库、头文件、CMake package 和许可证，可用 `find_package(ZcAiLib 0.2 CONFIG REQUIRED)`；开发者仍需对应平台 Qt 6.8.3。应用嵌入时只构建共享库。

## Docker 构建与隔离验证

系统开发包和 aqt 安装参数见 `tests/linux/Dockerfile`，其中固定 Qt 6.8.3、CMake 3.30.5。Qt 的 `qtsvg`、`qtwayland`、`icu` 属于 base archives。依赖安装完成后，项目配置和构建可离线进行。

以下在仓库根目录执行，将当前提交复制到 Linux volume；不在 Windows 绑定目录中跑大小写、权限及 inotify 回归：

```bash
docker build -t zcversionbox-linux:qt6.8.3 tests/linux
docker volume create zcversionbox-linux-work
docker run -d --name zc-linux-build -v zcversionbox-linux-work:/workspace \
  zcversionbox-linux:qt6.8.3 sleep infinity
git archive --format=tar --output=build/linux-source.tar HEAD
docker cp build/linux-source.tar zc-linux-build:/workspace/source.tar
docker exec zc-linux-build bash -c 'mkdir -p /workspace/app && tar -xf /workspace/source.tar -C /workspace/app'
docker exec zc-linux-build bash -c 'cmake -S /workspace/app -B /workspace/app/build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DZCVERSIONBOX_DEPLOY_LINUX=ON -DCMAKE_INSTALL_LIBDIR=lib && cmake --build /workspace/app/build/linux --parallel 4 && ctest --test-dir /workspace/app/build/linux --output-on-failure'
docker exec zc-linux-build bash -c 'cd /workspace/app && bash tests/linux/run-desktop-smoke.sh build/linux/tests/zc_tests build/native-smoke && bash scripts/package-linux.sh build/linux dist/linux v0.1.0 && bash tests/linux/package-validation.sh build/linux build/validation'
```

首次归档前创建 `build` 目录。应用和验证输出目录须不存在。网络环境需要镜像时，可使用 Docker build 参数 `UBUNTU_IMAGE` 与 `UBUNTU_APT_MIRROR`；本轮使用 Ubuntu 官方 ECR 镜像及 USTC 的签名 Ubuntu 包镜像。

`tests/linux/Dockerfile.runtime` 不安装 Qt kit、编译器或项目源码。将 `.deb`、独立的 `validation.tar.gz` 及 `verify-package.sh` 复制到新 runtime 容器后执行：

```bash
bash /work/verify-package.sh /work/ZcVersionBox.deb /work/validation.tar.gz /work/results
```

这个脚本只允许在一次性 Docker 容器内以 root 安装/升级/purge；业务测试切换到 UID 1100 普通用户，在 Linux 文件系统中运行。验证包只额外包含 Qt Test 和 offscreen 插件，其他 Qt/SDK 依赖必须来自已安装 `.deb`。先执行两套完整测试，再用 Xvfb+Openbox 和 headless Weston 检查真实 QPA、窗口暴露、系统主题中的应用图标、SVG、TLS 与无托盘关闭，保存截图。测试始终使用临时仓库、隔离 Git 配置和模拟 AI，不启动正常应用或访问真实 AI/Git 服务。

升级用例以仅更改版本号的 `0.0.0~compat-fixture` 包作为前序，再升级到实际交付包；它不是历史发布包。安装前写入用户数据/配置/入口哨兵，在安装、升级、purge 后逐项核对。CI 的 Linux action 在 Ubuntu 22.04 和 24.04 上执行同一流程，发布步骤依赖验证成功。

## 本轮实际结果

| 平台和范围 | 结果 |
| --- | --- |
| Windows 11 / MSVC 2022 / Qt 6.8.3 | 应用和两套测试增量构建成功；regression：28 passed / 1 Linux-only skipped；backup_core：79 passed / 5 Linux-only skipped；URI 边界修正追加定向通过 |
| Ubuntu 22.04 / GCC 11 / Qt 6.8.3 | 应用完整编译通过；安装包运行时 regression：29 passed，backup_core：84 passed，无失败/跳过 |
| Ubuntu 24.04 / 同一 Ubuntu 22.04 安装包 | regression：29 passed，backup_core：84 passed，无失败/跳过 |
| 两个 Ubuntu 的 X11 与 Wayland | 各通过窗口、系统图标解析、SVG/TLS 插件和无托盘关闭测试，截图已检查 |
| 两个 Ubuntu 的安装、升级、purge | 用户备份、设置及用户级桌面入口均保留；无开发目录运行库依赖 |
| SDK / Windows MSVC、Ubuntu 22.04 GCC | 共享/静态各 10 passed；模型列表、普通响应、分段 UTF-8 SSE、错误回调、TLS 可用性；安装迁移后的共享/静态消费者通过 |
| macOS arm64、Windows MinGW | 已更新源码构建与发布流程，未实际执行 |

Qt Test 的 passed 数包含初始化和清理。Windows 完整两组日志为 `build/backup-core/tests/{regression,backup_core}.txt`，后续 URI 定向日志为 `build/windows-nautilus-uri.txt`；Linux 交付包结果与截图位于 `build/runtime-{22.04,24.04}-final/`。SDK 日志位于 SDK 仓库的 `build/linux-compat/` 和 `build/linux-sdk*.log`。

本地交付文件：

- 应用：`dist/linux/ZcVersionBox-v0.1.0-linux-amd64.deb`。
- SDK 仓库：`dist/0.2.0/ZcAILib-v0.2.0-windows-msvc.zip` 和 `dist/0.2.0/ZcAILib-v0.2.0-linux-gcc.tar.gz`。
- 哈希及构建信息随对应产物保存；不将构建产物加入 Git。

## 真实 GNOME 验收步骤（待用户验收）

1. 在 Ubuntu 22.04 和 24.04 安装同一 `.deb`，分别在 X11、Wayland 登录会话从应用列表启动，检查名称、图标、中文、缩放和文件选择框。
2. 用临时目录添加普通文件、目录、同目录的 `README`/`readme`；修改内容及仅大小写改名，检查历史、Diff 和恢复。对脚本执行 `chmod +x`，确认自动版本产生，再恢复此前版本并检查执行位关闭；恢复可执行版本时执行位开启。
3. 在 AI 设置选择服务商或自定义地址，填写自己的 Key，获取模型并生成一次提交说明/版本分析；真实外部 AI 的认证、网络及服务兼容性尚未自动验收。
4. 启用 Nautilus 入口，多选带中文、空格、引号和百分号的文件/目录，从“右键 → 脚本”添加；检查逐项结果及正在运行窗口的条目刷新。重复启停不产生额外文件。
5. 启用自启动，注销再登录确认启动；停用后再次登录确认不启动。入口目录不可写时应显示失败并保持原开关状态。
6. 分别在有托盘和无托盘的桌面检查关闭行为；有托盘可从托盘恢复/退出，无托盘关闭应结束进程。无托盘执行脚本添加时必须出现可见结果对话框。
7. 对已有备份执行升级和卸载，确认备份与配置保留；卸载前可关闭用户级入口。重新安装后确认原记录正常读取，待处理拉取/异常暂停条目仍维持保护。

容器的 Xvfb/Weston 验证不能替代真实 GNOME、托盘扩展、Nautilus 会话、登录自启动和多显示器体验验收。本轮没有 macOS 或 MinGW 执行结果，也没有推送、合并或发布。
