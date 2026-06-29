#include "homepage.h"
#include "ui_homepage.h"

#include <QFileInfo>
#include <QUrl>

HomePage::HomePage(QWidget *parent)
    : QWidget(parent), ui(new Ui::HomePage)
{
    ui->setupUi(this);
    SetupTrackFilesPage();
    SetupDashboardPage();
    SetupDiffPage();
}

HomePage::~HomePage()
{
    ReleaseDashboardPageUi();
    delete ui;
}

/*面包屑点击返回*/
void HomePage::on_widget_BreadcrumbBar_breadcrumbClicked(QString breadcrumb, QStringList lastBreadcrumbList)
{
    Q_UNUSED(lastBreadcrumbList)

    const QString rootBreadcrumb = "备份中文件";
    const QString displayName = QFileInfo(QUrl::fromPercentEncoding(m_NowFilePathWithCode.toUtf8())).baseName();
    const QString dashboardBreadcrumb = displayName + " 仪表盘";
    const QString historyBreadcrumb = displayName + " 历史版本";

    if (breadcrumb == rootBreadcrumb || m_NowFilePathWithCode.isEmpty())
    {
        ui->widget_BreadcrumbBar->setBreadcrumbList(QStringList() << rootBreadcrumb);
        ui->stackedWidget->setCurrentIndex(0);
        return;
    }

    if (breadcrumb == dashboardBreadcrumb)
    {
        openBackupDashboard(m_NowFilePathWithCode);
        return;
    }

    // Diff 页点击历史版本面包屑时回到当前文件的版本列表。
    if (breadcrumb == historyBreadcrumb)
    {
        ui->widget_BreadcrumbBar->setBreadcrumbList(QStringList() << rootBreadcrumb << historyBreadcrumb);
        ui->stackedWidget->setCurrentIndex(1);
        return;
    }

    // 兼容旧面包屑：文件名曾经代表当前备份对象首页。
    if (breadcrumb == displayName)
    {
        openBackupDashboard(m_NowFilePathWithCode);
        return;
    }

    ui->widget_BreadcrumbBar->setBreadcrumbList(QStringList() << rootBreadcrumb << historyBreadcrumb << "版本对比");
    ui->stackedWidget->setCurrentIndex(2);
}
