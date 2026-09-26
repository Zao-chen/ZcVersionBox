#include "backupmonitor.h"
#include <algorithm>
#include <limits>

BackupMonitor::BackupMonitor(BackupService *service, QObject *parent, BackupMonitorOptions options,
                             BackupMonitorDependencies dependencies)
    : QObject(parent), m_service(service), m_options(options),
      m_clock(dependencies.clock ? std::move(dependencies.clock) : BackupMonitorScheduler::Clock([this]
                                                                                                 { return m_elapsed.elapsed(); })),
      m_scheduler(m_clock, options), m_sources(this, options.watchBatchSize, std::move(dependencies.addWatchPaths)),
      m_scanner(this, std::move(dependencies.scanFiles)), m_catalog(service->paths().backupRoot, this)
{
    m_elapsed.start();
    m_wakeup.setSingleShot(true);
    m_wakeup.setTimerType(Qt::PreciseTimer);
    connect(&m_wakeup, &QTimer::timeout, this, &BackupMonitor::pump);
    connect(service, &BackupService::trackedItemsChanged, this, &BackupMonitor::reconcile);
    connect(service, &BackupService::ready, this, &BackupMonitor::reconcile);
    connect(service, &BackupService::reloadFinished, this, [this](const OperationResult &result)
            {
        if (m_state == State::Running && !m_reloadTask)
        {
            audited(result, false); // The service owns notifications for other callers.
            reconcile();
        } });
    connect(service, &QObject::destroyed, this, [this]
            {
        m_service.clear();
        m_backupTask = m_reloadTask = 0;
        stop(); });
    const auto dirty = [this](const QString &id)
    {
        if (m_state != State::Running)
            return;
        m_scheduler.markDirty(id);
        armTimer();
    };
    connect(&m_sources, &BackupSourceWatcher::sourceChanged, this, dirty);
    connect(&m_sources, &BackupSourceWatcher::coverageEstablished, this, [this, dirty](const QString &id)
            { m_errors.remove("watch:" + id); dirty(id); });
    connect(&m_sources, &BackupSourceWatcher::registrationFailed, this, [this](const QString &id, const QString &path)
            {
        if (m_state == State::Running)
            report("watch:" + id, OperationResult::warn("部分文件监听不可用", "将继续通过周期校验检查变化：" + path)); });
    connect(&m_scanner, &BackupSourceScanner::finished, this, &BackupMonitor::scanned);
    connect(&m_catalog, &BackupCatalogWatcher::changed, this, &BackupMonitor::catalogChanged);
    connect(&m_catalog, &BackupCatalogWatcher::registrationFailed, this, [this](const QString &path)
            {
        if (m_state == State::Running)
            report("catalog-watch", OperationResult::warn("备份清单监听不可用", "将继续通过周期审计检查变化：" + path)); });
}

BackupMonitor::~BackupMonitor()
{
    m_destroying = true;
    stop();
}

void BackupMonitor::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged(state);
}

void BackupMonitor::start()
{
    if (m_destroying || !m_service || m_state == State::Running)
        return;
    if (m_state == State::Stopping)
    {
        m_restartRequested = true;
        return;
    }
    ++m_epoch;
    m_scheduler.clear();
    m_errors.clear();
    m_auditFailures = 0;
    m_auditRetryAt = 0;
    m_catalogDirty = false;
    m_nextAudit = m_clock();
    setState(State::Running);
    if (m_state != State::Running)
        return;
    m_catalog.start();
    reconcile();
}

void BackupMonitor::stop()
{
    m_restartRequested = false;
    if (m_state == State::Stopped)
        return;
    if (m_state == State::Running)
    {
        ++m_epoch;
        setState(State::Stopping);
        m_wakeup.stop();
        m_sources.clear();
        m_catalog.stop();
        m_scheduler.clear();
        m_scanner.cancel();
        if (m_service)
        {
            if (m_backupTask)
                m_service->cancel(m_backupTask);
            if (m_reloadTask)
                m_service->cancel(m_reloadTask);
        }
    }
    finishStopping();
}

void BackupMonitor::finishStopping()
{
    if (m_state != State::Stopping || m_scanner.isBusy() || m_backupTask || m_reloadTask)
        return;
    m_scanRequest.reset();
    m_backupRequest.reset();
    setState(State::Stopped);
    if (m_restartRequested)
    {
        m_restartRequested = false;
        start();
    }
}

bool BackupMonitor::isIdle() const
{
    return !m_scanner.isBusy() && !m_backupTask && !m_reloadTask && !m_sources.isRegistering() && !m_scheduler.hasWork();
}

void BackupMonitor::reconcile()
{
    if (m_state != State::Running || !m_service)
        return;
    const auto targets = m_service->observationTargets();
    m_scheduler.reconcile(targets);
    m_sources.setTargets(targets);
    m_catalog.setTargets(targets);
    if (m_scanRequest && !m_scheduler.isCurrent(*m_scanRequest))
        m_scanner.cancel();
    if (m_backupRequest && (!m_scheduler.sameRepository(*m_backupRequest) ||
                            m_service->syncState(m_backupRequest->target.id) != BackupSyncState::Tracking))
        m_service->cancel(m_backupTask);
    pump();
}

void BackupMonitor::scanNow()
{
    if (m_state != State::Running)
        return;
    m_scheduler.requestScan();
    pump();
}

void BackupMonitor::pump()
{
    if (m_state != State::Running || !m_service)
        return;
    if (!m_reloadTask && !m_service->isReloading() && auditDeadline() <= m_clock())
    {
        m_catalogDirty = false;
        const auto epoch = m_epoch;
        m_reloadTask = m_service->reload(this, [this, epoch](const OperationResult &result)
                                         {
            m_reloadTask = 0;
            if (m_state == State::Running && epoch == m_epoch)
            {
                audited(result, true);
                reconcile();
            }
            else
                finishStopping(); }, {BackupTaskPriority::Background, false});
    }
    if (m_service->isReady())
    {
        if (!m_backupTask)
            if (const auto request = m_scheduler.takeBackup())
            {
                m_backupRequest = request;
                const auto epoch = m_epoch;
                m_backupTask = m_service->backup(request->target.id, this, [this, request = *request, epoch](const OperationResult &result)
                                                 {
                    m_backupTask = 0;
                    m_backupRequest.reset();
                    if (m_state == State::Running && epoch == m_epoch)
                    {
                        if (m_scheduler.completeBackup(request, result))
                            report(request.target.id, result);
                        pump();
                    }
                    else
                        finishStopping(); }, {true, BackupTaskPriority::Background});
                emit backupRequested(request->target.id);
            }
        if (!m_scanner.isBusy())
            if (const auto request = m_scheduler.takeScan())
            {
                m_scanRequest = request;
                m_scanner.scan(*request);
                emit scanStarted(request->target.id);
            }
    }
    armTimer();
}

void BackupMonitor::scanned(const BackupScanResult &reply)
{
    m_scanRequest.reset();
    if (m_state != State::Running)
    {
        finishStopping();
        return;
    }
    if (m_scheduler.completeScan(reply))
    {
        m_sources.updatePaths(reply.request.target.id, reply.watchPaths, reply.status != BackupScanStatus::Unavailable);
        if (reply.request.target.state == BackupSyncState::Tracking && reply.status != BackupScanStatus::Changed)
            report(reply.request.target.id, reply.result);
    }
    pump();
}

void BackupMonitor::catalogChanged()
{
    if (m_state != State::Running)
        return;
    if (!m_catalogDirty)
        m_firstCatalogChange = m_clock();
    m_lastCatalogChange = m_clock();
    m_catalogDirty = true;
    armTimer();
}

qint64 BackupMonitor::auditDeadline() const
{
    auto deadline = m_nextAudit;
    if (m_catalogDirty)
        deadline = std::min(deadline, std::min(m_lastCatalogChange + m_options.quietPeriodMs,
                                               m_firstCatalogChange + m_options.maximumCoalesceMs));
    return std::max(deadline, m_auditRetryAt);
}

void BackupMonitor::audited(const OperationResult &result, bool notify)
{
    if (result.cancelled)
    {
        m_nextAudit = m_clock() + m_options.auditIntervalMs;
        return;
    }
    if (result.success)
    {
        m_auditFailures = 0;
        m_auditRetryAt = 0;
        m_nextAudit = m_clock() + m_options.auditIntervalMs;
    }
    else
    {
        m_auditFailures = std::min(m_auditFailures + 1, 7);
        m_auditRetryAt = m_clock() + std::min(m_options.maximumRetryMs,
                                              m_options.initialRetryMs * (qint64(1) << (m_auditFailures - 1)));
        m_nextAudit = m_auditRetryAt;
    }
    report("catalog", result, notify);
}

void BackupMonitor::report(const QString &id, const OperationResult &result, bool notify)
{
    if (result.cancelled)
        return;
    if (result.success && result.warning.isEmpty())
    {
        m_errors.remove(id);
        return;
    }
    const QStringList key{result.title, result.message, result.warning, result.path, QString::number(int(result.level))};
    if (notify && m_errors.value(id) != key)
        emit notification(result);
    m_errors[id] = key;
}

void BackupMonitor::armTimer()
{
    if (m_state != State::Running || !m_service)
        return;
    qint64 deadline = m_service->isReady() ? m_scheduler.nextDeadline() : -1;
    if (!m_reloadTask && !m_service->isReloading())
        deadline = deadline < 0 ? auditDeadline() : std::min(deadline, auditDeadline());
    if (deadline < 0)
        m_wakeup.stop();
    else
        m_wakeup.start(int(std::clamp<qint64>(deadline - m_clock(), 0, std::numeric_limits<int>::max())));
}
