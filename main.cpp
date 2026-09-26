#include "utils/backupmonitor.h"
#include "windows/mainwindow.h"
#include <QApplication>
#include <QSystemTrayIcon>
#include <QMessageBox>
#include <QTimer>
#ifdef Q_OS_MACOS
#include "macos/macos_services.h"
#endif

int main(int argc, char *argv[])
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv);
    app.setApplicationName("ZcVersionBox");
    app.setApplicationVersion(ZCVERSIONBOX_VERSION);
    app.setWindowIcon(QIcon(":/img/ico/res/img/logo.png"));
#ifdef Q_OS_LINUX
    app.setDesktopFileName("com.zc.versionbox");
#endif
    const auto paths = AppPaths::defaults();
    AiGateway gateway(&app);
    BackupService backups(paths, &app, &gateway);
    auto args = app.arguments();
    args.removeFirst();
    if (!args.isEmpty())
    {
        QStringList selected = args;
        OperationResult parsed = OperationResult::ok({});
        if (args.first() == "--nautilus-add")
            parsed = args.size() == 1 ? LinuxIntegration::nautilusSelection(qgetenv("NAUTILUS_SCRIPT_SELECTED_URIS"), selected)
                                      : OperationResult::fail("添加失败", "文件管理器入口参数无效");
        else if (selected.first() == "--")
            selected.removeFirst();
        if (!parsed.success || selected.isEmpty())
        {
            QMessageBox::warning(nullptr, "添加失败", parsed.success ? "请指定文件或文件夹" : parsed.message);
            return 1;
        }
        app.setQuitOnLastWindowClosed(false);
        QSystemTrayIcon tray(app.windowIcon());
        if (QSystemTrayIcon::isSystemTrayAvailable())
            tray.show();
        int index = 0, succeeded = 0;
        QStringList failures;
        std::function<void()> next;
        next = [&]
        {
            if (index < selected.size())
            {
                const auto path = selected[index++];
                backups.addLocal(path, &app, [&, path](const OperationResult &result)
                {
                    if (result.success)
                        ++succeeded;
                    else
                        failures.append(path + "：" + result.title + "，" + result.message);
                    QTimer::singleShot(0, &app, next);
                });
                return;
            }
            const auto message = QString("已添加 %1 项备份").arg(succeeded) +
                                 (failures.isEmpty() ? QString() : "\n" + failures.join('\n'));
            const int exitCode = failures.isEmpty() ? 0 : 1;
            if (QSystemTrayIcon::isSystemTrayAvailable() && QSystemTrayIcon::supportsMessages())
            {
                tray.showMessage("ZcVersionBox", message,
                                 exitCode ? QSystemTrayIcon::Warning : QSystemTrayIcon::Information, 4000);
                QTimer::singleShot(4000, &app, [&app, exitCode] { app.exit(exitCode); });
            }
            else
            {
                QMessageBox result(exitCode ? QMessageBox::Warning : QMessageBox::Information,
                                   "ZcVersionBox", message, QMessageBox::Ok);
                result.exec();
                app.exit(exitCode);
            }
        };
        QTimer::singleShot(0, &app, next);
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
#ifdef Q_OS_MACOS
    setupMacTitleBar(window.winId());
#endif
    return app.exec();
}
