#include "homepage.h"
#include "ui_homepage.h"

#include "../../../GlobalConstants.h"
#include "../../../utils/fileutils.h"
#include "homepagechild_trackfile.h"

#include "ElaIconButton.h"
#include "ElaLineEdit.h"
#include "ElaMessageBar.h"
#include "ElaPushButton.h"
#include "ElaText.h"
#include "ElaToggleSwitch.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileSystemWatcher>
#include <QProcess>
#include <QSignalBlocker>
#include <QSpacerItem>
#include <QStandardItem>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

HomePage::HomePage(QWidget *parent)
    : QWidget(parent), ui(new Ui::HomePage)
{
    /*窗口初始化*/
    ui->setupUi(this);
    LoadBackupFileList();

    /*远程开关*/
    m_ToggleSwitch_Remote = new ElaToggleSwitch(this);
    QWidget *drawerHeader = new QWidget(this);
    QHBoxLayout *drawerHeaderLayout = new QHBoxLayout(drawerHeader);
    drawerHeaderLayout->setContentsMargins(8, 0, 8, 0);
    //展开按钮
    ElaText *drawerIcon = new ElaText(this);
    drawerIcon->setTextPixelSize(15);
    drawerIcon->setElaIcon(ElaIconType::MessageArrowDown);
    drawerIcon->setFixedSize(25, 25);
    //文本描述
    ElaText *drawerText = new ElaText("远程仓库", this);
    drawerText->setTextPixelSize(15);
    //开关
    connect(m_ToggleSwitch_Remote, &ElaToggleSwitch::toggled, this,
            [=](bool checked)
            {
                if (m_RemoteUiSyncing)
                    return;
                m_RemoteUiSyncing = true;
                if (checked)
                    ui->ElaDrawerArea_Remote->expand();
                else
                    ui->ElaDrawerArea_Remote->collapse();
                m_RemoteUiSyncing = false;
            });
    connect(ui->ElaDrawerArea_Remote, &ElaDrawerArea::expandStateChanged, this,
            [=](bool isExpand)
            {
                if (m_RemoteUiSyncing)
                    return;
                m_RemoteUiSyncing = true;
                m_ToggleSwitch_Remote->setIsToggled(isExpand);
                m_RemoteUiSyncing = false;
            });
    connect(m_ToggleSwitch_Remote, &ElaToggleSwitch::toggled, this, &HomePage::on_ToggleSwitch_Remote_toggled);
    //布局
    drawerHeaderLayout->addWidget(drawerIcon);
    drawerHeaderLayout->addWidget(drawerText);
    drawerHeaderLayout->addStretch();
    drawerHeaderLayout->addWidget(m_ToggleSwitch_Remote);
    ui->ElaDrawerArea_Remote->setDrawerHeader(drawerHeader);
    //展开内容
    QWidget *remoteDrawerContent = new QWidget(this);
    QVBoxLayout *remoteDrawerLayout = new QVBoxLayout(remoteDrawerContent);
    ElaText *remoteHintText = new ElaText("远程仓库地址", remoteDrawerContent);
    remoteHintText->setTextPixelSize(13);
    m_RemoteUrlEdit = new ElaLineEdit(remoteDrawerContent);
    m_RemoteUrlEdit->setPlaceholderText(u8"请输入远程仓库地址");
    QHBoxLayout *remoteRowLayout = new QHBoxLayout();
    remoteRowLayout->addWidget(remoteHintText);
    remoteRowLayout->addWidget(m_RemoteUrlEdit, 1);
    //创建同步和上传图标按钮
    ElaIconButton *btnPull = new ElaIconButton(ElaIconType::Download, 16, remoteDrawerContent);
    btnPull->setFixedSize(32, 32);
    btnPull->setToolTip("从远程仓库同步");
    ElaIconButton *btnPush = new ElaIconButton(ElaIconType::Upload, 16, remoteDrawerContent);
    btnPush->setFixedSize(32, 32);
    btnPush->setToolTip("上传到远程仓库");
    remoteRowLayout->addWidget(btnPull);
    remoteRowLayout->addWidget(btnPush);
    //保存内容
    connect(m_RemoteUrlEdit, &QLineEdit::editingFinished, this, [=]()
            { ApplyRemoteUrlFromInput(true); });
    //从远程仓库同步（git pull）
    connect(btnPull, &ElaIconButton::clicked, this, [=]()
            {
                const QString repoPath = BackupPath + "/" + m_NowFilePathWithCode;
                //执行 git pull
                QProcess pullProcess;
                pullProcess.setWorkingDirectory(repoPath);
                pullProcess.start("git", QStringList() << "pull" << "origin" << "master");
                pullProcess.waitForFinished();
                //推送结果判断
                if (pullProcess.exitCode() == 0)
                {
                    ElaMessageBar::success(ElaMessageBarType::BottomRight,
                                        "同步成功",
                                        "已从远程仓库拉取最新版本",
                                        2000,
                                        parentWidget());
                    openBackup(m_NowFilePathWithCode); //刷新备份列表
                }
                else
                {
                    QString errorMsg = pullProcess.readAllStandardError();
                    ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                        "同步失败",
                                        errorMsg.isEmpty() ? "请检查网络连接和仓库配置" : errorMsg,
                                        3000,
                                        parentWidget());
                } });
    //上传到远程仓库（git push）
    connect(btnPush, &ElaIconButton::clicked, this, [=]()
            {
                const QString repoPath = BackupPath + "/" + m_NowFilePathWithCode;
                QProcess checkProcess;
                checkProcess.setWorkingDirectory(repoPath);
                // 执行 git push
                QProcess pushProcess;
                pushProcess.setWorkingDirectory(repoPath);
                pushProcess.start("git", QStringList() << "push" << "origin" << "master");
                pushProcess.waitForFinished();
                if (pushProcess.exitCode() == 0)
                {
                    ElaMessageBar::success(ElaMessageBarType::BottomRight,
                                        "上传成功",
                                        "已将本地版本推送到远程仓库",
                                        2000,
                                        parentWidget());
                }
                else
                {
                    QString errorMsg = pushProcess.readAllStandardError();
                    ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                        "上传失败",
                                        errorMsg.isEmpty() ? "请检查网络连接和推送权限" : errorMsg,
                                        3000,
                                        parentWidget());
                } });
    //添加布局
    remoteDrawerLayout->addLayout(remoteRowLayout);
    ui->ElaDrawerArea_Remote->addDrawer(remoteDrawerContent);

    /*创建面包屑*/
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
                LoadBackupFileList(); });
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
        HomePageChild_TrackFile *trackfile_widget = new HomePageChild_TrackFile(name, this); //创建子窗口
        ui->verticalLayout_TrackFiles->addWidget(trackfile_widget);
    }
    //最后再添加一个verticalSpacer
    auto *spacer = new QSpacerItem(20, 40, QSizePolicy::Minimum, QSizePolicy::Expanding);
    ui->verticalLayout_TrackFiles->addItem(spacer);
}

//写入仓库
void HomePage::ApplyRemoteUrlFromInput(bool showSuccessMessage)
{
    const QString url = m_RemoteUrlEdit->text().trimmed();
    const QString repoPath = BackupPath + "/" + m_NowFilePathWithCode;
    QProcess checkProcess;
    //设置远程仓库
    checkProcess.setWorkingDirectory(repoPath);
    checkProcess.start("git", QStringList() << "remote" << "get-url" << "origin");
    checkProcess.waitForFinished();
    const bool hasOrigin = (checkProcess.exitCode() == 0);
    QProcess setProcess;
    setProcess.setWorkingDirectory(repoPath);
    if (hasOrigin)
        setProcess.start("git", QStringList() << "remote" << "set-url" << "origin" << url);
    else
        setProcess.start("git", QStringList() << "remote" << "add" << "origin" << url);
    setProcess.waitForFinished();
    if (setProcess.exitCode() != 0)
    {
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "远程仓库设置失败",
                             setProcess.readAllStandardError(),
                             3000,
                             parentWidget());
        return;
    }
    {
        QSignalBlocker blocker(m_ToggleSwitch_Remote);
        m_ToggleSwitch_Remote->setIsToggled(true);
    }
    ui->ElaDrawerArea_Remote->expand();
    if (showSuccessMessage)
    {
        ElaMessageBar::success(ElaMessageBarType::BottomRight,
                               "远程仓库已自动保存",
                               url,
                               2000,
                               parentWidget());
    }
}

/*打开备份*/
void HomePage::openBackup(QString FilePathWithCode)
{
    m_NowFilePathWithCode = FilePathWithCode;

    /*设置面包屑*/
    ui->widget_BreadcrumbBar->appendBreadcrumb(
        QFileInfo(QUrl::fromPercentEncoding(FilePathWithCode.toUtf8())).baseName());
    ui->stackedWidget->setCurrentIndex(1);

    /*设置表格*/
    //初始化表格模型
    QStandardItemModel *model = new QStandardItemModel(this);
    model->setColumnCount(3); //三列: commit hash, message, 操作
    model->setHorizontalHeaderLabels(QStringList() << "Commit" << "Message" << "操作");
    //获取 Git 仓库的所有 commit
    QString repoPath = BackupPath + "/" + FilePathWithCode;
    QProcess git;
    git.setWorkingDirectory(repoPath);
    git.start("git", QStringList() << "log" << "--oneline");
    git.waitForFinished();
    QString output = git.readAllStandardOutput();
    QStringList commitList = output.split("\n", Qt::SkipEmptyParts);
    //填充模型
    for (const QString &commitInfo : std::as_const(commitList))
    {
        QStringList parts = commitInfo.split(' ', Qt::SkipEmptyParts);
        if (parts.size() < 2)
            continue;
        QString hash = parts.takeFirst();
        QString message = parts.join(" ");
        QList<QStandardItem *> rowItems;
        QStandardItem *item0 = new QStandardItem(hash);
        QStandardItem *item1 = new QStandardItem(message);
        QStandardItem *item2 = new QStandardItem(""); //占位，用于按钮
        item0->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        item1->setTextAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
        item2->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        rowItems << item0 << item1 << item2;
        model->appendRow(rowItems);
    }
    ui->tableView_BackupFiles->setModel(model);                                    //绑定模型到 TableView
    ui->tableView_BackupFiles->verticalHeader()->setVisible(false);                //隐藏左侧行号
    ui->tableView_BackupFiles->setSelectionMode(QAbstractItemView::NoSelection);   //禁止选择
    ui->tableView_BackupFiles->setEditTriggers(QAbstractItemView::NoEditTriggers); //禁止编辑
    //设置列宽和表头行为
    ui->tableView_BackupFiles->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    ui->tableView_BackupFiles->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    ui->tableView_BackupFiles->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    ui->tableView_BackupFiles->setColumnWidth(2, 200); //操作列宽
    //为每一行添加按钮
    for (int row = 0; row < model->rowCount(); ++row)
    {
        QWidget *buttonWidget = new QWidget(ui->tableView_BackupFiles);
        QHBoxLayout *layout = new QHBoxLayout(buttonWidget);
        layout->setContentsMargins(4, 2, 4, 2);
        layout->setSpacing(4);
        ElaPushButton *button1 = new ElaPushButton("预览", buttonWidget);
        ElaPushButton *button2 = new ElaPushButton("还原", buttonWidget);
        QFont font;
        font.setPointSize(10); //字体大小统一
        button1->setFont(font);
        button2->setFont(font);
        layout->addWidget(button1);
        layout->addWidget(button2);
        layout->addStretch();
        QString currentHash = model->item(row, 0)->text();
        connect(button1, &QPushButton::clicked, this, [=]()
                {
                    /*切换到指定的commit*/
                    QString sourceRepoPath = BackupPath + "/" + m_NowFilePathWithCode;
                    QProcess git;
                    git.setWorkingDirectory(sourceRepoPath);
                    QString shortId = currentHash; //获取短 ID 方便后续命名
                    git.start("git", QStringList() << "checkout" << "-f" << shortId); //强制切换到历史版本
                    git.waitForFinished();
                    /*准备临时目标路径*/
                    QString pureFolderName = QFileInfo(m_NowFilePathWithCode).fileName();
                    QString destinationPath = QDir::tempPath() + "/ZcBox_Preview_" + shortId + "_" + pureFolderName;
                    QDir oldDir(destinationPath); //清理已存在的旧预览目录
                    if (oldDir.exists()) oldDir.removeRecursively();
                    /*执行复制并处理属性*/
                    FileUtils::copyDirectory(sourceRepoPath, destinationPath);
                    FileUtils::setReadOnlyRecursive(destinationPath); //设置只读保护
                    QDesktopServices::openUrl(QUrl::fromLocalFile(destinationPath)); //打开文件夹
                    /*将备份仓库切回 master*/
                    git.start("git", QStringList() << "checkout" << "-f" << "master");
                    git.waitForFinished();
                });
        connect(button2, &QPushButton::clicked, this, [=]()
                {
                    //回滚仓库
                    QProcess git;
                    git.setWorkingDirectory(BackupPath + "/" + m_NowFilePathWithCode);
                    QString shortId = currentHash;
                    git.start("git", QStringList() << "reset" << "--hard" << shortId);
                    git.waitForFinished();
                    //替换源文件
                    QString sourceFilePath = QUrl::fromPercentEncoding(m_NowFilePathWithCode.toUtf8());
                    QString backupFilePath = BackupPath + "/" + m_NowFilePathWithCode + "/" + QFileInfo(sourceFilePath).fileName();
                    if(QFile::exists(sourceFilePath)) QFile::remove(sourceFilePath);
                    QFile::copy(backupFilePath, sourceFilePath);
                    //提示
                    ElaMessageBar::success(ElaMessageBarType::BottomRight, "还原成功", "已还原至" + currentHash, 3000, parentWidget());
                    //删除当前行及以上
                    QStandardItemModel* model = qobject_cast<QStandardItemModel*>(ui->tableView_BackupFiles->model());
                    if(model)
                    {
                        //找到当前行索引
                        QModelIndexList matches = model->match(model->index(0,0), Qt::DisplayRole, currentHash, 1, Qt::MatchExactly);
                        if(!matches.isEmpty())
                        {
                            int row = matches.first().row();
                            //删除从0到row行
                            for(int r = row-1; r >= 0; --r) model->removeRow(r);
                        }
                    }
                });
        QModelIndex index = model->index(row, 2);
        ui->tableView_BackupFiles->setIndexWidget(index, buttonWidget);
    }
    //初始化远程开关与远程地址输入框
    QProcess checkProcess;
    checkProcess.setWorkingDirectory(repoPath);
    checkProcess.start("git", QStringList() << "remote" << "get-url" << "origin");
    checkProcess.waitForFinished();
    //读取并设置到输入框
    const bool hasOrigin = (checkProcess.exitCode() == 0);
    const QString originUrl = QString::fromUtf8(checkProcess.readAllStandardOutput()).trimmed();
    if (m_RemoteUrlEdit)
        m_RemoteUrlEdit->setText(hasOrigin ? originUrl : QString());
    m_RemoteUiSyncing = true;
    {
        QSignalBlocker blocker(m_ToggleSwitch_Remote);
        m_ToggleSwitch_Remote->setIsToggled(hasOrigin);
    }
    if (hasOrigin)
        ui->ElaDrawerArea_Remote->expand();
    else
        ui->ElaDrawerArea_Remote->collapse();
    m_RemoteUiSyncing = false;
}

/*面包屑点击返回*/
void HomePage::on_widget_BreadcrumbBar_breadcrumbClicked(QString breadcrumb, QStringList lastBreadcrumbList)
{
    ui->stackedWidget->setCurrentIndex(0);
}

/*远程同步开关*/
void HomePage::on_ToggleSwitch_Remote_toggled(bool checked)
{
    if (m_NowFilePathWithCode.isEmpty())
    {
        return;
    }
    const QString repoPath = BackupPath + "/" + m_NowFilePathWithCode;

    /*关闭远程同步：解绑 origin*/
    if (!checked)
    {
        QProcess checkProcess;
        checkProcess.setWorkingDirectory(repoPath);
        checkProcess.start("git", QStringList() << "remote" << "get-url" << "origin");
        checkProcess.waitForFinished();
        if (checkProcess.exitCode() != 0)
            return;

        QProcess process;
        process.setWorkingDirectory(repoPath);
        process.start("git", QStringList() << "remote" << "remove" << "origin");
        process.waitForFinished();
        if (process.exitCode() == 0)
        {
            if (m_RemoteUrlEdit)
            {
                m_RemoteUrlEdit->clear();
            }
            ElaMessageBar::success(ElaMessageBarType::BottomRight,
                                   "已解绑远程仓库",
                                   "",
                                   2000,
                                   parentWidget());
        }
        else
        {
            ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                 "解绑失败",
                                 process.readAllStandardError(),
                                 3000,
                                 parentWidget());
        }
        return;
    }

    /*开启远程同步：未配置时保持展开并等待输入自动保存*/
    QProcess checkProcess;
    checkProcess.setWorkingDirectory(repoPath);
    checkProcess.start("git", QStringList() << "remote" << "get-url" << "origin");
    checkProcess.waitForFinished();
    //结果反馈和提示
    if (checkProcess.exitCode() != 0)
    {
        ui->ElaDrawerArea_Remote->expand();
        ElaMessageBar::warning(ElaMessageBarType::BottomRight,
                               "请输入远程仓库地址",
                               "输入完成后将自动保存",
                               2500,
                               parentWidget());
        ApplyRemoteUrlFromInput(false);
        return;
    }
    if (m_RemoteUrlEdit)
    {
        const QString originUrl = QString::fromUtf8(checkProcess.readAllStandardOutput()).trimmed();
        m_RemoteUrlEdit->setText(originUrl);
    }
    ElaMessageBar::success(ElaMessageBarType::BottomRight,
                           "远程同步已开启",
                           "",
                           2000,
                           parentWidget());
}
