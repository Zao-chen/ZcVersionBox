#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "windows/mainwindow_child/homepage/pages/homepage_page_branches.h"
#include <QComboBox>
#include "utils/update_service.h"
#include "windows/mainwindow_child/aboutpage/aboutpage.h"
#include "windows/mainwindow_child/homepage/homepage.h"
#include "windows/mainwindow_child/homepage/pages/homepage_page_backup.h"
#include "windows/mainwindow_child/homepage/pages/homepage_page_dashboard.h"
#include "windows/mainwindow_child/homepage/pages/homepage_page_diff.h"
#include "windows/mainwindow_child/homepage/pages/homepage_page_conflict.h"
#include "windows/mainwindow_child/homepage/pages/homepage_page_trackfiles.h"
#include "windows/mainwindow_child/homepage/trackfiles/homepagechild_trackfile.h"
#include "windows/mainwindow_child/settingpage/settingpage.h"
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QTextEdit>
#include <QShowEvent>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QVariantAnimation>
#include <oclero/qlementine/style/QlementineStyle.hpp>
#include <algorithm>
#include <type_traits>
#include <QWKWidgets/widgetwindowagent.h>
#ifdef Q_OS_MACOS
#include "macos/macos_services.h"
#endif

namespace
{
class NotificationDot final : public QWidget
{
public:
    explicit NotificationDot(QWidget *parent = nullptr) : QWidget(parent)
    {
        setObjectName("overviewNotificationBadge");
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
        setFixedSize(7, 7);
        hide();
    }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(UiStyle::colors().removed);
        p.drawEllipse(rect());
    }
};
#ifdef Q_OS_MACOS
// Qt 6.8 applies the macOS titlebar safe area (the contentLayoutRect inset)
// as contents margins on widgets that keep WA_ContentsMarginsRespectsSafeArea
// enabled, pushing the title bar row below the traffic lights even though the
// native window draws full-size content. The inset moves to the next widget
// that has not opted out, so the whole tree has to opt out.
void disableSafeAreaInsets(QWidget *root)
{
    root->setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
    const auto children = root->findChildren<QWidget *>();
    for (auto *child : children)
        child->setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
}
#endif

bool matchesSettings(PageId page, const QString &query)
{
    QString keywords;
    switch (page)
    {
    case PageId::GeneralSettings:
        keywords = "常规 设置 外观 主题 跟随系统 浅色 深色 选择界面颜色跟随系统或固定使用浅色深色 系统集成 右键菜单快捷入口 从文件或文件夹的系统菜单直接添加备份 开机自动启动 登录系统后启动 ZcVersionBox 继续自动备份 自启动";
        break;
    case PageId::AiSettings:
        keywords = "AI 设置 自动化 自动生成提交说明 使用AI为自动备份生成简短的变更说明 服务配置 服务商 选择生成提交说明和分析变更的AI服务 OpenAI DeepSeek Custom API Key 凭据 密钥 模型 model 获取模型 手工输入模型名称 自定义 Base URL 服务地址 兼容OpenAI的服务接口地址";
        break;
    case PageId::About:
        keywords = "关于 设置 应用 版本 ZcVersionBox Qt Qlementine";
        break;
    default:
        return false;
    }
    const auto words = query.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    return std::all_of(words.cbegin(), words.cend(), [&](const QString &word)
                       { return keywords.contains(word, Qt::CaseInsensitive); });
}
} // namespace

MainWindow::MainWindow(BackupService *backups, SettingsService *settings, AiGateway *gateway, ThemeController *theme, bool trayEnabled, QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow), m_backups(backups), m_settings(settings), m_theme(theme)
{
    setAttribute(Qt::WA_DontCreateNativeAncestors);
    ui->setupUi(this);
#ifdef Q_OS_MACOS
    disableSafeAreaInsets(this);
#endif
    setWindowTitle("ZcVersionBox");
    resize(1080, 740);
    setMinimumSize(760, 520);
    UiStyle::surface(ui->titleBar, UiStyle::Surface::Sidebar);
    UiStyle::surface(ui->sidebar, UiStyle::Surface::Sidebar);
    UiStyle::surface(ui->sidebarContent, UiStyle::Surface::Sidebar);
    UiStyle::surface(ui->content, UiStyle::Surface::Canvas);
    UiStyle::surface(ui->sidebarList, UiStyle::Surface::Sidebar);
    UiStyle::text(ui->contextTitle, UiStyle::FontRole::Title);
    UiStyle::text(ui->sidebarFilter, UiStyle::FontRole::Sidebar);
    UiStyle::text(ui->settingsSearch, UiStyle::FontRole::Sidebar);
    UiStyle::text(ui->settingsGroupLabel, UiStyle::FontRole::Caption, true);
    ui->settingsGroupLabel->setContentsMargins(8, 0, 0, 0);
    UiStyle::text(ui->settingsEmptyTitle, UiStyle::FontRole::Object);
    UiStyle::text(ui->settingsEmptyDescription, UiStyle::FontRole::Caption, true);
    ui->settingsEmptyState->hide();
#ifdef Q_OS_MACOS
    // QWindowKit's Cocoa agent requires a native NSWindow. Headless Qt
    // platforms provide synthetic window IDs which are not Cocoa objects.
    const bool useWindowAgent = QGuiApplication::platformName() == "cocoa";
#else
    const bool useWindowAgent = true;
#endif
    if (useWindowAgent)
    {
        m_windowAgent = new QWK::WidgetWindowAgent(this);
        m_windowAgent->setup(this);
        m_windowAgent->setTitleBar(ui->titleBar);
    }
#ifndef Q_OS_MAC
    if (m_windowAgent)
    {
        m_windowAgent->setSystemButton(QWK::WindowAgentBase::Minimize, ui->minimizeButton);
        m_windowAgent->setSystemButton(QWK::WindowAgentBase::Maximize, ui->maximizeButton);
        m_windowAgent->setSystemButton(QWK::WindowAgentBase::Close, ui->closeButton);
    }
    connect(ui->minimizeButton, &QToolButton::clicked, this, &QWidget::showMinimized);
    connect(ui->maximizeButton, &QToolButton::clicked, this, [this] {
        if (isMaximized())
            showNormal();
        else
            showMaximized();
    });
    connect(ui->closeButton, &QToolButton::clicked, this, &QWidget::close);
#else
    ui->minimizeButton->hide();
    ui->maximizeButton->hide();
    ui->closeButton->hide();
    auto *macButtonsArea = new QWidget(ui->titleBar);
    macButtonsArea->setObjectName("macButtonsArea");
    macButtonsArea->setFixedSize(72, 36);
    ui->titleBarLayout->insertWidget(0, macButtonsArea);
    // Traffic light buttons measure 14pt tall starting 14pt from the window
    // top, so their center line sits at y=21; center the 28pt nav buttons on
    // the same line (7pt top + 14pt half-height + 1pt bottom). The nav
    // buttons pack tightly (2pt) while a fixed 12pt spacer keeps them clear
    // of the lights, which end at x=72.
    ui->titleBarLayout->setContentsMargins(0, 7, 4, 1);
    ui->titleBarLayout->setSpacing(2);
    ui->titleBarLayout->insertSpacing(1, 12);
    if (m_windowAgent)
        m_windowAgent->setSystemButtonArea(macButtonsArea);
#endif
    if (m_windowAgent)
    {
        m_windowAgent->setHitTestVisible(ui->collapseButton, true);
        m_windowAgent->setHitTestVisible(ui->backButton, true);
        m_windowAgent->setHitTestVisible(ui->forwardButton, true);
    }
    for (auto *button : {ui->historyTab, ui->overviewTab, ui->returnHistoryButton, ui->generalTab, ui->aiTab, ui->aboutTab,
                         ui->backupsButton, ui->settingsButton, ui->returnApplicationButton,
                         ui->addSidebarButton, ui->collapseButton, ui->backButton, ui->forwardButton,
                         ui->moreButton, ui->appMenuButton})
        button->setFocusPolicy(Qt::TabFocus);
    qApp->installEventFilter(this);
    for (auto *button : {ui->backupsButton, ui->settingsButton, ui->returnApplicationButton, ui->generalTab, ui->aiTab, ui->aboutTab})
        UiStyle::text(button, UiStyle::FontRole::Sidebar);
    for (auto *button : {ui->backupsButton, ui->settingsButton, ui->returnApplicationButton, ui->generalTab, ui->aiTab, ui->aboutTab})
        button->setProperty("navigationItem", true);
    auto *searchIcon = UiStyle::action(ui->settingsSearch, "settingsSearchIcon", "搜索设置", "search");
    ui->settingsSearch->addAction(searchIcon, QLineEdit::LeadingPosition);
    connect(searchIcon, &QAction::triggered, this, [this]
            { ui->settingsSearch->setFocus(Qt::ShortcutFocusReason); });
    ui->headerLayout->setContentsMargins(24, 12, 8, 8);
    ui->tabsLayout->setContentsMargins(24, 0, 24, 8);
    ui->windowSplitter->setStretchFactor(0, 0);
    ui->windowSplitter->setStretchFactor(1, 1);
    ui->windowSplitter->setCollapsible(0, true);
    ui->windowSplitter->setCollapsible(1, false);
    ui->windowSplitter->setSizes({224, 856});
    ui->sidebar->setMinimumWidth(200);
    ui->sidebar->setMaximumWidth(280);
    ui->sidebar->installEventFilter(this);
    ui->header->installEventFilter(this);
    ui->contextTitle->installEventFilter(this);
    m_overviewBadge = new NotificationDot(ui->overviewTab);
    ui->overviewTab->installEventFilter(this);
    m_aboutBadge = new NotificationDot(ui->aboutTab);
    m_aboutBadge->setObjectName("aboutNotificationBadge");
    ui->aboutTab->installEventFilter(this);
    connect(ui->windowSplitter, &QSplitter::splitterMoved, this, [this]
            {
        if (ui->sidebar->width() >= 200)
            m_sidebarWidth = ui->sidebar->width();
        updateHeaderLayout(); });
    m_sidebarAnimation = new QVariantAnimation(this);
    m_sidebarAnimation->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_sidebarAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value)
            {
        const int w = value.toInt();
        ui->sidebar->setFixedWidth(w);
        const int total = ui->windowSplitter->width();
        ui->windowSplitter->setSizes({w, qMax(0, total - w)}); });
    connect(m_sidebarAnimation, &QVariantAnimation::finished, this, [this]
            {
        const bool collapsed = (m_sidebarTargetWidth == 0);
        const int total = ui->windowSplitter->width();
        if (collapsed)
        {
            ui->sidebar->setVisible(false);
            ui->sidebar->setMinimumWidth(200);
            ui->sidebar->setMaximumWidth(280);
            ui->windowSplitter->setSizes({0, total});
        }
        else
        {
            ui->sidebar->setVisible(true);
            ui->sidebar->setMinimumWidth(200);
            ui->sidebar->setMaximumWidth(280);
            ui->windowSplitter->setSizes({m_sidebarWidth, qMax(0, total - m_sidebarWidth)});
            ui->sidebarContent->setGeometry(0, 0, m_sidebarWidth, ui->sidebar->height());
        }
        m_sidebarTargetWidth = -1;
        updateHeaderLayout(); });
    m_actions = new BackupUiActions(backups, this);
    m_backupModel = new BackupListModel(this);
    m_sidebarFilter = new BackupFilterModel(this);
    m_sidebarFilter->setSourceModel(m_backupModel);
    m_sidebarFilter->sort(0);
    ui->sidebarList->setModel(m_sidebarFilter);
    UiStyle::flatView(ui->sidebarList);
    ui->sidebarList->setAccessibleName("备份对象");
    auto *delegate = new BackupItemDelegate(ui->sidebarList, true);
    ui->sidebarList->setItemDelegate(delegate);
    connect(delegate, &BackupItemDelegate::menuRequested, this, [this](const QString &id, const QPoint &position)
            { m_actions->showObjectMenu(id, position); });
    const auto open = [this](const QModelIndex &index)
    {
        if (index.isValid())
        {
            const auto id = index.data(BackupListModel::IdRole).toString();
            navigate({PageId::History, id});
        }
    };
    connect(ui->sidebarList, &QListView::clicked, this, open);
    connect(ui->sidebarList, &QListView::activated, this, open);
    connect(ui->sidebarFilter, &QLineEdit::textChanged, this, [this](const QString &text)
            {
        m_sidebarFilter->setFilterFixedString(text);
        syncSidebarSelection(); });
    ui->addSidebarButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    connect(ui->addSidebarButton, &QToolButton::clicked, m_actions->addAction(), &QAction::trigger);
    m_actions->addAction()->setShortcut(QKeySequence::New);
    addAction(m_actions->addAction());
    connect(m_actions->addAction(), &QAction::triggered, this, [this]
            {
        const bool sidebarVisible = (m_sidebarAnimation && m_sidebarAnimation->state() == QAbstractAnimation::Running)
                                        ? (m_sidebarTargetWidth > 0)
                                        : (ui->sidebar->isVisible() && ui->sidebar->width() > 0);
        auto *anchor = sidebarVisible ? ui->addSidebarButton : ui->collapseButton;
        m_actions->addMenu()->popup(anchor->mapToGlobal(QPoint(0, anchor->height()))); });

    m_notifications = new NotificationBar(ui->content);
    m_updates = new UpdateService(this);
    m_list = new HomePage(backups, this, m_backupModel, m_actions);
    m_dashboard = new HomePageDashboardPage(backups, this);
    m_history = new HomePageBackupPage(backups, this);
    m_diff = new HomePageDiffPage(backups, settings, gateway, this);
    m_conflict = new HomePageConflictPage(backups, this);
    m_branches = new HomePageBranchesPage(backups, this);
    m_general = new SettingPage(settings, theme, this);
    m_ai = new SettingPageAiPage(settings, this);
    m_about = new AboutPage(m_updates, this);
    connect(m_updates, &UpdateService::automaticUpdateAvailable, this, [this](const UpdateRelease &release)
            {
        m_about->setAvailableUpdate(release);
        m_hasUnreadUpdate = m_route.page != PageId::About;
        updateUpdateBadge();
        m_notifications->showResult(OperationResult::info(
            QStringLiteral("发现新版本"),
            QStringLiteral("ZcVersionBox v%1 已发布。").arg(release.version), 8000),
            QStringLiteral("查看更新"), [this]
            {
                restoreWindow();
                navigate({PageId::About});
                m_about->showAvailableUpdate();
            }); });
    for (QWidget *page : QList<QWidget *>{m_list, m_dashboard, m_history, m_diff, m_conflict, m_branches, m_general, m_ai, m_about})
        ui->pages->addWidget(page);
    const auto wire = [this](auto *page)
    {
        connect(page, &std::remove_pointer_t<decltype(page)>::navigate, &m_navigation, &Navigation::go);
        connect(page, &std::remove_pointer_t<decltype(page)>::notification, this, &MainWindow::notify);
    };
    wire(m_list);
    wire(m_dashboard);
    wire(m_history);
    wire(m_diff);
    wire(m_conflict);
    wire(m_branches);
    connect(m_branches, &HomePageBranchesPage::replaceRoute, &m_navigation, &Navigation::replaceCurrent);
    connect(m_history, &HomePageBackupPage::branchRequested, this, [this](const QString &ref)
    {
        if (ref.isEmpty()) return;
        Route route{PageId::Branches, m_route.backupId};
        route.branchRef = ref;
        navigate(route);
    });
    connect(m_history, &HomePageBackupPage::createBranchRequested, m_branches, &HomePageBranchesPage::createFrom);
    connect(ui->branchSelector, &QComboBox::activated, this, [this](int index)
    {
        const auto id = m_route.backupId, ref = ui->branchSelector->itemData(index).toString();
        const auto current = m_backups->branchContext(id).ref;
        { const QSignalBlocker blocker(ui->branchSelector); ui->branchSelector->setCurrentIndex(ui->branchSelector->findData(current)); }
        if (!ref.isEmpty() && ref != current) m_branches->requestSwitch(id, ref);
    });
    wire(m_general);
    wire(m_actions);
    connect(m_ai, &SettingPageAiPage::navigate, &m_navigation, &Navigation::go);
    connect(settings, &SettingsService::notification, this, &MainWindow::notify);
    connect(&m_navigation, &Navigation::changed, this, &MainWindow::displayRoute);
    connect(ui->backButton, &QToolButton::clicked, &m_navigation, &Navigation::back);
    connect(ui->forwardButton, &QToolButton::clicked, &m_navigation, &Navigation::forward);
    connect(new QShortcut(QKeySequence::Back, this), &QShortcut::activated, &m_navigation, &Navigation::back);
    connect(new QShortcut(QKeySequence::Forward, this), &QShortcut::activated, &m_navigation, &Navigation::forward);
    connect(new QShortcut(QKeySequence("Ctrl+B"), this), &QShortcut::activated, this, &MainWindow::toggleSidebar);
    connect(new QShortcut(QKeySequence("Ctrl+,"), this), &QShortcut::activated, this, &MainWindow::openSettings);
    connect(new QShortcut(QKeySequence::Find, ui->settingsSidebar), &QShortcut::activated, this, [this]
            {
        ui->settingsSearch->setFocus(Qt::ShortcutFocusReason);
        ui->settingsSearch->selectAll(); });
    connect(ui->backupsButton, &QToolButton::clicked, this, [this]
            {
        navigate({});
        ui->backupsButton->setChecked(true); });
    connect(ui->settingsButton, &QToolButton::clicked, this, &MainWindow::openSettings);
    connect(ui->returnApplicationButton, &QToolButton::clicked, this, &MainWindow::returnToApplication);
    ui->returnHistoryButton->setCursor(Qt::PointingHandCursor);
    UiStyle::text(ui->diffTitleLabel, UiStyle::FontRole::Caption, true);
    connect(ui->returnHistoryButton, &QToolButton::clicked, this, [this]
    {
        if (m_route.page == PageId::Branches)
        {
            if (m_navigation.canBack() && m_navigation.previous().page == PageId::History)
                m_navigation.back();
            else
                navigate({PageId::History, m_route.backupId});
        }
        else if (m_route.page == PageId::Conflict)
        {
            if (m_navigation.canBack() && (m_navigation.previous().page == PageId::History || m_navigation.previous().page == PageId::Branches))
                m_navigation.back();
            else
                navigate({PageId::Dashboard, m_route.backupId});
        }
        else
        {
            navigate({PageId::History, m_route.backupId});
        }
    });
    connect(m_diff, &HomePageDiffPage::titleChanged, ui->diffTitleLabel, &QLabel::setText);
    connect(new QShortcut(QKeySequence(Qt::Key_Escape), this), &QShortcut::activated, this, [this]
            {
        // A single window shortcut avoids ambiguous Escape registrations when
        // the settings sidebar and the main window are both visible.
        if (isSettingsPage(m_route.page))
        {
            if (!ui->settingsSearch->text().isEmpty())
                ui->settingsSearch->clear();
            else
                returnToApplication();
        }
        else if (m_route.page == PageId::Diff)
            navigate({PageId::History, m_route.backupId});
        else if (m_route.page == PageId::Branches)
        {
            if (m_navigation.canBack() && m_navigation.previous().page == PageId::History)
                m_navigation.back();
            else
                navigate({PageId::History, m_route.backupId});
        }
        else if (m_route.page == PageId::Conflict)
        {
            if (m_navigation.canBack() && (m_navigation.previous().page == PageId::History || m_navigation.previous().page == PageId::Branches))
                m_navigation.back();
            else
                navigate({PageId::Dashboard, m_route.backupId});
        } });
    connect(ui->settingsSearch, &QLineEdit::textChanged, this, &MainWindow::updateSettingsSearch);
    connect(ui->clearSettingsSearchButton, &QPushButton::clicked, ui->settingsSearch, &QLineEdit::clear);
    connect(ui->collapseButton, &QToolButton::clicked, this, &MainWindow::toggleSidebar);
    const QList<QPair<QToolButton *, PageId>> tabs{
        {ui->historyTab, PageId::History}, {ui->overviewTab, PageId::Dashboard}, {ui->generalTab, PageId::GeneralSettings}, {ui->aiTab, PageId::AiSettings}, {ui->aboutTab, PageId::About}};
    for (const auto &tab : tabs)
    {
        auto *button = tab.first;
        const auto page = tab.second;
        button->setAutoExclusive(true);
        connect(button, &QToolButton::clicked, this, [this, page]
                {
            navigate(isObjectPage(page) ? Route{page, m_route.backupId} : Route{page}); });
    }
    connect(ui->moreButton, &QToolButton::clicked, this, [this]
            { m_actions->showObjectMenu(m_route.backupId, ui->moreButton->mapToGlobal(QPoint(0, ui->moreButton->height()))); });
    auto *menu = new QMenu(this);
    menu->setObjectName("applicationMenu");
    auto *themeAction = UiStyle::action(this, "themeAction", "切换明暗主题", "theme");
    auto *pinAction = UiStyle::action(this, "pinAction", "窗口置顶", "pin");
    auto *aboutAction = UiStyle::action(this, "aboutAction", "关于 ZcVersionBox", "info");
    pinAction->setCheckable(true);
    menu->addActions({themeAction, pinAction, aboutAction});
    menu->addSeparator();
    menu->addAction("退出", qApp, &QApplication::quit);
    connect(ui->appMenuButton, &QToolButton::clicked, menu, [this, menu]
            { menu->popup(ui->appMenuButton->mapToGlobal(QPoint(0, 0)) - QPoint(0, menu->sizeHint().height())); });
    connect(themeAction, &QAction::triggered, theme, &ThemeController::toggle);
    connect(aboutAction, &QAction::triggered, this, [this]
            {
        ui->settingsSearch->clear();
        navigate({PageId::About}); });
    connect(pinAction, &QAction::toggled, this, [this](bool checked)
            {
        const auto state = windowState();
        const bool wasVisible = isVisible();
        setWindowFlag(Qt::WindowStaysOnTopHint, checked);
        setWindowState(state);
        if (wasVisible)
            show(); });
    connect(theme, &ThemeController::changed, this, [this]
            {
        updateIcons();
        m_diff->refreshTheme();
        m_conflict->refreshTheme();
        updateHeaderLayout(); });
    connect(backups, &BackupService::trackedItemsChanged, this, &MainWindow::syncBackups);
    connect(backups, &BackupService::repositoryChanged, this, [this](const QString &id)
            {
        if (id == m_route.backupId)
        { updateConflictBadge(); updateBranchSelector(); } });
    connect(backups, &BackupService::repositoryInvalidated, this, [this](const QString &id)
            {
        if ((m_applicationRoute.page == PageId::Diff || m_applicationRoute.page == PageId::Conflict) && m_applicationRoute.backupId == id)
            m_applicationRoute = {PageId::History, id};
        m_navigation.invalidateRevisions(id); });
    if (trayEnabled)
    {
        m_tray = new QSystemTrayIcon(windowIcon(), this);
        auto *trayMenu = new QMenu(this);
        trayMenu->addAction("主窗口", this, &MainWindow::restoreWindow);
        trayMenu->addAction("退出", qApp, &QApplication::quit);
        m_tray->setContextMenu(trayMenu);
        connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason)
                {
            if (reason == QSystemTrayIcon::Trigger)
                restoreWindow();
            else if (reason == QSystemTrayIcon::DoubleClick)
                m_tray->showMessage("ZcVersionBox运行中", "右击托盘图标打开菜单", QSystemTrayIcon::Information, 3000); });
        m_tray->show();
    }
    syncBackups();
    updateIcons();
    displayRoute({});
}
MainWindow::~MainWindow()
{
    qApp->removeEventFilter(this);
}
void MainWindow::syncBackups()
{
    const auto items = m_backups->trackedItems();
    m_backupModel->setItems(items);
    QSet<QString> ids;
    for (const auto &item : items)
        ids.insert(item.id);
    if (!m_applicationRoute.backupId.isEmpty() && !ids.contains(m_applicationRoute.backupId))
        m_applicationRoute = {};
    m_navigation.retainBackups(ids);
    syncSidebarSelection();
    m_list->refresh();
    updateConflictBadge();
}
void MainWindow::syncSidebarSelection()
{
    const auto &route = isSettingsPage(m_route.page) ? m_applicationRoute : m_route;
    const auto index = m_sidebarFilter->mapFromSource(m_backupModel->indexForId(route.backupId));
    // Filtering only changes what is visible. The route retains the object's ID.
    if (index.isValid())
        ui->sidebarList->setCurrentIndex(index);
    else
    {
        ui->sidebarList->clearSelection();
        ui->sidebarList->setCurrentIndex({});
    }
}
void MainWindow::toggleSidebar()
{
    if (isSettingsPage(m_route.page))
        return;
    const bool isRunning = m_sidebarAnimation && m_sidebarAnimation->state() == QAbstractAnimation::Running;
    const bool show = isRunning ? (m_sidebarTargetWidth == 0) : (!ui->sidebar->isVisible() || ui->sidebar->width() == 0);
    animateSidebar(show);
}
void MainWindow::animateSidebar(bool show)
{
    if (isSettingsPage(m_route.page))
        return;

    const bool isRunning = m_sidebarAnimation && m_sidebarAnimation->state() == QAbstractAnimation::Running;
    if (!show && !isRunning && ui->sidebar->isVisible() && ui->sidebar->width() >= 200)
        m_sidebarWidth = ui->sidebar->width();

    const auto *qlementineStyle = qobject_cast<const oclero::qlementine::QlementineStyle *>(this->style());
    const bool animations = qlementineStyle ? qlementineStyle->animationsEnabled() : true;
    const int baseDuration = (animations && isVisible()) ? this->style()->styleHint(QStyle::SH_Widget_Animation_Duration) : 0;

    if (baseDuration <= 0)
    {
        if (isRunning)
            m_sidebarAnimation->stop();
        m_sidebarTargetWidth = -1;
        ui->sidebar->setMinimumWidth(200);
        ui->sidebar->setMaximumWidth(280);
        ui->sidebar->setVisible(show);
        const int total = ui->windowSplitter->width();
        if (show)
        {
            ui->sidebarContent->setGeometry(0, 0, m_sidebarWidth, ui->sidebar->height());
            ui->windowSplitter->setSizes({m_sidebarWidth, qMax(0, total - m_sidebarWidth)});
        }
        else
        {
            ui->windowSplitter->setSizes({0, total});
        }
        updateHeaderLayout();
        return;
    }

    const int targetWidth = show ? m_sidebarWidth : 0;
    if (!isRunning && ui->sidebar->isVisible() == show && ui->sidebar->width() == targetWidth)
        return;

    ui->sidebar->setMinimumWidth(0);
    int currentWidth = 0;
    if (isRunning)
    {
        currentWidth = m_sidebarAnimation->currentValue().toInt();
        m_sidebarAnimation->stop();
    }
    else
    {
        currentWidth = show ? 0 : ui->sidebar->width();
        if (show)
        {
            ui->sidebar->setFixedWidth(0);
            ui->sidebar->setVisible(true);
            const int total = ui->windowSplitter->width();
            ui->windowSplitter->setSizes({0, total});
            ui->sidebarContent->setGeometry(-m_sidebarWidth, 0, m_sidebarWidth, ui->sidebar->height());
        }
    }

    m_sidebarTargetWidth = targetWidth;
    const int distance = qAbs(targetWidth - currentWidth);
    const int animDuration = qMax(40, static_cast<int>(baseDuration * (double(distance) / qMax(1, m_sidebarWidth))));

    m_sidebarAnimation->setDuration(animDuration);
    m_sidebarAnimation->setStartValue(currentWidth);
    m_sidebarAnimation->setEndValue(targetWidth);
    m_sidebarAnimation->start();
}
void MainWindow::openSettings()
{
    if (auto *focus = QApplication::focusWidget())
        focus->clearFocus();
    ui->settingsSearch->clear();
    navigate({m_settingsPage});
}
void MainWindow::returnToApplication()
{
    if (auto *focus = QApplication::focusWidget())
        focus->clearFocus();
    if (isSettingsPage(m_route.page))
        navigate(m_applicationRoute);
}
void MainWindow::updateSettingsSearch()
{
    if (!isSettingsPage(m_route.page))
    {
        ui->settingsEmptyState->hide();
        ui->pages->show();
        return;
    }
    const auto query = ui->settingsSearch->text();
    const QList<QPair<QToolButton *, PageId>> categories{
        {ui->generalTab, PageId::GeneralSettings}, {ui->aiTab, PageId::AiSettings}, {ui->aboutTab, PageId::About}};
    PageId firstMatch = PageId::GeneralSettings;
    bool found = false;
    for (const auto &[button, page] : categories)
    {
        const bool matches = matchesSettings(page, query);
        button->setVisible(matches);
        if (matches && !found)
        {
            firstMatch = page;
            found = true;
        }
    }
    ui->settingsGroupLabel->setVisible(found);
    ui->pages->setVisible(found);
    ui->settingsEmptyState->setVisible(!found);
    if (found && !matchesSettings(m_route.page, query))
        navigate({firstMatch});
}
void MainWindow::updateIcons()
{
    const QList<QPair<QToolButton *, QString>> icons{
        {ui->backButton, "back"},
        {ui->forwardButton, "forward"},
        {ui->collapseButton, "menu"},
        {ui->minimizeButton, "window_minimize"},
        {ui->maximizeButton, isMaximized() ? "window_restore" : "window_maximize"},
        {ui->closeButton, "close"},
        {ui->backupsButton, "folder"},
        {ui->settingsButton, "settings"},
        {ui->appMenuButton, "more"},
        {ui->moreButton, "more"},
        {ui->addSidebarButton, "add"},
        {ui->returnApplicationButton, "back"},
        {ui->returnHistoryButton, "back"},
        {ui->generalTab, "settings"},
        {ui->aiTab, "sparkles"},
        {ui->aboutTab, "info"}};
    for (const auto &[button, name] : icons)
        button->setIcon(m_theme->icon(name));
    for (auto *action : findChildren<QAction *>())
    {
        const auto name = action->property("iconName").toString();
        if (!name.isEmpty())
            action->setIcon(m_theme->icon(name));
    }
    m_actions->refreshIcons();
    ui->sidebarList->viewport()->update();
    if (m_overviewBadge)
        m_overviewBadge->update();
    if (m_aboutBadge)
        m_aboutBadge->update();
}
void MainWindow::setToolbar(const QList<QAction *> &actions)
{
    m_toolbarButtons.clear();
    while (auto *item = ui->toolbarLayout->takeAt(0))
    {
        if (auto *widget = item->widget())
        {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    for (auto *action : actions)
    {
        if (action->property("primary").toBool())
        {
            auto *button = new QPushButton(action->icon(), action->text(), ui->toolbarHost);
            button->setObjectName(action->objectName() + "Button");
            button->setDefault(true);
            button->setAutoDefault(false);
            button->setMenu(action->menu());
            button->setMinimumHeight(32);
            connect(button, &QPushButton::clicked, action, &QAction::trigger);
            connect(action, &QAction::changed, button, [button, action]
                    {
                button->setIcon(action->icon());
                button->setEnabled(action->isEnabled()); });
            ui->toolbarLayout->addWidget(button);
        }
        else
        {
            auto *button = UiStyle::toolButton(ui->toolbarHost, action, action->property("iconOnly").toBool());
            button->setObjectName(action->objectName() + "Button");
            ui->toolbarLayout->addWidget(button);
            m_toolbarButtons.append(button);
        }
    }
    updateHeaderLayout();
}
void MainWindow::displayRoute(const Route &route)
{
    if (isObjectPage(route.page) && (route.backupId.isEmpty() || !m_backups->contains(route.backupId)))
    {
        if (route.backupId.isEmpty())
            m_navigation.go({});
        else
            m_navigation.removeBackup(route.backupId);
        return;
    }
    if (route.page == PageId::Branches && route.branchRef.isEmpty())
    {
        m_navigation.replaceCurrent({PageId::History, route.backupId});
        return;
    }
    if (m_route.page == PageId::History)
        m_history->deactivate();
    if (m_route.page == PageId::Dashboard)
        m_dashboard->deactivate();
    if (m_route.page == PageId::Diff)
        m_diff->deactivate();
    if (m_route.page == PageId::Conflict)
        m_conflict->deactivate();
    const bool settingsMode = isSettingsPage(route.page);
    const bool wasSettingsMode = isSettingsPage(m_route.page);
    if (settingsMode && !wasSettingsMode)
    {
        if (m_sidebarAnimation && m_sidebarAnimation->state() == QAbstractAnimation::Running)
            m_sidebarAnimation->stop();
        m_applicationRoute = m_route;
        auto *focus = QApplication::focusWidget();
        m_applicationFocus = focus && isAncestorOf(focus) ? focus : nullptr;
        bool isSidebarVisible = !ui->sidebar->isHidden() && ui->sidebar->width() > 0;
        if (m_sidebarTargetWidth >= 0)
            isSidebarVisible = (m_sidebarTargetWidth > 0);
        m_applicationSidebarVisible = isSidebarVisible;
        if (ui->sidebar->width() >= 200)
            m_sidebarWidth = ui->sidebar->width();
        m_sidebarTargetWidth = -1;
        ui->sidebar->setMinimumWidth(200);
        ui->sidebar->setMaximumWidth(280);
        ui->windowSplitter->setCollapsible(0, false);
        ui->sidebar->show();
        ui->sidebarContent->setGeometry(0, 0, m_sidebarWidth, ui->sidebar->height());
        ui->windowSplitter->setSizes({m_sidebarWidth, width() - m_sidebarWidth});
    }
    else if (!settingsMode && wasSettingsMode)
    {
        if (m_sidebarAnimation && m_sidebarAnimation->state() == QAbstractAnimation::Running)
            m_sidebarAnimation->stop();
        m_sidebarTargetWidth = -1;
        ui->sidebar->setMinimumWidth(200);
        ui->sidebar->setMaximumWidth(280);
        ui->windowSplitter->setCollapsible(0, true);
        ui->sidebar->setVisible(m_applicationSidebarVisible);
        if (m_applicationSidebarVisible)
        {
            ui->sidebarContent->setGeometry(0, 0, m_sidebarWidth, ui->sidebar->height());
            ui->windowSplitter->setSizes({m_sidebarWidth, width() - m_sidebarWidth});
        }
        else
        {
            ui->windowSplitter->setSizes({0, width()});
        }
    }
    // Explicit navigation/back/forward takes precedence over a search filter.
    if (!settingsMode || (!(route == m_route) && !matchesSettings(route.page, ui->settingsSearch->text())))
    {
        const QSignalBlocker blocker(ui->settingsSearch);
        ui->settingsSearch->clear();
    }
    m_route = route;
    if (settingsMode)
        m_settingsPage = route.page;
    else
        m_applicationRoute = route;
    if (auto *focus = QApplication::focusWidget())
        focus->clearFocus();
    ui->sidebarStack->setCurrentWidget(settingsMode ? ui->settingsSidebar : ui->applicationSidebar);
    ui->header->setVisible(!settingsMode);
    ui->settingsButton->setVisible(!settingsMode);
    ui->collapseButton->setVisible(!settingsMode);
    m_actions->addAction()->setEnabled(!settingsMode);
    m_actions->setCurrentBackup(route.backupId);
    QWidget *page = m_list;
    QList<QAction *> actions;
    switch (route.page)
    {
    case PageId::Backups:
        m_list->refresh();
        actions = m_list->toolbarActions();
        break;
    case PageId::Dashboard:
        page = m_dashboard;
        m_dashboard->setBackup(route.backupId);
        actions = m_dashboard->toolbarActions();
        break;
    case PageId::History:
        page = m_history;
        m_history->setBackup(route.backupId, route.commit, route.branchRef);
        actions = m_history->toolbarActions();
        break;
    case PageId::Diff:
        page = m_diff;
        m_diff->setRevision(route.backupId, route.commit, route.oldCommit);
        actions = m_diff->toolbarActions();
        break;
    case PageId::Branches:
        page = m_branches;
        m_branches->setBackup(route.backupId, route.branchRef);
        actions = m_branches->toolbarActions();
        break;
    case PageId::Conflict:
        page = m_conflict;
        m_conflict->setBackup(route.backupId, route.branchRef);
        break;
    case PageId::GeneralSettings:
        page = m_general;
        break;
    case PageId::AiSettings:
        page = m_ai;
        if (!route.provider.isEmpty() && m_settings->provider() != route.provider)
            m_settings->selectProvider(route.provider);
        m_ai->refresh();
        break;
    case PageId::About:
        page = m_about;
        break;
    }
    ui->pages->setCurrentWidget(page);
    if (auto *focus = QApplication::focusWidget())
        focus->clearFocus();
    ui->backButton->setEnabled(m_navigation.canBack());
    ui->forwardButton->setEnabled(m_navigation.canForward());
    ui->backupsButton->setChecked(route.page == PageId::Backups);
    ui->settingsButton->setChecked(isSettingsPage(route.page));
    const bool isObject = isObjectPage(route.page);
    ui->tabs->setVisible(isObject);
    const bool isDiff = (route.page == PageId::Diff);
    const bool isConflict = (route.page == PageId::Conflict);
    const bool isBranches = (route.page == PageId::Branches);
    const bool isStandalone = isDiff || isConflict || isBranches;
    ui->returnHistoryButton->setVisible(isStandalone);
    if (isConflict)
    {
        const bool fromHistory = (m_navigation.canBack() && m_navigation.previous().page == PageId::History);
        const bool fromBranches = (m_navigation.canBack() && m_navigation.previous().page == PageId::Branches);
        const QString text = fromHistory ? "返回历史版本" : fromBranches ? "返回方案" : "返回概览";
        ui->returnHistoryButton->setText(text);
        ui->returnHistoryButton->setToolTip(text + " (Esc)");
        ui->returnHistoryButton->setAccessibleName(text);
    }
    else
    {
        ui->returnHistoryButton->setText("返回历史版本");
        ui->returnHistoryButton->setToolTip("返回历史版本 (Esc)");
        ui->returnHistoryButton->setAccessibleName("返回历史版本");
    }
    ui->diffTitleLabel->setVisible(isDiff);
    ui->historyTab->setVisible(!isStandalone && isObject);
    ui->overviewTab->setVisible(!isStandalone && isObject);
    updateBranchSelector();
    ui->historyTab->setChecked(route.page == PageId::History);
    ui->overviewTab->setChecked(route.page == PageId::Dashboard);
    ui->generalTab->setChecked(route.page == PageId::GeneralSettings);
    ui->aiTab->setChecked(route.page == PageId::AiSettings);
    ui->aboutTab->setChecked(route.page == PageId::About);
    if (route.page == PageId::About)
        m_hasUnreadUpdate = false;
    updateUpdateBadge();
    ui->moreButton->setVisible(isObjectPage(route.page));
    m_contextTitle = isObjectPage(route.page)     ? QFileInfo(m_backups->sourcePath(route.backupId)).fileName()
                     : isSettingsPage(route.page) ? "设置"
                                                  : "全部备份";
    ui->contextTitle->setToolTip(isObjectPage(route.page) ? QDir::toNativeSeparators(m_backups->sourcePath(route.backupId)) : m_contextTitle);
    setToolbar(actions);
    syncSidebarSelection();
    updateSettingsSearch();
    if (!settingsMode && wasSettingsMode && m_applicationFocus && m_applicationFocus->isVisible() && m_applicationFocus->isEnabled())
        m_applicationFocus->setFocus(Qt::OtherFocusReason);
    m_notifications->raise();
    updateConflictBadge();
}
void MainWindow::updateHeaderLayout()
{
    static bool updating = false;
    if (updating)
        return;
    updating = true;
    const bool compact = ui->content->width() < 640;
    const int margin = compact ? 16 : 24;
    for (auto *button : m_toolbarButtons)
        button->setToolButtonStyle(compact || button->defaultAction()->property("iconOnly").toBool() ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
    ui->headerLayout->setContentsMargins(margin, 12, 8, 8);
    ui->tabsLayout->setContentsMargins(margin, 0, margin, 8);
    ui->contextTitle->setText(ui->contextTitle->fontMetrics().elidedText(m_contextTitle, Qt::ElideRight, qMax(0, ui->contextTitle->width())));
    updating = false;
}
bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if ((watched == ui->header || watched == ui->contextTitle) && event->type() == QEvent::Resize)
        updateHeaderLayout();

    if (watched == ui->overviewTab && (event->type() == QEvent::Resize || event->type() == QEvent::Move) && m_overviewBadge)
    {
        m_overviewBadge->move(ui->overviewTab->width() - m_overviewBadge->width() - 3, 3);
        m_overviewBadge->raise();
    }
    if (watched == ui->aboutTab && (event->type() == QEvent::Resize || event->type() == QEvent::Move) && m_aboutBadge)
    {
        m_aboutBadge->move(ui->aboutTab->width() - m_aboutBadge->width() - 3, 3);
        m_aboutBadge->raise();
    }

    if (watched == ui->sidebar && event->type() == QEvent::Resize)
    {
        const int w = ui->sidebar->width();
        const int contentW = qMax(w, m_sidebarWidth);
        const int x = w < m_sidebarWidth ? (w - m_sidebarWidth) : 0;
        ui->sidebarContent->setGeometry(x, 0, contentW, ui->sidebar->height());
    }

    if (event->type() == QEvent::MouseButtonPress)
    {
        UiStyle::setKeyboardNavigationActive(false);

        if (auto *widget = qobject_cast<QWidget *>(watched))
        {
            if (widget->focusPolicy() == Qt::NoFocus && !qobject_cast<QMenu *>(widget) && !qobject_cast<QScrollBar *>(widget))
            {
                if (auto *focus = QApplication::focusWidget())
                {
                    if (isAncestorOf(focus) && !focus->isAncestorOf(widget))
                        focus->clearFocus();
                }
            }
        }
    }
    else if (event->type() == QEvent::KeyPress)
    {
        const auto *keyEvent = static_cast<QKeyEvent *>(event);
        const int key = keyEvent->key();
        if (key == Qt::Key_Tab || key == Qt::Key_Backtab)
        {
            UiStyle::setKeyboardNavigationActive(true);
        }
        else if (key == Qt::Key_Up || key == Qt::Key_Down || key == Qt::Key_Left || key == Qt::Key_Right ||
                 key == Qt::Key_PageUp || key == Qt::Key_PageDown || key == Qt::Key_Home || key == Qt::Key_End)
        {
            auto *focus = QApplication::focusWidget();
            if (!qobject_cast<QLineEdit *>(focus) && !qobject_cast<QTextEdit *>(focus) && !qobject_cast<QPlainTextEdit *>(focus))
            {
                UiStyle::setKeyboardNavigationActive(true);
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}
void MainWindow::updateConflictBadge()
{
    const bool isObject = isObjectPage(m_route.page);
    const auto state = isObject ? m_backups->syncState(m_route.backupId) : BackupSyncState::Tracking;
    const bool hasConflict = isObject && (state == BackupSyncState::ResolutionPending || state == BackupSyncState::RemotePending);
    const bool isStandalone = (m_route.page == PageId::Diff || m_route.page == PageId::Conflict || m_route.page == PageId::Branches);
    const bool badgeVisible = hasConflict && !isStandalone;
    if (m_overviewBadge)
    {
        m_overviewBadge->setVisible(badgeVisible);
        if (badgeVisible)
        {
            m_overviewBadge->move(ui->overviewTab->width() - m_overviewBadge->width() - 3, 3);
            m_overviewBadge->raise();
        }
    }
    ui->overviewTab->setToolTip(hasConflict ? "概览 (存在同步差异，请继续处理)" : "概览");
}
void MainWindow::updateUpdateBadge()
{
    const bool badgeVisible = m_hasUnreadUpdate && m_route.page != PageId::About;
    if (m_aboutBadge)
    {
        m_aboutBadge->setVisible(badgeVisible);
        if (badgeVisible)
        {
            m_aboutBadge->move(ui->aboutTab->width() - m_aboutBadge->width() - 3, 3);
            m_aboutBadge->raise();
        }
    }
    ui->aboutTab->setToolTip(m_hasUnreadUpdate ? "关于 (有新版本)" : "关于");
}
void MainWindow::notify(const OperationResult &result) { m_notifications->showResult(result); }
void MainWindow::startAutomaticUpdateCheck()
{
    m_updates->startAutomaticCheck();
}
void MainWindow::restoreWindow()
{
    if (isMinimized())
        showNormal();
    else
        show();
    raise();
    activateWindow();
}
void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_tray && QSystemTrayIcon::isSystemTrayAvailable())
    {
        hide();
        event->ignore();
    }
    else
    {
        QMainWindow::closeEvent(event);
        if (m_tray)
            QCoreApplication::quit();
    }
}
void MainWindow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::WindowStateChange)
    {
        const bool max = isMaximized();
        if (ui->maximizeButton)
        {
            ui->maximizeButton->setToolTip(max ? "还原" : "最大化");
            ui->maximizeButton->setIcon(m_theme->icon(max ? "window_restore" : "window_maximize"));
        }
    }
    QMainWindow::changeEvent(event);
}
void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
#ifdef Q_OS_MACOS
    disableSafeAreaInsets(this);
    if (QGuiApplication::platformName() == "cocoa")
        setupMacTitleBar(winId());
#endif
}

void MainWindow::updateBranchSelector()
{
    const bool visible = isObjectPage(m_route.page) && m_route.page != PageId::Branches;
    ui->branchSelector->setVisible(visible);
    const auto request = ++m_branchRequest;
    if (!visible) return;
    const auto id = m_route.backupId;
    const auto context = m_backups->branchContext(id);
    ui->branchSelector->setEnabled(m_backups->syncState(id) == BackupSyncState::Tracking);
    m_backups->branches(id, this, [this, id, request, context](const BackupResult<BranchSnapshot> &result)
    {
        if (request != m_branchRequest || id != m_route.backupId || !result.result.success) return;
        const QSignalBlocker blocker(ui->branchSelector);
        ui->branchSelector->clear();
        for (const auto &branch : result.value.branches)
            if (!branch.remoteBranch) ui->branchSelector->addItem((branch.current ? "正在使用：" : "切换到：") + branch.name, branch.ref);
        ui->branchSelector->setCurrentIndex(ui->branchSelector->findData(result.value.context.ref));
        ui->branchSelector->setToolTip("正在使用：" + result.value.context.ref.mid(11));
    });
}
