#include "homepagechild_backupfile.h"
#include "ui_homepagechild_backupfile.h"

#include "../GlobalConstants.h"

#include <QFileInfo>
#include <QDesktopServices>

HomePageChild_BackupFile::HomePageChild_BackupFile(QString FilePathWithCode, QString BackupFileName, QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::HomePageChild_BackupFile)
{
    ui->setupUi(this);
    m_BackupFileName = BackupFileName;
    m_FilePathWithCode = FilePathWithCode;
    ui->label->setText(QFileInfo(m_BackupFileName).fileName());
}

HomePageChild_BackupFile::~HomePageChild_BackupFile()
{
    delete ui;
}

/*打开备份文件*/
void HomePageChild_BackupFile::on_pushButton_OpenFile_clicked()
{
    QString backupFilePath = BackupPath + "/" + m_FilePathWithCode + "/" + m_BackupFileName;
    qInfo()<<"打开备份文件："<< backupFilePath;
    QDesktopServices::openUrl(QUrl::fromLocalFile(backupFilePath));
}
/*删除备份文件*/
void HomePageChild_BackupFile::on_pushButton_RemoveBackup_clicked()
{
    QFile::remove(BackupPath + "/" + m_FilePathWithCode + "/" + m_BackupFileName);
}


