#include "backupservice.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRunnable>
#include <QStandardPaths>
#include <QUuid>

namespace Backup
{
namespace
{
QString channelFor(const QString &operation)
{
    if (QStringList{"history", "diff", "export", "status", "remoteHead", "resolveConflict"}.contains(operation))
        return {};
    if (operation == "annotateAi")
        return "ai";
    if (operation == "pull" || operation == "push" || operation == "resetRemote" || operation == "setRemote")
        return "sync";
    return "backup";
}
bool needsMonitor(const QString &operation)
{
    return QStringList{"snapshot", "restore", "import", "pull", "push", "resetRemote", "rebuild"}.contains(operation);
}
QString failureKey(const Request &request) { return request.operation + '/' + request.revision; }
QString titleFor(const QString &operation)
{
    const QMap<QString, QString> titles{{"snapshot", "备份"}, {"restore", "恢复"}, {"pull", "拉取"}, {"push", "推送"}, {"resetRemote", "覆盖远端"}, {"setRemote", "保存云端地址"}, {"remove", "删除备份"}, {"rebuild", "重建"}, {"import", "导入"}, {"annotate", "保存备注"}, {"annotateAi", "保存 AI 备注"}};
    return titles.value(operation, operation);
}
} // namespace
BackupService::BackupService(QString root, QObject *parent, MonitorFactory factory, EngineFactory engines) : QObject(parent), m_root(std::move(root)), m_factory(std::move(factory)), m_engineFactory(std::move(engines))
{
    qRegisterMetaType<TaskResult>();
    m_pool.setMaxThreadCount(2);
}
BackupService::~BackupService()
{
    shutdown();
    m_pool.waitForDone();
}
void BackupService::start()
{
    try
    {
        const auto marker = QDir(m_root).filePath("engine.json");
        if (QFileInfo::exists(marker))
        {
            const auto format = readJson(marker);
            if (format["format"] != "zcversionbox-engine" || format["version"].toInt() != 1)
                throw Error(ErrorCode::InvalidFormat, "不支持的备份引擎数据格式");
        }
        else
        {
            const auto entries = QDir(m_root).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
            for (const auto &name : entries)
                if (name != "instance.lock" && name != "settings.ini")
                    throw Error(ErrorCode::InvalidFormat, "数据目录不是空目录，且缺少新版引擎标识");
            writeJson(marker, {{"format", "zcversionbox-engine"}, {"version", 1}});
        }
        if (!QDir().mkpath(QDir(m_root).filePath("objects")))
            throw Error(ErrorCode::Permission, "无法创建备份数据目录");
        const QDir objects(QDir(m_root).filePath("objects"));
        QList<Tracking> loaded;
        for (const auto &id : objects.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        {
            if (QUuid(id).isNull())
                throw Error(ErrorCode::InvalidFormat, "数据目录包含未知对象：" + id);
            const auto tracking = Tracking::parse(readJson(objects.filePath(id + "/config.json")));
            if (tracking.id != id)
                throw Error(ErrorCode::InvalidFormat, "备份对象标识不一致");
            loaded.append(tracking);
        }
        for (const auto &tracking : loaded)
            attach(tracking);
        m_started = true;
        QStringList cleanupPaths;
        for (const auto &area : {QString("previews"), QString("trash")})
        {
            QDir dir(QDir(m_root).filePath(area));
            for (const auto &name : dir.entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot))
                cleanupPaths.append(dir.filePath(name));
        }
        m_pool.start(QRunnable::create([this, cleanupPaths]
                                       {
                                           for (const auto &path : cleanupPaths)
                                           {
                                               try
                                               {
                                                   removeOwnedPath(path);
                                               }
                                               catch (const Error &e)
                                               {
                                                   QMetaObject::invokeMethod(this, [this, message = e.message]
                                                                             {
                                                                                 emit startupFailed("临时数据清理失败，文件已保留：" + message);
                                                                             },
                                                                             Qt::QueuedConnection);
                                               }
                                           }
                                       }));
        emit trackingChanged();
        dispatch();
    }
    catch (const Error &e)
    {
        emit startupFailed(e.message);
    }
}
QList<Tracking> BackupService::trackings() const
{
    QList<Tracking> rows;
    for (const auto &entry : m_entries)
        rows.append(entry->tracking);
    return rows;
}
QString BackupService::track(const QString &source, const QString &remote, bool importing)
{
    if (m_stopping)
        throw Error(ErrorCode::Cancelled, "应用正在退出");
    if (!m_started)
        throw Error(ErrorCode::RecoveryRequired, "备份引擎尚未成功加载");
    auto normalized = normalizedPath(source);
    if (QFileInfo::exists(normalized) || QFileInfo(normalized).isSymLink())
    {
        fileIdentity(normalized);
        normalized = QFileInfo(normalized).canonicalFilePath();
    }
    else
    {
        const auto parent = QFileInfo(QFileInfo(normalized).absolutePath()).canonicalFilePath();
        if (parent.isEmpty())
            throw Error(ErrorCode::InvalidFormat, "请选择存在的父目录");
        normalized = QDir(parent).filePath(QFileInfo(normalized).fileName());
    }
    if (QFileInfo(normalized).fileName().isEmpty() || !QFileInfo(QFileInfo(normalized).absolutePath()).isDir())
        throw Error(ErrorCode::InvalidFormat, "请选择存在父目录的有效路径");
    if (containsPath(m_root, normalized))
        throw Error(ErrorCode::Unsupported, "不能追踪备份引擎的数据目录");
    for (const auto &entry : m_entries)
        if (entry->tracking.source == normalized)
            throw Error(ErrorCode::Conflict, "此路径已在追踪列表中");
    if (!importing && !QFileInfo::exists(normalized))
        throw Error(ErrorCode::SourceUnavailable, "源路径不存在");
    Tracking tracking{newId(), normalized, remote, importing};
    writeJson(QDir(m_root).filePath("objects/" + tracking.id + "/config.json"), tracking.json());
    attach(tracking);
    emit trackingChanged();
    return tracking.id;
}
void BackupService::attach(const Tracking &tracking)
{
    auto entry = std::make_shared<Entry>();
    entry->tracking = tracking;
    entry->engine = m_engineFactory ? m_engineFactory(m_root, tracking) : std::make_shared<Engine>(m_root, tracking);
    entry->monitor = m_factory(tracking.source, this);
    entry->debounce = new QTimer(this);
    entry->deadline = new QTimer(this);
    entry->retry = new QTimer(this);
    for (auto timer : {entry->debounce, entry->deadline, entry->retry})
        timer->setSingleShot(true);
    entry->debounce->setInterval(2000);
    entry->deadline->setInterval(30000);
    const auto id = tracking.id;
    connect(entry->debounce, &QTimer::timeout, this, [this, id]
            {
                enqueueSnapshot(id);
            });
    connect(entry->deadline, &QTimer::timeout, this, [this, id]
            {
                enqueueSnapshot(id);
            });
    connect(entry->retry, &QTimer::timeout, this, [this, id]
            {
                retryDue(id);
            });
    connect(entry->monitor, &SourceMonitor::ready, this, [this, id]
            {
                if (m_stopping || !m_entries.contains(id))
                    return;
                auto entry = m_entries[id];
                entry->ready = true;
                entry->monitorError.clear();
                enqueueInitial(id);
                armRetries(id);
                dispatch();
            });
    connect(entry->monitor, &SourceMonitor::changed, this, [this, id](const QStringList &paths, bool lost)
            {
                if (m_stopping || !m_entries.contains(id))
                    return;
                auto entry = m_entries[id];
                if (entry->maintenance)
                    return;
                const SourceIndex policy({m_root, QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath("ZcVersionBox")});
                for (const auto &path : paths)
                    if (!policy.excluded(path))
                        entry->dirty.insert(path);
                if (entry->dirty.isEmpty() && !lost)
                    return;
                entry->full |= lost;
                if (lost)
                    emit stateChanged(id, "backup", "事件丢失，正在重新校验");
                entry->debounce->start();
                if (!entry->deadline->isActive())
                    entry->deadline->start();
            });
    connect(entry->monitor, &SourceMonitor::failed, this, [this, id](const QString &message)
            {
                if (!m_entries.contains(id))
                    return;
                auto entry = m_entries[id];
                entry->ready = false;
                cancelTasks(id, !entry->initializing && entry->activeOperation != "recover");
                entry->monitorError = "监听失败：" + message;
                emit stateChanged(id, "backup", entry->monitorError);
                TaskResult result;
                result.success = false;
                result.code = ErrorCode::Monitor;
                result.trackingId = id;
                result.message = message;
                result.operation = "monitor";
                emit taskFinished(result);
            });
    m_entries.insert(id, entry);
    entry->monitor->start();
    Request request;
    request.trackingId = id;
    request.operation = "initialize";
    submit(request);
}
QString BackupService::submit(Request request)
{
    if (m_stopping || !m_entries.contains(request.trackingId))
        return {};
    if (request.requestId.isEmpty())
        request.requestId = newId();
    if (request.operation == "remove" || request.operation == "rebuild")
    {
        m_entries[request.trackingId]->maintenance = true;
        emit trackingResetRequested(request.trackingId);
        const auto entry = m_entries[request.trackingId];
        cancelTasks(request.trackingId, !entry->initializing && entry->activeOperation != "recover");
    }
    m_queue.enqueue(request);
    dispatch();
    return request.requestId;
}
void BackupService::enqueueSnapshot(const QString &id)
{
    if (m_stopping || !m_entries.contains(id))
        return;
    auto entry = m_entries[id];
    entry->debounce->stop();
    entry->deadline->stop();
    if (!entry->ready || entry->initializing || entry->recoveryBlocked || entry->maintenance || entry->tracking.pendingImport)
        return;
    for (const auto &queued : m_queue)
        if (queued.trackingId == id && queued.operation == "snapshot")
            return;
    Request r;
    r.trackingId = id;
    r.operation = "snapshot";
    submit(r);
}
void BackupService::enqueueInitial(const QString &id)
{
    const auto entry = m_entries.value(id);
    if (!entry || !entry->ready || entry->initializing || entry->recoveryBlocked || entry->maintenance || m_stopping)
        return;
    const auto operation = entry->tracking.pendingImport ? QString("import") : QString("snapshot");
    for (const auto &queued : m_queue)
        if (queued.trackingId == id && queued.operation == operation)
            return;
    Request request;
    request.requestId = newId();
    request.trackingId = id;
    request.operation = operation;
    request.fullScan = true;
    m_queue.enqueue(request);
}
void BackupService::dispatch()
{
    if (m_stopping || !m_started)
        return;
    int busy = 0;
    for (const auto &entry : m_entries)
        if (entry->busy)
            ++busy;
    for (int i = 0; i < m_queue.size() && busy < 2;)
    {
        auto request = m_queue[i];
        auto entry = m_entries.value(request.trackingId);
        if (!entry)
        {
            m_queue.removeAt(i);
            continue;
        }
        bool blocked = entry->busy;
        const bool recovery = request.operation == "initialize" || request.operation == "recover";
        for (const auto &other : m_entries)
            if (containsPath(entry->tracking.source, other->tracking.source) || containsPath(other->tracking.source, entry->tracking.source))
                blocked |= other->busy || (!recovery && (other->initializing || other->recoveryBlocked));
        if ((!entry->ready && needsMonitor(request.operation)) || (entry->maintenance && !recovery && request.operation != "remove" && request.operation != "rebuild"))
            blocked = true;
        if (blocked)
        {
            ++i;
            continue;
        }
        m_queue.removeAt(i);
        ++busy;
        entry->busy = true;
        entry->activeOperation = request.operation;
        entry->cancellation = std::make_shared<std::atomic_bool>(false);
        if (request.operation == "snapshot")
        {
            request.fullScan |= entry->full;
            request.dirtyPaths += entry->dirty.values();
            entry->dirty.clear();
            entry->full = false;
        }
        const auto channel = channelFor(request.operation);
        if (!channel.isEmpty())
            emit stateChanged(request.trackingId, channel, "正在" + titleFor(request.operation));
        m_pool.start(QRunnable::create([this, entry, request]
                                       {
                                           TaskResult result;
                                           try
                                           {
                                               result = entry->engine->execute(request, entry->cancellation);
                                           }
                                           catch (const std::exception &e)
                                           {
                                               result.success = false;
                                               result.code = ErrorCode::RecoveryRequired;
                                               result.message = QString::fromUtf8(e.what());
                                               result.trackingId = request.trackingId;
                                               result.requestId = request.requestId;
                                               result.operation = request.operation;
                                           }
                                           QMetaObject::invokeMethod(this, [this, request, result]
                                                                     {
                                                                         finish(request, result);
                                                                     },
                                                                     Qt::QueuedConnection);
                                       }));
    }
}
void BackupService::finish(const Request &request, TaskResult result)
{
    if (!result.conflict.category.isEmpty())
        m_conflicts[request.trackingId] = result.conflict;
    else if (result.success && QStringList{"pull", "push", "resetRemote", "remove", "setRemote"}.contains(request.operation))
        m_conflicts.remove(request.trackingId);
    auto entry = m_entries.value(request.trackingId);
    if (!entry)
        return;
    entry->busy = false;
    entry->activeOperation.clear();
    const bool initializing = request.operation == "initialize";
    const bool recovering = request.operation == "recover" || initializing;
    const bool wasBlocked = entry->recoveryBlocked || entry->initializing;
    if (initializing)
        entry->initializing = false;
    if (recovering)
        entry->recoveryBlocked = !result.success;
    if (result.data["removed"].toBool())
    {
        cancel(request.trackingId);
        entry->monitor->stop();
        entry->monitor->deleteLater();
        for (auto timer : {entry->debounce, entry->deadline, entry->retry})
            timer->deleteLater();
        m_entries.remove(request.trackingId);
        emit trackingChanged();
    }
    else
    {
        if (request.operation == "rebuild" || request.operation == "remove")
            entry->maintenance = false;
        entry->tracking = entry->engine->tracking();
        const auto channel = channelFor(request.operation);
        if (!initializing && !channel.isEmpty())
        {
            const auto state = channel == "backup" && !entry->monitorError.isEmpty() ? entry->monitorError : result.success ? titleFor(request.operation) + "已完成"
                                                                                                                            : result.message;
            emit stateChanged(request.trackingId, channel, state);
        }
        const bool recoveryNeeded = result.data["recoveryRequired"].toBool() || result.code == ErrorCode::RecoveryRequired || (initializing && !result.success);
        if (recoveryNeeded)
        {
            entry->recoveryBlocked = true;
            auto recovery = request;
            recovery.operation = "recover";
            recovery.revision.clear();
            auto &failure = entry->failures[failureKey(recovery)];
            failure.request = recovery;
            const int delays[] = {5000, 15000, 60000};
            const bool retryable = result.data["recoveryRetryable"].toBool() || (initializing && result.retryable);
            failure.due = retryable ? QDateTime::currentMSecsSinceEpoch() + delays[qMin(failure.attempts++, 2)] : -1;
            emit stateChanged(request.trackingId, "backup", result.message);
        }
        else if (!result.success && result.code != ErrorCode::Cancelled && !channel.isEmpty() && request.operation != "annotateAi")
        {
            auto &failure = entry->failures[failureKey(request)];
            failure.request = request;
            entry->full = true;
            const int delays[] = {5000, 15000, 60000};
            failure.due = result.retryable ? QDateTime::currentMSecsSinceEpoch() + delays[qMin(failure.attempts++, 2)] : -1;
            if (result.retryable)
                emit stateChanged(request.trackingId, channel, "等待重试：" + result.message);
        }
        else if (result.success)
            entry->failures.remove(failureKey(request));
        if (result.success && result.changed && !result.revision.isEmpty() && !entry->maintenance && QStringList{"snapshot", "restore", "rebuild", "push"}.contains(request.operation))
            emit snapshotCommitted(request.trackingId, result.revision);
        if (request.operation == "setRemote")
            emit trackingChanged();
        if (recovering && result.success && wasBlocked)
            enqueueInitial(request.trackingId);
        if (!entry->dirty.isEmpty() && !entry->debounce->isActive() && !entry->recoveryBlocked && !entry->maintenance)
            entry->debounce->start();
        armRetries(request.trackingId);
    }
    if (!initializing || !result.success)
        emit taskFinished(result);
    if (m_stopping)
    {
        bool running = false;
        for (const auto &e : m_entries)
            running |= e->busy;
        if (!running)
            emit stopped();
    }
    else
        dispatch();
}
void BackupService::armRetries(const QString &id)
{
    auto entry = m_entries.value(id);
    if (!entry)
        return;
    entry->retry->stop();
    if (m_stopping || entry->initializing || entry->maintenance)
        return;
    qint64 next = -1;
    for (const auto &failure : entry->failures)
    {
        if (failure.due < 0 || (!entry->ready && needsMonitor(failure.request.operation)) ||
            (entry->recoveryBlocked && failure.request.operation != "recover"))
            continue;
        if (next < 0 || failure.due < next)
            next = failure.due;
    }
    if (next >= 0)
        entry->retry->start(int(qMax<qint64>(1, next - QDateTime::currentMSecsSinceEpoch())));
}
void BackupService::retryDue(const QString &id)
{
    auto entry = m_entries.value(id);
    if (!entry || m_stopping)
        return;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    for (auto &failure : entry->failures)
    {
        if (failure.due < 0 || failure.due > now || (!entry->ready && needsMonitor(failure.request.operation)) ||
            (entry->recoveryBlocked && failure.request.operation != "recover"))
            continue;
        auto request = failure.request;
        failure.due = -1;
        request.requestId = newId();
        request.fullScan = true;
        if (request.operation == "rebuild" || request.operation == "remove")
            entry->maintenance = true;
        m_queue.enqueue(request);
    }
    armRetries(id);
    dispatch();
}
void BackupService::cancel(const QString &id)
{
    cancelTasks(id, true);
}
void BackupService::cancelTasks(const QString &id, bool cancelActive)
{
    auto entry = m_entries.value(id);
    if (!entry)
        return;
    if (cancelActive && entry->cancellation)
        entry->cancellation->store(true);
    entry->retry->stop();
    entry->debounce->stop();
    entry->deadline->stop();
    entry->dirty.clear();
    entry->full = true;
    entry->failures.clear();
    QList<Request> cancelled;
    for (int i = m_queue.size() - 1; i >= 0; --i)
        if (m_queue[i].trackingId == id && (cancelActive || (m_queue[i].operation != "initialize" && m_queue[i].operation != "recover")))
            cancelled.append(m_queue.takeAt(i));
    for (const auto &request : cancelled)
    {
        TaskResult result;
        result.requestId = request.requestId;
        result.trackingId = id;
        result.operation = request.operation;
        result.success = false;
        result.code = ErrorCode::Cancelled;
        result.message = "任务已取消";
        emit taskFinished(result);
    }
}
void BackupService::retry(const QString &id)
{
    auto entry = m_entries.value(id);
    if (!entry || m_stopping)
        return;
    if (!entry->ready)
    {
        entry->monitor->stop();
        entry->monitor->start();
    }
    if (entry->initializing)
        return;
    if (entry->recoveryBlocked)
    {
        Request request;
        request.trackingId = id;
        request.operation = "recover";
        submit(request);
        return;
    }
    if (entry->failures.isEmpty())
        enqueueInitial(id);
    else
        for (auto &failure : entry->failures)
            failure.due = QDateTime::currentMSecsSinceEpoch();
    retryDue(id);
}
void BackupService::shutdown()
{
    if (m_stopping)
        return;
    m_stopping = true;
    emit stopping();
    bool running = false;
    for (const auto &entry : m_entries)
    {
        entry->monitor->stop();
        cancel(entry->tracking.id);
        running |= entry->busy;
    }
    if (!running)
        emit stopped();
}
} // namespace Backup
