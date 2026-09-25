#include "aboutpage.h"
#include "ui_aboutpage.h"
#include "windows/mainwindow_presentation.h"
#include <QCoreApplication>
#include <QResizeEvent>
AboutPage::AboutPage(QWidget *parent) : QWidget(parent), ui(new Ui::AboutPage)
{
    ui->setupUi(this);
    ui->versionLabel->setText("版本 " + QCoreApplication::applicationVersion());
    ui->logoLabel->setPixmap(QPixmap(":/img/ico/res/img/logo.png").scaled(40, 40, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    UiStyle::text(ui->pageTitle, UiStyle::FontRole::Page);
    UiStyle::text(ui->appNameLabel, UiStyle::FontRole::Object);
    UiStyle::text(ui->versionLabel, UiStyle::FontRole::Caption, true);
    UiStyle::text(ui->technologyLabel, UiStyle::FontRole::Caption, true);
}
AboutPage::~AboutPage() = default;
void AboutPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    const bool compact = width() < 640;
    const auto margin = compact ? 16 : 24;
    ui->readingLayout->setContentsMargins(margin, compact ? 24 : 64, margin, 32);
}
