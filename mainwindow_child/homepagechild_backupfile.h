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
    explicit HomePageChild_BackupFile(QString FilePath, QWidget *parent = nullptr);
    ~HomePageChild_BackupFile();

private:
    Ui::HomePageChild_BackupFile *ui;
};

#endif // HOMEPAGECHILD_BACKUPFILE_H
