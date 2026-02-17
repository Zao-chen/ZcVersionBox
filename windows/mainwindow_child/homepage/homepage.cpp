#include "homepage.h"
#include "ui_homepage.h"

#include "homepagechild_trackfile.h"
#include "homepagechild_backupfile.h"
#include "../../../GlobalConstants.h"

#include <QStandardPaths>
#include <QDir>
#include <QSpacerItem>
#include <QFileSystemWatcher>
#include <QUrl>
#include <QProcess>

HomePage::HomePage(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::HomePage)
{
    /*窗口初始化*/
    ui->setupUi(this);
    LoadBackupFileList();
    //创建面包屑
    QStringList breadcrumbBarList;
    ui->widget_BreadcrumbBar->setTextPixelSize(25);
    ui->widget_BreadcrumbBar->appendBreadcrumb("追踪中的文件");
    /*监控追踪中的文件*/
    QFileSystemWatcher *watcher = new QFileSystemWatcher(this);
    watcher->addPath(BackupPath);
    connect(watcher, &QFileSystemWatcher::directoryChanged,
            this, [=](const QString &path)
            {
                qInfo()<<"追踪中的文件列表变化："<<path;
                LoadBackupFileList();
            });
}

HomePage::~HomePage()
{
    delete ui;
}

/*加载追踪文件列表*/
void HomePage::LoadBackupFileList()
{
    //清空文件列表
    QLayoutItem *child;
    while ((child = ui->verticalLayout_TrackFiles->takeAt(0)) != nullptr)
    {
        if (QWidget *w = child->widget()) w->deleteLater();
        delete child;
    }
    /*获取所有备份文件夹*/
    //获取路径下所有文件夹并输出名字
    QString docPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)+"/ZcVersionBox/Backup";
    QDir dir(docPath);
    //只列出目录（排除文件），并排除 "." 和 ".."
    QStringList folderNames = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : std::as_const(folderNames))
    {
        HomePageChild_TrackFile *trackfile_widget = new HomePageChild_TrackFile(name, this); //创建子窗口
        ui->verticalLayout_TrackFiles->addWidget(trackfile_widget);
    }
    //最后再添加一个verticalSpacer
    auto *spacer = new QSpacerItem(20, 40, QSizePolicy::Minimum, QSizePolicy::Expanding);
    ui->verticalLayout_TrackFiles->addItem(spacer);
}

/*打开备份*/
void HomePage::openBackup(QString FilePathWithCode)
{
    ui->widget_BreadcrumbBar->appendBreadcrumb(QFileInfo(QUrl::fromPercentEncoding(FilePathWithCode.toUtf8())).baseName());
    ui->stackedWidget->setCurrentIndex(1);
    //清空文件列表
    QLayoutItem *child;
    while ((child = ui->verticalLayout_BackupFiles->takeAt(0)) != nullptr)
    {
        if (QWidget *w = child->widget())
        {
            w->deleteLater();
        }
        delete child;
    }
    /*获取 Git 仓库的所有 commit*/
    QString repoPath = BackupPath + "/" + FilePathWithCode;
    QProcess git;
    git.setWorkingDirectory(repoPath);
    //获取所有 commit 的简短信息
    git.start("git", QStringList() << "log" << "--oneline");
    git.waitForFinished();
    QString output = git.readAllStandardOutput();
    QStringList commitList = output.split("\n", Qt::SkipEmptyParts);
    for (const QString &commitInfo : std::as_const(commitList))
    {
        //commitInfo 格式: "<hash> <message>"
        HomePageChild_BackupFile *backupfile_widget = new HomePageChild_BackupFile(FilePathWithCode, commitInfo, this); //创建子窗口
        ui->verticalLayout_BackupFiles->addWidget(backupfile_widget);
    }
    //最后再添加一个verticalSpacer
    auto *spacer = new QSpacerItem(20, 40, QSizePolicy::Minimum, QSizePolicy::Expanding);
    ui->verticalLayout_BackupFiles->addItem(spacer);
}

/*面包屑点击返回*/
void HomePage::on_widget_BreadcrumbBar_breadcrumbClicked(QString breadcrumb, QStringList lastBreadcrumbList)
{
    ui->stackedWidget->setCurrentIndex(0);
}
