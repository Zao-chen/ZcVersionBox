#include "homepage_page_collaboration.h"
#include "ui_homepage_page_collaboration.h"
#include "windows/mainwindow_dialog.h"
#include "windows/mainwindow_presentation.h"
#include <QApplication>
#include <QClipboard>
#include <QPointer>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <oclero/qlementine/widgets/Switch.hpp>

HomePageCollaborationPage::HomePageCollaborationPage(BackupService *service, QWidget *parent)
    : QWidget(parent), ui(new Ui::HomePageCollaborationPage), m_service(service)
{
    ui->setupUi(this);
    for (auto *label : {ui->stateLabel, ui->automaticTitle, ui->remoteTitle}) UiStyle::text(label, UiStyle::FontRole::Section);
    for (auto *label : {ui->lastSync, ui->automaticHint}) UiStyle::text(label, UiStyle::FontRole::Caption, true);
    UiStyle::text(ui->remoteUrl, UiStyle::FontRole::Body);
    ui->syncButton->setProperty("primary", true);
    ui->syncButton->setIcon(UiStyle::icon("refresh"));
    ui->connectButton->setIcon(UiStyle::icon("link"));
    ui->copyButton->setIcon(UiStyle::icon("copy"));
    ui->disconnectButton->setIcon(UiStyle::icon("unlink"));
    m_automatic = new oclero::qlementine::Switch(this);
    m_automatic->setObjectName("automaticSyncSwitch"); m_automatic->setAccessibleName("自动同步"); m_automatic->setFocusPolicy(Qt::TabFocus);
    m_automatic->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    ui->automaticRow->addWidget(m_automatic, 0, Qt::AlignVCenter);
    connect(service, &BackupService::syncStatusChanged, this, [this](const QString &id) { if (m_active && id == m_id) refresh(); });
    connect(service, &BackupService::repositoryChanged, this, [this](const QString &id) { if (m_active && id == m_id) refresh(); });
    connect(service, &BackupService::repositoryInvalidated, this, [this](const QString &id) { if (id == m_id) { ++m_context; m_connecting = false; refresh(); } });
    connect(ui->syncButton, &QPushButton::clicked, this, &HomePageCollaborationPage::synchronize);
    connect(ui->connectButton, &QToolButton::clicked, this, &HomePageCollaborationPage::connectRemote);
    connect(ui->remoteUrl, &QLineEdit::textEdited, this, [this] { refresh(); });
    connect(ui->remoteUrl, &QLineEdit::returnPressed, this, &HomePageCollaborationPage::connectRemote);
    connect(ui->copyButton, &QToolButton::clicked, this, [this] {
        QApplication::clipboard()->setText(m_service->syncStatus(m_id).remoteUrl);
        emit notification(OperationResult::ok("协作地址已复制", "协作者可通过“从云端导入”加入。"));
    });
    connect(ui->disconnectButton, &QToolButton::clicked, this, [this] {
        const auto id = m_id; const auto generation = m_generation, context = m_context;
        const QPointer<HomePageCollaborationPage> guard(this);
        if (!confirmAction(this, "断开后停止云端同步，本地文件和版本历史仍然保留。", "断开共享仓库") || !guard || !current(id, generation, context)) return;
        m_service->removeRemote(id, this, [this, id, generation, context](const OperationResult &result) {
            if (!current(id, generation, context)) return;
            if (result.success) { ui->remoteUrl->clear(); ui->remoteUrl->setModified(false); }
            emit notification(result); refresh();
        });
    });
    connect(m_automatic, &QAbstractButton::toggled, this, [this](bool enabled) {
        const auto id = m_id; const auto generation = m_generation, context = m_context;
        m_service->setAutoSync(id, enabled, this, [this, id, generation, context](const OperationResult &result) {
            if (!current(id, generation, context)) return;
            if (!result.success) emit notification(result);
            refresh();
        });
    });
}
HomePageCollaborationPage::~HomePageCollaborationPage() = default;
bool HomePageCollaborationPage::current(const QString &id, quint64 generation, quint64 context) const
{
    return m_active && m_id == id && m_context == context && m_service->contains(id) && m_service->repositoryGeneration(id) == generation;
}
void HomePageCollaborationPage::setBackup(const QString &id)
{
    if (!m_id.isEmpty()) m_scrolls[m_id] = ui->scroll->verticalScrollBar()->value();
    m_id = id; m_generation = m_service->repositoryGeneration(id); ++m_context;
    m_active = true; m_connecting = false; ui->remoteUrl->setModified(false);
    ui->remoteUrl->setText(m_service->syncStatus(id).remoteUrl); refresh();
    ui->scroll->verticalScrollBar()->setValue(m_scrolls.value(id));
}
void HomePageCollaborationPage::deactivate()
{
    m_scrolls[m_id] = ui->scroll->verticalScrollBar()->value(); m_active = false; ++m_context; m_connecting = false; setLoading(false);
}
void HomePageCollaborationPage::setLoading(bool loading)
{
    if (m_loading != loading) { m_loading = loading; emit loadingChanged(loading); }
}
void HomePageCollaborationPage::connectRemote()
{
    if (!m_active || m_connecting || !ui->connectButton->isEnabled()) return;
    const auto id = m_id, url = ui->remoteUrl->text().trimmed();
    const auto generation = m_generation, context = m_context;
    m_connecting = true; refresh();
    m_service->checkRemote(url, this, [this, id, url, generation, context](const OperationResult &checked) {
        if (!current(id, generation, context)) return;
        if (!checked.success) { m_connecting = false; emit notification(checked); refresh(); return; }
        m_service->setRemote(id, url, this, [this, id, generation, context](const OperationResult &saved) {
            if (!current(id, generation, context)) return;
            m_connecting = false;
            if (saved.success) ui->remoteUrl->setModified(false);
            emit notification(saved); refresh();
        });
    });
}
void HomePageCollaborationPage::synchronize()
{
    if (!m_active || !m_service->contains(m_id) || m_service->isSynchronizing(m_id)) return;
    const auto id = m_id; const auto generation = m_generation, context = m_context;
    const auto status = m_service->syncStatus(id);
    const auto backupState = m_service->syncState(id);
    if (backupState == BackupSyncState::ResolutionPending || backupState == BackupSyncState::RemotePending) { emit navigate({PageId::Conflict, id}); return; }
    if (status.state == SyncState::NeedsBranch && !status.branchRef.isEmpty() && status.branchRef != m_service->branchContext(id).ref) { emit switchRequested(id, status.branchRef); return; }
    if (!m_service->tagConflicts(id).isEmpty()) { emit navigate({PageId::History, id}); return; }
    if (backupState == BackupSyncState::NeedsAttention)
    {
        m_service->recheck(id, this, [this, id, generation, context](const OperationResult &result) {
            if (!current(id, generation, context)) return;
            emit notification(result); refresh();
        });
        return;
    }
    m_service->synchronize(id, SyncRequestOptions{}, this, [this, id, generation, context](const BackupResult<SyncResult> &result) {
        if (!current(id, generation, context)) return;
        emit notification(result.result); refresh();
        if (result.value.status.state == SyncState::NeedsResolution) emit navigate({PageId::Conflict, id});
    });
}
void HomePageCollaborationPage::refresh()
{
    if (!m_active) return;
    const auto status = m_service->syncStatus(m_id);
    const auto state = m_service->syncState(m_id);
    const bool connected = !status.remoteUrl.isEmpty(), running = m_service->isSynchronizing(m_id);
    QString title, summary;
    switch (status.state)
    {
    case SyncState::Disconnected: title = "尚未连接共享仓库"; summary = "连接共享仓库，与他人同步文件。"; break;
    case SyncState::Waiting: title = "已保存，等待同步"; break;
    case SyncState::Running:
        title = "正在同步";
        switch (status.phase)
        {
        case SyncPhase::Saving: summary = "正在保存本地修改…"; break;
        case SyncPhase::Fetching: summary = "正在获取全部方案与里程碑…"; break;
        case SyncPhase::Analyzing: summary = "正在整理双方的更新…"; break;
        case SyncPhase::Applying: summary = "正在更新本机文件…"; break;
        case SyncPhase::Uploading: summary = "正在发送全部方案与里程碑…"; break;
        case SyncPhase::Idle: summary = "正在等待任务执行…"; break;
        } break;
    case SyncState::Synced: title = "已同步"; break;
    case SyncState::NeedsResolution: title = "需要处理差异"; summary = "双方改动有重叠，处理后继续同步。"; break;
    case SyncState::NeedsBranch: title = "需要处理另一个方案"; summary = QString("请先处理方案“%1”，再同步全部方案。").arg(status.branchRef.mid(11)); break;
    case SyncState::NeedsTags: title = "需要处理里程碑"; summary = "请在历史版本中处理同名里程碑。"; break;
    case SyncState::RetryLater: title = status.message.isEmpty() ? "暂未完成同步" : status.message; summary = status.automatic ? "本地版本已保留，稍后自动重试。" : "本地版本已保留，请稍后重试。"; break;
    case SyncState::NeedsAttention: title = status.message.isEmpty() ? "同步需要检查" : status.message; summary = "检查连接或访问权限。"; break;
    case SyncState::Editing: title = "等待保存编辑"; summary = "保存或放弃编辑后继续。"; break;
    }
    if (state == BackupSyncState::ResolutionPending || state == BackupSyncState::RemotePending) { title = "需要处理差异"; summary = "处理期间自动备份暂停，完成后继续。"; }
    if (state == BackupSyncState::NeedsAttention) { title = "需要检查备份"; summary = "自动写入已暂停，请先检查备份。"; }
    QString line = summary;
    if (!running && !status.detail.isEmpty())
    {
        if (status.state == SyncState::Synced && status.detail.contains('\n')) line = status.detail.section('\n', 1);
        else if (status.state != SyncState::Synced && status.state != SyncState::Waiting) line = status.detail;
    }
    if (line.isEmpty())
    {
        if (running) line = QStringLiteral("正在同步…");
        else if (connected) line = status.lastSuccess.isValid() ? "最近同步：" + status.lastSuccess.toLocalTime().toString("yyyy-MM-dd HH:mm:ss") : "尚未同步";
    }
    ui->stateLabel->setText(title);
    ui->lastSync->setText(line);
    ui->lastSync->setVisible(!line.isEmpty());
    ui->syncButton->setText(state == BackupSyncState::NeedsAttention ? "重新检查" : state == BackupSyncState::ResolutionPending || state == BackupSyncState::RemotePending ? "处理差异" : status.state == SyncState::NeedsBranch ? "切换方案并处理" : status.state == SyncState::NeedsTags ? "处理里程碑" : running ? "同步中…" : "立即同步");
    ui->syncButton->setAccessibleName(ui->syncButton->text() == "立即同步" ? "立即同步全部方案" : ui->syncButton->text());
    ui->syncButton->setEnabled(connected && !running && !m_connecting);
    setLoading(running || m_connecting);
    { const QSignalBlocker block(m_automatic); m_automatic->setChecked(status.automatic && connected); }
    m_automatic->setEnabled(connected && !m_connecting);
    if (!ui->remoteUrl->isModified()) ui->remoteUrl->setText(status.remoteUrl);
    ui->remoteUrl->setEnabled(!m_connecting);
    const auto connectText = m_connecting ? "连接中…" : connected ? "保存并连接" : "连接";
    ui->connectButton->setText(connectText);
    ui->connectButton->setToolTip(connectText);
    ui->connectButton->setAccessibleName(connectText);
    ui->connectButton->setEnabled(!m_connecting && !ui->remoteUrl->text().trimmed().isEmpty() && ui->remoteUrl->text().trimmed() != status.remoteUrl);
    ui->copyButton->setEnabled(connected); ui->disconnectButton->setEnabled(connected && !m_connecting);
}
void HomePageCollaborationPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    const bool narrow = width() < 640;
    ui->contentLayout->setContentsMargins(narrow ? 32 : 40, 12, narrow ? 16 : 24, 8);
    ui->statusRow->setDirection(narrow ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    ui->statusRow->setSpacing(narrow ? 8 : 24);
}
