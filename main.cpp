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
    //获取启动参数
    QStringList args = QCoreApplication::arguments();
    args.removeFirst(); //args[0] 是程序自身路径，去掉
    if(args.isEmpty()) //程序正常启动
    {
        eApp->init();
        eApp->setWindowDisplayMode((ElaApplicationType::WindowDisplayMode)2); // 云母模式
        w.show();
    }
    else //使用右键菜单或拖拽启动
    {
        //创建备份文件夹
        QDir dir(BackupPath);
        dir.mkpath(BackupPath+"/"+QUrl::toPercentEncoding(args.first()));
        //弹出消息框提示创建成功
        static QSystemTrayIcon trayIcon;
        trayIcon.setIcon(QIcon(":/img/ico/res/img/logo.png"));
        trayIcon.show();
        trayIcon.showMessage("ZcVersionBox提示", "已将文件添加至版本控制", QSystemTrayIcon::Information, 3000);
        //直接结束进程
        return 0;
    }
    return a.exec();
}
