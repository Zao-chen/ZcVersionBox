#include "aboutpage.h"
#include "ui_aboutpage.h"

#include "../../../GlobalConstants.h"

#include <QCoreApplication>
#include <QPixmap>

AboutPage::AboutPage(QWidget *parent)
    : QWidget(parent), ui(new Ui::AboutPage)
{
    ui->setupUi(this);
    ui->widget_BreadcrumbBar->setTextPixelSize(25);
    ui->widget_BreadcrumbBar->appendBreadcrumb("关于");

    ui->label_AppName->setText(QStringLiteral("ZcVersionBox"));

    QString appVersion = QCoreApplication::applicationVersion().trimmed();
    if (appVersion.isEmpty())
    {
        appVersion = QStringLiteral("0.1.0");
    }
}

AboutPage::~AboutPage()
{
    delete ui;
}
