#pragma once
#include "services/backupservice.h"
#include "services/settingsservice.h"
#include "ui/components/presentation.h"
#include "ui/navigation.h"
#include <QMainWindow>
#include <memory>
namespace Ui
{
class MainWindow;
}
class BackupsPage;
class DashboardPage;
class HistoryPage;
class DiffPage;
class GeneralSettingsPage;
class AiSettingsPage;
class AboutPage;
class QSystemTrayIcon;

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

  private:
    std::unique_ptr<Ui::MainWindow> ui;
    BackupService *m_backups;
    SettingsService *m_settings;
    ThemeController *m_theme;
    Navigation m_navigation;
    BreadcrumbBar *m_breadcrumb;
    NotificationBar *m_notifications;
    BackupsPage *m_list;
    DashboardPage *m_dashboard;
    HistoryPage *m_history;
    DiffPage *m_diff;
    GeneralSettingsPage *m_general;
    AiSettingsPage *m_ai;
    AboutPage *m_about;
    QSystemTrayIcon *m_tray{nullptr};
    void displayRoute(const Route &route);
    void restoreWindow();
    void updateIcons();
};
