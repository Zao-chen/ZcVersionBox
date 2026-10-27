#include "syncscheduler.h"
#include <algorithm>
#include <limits>

SyncScheduler::SyncScheduler(BackupService *service, QObject *parent, SyncSchedulerOptions options, Clock clock)
    : QObject(parent), m_service(service), m_options(options), m_clock(std::move(clock))
{
    m_elapsed.start();
    if (!m_clock) m_clock = [this] { return m_elapsed.elapsed(); };
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &SyncScheduler::tick);
    connect(service, &BackupService::ready, this, &SyncScheduler::reconcile);
    connect(service, &BackupService::trackedItemsChanged, this, &SyncScheduler::reconcile);
    connect(service, &BackupService::syncStatusChanged, this, &SyncScheduler::reconcile);
    connect(service, &BackupService::localVersionsChanged, this, &SyncScheduler::changed);
    connect(service, &BackupService::syncEditingChanged, this, [this](const QString &id) {
        if (m_entries.contains(id) && !m_service->isSyncEditing(id)) m_entries[id].due = m_clock() + m_options.quietPeriodMs;
        arm();
    });
    connect(service, &QObject::destroyed, this, [this] { m_service.clear(); m_task = 0; stop(); });
}
SyncScheduler::~SyncScheduler() { stop(); }
void SyncScheduler::start()
{
    if (!m_service || m_state == State::Running) return;
    if (m_state == State::Stopping) { m_restart = true; return; }
    m_state = State::Running; reconcile();
}
void SyncScheduler::stop()
{
    m_restart = false;
    if (m_state == State::Stopped) return;
    m_state = State::Stopping; m_timer.stop(); m_entries.clear();
    if (m_task && m_service) m_service->cancel(m_task);
    finishStop();
}
void SyncScheduler::finishStop()
{
    if (m_state != State::Stopping || m_task) return;
    m_state = State::Stopped;
    if (m_restart) { m_restart = false; start(); }
}
bool SyncScheduler::eligible(const QString &id) const
{
    if (!m_service || m_service->isSynchronizing(id) || m_service->isSyncEditing(id) || m_service->syncState(id) != BackupSyncState::Tracking) return false;
    const auto status = m_service->syncStatus(id);
    if (status.state == SyncState::NeedsResolution || status.state == SyncState::NeedsTags || status.state == SyncState::NeedsAttention) return false;
    if (status.state == SyncState::NeedsBranch && status.branchRef != m_service->branchContext(id).ref) return false;
    return status.automatic && !status.remoteUrl.isEmpty();
}
void SyncScheduler::reconcile()
{
    if (m_state != State::Running || !m_service || !m_service->isReady()) return;
    QSet<QString> ids;
    for (const auto &item : m_service->trackedItems())
    {
        const auto status = m_service->syncStatus(item.id);
        if (!status.automatic || status.remoteUrl.isEmpty()) continue;
        ids.insert(item.id);
        const auto generation = m_service->repositoryGeneration(item.id);
        if (!m_entries.contains(item.id) || m_entries[item.id].generation != generation || m_entries[item.id].remote != status.remoteUrl)
        {
            if (m_activeId == item.id && m_task) m_service->cancel(m_task);
            m_entries[item.id] = {generation, ++m_sequence, 0, status.remoteUrl, {}, m_clock(), 0, status.state};
        }
        auto &entry = m_entries[item.id];
        if (status.state == SyncState::Synced && entry.lastState != SyncState::Synced && m_activeId != item.id)
        { entry.due = m_clock() + m_options.checkIntervalMs; entry.failures = 0; entry.lastError.clear(); }
        entry.lastState = status.state;
    }
    for (auto it = m_entries.begin(); it != m_entries.end();)
        if (!ids.contains(it.key()))
        {
            if (m_activeId == it.key() && m_task) m_service->cancel(m_task);
            it = m_entries.erase(it);
        }
        else ++it;
    arm();
}
void SyncScheduler::changed(const QString &id)
{
    if (m_state != State::Running || !m_entries.contains(id)) return;
    auto &entry = m_entries[id]; ++entry.changes;
    const auto quiet = m_clock() + m_options.quietPeriodMs;
    entry.due = entry.failures ? qMax(entry.due, quiet) : quiet;
    arm();
}
void SyncScheduler::arm()
{
    m_timer.stop();
    if (m_state != State::Running || m_task || m_dispatching || !m_service) return;
    qint64 next = std::numeric_limits<qint64>::max();
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it)
        if (eligible(it.key())) next = qMin(next, it->due);
    if (next != std::numeric_limits<qint64>::max()) m_timer.start(int(qBound<qint64>(qint64(1), next - m_clock(), qint64(std::numeric_limits<int>::max()))));
}
void SyncScheduler::tick()
{
    if (m_state != State::Running || !m_service || m_task || m_dispatching) return;
    QString id;
    qint64 earliest = std::numeric_limits<qint64>::max();
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it)
        if (eligible(it.key()) && it->due <= m_clock() && it->due < earliest) { id = it.key(); earliest = it->due; }
    if (id.isEmpty()) { arm(); return; }
    const auto captured = m_entries.value(id);
    m_activeId = id; m_dispatching = true;
    emit syncRequested(id);
    if (m_state != State::Running) { m_dispatching = false; return; }
    m_task = m_service->synchronize(id, {SyncTrigger::Automatic, BackupTaskPriority::Background}, this,
        [this, id, captured](const BackupResult<SyncResult> &reply)
    {
        m_task = 0; m_activeId.clear();
        if (m_state == State::Stopping) { finishStop(); return; }
        if (m_state != State::Running || !m_entries.contains(id) || m_entries[id].token != captured.token) { arm(); return; }
        auto &entry = m_entries[id];
        if (reply.value.status.state == SyncState::RetryLater)
        {
            entry.failures = qMin(entry.failures + 1, 16);
            entry.due = m_clock() + qMin(m_options.maximumRetryMs, m_options.initialRetryMs * (qint64(1) << (entry.failures - 1)));
        }
        else
        {
            entry.failures = 0;
            entry.due = m_clock() + (entry.changes != captured.changes || reply.result.cancelled ? m_options.quietPeriodMs : m_options.checkIntervalMs);
        }
        const auto state = reply.value.status.state;
        if (state == SyncState::Synced)
        {
            entry.lastError.clear();
            if (!reply.result.warning.isEmpty())
            {
                auto notice = OperationResult::info("方案已同步，请留意", reply.result.warning);
                notice.level = NotificationLevel::Warning;
                for (const auto &item : m_service->trackedItems()) if (item.id == id) notice.title = item.name + "：" + notice.title;
                emit notification(notice);
            }
        }
        else if (!reply.result.cancelled && state != SyncState::Editing && state != SyncState::Waiting)
        {
            const auto key = QString::number(int(state)) + ':' + reply.result.message;
            if (entry.lastError != key)
            {
                entry.lastError = key;
                auto notice = reply.result;
                for (const auto &item : m_service->trackedItems()) if (item.id == id) notice.title = item.name + "：" + notice.title;
                emit notification(notice);
            }
        }
        arm();
    });
    m_dispatching = false;
}
