#include "../homepage.h"
#define HOMEPAGE_UI_HEADER "ui_homepage.h"
#include HOMEPAGE_UI_HEADER
#undef HOMEPAGE_UI_HEADER
#include "ui_homepage_page_dashboard.h"

#include "../../../../GlobalConstants.h"

#include "ElaContentDialog.h"
#include "ElaDrawerArea.h"
#include "ElaIconButton.h"
#include "ElaLineEdit.h"
#include "ElaMessageBar.h"
#include "ElaPushButton.h"
#include "ElaScrollPageArea.h"
#include "ElaText.h"
#include "ElaToggleSwitch.h"

#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QProcess>
#include <QSignalBlocker>
#include <QSizePolicy>
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

QString decodedPath(const QString &filePathWithCode)
{
    return QUrl::fromPercentEncoding(filePathWithCode.toUtf8());
}

QString repoPathFor(const QString &filePathWithCode)
{
    return BackupPath + "/" + filePathWithCode;
}

QString snapshotPathFor(const QString &filePathWithCode)
{
    const QString sourcePath = decodedPath(filePathWithCode);
    return repoPathFor(filePathWithCode) + "/" + QFileInfo(sourcePath).fileName();
}

QString formatBytes(qint64 bytes)
{
    const QStringList units = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unitIndex = 0;
    while (value >= 1024.0 && unitIndex < units.size() - 1)
    {
        value /= 1024.0;
        ++unitIndex;
    }

    if (unitIndex == 0)
        return QString::number(bytes) + " " + units.at(unitIndex);
    return QString::number(value, 'f', value >= 10.0 ? 1 : 2) + " " + units.at(unitIndex);
}

QString runGit(const QString &repoPath, const QStringList &arguments, bool *ok = nullptr)
{
    // Review note: dashboard 只需要同步 Git 的短命令结果；成功返回 stdout，失败返回 stderr 方便直接展示。
    QProcess git;
    git.setWorkingDirectory(repoPath);
    git.start("git", arguments);
    if (!git.waitForStarted())
    {
        if (ok)
            *ok = false;
        return {};
    }

    git.waitForFinished();
    const bool success = git.exitCode() == 0;
    if (ok)
        *ok = success;
    return QString::fromUtf8(success ? git.readAllStandardOutput() : git.readAllStandardError()).trimmed();
}

QPair<int, qint64> collectPathStats(const QString &path)
{
    QFileInfo info(path);
    if (!info.exists())
        return {0, 0};

    if (info.isFile())
        return {1, info.size()};

    int fileCount = 0;
    qint64 totalBytes = 0;
    QDirIterator it(path,
                    QDir::Files | QDir::Hidden | QDir::Readable | QDir::NoSymLinks,
                    QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        it.next();
        QFileInfo fileInfo = it.fileInfo();
        ++fileCount;
        totalBytes += fileInfo.size();
    }
    return {fileCount, totalBytes};
}

void configureText(ElaText *label, int pixelSize, bool clipSingleLine = false)
{
    if (!label)
        return;

    label->setTextPixelSize(pixelSize);

    // Review note: 使用 ElaText 的字号构造后，再同步 QWidget font，保证布局高度和 ElaText 绘制字号一致。
    QFont layoutFont = label->font();
    layoutFont.setPixelSize(pixelSize);
    label->setFont(layoutFont);
    label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    const int lineHeight = QFontMetrics(layoutFont).lineSpacing() + (pixelSize >= 20 ? 12 : 4);
    label->setMinimumHeight(lineHeight);

    label->setMinimumWidth(0);
    if (clipSingleLine)
    {
        label->setWordWrap(false);

        // Review note: 单行路径/文件名允许横向裁切，不能让完整文本宽度反向撑大窗口。
        // 这里只忽略水平 sizeHint，垂直高度仍交给 ElaText 自身布局计算。
        QSizePolicy textPolicy = label->sizePolicy();
        textPolicy.setHorizontalPolicy(QSizePolicy::Ignored);
        textPolicy.setVerticalPolicy(QSizePolicy::Fixed);
        label->setSizePolicy(textPolicy);
        label->setMaximumHeight(lineHeight);
    }
    else
    {
        label->setWordWrap(true);
    }
}

ElaText *createText(const QString &text, int pixelSize, QWidget *parent, bool clipSingleLine = false)
{
    ElaText *label = new ElaText(text, pixelSize, parent);
    configureText(label, pixelSize, clipSingleLine);
    return label;
}

bool confirmDangerousAction(QWidget *owner, const QString &body, const QString &confirmText)
{
    SafeElaContentDialog *dlg = new SafeElaContentDialog(owner);
    QWidget *central = new QWidget(dlg);
    QVBoxLayout *layout = new QVBoxLayout(central);
    ElaText *warnText = createText(body, 14, central);
    layout->addWidget(warnText);

    dlg->setCentralWidget(central);
    dlg->setLeftButtonText(confirmText);
    dlg->setMiddleButtonText(QObject::tr("返回仪表盘"));
    dlg->setRightButtonText(QObject::tr("取消"));

    bool confirmed = false;
    dlg->leftClicked = [&]()
    {
        confirmed = true;
        dlg->accept();
    };
    dlg->middleClicked = [&]()
    {
        confirmed = false;
        dlg->reject();
    };
    dlg->rightClicked = [&]()
    {
        confirmed = false;
        dlg->reject();
    };
    dlg->exec();
    dlg->leftClicked = nullptr;
    dlg->middleClicked = nullptr;
    dlg->rightClicked = nullptr;
    QTimer::singleShot(1000, dlg, &QObject::deleteLater);
    return confirmed;
}

} // namespace

void HomePage::SetupDashboardPage()
{
    QWidget *page = new QWidget(ui->stackedWidget);
    m_DashboardUi = new Ui::HomePageDashboardPage;
    m_DashboardUi->setupUi(page);

    m_DashboardTitle = m_DashboardUi->text_DashboardTitle;
    m_DashboardSourcePath = m_DashboardUi->text_DashboardSourcePath;
    m_DashboardRepoPath = m_DashboardUi->text_DashboardRepoPath;
    m_DashboardVersionCount = m_DashboardUi->text_DashboardVersionCount;
    m_DashboardFileCount = m_DashboardUi->text_DashboardFileCount;
    m_DashboardFileSize = m_DashboardUi->text_DashboardFileSize;
    m_DashboardCacheSize = m_DashboardUi->text_DashboardCacheSize;
    m_DashboardSourceState = m_DashboardUi->text_DashboardSourceState;

    m_DashboardUi->verticalLayout_DashboardSummary->setSpacing(6);
    configureText(m_DashboardTitle, 24, true);
    configureText(m_DashboardSourcePath, 13, true);
    configureText(m_DashboardRepoPath, 13, true);
    const QMargins summaryMargins = m_DashboardUi->verticalLayout_DashboardSummary->contentsMargins();
    const int summaryHeight = summaryMargins.top() + summaryMargins.bottom() +
                              m_DashboardUi->verticalLayout_DashboardSummary->spacing() * 2 +
                              m_DashboardTitle->minimumHeight() +
                              m_DashboardSourcePath->minimumHeight() +
                              m_DashboardRepoPath->minimumHeight();
    m_DashboardUi->area_DashboardSummary->setFixedHeight(summaryHeight);
    configureText(m_DashboardUi->text_DashboardVersionTitle, 12);
    configureText(m_DashboardUi->text_DashboardFileCountTitle, 12);
    configureText(m_DashboardUi->text_DashboardFileSizeTitle, 12);
    configureText(m_DashboardUi->text_DashboardCacheTitle, 12);
    configureText(m_DashboardUi->text_DashboardSourceStateTitle, 12);
    configureText(m_DashboardVersionCount, 22);
    configureText(m_DashboardFileCount, 22);
    configureText(m_DashboardFileSize, 22);
    configureText(m_DashboardCacheSize, 22);
    configureText(m_DashboardSourceState, 22);

    SetupDashboardRemoteSection();

    m_DashboardPageIndex = ui->stackedWidget->addWidget(page);

    connect(m_DashboardUi->pushButton_DashboardRefresh, &QPushButton::clicked, this, &HomePage::on_pushButton_DashboardRefresh_clicked);
    connect(m_DashboardUi->pushButton_DashboardRemoveTrack, &QPushButton::clicked, this, &HomePage::on_pushButton_DashboardRemoveTrack_clicked);
    connect(m_DashboardUi->pushButton_DashboardRebuild, &QPushButton::clicked, this, &HomePage::on_pushButton_DashboardRebuild_clicked);
}

void HomePage::ReleaseDashboardPageUi()
{
    delete m_DashboardUi;
    m_DashboardUi = nullptr;
}

void HomePage::SetupDashboardRemoteSection()
{
    if (!m_DashboardUi)
        return;

    m_RemoteDrawer = m_DashboardUi->drawer_DashboardRemote;
    m_RemoteUrlEdit = m_DashboardUi->lineEdit_DashboardRemoteUrl;
    m_ToggleSwitch_Remote = m_DashboardUi->toggleSwitch_DashboardRemote;

    configureText(m_DashboardUi->text_DashboardRemoteIcon, 15);
    m_DashboardUi->text_DashboardRemoteIcon->setElaIcon(ElaIconType::MessageArrowDown);
    m_DashboardUi->text_DashboardRemoteIcon->setFixedSize(25, 25);
    configureText(m_DashboardUi->text_DashboardRemoteTitle, 15);
    configureText(m_DashboardUi->text_DashboardRemoteHint, 13);

    m_RemoteDrawer->setDrawerHeader(m_DashboardUi->widget_DashboardRemoteHeader);
    m_RemoteDrawer->addDrawer(m_DashboardUi->widget_DashboardRemoteContent);

    connect(m_ToggleSwitch_Remote, &ElaToggleSwitch::toggled, this,
            [=](bool checked)
            {
                if (!m_RemoteDrawer)
                    return;
                if (checked)
                    m_RemoteDrawer->expand();
                else
                    m_RemoteDrawer->collapse();
            });
    connect(m_RemoteDrawer, &ElaDrawerArea::expandStateChanged, this,
            [=](bool isExpand)
            {
                if (m_ToggleSwitch_Remote && m_ToggleSwitch_Remote->getIsToggled() != isExpand)
                    m_ToggleSwitch_Remote->setIsToggled(isExpand);
            });
    // Review note: 开关开启时也承担“展开配置抽屉”的入口；只有关闭已配置的 origin 时才移除 remote。
    connect(m_ToggleSwitch_Remote, &ElaToggleSwitch::toggled, this, &HomePage::on_ToggleSwitch_Remote_toggled);

    ElaIconButton *btnOpen = new ElaIconButton(ElaIconType::Link, 16, m_DashboardUi->widget_DashboardRemoteContent);
    btnOpen->setFixedSize(32, 32);
    btnOpen->setToolTip("在浏览器打开云端地址");
    ElaIconButton *btnPull = new ElaIconButton(ElaIconType::Download, 16, m_DashboardUi->widget_DashboardRemoteContent);
    btnPull->setFixedSize(32, 32);
    btnPull->setToolTip("从云端拉取最新内容");
    ElaIconButton *btnPush = new ElaIconButton(ElaIconType::Upload, 16, m_DashboardUi->widget_DashboardRemoteContent);
    btnPush->setFixedSize(32, 32);
    btnPush->setToolTip("上传本地更改到云端");

    m_DashboardUi->horizontalLayout_DashboardRemoteActions->addWidget(btnOpen);
    m_DashboardUi->horizontalLayout_DashboardRemoteActions->addWidget(btnPull);
    m_DashboardUi->horizontalLayout_DashboardRemoteActions->addWidget(btnPush);

    connect(m_RemoteUrlEdit, &QLineEdit::editingFinished, this, [=]()
            { ApplyRemoteUrlFromInput(true); });
    connect(btnOpen, &ElaIconButton::clicked, this, [=]()
            {
                QString repoUrl = m_RemoteUrlEdit ? m_RemoteUrlEdit->text().trimmed() : QString();
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
    connect(btnPull, &ElaIconButton::clicked, this, [=]()
            {
                const QString repoPath = repoPathFor(m_NowFilePathWithCode);
                if (m_NowFilePathWithCode.isEmpty() || !QDir(repoPath).exists())
                {
                    ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                         "同步失败",
                                         "本地备份仓库不存在",
                                         3000,
                                         parentWidget());
                    return;
                }

                bool ok = false;
                const QString output = runGit(repoPath, QStringList() << "pull" << "origin" << "master", &ok);
                if (ok)
                {
                    ElaMessageBar::success(ElaMessageBarType::BottomRight,
                                           "已同步",
                                           "已从云端获取最新内容",
                                           2000,
                                           parentWidget());
                    RefreshBackupDashboard();
                }
                else
                {
                    ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                         "同步失败",
                                         output.isEmpty() ? "请检查网络和云端地址是否正确" : output,
                                         3000,
                                         parentWidget());
                } });
    connect(btnPush, &ElaIconButton::clicked, this, [=]()
            {
                const QString repoPath = repoPathFor(m_NowFilePathWithCode);
                if (m_NowFilePathWithCode.isEmpty() || !QDir(repoPath).exists())
                {
                    ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                         "上传失败",
                                         "本地备份仓库不存在",
                                         3000,
                                         parentWidget());
                    return;
                }

                bool ok = false;
                const QString output = runGit(repoPath, QStringList() << "push" << "origin" << "master", &ok);
                if (ok)
                {
                    ElaMessageBar::success(ElaMessageBarType::BottomRight,
                                           "上传完成",
                                           "本地更改已上传到云端",
                                           2000,
                                           parentWidget());
                    RefreshBackupDashboard();
                }
                else
                {
                    ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                         "上传失败",
                                         output.isEmpty() ? "请检查网络和上传权限" : output,
                                         3000,
                                         parentWidget());
                } });
}

void HomePage::openBackupDashboard(QString FilePathWithCode)
{
    m_NowFilePathWithCode = FilePathWithCode;
    const QString displayName = QFileInfo(decodedPath(FilePathWithCode)).baseName();
    ui->widget_BreadcrumbBar->setBreadcrumbList(QStringList() << "备份中文件" << displayName + " 仪表盘");
    ui->stackedWidget->setCurrentIndex(m_DashboardPageIndex);
    RefreshBackupDashboard();
}

void HomePage::RefreshBackupDashboard()
{
    if (m_NowFilePathWithCode.isEmpty())
        return;

    const QString sourcePath = decodedPath(m_NowFilePathWithCode);
    const QString repoPath = repoPathFor(m_NowFilePathWithCode);
    const QString snapshotPath = snapshotPathFor(m_NowFilePathWithCode);

    // Review note: 备份缓存只统计 Git 对象库，不包含工作区快照和 .git 固定模板文件；
    // 这样重建后缓存大小会回落到接近单版本备份的体量。
    const QString gitObjectPath = repoPath + "/.git/objects";
    const QFileInfo sourceInfo(sourcePath);
    const QFileInfo repoInfo(repoPath);
    const auto snapshotStats = collectPathStats(snapshotPath);
    const auto cacheStats = collectPathStats(gitObjectPath);

    if (m_DashboardTitle)
    {
        m_DashboardTitle->setText(QFileInfo(sourcePath).fileName());
        m_DashboardTitle->setToolTip(QFileInfo(sourcePath).fileName());
    }
    if (m_DashboardSourcePath)
    {
        m_DashboardSourcePath->setText("源路径：" + sourcePath);
        m_DashboardSourcePath->setToolTip(sourcePath);
    }
    if (m_DashboardRepoPath)
    {
        m_DashboardRepoPath->setText("仓库路径：" + repoPath);
        m_DashboardRepoPath->setToolTip(repoPath);
    }
    if (m_DashboardFileCount)
        m_DashboardFileCount->setText(QString::number(snapshotStats.first));
    if (m_DashboardFileSize)
        m_DashboardFileSize->setText(formatBytes(snapshotStats.second));
    if (m_DashboardCacheSize)
        m_DashboardCacheSize->setText(formatBytes(cacheStats.second));
    if (m_DashboardSourceState)
    {
        if (!sourceInfo.exists())
            m_DashboardSourceState->setText("缺失");
        else if (sourceInfo.isDir())
            m_DashboardSourceState->setText("文件夹");
        else
            m_DashboardSourceState->setText("文件");
    }

    if (!repoInfo.exists())
    {
        if (m_DashboardVersionCount)
            m_DashboardVersionCount->setText("0");
        ApplyRemoteControlsState(false, QString(), true);
        return;
    }

    bool ok = false;
    const QString versionCount = runGit(repoPath, QStringList() << "rev-list" << "--count" << "HEAD", &ok);
    if (m_DashboardVersionCount)
        m_DashboardVersionCount->setText(ok ? versionCount : "0");

    bool remoteOk = false;
    const QString remoteUrl = runGit(repoPath, QStringList() << "remote" << "get-url" << "origin", &remoteOk);
    const bool hasOrigin = remoteOk && !remoteUrl.isEmpty();
    ApplyRemoteControlsState(hasOrigin, remoteUrl, false);
}

void HomePage::ApplyRemoteControlsState(bool hasOrigin, const QString &remoteUrl, bool collapseDrawer)
{
    if (m_RemoteUrlEdit)
        m_RemoteUrlEdit->setText(hasOrigin ? remoteUrl : QString());

    if (m_ToggleSwitch_Remote)
    {
        // Review note: 刷新 UI 时屏蔽 toggled，避免“读状态”误触发 git remote add/remove。
        QSignalBlocker blocker(m_ToggleSwitch_Remote);
        m_ToggleSwitch_Remote->setIsToggled(hasOrigin);
    }

    if (collapseDrawer && m_RemoteDrawer)
        m_RemoteDrawer->collapse();
}

/*写入云端仓库地址*/
void HomePage::ApplyRemoteUrlFromInput(bool showSuccessMessage)
{
    const QString url = m_RemoteUrlEdit ? m_RemoteUrlEdit->text().trimmed() : QString();
    const QString repoPath = repoPathFor(m_NowFilePathWithCode);

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

    bool hasOrigin = false;
    runGit(repoPath, QStringList() << "remote" << "get-url" << "origin", &hasOrigin);

    bool ok = false;
    const QString output = runGit(repoPath,
                                  hasOrigin
                                      ? (QStringList() << "remote" << "set-url" << "origin" << url)
                                      : (QStringList() << "remote" << "add" << "origin" << url),
                                  &ok);
    if (!ok)
    {
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "云端地址保存失败",
                             output.isEmpty() ? "请检查云端地址格式" : output,
                             3000,
                             parentWidget());
        return;
    }

    ApplyRemoteControlsState(true, url, false);
    if (m_RemoteDrawer)
        m_RemoteDrawer->expand();
    RefreshBackupDashboard();

    if (showSuccessMessage)
    {
        ElaMessageBar::success(ElaMessageBarType::BottomRight,
                               "云端地址已保存",
                               url,
                               2000,
                               parentWidget());
    }
}

/*远程同步开关*/
void HomePage::on_ToggleSwitch_Remote_toggled(bool checked)
{
    if (m_NowFilePathWithCode.isEmpty())
        return;

    const QString repoPath = repoPathFor(m_NowFilePathWithCode);
    if (!QDir(repoPath).exists())
    {
        ApplyRemoteControlsState(false, QString(), true);
        return;
    }

    if (!checked)
    {
        bool hasOrigin = false;
        runGit(repoPath, QStringList() << "remote" << "get-url" << "origin", &hasOrigin);
        if (!hasOrigin)
            return;

        bool ok = false;
        const QString output = runGit(repoPath, QStringList() << "remote" << "remove" << "origin", &ok);
        if (!ok)
        {
            ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                 "关闭云端同步失败",
                                 output.isEmpty() ? "无法移除 origin" : output,
                                 3000,
                                 parentWidget());
            RefreshBackupDashboard();
            return;
        }

        ApplyRemoteControlsState(false, QString(), true);
        ElaMessageBar::success(ElaMessageBarType::BottomRight,
                               "已关闭云端同步",
                               "",
                               2000,
                               parentWidget());
        RefreshBackupDashboard();
        return;
    }

    bool hasOrigin = false;
    const QString originUrl = runGit(repoPath, QStringList() << "remote" << "get-url" << "origin", &hasOrigin);
    if (!hasOrigin)
    {
        if (m_RemoteDrawer)
            m_RemoteDrawer->expand();
        ElaMessageBar::warning(ElaMessageBarType::BottomRight,
                               "请输入云端地址",
                               "填写后会自动保存",
                               2500,
                               parentWidget());
        return;
    }

    ApplyRemoteControlsState(true, originUrl, false);
    ElaMessageBar::success(ElaMessageBarType::BottomRight,
                           "云端同步已开启",
                           "",
                           2000,
                           parentWidget());
    RefreshBackupDashboard();
}

void HomePage::on_pushButton_DashboardRefresh_clicked()
{
    RefreshBackupDashboard();
}

void HomePage::on_pushButton_DashboardRemoveTrack_clicked()
{
    if (m_NowFilePathWithCode.isEmpty())
        return;

    const QString sourcePath = decodedPath(m_NowFilePathWithCode);
    const QString repoPath = repoPathFor(m_NowFilePathWithCode);
    if (!confirmDangerousAction(this->window(),
                                tr("确定要删除这个备份吗？\n\n"
                                   "此操作会删除本地备份仓库和所有历史版本记录，但不会删除源文件。\n\n"
                                   "此操作不可撤销！"),
                                tr("确认删除")))
    {
        return;
    }

    QDir dir(repoPath);
    qInfo() << "移除追踪：" << dir;
    if (!dir.removeRecursively())
    {
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "删除备份失败",
                             "删除备份目录失败，请检查权限或文件占用",
                             3000,
                             parentWidget());
        return;
    }

    ElaMessageBar::success(ElaMessageBarType::BottomRight,
                           "已删除备份",
                           QFileInfo(sourcePath).fileName(),
                           2000,
                           parentWidget());
    m_NowFilePathWithCode.clear();
    LoadBackupFileList();
    ui->widget_BreadcrumbBar->setBreadcrumbList(QStringList() << "备份中文件");
    ui->stackedWidget->setCurrentIndex(0);
}

void HomePage::on_pushButton_DashboardRebuild_clicked()
{
    if (m_NowFilePathWithCode.isEmpty())
        return;

    const QString repoPath = repoPathFor(m_NowFilePathWithCode);
    if (!QDir(repoPath).exists())
    {
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "重建失败",
                             "本地备份仓库不存在",
                             3000,
                             parentWidget());
        return;
    }

    if (!confirmDangerousAction(this->window(),
                                tr("确定要重建此仓库吗？\n\n"
                                   "此操作将：\n"
                                   "  - 删除所有历史版本记录\n"
                                   "  - 仅保留当前文件的单一快照\n"
                                   "  - 如已配置云端地址，将强制覆盖云端仓库\n\n"
                                   "此操作不可撤销！"),
                                tr("确认重建")))
    {
        return;
    }

    // Review note: 重建会删除 .git，必须先缓存 origin，后面才能恢复云端配置并 force push。
    bool ok = false;
    QString savedRemoteUrl = runGit(repoPath, QStringList() << "remote" << "get-url" << "origin", &ok);
    if (!ok)
        savedRemoteUrl.clear();

    QDir gitDir(repoPath + "/.git");
    if (gitDir.exists() && !gitDir.removeRecursively())
    {
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "重建失败",
                             "无法删除旧的 .git 目录，请检查文件权限",
                             3000,
                             parentWidget());
        return;
    }

    runGit(repoPath, QStringList() << "init", &ok);
    if (!ok)
    {
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "重建失败",
                             "git init 执行失败",
                             3000,
                             parentWidget());
        return;
    }

    runGit(repoPath, QStringList() << "config" << "user.name" << "ZcVersionBox");
    runGit(repoPath, QStringList() << "config" << "user.email" << "backup@zcversionbox.local");

    runGit(repoPath, QStringList() << "add" << ".", &ok);
    if (!ok)
    {
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "重建失败",
                             "git add 执行失败",
                             3000,
                             parentWidget());
        return;
    }

    runGit(repoPath, QStringList() << "commit" << "-m" << "Initial backup", &ok);
    if (!ok)
    {
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "重建失败",
                             "git commit 执行失败",
                             3000,
                             parentWidget());
        return;
    }

    if (!savedRemoteUrl.isEmpty())
    {
        runGit(repoPath, QStringList() << "remote" << "add" << "origin" << savedRemoteUrl, &ok);
        if (!ok)
        {
            ElaMessageBar::warning(ElaMessageBarType::BottomRight,
                                   "重建完成（部分）",
                                   "仓库已重建，但云端地址恢复失败，请手动重新配置",
                                   4000,
                                   parentWidget());
            RefreshBackupDashboard();
            return;
        }

        runGit(repoPath, QStringList() << "push" << "--force" << "origin" << "master", &ok);
        if (!ok)
        {
            ElaMessageBar::warning(ElaMessageBarType::BottomRight,
                                   "重建完成（部分）",
                                   "仓库已重建，但强制推送失败，请检查网络和云端地址",
                                   4000,
                                   parentWidget());
            RefreshBackupDashboard();
            return;
        }
    }

    ElaMessageBar::success(ElaMessageBarType::BottomRight,
                           "重建完成",
                           savedRemoteUrl.isEmpty()
                               ? "仓库已重建，历史已清除"
                               : "仓库已重建，历史已清除，云端已同步",
                           3000,
                           parentWidget());
    RefreshBackupDashboard();
}
