#include "homepagechild_trackfile.h"
#include "ui_homepagechild_trackfile.h"

#include "../../../../GlobalConstants.h"
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
#include <QStandardPaths>
#include <QTimer>

#include "ElaMessageBar.h"
#include "ElaToolTip.h"

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
    timer->setInterval(1500); // 1.5秒扫描一次
    QString rootPath = QUrl::fromPercentEncoding(m_FilePathWithCode.toUtf8());
    rootPath = QDir::cleanPath(rootPath);
    const bool isTrackedFile = QFileInfo(rootPath).isFile();
    QMap<QString, QString> lastState; // path -> fingerprint
    //计算文件指纹
    auto calcFingerprint = [](const QFileInfo &info) -> QString
    {
        return QString::number(info.size()) + "|" +
               QString::number(info.lastModified().toMSecsSinceEpoch());
    };
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
    bool *busy = new bool(false); //简单做法；也可用成员变量更干净
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
    QString docPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/ZcVersionBox/Backup";
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
    git.setWorkingDirectory(BackupPath + "/" + m_FilePathWithCode);
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
        // 获取当前时间
        QString timeStr = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
        git.start("git", QStringList() << "commit" << "-m" << QString("Auto backup - %1").arg(timeStr));
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
