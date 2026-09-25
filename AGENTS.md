# ZcVersionBox 工程约定

- 沿用用户已有的文件命名和目录结构。C++ / Qt Designer 文件名使用小写，模块拆分遵循已有下划线前缀，例如 `homepage_page_backup.cpp`；类名使用 Qt 风格的 PascalCase。
- 已有文件保留原名和大小写，包括 `GlobalConstants.h`、`CMakeLists.txt`。修改 UI 或功能时，不顺带改换命名体系或重组顶层目录。
- 主窗口位于 `windows/mainwindow.*`，子页面位于 `windows/mainwindow_child/{homepage,settingpage,aboutpage}/`；首页分区使用 `homepage/pages/homepage_page_*`，备份条目展示使用 `homepage/trackfiles/`。新增文件就近放入原模块。
- 通用业务、监控、配置和文件辅助放在 `utils/`，窗口导航、主题和通知辅助放在 `windows/mainwindow_*`。保持页面、服务、模型和监控生命周期独立。
- 保留 Designer、标准 Qt Widgets 与 QlementineStyle；Switch、Expander、LoadingSpinner、Popover 等增强控件按实际用途使用。
- 分支名不使用 `codex` 前缀。
- 回归使用临时仓库、隔离 Git 配置和模拟 AI 的 `zc_tests`。正常应用会读取真实备份并启动监控，不能用来运行自动化回归。
