#include "aboutpage.h"
#include "ui_aboutpage.h"
#include "windows/mainwindow_presentation.h"
#include <QCoreApplication>
#include <QDesktopServices>
#include <QFileInfo>
#include <QResizeEvent>
#include <QUrl>
AboutPage::AboutPage(QWidget *parent) : QWidget(parent), ui(new Ui::AboutPage)
{
    ui->setupUi(this);
    ui->versionLabel->setText("版本 " + QCoreApplication::applicationVersion());
    ui->logoLabel->setPixmap(QPixmap(":/img/ico/res/img/logo.png").scaled(40, 40, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    UiStyle::text(ui->pageTitle, UiStyle::FontRole::Page);
    UiStyle::text(ui->appNameLabel, UiStyle::FontRole::Object);
    UiStyle::text(ui->versionLabel, UiStyle::FontRole::Caption, true);
    UiStyle::text(ui->technologyLabel, UiStyle::FontRole::Caption, true);
    UiStyle::text(ui->updateTitleLabel, UiStyle::FontRole::Section);
    UiStyle::text(ui->updateStatusLabel, UiStyle::FontRole::Body);
    UiStyle::text(ui->updateDetailsLabel, UiStyle::FontRole::Caption, true);
    ui->updateProgress->setVisible(false);
    ui->downloadUpdateButton->setVisible(false);
    ui->openReleaseButton->setVisible(false);
    ui->openDownloadedButton->setVisible(false);
    ui->checkUpdatesButton->setAutoDefault(false);
    ui->downloadUpdateButton->setAutoDefault(false);
    ui->openReleaseButton->setAutoDefault(false);
    ui->openDownloadedButton->setAutoDefault(false);
    ui->checkUpdatesButton->setProperty("primary", true);

    m_updates = new UpdateService(this);
    connect(ui->checkUpdatesButton, &QPushButton::clicked, this, [this]
            {
        resetDownloadState();
        m_updates->checkForUpdates(); });
    connect(ui->downloadUpdateButton, &QPushButton::clicked, this, [this]
            {
        if (m_updates->isDownloading())
            m_updates->cancelDownload();
        else
            m_updates->downloadUpdate(m_release); });
    connect(ui->openReleaseButton, &QPushButton::clicked, this, [this]
            { QDesktopServices::openUrl(m_release.releaseUrl); });
    connect(ui->openDownloadedButton, &QPushButton::clicked, this, [this]
            { QDesktopServices::openUrl(QUrl::fromLocalFile(m_downloadedPath)); });
    connect(m_updates, &UpdateService::checkingChanged, this, [this](bool checking)
            {
        ui->checkUpdatesButton->setEnabled(!checking);
        ui->checkUpdatesButton->setText(checking ? QStringLiteral("检查中…") : QStringLiteral("检查更新"));
        if (checking)
        {
            ui->updateStatusLabel->setText(QStringLiteral("正在连接 GitHub 获取最新版本…"));
            ui->updateDetailsLabel->clear();
            ui->updateProgress->setVisible(false);
        } });
    connect(m_updates, &UpdateService::checkFinished, this, &AboutPage::setUpdateResult);
    connect(m_updates, &UpdateService::downloadingChanged, this, [this](bool downloading)
            {
        ui->downloadUpdateButton->setEnabled(true);
        ui->downloadUpdateButton->setText(downloading ? QStringLiteral("取消下载") : QStringLiteral("下载更新"));
        ui->checkUpdatesButton->setEnabled(!downloading && !m_updates->isChecking());
        if (downloading)
        {
            ui->updateProgress->setVisible(true);
            ui->updateProgress->setRange(0, 0);
            ui->updateStatusLabel->setText(QStringLiteral("正在下载更新…"));
        } });
    connect(m_updates, &UpdateService::downloadProgress, this, [this](qint64 received, qint64 total)
            {
        if (total <= 0)
            return;
        ui->updateProgress->setRange(0, 100);
        ui->updateProgress->setValue(static_cast<int>((received * 100) / total));
        ui->updateStatusLabel->setText(QStringLiteral("正在下载更新… %1%").arg(ui->updateProgress->value())); });
    connect(m_updates, &UpdateService::downloadFinished, this, [this](const QString &path)
            {
        m_downloadedPath = path;
        ui->updateProgress->setVisible(false);
        ui->downloadUpdateButton->setVisible(false);
        ui->openDownloadedButton->setVisible(true);
        ui->updateStatusLabel->setText(QStringLiteral("更新包已下载到：%1").arg(QFileInfo(path).fileName()));
        ui->updateDetailsLabel->setText(QStringLiteral("打开下载目录中的安装包即可完成更新。")); });
    connect(m_updates, &UpdateService::downloadFailed, this, [this](const QString &error)
            {
        ui->updateProgress->setVisible(false);
        ui->downloadUpdateButton->setEnabled(true);
        ui->updateStatusLabel->setText(QStringLiteral("下载失败：%1").arg(error)); });
}
AboutPage::~AboutPage() = default;
void AboutPage::setUpdateResult(const UpdateCheckResult &result)
{
    ui->checkUpdatesButton->setEnabled(true);
    ui->openDownloadedButton->setVisible(false);
    m_downloadedPath.clear();
    if (!result.success)
    {
        ui->updateStatusLabel->setText(QStringLiteral("检查失败：%1").arg(result.error));
        ui->updateDetailsLabel->setText(QStringLiteral("请检查网络连接后重试，也可以打开项目发布页手动下载。"));
        ui->downloadUpdateButton->setVisible(false);
        ui->openReleaseButton->setVisible(false);
        return;
    }

    m_release = result.release;
    ui->openReleaseButton->setVisible(result.release.releaseUrl.isValid());
    ui->updateDetailsLabel->setText(result.updateAvailable
                                        ? (result.release.notes.isEmpty() ? QStringLiteral("该版本暂无更新说明。") : result.release.notes)
                                        : QStringLiteral("当前版本已包含最新稳定版本的功能。"));
    if (!result.updateAvailable)
    {
        ui->updateStatusLabel->setText(QStringLiteral("已是最新版本：v%1").arg(result.release.currentVersion));
        ui->downloadUpdateButton->setVisible(false);
        return;
    }

    ui->updateStatusLabel->setText(QStringLiteral("发现新版本：v%1").arg(result.release.version));
    ui->downloadUpdateButton->setVisible(result.release.downloadUrl.isValid());
    ui->downloadUpdateButton->setEnabled(true);
    if (!result.release.downloadUrl.isValid())
        ui->updateDetailsLabel->setText(ui->updateDetailsLabel->text() + QStringLiteral("\n当前平台暂无直接安装包，请打开发布页下载。"));
}
void AboutPage::resetDownloadState()
{
    ui->downloadUpdateButton->setVisible(false);
    ui->openReleaseButton->setVisible(false);
    ui->openDownloadedButton->setVisible(false);
    ui->updateProgress->setVisible(false);
    ui->updateDetailsLabel->clear();
    m_release = {};
    m_downloadedPath.clear();
}
void AboutPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    const bool compact = width() < 640;
    const auto margin = compact ? 16 : 24;
    ui->readingLayout->setContentsMargins(margin, compact ? 24 : 64, margin, 32);
}
