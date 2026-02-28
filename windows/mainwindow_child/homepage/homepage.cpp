#include "homepage.h"
#include "ui_homepage.h"

#include "homepagechild_trackfile.h"
#include "../../../utils/fileutils.h"
#include "../../../GlobalConstants.h"

#include "ElaPushButton.h"
#include "ElaMessageBar.h"
#include "ElaContentDialog.h"
#include "ElaLineEdit.h"

#include <QDesktopServices>
#include <QStandardItem>
#include <QStandardPaths>
#include <QDir>
#include <QSpacerItem>
#include <QFileSystemWatcher>
#include <QUrl>
#include <QProcess>

#include <QInputDialog>
#include <QLineEdit>

HomePage::HomePage(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::HomePage)
{
    /*窗口初始化*/
    ui->setupUi(this);
    LoadBackupFileList();


    // ===== Header =====
    QWidget* drawerHeader = new QWidget(this);
    QHBoxLayout* drawerHeaderLayout = new QHBoxLayout(drawerHeader);
    drawerHeaderLayout->setContentsMargins(8,0,8,0);

    ElaText* drawerIcon = new ElaText(this);
    drawerIcon->setTextPixelSize(15);
    drawerIcon->setElaIcon(ElaIconType::MessageArrowDown);
    drawerIcon->setFixedSize(25,25);

    ElaText* drawerText = new ElaText("远程仓库", this);
    drawerText->setTextPixelSize(15);

    // ===== 开关 =====
    ElaToggleSwitch* drawerSwitch = new ElaToggleSwitch(this);
    ElaText* drawerSwitchText = new ElaText("关", this);
    drawerSwitchText->setTextPixelSize(15);

    //开关控制Drawer
    connect(drawerSwitch, &ElaToggleSwitch::toggled,
            this, [=](bool toggled)
            {
                if(toggled)
                {
                    drawerSwitchText->setText("开");
                    ui->ElaDrawerArea_Remote->expand();
                }
                else
                {
                    drawerSwitchText->setText("关");
                    ui->ElaDrawerArea_Remote->collapse();
                }
            });

    //Drawer状态反向控制开关
    connect(ui->ElaDrawerArea_Remote,
            &ElaDrawerArea::expandStateChanged,
            this,
            [=](bool isExpand)
            {
                drawerSwitch->setIsToggled(isExpand);
            });

    // ===== Header布局 =====
    drawerHeaderLayout->addWidget(drawerIcon);
    drawerHeaderLayout->addWidget(drawerText);
    drawerHeaderLayout->addStretch();
    drawerHeaderLayout->addWidget(drawerSwitchText);
    drawerHeaderLayout->addWidget(drawerSwitch);

    //设置Header
    ui->ElaDrawerArea_Remote->setDrawerHeader(drawerHeader);




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
    m_NowFilePathWithCode = FilePathWithCode;

    /*设置面包屑*/
    ui->widget_BreadcrumbBar->appendBreadcrumb(
        QFileInfo(QUrl::fromPercentEncoding(FilePathWithCode.toUtf8())).baseName()
        );
    ui->stackedWidget->setCurrentIndex(1);

    /*设置表格*/
    //初始化表格模型
    QStandardItemModel *model = new QStandardItemModel(this);
    model->setColumnCount(3); // 三列: commit hash, message, 操作
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
        if (parts.size() < 2) continue;
        QString hash = parts.takeFirst();
        QString message = parts.join(" ");
        QList<QStandardItem*> rowItems;
        QStandardItem *item0 = new QStandardItem(hash);
        QStandardItem *item1 = new QStandardItem(message);
        QStandardItem *item2 = new QStandardItem(""); // 占位，用于按钮
        item0->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        item1->setTextAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
        item2->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        rowItems << item0 << item1 << item2;
        model->appendRow(rowItems);
    }
    ui->tableView_BackupFiles->setModel(model); //绑定模型到 TableView
    ui->tableView_BackupFiles->verticalHeader()->setVisible(false); //隐藏左侧行号
    ui->tableView_BackupFiles->setSelectionMode(QAbstractItemView::NoSelection); //禁止选择
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
        connect(button1, &QPushButton::clicked, this, [=]() {
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
        connect(button2, &QPushButton::clicked, this, [=]() {
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


    //初始化远程开关
    QProcess checkProcess;
    checkProcess.setWorkingDirectory(repoPath);
    checkProcess.start("git", QStringList() << "remote" << "get-url" << "origin");
    checkProcess.waitForFinished();
    ui->ToggleSwitch_Remote->setIsToggled(checkProcess.exitCode() == 0);
}

/*面包屑点击返回*/
void HomePage::on_widget_BreadcrumbBar_breadcrumbClicked(QString breadcrumb, QStringList lastBreadcrumbList)
{
    ui->stackedWidget->setCurrentIndex(0);
}

/*远程同步开关*/
void HomePage::on_ToggleSwitch_Remote_toggled(bool checked)
{
    QString repoPath = BackupPath + "/" + m_NowFilePathWithCode;
    /*解绑远程*/
    if(!checked)
    {
        QString repoPath = BackupPath + "/" + m_NowFilePathWithCode;
        QProcess process;
        process.setWorkingDirectory(repoPath);
        process.start("git", QStringList() << "remote" << "remove" << "origin");
        process.waitForFinished();
        if(process.exitCode() == 0)
            ElaMessageBar::success(ElaMessageBarType::BottomRight,
                                   "已解绑远程仓库",
                                   "",
                                   2000,
                                   parentWidget());
        else
            ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                 "解绑失败",
                                 process.readAllStandardError(),
                                 3000,
                                 parentWidget());
        return;
    }
    /*检查是否存在 origin*/
    QProcess checkProcess;
    checkProcess.setWorkingDirectory(repoPath);
    checkProcess.start("git", QStringList() << "remote" << "get-url" << "origin");
    checkProcess.waitForFinished();
    /*没有远程，要求输入*/
    if(checkProcess.exitCode() != 0)
    {
        /*远程仓库输入*/
        ElaContentDialog dialog(this);
        dialog.setWindowTitle(u8"设置远程仓库");
        dialog.setLeftButtonText(u8"取消");
        dialog.setRightButtonText(u8"确认");
        //关闭中间按钮
        for(auto btn : dialog.findChildren<ElaPushButton*>())
        {
            if(btn->text() == "minimum")
            {
                btn->hide();
                break;
            }
        }
        ElaLineEdit* edit = new ElaLineEdit(&dialog);
        edit->setPlaceholderText(u8"请输入远程仓库地址:");
        dialog.setCentralWidget(edit);
        //按钮行为
        connect(&dialog,&ElaContentDialog::rightButtonClicked,&dialog,&QDialog::accept);
        connect(&dialog,&ElaContentDialog::leftButtonClicked,&dialog,&QDialog::reject);
        if(dialog.exec()!=QDialog::Accepted)
        {
            qDebug()<<"取消";
            QSignalBlocker blocker(ui->ToggleSwitch_Remote);
            ui->ToggleSwitch_Remote->setIsToggled(false);
            return;
        }
        QString url = edit->text().trimmed();
        if(url.isEmpty())
        {
            ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                 "远程仓库设置失败",
                                 "无效的Url",
                                 3000,
                                 parentWidget());
            QSignalBlocker blocker(ui->ToggleSwitch_Remote);
            ui->ToggleSwitch_Remote->setIsToggled(false);
            return;
        }
        /*添加远程*/
        QProcess addProcess;
        addProcess.setWorkingDirectory(repoPath);
        addProcess.start("git",
                         QStringList() << "remote"
                                       << "add"
                                       << "origin"
                                       << url.trimmed());
        addProcess.waitForFinished();
        if(addProcess.exitCode() != 0)
        {
            ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                 "远程仓库设置失败",
                                 addProcess.readAllStandardError(),
                                 3000,
                                 parentWidget());
            ui->ToggleSwitch_Remote->setIsToggled(false);
            return;
        }
        ElaMessageBar::success(ElaMessageBarType::BottomRight,
                               "远程仓库设置成功",
                               url,
                               3000,
                               parentWidget());
    }
    ElaMessageBar::success(ElaMessageBarType::BottomRight,
                           "远程同步已开启",
                           "",
                           2000,
                           parentWidget());
}
