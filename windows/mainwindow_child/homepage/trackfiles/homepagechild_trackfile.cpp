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
    QDesktopServices::openUrl(QUrl::fromLocalFile(QUrl::fromPercentEncoding(m_FilePathWithCode.toUtf8())));
}

/*移除追踪*/
void HomePageChild_TrackFile::on_pushButton_RemoveTrack_clicked()
{
    //删除文件夹
    QString docPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/ZcVersionBox/Backup";
    QDir dir(BackupPath + "/" + m_FilePathWithCode);
    qInfo() << "移除追踪：" << dir;
    dir.removeRecursively();
    this->deleteLater();
}

/*同步文件到仓库*/
void HomePageChild_TrackFile::BackupFile()
{
    QString FilePathWithoutCode = QUrl::fromPercentEncoding(m_FilePathWithCode.toUtf8());
    QString backupDirPath = BackupPath + "/" + m_FilePathWithCode + "/" + QFileInfo(FilePathWithoutCode).fileName();
    const QFileInfo sourceInfo(FilePathWithoutCode);
    if (QFile::exists(backupDirPath))
        QFile::remove(backupDirPath);

    if (sourceInfo.isFile())
    {
        QFile::copy(FilePathWithoutCode, backupDirPath);
    }
    else
    {
        FileUtils::copyDirectory(FilePathWithoutCode, backupDirPath);
    }

    /*Git自动Commit*/
    QProcess git;
    git.setWorkingDirectory(BackupPath + "/" + m_FilePathWithCode);
    //添加变更
    git.start("git", QStringList() << "add" << ".");
    git.waitForFinished();
    //检查是否真的有改动（避免空提交）
    git.start("git", QStringList() << "diff" << "--cached" << "--quiet");
    git.waitForFinished();
    if (git.exitCode() != 0)
    {
        // 获取当前时间
        QString timeStr = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
        git.start("git", QStringList() << "commit" << "-m" << QString("Auto backup - %1").arg(timeStr));
        git.waitForFinished();
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
