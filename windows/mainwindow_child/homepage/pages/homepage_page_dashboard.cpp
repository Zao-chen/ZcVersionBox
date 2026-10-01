#include "homepage_page_dashboard.h"
#include "ui_homepage_page_dashboard.h"
#include "windows/mainwindow_presentation.h"
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QPointer>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTimer>
#include <QUrl>
#include <oclero/qlementine/widgets/Expander.hpp>
#include <oclero/qlementine/widgets/Switch.hpp>

HomePageDashboardPage::HomePageDashboardPage(BackupService *service, QWidget *parent) : QWidget(parent), ui(new Ui::HomePageDashboardPage), m_service(service)
{
    ui->setupUi(this);
    for (auto *label : {ui->sourceLabel, ui->repositoryLabel, ui->stateValue, ui->versionCaption, ui->fileCaption, ui->sizeCaption, ui->cacheCaption, ui->remoteHint})
        UiStyle::text(label, UiStyle::FontRole::Caption, true);
    for (auto *value : {ui->versionValue, ui->fileValue, ui->sizeValue, ui->cacheValue})
        UiStyle::text(value, UiStyle::FontRole::Object);
    UiStyle::text(ui->sourcePathEdit, UiStyle::FontRole::Body);
    UiStyle::text(ui->repositoryPathEdit, UiStyle::FontRole::Body);
    ui->sourcePathEdit->setAccessibleName("源位置");
    ui->repositoryPathEdit->setAccessibleName("备份仓库位置");
    ui->remoteUrl->setAccessibleName("云端仓库地址");
    ui->remoteUrlLabel->setBuddy(ui->remoteUrl);
    auto *syncPanel = new QWidget(this);
    syncPanel->setObjectName("syncStatePanel");
    auto *syncLayout = new QVBoxLayout(syncPanel);
    syncLayout->setContentsMargins(0, 0, 0, 0);
    syncLayout->setSpacing(8);
    m_syncState = new QLabel(syncPanel);
    m_syncState->setObjectName("syncStateLabel");
    m_syncDetail = new QLabel(syncPanel);
    m_syncDetail->setObjectName("syncDetailLabel");
    m_syncDetail->setWordWrap(true);
    m_syncDetail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_busy = new QLabel("正在处理备份任务…", syncPanel);
    m_busy->setObjectName("backupBusyLabel");
    for (auto *label : {m_syncState, m_syncDetail, m_busy})
    {
        label->setTextFormat(Qt::PlainText);
        syncLayout->addWidget(label);
    }
    UiStyle::text(m_syncState, UiStyle::FontRole::Body);
    UiStyle::text(m_syncDetail, UiStyle::FontRole::Caption, true);
    UiStyle::text(m_busy, UiStyle::FontRole::Caption, true);
    m_continueResolution = new QPushButton("继续处理同步差异…", syncPanel);
    m_continueResolution->setObjectName("continueResolutionButton");
    m_recheck = new QPushButton("重新检查", syncPanel);
    m_recheck->setObjectName("recheckBackupButton");
    for (auto *button : {m_continueResolution, m_recheck})
    {
        button->setAutoDefault(false);
        syncLayout->addWidget(button, 0, Qt::AlignLeft);
    }
    ui->bodyLayout->insertWidget(1, syncPanel);
    connect(m_continueResolution, &QPushButton::clicked, this, [this]
            { emit navigate({PageId::Conflict, m_id}); });
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
    m_remoteSwitch = new oclero::qlementine::Switch(this);
    m_remoteSwitch->setObjectName("remoteSwitch");
    m_remoteSwitch->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_remoteSwitch->setFocusPolicy(Qt::TabFocus);
    m_remoteSwitch->setAccessibleName("云端同步");
    ui->remoteHeaderLayout->addWidget(m_remoteSwitch);
    ui->remoteHeaderLayout->setAlignment(m_remoteSwitch, Qt::AlignVCenter);
    ui->remoteLayout->removeWidget(ui->remoteContent);
    m_expander = new oclero::qlementine::Expander(this);
    m_expander->setObjectName("remoteExpander");
    m_expander->setContent(ui->remoteContent);
    ui->remoteLayout->addWidget(m_expander);
    connect(m_remoteSwitch, &QAbstractButton::toggled, this, &HomePageDashboardPage::remoteToggled);
    connect(ui->remoteUrl, &QLineEdit::editingFinished, this, [this]
            {
        if (!ui->remoteUrl->isModified())
            return;
        ui->remoteUrl->setModified(false);
        m_service->setRemote(m_id, ui->remoteUrl->text(), this, completion()); });
    connect(ui->openRemoteButton, &QPushButton::clicked, this, [this]
            {
        if (!QDesktopServices::openUrl(QUrl(ui->remoteUrl->text())))
            emit notification(OperationResult::fail("打开失败", "无法打开云端地址，请检查链接格式")); });
    connect(ui->pullButton, &QPushButton::clicked, this, [this]
    {
        const auto id = m_id;
        const auto generation = m_repositoryGeneration;
        const auto context = m_contextGeneration;
        m_service->prepareSyncResolution(id, this, [this, id, generation, context](const BackupResult<SyncResolutionSession> &reply)
        {
            if (!isCurrent(id, generation, context)) return;
            emit notification(reply.result);
            if (reply.result.success && !reply.value.id.isEmpty()) emit navigate({PageId::Conflict, id});
            else refresh();
        });
    });
    connect(ui->pushButton, &QPushButton::clicked, this, [this]
            { m_service->synchronize(m_id, true, this, completion()); });
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
            ui->remoteUrl->setModified(false);
            const QSignalBlocker blocker(m_remoteSwitch);
            m_remoteSwitch->setChecked(false);
            m_expander->setExpanded(false);
        } });
    updateActions();
}
HomePageDashboardPage::~HomePageDashboardPage() = default;
QList<QAction *> HomePageDashboardPage::toolbarActions() const { return {m_refresh}; }
void HomePageDashboardPage::rememberState()
{
    if (!m_id.isEmpty() && m_repositoryGeneration == m_service->repositoryGeneration(m_id))
        m_states[m_id] = {m_repositoryGeneration, ui->scroll->verticalScrollBar()->value(), m_expander->expanded()};
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
    const bool changed = id != m_id || m_repositoryGeneration != m_service->repositoryGeneration(id);
    if (changed)
    {
        // A focused editor can survive navigation to another object.
        ui->remoteUrl->setModified(false);
        ui->remoteUrl->clear();
    }
    m_id = id;
    m_repositoryGeneration = m_service->repositoryGeneration(id);
    ++m_contextGeneration;
    m_active = true;
    refresh();
    const auto state = m_states.value(id);
    const bool expanded = state.generation == m_repositoryGeneration && state.expanded;
    m_expander->setExpanded(expanded);
    {
        const QSignalBlocker blocker(m_remoteSwitch);
        m_remoteSwitch->setChecked(expanded);
    }
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
    const bool hasRemote = !stats.remoteUrl.isEmpty();
    const bool editing = (ui->remoteUrl->hasFocus() || ui->remoteUrl->isModified()) && m_remoteSwitch->isChecked();
    const bool active = hasRemote || editing;
    {
        const QSignalBlocker blocker(m_remoteSwitch);
        m_remoteSwitch->setChecked(active);
    }
    m_expander->setExpanded(active);
    if (!ui->remoteUrl->hasFocus() || !ui->remoteUrl->isModified())
    {
        ui->remoteUrl->setText(stats.remoteUrl);
        ui->remoteUrl->setModified(false);
    }
    ui->openRemoteButton->setEnabled(hasRemote);
    updateActions(); });
}
void HomePageDashboardPage::remoteToggled(bool checked)
{
    if (!checked)
    {
        ui->remoteUrl->clearFocus();
        ui->remoteUrl->setModified(false);
        ui->remoteUrl->clear();
        m_service->removeRemote(m_id, this, completion());
        m_expander->setExpanded(false);
    }
    else
    {
        m_expander->setExpanded(true);
        ui->remoteUrl->setFocus();
    }
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
            refresh();
        }
    };
}
void HomePageDashboardPage::updateActions()
{
    const bool busy = m_service->isBusy();
    const bool present = m_service->contains(m_id);
    const auto state = m_service->syncState(m_id);
    const bool pending = present && (state == BackupSyncState::RemotePending || state == BackupSyncState::ResolutionPending);
    m_busy->setVisible(busy);
    m_syncState->setText(present ? backupStateText(state) : QString());
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
    for (auto *button : {m_continueResolution, m_recheck})
        button->setEnabled(!busy);
    m_remoteSwitch->setEnabled(!busy);
    const bool remote = present && !ui->remoteUrl->text().isEmpty();
    ui->pullButton->setEnabled(remote && !busy && state == BackupSyncState::Tracking);
    ui->pushButton->setEnabled(remote && !busy && state == BackupSyncState::Tracking);
}
void HomePageDashboardPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    const auto margin = width() < 640 ? 16 : 24;
    ui->readingLayout->setContentsMargins(margin, 24, margin, 24);
}
