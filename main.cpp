#include "utils/backupmonitor.h"
#include "windows/mainwindow.h"
#include <QApplication>
#include <QSystemTrayIcon>
#ifdef Q_OS_MACOS
#include "macos/macos_services.h"
#endif

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("ZcVersionBox");
    app.setApplicationVersion(ZCVERSIONBOX_VERSION);
    app.setWindowIcon(QIcon(":/img/ico/res/img/logo.png"));
    const auto paths = AppPaths::defaults();
    BackupService backups(paths, &app);
    auto args = app.arguments();
    args.removeFirst();
    if (!args.isEmpty())
    {
        const auto result = backups.addLocal(args.first());
        QSystemTrayIcon tray(app.windowIcon());
        tray.show();
        tray.showMessage("ZcVersionBox提示", result.success ? result.message : result.title + "：" + result.message,
                         result.success ? QSystemTrayIcon::Information : QSystemTrayIcon::Warning, result.success ? 3000 : 5000);
        return 0;
    }
    app.setQuitOnLastWindowClosed(false);
    ThemeController theme(&app);
    AiGateway gateway(&app);
    SettingsService settings(paths, &gateway, &app);
#ifdef Q_OS_MACOS
    setMacServicesProviderEnabled(settings.value("RightClickMenu", false).toBool());
#endif
    BackupMonitor monitor(&backups, &app);
    MainWindow window(&backups, &settings, &gateway, &theme);
    QObject::connect(&monitor, &BackupMonitor::notification, &window, &MainWindow::notify);
    monitor.start();
    window.show();
    return app.exec();
}
