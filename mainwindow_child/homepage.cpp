#include "homepage.h"
#include "ui_homepage.h"

#include "homepagechild_trackfile.h"

#include <QStandardPaths>
#include <QDir>
#include <QSpacerItem>

HomePage::HomePage(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::HomePage)
{
    ui->setupUi(this);

    /*获取所有备份文件夹*/
    //获取路径下所有文件夹并输出名字
    QString docPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)+"/ZcVersionBox/Backup";
    QDir dir(docPath);
    //只列出目录（排除文件），并排除 "." 和 ".."
    QStringList folderNames = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : std::as_const(folderNames))
    {
        HomePageChild_TrackFile *trackfile_widget = new HomePageChild_TrackFile(name, this); //创建子窗口
        ui->verticalLayout_TrackFiles->addWidget(trackfile_widget);
    }
    //最后再添加一个verticalSpacer
    auto *spacer = new QSpacerItem(20, 40, QSizePolicy::Minimum, QSizePolicy::Expanding);
    ui->verticalLayout_TrackFiles->addItem(spacer);
}

HomePage::~HomePage()
{
    delete ui;
}
