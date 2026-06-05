#include "homepage.h"
#include "ui_homepage.h"

#include <QFileInfo>
#include <QUrl>

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
    Q_UNUSED(lastBreadcrumbList)

    const QString rootBreadcrumb = "备份中文件";
    const QString displayName = QFileInfo(QUrl::fromPercentEncoding(m_NowFilePathWithCode.toUtf8())).baseName();

    if (breadcrumb == rootBreadcrumb || m_NowFilePathWithCode.isEmpty())
    {
        ui->widget_BreadcrumbBar->setBreadcrumbList(QStringList() << rootBreadcrumb);
        ui->stackedWidget->setCurrentIndex(0);
        return;
    }

    //Diff页点击二级面包屑时回到当前文件的版本列表，而不是回到根页。
    if (breadcrumb == displayName)
    {
        ui->widget_BreadcrumbBar->setBreadcrumbList(QStringList() << rootBreadcrumb << displayName);
        ui->stackedWidget->setCurrentIndex(1);
        return;
    }

    ui->widget_BreadcrumbBar->setBreadcrumbList(QStringList() << rootBreadcrumb << displayName << "版本对比");
    ui->stackedWidget->setCurrentIndex(2);
}
