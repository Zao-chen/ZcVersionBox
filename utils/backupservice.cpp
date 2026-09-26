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
        quint64 generation;
        BackupTaskPriority priority{BackupTaskPriority::Foreground};
        QPointer<QObject> context;
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
            return OperationResult::warn("操作已取消", {});
        auto result = engine->begin(job->cancelled);
        if (!result.success)
            return result;
        if (!job->id.isEmpty())
        {
            const auto records = engine->records();
            auto found = std::find_if(records.cbegin(), records.cend(), [&](const BackupRecord &r)
                                      { return r.id == job->id; });
            if (found == records.cend() || found->generation != job->generation)
                return OperationResult::warn("操作已取消", "追踪对象已删除或重建，请重新打开此页面后再试");
        }
        return result;
    }
    static bool same(const BackupRecord &a, const BackupRecord &b)
    {
        return a.sourcePath == b.sourcePath && a.directory == b.directory && a.repositoryPath == b.repositoryPath &&
               a.generation == b.generation && a.state == b.state &&
               a.stateDetail == b.stateDetail && a.lastCommit == b.lastCommit && a.pendingCommit == b.pendingCommit &&
               a.fingerprint == b.fingerprint && a.operation == b.operation && a.recoveryPaths == b.recoveryPaths;
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
        foregroundStreak = job->priority == BackupTaskPriority::Background ? 0 : std::min(foregroundStreak + 1, 8);
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
BackupTaskId BackupService::editMessage(const QString &id, const QString &commit, const QString &message, QObject *c, Completion f)
{
    return d->mutate(id, c, std::move(f), [id, commit, message](BackupEngine &e)
                     { return e.editMessage(id, commit, message); });
}
BackupTaskId BackupService::setRemote(const QString &id, const QString &url, QObject *c, Completion f)
{
    return d->mutate(id, c, std::move(f), [id, url](BackupEngine &e)
                     { return e.setRemote(id, url); });
}
BackupTaskId BackupService::removeRemote(const QString &id, QObject *c, Completion f)
{
    return d->mutate(id, c, std::move(f), [id](BackupEngine &e)
                     { return e.removeRemote(id); });
}
BackupTaskId BackupService::synchronize(const QString &id, bool push, QObject *c, Completion f)
{
    return d->mutate(id, c, std::move(f), [id, push](BackupEngine &e)
                     { return e.synchronize(id, push); });
}
BackupTaskId BackupService::removeBackup(const QString &id, QObject *c, Completion f)
{
    return d->mutate(id, c, std::move(f), [id](BackupEngine &e)
                     { return e.removeBackup(id); });
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
