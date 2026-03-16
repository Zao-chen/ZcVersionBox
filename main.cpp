#include "ElaApplication.h"
#include "windows/mainwindow.h"

#include "GlobalConstants.h"
#include "utils/fileutils.h"

#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QSystemTrayIcon>
#include <QUrl>
#include <qlogging.h>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    eApp->init();
    MainWindow w;
    /*获取启动参数*/
    QStringList args = QCoreApplication::arguments();
    args.removeFirst(); //args[0]是程序自身路径，去掉

    if (args.isEmpty()) //程序正常启动
    {
        w.show();
    }
    else // 使用右键菜单或拖拽启动
    {
        /*同步文件到仓库*/
        QDir dir(BackupPath);
        const QString sourcePath = args.first();
        const QString repoPath = BackupPath + "/" + QUrl::toPercentEncoding(sourcePath);
        const QString backupPath = repoPath + "/" + QFileInfo(sourcePath).fileName();
        dir.mkpath(repoPath);

        if (QFileInfo(sourcePath).isFile())
        {
            QFile::remove(backupPath);
            QFile::copy(sourcePath, backupPath);
        }
        else
        {
            FileUtils::copyDirectory(sourcePath, backupPath);
        }

        /*创建初始化Git仓库*/
        QProcess git;
        git.setWorkingDirectory(repoPath);
        git.start("git", QStringList() << "init");
        git.waitForFinished();
        git.start("git", QStringList() << "add" << ".");
        git.waitForFinished();
        git.start("git", QStringList() << "commit" << "-m" << "Initial backup");
        git.waitForFinished();
        /*相关提示*/
        static QSystemTrayIcon trayIcon;
        trayIcon.setIcon(QIcon(":/img/ico/res/img/logo.png"));
        trayIcon.show();
        trayIcon.showMessage("ZcVersionBox提示", "已将文件添加至版本控制",
                             QSystemTrayIcon::Information, 3000);
        return 0;
    }
    return a.exec();
}
