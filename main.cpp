#include "windows/mainwindow.h"
#include "ElaApplication.h"

#include "GlobalConstants.h"

#include <QApplication>
#include <QUrl>
#include <QMessageBox>
#include <QSystemTrayIcon>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    MainWindow w;

    QStringList args = QCoreApplication::arguments();
    args.removeFirst(); // args[0] 是程序自身路径，去掉
    qDebug() << args;
    if(args.isEmpty()) //程序正常启动
    {
        eApp->init();
        eApp->setWindowDisplayMode((ElaApplicationType::WindowDisplayMode)2); // 云母模式
        w.show();
    }
    else //使用右键菜单或拖拽启动
    {
        //再BackupPath下创建文件夹
        QDir dir(BackupPath);
        dir.mkpath(BackupPath+"/"+QUrl::toPercentEncoding(args.first()));
        //弹出消息框提示创建成功
        static QSystemTrayIcon trayIcon;       // 用 static 保证生命周期够长
        trayIcon.setIcon(QIcon(":/img/ico/res/img/logo.png")); // 设置你自己的图标
        trayIcon.show();                        // 必须先 show
        trayIcon.showMessage("ZcVersionBox提示", "已将文件添加至版本控制", QSystemTrayIcon::Information, 3000);
        //直接结束进程
        return 0;
    }

    return a.exec();
}
