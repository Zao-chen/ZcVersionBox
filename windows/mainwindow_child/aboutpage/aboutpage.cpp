#include "aboutpage.h"
#include "ui_aboutpage.h"
#include "windows/mainwindow_presentation.h"
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QTextBrowser>
#include <QVBoxLayout>
AboutPage::AboutPage(UpdateService *updates, QWidget *parent) : QWidget(parent), ui(new Ui::AboutPage), m_updates(updates)
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

    connect(ui->checkUpdatesButton, &QPushButton::clicked, this, [this]
            {
        if (m_updates->isChecking())
            return;
        if (m_availableUpdate)
        {
            showAvailableUpdate();
            return;
        }
        m_manualCheckPending = true;
        m_updates->checkForUpdates(); });
    connect(m_updates, &UpdateService::checkingChanged, this, [this](bool checking)
            {
        ui->checkUpdatesButton->setEnabled(!checking);
        ui->checkUpdatesButton->setText(checking ? QStringLiteral("检查中…") :
            m_availableUpdate ? QStringLiteral("查看更新") : QStringLiteral("检查更新"));
        });
    connect(m_updates, &UpdateService::checkFinished, this, [this](const UpdateCheckResult &result)
            {
        ui->checkUpdatesButton->setEnabled(true);
        if (!m_manualCheckPending)
            return;
        m_manualCheckPending = false;
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
        setAvailableUpdate(result.release);
        showAvailableUpdate(); });
}
void AboutPage::setAvailableUpdate(const UpdateRelease &release)
{
    m_availableUpdate = release;
    ui->checkUpdatesButton->setText(QStringLiteral("查看更新"));
    ui->checkUpdatesButton->setAccessibleName(QStringLiteral("查看更新"));
}
void AboutPage::showAvailableUpdate()
{
    if (m_availableUpdate)
        showRelease(*m_availableUpdate);
}
void AboutPage::showRelease(const UpdateRelease &release)
{
        QDialog dialog(this);
        dialog.setObjectName(QStringLiteral("updateDialog"));
        dialog.setWindowTitle(QStringLiteral("发现新版本"));
        dialog.setModal(true);
        dialog.setMinimumSize(460, 320);
        dialog.setMaximumSize(720, 560);
        dialog.resize(560, 440);
        auto *layout = new QVBoxLayout(&dialog);
        layout->setContentsMargins(24, 24, 24, 20);
        layout->setSpacing(12);

        auto *title = new QLabel(QStringLiteral("发现 ZcVersionBox v%1").arg(release.version), &dialog);
        UiStyle::text(title, UiStyle::FontRole::Section);
        layout->addWidget(title);

        auto *current = new QLabel(QStringLiteral("当前版本 v%1").arg(release.currentVersion), &dialog);
        UiStyle::text(current, UiStyle::FontRole::Caption, true);
        layout->addWidget(current);

        auto *notes = new QTextBrowser(&dialog);
        notes->setObjectName(QStringLiteral("updateNotes"));
        notes->setFrameShape(QFrame::NoFrame);
        notes->setOpenLinks(false);
        notes->setOpenExternalLinks(false);
        notes->setMarkdown(release.notes.isEmpty() ? QStringLiteral("该版本暂无更新说明。") : release.notes);
        notes->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        notes->setMinimumHeight(180);
        layout->addWidget(notes, 1);

        auto *status = new QLabel(&dialog);
        status->setObjectName(QStringLiteral("updateStatus"));
        UiStyle::text(status, UiStyle::FontRole::Caption, true);
        layout->addWidget(status);

        auto *progress = new QProgressBar(&dialog);
        progress->setObjectName(QStringLiteral("updateProgress"));
        progress->setTextVisible(true);
        progress->setVisible(false);
        layout->addWidget(progress);

        auto *buttonBar = new QWidget(&dialog);
        auto *buttonLayout = new QHBoxLayout(buttonBar);
        buttonLayout->setContentsMargins(0, 0, 0, 0);
        auto *later = new QPushButton(QStringLiteral("稍后"), buttonBar);
        auto *downloadUpdate = new QPushButton(QStringLiteral("下载并更新"), buttonBar);
        buttonLayout->addWidget(later);
        buttonLayout->addStretch();
        buttonLayout->addWidget(downloadUpdate);
        const bool canDownload = release.downloadUrl.isValid() && !release.downloadName.isEmpty();
        downloadUpdate->setEnabled(canDownload);
        downloadUpdate->setProperty("primary", canDownload);
        later->setAutoDefault(false);
        downloadUpdate->setAutoDefault(false);
        layout->addWidget(buttonBar);

        if (!canDownload)
            status->setText(QStringLiteral("当前平台暂无可用安装包，请稍后通过项目发布页手动下载。"));
        connect(later, &QPushButton::clicked, &dialog, &QDialog::reject);
        connect(downloadUpdate, &QPushButton::clicked, &dialog, [this, &release, downloadUpdate, status]
                {
            if (m_updates->isDownloading())
                return;
            status->setText(QStringLiteral("正在准备下载更新…"));
            downloadUpdate->setEnabled(false);
            m_updates->downloadUpdate(release); });
        connect(m_updates, &UpdateService::downloadingChanged, &dialog, [downloadUpdate, later, progress, status](bool downloading)
                {
            downloadUpdate->setEnabled(!downloading);
            downloadUpdate->setText(downloading ? QStringLiteral("下载中…") : QStringLiteral("下载并更新"));
            later->setEnabled(true);
            progress->setVisible(downloading);
            if (downloading)
            {
                progress->setRange(0, 0);
                status->setText(QStringLiteral("正在下载更新…"));
            }
            else if (progress->isVisible())
            {
                progress->setVisible(false);
            } });
        connect(m_updates, &UpdateService::downloadProgress, &dialog, [progress, status](qint64 received, qint64 total)
                {
            if (total <= 0)
            {
                progress->setRange(0, 0);
                return;
            }
            const auto percentage = static_cast<int>((received * 100) / total);
            progress->setRange(0, 100);
            progress->setValue(qBound(0, percentage, 100));
            status->setText(QStringLiteral("正在下载更新… %1%").arg(progress->value())); });
        connect(m_updates, &UpdateService::downloadFinished, &dialog, [this, &dialog, progress, status](const QString &path)
                {
            progress->setVisible(false);
            status->setText(QStringLiteral("更新包已下载：%1，正在打开安装包…").arg(QFileInfo(path).fileName()));
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
            dialog.accept(); });
        connect(m_updates, &UpdateService::downloadFailed, &dialog, [downloadUpdate, progress, status](const QString &error)
                {
            progress->setVisible(false);
            downloadUpdate->setEnabled(true);
            downloadUpdate->setText(QStringLiteral("下载并更新"));
            status->setText(QStringLiteral("下载失败：%1").arg(error)); });
        connect(&dialog, &QDialog::finished, &dialog, [this]
                {
            if (m_updates->isDownloading())
                m_updates->cancelDownload(); });
        dialog.exec();
}
AboutPage::~AboutPage() = default;
void AboutPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    const bool compact = width() < 640;
    const auto margin = compact ? 16 : 24;
    ui->readingLayout->setContentsMargins(margin, compact ? 24 : 64, margin, 32);
}
