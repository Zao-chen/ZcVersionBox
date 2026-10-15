#include "backupservice.h"
#include "aiconfighelper.h"
#include "aigateway.h"
#include "backup_engine.h"
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QQueue>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <optional>

class BackupService::Private
{
  public:
    struct Job
    {
        BackupTaskId task;
        QString id;
        quint64 generation, branchVersion;
        BackupTaskPriority priority{BackupTaskPriority::Foreground};
        QPointer<QObject> context;
        bool synchronization{false}, branchBound{true};
        std::shared_ptr<std::atomic_bool> cancelled{std::make_shared<std::atomic_bool>(false)};
        std::function<void(const std::shared_ptr<Job> &)> start;
    };
    using JobPtr = std::shared_ptr<Job>;
    BackupService *owner;
    AppPaths paths;
    BackupDependencies dependencies;
    QPointer<AiGateway> gateway;
    QThread thread;
    QObject *worker{new QObject};
    std::unique_ptr<BackupEngine> engine;
    QMap<QString, BackupRecord> cache;
    QQueue<JobPtr> foregroundQueue, backgroundQueue;
    JobPtr active;
    BackupTaskId sequence{0};
    struct SyncObserver { QPointer<QObject> context; Reply<SyncResult> callback; };
    QMap<QString, BackupTaskId> syncJobs;
    QMap<QString, QVector<SyncObserver>> syncObservers;
    QMap<QString, SyncStatus> syncStatuses;
    QSet<QString> locallyDirty;
    QMap<QString, std::shared_ptr<std::atomic_bool>> editGuards;
    quint64 observationSequence{0};
    QMap<QString, quint64> observationVersions;
    int foregroundStreak{0}, reloadCount{0};
    bool reportedBusy{false};
    bool loaded{false}, stopping{false};
    QPointer<QObject> aiContext;
    std::function<void()> cancelAi;

    Private(BackupService *service, AppPaths p, AiGateway *ai, BackupDependencies deps)
        : owner(service), paths(std::move(p)), dependencies(std::move(deps)),
          gateway(ai ? ai : new AiGateway(service)), engine(std::make_unique<BackupEngine>(paths, dependencies))
    {
        worker->moveToThread(&thread);
        QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
        thread.setObjectName("BackupWorker");
        thread.start();
    }
    ~Private()
    {
        stopping = true;
        if (active)
            active->cancelled->store(true);
        for (const auto *queue : {&foregroundQueue, &backgroundQueue})
            for (const auto &job : *queue)
                job->cancelled->store(true);
        if (aiContext)
            delete aiContext.data();
        cancelAi = {};
        // The engine rolls back any prepared backup before releasing its process lock.
        QMetaObject::invokeMethod(worker, [this]
                                  { engine.reset(); }, Qt::BlockingQueuedConnection);
        thread.quit();
        thread.wait();
    }
    bool busy() const { return active || !foregroundQueue.isEmpty() || !backgroundQueue.isEmpty(); }
    void publishBusy()
    {
        const bool value = busy();
        if (reportedBusy != value)
        {
            reportedBusy = value;
            emit owner->busyChanged(value);
        }
    }
    BackupTaskId schedule(const QString &id, QObject *context, std::function<void(const JobPtr &)> start,
                          BackupTaskPriority priority = BackupTaskPriority::Foreground)
    {
        auto job = std::make_shared<Job>();
        job->task = ++sequence;
        job->id = id;
        job->generation = owner->repositoryGeneration(id);
        job->branchVersion = cache.value(id).branchVersion;
        job->priority = priority;
        job->context = context ? context : owner;
        job->start = std::move(start);
        (priority == BackupTaskPriority::Foreground ? foregroundQueue : backgroundQueue).enqueue(job);
        publishBusy();
        QTimer::singleShot(0, owner, [this]
                           { next(); });
        return job->task;
    }
    void next()
    {
        if (active || stopping || (foregroundQueue.isEmpty() && backgroundQueue.isEmpty()))
            return;
        const bool background = !backgroundQueue.isEmpty() && (foregroundQueue.isEmpty() || foregroundStreak >= 8);
        active = (background ? backgroundQueue : foregroundQueue).dequeue();
        emit owner->taskStarted(active->task, active->id);
        active->start(active);
    }
    OperationResult begin(const JobPtr &job)
    {
        if (job->cancelled->load())
            return OperationResult::cancel("操作已取消");
        auto result = engine->begin(job->cancelled);
        if (!result.success)
            return result;
        if (!job->id.isEmpty())
        {
            const auto records = engine->records();
            auto found = std::find_if(records.cbegin(), records.cend(), [&](const BackupRecord &r)
                                      { return r.id == job->id; });
            if (found == records.cend() || found->generation != job->generation || (job->branchBound && found->branchVersion != job->branchVersion))
                return OperationResult::cancel("操作已取消", "追踪对象或工作方案已变化，请刷新后再试");
        }
        return result;
    }
    static bool same(const BackupRecord &a, const BackupRecord &b)
    {
        return a.sourcePath == b.sourcePath && a.directory == b.directory && a.repositoryPath == b.repositoryPath &&
               a.generation == b.generation && a.branchRef == b.branchRef && a.branchVersion == b.branchVersion && a.state == b.state &&
               a.stateDetail == b.stateDetail && a.lastCommit == b.lastCommit && a.pendingCommit == b.pendingCommit &&
               a.fingerprint == b.fingerprint && a.operation == b.operation && a.recoveryPaths == b.recoveryPaths &&
               a.resolutionSession == b.resolutionSession && a.resolutionHead == b.resolutionHead;
    }
    void publish(const QVector<BackupRecord> &records)
    {
        const auto previous = cache;
        cache.clear();
        for (auto r : records)
        {
            if (!r.operation.isEmpty())
            {
                r.state = BackupSyncState::NeedsAttention;
                r.stateDetail = "上次操作未完整确认，自动备份已暂停。保留副本：" + r.recoveryPaths.join("、");
            }
            cache.insert(r.id, r);
        }
        for (auto it = syncStatuses.begin(); it != syncStatuses.end();)
            if (!cache.contains(it.key())) it = syncStatuses.erase(it); else ++it;
        for (const auto &r : cache)
        {
            const bool localChange = (!active || !active->synchronization || active->id != r.id) && previous.contains(r.id) &&
                (previous[r.id].lastCommit != r.lastCommit || previous[r.id].branchVersion != r.branchVersion || previous[r.id].tagRevision != r.tagRevision);
            if (localChange) locallyDirty.insert(r.id);
            auto status = r.sync;
            if (locallyDirty.contains(r.id) && status.state == SyncState::Synced) status.state = SyncState::Waiting;
            if (!syncJobs.contains(r.id) && (!syncStatuses.contains(r.id) || syncStatuses[r.id] != status))
            { syncStatuses[r.id] = status; emit owner->syncStatusChanged(r.id); }
            if (localChange) emit owner->localVersionsChanged(r.id);
        }
        bool changed = previous.size() != cache.size();
        for (const auto &id : observationVersions.keys())
            if (!cache.contains(id))
                observationVersions.remove(id);
        for (const auto &r : cache)
            if (!previous.contains(r.id) || !same(previous[r.id], r))
                observationVersions[r.id] = ++observationSequence;
        for (const auto &old : previous)
            if (!cache.contains(old.id) || cache[old.id].generation != old.generation)
                emit owner->repositoryInvalidated(old.id);
        for (const auto &r : cache)
            if (!previous.contains(r.id) || !same(previous[r.id], r))
            {
                changed = true;
                emit owner->repositoryChanged(r.id);
            }
        for (const auto &r : cache)
            if (previous.contains(r.id) && same(previous[r.id], r) && previous[r.id].tagRevision != r.tagRevision)
                emit owner->repositoryChanged(r.id);
        if (changed)
            emit owner->trackedItemsChanged();
    }
    void done(const JobPtr &job, const OperationResult &result)
    {
        if (stopping)
            return;
        if (!loaded && result.success)
        {
            loaded = true;
            emit owner->ready();
        }
        emit owner->taskFinished(job->task, job->id, result);
        foregroundStreak = job->priority == BackupTaskPriority::Background || backgroundQueue.isEmpty()
                               ? 0
                               : std::min(foregroundStreak + 1, 8);
        active.reset();
        publishBusy();
        QTimer::singleShot(0, owner, [this]
                           { next(); });
    }
    template <class T>
    BackupTaskId submit(const QString &id, QObject *context, Reply<T> callback,
                        std::function<BackupResult<T>(BackupEngine &)> action, bool notify = false,
                        BackupTaskPriority priority = BackupTaskPriority::Foreground)
    {
        return schedule(id, context, [this, action = std::move(action), callback = std::move(callback), notify](const JobPtr &job)
                        { QMetaObject::invokeMethod(worker, [this, job, action, callback, notify]
                                                    {
                BackupResult<T> reply;
                reply.result = begin(job);
                if (reply.result.success)
                {
                    const auto warning = reply.result.warning;
                    reply = action(*engine);
                    if (!warning.isEmpty()) reply.result.warning = warning + (reply.result.warning.isEmpty() ? QString() : '\n' + reply.result.warning);
                }
                const auto records = engine->records();
                engine->end();
                QMetaObject::invokeMethod(owner, [this, job, reply, records, callback, notify]
                {
                    publish(records);
                    if (notify && reply.result.success && !job->id.isEmpty()) emit owner->repositoryChanged(job->id);
                    done(job, reply.result);
                    if (job->context && callback) callback(reply);
                }, Qt::QueuedConnection); }, Qt::QueuedConnection); }, priority);
    }
    BackupTaskId unbound(BackupTaskId task)
    {
        for (auto *queue : {&foregroundQueue, &backgroundQueue})
            for (const auto &job : *queue) if (job->task == task) job->branchBound = false;
        return task;
    }
    BackupTaskId mutate(const QString &id, QObject *context, Completion callback,
                        std::function<OperationResult(BackupEngine &)> action, bool notify = true,
                        BackupTaskPriority priority = BackupTaskPriority::Foreground)
    {
        return submit<bool>(id, context, [callback](const BackupResult<bool> &reply)
                            { if (callback) callback(reply.result); }, [action](BackupEngine &e)
                            { return BackupResult<bool>{action(e), false}; }, notify, priority);
    }
    void finishBackup(const JobPtr &job, const std::shared_ptr<PendingBackup> &work, const QString &message, Completion callback)
    {
        cancelAi = {};
        if (aiContext)
            aiContext->deleteLater();
        aiContext.clear();
        QMetaObject::invokeMethod(worker, [this, job, work, message, callback]
                                  {
            const auto result = engine->finishBackup(work, message, job->cancelled->load());
            const auto records = engine->records();
            engine->end();
            QMetaObject::invokeMethod(owner, [this, job, result, records, callback]
            {
                publish(records);
                done(job, result);
                if (job->context && callback) callback(result);
            }, Qt::QueuedConnection); }, Qt::QueuedConnection);
    }
    BackupTaskId submitBackup(const QString &id, QObject *context, Completion callback, BackupRequestOptions options,
                              std::optional<RestoreRequest> resolution = {})
    {
        const bool changedOnly = options.changedOnly;
        return schedule(id, context, [this, id, callback, changedOnly, resolution](const JobPtr &job)
                        { QMetaObject::invokeMethod(worker, [this, id, job, callback, changedOnly, resolution]
                                                    {
                BackupResult<std::shared_ptr<PendingBackup>> prepared;
                prepared.result = begin(job);
                if (prepared.result.success) prepared = engine->prepareBackup(id, changedOnly, resolution ? &*resolution : nullptr);
                AiConfigHelper::RuntimeConfig config;
                bool useAi = false;
                if (prepared.value && prepared.value->changed && !prepared.value->diff.isEmpty())
                {
                    QSettings settings(paths.settingsFile, QSettings::IniFormat);
                    useAi = settings.value("AI/Enabled", settings.value("AI/AutoCommitMessage", false)).toBool() &&
                            AiConfigHelper::loadRuntimeConfig(config, nullptr, paths.settingsFile);
                }
                const auto records = engine->records();
                if (!prepared.value) engine->end();
                QMetaObject::invokeMethod(owner, [this, job, callback, prepared, records, useAi, config]
                {
                    if (!prepared.value)
                    {
                        publish(records);
                        done(job, prepared.result);
                        if (job->context && callback) callback(prepared.result);
                        return;
                    }
                    if (!useAi || !gateway || job->cancelled->load())
                    {
                        finishBackup(job, prepared.value, {}, callback);
                        return;
                    }
                    aiContext = new QObject(owner);
                    const auto completed = std::make_shared<bool>(false);
                    const QPointer<BackupService> guard(owner);
                    const QPointer<QObject> context(aiContext);
                    const auto complete = [this, guard, context, job, work = prepared.value, callback, completed](const QString &message)
                    {
                        if (!guard || !context || *completed || stopping) return;
                        *completed = true;
                        finishBackup(job, work, message, callback);
                    };
                    cancelAi = [complete] { complete({}); };
                    QTimer::singleShot(dependencies.aiTimeoutMs, aiContext, [complete] { complete({}); });
                    gateway->generateCommitMessage(config, prepared.value->diff, aiContext,
                        [complete](const QString &message, const QString &) { complete(message); });
                }, Qt::QueuedConnection); }, Qt::QueuedConnection); }, options.priority);
    }
};

BackupService::BackupService(const AppPaths &paths, QObject *parent, AiGateway *gateway, BackupDependencies dependencies)
    : QObject(parent), d(std::make_unique<Private>(this, paths, gateway, std::move(dependencies))) { reload(); }
BackupService::~BackupService() = default;
const AppPaths &BackupService::paths() const { return d->paths; }
QVector<TrackedItem> BackupService::trackedItems() const
{
    QVector<TrackedItem> items;
    for (const auto &r : d->cache)
        items.append(r.item());
    std::sort(items.begin(), items.end(), [](const TrackedItem &a, const TrackedItem &b)
              { return a.sourcePath < b.sourcePath; });
    return items;
}
QVector<BackupObservationTarget> BackupService::observationTargets() const
{
    QVector<BackupObservationTarget> targets;
    targets.reserve(d->cache.size());
    for (const auto &r : d->cache)
        targets.append({r.id, r.sourcePath, r.directory, r.generation, d->observationVersions.value(r.id), r.state, r.fingerprint});
    return targets;
}
QString BackupService::sourcePath(const QString &id) const { return d->cache.value(id).sourcePath; }
QString BackupService::repoPath(const QString &id) const { return contains(id) ? QDir(d->paths.backupRoot).filePath("items/" + id + "/repository") : QString(); }
QString BackupService::idForSource(const QString &source) const
{
    const auto path = QDir::fromNativeSeparators(QDir::cleanPath(QFileInfo(source).absoluteFilePath()));
    for (const auto &r : d->cache)
    {
#ifdef Q_OS_WIN
        if (r.sourcePath.compare(path, Qt::CaseInsensitive) == 0)
            return r.id;
#else
        if (r.sourcePath == path)
            return r.id;
#endif
    }
    return {};
}
bool BackupService::contains(const QString &id) const { return d->cache.contains(id); }
bool BackupService::isReady() const { return d->loaded; }
bool BackupService::isReloading() const { return d->reloadCount > 0; }
bool BackupService::isBusy() const { return d->busy(); }
quint64 BackupService::repositoryGeneration(const QString &id) const { return contains(id) ? d->cache.value(id).generation : 0; }
BackupSyncState BackupService::syncState(const QString &id) const { return contains(id) ? d->cache[id].state : BackupSyncState::NeedsAttention; }
QString BackupService::pendingCommit(const QString &id) const { return d->cache.value(id).pendingCommit; }
BackupTaskId BackupService::reload(QObject *c, Completion f, BackupReloadOptions options)
{
    ++d->reloadCount;
    const QPointer<QObject> context(c ? c : this);
    // Load accounting belongs to the service even if the caller is destroyed.
    return d->mutate({}, this, [this, context, f, options](const OperationResult &result)
                     {
        --d->reloadCount;
        emit reloadFinished(result);
        if (options.notify && (!result.success || !result.warning.isEmpty())) emit notification(result);
        if (context && f) f(result); }, [](BackupEngine &e)
                     { return e.reload(); }, false, options.priority);
}
BackupTaskId BackupService::addLocal(const QString &p, QObject *c, Completion f)
{
    return d->mutate({}, c, std::move(f), [p](BackupEngine &e)
                     { return e.addLocal(p); });
}
BackupTaskId BackupService::backup(const QString &id, QObject *c, Completion f, BackupRequestOptions options) { return d->submitBackup(id, c, std::move(f), options); }
BackupTaskId BackupService::statistics(const QString &id, QObject *c, Reply<BackupStats> f)
{
    return d->submit<BackupStats>(id, c, std::move(f), [id](BackupEngine &e)
                                  { return e.statistics(id); });
}
BackupTaskId BackupService::history(const QString &id, QObject *c, Reply<QVector<Revision>> f)
{
    return d->submit<QVector<Revision>>(id, c, std::move(f), [id](BackupEngine &e)
                                        { return e.history(id); });
}
BackupTaskId BackupService::diff(const QString &id, const QString &commit, QObject *c, Reply<DiffData> f)
{
    return d->submit<DiffData>(id, c, std::move(f), [id, commit](BackupEngine &e)
                               { return e.diff(id, commit); });
}
BackupTaskId BackupService::diffText(const QString &id, const DiffData &data, const QString &file, QObject *c, Reply<QString> f)
{
    return d->submit<QString>(id, c, std::move(f), [id, data, file](BackupEngine &e)
                              { return e.diffText(id, data, file); });
}
BackupTaskId BackupService::preview(const QString &id, const QString &commit, QObject *c, Completion f)
{
    return d->mutate(id, c, std::move(f), [id, commit](BackupEngine &e)
                     { return e.preview(id, commit); }, false);
}
BackupTaskId BackupService::prepareRestore(const QString &id, const QString &commit, QObject *c, Reply<RestoreRequest> f)
{
    return d->submit<RestoreRequest>(id, c, std::move(f), [id, commit](BackupEngine &e)
                                     { return e.prepareRestore(id, commit, false); });
}
BackupTaskId BackupService::restore(const RestoreRequest &r, QObject *c, Completion f)
{
    return d->mutate(r.id, c, std::move(f), [r](BackupEngine &e)
                     { return e.restore(r); });
}
BackupTaskId BackupService::preparePullResolution(const QString &id, QObject *c, Reply<RestoreRequest> f)
{
    return d->submit<RestoreRequest>(id, c, std::move(f), [id](BackupEngine &e)
                                     { return e.prepareRestore(id, {}, true); });
}
BackupTaskId BackupService::resolvePull(const RestoreRequest &r, bool apply, QObject *c, Completion f)
{
    if (apply)
        return restore(r, c, std::move(f));
    return d->submitBackup(r.id, c, std::move(f), {}, r);
}
QVector<TagConflict> BackupService::tagConflicts(const QString &id) const
{
    const auto record = d->cache.value(id);
    const auto state = record.tagRemotes.value(record.tagEndpoint);
    QVector<TagConflict> result;
    for (auto it = state.conflicts.cbegin(); it != state.conflicts.cend(); ++it)
        result.append({it.key(), state.pending.value(it.key()), it.value(), record.tagEndpoint});
    return result;
}
BackupTaskId BackupService::resolveTagConflict(const QString &id, const TagConflict &conflict, const QString &name, QObject *c, Completion f)
{
    const QPointer<QObject> receiver(c ? c : this);
    return d->mutate(id, this, [this, id, receiver, f = std::move(f)](const OperationResult &result)
    {
        if (receiver && f) f(result);
        if (result.success && tagConflicts(id).isEmpty() && d->cache.value(id).syncContinuation)
            synchronize(id, {SyncTrigger::Continuation, BackupTaskPriority::Foreground}, this);
    }, [id, conflict, name](BackupEngine &e) { return e.resolveTagConflict(id, conflict, name); });
}
BackupTaskId BackupService::prepareRebuild(const QString &id, QObject *c, Reply<PreparedRebuild> f)
{ return d->submit<PreparedRebuild>(id, c, std::move(f), [id](BackupEngine &e) { return e.prepareRebuild(id); }); }
BackupTaskId BackupService::rebuild(const PreparedRebuild &r, QObject *c, Completion f)
{ return d->mutate(r.id, c, std::move(f), [r](BackupEngine &e) { return e.rebuild(r); }); }
BackupTaskId BackupService::createTag(const TagRequest &r, QObject *c, Completion f)
{ return d->mutate(r.id, c, std::move(f), [r](BackupEngine &e) { return e.createTag(r); }); }
BackupTaskId BackupService::renameTag(const TagRequest &r, const QString &name, QObject *c, Completion f)
{ return d->mutate(r.id, c, std::move(f), [r, name](BackupEngine &e) { return e.renameTag(r, name); }); }
BackupTaskId BackupService::removeTag(const TagRequest &r, QObject *c, Completion f)
{ return d->mutate(r.id, c, std::move(f), [r](BackupEngine &e) { return e.removeTag(r); }); }
BackupTaskId BackupService::editMessage(const QString &id, const QString &commit, const QString &message, QObject *c, Completion f)
{
    return d->mutate(id, c, std::move(f), [id, commit, message](BackupEngine &e)
                     { return e.editMessage(id, commit, message); });
}
BackupTaskId BackupService::setRemote(const QString &id, const QString &url, QObject *c, Completion f)
{
    cancelSync(id);
    return d->unbound(d->mutate(id, c, std::move(f), [id, url](BackupEngine &e)
                     { return e.setRemote(id, url); }));
}
BackupTaskId BackupService::removeRemote(const QString &id, QObject *c, Completion f)
{
    cancelSync(id);
    return d->unbound(d->mutate(id, c, std::move(f), [id](BackupEngine &e)
                     { return e.removeRemote(id); }));
}
BackupTaskId BackupService::transferSavedVersions(const QString &id, bool push, QObject *c, Completion f)
{
    return d->mutate(id, c, std::move(f), [id, push](BackupEngine &e)
                     { return e.transferSavedVersions(id, push); });
}
bool BackupService::isSynchronizing(const QString &id) const { return d->syncJobs.contains(id); }
SyncStatus BackupService::syncStatus(const QString &id) const
{
    auto status = d->syncStatuses.value(id, d->cache.value(id).sync);
    if (isSyncEditing(id) && (status.state == SyncState::Waiting || status.state == SyncState::Synced)) status.state = SyncState::Editing;
    if (!isSyncEditing(id) && status.state == SyncState::Editing) status.state = SyncState::Waiting;
    return status;
}
bool BackupService::isSyncEditing(const QString &id) const { const auto guard = d->editGuards.value(id); return guard && guard->load(); }
void BackupService::setSyncEditing(const QString &id, bool editing)
{
    if (id.isEmpty() || isSyncEditing(id) == editing) return;
    if (!d->editGuards.contains(id)) d->editGuards[id] = std::make_shared<std::atomic_bool>(false);
    d->editGuards[id]->store(editing);
    emit syncStatusChanged(id);
    emit syncEditingChanged(id);
}
void BackupService::cancelSync(const QString &id)
{
    if (d->syncJobs.contains(id)) cancel(d->syncJobs.value(id));
}
BackupTaskId BackupService::setAutoSync(const QString &id, bool enabled, QObject *c, Completion f)
{
    if (!enabled) cancelSync(id);
    return d->unbound(d->mutate(id, c, std::move(f), [id, enabled](BackupEngine &e) { return e.setAutoSync(id, enabled); }));
}
BackupTaskId BackupService::synchronize(const QString &id, SyncRequestOptions options, QObject *c, Reply<SyncResult> f)
{
    d->syncObservers[id].append({c ? c : this, std::move(f)});
    if (d->syncJobs.contains(id)) return d->syncJobs.value(id);
    auto status = syncStatus(id);
    status.state = isSyncEditing(id) ? SyncState::Editing : SyncState::Waiting;
    d->syncStatuses[id] = status;
    if (!d->editGuards.contains(id)) d->editGuards[id] = std::make_shared<std::atomic_bool>(false);
    const auto editing = d->editGuards.value(id);
    const auto task = d->submit<SyncResult>(id, this, [this, id](const BackupResult<SyncResult> &reply)
    {
        d->syncJobs.remove(id);
        auto output = reply;
        if (output.value.status.remoteUrl.isEmpty() && !output.result.success)
        {
            output.value.status = d->cache.value(id).sync;
            output.value.status.state = output.result.cancelled ? SyncState::Waiting : output.result.retryable ? SyncState::RetryLater : SyncState::NeedsAttention;
            output.value.status.message = output.result.title;
            output.value.status.detail = output.result.message;
        }
        if (output.value.status.state == SyncState::Synced) d->locallyDirty.remove(id);
        if (contains(id)) { d->syncStatuses[id] = output.value.status; emit syncStatusChanged(id); }
        const auto observers = d->syncObservers.take(id);
        for (const auto &observer : observers) if (observer.context && observer.callback) observer.callback(output);
    }, [this, id, options, editing](BackupEngine &e) -> BackupResult<SyncResult>
    {
        if (editing->load())
        {
            SyncStatus status;
            for (const auto &r : e.records()) if (r.id == id) status = r.sync;
            status.state = SyncState::Editing; status.detail = "文件有未保存的编辑，保存或放弃后继续同步。";
            return {OperationResult::info("等待保存编辑", status.detail), {status}};
        }
        return e.synchronize(id, options, [this, id](SyncPhase phase)
        {
            QMetaObject::invokeMethod(this, [this, id, phase]
            {
                if (!d->syncJobs.contains(id) || !contains(id)) return;
                auto &status = d->syncStatuses[id]; status.state = SyncState::Running; status.phase = phase;
                emit syncStatusChanged(id);
            }, Qt::QueuedConnection);
        }, [editing] { return editing->load(); });
    }, true, options.priority);
    d->syncJobs[id] = task;
    for (auto *queue : {&d->foregroundQueue, &d->backgroundQueue})
        for (const auto &job : *queue) if (job->task == task) job->synchronization = true;
    emit syncStatusChanged(id);
    return task;
}
BackupTaskId BackupService::removeBackup(const QString &id, QObject *c, Completion f)
{
    cancelSync(id);
    return d->unbound(d->mutate(id, c, std::move(f), [id](BackupEngine &e)
                     { return e.removeBackup(id); }));
}
BackupTaskId BackupService::rebuild(const QString &id, QObject *c, Completion f)
{
    return d->mutate(id, c, std::move(f), [id](BackupEngine &e)
                     { return e.rebuild(id); });
}
BackupTaskId BackupService::checkRemote(const QString &url, QObject *c, Completion f)
{
    return d->mutate({}, c, std::move(f), [url](BackupEngine &e)
                     { return e.checkRemote(url); }, false);
}
BackupTaskId BackupService::prepareImport(const QString &url, QObject *c, Reply<PreparedImport> f)
{
    return d->submit<PreparedImport>({}, c, std::move(f), [url](BackupEngine &e)
                                     { return e.prepareImport(url); });
}
BackupTaskId BackupService::finishImport(const QString &session, const QString &entry, const QString &target, bool replace, QObject *c, Completion f)
{
    return d->mutate({}, c, std::move(f), [session, entry, target, replace](BackupEngine &e)
                     { return e.finishImport(session, entry, target, replace); });
}
BackupTaskId BackupService::cancelImport(const QString &session, QObject *c, Completion f)
{
    return d->mutate({}, c, std::move(f), [session](BackupEngine &e)
                     { return e.cancelImport(session); }, false);
}
BackupTaskId BackupService::recheck(const QString &id, QObject *c, Completion f)
{
    return d->mutate(id, c, std::move(f), [id](BackupEngine &e)
                     { return e.recheck(id); });
}
void BackupService::cancel(BackupTaskId task)
{
    if (d->active && d->active->task == task)
    {
        d->active->cancelled->store(true);
        if (d->cancelAi)
        {
            auto cancel = d->cancelAi;
            cancel();
        }
    }
    for (const auto *queue : {&d->foregroundQueue, &d->backgroundQueue})
        for (const auto &job : *queue)
            if (job->task == task)
                job->cancelled->store(true);
}

BackupTaskId BackupService::prepareSyncResolution(const QString &id, QObject *c, Reply<SyncResolutionSession> f, bool restart)
{
    return d->submit<SyncResolutionSession>(id, c, std::move(f), [id, restart](BackupEngine &e) { return e.prepareSyncResolution(id, restart); });
}
BackupTaskId BackupService::syncResolution(const QString &id, QObject *c, Reply<SyncResolutionSession> f)
{
    return d->submit<SyncResolutionSession>(id, c, std::move(f), [id](BackupEngine &e) { return e.syncResolution(id); });
}
BackupTaskId BackupService::chooseSyncResolution(const QString &id, const QString &session, quint64 revision, const QString &path, int hunk, ConflictChoice choice, QObject *c, Reply<SyncResolutionSession> f)
{
    return d->submit<SyncResolutionSession>(id, c, std::move(f), [=](BackupEngine &e) { return e.chooseSyncResolution(id, session, revision, path, hunk, choice); }, true);
}
BackupTaskId BackupService::prepareSyncApply(const QString &id, const QString &session, quint64 revision, QObject *c, Reply<PreparedSyncApply> f)
{
    return d->submit<PreparedSyncApply>(id, c, std::move(f), [=](BackupEngine &e) { return e.prepareSyncApply(id, session, revision); });
}
BackupTaskId BackupService::applySync(const PreparedSyncApply &request, QObject *c, Completion f)
{
    const QPointer<QObject> receiver(c ? c : this);
    return d->mutate(request.id, this, [this, id = request.id, receiver, f = std::move(f)](const OperationResult &result)
    {
        if (receiver && f) f(result);
        if (result.success && d->cache.value(id).syncContinuation)
            synchronize(id, {SyncTrigger::Continuation, BackupTaskPriority::Foreground}, this);
    }, [request](BackupEngine &e) { return e.applySync(request); });
}
BackupTaskId BackupService::syncContent(const QString &id, const QString &session, const QString &path, ConflictSide side, QObject *c, Reply<ConflictContent> f)
{
    return d->submit<ConflictContent>(id, c, std::move(f), [=](BackupEngine &e) { return e.syncContent(id, session, path, side); });
}
BackupTaskId BackupService::previewSync(const QString &id, const QString &session, const QString &path, ConflictSide side, QObject *c, Completion f)
{
    return d->mutate(id, c, std::move(f), [=](BackupEngine &e) { return e.previewSync(id, session, path, side); }, false);
}

BranchContext BackupService::branchContext(const QString &id) const
{
    const auto r = d->cache.value(id);
    return {id, r.branchRef, r.lastCommit, r.generation, r.branchVersion};
}
BackupTaskId BackupService::branches(const QString &id, QObject *c, Reply<BranchSnapshot> f)
{ return d->submit<BranchSnapshot>(id, c, std::move(f), [id](BackupEngine &e) { return e.branches(id); }); }
BackupTaskId BackupService::branchHistory(const QString &id, const HistoryQuery &query, QObject *c, Reply<HistoryPage> f)
{ return d->submit<HistoryPage>(id, c, std::move(f), [id, query](BackupEngine &e) { return e.branchHistory(id, query); }); }
BackupTaskId BackupService::diffBetween(const QString &id, const QString &oldCommit, const QString &newCommit, QObject *c, Reply<DiffData> f)
{ return d->submit<DiffData>(id, c, std::move(f), [=](BackupEngine &e) { return e.diffBetween(id, oldCommit, newCommit); }); }
BackupTaskId BackupService::createBranch(const BranchRequest &r, QObject *c, Completion f)
{ return d->mutate(r.context.id, c, std::move(f), [r](BackupEngine &e) { return e.createBranch(r); }); }
BackupTaskId BackupService::renameBranch(const BranchRequest &r, QObject *c, Completion f)
{ return d->mutate(r.context.id, c, std::move(f), [r](BackupEngine &e) { return e.renameBranch(r); }); }
BackupTaskId BackupService::deleteBranch(const BranchRequest &r, QObject *c, Completion f)
{ return d->mutate(r.context.id, c, std::move(f), [r](BackupEngine &e) { return e.deleteBranch(r); }); }
BackupTaskId BackupService::setBranchUpstream(const BranchRequest &r, QObject *c, Completion f)
{ return d->mutate(r.context.id, c, std::move(f), [r](BackupEngine &e) { return e.setBranchUpstream(r); }); }
BackupTaskId BackupService::deleteRemoteBranch(const BranchRequest &r, QObject *c, Completion f)
{ return d->mutate(r.context.id, c, std::move(f), [r](BackupEngine &e) { return e.deleteRemoteBranch(r); }); }
BackupTaskId BackupService::fetchBranches(const BranchContext &r, QObject *c, Completion f)
{ return d->mutate(r.id, c, std::move(f), [r](BackupEngine &e) { return e.fetchBranches(r); }); }
BackupTaskId BackupService::prepareBranchSwitch(const BranchRequest &r, QObject *c, Reply<PreparedBranchSwitch> f)
{ return d->submit<PreparedBranchSwitch>(r.context.id, c, std::move(f), [r](BackupEngine &e) { return e.prepareBranchSwitch(r); }); }
BackupTaskId BackupService::switchBranch(const PreparedBranchSwitch &r, QObject *c, Completion f)
{ return d->mutate(r.context.id, c, std::move(f), [r](BackupEngine &e) { return e.switchBranch(r); }); }

BackupTaskId BackupService::prepareBranchMerge(const BranchRequest &r, QObject *c, Reply<SyncResolutionSession> f)
{ return d->submit<SyncResolutionSession>(r.context.id, c, std::move(f), [r](BackupEngine &e) { return e.prepareBranchMerge(r); }); }
BackupTaskId BackupService::cancelResolution(const QString &id, const QString &session, QObject *c, Completion f, bool disableAutomatic)
{
    return d->mutate(id, c, std::move(f), [=](BackupEngine &e) {
        const auto result = e.cancelResolution(id, session);
        if (result.success && disableAutomatic) return e.setAutoSync(id, false);
        return result;
    });
}

BackupTaskId BackupService::uploadBranch(const BranchRequest &r, QObject *c, Completion f)
{ return d->mutate(r.context.id, c, std::move(f), [r](BackupEngine &e) { return e.uploadBranch(r); }); }

BackupTaskId BackupService::sourceDirectory(const QString &id, const QString &path, QObject *c, Reply<SourceDirectory> f)
{ return d->submit<SourceDirectory>(id, c, std::move(f), [=](BackupEngine &e) { return e.sourceDirectory(id, path); }); }
BackupTaskId BackupService::sourceContent(const QString &id, const QString &path, QObject *c, Reply<SourceContent> f)
{ return d->submit<SourceContent>(id, c, std::move(f), [=](BackupEngine &e) { return e.sourceContent(id, path); }); }
BackupTaskId BackupService::sourceChanges(const QString &id, const QStringList &paths, QObject *c, Reply<SourceChanges> f)
{ return d->submit<SourceChanges>(id, c, std::move(f), [=](BackupEngine &e) { return e.sourceChanges(id, paths); }); }

BackupTaskId BackupService::saveSourceFile(const SourceSaveRequest &request, QObject *c, Completion f)
{
    const auto id = request.location.context.id;
    return d->mutate(id, c, [this, id, f = std::move(f)](const OperationResult &result) {
        if (result.success) emit sourceChanged(id);
        if (f) f(result);
    }, [request](BackupEngine &e) { return e.saveSourceFile(request); }, false);
}
BackupTaskId BackupService::prepareSourceMutation(const SourceLocation &location, SourceMutationKind kind, const QString &name, QObject *c, Reply<SourceMutationRequest> f)
{ return d->submit<SourceMutationRequest>(location.context.id, c, std::move(f), [=](BackupEngine &e) { return e.prepareSourceMutation(location, kind, name); }); }
BackupTaskId BackupService::applySourceMutation(const SourceMutationRequest &request, QObject *c, Completion f)
{
    const auto id = request.location.context.id;
    return d->mutate(id, c, [this, id, f = std::move(f)](const OperationResult &result) {
        emit sourceChanged(id); // A failed recursive delete may have removed some entries.
        if (f) f(result);
    }, [request](BackupEngine &e) { return e.applySourceMutation(request); }, false);
}
