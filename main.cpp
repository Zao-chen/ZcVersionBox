#include "ElaApplication.h"
#include "ElaTheme.h"
#include "GlobalConstants.h"
#include "application/aiannotationcontroller.h"
#include "application/instancecontroller.h"
#include "backup/backupservice.h"
#include "windows/mainwindow.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>
#include <QMessageBox>
#include <QPalette>
#include <QTimer>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("ZcVersionBox");
    app.setOrganizationName("ZcVersionBox");
    app.setQuitOnLastWindowClosed(false);
    app.setWindowIcon(QIcon(":/img/ico/res/img/logo.png"));
    QCommandLineParser parser;
    parser.setApplicationDescription("ZcVersionBox 文件版本备份");
    parser.addHelpOption();
    parser.addOption({"data-dir", "使用独立备份数据目录", "directory"});
    parser.addPositionalArgument("paths", "添加到备份的文件或文件夹", "[paths...] ");
    parser.process(app);
    QStringList paths;
    for (const auto &path : parser.positionalArguments())
        paths.append(Backup::normalizedPath(path));
    const auto root = parser.isSet("data-dir") ? Backup::normalizedPath(parser.value("data-dir")) : Backup::storageRoot();
    app.setProperty("backupDataRoot", root);
    try
    {
        InstanceController instance(root);
        if (!instance.acquire())
        {
            QString error;
            if (!instance.forward(paths, &error))
            {
                QMessageBox::critical(nullptr, "ZcVersionBox", error);
                return 1;
            }
            return 0;
        }
        eApp->init();
        auto applyPalette = [&app](ElaThemeType::ThemeMode mode)
        {
            auto palette = app.palette();
            palette.setColor(QPalette::Window, ElaThemeColor(mode, WindowBase));
            palette.setColor(QPalette::WindowText, ElaThemeColor(mode, BasicText));
            palette.setColor(QPalette::Base, ElaThemeColor(mode, BasicBase));
            palette.setColor(QPalette::AlternateBase, ElaThemeColor(mode, BasicAlternating));
            palette.setColor(QPalette::Text, ElaThemeColor(mode, BasicText));
            palette.setColor(QPalette::Button, ElaThemeColor(mode, BasicBase));
            palette.setColor(QPalette::ButtonText, ElaThemeColor(mode, BasicText));
            palette.setColor(QPalette::Highlight, ElaThemeColor(mode, PrimaryNormal));
            palette.setColor(QPalette::HighlightedText, ElaThemeColor(mode, BasicTextInvert));
            palette.setColor(QPalette::Disabled, QPalette::Text, ElaThemeColor(mode, BasicTextDisable));
            app.setPalette(palette);
        };
        applyPalette(eTheme->getThemeMode());
        QObject::connect(eTheme, &ElaTheme::themeModeChanged, &app, applyPalette);
        Backup::BackupService service(root);
        QObject::connect(&service, &Backup::BackupService::stopping, &instance, &InstanceController::stopAccepting);
        AiAnnotationController annotations(&service);
        MainWindow window(&service);
        QObject::connect(&service, &Backup::BackupService::stopped, &app, &QCoreApplication::quit, Qt::QueuedConnection);
        QObject::connect(&app, &QCoreApplication::aboutToQuit, &service, &Backup::BackupService::shutdown);
        auto addPaths = [&service, &window](const QStringList &sources)
        {
            for (const auto &source : sources)
            {
                try
                {
                    service.track(source);
                }
                catch (const Backup::Error &error)
                {
                    QMessageBox::warning(&window, "添加备份失败", error.message);
                }
            }
        };
        QObject::connect(&instance, &InstanceController::pathsReceived, &window, addPaths);
        QObject::connect(&instance, &InstanceController::activateRequested, &window, [&window]
                         {
                             window.show();
                             window.raise();
                             window.activateWindow();
                         });
        QTimer::singleShot(0, &service, [&service, addPaths, paths]
                           {
                               service.start();
                               addPaths(paths);
                           });
        window.show();
        return app.exec();
    }
    catch (const Backup::Error &error)
    {
        QMessageBox::critical(nullptr, "ZcVersionBox", error.message);
        return 1;
    }
}
