#include "../homepage.h"
#define HOMEPAGE_UI_HEADER "ui_homepage.h"
#include HOMEPAGE_UI_HEADER
#undef HOMEPAGE_UI_HEADER

#include "../../../../GlobalConstants.h"
#include "../../../../utils/fileutils.h"
#include "../trackfiles/homepagechild_trackfile.h"

#include "ElaText.h"

#include <ElaContentDialog.h>
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
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>

namespace
{
class SafeElaContentDialog : public ElaContentDialog
{
public:
    using ElaContentDialog::ElaContentDialog;

    void onLeftButtonClicked() override
    {
        if (leftClicked)
            leftClicked();
    }

    void onMiddleButtonClicked() override
    {
        if (middleClicked)
            middleClicked();
    }

    void onRightButtonClicked() override
    {
        if (rightClicked)
            rightClicked();
    }

    std::function<void()> leftClicked;
    std::function<void()> middleClicked;
    std::function<void()> rightClicked;
};
}

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
    SafeElaContentDialog *dlg = new SafeElaContentDialog(this);

    QWidget *central = new QWidget(dlg);
    QVBoxLayout *layout = new QVBoxLayout(central);

    ElaText *label = new ElaText(tr("你要添加单个文件，还是整个文件夹？"), central);
    label->setTextPixelSize(16);
    layout->addWidget(label);

    dlg->setCentralWidget(central);
    dlg->setLeftButtonText(tr("文件"));
    dlg->setMiddleButtonText(tr("文件夹"));
    dlg->setRightButtonText(tr("取消"));

    int choose = 0;
    dlg->leftClicked = [&]() { choose = 1; };
    dlg->middleClicked = [&]()
    {
        choose = 2;
        dlg->reject();
    };
    dlg->rightClicked = [&]() { choose = 0; };

    dlg->exec();
    dlg->leftClicked = nullptr;
    dlg->middleClicked = nullptr;
    dlg->rightClicked = nullptr;
    QTimer::singleShot(1000, dlg, &QObject::deleteLater);

    QString path;
    QWidget *owner = this->window();

    if (choose == 1) //添加文件
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
    else if (choose == 2) //添加文件夹
    {
        QFileDialog::Options options = QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks;
        path = QFileDialog::getExistingDirectory(
            owner,
            tr("选择文件夹"),
            QDir::homePath(),
            options);
    }
    else //取消
    {
        return;
    }

    if (path.isEmpty())
        return;

    QString exePath = QCoreApplication::applicationFilePath();
    if (!QProcess::startDetached(exePath, QStringList() << path))
    {
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "添加失败",
                             "无法启动新进程，请检查程序权限",
                             3000,
                             parentWidget());
    }
}

/*从云端导入备份*/
void HomePage::on_pushButton_AddFromRemo_clicked()
{
    SafeElaContentDialog *urlDialog = new SafeElaContentDialog(this);

    QWidget *urlCentral = new QWidget(urlDialog);
    QVBoxLayout *urlLayout = new QVBoxLayout(urlCentral);

    ElaText *hintText = new ElaText(tr("请输入云端仓库地址"), urlCentral);
    hintText->setTextPixelSize(16);
    urlLayout->addWidget(hintText);

    QLineEdit *urlEdit = new QLineEdit(urlCentral);
    urlEdit->setPlaceholderText(tr("例如: https://github.com/user/repo.git"));
    urlLayout->addWidget(urlEdit);

    urlDialog->setCentralWidget(urlCentral);
    urlDialog->setLeftButtonText(tr("确定"));
    urlDialog->setMiddleButtonText(tr("检查链接"));
    urlDialog->setRightButtonText(tr("取消"));

    int confirm = 0;
    urlDialog->leftClicked = [&]() { confirm = 1; };
    urlDialog->middleClicked = [&]() //检查链接
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
        if (!checkProcess.waitForStarted())
        {
            ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                 "检查失败",
                                 "无法启动 git，请确认 git 已安装",
                                 3000,
                                 parentWidget());
            return;
        }
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
        }
    };
    urlDialog->rightClicked = [&]() { confirm = 0; };

    urlDialog->exec();
    const QString repoUrl = urlEdit->text().trimmed();
    urlDialog->leftClicked = nullptr;
    urlDialog->middleClicked = nullptr;
    urlDialog->rightClicked = nullptr;
    QTimer::singleShot(1000, urlDialog, &QObject::deleteLater);

    if (confirm != 1)
    {
        return;
    }

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

    /*Clone仓库到暂存*/
    QString repoName = QFileInfo(QUrl(repoUrl).path()).baseName();
    if (repoName.isEmpty())
        repoName = "repo";
    const QString timeSuffix = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    const QString tempRepoPath = QDir(BackupPath).filePath("_import_tmp_" + repoName + "_" + timeSuffix);

    QProcess cloneProcess;
    cloneProcess.start("git", QStringList() << "clone" << repoUrl << tempRepoPath);
    if (!cloneProcess.waitForStarted())
    {
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "导入失败",
                             "无法启动 git，请确认 git 已安装",
                             3000,
                             parentWidget());
        return;
    }
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
        if (!QDir(tempRepoPath).removeRecursively())
        {
            ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                 "导入失败",
                                 "清理临时仓库失败",
                                 3000,
                                 parentWidget());
            return;
        }
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "导入失败",
                             "仓库中未找到可导入内容",
                             3000,
                             parentWidget());
        return;
    }

    /*选择保存位置*/
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
        if (!tempRepoDir.rename(trackedEntry.fileName(), targetName))
        {
            QDir(tempRepoPath).removeRecursively();
            ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                 "导入失败",
                                 "重命名导入内容失败",
                                 3000,
                                 parentWidget());
            return;
        }
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
        if (!QDir().mkpath(QFileInfo(targetPath).absolutePath()))
        {
            ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                 "导入失败",
                                 "无法创建目标目录",
                                 3000,
                                 parentWidget());
            return;
        }
        if (QFile::exists(targetPath))
        {
            if (!QFile::remove(targetPath))
            {
                ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                     "导入失败",
                                     "无法覆盖现有文件",
                                     3000,
                                     parentWidget());
                return;
            }
        }
        if (!QFile::copy(QDir(finalRepoPath).filePath(targetName), targetPath))
        {
            ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                 "导入失败",
                                 "复制导入文件失败",
                                 3000,
                                 parentWidget());
            return;
        }
    }
    else
    {
        QDir targetDir(targetPath);
        if (targetDir.exists())
        {
            if (!targetDir.removeRecursively())
            {
                ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                     "导入失败",
                                     "清理目标文件夹失败",
                                     3000,
                                     parentWidget());
                return;
            }
        }
        if (!FileUtils::copyDirectory(QDir(finalRepoPath).filePath(targetName), targetPath))
        {
            ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                 "导入失败",
                                 "复制导入文件夹失败",
                                 3000,
                                 parentWidget());
            return;
        }
    }

    ElaMessageBar::success(ElaMessageBarType::BottomRight,
                           "导入成功",
                           "云端备份已加入追踪",
                           2500,
                           parentWidget());
    LoadBackupFileList();
}
