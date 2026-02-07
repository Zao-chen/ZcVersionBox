#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include "mainwindow_child/homepage.h"
#include "mainwindow_child/settingpage.h"

#include <QCloseEvent>

MainWindow::MainWindow(QWidget *parent)
    : ElaWindow(parent)
    , ui(new Ui::MainWindow)
{
    // ui->setupUi(this);

    /*初始化设定*/
    setUserInfoCardVisible(false);
    setWindowTitle("ZcVersionBox");
    setUserInfoCardTitle("VersionBox");

    /*托盘*/
    /*创建托盘*/
    m_sysTrayIcon = new QSystemTrayIcon(this); //新建QSystemTrayIcon对象
    //创建菜单
    m_menu = new ElaMenu(this);
    m_showWin = new QAction("主窗口", this);
    m_menu->addAction(m_showWin);
    m_exitAppAction = new QAction("退出", this);
    m_menu->addAction(m_exitAppAction);
    m_sysTrayIcon->setContextMenu(m_menu);
    connect(m_showWin,SIGNAL(triggered()),this,SLOT(on_showMainAction()));
    connect(m_exitAppAction,SIGNAL(triggered()),this,SLOT(on_exitAppAction()));
    //设置图标
    QIcon icon = QIcon(":/img/ico/res/img/logo.png"); //资源文件添加的图标
    m_sysTrayIcon->setIcon(icon);
    m_sysTrayIcon->show();
    //托盘事件
    connect(m_sysTrayIcon, &QSystemTrayIcon::activated, //给QSystemTrayIcon添加槽函数
            [=](QSystemTrayIcon::ActivationReason reason)
            {
                switch(reason)
                {
                case QSystemTrayIcon::Trigger: //单击托盘图标
                    //显示主窗口
                    on_showMainAction();
                    break;
                case QSystemTrayIcon::DoubleClick: //双击托盘图标
                    //双击提示
                    m_sysTrayIcon->showMessage("ZcVersionBox运行中", "右击托盘图标打开菜单", QSystemTrayIcon::Information, 3000);
                    break;
                default:
                    break;
                }
            });


    /*页面创建*/
    //正常页面
    HomePage *homepage_ui = new HomePage(this);
    addPageNode(tr("主页"), homepage_ui, ElaIconType::House);
    //底部页面
    SettingPage *settingpage_ui = new SettingPage(this);
    addFooterNode(tr("设置"), settingpage_ui, *new QString(""), 0, ElaIconType::Gear);
}

MainWindow::~MainWindow()
{
    delete ui;
}


/*托盘动作*/
//托盘主界面
void MainWindow::on_showMainAction()
{
    this->show();
    this->raise();
}
//托盘推出
void MainWindow::on_exitAppAction()
{
    qApp->exit();
}
/*重写事件*/
void MainWindow::closeEvent(QCloseEvent *e)
{
    this->hide();
    e->ignore(); // 阻止真正 close
}
