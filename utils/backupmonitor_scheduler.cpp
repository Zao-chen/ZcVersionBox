#include "backupmonitor_scheduler.h"
#include <QSet>
#include <algorithm>

BackupMonitorScheduler::BackupMonitorScheduler(Clock clock, BackupMonitorOptions options)
    : m_clock(std::move(clock)), m_options(options) {}

void BackupMonitorScheduler::reconcile(const QVector<BackupObservationTarget> &targets)
{
    QSet<QString> active;
    for (const auto &target : targets)
    {
        active.insert(target.id);
        auto it = m_entries.find(target.id);
        if (it != m_entries.end() && it->target.version == target.version &&
            it->target.generation == target.generation)
            continue;
        Entry entry;
        entry.target = target;
        entry.nextCheck = m_clock();
        entry.changes = it == m_entries.end() ? 1 : it->changes + 1;
        if (it == m_entries.end())
            m_order.append(target.id);
        m_entries.insert(target.id, std::move(entry));
    }
    for (const auto &id : m_entries.keys())
        if (!active.contains(id))
        {
            m_entries.remove(id);
            m_order.removeAll(id);
        }
}

void BackupMonitorScheduler::clear()
{
    m_entries.clear();
    m_order.clear();
    m_scan.reset();
    m_backup.reset();
    m_lastScan.clear();
    m_lastBackup.clear();
    // Never reuse tokens: an asynchronous completion may outlive a reset.
}

void BackupMonitorScheduler::markDirty(const QString &id)
{
    auto it = m_entries.find(id);
    if (it == m_entries.end())
        return;
    const auto now = m_clock();
    if (!it->dirty)
        it->firstChange = now;
    it->dirty = true;
    it->lastChange = now;
    ++it->changes;
}

void BackupMonitorScheduler::requestScan(const QString &id)
{
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
        if (id.isEmpty() || it.key() == id)
        {
            it->dirty = it->immediate = true;
            ++it->changes;
        }
}

bool BackupMonitorScheduler::occupied(const QString &id) const
{
    return (m_scan && m_scan->target.id == id) || (m_backup && m_backup->target.id == id);
}

qint64 BackupMonitorScheduler::due(const Entry &entry) const
{
    auto deadline = entry.nextCheck;
    if (entry.immediate)
        deadline = 0;
    else if (entry.dirty)
        deadline = std::min(deadline, std::min(entry.lastChange + m_options.quietPeriodMs,
                                               entry.firstChange + m_options.maximumCoalesceMs));
    return std::max(deadline, entry.retryAt);
}

QStringList BackupMonitorScheduler::orderedAfter(const QString &id) const
{
    const auto index = m_order.indexOf(id);
    return index < 0 ? m_order : m_order.mid(index + 1) + m_order.mid(0, index + 1);
}

std::optional<BackupScanRequest> BackupMonitorScheduler::takeScan()
{
    if (m_scan)
        return {};
    for (const auto &id : orderedAfter(m_lastScan))
    {
        auto &entry = m_entries[id];
        if (occupied(id) || entry.backup || due(entry) > m_clock())
            continue;
        m_scan = BackupScanRequest{entry.target, ++m_sequence, entry.changes};
        entry.dirty = entry.immediate = false;
        m_lastScan = id;
        return m_scan;
    }
    return {};
}

std::optional<BackupScanRequest> BackupMonitorScheduler::takeBackup()
{
    if (m_backup)
        return {};
    for (const auto &id : orderedAfter(m_lastBackup))
    {
        auto &entry = m_entries[id];
        if (!entry.backup || occupied(id) || entry.target.state != BackupSyncState::Tracking)
            continue;
        m_backup = entry.backup;
        entry.backup.reset();
        m_lastBackup = id;
        return m_backup;
    }
    return {};
}

bool BackupMonitorScheduler::sameRepository(const BackupScanRequest &request) const
{
    const auto it = m_entries.constFind(request.target.id);
    return it != m_entries.cend() && it->target.generation == request.target.generation &&
           it->target.sourcePath == request.target.sourcePath && it->target.directory == request.target.directory;
}

bool BackupMonitorScheduler::isCurrent(const BackupScanRequest &request) const
{
    return sameRepository(request) && m_entries[request.target.id].target.version == request.target.version;
}

void BackupMonitorScheduler::failed(Entry &entry)
{
    entry.failures = std::min(entry.failures + 1, 7);
    entry.retryAt = m_clock() + std::min(m_options.maximumRetryMs,
                                         m_options.initialRetryMs * (qint64(1) << (entry.failures - 1)));
    entry.dirty = entry.immediate = true;
}

bool BackupMonitorScheduler::completeScan(const BackupScanResult &result)
{
    if (!m_scan || m_scan->token != result.request.token)
        return false;
    m_scan.reset();
    if (!isCurrent(result.request))
        return false;
    auto &entry = m_entries[result.request.target.id];
    if (result.status == BackupScanStatus::Cancelled)
    {
        entry.dirty = entry.immediate = true;
        return false;
    }
    if (result.status == BackupScanStatus::Unavailable)
        failed(entry);
    else
    {
        entry.nextCheck = m_clock() + m_options.scanIntervalMs;
        // A successful scan must not reset retries for a failing backup.
        if (result.status == BackupScanStatus::Changed && entry.target.state == BackupSyncState::Tracking)
            entry.backup = result.request;
        else
        {
            entry.failures = 0;
            entry.retryAt = 0;
        }
    }
    return true;
}

bool BackupMonitorScheduler::completeBackup(const BackupScanRequest &request, const OperationResult &result)
{
    if (!m_backup || m_backup->token != request.token)
        return false;
    m_backup.reset();
    if (!sameRepository(request))
        return false;
    if (result.cancelled)
    {
        if (isCurrent(request))
        {
            auto &entry = m_entries[request.target.id];
            entry.dirty = entry.immediate = true;
        }
        return false;
    }
    // publish() precedes the completion. A successful write (or a durable pause)
    // changes the snapshot; do not overwrite the freshly reconciled entry.
    if (!isCurrent(request))
        return true;
    auto &entry = m_entries[request.target.id];
    if (result.success)
    {
        entry.failures = 0;
        entry.retryAt = 0;
        entry.nextCheck = m_clock() + m_options.scanIntervalMs;
    }
    else if (entry.target.state == BackupSyncState::Tracking)
        failed(entry);
    return true;
}

qint64 BackupMonitorScheduler::nextDeadline() const
{
    qint64 deadline = -1;
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it)
    {
        if (occupied(it.key()))
            continue;
        if (it->backup)
        {
            if (!m_backup)
                return m_clock();
        }
        else if (!m_scan)
            deadline = deadline < 0 ? due(*it) : std::min(deadline, due(*it));
    }
    return deadline;
}

BackupMonitorScheduler::Phase BackupMonitorScheduler::phase(const QString &id) const
{
    const auto it = m_entries.constFind(id);
    if (it == m_entries.cend())
        return Phase::Idle;
    if (it->target.state != BackupSyncState::Tracking)
        return Phase::Paused;
    if (m_scan && m_scan->target.id == id)
        return Phase::Scanning;
    if (m_backup && m_backup->target.id == id)
        return Phase::BackingUp;
    if (it->backup)
        return Phase::AwaitingBackup;
    if (it->retryAt > m_clock())
        return Phase::BackingOff;
    return it->dirty ? Phase::Debouncing : Phase::Idle;
}

bool BackupMonitorScheduler::hasWork() const
{
    if (m_scan || m_backup)
        return true;
    for (const auto &entry : m_entries)
        if (entry.backup || due(entry) <= m_clock())
            return true;
    return false;
}
