#include "aboutPage.h"
#include "ui_aboutPage.h"
#include <QCoreApplication>
AboutPage::AboutPage(QWidget *parent) : QWidget(parent), ui(new Ui::AboutPage)
{
    ui->setupUi(this);
    ui->versionLabel->setText("版本 " + QCoreApplication::applicationVersion());
    ui->logoLabel->setPixmap(QPixmap(":/img/ico/res/img/logo.png").scaled(80, 80, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}
AboutPage::~AboutPage() = default;
