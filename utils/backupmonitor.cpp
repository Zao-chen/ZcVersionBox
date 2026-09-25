#include "backupmonitor.h"
#include <QDir>
#include <algorithm>

BackupMonitor::BackupMonitor(BackupService *service, QObject *parent, std::function<qint64()> clock)
    : QObject(parent), m_service(service), m_clock(std::move(clock))
{
    m_elapsed.start();
    if (!m_clock)
        m_clock = [this]
        { return m_elapsed.elapsed(); };
    m_timer.setInterval(1500);
    connect(&m_timer, &QTimer::timeout, this, &BackupMonitor::scanNow);
    connect(service, &BackupService::trackedItemsChanged, this, &BackupMonitor::reconcile);
    connect(service, &BackupService::ready, this, [this]
            { reconcile(); watchCatalog(); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this]
            {
        if (!m_enabled || m_reloadPending) return;
        m_reloadPending = true;
        m_service->reload(this, [this](const OperationResult &result)
        {
            m_reloadPending = false;
            Q_UNUSED(result);
            watchCatalog();
        }); });
}
void BackupMonitor::watchCatalog()
{
    if (!m_enabled || !m_timer.isActive())
        return;
    // The writer lock lives in backupRoot. Watching it would schedule another
    // reload on every lock release, including releases caused by reload itself.
    for (const auto &path : {m_service->paths().backupRoot + "/items"})
        if (QDir(path).exists() && !m_watcher.directories().contains(path))
            m_watcher.addPath(path);
}
void BackupMonitor::start()
{
    m_enabled = true;
    reconcile();
    m_timer.start();
    watchCatalog();
    scanNow(); // Compare with the durable success baseline, including offline edits.
}
void BackupMonitor::stop()
{
    m_enabled = false;
    ++m_epoch;
    m_timer.stop();
    if (!m_watcher.directories().isEmpty())
        m_watcher.removePaths(m_watcher.directories());
    for (const auto &state : m_states)
    {
        if (state->busy)
            m_service->cancel(state->task);
        state->busy = false;
    }
}
void BackupMonitor::reconcile()
{
    QSet<QString> active;
    for (const auto &item : m_service->trackedItems())
    {
        active.insert(item.id);
        const auto generation = m_service->repositoryGeneration(item.id);
        if (m_states.contains(item.id) && m_states[item.id]->generation == generation)
            continue;
        auto state = std::make_shared<State>();
        state->generation = generation;
        m_states.insert(item.id, state);
    }
    for (const auto &id : m_states.keys())
        if (!active.contains(id))
            m_states.remove(id);
}
void BackupMonitor::completed(const std::shared_ptr<State> &state, const OperationResult &result)
{
    state->busy = false;
    if (result.success)
    {
        state->failures = 0;
        state->retryAt = 0;
        state->lastError.clear();
        if (!result.warning.isEmpty())
            emit notification(result);
        return;
    }
    ++state->failures;
    state->retryAt = m_clock() + qMin<qint64>(60000, 1500LL << qMin(state->failures - 1, 6));
    const auto error = result.title + result.message + result.warning;
    if (error != state->lastError)
        emit notification(result);
    state->lastError = error;
}
void BackupMonitor::scanNow()
{
    if (!m_enabled)
        return;
    if (!m_service->isReady())
    {
        if (!m_reloadPending)
        {
            m_reloadPending = true;
            m_service->reload(this, [this](const OperationResult &)
                              { m_reloadPending = false; watchCatalog(); });
        }
        return;
    }
    const auto epoch = m_epoch;
    for (auto it = m_states.cbegin(); it != m_states.cend(); ++it)
    {
        const auto id = it.key();
        const auto state = it.value();
        if (state->busy || state->retryAt > m_clock())
            continue;
        state->busy = true;
        state->task = m_service->observe(id, this, [this, id, state, epoch](const BackupResult<bool> &reply)
                                         {
            if (!m_enabled || epoch != m_epoch || m_states.value(id) != state) return;
            if (!reply.result.success || !reply.value || m_service->syncState(id) != BackupSyncState::Tracking)
            {
                completed(state, reply.result);
                return;
            }
            state->task = m_service->backup(id, this, [this, id, state, epoch](const OperationResult &result)
            {
                if (m_enabled && epoch == m_epoch && m_states.value(id) == state) completed(state, result);
            }, true); });
    }
}
