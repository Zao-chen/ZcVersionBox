#include "homepagechild_backupfile.h"
#include "ui_homepagechild_backupfile.h"

HomePageChild_BackupFile::HomePageChild_BackupFile(QString FilePath, QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::HomePageChild_BackupFile)
{
    ui->setupUi(this);
    ui->label->setText(FilePath);
}

HomePageChild_BackupFile::~HomePageChild_BackupFile()
{
    delete ui;
}
