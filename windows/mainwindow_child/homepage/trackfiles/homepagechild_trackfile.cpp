#include "homepagechild_trackfile.h"
#include "ui_homepagechild_trackfile.h"

#include "../../../../GlobalConstants.h"
#include "../../../../utils/aicommitmessagehelper.h"
#include "../../../../utils/fileutils.h"
#include "../homepage.h"

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include "ElaMessageBar.h"
#include "ElaToolTip.h"

namespace
{

QString buildAutoCommitMessageWithAi(const QString &repoPath)
{
    // Auto AI commit message is optional and controlled by settings.
    if (!AiCommitMessageHelper::isAutoCommitEnabled())
        return {};

    QProcess diffProcess;
    diffProcess.setWorkingDirectory(repoPath);
    diffProcess.start("git", QStringList() << "diff" << "--cached" << "--unified=0");
    if (!diffProcess.waitForStarted())
    {
        qInfo() << "自动 AI 提交信息获取 diff 失败：无法启动 git";
        return {};
    }
    diffProcess.waitForFinished();
    if (diffProcess.exitCode() != 0)
    {
        qInfo() << "自动 AI 提交信息获取 diff 失败：" << QString::fromUtf8(diffProcess.readAllStandardError()).trimmed();
        return {};
    }

    const QString diffText = QString::fromUtf8(diffProcess.readAllStandardOutput()).trimmed();
    if (diffText.isEmpty())
    {
        qInfo() << "自动 AI 提交信息获取到空 diff，回退默认提交信息";
        return {};
    }

    const QString prompt = AiCommitMessageHelper::buildPromptFromDiff(diffText);
    qInfo() << "自动 AI 提交信息请求，repo=" << repoPath
            << "diff长度=" << diffText.length()
            << "prompt长度=" << prompt.length();

    QString aiError;
    const QString generated = AiCommitMessageHelper::generateCommitMessageSync(diffText, 15000, &aiError);

    if (generated.isEmpty())
    {
        if (aiError.isEmpty())
            qInfo() << "自动 AI 提交信息失败：未知错误";
        else
            qInfo() << "自动 AI 提交信息失败：" << aiError;
        return {};
    }

    qInfo() << "自动 AI 提交信息成功，message长度=" << generated.length();
    return generated;
}

} // namespace

HomePageChild_TrackFile::HomePageChild_TrackFile(QString FilePathWithCode, QWidget *parent)
    : QWidget(parent), ui(new Ui::HomePageChild_TrackFile)
{
    /*初始化*/
    ui->setupUi(this);
    /*读取参数*/
    m_FilePathWithCode = FilePathWithCode;
    /*显示设置*/
    ui->label->setText(QFileInfo(QUrl::fromPercentEncoding(m_FilePathWithCode.toUtf8())).fileName());
    ElaToolTip *NameToolTip = new ElaToolTip(ui->label);
    NameToolTip->setToolTip(QUrl::fromPercentEncoding(m_FilePathWithCode.toUtf8()));

    /*开始备份*/
    QTimer *timer = new QTimer(this);
    timer->setInterval(1500); //1.5秒扫描一次
    QString rootPath = QUrl::fromPercentEncoding(m_FilePathWithCode.toUtf8());
    rootPath = QDir::cleanPath(rootPath);
    const bool isTrackedFile = QFileInfo(rootPath).isFile();
    QMap<QString, QString> lastState; // path -> fingerprint
    //递归扫描
    auto scanState = [rootPath, isTrackedFile](QMap<QString, QString> &state)
    {
        if (isTrackedFile)
        {
            QFileInfo info(rootPath);
            if (info.exists() && info.isFile())
            {
                state[rootPath] = QString::number(info.size()) + "|" +
                                  QString::number(info.lastModified().toMSecsSinceEpoch());
            }
            return;
        }

        QDirIterator it(rootPath,
                        QDir::Files | QDir::Readable | QDir::NoSymLinks,
                        QDirIterator::Subdirectories);
        while (it.hasNext())
        {
            it.next();
            QFileInfo info = it.fileInfo();

            QString filePath = QDir::cleanPath(it.filePath());
            const QString norm = QDir::fromNativeSeparators(filePath);

            if (norm.contains("/.git/") || norm.endsWith("/.git") ||
                norm.contains("/build/") || norm.endsWith("/build"))
            {
                continue;
            }

            const QString fingerprint =
                QString::number(info.size()) + "|" +
                QString::number(info.lastModified().toMSecsSinceEpoch());

            state[filePath] = fingerprint;
        }
    };

    //初始化
    scanState(lastState);

    //防止备份重入（备份过程中不重复触发）
    bool *busy = new bool(false);
    connect(timer, &QTimer::timeout, this, [=]() mutable
            {
                if (*busy) return;
                QMap<QString, QString> newState;
                scanState(newState);

                if (newState != lastState)
                {
                    *busy = true;
                    qInfo() << "检测到文件系统变化";
                    BackupFile();
                    lastState = std::move(newState);
                    *busy = false;
                } });
    timer->start();
}

HomePageChild_TrackFile::~HomePageChild_TrackFile()
{
    delete ui;
}

/*打开文件*/
void HomePageChild_TrackFile::on_pushButton_OpenFile_clicked()
{
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(QUrl::fromPercentEncoding(m_FilePathWithCode.toUtf8()))))
    {
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "打开失败",
                             "无法打开目标路径",
                             3000,
                             this);
    }
}

/*移除追踪*/
void HomePageChild_TrackFile::on_pushButton_RemoveTrack_clicked()
{
    //删除文件夹
    QDir dir(BackupPath + "/" + m_FilePathWithCode);
    qInfo() << "移除追踪：" << dir;
    if (!dir.removeRecursively())
    {
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "移除追踪失败",
                             "删除备份目录失败，请检查权限或文件占用",
                             3000,
                             this);
        return;
    }
    this->deleteLater();
}

/*同步文件到仓库*/
void HomePageChild_TrackFile::BackupFile()
{
    QString FilePathWithoutCode = QUrl::fromPercentEncoding(m_FilePathWithCode.toUtf8());
    QString backupDirPath = BackupPath + "/" + m_FilePathWithCode + "/" + QFileInfo(FilePathWithoutCode).fileName();
    const QFileInfo sourceInfo(FilePathWithoutCode);
    if (!sourceInfo.exists())
    {
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "自动备份失败",
                             "源文件或文件夹不存在",
                             3000,
                             this);
        return;
    }

    /*备份*/
    const QFileInfo backupInfo(backupDirPath);
    if (backupInfo.exists())
    {
        if (backupInfo.isDir())
        {
            if (!QDir(backupDirPath).removeRecursively())
            {
                ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                     "自动备份失败",
                                     "清理旧备份目录失败",
                                     3000,
                                     this);
                return;
            }
        }
        else
        {
            if (!QFile::remove(backupDirPath))
            {
                ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                     "自动备份失败",
                                     "清理旧备份文件失败",
                                     3000,
                                     this);
                return;
            }
        }
    }

    if (sourceInfo.isFile())
    {
        if (!QFile::copy(FilePathWithoutCode, backupDirPath))
        {
            ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                 "自动备份失败",
                                 "复制文件失败，请检查权限",
                                 3000,
                                 this);
            return;
        }
    }
    else
    {
        if (!FileUtils::copyDirectory(FilePathWithoutCode, backupDirPath))
        {
            ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                 "自动备份失败",
                                 "复制文件夹失败，请检查权限",
                                 3000,
                                 this);
            return;
        }
    }

    /*Git自动Commit*/
    QProcess git;
    const QString repoPath = BackupPath + "/" + m_FilePathWithCode;
    git.setWorkingDirectory(repoPath);
    qInfo() << "自动备份开始，repo=" << repoPath;
    //添加变更
    git.start("git", QStringList() << "add" << ".");
    if (!git.waitForStarted())
    {
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "自动备份失败",
                             "无法启动 git，请确认 git 已安装",
                             3000,
                             this);
        return;
    }
    git.waitForFinished();
    if (git.exitCode() != 0)
    {
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "自动备份失败",
                             QString::fromUtf8(git.readAllStandardError()).trimmed(),
                             3000,
                             this);
        return;
    }
    //检查是否真的有改动（避免空提交）
    git.start("git", QStringList() << "diff" << "--cached" << "--quiet");
    if (!git.waitForStarted())
    {
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "自动备份失败",
                             "无法启动 git，请确认 git 已安装",
                             3000,
                             this);
        return;
    }
    git.waitForFinished();
    if (git.exitCode() != 0)
    {
        QString timeStr = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
        QString commitMessage = buildAutoCommitMessageWithAi(repoPath);
        if (commitMessage.isEmpty())
            commitMessage = QString("Auto backup - %1").arg(timeStr);

        qInfo() << "自动备份提交信息：" << commitMessage.left(120);

        git.start("git", QStringList() << "commit" << "-m" << commitMessage);
        if (!git.waitForStarted())
        {
            ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                 "自动备份失败",
                                 "无法启动 git，请确认 git 已安装",
                                 3000,
                                 this);
            return;
        }
        git.waitForFinished();
        if (git.exitCode() != 0)
        {
            ElaMessageBar::error(ElaMessageBarType::BottomRight,
                                 "自动备份失败",
                                 QString::fromUtf8(git.readAllStandardError()).trimmed(),
                                 3000,
                                 this);
        }
    }
}

/*查看备份*/
void HomePageChild_TrackFile::on_pushButton_Backup_clicked()
{
    qInfo() << "打开备份：" << m_FilePathWithCode;
    //传递到父窗口
    HomePage *mw = qobject_cast<HomePage *>(this->parent()->parent()->parent());
    mw->openBackup(m_FilePathWithCode);
}
