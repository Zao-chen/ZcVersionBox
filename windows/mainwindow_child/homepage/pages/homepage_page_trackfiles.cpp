#include "../homepage.h"
#define HOMEPAGE_UI_HEADER "ui_homepage.h"
#include HOMEPAGE_UI_HEADER
#undef HOMEPAGE_UI_HEADER

#include "../../../../GlobalConstants.h"
#include "../../../../utils/fileutils.h"
#include "../trackfiles/homepagechild_trackfile.h"

#include "ElaText.h"

#include <ElaContentDialog.h>
#include <ElaDialog.h>
#include <ElaMessageBar.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QLineEdit>
#include <QProcess>
#include <QSpacerItem>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

void HomePage::SetupTrackFilesPage()
{
    /*窗口初始化*/
    LoadBackupFileList();

    /*创建面包屑*/
    ui->widget_BreadcrumbBar->setTextPixelSize(25);
    ui->widget_BreadcrumbBar->appendBreadcrumb("备份中文件");

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

    ElaText *label = new ElaText(tr("你要添加单个文件，还是整个文件夹？"), central);
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

/*从云端导入备份*/
void HomePage::on_pushButton_AddFromRemo_clicked()
{
    ElaContentDialog urlDialog(this);

    QWidget *urlCentral = new QWidget(&urlDialog);
    QVBoxLayout *urlLayout = new QVBoxLayout(urlCentral);

    ElaText *hintText = new ElaText(tr("请输入云端仓库地址"), urlCentral);
    hintText->setTextPixelSize(16);
    urlLayout->addWidget(hintText);

    QLineEdit *urlEdit = new QLineEdit(urlCentral);
    urlEdit->setPlaceholderText(tr("例如: https://github.com/user/repo.git"));
    urlLayout->addWidget(urlEdit);

    urlDialog.setCentralWidget(urlCentral);
    urlDialog.setLeftButtonText(tr("确定"));
    urlDialog.setMiddleButtonText(tr("检查链接"));
    urlDialog.setRightButtonText(tr("取消"));

    int confirm = 0;
    connect(&urlDialog, &ElaContentDialog::leftButtonClicked, &urlDialog, [&]()
            {
                confirm = 1;
                urlDialog.close(); });
    connect(&urlDialog, &ElaContentDialog::middleButtonClicked, &urlDialog, [&]()
            {
                const QString testUrl = urlEdit->text().trimmed();
                if (testUrl.isEmpty())
                {
                    ElaMessageBar::warning(ElaMessageBarType::BottomRight,
                                           "检查失败",
                                           "请先输入云端仓库地址",
                                           2000,
                                           parentWidget());
                    return;
                }

                QProcess checkProcess;
                checkProcess.start("git", QStringList() << "ls-remote" << "--heads" << testUrl);
                checkProcess.waitForFinished();

                if (checkProcess.exitCode() == 0)
                {
                    ElaMessageBar::success(ElaMessageBarType::BottomRight,
                                           "检查成功",
                                           "仓库地址可访问",
                                           2000,
                                           parentWidget());
                }
                else
                {
                    const QString errorText = QString::fromUtf8(checkProcess.readAllStandardError()).trimmed();
                    ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                         "检查失败",
                                         errorText.isEmpty() ? "仓库地址不可用，请检查链接和网络" : errorText,
                                         3000,
                                         parentWidget());
                } });
    connect(&urlDialog, &ElaContentDialog::rightButtonClicked, &urlDialog, [&]()
            {
                confirm = 0;
                urlDialog.close(); });

    urlDialog.exec();

    if (confirm != 1)
        return;

    const QString repoUrl = urlEdit->text().trimmed();

    if (repoUrl.isEmpty())
    {
        ElaMessageBar::warning(ElaMessageBarType::BottomRight,
                               "导入失败",
                               "云端仓库地址不能为空",
                               2000,
                               parentWidget());
        return;
    }

    QDir().mkpath(BackupPath);

    QString repoName = QFileInfo(QUrl(repoUrl).path()).baseName();
    if (repoName.isEmpty())
        repoName = "repo";
    const QString timeSuffix = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    const QString tempRepoPath = QDir(BackupPath).filePath("_import_tmp_" + repoName + "_" + timeSuffix);

    QProcess cloneProcess;
    cloneProcess.start("git", QStringList() << "clone" << repoUrl << tempRepoPath);
    cloneProcess.waitForFinished();

    if (cloneProcess.exitCode() != 0)
    {
        const QString errorText = QString::fromUtf8(cloneProcess.readAllStandardError()).trimmed();
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "导入失败",
                             errorText.isEmpty() ? "仓库克隆失败，请检查仓库地址和网络连接" : errorText,
                             3000,
                             parentWidget());
        return;
    }

    QDir tempRepoDir(tempRepoPath);
    const QFileInfoList rootEntries = tempRepoDir.entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries, QDir::Name);
    QFileInfo trackedEntry;
    for (const QFileInfo &entry : rootEntries)
    {
        if (entry.fileName() == ".git")
            continue;
        trackedEntry = entry;
        break;
    }

    if (!trackedEntry.exists())
    {
        QDir(tempRepoPath).removeRecursively();
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "导入失败",
                             "仓库中未找到可导入内容",
                             3000,
                             parentWidget());
        return;
    }

    QString targetPath;
    QWidget *owner = this->window();
    if (trackedEntry.isFile())
    {
        targetPath = QFileDialog::getSaveFileName(
            owner,
            tr("选择追踪文件位置"),
            QDir::home().filePath(trackedEntry.fileName()),
            tr("All Files (*.*)"));
    }
    else
    {
        const QString targetParent = QFileDialog::getExistingDirectory(
            owner,
            tr("选择追踪文件夹位置"),
            QDir::homePath(),
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (!targetParent.isEmpty())
            targetPath = QDir(targetParent).filePath(trackedEntry.fileName());
    }

    if (targetPath.isEmpty())
    {
        QDir(tempRepoPath).removeRecursively();
        return;
    }

    const QString encodedPath = QString::fromUtf8(QUrl::toPercentEncoding(targetPath));
    const QString finalRepoPath = QDir(BackupPath).filePath(encodedPath);
    if (QDir(finalRepoPath).exists())
    {
        QDir(tempRepoPath).removeRecursively();
        ElaMessageBar::warning(ElaMessageBarType::BottomRight,
                               "导入失败",
                               "该位置已存在追踪记录，请更换位置",
                               3000,
                               parentWidget());
        return;
    }

    const QString targetName = QFileInfo(targetPath).fileName();
    if (trackedEntry.fileName() != targetName)
    {
        tempRepoDir.rename(trackedEntry.fileName(), targetName);
    }

    if (!QDir(BackupPath).rename(QFileInfo(tempRepoPath).fileName(), encodedPath))
    {
        QDir(tempRepoPath).removeRecursively();
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "导入失败",
                             "无法写入备份仓库，请检查权限",
                             3000,
                             parentWidget());
        return;
    }

    if (trackedEntry.isFile())
    {
        QDir().mkpath(QFileInfo(targetPath).absolutePath());
        if (QFile::exists(targetPath))
            QFile::remove(targetPath);
        QFile::copy(QDir(finalRepoPath).filePath(targetName), targetPath);
    }
    else
    {
        QDir targetDir(targetPath);
        if (targetDir.exists())
            targetDir.removeRecursively();
        FileUtils::copyDirectory(QDir(finalRepoPath).filePath(targetName), targetPath);
    }

    ElaMessageBar::success(ElaMessageBarType::BottomRight,
                           "导入成功",
                           "云端备份已加入追踪",
                           2500,
                           parentWidget());
    LoadBackupFileList();
}
