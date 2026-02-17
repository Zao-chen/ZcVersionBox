#ifndef HOMEPAGECHILD_BACKUPFILE_H
#define HOMEPAGECHILD_BACKUPFILE_H

#include <QWidget>

namespace Ui {
class HomePageChild_BackupFile;
}

class HomePageChild_BackupFile : public QWidget
{
    Q_OBJECT

public:
    explicit HomePageChild_BackupFile(QString FilePathWithCode, QString commitInfo, QWidget *parent = nullptr);
    ~HomePageChild_BackupFile();

private slots:
    void on_pushButton_OpenFile_clicked();
    void on_pushButton_RestoreBackup_clicked();

private:
    Ui::HomePageChild_BackupFile *ui;
    QString m_commitInfo;
    QString m_FilePathWithCode;
};

#endif // HOMEPAGECHILD_BACKUPFILE_H
