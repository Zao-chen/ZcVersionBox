#ifndef HOMEPAGECHILD_TRACKFILE_H
#define HOMEPAGECHILD_TRACKFILE_H

#include <QWidget>

namespace Ui {
class HomePageChild_TrackFile;
}

class HomePageChild_TrackFile : public QWidget
{
    Q_OBJECT

public:
    explicit HomePageChild_TrackFile(QString FilePath, QWidget *parent = nullptr);
    ~HomePageChild_TrackFile();

private slots:
    void on_pushButton_OpenFile_clicked();
    void on_pushButton_RemoveTrack_clicked();

private:
    Ui::HomePageChild_TrackFile *ui;
    QString m_FilePath;
    void BackupFile(); //备份文件
};

#endif // HOMEPAGECHILD_TRACKFILE_H
