#include "../homepage.h"
#define HOMEPAGE_UI_HEADER "ui_homepage.h"
#include HOMEPAGE_UI_HEADER
#undef HOMEPAGE_UI_HEADER

#include "../../../../GlobalConstants.h"
#include "../trackfiles/homepagechild_trackfile.h"

#include "ElaText.h"

#include <ElaContentDialog.h>
#include <ElaDialog.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileSystemWatcher>
#include <QProcess>
#include <QSpacerItem>
#include <QStandardPaths>
#include <QVBoxLayout>

void HomePage::SetupTrackFilesPage()
{
    /*窗口初始化*/
    LoadBackupFileList();

    /*创建面包屑*/
    ui->widget_BreadcrumbBar->setTextPixelSize(25);
    ui->widget_BreadcrumbBar->appendBreadcrumb("追踪中的文件");

    /*监控追踪中的文件*/
    QFileSystemWatcher *watcher = new QFileSystemWatcher(this);
    watcher->addPath(BackupPath);
    connect(watcher, &QFileSystemWatcher::directoryChanged,
            this, [=](const QString &path)
            {
                qInfo() << "追踪中的文件列表变化：" << path;
                LoadBackupFileList(); });
}

/*加载追踪文件列表*/
void HomePage::LoadBackupFileList()
{
    //清空文件列表
    QLayoutItem *child;
    while ((child = ui->verticalLayout_TrackFiles->takeAt(0)) != nullptr)
    {
        if (QWidget *w = child->widget())
            w->deleteLater();
        delete child;
    }

    /*获取所有备份文件夹*/
    //获取路径下所有文件夹并输出名字
    QString docPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/ZcVersionBox/Backup";
    QDir dir(docPath);
    //只列出目录（排除文件），并排除 "." 和 ".."
    QStringList folderNames = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : std::as_const(folderNames))
    {
        //创建子窗口
        HomePageChild_TrackFile *trackfile_widget = new HomePageChild_TrackFile(name, this);
        ui->verticalLayout_TrackFiles->addWidget(trackfile_widget);
    }

    //最后再添加一个verticalSpacer
    auto *spacer = new QSpacerItem(20, 40, QSizePolicy::Minimum, QSizePolicy::Expanding);
    ui->verticalLayout_TrackFiles->addItem(spacer);
}

/*添加本地仓库*/
void HomePage::on_pushButton_AddFromLoc_clicked()
{
    ElaContentDialog dlg(this);

    QWidget *central = new QWidget(&dlg);
    QVBoxLayout *layout = new QVBoxLayout(central);

    ElaText *label = new ElaText(tr("请选择要添加的是文件还是文件夹"), central);
    label->setTextPixelSize(16);
    layout->addWidget(label);

    dlg.setCentralWidget(central);
    dlg.setLeftButtonText(tr("文件"));
    dlg.setMiddleButtonText(tr("文件夹"));
    dlg.setRightButtonText(tr("取消"));

    int choose = 0;

    connect(&dlg, &ElaContentDialog::leftButtonClicked, &dlg, [&]()
            {
                choose = 1;
                dlg.close(); });

    connect(&dlg, &ElaContentDialog::middleButtonClicked, &dlg, [&]()
            {
                choose = 2;
                dlg.close(); });

    connect(&dlg, &ElaContentDialog::rightButtonClicked, &dlg, [&]()
            {
                choose = 0;
                dlg.close(); });

    dlg.exec();

    QString path;
    QWidget *owner = this->window();

    if (choose == 1)
    {
        QFileDialog::Options options;
        path = QFileDialog::getOpenFileName(
            owner,
            tr("选择文件"),
            QDir::homePath(),
            tr("All Files (*.*)"),
            nullptr,
            options);
    }
    else if (choose == 2)
    {
        QFileDialog::Options options = QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks;
        path = QFileDialog::getExistingDirectory(
            owner,
            tr("选择文件夹"),
            QDir::homePath(),
            options);
    }
    else
    {
        return;
    }

    if (path.isEmpty())
        return;

    QString exePath = QCoreApplication::applicationFilePath();
    QProcess::startDetached(exePath, QStringList() << path);
}
