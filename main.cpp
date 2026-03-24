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
    /*启动*/
    if (args.isEmpty()) //程序正常启动
    {
        w.show();
    }
    else //使用右键菜单或拖拽启动
    {
        /*同步文件到仓库*/
        QDir dir(BackupPath);
        const QString sourcePath = args.first();
        const QString repoPath = BackupPath + "/" + QUrl::toPercentEncoding(sourcePath);
        const QString backupPath = repoPath + "/" + QFileInfo(sourcePath).fileName();
        dir.mkpath(repoPath);
        /*添加文件到备份目录*/
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

        //初始化仓库
        git.start("git", QStringList() << "init");
        if (!git.waitForStarted())
        {
            static QSystemTrayIcon trayIcon;
            trayIcon.setIcon(QIcon(":/img/ico/res/img/logo.png"));
            trayIcon.show();
            trayIcon.showMessage("ZcVersionBox提示", "初始化失败：无法启动 Git（请确认已安装 Git）",
                                 QSystemTrayIcon::Warning, 5000);
            return 0;
        }
        git.waitForFinished();
        if (git.exitCode() != 0)
        {
            static QSystemTrayIcon trayIcon;
            trayIcon.setIcon(QIcon(":/img/ico/res/img/logo.png"));
            trayIcon.show();
            trayIcon.showMessage("ZcVersionBox提示", "初始化失败：git init 命令执行失败",
                                 QSystemTrayIcon::Warning, 5000);
            return 0;
        }

        //配置本地 git user（如果全局未配置）
        git.start("git", QStringList() << "config" << "user.name" << "ZcVersionBox");
        git.waitForFinished();
        git.start("git", QStringList() << "config" << "user.email" << "backup@zcversionbox.local");
        git.waitForFinished();

        //添加文件
        git.start("git", QStringList() << "add" << ".");
        if (!git.waitForStarted())
        {
            static QSystemTrayIcon trayIcon;
            trayIcon.setIcon(QIcon(":/img/ico/res/img/logo.png"));
            trayIcon.show();
            trayIcon.showMessage("ZcVersionBox提示", "添加失败：无法启动 Git",
                                 QSystemTrayIcon::Warning, 5000);
            return 0;
        }
        git.waitForFinished();
        if (git.exitCode() != 0)
        {
            static QSystemTrayIcon trayIcon;
            trayIcon.setIcon(QIcon(":/img/ico/res/img/logo.png"));
            trayIcon.show();
            QString errorMsg = QString::fromUtf8(git.readAllStandardError()).trimmed();
            trayIcon.showMessage("ZcVersionBox提示",
                                 errorMsg.isEmpty() ? "添加失败：git add 命令执行失败" : errorMsg,
                                 QSystemTrayIcon::Warning, 5000);
            return 0;
        }

        //创建初始提交
        git.start("git", QStringList() << "commit" << "-m" << "Initial backup");
        if (!git.waitForStarted())
        {
            static QSystemTrayIcon trayIcon;
            trayIcon.setIcon(QIcon(":/img/ico/res/img/logo.png"));
            trayIcon.show();
            trayIcon.showMessage("ZcVersionBox提示", "提交失败：无法启动 Git",
                                 QSystemTrayIcon::Warning, 5000);
            return 0;
        }
        git.waitForFinished();
        if (git.exitCode() != 0)
        {
            static QSystemTrayIcon trayIcon;
            trayIcon.setIcon(QIcon(":/img/ico/res/img/logo.png"));
            trayIcon.show();
            QString errorMsg = QString::fromUtf8(git.readAllStandardError()).trimmed();
            trayIcon.showMessage("ZcVersionBox提示",
                                 errorMsg.isEmpty() ? "提交失败：git commit 命令执行失败" : errorMsg,
                                 QSystemTrayIcon::Warning, 5000);
            return 0;
        }

        //完成提示
        static QSystemTrayIcon trayIcon;
        trayIcon.setIcon(QIcon(":/img/ico/res/img/logo.png"));
        trayIcon.show();
        trayIcon.showMessage("ZcVersionBox提示", "已将文件添加至版本控制",
                             QSystemTrayIcon::Information, 3000);
        return 0;
    }
    return a.exec();
}
