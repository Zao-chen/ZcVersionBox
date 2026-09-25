#include "mainwindow.h"
#include "ui/pages/aboutPage.h"
#include "ui/pages/backupsPage.h"
#include "ui/pages/dashboardPage.h"
#include "ui/pages/diffPage.h"
#include "ui/pages/historyPage.h"
#include "ui/pages/settingsPage.h"
#include "ui_mainwindow.h"
#include <QApplication>
#include <QCloseEvent>
#include <QMenu>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSystemTrayIcon>
#include <type_traits>

MainWindow::MainWindow(BackupService *backups, SettingsService *settings, AiGateway *gateway, ThemeController *theme, bool trayEnabled, QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow), m_backups(backups), m_settings(settings), m_theme(theme)
{
    ui->setupUi(this);
    setWindowTitle("ZcVersionBox");
    resize(1080, 740);
    setMinimumSize(760, 520);
    m_breadcrumb = new BreadcrumbBar(this);
    ui->headerLayout->insertWidget(3, m_breadcrumb, 1);
    m_notifications = new NotificationBar(this);
    ui->contentLayout->addWidget(m_notifications);
    m_list = new BackupsPage(backups, this);
    m_dashboard = new DashboardPage(backups, this);
    m_history = new HistoryPage(backups, this);
    m_diff = new DiffPage(backups, settings, gateway, this);
    m_general = new GeneralSettingsPage(settings, this);
    m_ai = new AiSettingsPage(settings, this);
    m_about = new AboutPage(this);
    for (QWidget *page : QList<QWidget *>{m_list, m_dashboard, m_history, m_diff, m_general, m_ai, m_about})
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
    wire(m_general);
    connect(m_ai, &AiSettingsPage::navigate, &m_navigation, &Navigation::go);
    connect(settings, &SettingsService::notification, this, &MainWindow::notify);
    connect(m_breadcrumb, &BreadcrumbBar::navigate, &m_navigation, &Navigation::go);
    connect(&m_navigation, &Navigation::changed, this, &MainWindow::displayRoute);
    connect(ui->backButton, &QToolButton::clicked, &m_navigation, &Navigation::back);
    connect(ui->forwardButton, &QToolButton::clicked, &m_navigation, &Navigation::forward);
    connect(new QShortcut(QKeySequence::Back, this), &QShortcut::activated, &m_navigation, &Navigation::back);
    connect(new QShortcut(QKeySequence::Forward, this), &QShortcut::activated, &m_navigation, &Navigation::forward);
    connect(ui->backupsButton, &QPushButton::clicked, this, [this]
            {
                navigate({});
            });
    connect(ui->settingsButton, &QPushButton::clicked, this, [this]
            {
                navigate({PageId::GeneralSettings});
            });
    connect(ui->aboutButton, &QPushButton::clicked, this, [this]
            {
                navigate({PageId::About});
            });
    connect(ui->collapseButton, &QToolButton::clicked, this, [this]
            {
                ui->sidebar->setVisible(!ui->sidebar->isVisible());
            });
    connect(ui->themeButton, &QToolButton::clicked, theme, &ThemeController::toggle);
    connect(ui->pinButton, &QToolButton::toggled, this, [this](bool checked)
            {
                const auto state = windowState();
                setWindowFlag(Qt::WindowStaysOnTopHint, checked);
                setWindowState(state);
                show();
            });
    connect(theme, &ThemeController::changed, this, [this]
            {
                updateIcons();
                m_diff->refreshTheme();
            });
    connect(backups, &BackupService::trackedItemsChanged, this, [this]
            {
                QSet<QString> ids;
                for (const auto &item : m_backups->trackedItems())
                    ids.insert(item.id);
                m_navigation.retainBackups(ids);
            });
    if (trayEnabled)
    {
        m_tray = new QSystemTrayIcon(windowIcon(), this);
        auto *menu = new QMenu(this);
        menu->addAction("主窗口", this, &MainWindow::restoreWindow);
        menu->addAction("退出", qApp, &QApplication::quit);
        m_tray->setContextMenu(menu);
        connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason)
                {
                    if (reason == QSystemTrayIcon::Trigger)
                        restoreWindow();
                    else if (reason == QSystemTrayIcon::DoubleClick)
                        m_tray->showMessage("ZcVersionBox运行中", "右击托盘图标打开菜单", QSystemTrayIcon::Information, 3000);
                });
        m_tray->show();
    }
    updateIcons();
    displayRoute({});
}
MainWindow::~MainWindow() = default;
void MainWindow::updateIcons()
{
    ui->backButton->setIcon(m_theme->icon("back"));
    ui->forwardButton->setIcon(m_theme->icon("forward"));
    ui->collapseButton->setIcon(m_theme->icon("menu"));
    ui->themeButton->setIcon(m_theme->icon("theme"));
    ui->pinButton->setIcon(m_theme->icon("pin"));
    ui->backupsButton->setIcon(m_theme->icon("folder"));
    ui->settingsButton->setIcon(m_theme->icon("settings"));
    ui->aboutButton->setIcon(m_theme->icon("info"));
}
void MainWindow::displayRoute(const Route &route)
{
    if (!route.backupId.isEmpty() && !m_backups->contains(route.backupId))
    {
        m_navigation.removeBackup(route.backupId);
        return;
    }
    m_diff->deactivate();
    QWidget *page = m_list;
    switch (route.page)
    {
    case PageId::Backups:
        m_list->refresh();
        break;
    case PageId::Dashboard:
        page = m_dashboard;
        m_dashboard->setBackup(route.backupId);
        break;
    case PageId::History:
        page = m_history;
        m_history->setBackup(route.backupId);
        break;
    case PageId::Diff:
        page = m_diff;
        m_diff->setRevision(route.backupId, route.commit);
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
    ui->backButton->setEnabled(m_navigation.canBack());
    ui->forwardButton->setEnabled(m_navigation.canForward());
    ui->backupsButton->setChecked(route.page <= PageId::Diff);
    ui->settingsButton->setChecked(route.page == PageId::GeneralSettings || route.page == PageId::AiSettings);
    ui->aboutButton->setChecked(route.page == PageId::About);
    m_breadcrumb->setRoute(route, QFileInfo(m_backups->sourcePath(route.backupId)).fileName());
}
void MainWindow::notify(const OperationResult &result) { m_notifications->showResult(result); }
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
    if (m_tray)
    {
        hide();
        event->ignore();
    }
    else
        QMainWindow::closeEvent(event);
}
