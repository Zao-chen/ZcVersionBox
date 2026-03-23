#include "../homepage.h"
#define HOMEPAGE_UI_HEADER "ui_homepage.h"
#include HOMEPAGE_UI_HEADER
#undef HOMEPAGE_UI_HEADER

#include "../../../../GlobalConstants.h"
#include "../../../../utils/fileutils.h"

#include "ElaIconButton.h"
#include "ElaLineEdit.h"
#include "ElaMessageBar.h"
#include "ElaText.h"
#include "ElaToggleSwitch.h"
#include "elapushbutton.h"

#include <QAbstractItemModel>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QItemDelegate>
#include <QProcess>
#include <QSignalBlocker>
#include <QStandardItem>
#include <QUrl>
#include <QVBoxLayout>

// 自定义委体，只允许第二列编辑
class EditableColumnDelegate : public QItemDelegate
{
  public:
    explicit EditableColumnDelegate(QObject *parent = nullptr) : QItemDelegate(parent) {}

    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        // 只允许第二列（索引1）编辑
        if (index.column() == 1)
        {
            return QItemDelegate::createEditor(parent, option, index);
        }
        return nullptr;
    }
};

/*页面初始化*/
void HomePage::SetupBackupPage()
{
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
    ElaText *drawerText = new ElaText("云端同步", this);
    drawerText->setTextPixelSize(15);

    //开关
    connect(m_ToggleSwitch_Remote, &ElaToggleSwitch::toggled, this,
            [=](bool checked)
            {
                if (checked)
                    ui->ElaDrawerArea_Remote->expand();
                else
                    ui->ElaDrawerArea_Remote->collapse();
            });
    connect(ui->ElaDrawerArea_Remote, &ElaDrawerArea::expandStateChanged, this,
            [=](bool isExpand)
            {
                m_ToggleSwitch_Remote->setIsToggled(isExpand);
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
    ElaText *remoteHintText = new ElaText("云端地址", remoteDrawerContent);
    remoteHintText->setTextPixelSize(13);
    m_RemoteUrlEdit = new ElaLineEdit(remoteDrawerContent);
    m_RemoteUrlEdit->setPlaceholderText(u8"请输入云端地址（例如 Git 仓库链接）");

    QHBoxLayout *remoteRowLayout = new QHBoxLayout();
    remoteRowLayout->addWidget(remoteHintText);
    remoteRowLayout->addWidget(m_RemoteUrlEdit, 1);

    //创建同步和上传图标按钮
    ElaIconButton *btnPull = new ElaIconButton(ElaIconType::Download, 16, remoteDrawerContent);
    btnPull->setFixedSize(32, 32);
    btnPull->setToolTip("从云端拉取最新内容");
    ElaIconButton *btnPush = new ElaIconButton(ElaIconType::Upload, 16, remoteDrawerContent);
    btnPush->setFixedSize(32, 32);
    btnPush->setToolTip("上传本地更改到云端");
    ElaIconButton *btnOpen = new ElaIconButton(ElaIconType::Link, 16, remoteDrawerContent);
    btnOpen->setFixedSize(32, 32);
    btnOpen->setToolTip("在浏览器打开云端地址");

    remoteRowLayout->addWidget(btnOpen);
    remoteRowLayout->addWidget(btnPull);
    remoteRowLayout->addWidget(btnPush);

    //保存内容
    connect(m_RemoteUrlEdit, &QLineEdit::editingFinished, this, [=]()
            { ApplyRemoteUrlFromInput(true); });
    //打开远程仓库链接
    connect(btnOpen, &ElaIconButton::clicked, this, [=]()
            {
                QString repoUrl = m_RemoteUrlEdit->text().trimmed();
                if (repoUrl.isEmpty())
                {
                    ElaMessageBar::warning(ElaMessageBarType::BottomRight,
                                           "打开失败",
                                           "请先填写云端地址",
                                           2000,
                                           parentWidget());
                    return;
                }
                if (!QDesktopServices::openUrl(QUrl(repoUrl)))
                {
                    ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                         "打开失败",
                                         "无法打开云端地址，请检查链接格式",
                                         3000,
                                         parentWidget());
                } });
    //从远程仓库同步（git pull）
    connect(btnPull, &ElaIconButton::clicked, this, [=]()
            {
                const QString repoPath = BackupPath + "/" + m_NowFilePathWithCode;
                if (!QDir(repoPath).exists())
                {
                    ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                         "同步失败",
                                         "本地备份仓库不存在",
                                         3000,
                                         parentWidget());
                    return;
                }
                //执行 git pull
                QProcess pullProcess;
                pullProcess.setWorkingDirectory(repoPath);
                pullProcess.start("git", QStringList() << "pull" << "origin" << "master");
                if (!pullProcess.waitForStarted())
                {
                    ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                         "同步失败",
                                         "无法启动 git，请确认 git 已安装",
                                         3000,
                                         parentWidget());
                    return;
                }
                pullProcess.waitForFinished();
                //推送结果判断
                if (pullProcess.exitCode() == 0)
                {
                    ElaMessageBar::success(ElaMessageBarType::BottomRight,
                                           "已同步",
                                           "已从云端获取最新内容",
                                           2000,
                                           parentWidget());
                    openBackup(m_NowFilePathWithCode);
                }
                else
                {
                    QString errorMsg = pullProcess.readAllStandardError();
                    ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                         "同步失败",
                                         errorMsg.isEmpty() ? "请检查网络和云端地址是否正确" : errorMsg,
                                         3000,
                                         parentWidget());
                } });
    //上传到远程仓库（git push）
    connect(btnPush, &ElaIconButton::clicked, this, [=]()
            {
                const QString repoPath = BackupPath + "/" + m_NowFilePathWithCode;
                if (!QDir(repoPath).exists())
                {
                    ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                         "上传失败",
                                         "本地备份仓库不存在",
                                         3000,
                                         parentWidget());
                    return;
                }
                // 执行 git push
                QProcess pushProcess;
                pushProcess.setWorkingDirectory(repoPath);
                pushProcess.start("git", QStringList() << "push" << "origin" << "master");
                if (!pushProcess.waitForStarted())
                {
                    ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                         "上传失败",
                                         "无法启动 git，请确认 git 已安装",
                                         3000,
                                         parentWidget());
                    return;
                }
                pushProcess.waitForFinished();
                if (pushProcess.exitCode() == 0)
                {
                    ElaMessageBar::success(ElaMessageBarType::BottomRight,
                                           "上传完成",
                                           "本地更改已上传到云端",
                                           2000,
                                           parentWidget());
                }
                else
                {
                    QString errorMsg = pushProcess.readAllStandardError();
                    ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                         "上传失败",
                                         errorMsg.isEmpty() ? "请检查网络和上传权限" : errorMsg,
                                         3000,
                                         parentWidget());
                } });

    //添加布局
    remoteDrawerLayout->addLayout(remoteRowLayout);
    ui->ElaDrawerArea_Remote->addDrawer(remoteDrawerContent);
}

/*写入仓库*/
void HomePage::ApplyRemoteUrlFromInput(bool showSuccessMessage)
{
    const QString url = m_RemoteUrlEdit->text().trimmed();
    const QString repoPath = BackupPath + "/" + m_NowFilePathWithCode;

    if (url.isEmpty())
    {
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "云端地址保存失败",
                             "云端地址不能为空",
                             3000,
                             parentWidget());
        return;
    }

    if (!QDir(repoPath).exists())
    {
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "云端地址保存失败",
                             "本地备份仓库不存在",
                             3000,
                             parentWidget());
        return;
    }

    QProcess checkProcess;
    //设置远程仓库
    checkProcess.setWorkingDirectory(repoPath);
    checkProcess.start("git", QStringList() << "remote" << "get-url" << "origin");
    if (!checkProcess.waitForStarted())
    {
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "云端地址保存失败",
                             "无法启动 git，请确认 git 已安装",
                             3000,
                             parentWidget());
        return;
    }
    checkProcess.waitForFinished();

    const bool hasOrigin = (checkProcess.exitCode() == 0);

    QProcess setProcess;
    setProcess.setWorkingDirectory(repoPath);
    if (hasOrigin)
        setProcess.start("git", QStringList() << "remote" << "set-url" << "origin" << url);
    else
        setProcess.start("git", QStringList() << "remote" << "add" << "origin" << url);
    if (!setProcess.waitForStarted())
    {
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "云端地址保存失败",
                             "无法启动 git，请确认 git 已安装",
                             3000,
                             parentWidget());
        return;
    }
    setProcess.waitForFinished();

    if (setProcess.exitCode() != 0)
    {
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "云端地址保存失败",
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
                               "云端地址已保存",
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
    model->setColumnCount(3);
    model->setHorizontalHeaderLabels(QStringList() << "版本号" << "说明" << "操作"); //三列: commit hash, message, 操作

    //获取 Git 仓库的所有 commit
    QString repoPath = BackupPath + "/" + FilePathWithCode;
    QProcess git;
    git.setWorkingDirectory(repoPath);
    git.start("git", QStringList() << "log" << "--oneline");
    if (!git.waitForStarted())
    {
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "打开备份失败",
                             "无法启动 git，请确认 git 已安装",
                             3000,
                             parentWidget());
        return;
    }
    git.waitForFinished();

    if (git.exitCode() != 0)
    {
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "打开备份失败",
                             QString::fromUtf8(git.readAllStandardError()).trimmed(),
                             3000,
                             parentWidget());
        return;
    }

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

    ui->tableView_BackupFiles->setModel(model);
    ui->tableView_BackupFiles->verticalHeader()->setVisible(false);
    ui->tableView_BackupFiles->setSelectionMode(QAbstractItemView::SingleSelection);
    ui->tableView_BackupFiles->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);

    // 设置自定义委体，限制只能编辑第二列
    ui->tableView_BackupFiles->setItemDelegate(new EditableColumnDelegate(ui->tableView_BackupFiles));

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

        ElaPushButton *button1 = new ElaPushButton("查看", buttonWidget);
        ElaPushButton *button2 = new ElaPushButton("恢复", buttonWidget);
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
                    if (!git.waitForStarted())
                    {
                        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                             "查看失败",
                                             "无法启动 git，请确认 git 已安装",
                                             3000,
                                             parentWidget());
                        return;
                    }
                    git.waitForFinished();
                    if (git.exitCode() != 0)
                    {
                        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                             "查看失败",
                                             QString::fromUtf8(git.readAllStandardError()).trimmed(),
                                             3000,
                                             parentWidget());
                        return;
                    }

                    /*准备临时目标路径*/
                    QString pureFolderName = QFileInfo(m_NowFilePathWithCode).fileName();
                    QString destinationPath = QDir::tempPath() + "/ZcBox_Preview_" + shortId + "_" + pureFolderName;
                    QDir oldDir(destinationPath); //清理已存在的旧预览目录
                    if (oldDir.exists())
                    {
                        if (!oldDir.removeRecursively())
                        {
                            ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                                 "查看失败",
                                                 "清理旧预览目录失败",
                                                 3000,
                                                 parentWidget());
                            git.start("git", QStringList() << "checkout" << "-f" << "master");
                            git.waitForFinished();
                            return;
                        }
                    }

                    /*执行复制并处理属性*/
                    if (!FileUtils::copyDirectory(sourceRepoPath, destinationPath))
                    {
                        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                             "查看失败",
                                             "复制预览文件失败",
                                             3000,
                                             parentWidget());
                        git.start("git", QStringList() << "checkout" << "-f" << "master");
                        git.waitForFinished();
                        return;
                    }
                    FileUtils::setReadOnlyRecursive(destinationPath); //设置只读保护
                    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(destinationPath)))
                    {
                        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                             "查看失败",
                                             "无法打开预览目录",
                                             3000,
                                             parentWidget());
                    }

                    /*将备份仓库切回 master*/
                    git.start("git", QStringList() << "checkout" << "-f" << "master");
                    if (git.waitForStarted())
                    {
                        git.waitForFinished();
                        if (git.exitCode() != 0)
                        {
                            ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                                 "查看失败",
                                                 "恢复工作分支失败，请手动执行 git checkout -f master",
                                                 3500,
                                                 parentWidget());
                        }
                    }
                    else
                    {
                        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                             "查看失败",
                                             "无法启动 git 恢复工作分支",
                                             3500,
                                             parentWidget());
                    } });

        connect(button2, &QPushButton::clicked, this, [=]()
                {
                    //回滚仓库
                    QProcess git;
                    git.setWorkingDirectory(BackupPath + "/" + m_NowFilePathWithCode);
                    QString shortId = currentHash;
                    git.start("git", QStringList() << "reset" << "--hard" << shortId);
                    if (!git.waitForStarted())
                    {
                        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                             "还原失败",
                                             "无法启动 git，请确认 git 已安装",
                                             3000,
                                             parentWidget());
                        return;
                    }
                    git.waitForFinished();
                    if (git.exitCode() != 0)
                    {
                        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                             "还原失败",
                                             QString::fromUtf8(git.readAllStandardError()).trimmed(),
                                             3000,
                                             parentWidget());
                        return;
                    }

                    //替换源文件
                    QString sourceFilePath = QUrl::fromPercentEncoding(m_NowFilePathWithCode.toUtf8());
                    QString backupFilePath = BackupPath + "/" + m_NowFilePathWithCode + "/" + QFileInfo(sourceFilePath).fileName();
                    if (QFile::exists(sourceFilePath))
                    {
                        if (!QFile::remove(sourceFilePath))
                        {
                            ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                                 "还原失败",
                                                 "无法删除原文件，请检查占用状态",
                                                 3000,
                                                 parentWidget());
                            return;
                        }
                    }
                    if (!QFile::copy(backupFilePath, sourceFilePath))
                    {
                        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                             "还原失败",
                                             "复制备份文件失败，请检查权限",
                                             3000,
                                             parentWidget());
                        return;
                    }

                    //提示
                    ElaMessageBar::success(ElaMessageBarType::BottomRight,
                                           "还原成功",
                                           "已恢复到版本 " + currentHash,
                                           3000,
                                           parentWidget());

                    //删除当前行及以上
                    QStandardItemModel *currentModel = qobject_cast<QStandardItemModel *>(ui->tableView_BackupFiles->model());
                    if (currentModel)
                    {
                        //找到当前行索引
                        QModelIndexList matches = currentModel->match(currentModel->index(0, 0), Qt::DisplayRole, currentHash, 1, Qt::MatchExactly);
                        if (!matches.isEmpty())
                        {
                            int currentRow = matches.first().row();
                            //删除从0到row行
                            for (int r = currentRow - 1; r >= 0; --r)
                                currentModel->removeRow(r);
                        }
                    } });

        QModelIndex index = model->index(row, 2);
        ui->tableView_BackupFiles->setIndexWidget(index, buttonWidget);
    }

    // 连接模型的itemChanged信号，处理提交消息编辑
    connect(model, &QStandardItemModel::itemChanged, this, [=](QStandardItem *item)
            {
        // 只处理第二列（说明列）的修改
        if (item->column() != 1)
            return;

        int row = item->row();
        QString hash = model->item(row, 0)->text();
        QString newMessage = item->text();

        if (newMessage.isEmpty())
        {
            ElaMessageBar::warning(ElaMessageBarType::BottomRight,
                                   "错误",
                                   "提交说明不能为空",
                                   2000,
                                   parentWidget());
            return;
        }

        // 使用 git commit --amend 修改提交消息
        // 注意：只能修改最新的提交，对于历史提交需要使用 rebase
        QString gitRepoPath = BackupPath + "/" + m_NowFilePathWithCode;
        
        // 获取当前HEAD的hash
        QProcess getCurrentHash;
        getCurrentHash.setWorkingDirectory(gitRepoPath);
        getCurrentHash.start("git", QStringList() << "rev-parse" << "HEAD");
        getCurrentHash.waitForFinished();
        QString currentHash = QString::fromUtf8(getCurrentHash.readAllStandardOutput()).trimmed();

        if (hash == currentHash)
        {
            // 修改最新提交的消息
            QProcess amendProcess;
            amendProcess.setWorkingDirectory(gitRepoPath);
            amendProcess.start("git", QStringList() << "commit" << "--amend" << "-m" << newMessage);
            amendProcess.waitForFinished();

            if (amendProcess.exitCode() == 0)
            {
                ElaMessageBar::success(ElaMessageBarType::BottomRight,
                                       "已保存",
                                       "提交说明已更新",
                                       2000,
                                       parentWidget());
            }
            else
            {
                QString error = QString::fromUtf8(amendProcess.readAllStandardError());
                ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                     "保存失败",
                                     error.isEmpty() ? "更新提交说明失败" : error,
                                     3000,
                                     parentWidget());
            }
        }
        else
        {
            ElaMessageBar::warning(ElaMessageBarType::BottomRight,
                                   "无法编辑",
                                   "只能编辑最新的提交说明",
                                   2000,
                                   parentWidget());
            // 恢复为原来的文本
            QProcess getCommitMsg;
            getCommitMsg.setWorkingDirectory(gitRepoPath);
            getCommitMsg.start("git", QStringList() << "log" << "-1" << "--format=%B" << hash);
            getCommitMsg.waitForFinished();
            QString originalMessage = QString::fromUtf8(getCommitMsg.readAllStandardOutput()).trimmed();
            item->setText(originalMessage);
        } });

    //初始化远程开关与远程地址输入框
    QProcess checkProcess;
    checkProcess.setWorkingDirectory(repoPath);
    checkProcess.start("git", QStringList() << "remote" << "get-url" << "origin");
    if (!checkProcess.waitForStarted())
    {
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "云端同步失败",
                             "无法启动 git，请确认 git 已安装",
                             3000,
                             parentWidget());
        return;
    }
    checkProcess.waitForFinished();

    //读取并设置到输入框
    const bool hasOrigin = (checkProcess.exitCode() == 0);
    const QString originUrl = QString::fromUtf8(checkProcess.readAllStandardOutput()).trimmed();
    if (m_RemoteUrlEdit)
        m_RemoteUrlEdit->setText(hasOrigin ? originUrl : QString());
    {
        QSignalBlocker blocker(m_ToggleSwitch_Remote);
        m_ToggleSwitch_Remote->setIsToggled(hasOrigin);
    }

    if (hasOrigin)
        ui->ElaDrawerArea_Remote->expand();
    else
        ui->ElaDrawerArea_Remote->collapse();

    /*设置焦点到表格，避免自动聚焦到远程URL输入框*/
    ui->tableView_BackupFiles->setFocus();
}

/*远程同步开关*/
void HomePage::on_ToggleSwitch_Remote_toggled(bool checked)
{
    if (m_NowFilePathWithCode.isEmpty())
        return;

    const QString repoPath = BackupPath + "/" + m_NowFilePathWithCode;
    /*关闭远程同步：解绑 origin*/
    if (!checked)
    {
        QProcess checkProcess;
        checkProcess.setWorkingDirectory(repoPath);
        checkProcess.start("git", QStringList() << "remote" << "get-url" << "origin");
        if (!checkProcess.waitForStarted())
        {
            ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                 "关闭云端同步失败",
                                 "无法启动 git，请确认 git 已安装",
                                 3000,
                                 parentWidget());
            return;
        }
        checkProcess.waitForFinished();
        if (checkProcess.exitCode() != 0)
            return;

        QProcess process;
        process.setWorkingDirectory(repoPath);
        process.start("git", QStringList() << "remote" << "remove" << "origin");
        if (!process.waitForStarted())
        {
            ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                 "关闭云端同步失败",
                                 "无法启动 git，请确认 git 已安装",
                                 3000,
                                 parentWidget());
            return;
        }
        process.waitForFinished();

        if (process.exitCode() == 0)
        {
            if (m_RemoteUrlEdit)
                m_RemoteUrlEdit->clear();
            ElaMessageBar::success(ElaMessageBarType::BottomRight,
                                   "已关闭云端同步",
                                   "",
                                   2000,
                                   parentWidget());
        }
        else
        {
            ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                 "关闭云端同步失败",
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
    if (!checkProcess.waitForStarted())
    {
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "云端同步失败",
                             "无法启动 git，请确认 git 已安装",
                             3000,
                             parentWidget());
        return;
    }
    checkProcess.waitForFinished();
    //结果反馈和提示
    if (checkProcess.exitCode() != 0)
    {
        ui->ElaDrawerArea_Remote->expand();
        ElaMessageBar::warning(ElaMessageBarType::BottomRight,
                               "请输入云端地址",
                               "填写后会自动保存",
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
                           "云端同步已开启",
                           "",
                           2000,
                           parentWidget());
}
