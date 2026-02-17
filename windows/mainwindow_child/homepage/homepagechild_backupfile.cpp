#include "homepagechild_backupfile.h"
#include "ui_homepagechild_backupfile.h"

#include "../../../GlobalConstants.h"
#include "../../../utils/fileutils.h"

#include "ElaMessageBar.h"

#include <QFileInfo>
#include <QDesktopServices>
#include <QProcess>
#include <QFile>

HomePageChild_BackupFile::HomePageChild_BackupFile(QString FilePathWithCode, QString commitInfo, QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::HomePageChild_BackupFile)
{
    ui->setupUi(this);
    m_commitInfo = commitInfo;
    m_FilePathWithCode = FilePathWithCode;
    ui->label->setText(QFileInfo(m_commitInfo).fileName());
}

HomePageChild_BackupFile::~HomePageChild_BackupFile()
{
    delete ui;
}

/*预览备份文件*/
void HomePageChild_BackupFile::on_pushButton_OpenFile_clicked()
{
    /*切换到指定的commit*/
    QString sourceRepoPath = BackupPath + "/" + m_FilePathWithCode;
    QProcess git;
    git.setWorkingDirectory(sourceRepoPath);
    QString shortId = m_commitInfo.left(7).trimmed(); //获取短 ID 方便后续命名
    git.start("git", QStringList() << "checkout" << "-f" << shortId); //强制切换到历史版本
    git.waitForFinished();
    /*准备临时目标路径*/
    QString pureFolderName = QFileInfo(m_FilePathWithCode).fileName();
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
}

/*还原备份*/
void HomePageChild_BackupFile::on_pushButton_RestoreBackup_clicked()
{
    /*回滚仓库*/
    QProcess git;
    git.setWorkingDirectory(BackupPath + "/" + m_FilePathWithCode);
    QString shortId = m_commitInfo.left(7).trimmed(); //获取短 ID 方便后续命名
    git.start("git", QStringList() << "reset" << "--hard" << shortId); //强制切换到历史版本
    git.waitForFinished();
    /*替换源文件*/
    QString sourceFilePath = QUrl::fromPercentEncoding(m_FilePathWithCode.toUtf8());
    QString backupFilePath = BackupPath + "/" + m_FilePathWithCode + "/" + QFileInfo(sourceFilePath).fileName();
    if (QFile::exists(sourceFilePath)) QFile::remove(sourceFilePath);
    QFile::copy(backupFilePath, sourceFilePath);
    /*提示*/
    ElaMessageBar::success(ElaMessageBarType::BottomRight, "还原成功", "已还原至" + m_commitInfo.left(7).trimmed(), 3000, parentWidget()->parentWidget());
    /*布局更新*/
    QWidget *parentWidget = this->parentWidget();
    if (!parentWidget) return;
    QVBoxLayout *targetLayout = parentWidget->findChild<QVBoxLayout*>("verticalLayout_BackupFiles");
    if (targetLayout)
    {
        int currentIndex = targetLayout->indexOf(this);
        if (currentIndex > 0)
        {
            for (int i = currentIndex - 1; i >= 0; i--) //从后往前移除
            {
                QLayoutItem *item = targetLayout->itemAt(i);
                if (item && item->widget())
                {
                    QWidget *widget = item->widget();
                    targetLayout->removeWidget(widget);
                    widget->deleteLater();
                }
            }
            parentWidget->update();
        }
    }
}