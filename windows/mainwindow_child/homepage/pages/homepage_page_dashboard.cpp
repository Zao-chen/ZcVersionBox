#include "homepage_page_dashboard.h"
#include "ui_homepage_page_dashboard.h"
#include "windows/mainwindow_presentation.h"
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTimer>
#include <QUrl>

HomePageDashboardPage::HomePageDashboardPage(BackupService *service, QWidget *parent) : QWidget(parent), ui(new Ui::HomePageDashboardPage), m_service(service)
{
    ui->setupUi(this);
    for (auto *label : {ui->sourceLabel, ui->repositoryLabel, ui->stateValue, ui->syncStateLabel, ui->headerDot, ui->versionCaption, ui->fileCaption, ui->sizeCaption, ui->cacheCaption})
        UiStyle::text(label, UiStyle::FontRole::Caption, true);
    for (auto *value : {ui->versionValue, ui->fileValue, ui->sizeValue, ui->cacheValue})
        UiStyle::text(value, UiStyle::FontRole::Object);
    UiStyle::text(ui->sourcePathEdit, UiStyle::FontRole::Body);
    UiStyle::text(ui->repositoryPathEdit, UiStyle::FontRole::Body);
    ui->sourcePathEdit->setAccessibleName("源位置");
    ui->repositoryPathEdit->setAccessibleName("备份仓库位置");
    m_syncState = ui->syncStateLabel;
    ui->headerDot->hide();
    m_syncState->hide();
    m_syncPanel = new QWidget(this);
    m_syncPanel->setObjectName("syncStatePanel");
    auto *syncLayout = new QVBoxLayout(m_syncPanel);
    syncLayout->setContentsMargins(0, 0, 0, 0);
    syncLayout->setSpacing(8);
    m_syncDetail = new QLabel(m_syncPanel);
    m_syncDetail->setObjectName("syncDetailLabel");
    m_syncDetail->setWordWrap(true);
    m_syncDetail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_syncDetail->setTextFormat(Qt::PlainText);
    syncLayout->addWidget(m_syncDetail);
    UiStyle::text(m_syncDetail, UiStyle::FontRole::Caption, true);
    m_continueResolution = new QPushButton("前往多人协作…", m_syncPanel);
    m_continueResolution->setObjectName("continueResolutionButton");
    m_recheck = new QPushButton("重新检查", m_syncPanel);
    m_recheck->setObjectName("recheckBackupButton");
    for (auto *button : {m_continueResolution, m_recheck})
    {
        button->setAutoDefault(false);
        syncLayout->addWidget(button, 0, Qt::AlignLeft);
    }
    ui->bodyLayout->insertWidget(1, m_syncPanel);
    m_syncPanel->hide();
    connect(m_continueResolution, &QPushButton::clicked, this, [this]
            { emit navigate({PageId::Collaboration, m_id}); });
    connect(m_recheck, &QPushButton::clicked, this, [this]
            { m_service->recheck(m_id, this, completion()); });
    connect(service, &BackupService::busyChanged, this, &HomePageDashboardPage::updateActions);
    m_refresh = UiStyle::action(this, "refreshOverviewAction", "刷新概览", "refresh");
    m_refresh->setProperty("iconOnly", true);
    connect(m_refresh, &QAction::triggered, this, &HomePageDashboardPage::refresh);
    for (auto *button : {ui->copySourceButton, ui->copyRepositoryButton})
        button->setIcon(UiStyle::icon("copy"));
    for (auto *button : {ui->openSourceButton, ui->openRepositoryButton})
        button->setIcon(UiStyle::icon("external"));
    connect(ui->copySourceButton, &QToolButton::clicked, this, [this]
            { QApplication::clipboard()->setText(ui->sourcePathEdit->text()); });
    connect(ui->copyRepositoryButton, &QToolButton::clicked, this, [this]
            { QApplication::clipboard()->setText(ui->repositoryPathEdit->text()); });
    connect(ui->openSourceButton, &QToolButton::clicked, this, [this]
            { openLocalPath(this, m_service->sourcePath(m_id)); });
    connect(ui->openRepositoryButton, &QToolButton::clicked, this, [this]
            { openLocalPath(this, m_service->repoPath(m_id)); });
    connect(service, &BackupService::repositoryChanged, this, [this](const QString &id)
            {
        if (id == m_id && isVisible())
            refresh(); });
    connect(service, &BackupService::repositoryInvalidated, this, [this](const QString &id)
            {
        m_states.remove(id);
        if (id == m_id)
        {
            ++m_contextGeneration;
            ++m_requestGeneration;
        } });
    updateActions();
}
HomePageDashboardPage::~HomePageDashboardPage() = default;
QList<QAction *> HomePageDashboardPage::toolbarActions() const { return {m_refresh}; }
void HomePageDashboardPage::rememberState()
{
    if (!m_id.isEmpty() && m_repositoryGeneration == m_service->repositoryGeneration(m_id))
        m_states[m_id] = {m_repositoryGeneration, ui->scroll->verticalScrollBar()->value()};
}
void HomePageDashboardPage::deactivate()
{
    rememberState();
    m_active = false;
    ++m_contextGeneration;
    ++m_requestGeneration;
}
void HomePageDashboardPage::setBackup(const QString &id)
{
    rememberState();
    m_id = id;
    m_repositoryGeneration = m_service->repositoryGeneration(id);
    ++m_contextGeneration;
    m_active = true;
    refresh();
    const auto state = m_states.value(id);
    QTimer::singleShot(0, this, [this, id, state]
                       {
        if (m_id == id)
            ui->scroll->verticalScrollBar()->setValue(state.generation == m_repositoryGeneration ? state.scroll : 0); });
}
void HomePageDashboardPage::refresh()
{
    if (m_id.isEmpty())
        return;
    const auto id = m_id;
    const auto generation = m_repositoryGeneration;
    const auto context = m_contextGeneration;
    const auto request = ++m_requestGeneration;
    m_service->statistics(id, this, [this, id, generation, context, request](const BackupResult<BackupStats> &reply)
                          {
    if (!isCurrent(id, generation, context) || request != m_requestGeneration)
        return;
    if (!reply.result.success)
    {
        emit notification(reply.result);
        updateActions();
        return;
    }
    const auto &stats = reply.value;
    const auto updatePath = [](QLineEdit *field, const QString &path)
    {
        const auto native = QDir::toNativeSeparators(path);
        if (field->text() != native)
        {
            field->setText(native);
            field->setCursorPosition(0);
        }
        field->setToolTip(native);
    };
    updatePath(ui->sourcePathEdit, m_service->sourcePath(m_id));
    updatePath(ui->repositoryPathEdit, m_service->repoPath(m_id));
    ui->versionValue->setText(QString::number(stats.versionCount));
    ui->fileValue->setText(QString::number(stats.fileCount));
    ui->sizeValue->setText(formatBytes(stats.fileSize));
    ui->cacheValue->setText(formatBytes(stats.cacheSize));
    ui->stateValue->setText(stats.sourceState);
    const bool hasState = !m_syncState->text().isEmpty();
    const bool hasType = !stats.sourceState.isEmpty();
    ui->headerDot->setVisible(hasState && hasType);
    updateActions(); });
}
bool HomePageDashboardPage::isCurrent(const QString &id, quint64 generation, quint64 context) const
{
    return m_active && id == m_id && context == m_contextGeneration && m_service->contains(id) && generation == m_service->repositoryGeneration(id);
}
BackupService::Completion HomePageDashboardPage::completion()
{
    return [this, id = m_id, generation = m_repositoryGeneration, context = m_contextGeneration](const OperationResult &result)
    {
        if (isCurrent(id, generation, context))
        {
            emit notification(result);
            if (!m_service->tagConflicts(id).isEmpty()) emit navigate({PageId::History, id});
            else refresh();
        }
    };
}
void HomePageDashboardPage::updateActions()
{
    const bool busy = m_service->isBusy();
    const bool present = m_service->contains(m_id);
    const auto state = m_service->syncState(m_id);
    const bool pending = present && (state == BackupSyncState::RemotePending || state == BackupSyncState::ResolutionPending);
    auto stateText = present ? backupStateText(state) : QString();
    if (busy)
        stateText = stateText.isEmpty() ? QStringLiteral("正在处理备份任务…") : stateText + " · 正在处理备份任务…";
    m_syncState->setText(stateText);
    const bool hasState = !stateText.isEmpty();
    const bool hasType = !ui->stateValue->text().isEmpty();
    m_syncState->setVisible(hasState);
    ui->headerDot->setVisible(hasState && hasType);
    QString detail;
    for (const auto &item : m_service->trackedItems())
        if (item.id == m_id)
        {
            detail = item.stateDetail;
            break;
        }
    if (state == BackupSyncState::RemotePending)
        detail = "已获取的云端内容尚未确认。自动备份已暂停，请继续处理同步差异。";
    m_syncDetail->setText(detail);
    m_syncDetail->setVisible(!detail.isEmpty());
    m_continueResolution->setVisible(pending);
    m_recheck->setVisible(present && state == BackupSyncState::NeedsAttention);
    m_syncPanel->setVisible(m_syncDetail->isVisible() || m_continueResolution->isVisible() || m_recheck->isVisible());
    for (auto *button : {m_continueResolution, m_recheck})
        button->setEnabled(!busy);

}
void HomePageDashboardPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    ui->readingLayout->setContentsMargins(UiStyle::pageMargins(width(), true));
}
