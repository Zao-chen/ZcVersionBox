#include "aboutpage.h"
#include "ui_aboutpage.h"
#include "windows/mainwindow_presentation.h"
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QResizeEvent>
#include <QTextBrowser>
#include <QVBoxLayout>
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
        QDialog dialog(this);
        dialog.setWindowTitle(QStringLiteral("发现新版本"));
        dialog.setModal(true);
        dialog.setMinimumSize(460, 320);
        dialog.setMaximumSize(720, 560);
        dialog.resize(560, 440);
        auto *layout = new QVBoxLayout(&dialog);
        layout->setContentsMargins(24, 24, 24, 20);
        layout->setSpacing(12);

        auto *title = new QLabel(QStringLiteral("发现 ZcVersionBox v%1").arg(result.release.version), &dialog);
        UiStyle::text(title, UiStyle::FontRole::Section);
        layout->addWidget(title);

        auto *current = new QLabel(QStringLiteral("当前版本 v%1").arg(result.release.currentVersion), &dialog);
        UiStyle::text(current, UiStyle::FontRole::Caption, true);
        layout->addWidget(current);

        auto *notes = new QTextBrowser(&dialog);
        notes->setFrameShape(QFrame::NoFrame);
        notes->setOpenLinks(false);
        notes->setOpenExternalLinks(false);
        notes->setMarkdown(result.release.notes.isEmpty() ? QStringLiteral("该版本暂无更新说明。") : result.release.notes);
        notes->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        notes->setMinimumHeight(180);
        layout->addWidget(notes, 1);

        auto *buttons = new QDialogButtonBox(Qt::Horizontal, &dialog);
        auto *openRelease = buttons->addButton(QStringLiteral("打开发布页"), QDialogButtonBox::AcceptRole);
        auto *later = buttons->addButton(QStringLiteral("稍后"), QDialogButtonBox::RejectRole);
        openRelease->setProperty("primary", true);
        openRelease->setAutoDefault(false);
        later->setAutoDefault(false);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        dialog.exec();
        if (dialog.result() == QDialog::Accepted)
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
