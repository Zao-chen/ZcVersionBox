#include "homepagechild_trackfile.h"
#include "ui_homepagechild_trackfile.h"

#include "../GlobalConstants.h"
#include "homepage.h"

#include <QFileInfo>
#include <QDesktopServices>
#include <QStandardPaths>
#include <QDir>
#include <QRegularExpression>
#include <QCryptographicHash>
#include <QFileSystemWatcher>

#include "ElaToolTip.h"

HomePageChild_TrackFile::HomePageChild_TrackFile(QString FilePath, QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::HomePageChild_TrackFile)
{
    /*初始化*/
    ui->setupUi(this);
    /*读取参数*/
    m_FilePath = FilePath;
    QString FilePathWithoutCode = QUrl::fromPercentEncoding(m_FilePath.toUtf8());
    /*显示设置*/
    ui->label->setText(QFileInfo(FilePathWithoutCode).fileName());
    ElaToolTip* NameToolTip = new ElaToolTip(ui->label);
    NameToolTip->setToolTip(FilePathWithoutCode);
    /*开始备份*/
    //监控文件
    QFileSystemWatcher *watcher = new QFileSystemWatcher(this);
    watcher->addPath(FilePathWithoutCode);
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
    //打开文件
    QDesktopServices::openUrl(QUrl::fromLocalFile(QUrl::fromPercentEncoding(m_FilePath.toUtf8())));
}

/*移除追踪*/
void HomePageChild_TrackFile::on_pushButton_RemoveTrack_clicked()
{
    //删除文件夹
    QString docPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)+"/ZcVersionBox/Backup";
    QDir dir(BackupPath+"/"+m_FilePath);
    qInfo()<<"移除追踪："<<dir;
    dir.removeRecursively();
    this->deleteLater();
}

/*备份文件*/
void HomePageChild_TrackFile::BackupFile()
{
    QString FilePathWithoutCode = QUrl::fromPercentEncoding(m_FilePath.toUtf8());
    /*文件备份*/
    /*检查和最新的文件是否相同*/
    //从文件名判断targetDir中的所有文件哪个最新
    QDir BackupFileDir(BackupPath + "/" + m_FilePath);
    QFileInfoList fileList = BackupFileDir.entryInfoList(QDir::Files | QDir::NoSymLinks);
    QFileInfo newestFile;
    QDateTime newestTime;
    //匹配文件名中的时间戳部分
    QRegularExpression re(R"((\d{8}_\d{6})\..+$)");
    //搜索最新文件
    for (const QFileInfo &file : std::as_const(fileList))
    {
        QRegularExpressionMatch match = re.match(file.fileName());
        if (match.hasMatch())
        {
            QString timestampStr = match.captured(1); // yyyyMMdd_HHmmss
            QDateTime fileTime = QDateTime::fromString(timestampStr, "yyyyMMdd_HHmmss");
            if (fileTime.isValid() && (!newestTime.isValid() || fileTime > newestTime))
            {
                newestTime = fileTime;
                newestFile = file;
            }
        }
    }
    /*对比哈希值*/
    if (newestFile.exists())
    {
        QFile currentFile(FilePathWithoutCode);
        QFile backupFile(newestFile.filePath());
        if (currentFile.open(QIODevice::ReadOnly) && backupFile.open(QIODevice::ReadOnly))
        {
            QByteArray currentHash = QCryptographicHash::hash(currentFile.readAll(), QCryptographicHash::Sha256);
            qInfo()<<"当前文件哈希值："<<currentHash.toHex();
            QByteArray backupHash = QCryptographicHash::hash(backupFile.readAll(), QCryptographicHash::Sha256);
            currentFile.close();
            backupFile.close();
            //如果相同就不备份
            if (currentHash != backupHash)
            {
                qInfo()<<"文件不同，正在备份";
                /*创建和复制文件*/
                QString targetFilePath =
                    BackupFileDir.filePath(
                        QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")
                        + "."
                        + QFileInfo(FilePathWithoutCode).suffix()
                    );
                QFile::copy(FilePathWithoutCode, targetFilePath);
            }
            else
            {
                qInfo()<<"文件相同，无需备份";
            }
        }
    }
    else
    {
        qInfo()<<"文件无备份，正在备份";
        /*创建和复制文件*/
        QString targetFilePath =
            BackupFileDir.filePath(
                QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")
                + "."
                + QFileInfo(FilePathWithoutCode).suffix()
                );
        QFile::copy(FilePathWithoutCode, targetFilePath);
    }
}
/*查看备份*/
void HomePageChild_TrackFile::on_pushButton_Backup_clicked()
{
    qInfo() << "打开备份：" << m_FilePath;
    //传递到父窗口
    HomePage *mw = qobject_cast<HomePage *>(this->parent()->parent()->parent());
    mw->openBackup(m_FilePath);
}


