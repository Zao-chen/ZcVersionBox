#include "homepage.h"
#include "ui_homepage.h"

HomePage::HomePage(QWidget *parent)
    : QWidget(parent), ui(new Ui::HomePage)
{
    ui->setupUi(this);
    SetupTrackFilesPage();
    SetupBackupPage();
    SetupDiffPage();
}

HomePage::~HomePage()
{
    delete ui;
}

/*面包屑点击返回*/
void HomePage::on_widget_BreadcrumbBar_breadcrumbClicked(QString breadcrumb, QStringList lastBreadcrumbList)
{
    Q_UNUSED(breadcrumb)
    Q_UNUSED(lastBreadcrumbList)
    ui->widget_BreadcrumbBar->setBreadcrumbList(QStringList() << "备份中文件");
    ui->stackedWidget->setCurrentIndex(0);
}
