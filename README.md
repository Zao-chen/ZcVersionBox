> [!TIP]
> 📦 ZcVersionBox 正在持续迭代中。欢迎通过 [Issues](https://github.com/Zao-chen/ZcVersionBox/issues) 反馈问题，或用 [Pull Request](https://github.com/Zao-chen/ZcVersionBox/pulls) 一起把文件版本管理做得更简单。

# ZcVersionBox

🌟 面向所有人的本地文件版本管理工具：不改造源目录，也能拥有 Git 级别的历史记录、预览、恢复与多人同步。🌟

<p align="center">
  <img width="180" src="res/img/logo_512.png" alt="ZcVersionBox logo" />
</p>

<p align="center">
  <a href="https://github.com/Zao-chen/ZcVersionBox/releases"><img src="https://img.shields.io/github/v/release/Zao-chen/ZcVersionBox?color=22c55e&style=for-the-badge" alt="GitHub Release" /></a>
  <a href="https://github.com/Zao-chen/ZcVersionBox/releases"><img src="https://img.shields.io/github/downloads/Zao-chen/ZcVersionBox/total?color=6366f1&style=for-the-badge" alt="GitHub Downloads" /></a>
  <a href="https://github.com/Zao-chen/ZcVersionBox/stargazers"><img src="https://img.shields.io/github/stars/Zao-chen/ZcVersionBox?color=f59e0b&style=for-the-badge" alt="GitHub Stars" /></a>
  <a href="LICENSE"><img src="https://img.shields.io/github/license/Zao-chen/ZcVersionBox?color=ef4444&style=for-the-badge" alt="License" /></a>
</p>

<p align="center">
  <a href="https://github.com/Zao-chen/ZcVersionBox/actions/workflows/ci.yml"><img src="https://img.shields.io/github/actions/workflow/status/Zao-chen/ZcVersionBox/ci.yml?branch=main&label=CI&style=for-the-badge" alt="CI" /></a>
  <img src="https://img.shields.io/badge/Qt-6.8.3-41CD52?logo=qt&logoColor=white&style=for-the-badge" alt="Qt 6.8.3" />
  <img src="https://img.shields.io/badge/platform-Windows%20%7C%20macOS%20%7C%20Linux-0ea5e9?style=for-the-badge" alt="Windows macOS Linux" />
</p>

## 🎯 项目介绍

ZcVersionBox 会为你选定的文件或文件夹建立独立的同步副本，并把副本作为普通 Git 仓库管理。源位置无需初始化 Git，也无需改变原有工作方式；应用负责监控变化、创建版本、展示历史，并在需要时恢复或同步。

### ✨ 核心特性

* 📁 **文件与文件夹都能管理：** 为每个对象建立独立版本线，源位置保持干净
* 🧭 **方案式版本管理：** 新建、切换、比较、合并、重命名和删除方案，并提供提交关系图
* 🕘 **历史记录一目了然：** 预览、Diff、恢复、编辑提交说明，或给重要版本添加里程碑
* 🤝 **多人协作同步：** 一处连接共享仓库，统一同步全部方案和里程碑；新方案、改名与删除也会同步
* 🧠 **AI 辅助理解变化：** 自动生成提交说明，并对独立版本差异进行分析
* 🖥️ **融入操作系统：** Windows 右键入口、macOS Finder 服务、Linux Nautilus 脚本和登录自启动
* 🎨 **专注于使用的界面：** Sidebar、前进/后退、置顶、浅色/深色主题、紧凑历史列表与响应式 Diff
* 🔔 **不打扰的更新提醒：** 启动后后台检查 GitHub Releases，通过应用内通知提醒新版本

### 🧩 它适合什么场景？

| 场景 | ZcVersionBox 的做法 |
| --- | --- |
| 想给资料、配置或素材保留历史 | 选择文件/文件夹即可，不要求源目录是 Git 仓库 |
| 想试验新方案，又不想弄乱当前版本 | 从当前方案创建独立方案，随时比较或合并 |
| 想找回某个过去的状态 | 在历史列表预览、对比，确认后恢复 |
| 想在多台设备之间共享版本 | 连接支持相关引用的共享 Git 仓库并同步 |

## 🗺 当前进度

- [x] 文件与文件夹的本地版本记录
- [x] 方案创建、切换、比较、合并、重命名与删除
- [x] 历史版本预览、Diff、恢复与里程碑
- [x] 多人协作页、共享仓库同步与离线保存
- [x] AI 提交说明与版本差异分析
- [x] Windows、macOS、Linux 的系统入口
- [x] GitHub Releases 更新检查与应用内更新入口
- [ ] 持续完善跨平台安装体验、迁移向导与文档

## 🚀 快速入门

### Step 1：安装并启动

1. 在 [Releases](https://github.com/Zao-chen/ZcVersionBox/releases) 下载对应平台的安装包。
2. 确保系统已安装 Git，然后启动 ZcVersionBox。
3. 从“添加备份”开始，选择文件、文件夹，或导入已有的云端方案。

### Step 2：创建第一个版本

1. 等待应用监控到文件变化，或按你的工作节奏保存一次修改。
2. 从 Sidebar 进入对象的历史页，查看版本、预览内容或打开 Diff。
3. 对重要版本添加里程碑；需要回到过去时，确认后执行恢复。

### Step 3：用方案安全试验

1. 在当前方案中创建一个新方案。
2. 在新方案里自由修改文件，原方案仍保持独立。
3. 通过“方案”菜单比较、合并、重命名或删除方案。

### Step 4：开启多人协作（可选）

1. 打开“多人协作”，连接一个支持 `atomic push` 和 `refs/zcversionbox/schemes` 引用的共享 Git 仓库。
2. 使用“立即同步”保存已落盘的本地修改、获取更新、整理文件并发送全部方案与里程碑。
3. 连接后默认自动同步：连续变化等待 3 秒合并，每 30 秒检查一次远端更新；断网时仍可继续本地保存。

完整流程见 [多人协作与双端验收](docs/collaboration.md) 和 [方案使用说明](docs/branch-management.md)。

### Step 5：配置 AI（可选）

在“设置 → AI”中配置服务。AI 可生成提交说明，也可以对版本差异做独立分析；不配置 AI 不影响本地备份、历史和恢复。

## 🛡️ 数据与安全边界

* 普通备份和预览不会写入源位置；恢复、导入、切换方案和有冲突的合并结果会在执行前要求确认。
* 只有当前对象真正进入冲突处理或需要检查时，才会暂停该对象的自动备份；其他对象继续工作。
* 未跟踪文件、被忽略文件不会按方案保存副本；隐藏文件会继续处理。链接，以及会递归包含备份存储的路径，会被明确拒绝。
* 数据默认保存在系统“文档”目录下的 `ZcVersionBox`，新条目使用 UUID 目录和独立记录。旧格式不会自动加载、迁移或删除。
* Linux 会记录并恢复 Git 的可执行模式（`100644` / `100755`）；完整 POSIX 权限、ACL、所有者和空目录不属于 Git 历史。

## 🐧 Linux 安装示例

Ubuntu 22.04/24.04 x86_64 可直接安装 `.deb`：

```bash
sudo apt install ./ZcVersionBox-v0.1.0-linux-amd64.deb
```

启动器名称为 **ZcVersionBox**，命令为 `zcversionbox`。启用设置中的右键入口后，在 Nautilus 中多选文件或文件夹，选择“右键 → 脚本 → 添加到 ZcVersionBox”。安装、升级和卸载会保留用户备份与配置；登录自启动和脚本入口可在卸载前关闭。

更多细节见 [Linux 兼容说明](docs/linux-compatibility.md)。

## 🛠️ 开发

### 技术栈

* **Qt 6.8.3**、C++17、标准 Qt Widgets 与 Qlementine
* **MSVC 2022**（Windows）、**macOS Universal**（arm64 + x86_64）或 **Linux/GCC**
* 仓库内固定源码：Qlementine v1.4.2、QWindowKit、efsw、ZcAILib 0.3.0
* 配置阶段不下载依赖；SDK 保留 `AiProvider` 接口与 ABI 1，并支持可安装的 CMake package

### 构建与验证

```bash
cmake -S . -B build
cmake --build build --config Release
```

构建、测试、打包和 Designer 编辑方式见 [开发指南](CONTRIBUTING.md)。备份状态与失败边界见 [备份架构](docs/backup-architecture.md)，页面边界见 [UI 架构](docs/ui-architecture.md)，软件更新流程见 [更新架构](docs/update-architecture.md)，验证范围见 [回归验证](docs/ui-regression.md)。

## 🤗 如何贡献

ZcVersionBox 是一个开源项目，欢迎用你最顺手的方式参与：

* **提交代码：** 欢迎 [Pull Request](https://github.com/Zao-chen/ZcVersionBox/pulls)，请先阅读 [开发指南](CONTRIBUTING.md)。
* **报告问题或建议：** 请通过 [Issues](https://github.com/Zao-chen/ZcVersionBox/issues) 提交可复现的问题、场景或想法。
* **分享项目：** 如果 ZcVersionBox 对你有帮助，给项目一个 ⭐ 就是很好的支持。

## 🔗 相关链接

* [ZcVersionBox Releases](https://github.com/Zao-chen/ZcVersionBox/releases)
* [开发指南](CONTRIBUTING.md)
* [多人协作与双端验收](docs/collaboration.md)
* [方案使用说明](docs/branch-management.md)
* [备份架构](docs/backup-architecture.md)
* [Linux 兼容说明](docs/linux-compatibility.md)
* [Zao-chen/ZcAILib](https://github.com/Zao-chen/ZcAILib)：Qt AI 请求库
* [Qlementine](https://github.com/oclero/qlementine)：Qt Widgets 主题与控件
* [Qt](https://www.qt.io/)：跨平台应用开发框架

## 📄 许可证

本项目基于 [GNU General Public License v3.0](LICENSE) 发布。
