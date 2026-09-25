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
    AiGateway gateway(&app);
    BackupService backups(paths, &app, &gateway);
    auto args = app.arguments();
    args.removeFirst();
    if (!args.isEmpty())
    {
        QSystemTrayIcon tray(app.windowIcon());
        tray.show();
        backups.addLocal(args.first(), &app, [&app, &tray](const OperationResult &result)
                         {
            tray.showMessage("ZcVersionBox提示", result.success ? result.message : result.title + "：" + result.message,
                         result.success ? QSystemTrayIcon::Information : QSystemTrayIcon::Warning, result.success ? 3000 : 5000);
            app.exit(result.success ? 0 : 1); });
        return app.exec();
    }
    app.setQuitOnLastWindowClosed(false);
    SettingsService settings(paths, &gateway, &app);
#ifdef Q_OS_MACOS
    setMacServicesProviderEnabled(settings.value("RightClickMenu", false).toBool());
#endif
    ThemeController theme(&settings, &app);
    BackupMonitor monitor(&backups, &app);
    MainWindow window(&backups, &settings, &gateway, &theme);
    QObject::connect(&backups, &BackupService::notification, &window, &MainWindow::notify);
    QObject::connect(&monitor, &BackupMonitor::notification, &window, &MainWindow::notify);
    monitor.start();
    window.show();
    return app.exec();
}
