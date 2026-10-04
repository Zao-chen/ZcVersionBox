#include "aboutpage.h"
#include "ui_aboutpage.h"
#include "windows/mainwindow_presentation.h"
#include <QCoreApplication>
#include <QDesktopServices>
#include <QMessageBox>
#include <QResizeEvent>
AboutPage::AboutPage(QWidget *parent) : QWidget(parent), ui(new Ui::AboutPage)
{
    ui->setupUi(this);
    ui->versionLabel->setText("版本 " + QCoreApplication::applicationVersion());
    ui->logoLabel->setPixmap(QPixmap(":/img/ico/res/img/logo.png").scaled(40, 40, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    UiStyle::text(ui->pageTitle, UiStyle::FontRole::Page);
    UiStyle::text(ui->appNameLabel, UiStyle::FontRole::Object);
    UiStyle::text(ui->versionLabel, UiStyle::FontRole::Caption, true);
    ui->checkUpdatesButton->setAutoDefault(false);
    ui->checkUpdatesButton->setAccessibleName(QStringLiteral("检查更新"));
    ui->checkUpdatesButton->setProperty("primary", true);
    ui->checkUpdatesButton->setIcon(UiStyle::icon("refresh"));

    m_updates = new UpdateService(this);
    connect(ui->checkUpdatesButton, &QPushButton::clicked, this, [this]
            { m_updates->checkForUpdates(); });
    connect(m_updates, &UpdateService::checkingChanged, this, [this](bool checking)
            {
        ui->checkUpdatesButton->setEnabled(!checking);
        ui->checkUpdatesButton->setText(checking ? QStringLiteral("检查中…") : QStringLiteral("检查更新"));
        });
    connect(m_updates, &UpdateService::checkFinished, this, [this](const UpdateCheckResult &result)
            {
        ui->checkUpdatesButton->setEnabled(true);
        if (!result.success)
        {
            QMessageBox::warning(this, QStringLiteral("检查更新失败"), result.error);
            return;
        }
        if (!result.updateAvailable)
        {
            QMessageBox::information(this, QStringLiteral("已是最新版本"),
                                     QStringLiteral("当前版本 v%1 已是最新稳定版本。").arg(result.release.currentVersion));
            return;
        }
        QMessageBox message(QMessageBox::Information, QStringLiteral("发现新版本"),
                            QStringLiteral("发现 ZcVersionBox v%1。\n\n%2")
                                .arg(result.release.version,
                                     result.release.notes.isEmpty() ? QStringLiteral("该版本暂无更新说明。") : result.release.notes),
                            QMessageBox::NoButton, this);
        auto *openRelease = message.addButton(QStringLiteral("打开发布页"), QMessageBox::AcceptRole);
        message.addButton(QStringLiteral("稍后"), QMessageBox::RejectRole);
        message.exec();
        if (message.clickedButton() == openRelease)
            QDesktopServices::openUrl(result.release.releaseUrl);
        });
}
AboutPage::~AboutPage() = default;
void AboutPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    const bool compact = width() < 640;
    const auto margin = compact ? 16 : 24;
    ui->readingLayout->setContentsMargins(margin, compact ? 24 : 64, margin, 32);
}
