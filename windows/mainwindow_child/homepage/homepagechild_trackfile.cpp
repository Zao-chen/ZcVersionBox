#include "homepagechild_trackfile.h"
#include "ui_homepagechild_trackfile.h"

#include "../../../GlobalConstants.h"
#include "homepage.h"

#include <QFileInfo>
#include <QDesktopServices>
#include <QStandardPaths>
#include <QDir>
#include <QRegularExpression>
#include <QCryptographicHash>
#include <QFileSystemWatcher>
#include <QProcess>

#include "ElaToolTip.h"

HomePageChild_TrackFile::HomePageChild_TrackFile(QString FilePathWithCode, QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::HomePageChild_TrackFile)
{
    /*初始化*/
    ui->setupUi(this);
    /*读取参数*/
    m_FilePathWithCode = FilePathWithCode;
    /*显示设置*/
    ui->label->setText(QFileInfo(QUrl::fromPercentEncoding(m_FilePathWithCode.toUtf8())).fileName());
    ElaToolTip* NameToolTip = new ElaToolTip(ui->label);
    NameToolTip->setToolTip(QUrl::fromPercentEncoding(m_FilePathWithCode.toUtf8()));
    /*开始备份*/
    //监控文件
    QFileSystemWatcher *watcher = new QFileSystemWatcher(this);
    watcher->addPath(QUrl::fromPercentEncoding(m_FilePathWithCode.toUtf8()));
    connect(watcher, &QFileSystemWatcher::fileChanged,
            this, [=](const QString &path)
            {
                qInfo()<<"文件变化："<<path;
                BackupFile();
            });
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
    QString docPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)+"/ZcVersionBox/Backup";
    QDir dir(BackupPath + "/" + m_FilePathWithCode);
    qInfo()<<"移除追踪："<<dir;
    dir.removeRecursively();
    this->deleteLater();
}

/*同步文件到仓库*/
void HomePageChild_TrackFile::BackupFile()
{
    QString FilePathWithoutCode = QUrl::fromPercentEncoding(m_FilePathWithCode.toUtf8());
    QString backupDirPath = BackupPath + "/" + m_FilePathWithCode + "/" + QFileInfo(FilePathWithoutCode).fileName();
    if (QFile::exists(backupDirPath)) QFile::remove(backupDirPath);
    QFile::copy(FilePathWithoutCode, backupDirPath);
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


