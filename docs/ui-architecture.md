# UI 与服务边界

UI 使用 Qt 6.8.3、固定版本的 Qlementine v1.4.2，以及原生窗口标题栏。标准 Qt Widgets 组织窗口、页面、布局和数据视图；QlementineStyle 统一视觉，Label、Switch、Expander、LoadingSpinner、Popover 按交互用途使用。页面可以直接使用这些增强控件，服务、Route 和业务数据不依赖它们。

## 窗口与导航

默认窗口为 1080×740，最小为 760×520。原生 QSplitter 将窗口分为可收起的 Sidebar 和主区。Sidebar 默认宽 224，可调整到 200–280；内容为添加入口、全部备份、独立筛选、对象列表、设置及应用菜单。

单击对象默认进入历史版本。主区只有一个上下文头部，复用当前页面的 QAction；对象使用“历史版本 / 概览”局部导航，对比页增加当前“版本对比”页签。主题、置顶和关于入口位于应用菜单。

设置使用独立的侧栏模式：顶部为“返回应用”和设置搜索，下方为“常规 / AI / 关于”。进入设置时隐藏备份侧栏、应用上下文头部及对象页签，页面自身显示标题。两个侧栏由 Designer 中的 QStackedWidget 切换，公共应用菜单始终可用。返回应用恢复原 Route、备份筛选、版本选择和侧栏收起状态；设置模式中侧栏保持展开。

Ctrl+, 打开上次使用的设置分类，Ctrl+F 聚焦设置搜索；Escape 先清除搜索，再返回应用。搜索按现有设置的名称和关键词筛选分类，支持不区分大小写的多词匹配；没有结果时显示可清除的空状态。搜索不写配置、不改变服务商。显式导航和前后退可以清除不匹配的搜索条件，避免过滤条件阻挡导航。

Navigation 继续使用 PageId / Route 和明确的备份 ID，不依赖显示名称、枚举大小关系或堆叠控件下标。删除对象会清理全部相关路由；仓库失效会将旧对比路由改为该对象的历史页面。

## 文件职责

文件组织沿用原工程的 `windows/mainwindow_child/`、`homepage/pages/`、`homepage/trackfiles/` 与 `utils/`。原有页面和默认路径文件恢复原名；新增 C++ / UI 文件也采用小写名称和对应模块的下划线前缀。页面、服务与模型保持独立，文件位置不改变业务生命周期。

| 位置 | 职责 |
| --- | --- |
| main.cpp | 应用级服务和监控，既有命令行添加入口 |
| windows/mainwindow.* | Designer 窗口、上下文头部、Sidebar、页面装配、应用菜单、托盘和置顶 |
| windows/mainwindow_navigation.* | 路由、前进后退、删除与重建后的导航失效处理 |
| windows/mainwindow_child/homepage/trackfiles/homepagechild_trackfile.* | 只读备份展示模型、独立筛选代理、紧凑行 delegate 与行菜单交互 |
| windows/mainwindow_child/homepage/pages/homepage_page_trackfiles.* | 复用添加和对象 QAction，标准添加/导入/危险操作对话框 |
| windows/mainwindow_presentation.* | 字体/颜色角色、Qlementine 主题、少量样式钩子、通知与 Popover、标准确认 |
| windows/mainwindow_child/homepage/homepage.* | QListView、数量、名称/路径筛选、空状态 |
| windows/mainwindow_child/homepage/pages/homepage_page_backup.* | 持久历史模型、提交时间、选择和滚动保持、预览/恢复/对比/编辑反馈 |
| windows/mainwindow_child/homepage/pages/homepage_page_dashboard.* | 无边框可复制路径、紧凑统计、独立远程 Switch 和 Expander |
| windows/mainwindow_child/homepage/pages/homepage_page_diff.* | 文件列表、响应式 QSplitter、原始 QPlainTextEdit、语法高亮、AI 展开区 |
| windows/mainwindow_child/settingpage/settingpage.* | 独立常规和 AI 页面、即时保存、单个可筛选/可编辑模型框及加载状态 |
| windows/mainwindow_child/aboutpage/aboutpage.* | 小号图标、应用名称、版本和简介 |
| res/themes | 项目自己的浅深色主题，不修改上游主题或源码 |
| utils/backupservice.* | 后台串行任务、强类型异步完成、业务状态快照和通知；用例由 BackupEngine 编排 |
| utils/backupmonitor.* | 应用级扫描、请求合并和失败退避；成功指纹由 Catalog 持久保存 |
| utils/settingsservice.* | 配置兼容、即时保存、模型请求、平台集成设置 |
| utils/aigateway.* | AI 传输边界、超时、单次完成回调 |
| GlobalConstants.h | 原有 BackupPath / Settingpath 默认路径常量 |
| utils/apppaths.h | 引用默认路径并保留临时测试路径注入 |
| utils/backup_catalog、backup_files、backup_git、backup_engine | 记录、文件安全、Git 和用例边界，详见备份架构 |

每个页面继续有独立 Designer 表单。滚动宿主和普通 QWidget 只承担布局，不绘制通用 Card 或嵌套装饰容器；没有新建页面继承树、业务事件总线或 Qlementine 控件包装体系。

## 数据与生命周期

- MainWindow 将 trackedItems 快照交给一个 BackupListModel；Sidebar 和全部备份使用不同 QSortFilterProxyModel。模型按 ID 发出插入、移动、删除和更新信号，data() 和 delegate 不执行 Git 查询。列表不使用 setIndexWidget 或逐行 QWidget。
- Sidebar 筛选不会改动当前 Route。当前对象被隐藏时主区继续显示原上下文，清除筛选后恢复选中态。同名对象使用不同 ID。
- 设置模式保留最后一个应用 Route。删除备份同步清除返回目标，重建仓库将待返回的旧 Diff 改为历史页面；设置侧栏不拥有业务对象或后台任务。搜索隐藏 AI 页时停止 Spinner 动画，清除搜索后按真实请求状态恢复。
- 历史表格与 Diff 文件列表的模型只创建一次。历史按“提交说明 / 提交时间 / 短哈希”展示，窄窗口隐藏哈希列，tooltip 仍保留时间和哈希。
- 历史行末尾保留 108 逻辑像素的操作区，悬停或键盘焦点时显示预览、对比和更多；delegate 绘制图标并处理 28×28 命中区域，不创建逐行 QWidget。图标缓存并随主题更新，鼠标移动不重复解析 SVG。顶部只显示刷新和对象菜单。Alt+P 预览，Shift+F10 / Menu 键或右键打开原生版本菜单，恢复和编辑说明位于菜单内。
- 行内操作在按下与释放命中同一版本、同一图标时执行；离开视口或模型重置取消旧按下状态。菜单捕获备份 ID、提交哈希、仓库代次和页面代次，普通刷新或选择变化不改变其目标；切换对象、离开页面或仓库失效关闭菜单并使旧操作失效。恢复确认显示对象和版本，以取消为默认，确认后再次验证上下文。
- Revision 的 committedAt 来自 git log -z --format=%H%x00%h%x00%ct%x00%s。按 NUL 四元组解析并保留空 subject；完整 OID 用作业务身份，短哈希仅展示。使用提交者时间，以 UTC 保存、本机时区显示。
- 窗口生命周期内按备份 ID、仓库代次保存历史选择/滚动、概览滚动/折叠状态和各版本文件选择/正文滚动。删除或重建使旧状态失效。新提交保留选中的旧版本；没有选择时默认选中最新版本。
- 编辑说明通过菜单或 F2 开始；Enter 和双击激活对比。编辑期间的监控更新延后到编辑器关闭后显示，失败仍反馈并回填原说明。
- Diff 原文保存在只读、无换行的 QPlainTextEdit，QSyntaxHighlighter 只着色。切换主题不替换文档，不重置文本选择和滚动，也不改写二进制标记、制表符或 Git 元信息。
- 内容宽度不足 640 时，文件列表移到 Diff 上方。AI Expander 内容及宿主同时限高，至多 200 且不超过正文宿主的三分之一，避免 sizeHint 额外占用空间。
- AI 分析请求绑定页面代次、备份 ID、仓库代次。切换路由或仓库失效会停止加载并丢弃旧响应；置顶引起的原生窗口重建不改变业务上下文。隐藏窗口停止 Spinner 动画，原有请求机制不变。
- AI 模型请求由 SettingsService 管理，绑定服务商和请求代次。Key、URL 或服务商变化使旧请求失效；手动模型名称不要求出现在获取列表中。Spinner 表达异步 AI 请求；Git 和文件操作通过独立后台队列执行，概览另显示忙态。
- 回填 Switch 不写配置；展开/收起远程 Expander 只改变展示状态。关闭远程 Switch 仍移除已保存的地址，并在附近明确说明。
- 删除、重建确认在打开时捕获对象 ID 和仓库代次，默认按钮和焦点均为取消。导航变化不会将确认应用到另一个对象。
- 通知是主区子浮层，不参与页面布局。长消息在 Popover 中完整、可选择复制；展开详情暂停自动隐藏，关闭后恢复剩余计时。新通知关闭旧详情，Escape/外部点击可关闭，释放弹出窗口后返回焦点。浮层不拥有后台请求或业务提交的生命周期。
- BackupMonitor 属于应用，由 `main.cpp` 显式启动；500 ms 静默窗口合并事件，最长合并 5 s，完整检查后 30 s 再校验。独立扫描不占服务忙态；自动备份/审计走后台优先级，页面任务默认前台。创建/销毁页面、筛选、刷新和滚动不重建监控。

## 视觉规范

统一逻辑像素尺度为 4、8、12、16、24、32。标签与辅助文字间距 4，同组操作 8，字段 12，相邻内容块 16，独立分区 24。主内容左右边距 24，窄窗口 16；Sidebar 为 12。嵌套布局通常不再增加外边距。

设置页的滚动内容水平居中，内容列上限为 760，加上两侧内边距的宿主上限为 808。宽窗口顶部留白 64，窄窗口为 24。常规、AI 和关于使用相同的标题及阅读宽度。设置名称和辅助说明在左、Switch 或字段在右，分区通过标题和细分隔线组织。AI 字段宽 320，内容宽度不足 640 时改为上下排列；Custom 地址行随服务商显示。导航按钮使用完整行作为点击与选中区域，QStyle 仅调整标签绘制区域的起点，图标、文字和状态继续由 Qlementine 绘制。

常规输入与按钮最小高度 32；图标 16，点击区域至少 28。控件/选中项圆角 6，浮层圆角 8；固定内容不加阴影。Qlementine 动画基础时长为 120 ms。

| 字体角色 | 逻辑字号 | 字重 | Qlementine 映射 |
| --- | --- | --- | --- |
| 页面标题 | 20 | 600 | H1 / H2 |
| 对象标题、统计值 | 16 | 600 | H3 / H4 |
| 分区标题 | 14 | 600 | H5 |
| 正文、输入、次要按钮 | 14 | 400 | Default / 原生 QFont |
| Sidebar | 13 | 400 | 原生 QFont |
| 辅助信息、时间 | 12 | 400 | Caption |
| Diff、哈希 | 13 | 400 | 系统等宽字体 |

保留系统 UI 字体与中文回退，并按系统文字大小调整字体和控件度量。ThemeController 在 useSystemFonts 初始化后同步设置主题的字体对象和字号字段，原生 QFont 与 Qlementine Label 使用相同角色。系统已有时优先使用 Cascadia Mono、Consolas 或 Menlo 作为代码字体。

| 颜色角色 | 浅色 | 深色 |
| --- | --- | --- |
| 主画布 | #FAFAFA | #1C1C1E |
| Sidebar | #F3F3F3 | #18181A |
| 浮层 | #FFFFFF | #252527 |
| 主文字 | #242424 | #EDEDED |
| 辅助文字 | #666666 | #A2A2A6 |
| Hover | #EFEFEF | #28282B |
| Selected | #E7E7E7 | #343438 |
| 分隔 | #E5E5E5 | #333337 |

列表和导航使用中性背景变化，主动作使用黑白强调。次要动作使用轻量文字/图标按钮；每页至多一个高强调动作。Diff 同时使用低饱和颜色和原始 + / - 符号。样式通过主题、公开 QStyle 钩子、标准控件属性和少量 delegate 实现，不依赖应用 QSS。

## 备份业务边界

备份核心的职责、持久状态、默认文件范围和失败保护见 [备份架构](backup-architecture.md)。页面继续使用既有 Designer、QAction、列表模型和导航，不直接操作 Git 或复制文件。

- 历史、Diff、预览、统计和远程操作均为异步；页面校验对象、仓库代次和请求代次，过期响应不更新新页面。
- 概览新增同步状态、处理中的提示和 pull 待处理的两个操作。恢复和应用拉取版本先准备确认请求，确认后重新验证源指纹与版本身份。
- pull 改变受管理内容后暂停自动备份；关闭对话框、切换页面、修改地址或重启均不解除暂停。
- `build` 单独变化仍不触发备份，其他变化触发时仍可复制；提交继续遵守 Git ignore。
- 删除和重建仍使用原有确认流程；后台按捕获的对象和代次执行，不会操作后来切换的对象。
