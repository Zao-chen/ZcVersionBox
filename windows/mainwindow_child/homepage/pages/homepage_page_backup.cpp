#include "../homepage.h"
#define HOMEPAGE_UI_HEADER "ui_homepage.h"
#include HOMEPAGE_UI_HEADER
#undef HOMEPAGE_UI_HEADER

#include "../../../../GlobalConstants.h"
#include "../../../../utils/fileutils.h"

#include "ElaMessageBar.h"
#include "ElaPushButton.h"

#include <QAbstractItemModel>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QItemDelegate>
#include <QProcess>
#include <QStandardItem>
#include <QUrl>
#include <QVBoxLayout>

//自定义委体，只允许第二列编辑
class EditableColumnDelegate : public QItemDelegate
{
  public:
    explicit EditableColumnDelegate(QObject *parent = nullptr) : QItemDelegate(parent) {}

    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        //只允许第二列（索引1）编辑
        if (index.column() == 1)
            return QItemDelegate::createEditor(parent, option, index);
        return nullptr;
    }
};

/*打开备份*/
void HomePage::openBackup(QString FilePathWithCode)
{
    m_NowFilePathWithCode = FilePathWithCode;

    /*设置面包屑*/
    const QString displayName = QFileInfo(QUrl::fromPercentEncoding(FilePathWithCode.toUtf8())).baseName();
    ui->widget_BreadcrumbBar->setBreadcrumbList(QStringList() << "备份中文件" << displayName + " 历史版本");
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

    //填充列表
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
        item0->setFlags(item0->flags() & ~Qt::ItemIsEditable);
        item1->setFlags(item1->flags() | Qt::ItemIsEditable);
        item2->setFlags(item2->flags() & ~Qt::ItemIsEditable);
        item0->setTextAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
        item1->setTextAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
        item2->setTextAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
        rowItems << item0 << item1 << item2;
        model->appendRow(rowItems);
    }

    ui->tableView_BackupFiles->setModel(model);
    ui->tableView_BackupFiles->verticalHeader()->setVisible(false);
    ui->tableView_BackupFiles->setSelectionMode(QAbstractItemView::SingleSelection);
    ui->tableView_BackupFiles->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);

    // 使用默认委托以兼容 ElaTableView 主题渲染；通过 item flags 限制可编辑列

    //设置列宽和表头行为
    ui->tableView_BackupFiles->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    ui->tableView_BackupFiles->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    ui->tableView_BackupFiles->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    ui->tableView_BackupFiles->setColumnWidth(0, 80);  //版本号列宽
    ui->tableView_BackupFiles->setColumnWidth(2, 240); //操作列宽

    //为每一行添加按钮
    for (int row = 0; row < model->rowCount(); ++row)
    {
        QWidget *buttonWidget = new QWidget(ui->tableView_BackupFiles);
        QHBoxLayout *layout = new QHBoxLayout(buttonWidget);
        layout->setContentsMargins(4, 2, 4, 2);
        layout->setSpacing(4);

        ElaPushButton *button1 = new ElaPushButton("查看", buttonWidget);
        ElaPushButton *button2 = new ElaPushButton("恢复", buttonWidget);
        ElaPushButton *button3 = new ElaPushButton("对比", buttonWidget);
        QFont font;
        font.setPointSize(10); //字体大小统一
        button1->setFont(font);
        button2->setFont(font);
        button3->setFont(font);

        layout->addStretch();
        layout->addWidget(button1);
        layout->addWidget(button2);
        layout->addWidget(button3);
        layout->addStretch();

        QString currentHash = model->item(row, 0)->text();
        //预览
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
        //恢复
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

        //对比上一版本
        connect(button3, &QPushButton::clicked, this, [=]()
                {
                    openDiff(currentHash);
                });

        QModelIndex index = model->index(row, 2);
        ui->tableView_BackupFiles->setIndexWidget(index, buttonWidget);
    }

    //修改提交信息
    connect(model, &QStandardItemModel::itemChanged, this, [=](QStandardItem *item)
            {
        //只处理第二列（说明列）的修改
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

        //使用 git commit --amend 修改提交消息
        QString gitRepoPath = BackupPath + "/" + m_NowFilePathWithCode;
        
        //获取当前HEAD的hash
        QProcess getCurrentHash;
        getCurrentHash.setWorkingDirectory(gitRepoPath);
        getCurrentHash.start("git", QStringList() << "rev-parse" << "HEAD");
        getCurrentHash.waitForFinished();
        QString currentHash = QString::fromUtf8(getCurrentHash.readAllStandardOutput()).trimmed();

        if (hash == currentHash)
        {
            //修改最新提交的消息
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
            //恢复为原来的文本
            QProcess getCommitMsg;
            getCommitMsg.setWorkingDirectory(gitRepoPath);
            getCommitMsg.start("git", QStringList() << "log" << "-1" << "--format=%B" << hash);
            getCommitMsg.waitForFinished();
            QString originalMessage = QString::fromUtf8(getCommitMsg.readAllStandardOutput()).trimmed();
            item->setText(originalMessage);
        } });

    //设置焦点到表格，避免自动聚焦到远程URL输入框
    ui->tableView_BackupFiles->setFocus();
}
