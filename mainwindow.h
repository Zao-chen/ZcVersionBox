#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QSystemTrayIcon>

#include "ElaWindow.h"
#include "ElaMenu.h"


QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public ElaWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

protected:
    void closeEvent(QCloseEvent *event) override; //重写主窗口关闭函数

private slots:
    void on_showMainAction(); //打开设置
    void on_exitAppAction(); //退出程序

private:
    Ui::MainWindow *ui;
    QSystemTrayIcon *m_sysTrayIcon; //系统托盘
    ElaMenu *m_menu; //菜单
    QAction *m_showWin; //主窗口
    QAction *m_exitAppAction; //退出程序
};
#endif // MAINWINDOW_H
