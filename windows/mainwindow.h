#pragma once
#include "utils/backupservice.h"
#include "utils/settingsservice.h"
#include "windows/mainwindow_navigation.h"
#include "windows/mainwindow_presentation.h"
#include <QMainWindow>
#include <QPointer>
#include <memory>
namespace Ui
{
class MainWindow;
}
namespace QWK
{
class WidgetWindowAgent;
}
class HomePage;
class HomePageDashboardPage;
class HomePageBackupPage;
class HomePageDiffPage;
class SettingPage;
class SettingPageAiPage;
class AboutPage;
class BackupListModel;
class BackupFilterModel;
class BackupUiActions;
class QSystemTrayIcon;
class QToolButton;

class MainWindow : public QMainWindow
{
    Q_OBJECT
  public:
    MainWindow(BackupService *backups, SettingsService *settings, AiGateway *gateway, ThemeController *theme, bool trayEnabled = true, QWidget *parent = nullptr);
    ~MainWindow();
    void navigate(const Route &route) { m_navigation.go(route); }
    void notify(const OperationResult &result);

  protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;

  private:
    std::unique_ptr<Ui::MainWindow> ui;
    QWK::WidgetWindowAgent *m_windowAgent{nullptr};
    BackupService *m_backups;
    SettingsService *m_settings;
    ThemeController *m_theme;
    Navigation m_navigation;
    Route m_route;
    Route m_applicationRoute;
    PageId m_settingsPage{PageId::GeneralSettings};
    QPointer<QWidget> m_applicationFocus;
    bool m_applicationSidebarVisible{true};
    BackupListModel *m_backupModel;
    BackupFilterModel *m_sidebarFilter;
    BackupUiActions *m_actions;
    NotificationBar *m_notifications;
    HomePage *m_list;
    HomePageDashboardPage *m_dashboard;
    HomePageBackupPage *m_history;
    HomePageDiffPage *m_diff;
    SettingPage *m_general;
    SettingPageAiPage *m_ai;
    AboutPage *m_about;
    QSystemTrayIcon *m_tray{nullptr};
    QList<QToolButton *> m_toolbarButtons;
    QString m_contextTitle;
    int m_sidebarWidth{224};
    void displayRoute(const Route &route);
    void syncBackups();
    void syncSidebarSelection();
    void restoreWindow();
    void toggleSidebar();
    void openSettings();
    void returnToApplication();
    void updateSettingsSearch();
    void updateIcons();
    void setToolbar(const QList<QAction *> &actions);
    void updateHeaderLayout();
};
