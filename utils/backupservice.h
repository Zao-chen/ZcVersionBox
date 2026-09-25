#pragma once
#include "apppaths.h"
#include "backup_dependencies.h"
#include <QObject>
#include <functional>
#include <memory>

class AiGateway;
class BackupService : public QObject
{
    Q_OBJECT
  public:
    using Completion = std::function<void(const OperationResult &)>;
    template <class T>
    using Reply = std::function<void(const BackupResult<T> &)>;
    explicit BackupService(const AppPaths &paths, QObject *parent = nullptr, AiGateway *gateway = nullptr, BackupDependencies dependencies = {});
    ~BackupService() override;
    const AppPaths &paths() const;
    QVector<TrackedItem> trackedItems() const;
    QString sourcePath(const QString &id) const;
    QString repoPath(const QString &id) const;
    QString idForSource(const QString &source) const;
    bool contains(const QString &id) const;
    bool isReady() const;
    bool isBusy() const;
    quint64 repositoryGeneration(const QString &id) const;
    BackupSyncState syncState(const QString &id) const;
    QString pendingCommit(const QString &id) const;

    BackupTaskId reload(QObject *context = nullptr, Completion callback = {});
    BackupTaskId addLocal(const QString &source, QObject *context, Completion callback = {});
    BackupTaskId backup(const QString &id, QObject *context, Completion callback = {}, bool changedOnly = false);
    BackupTaskId observe(const QString &id, QObject *context, Reply<bool> callback);
    BackupTaskId statistics(const QString &id, QObject *context, Reply<BackupStats> callback);
    BackupTaskId history(const QString &id, QObject *context, Reply<QVector<Revision>> callback);
    BackupTaskId diff(const QString &id, const QString &commit, QObject *context, Reply<DiffData> callback);
    BackupTaskId diffText(const QString &id, const DiffData &data, const QString &file, QObject *context, Reply<QString> callback);
    BackupTaskId preview(const QString &id, const QString &commit, QObject *context, Completion callback);
    BackupTaskId prepareRestore(const QString &id, const QString &commit, QObject *context, Reply<RestoreRequest> callback);
    BackupTaskId restore(const RestoreRequest &request, QObject *context, Completion callback);
    BackupTaskId preparePullResolution(const QString &id, QObject *context, Reply<RestoreRequest> callback);
    BackupTaskId resolvePull(const RestoreRequest &request, bool applyToSource, QObject *context, Completion callback);
    BackupTaskId editMessage(const QString &id, const QString &commit, const QString &message, QObject *context, Completion callback = {});
    BackupTaskId setRemote(const QString &id, const QString &url, QObject *context, Completion callback = {});
    BackupTaskId removeRemote(const QString &id, QObject *context, Completion callback = {});
    BackupTaskId synchronize(const QString &id, bool push, QObject *context, Completion callback = {});
    BackupTaskId removeBackup(const QString &id, QObject *context, Completion callback = {});
    BackupTaskId rebuild(const QString &id, QObject *context, Completion callback = {});
    BackupTaskId checkRemote(const QString &url, QObject *context, Completion callback = {});
    BackupTaskId prepareImport(const QString &url, QObject *context, Reply<PreparedImport> callback);
    BackupTaskId finishImport(const QString &sessionId, const QString &entry, const QString &target, bool replaceExisting, QObject *context, Completion callback);
    BackupTaskId cancelImport(const QString &sessionId, QObject *context = nullptr, Completion callback = {});
    BackupTaskId recheck(const QString &id, QObject *context, Completion callback);
    void cancel(BackupTaskId task);

  signals:
    void notification(const OperationResult &result);
    void ready();
    void trackedItemsChanged();
    void repositoryChanged(const QString &id);
    void repositoryInvalidated(const QString &id);
    void taskStarted(BackupTaskId task, const QString &id);
    void taskFinished(BackupTaskId task, const QString &id, const OperationResult &result);
    void busyChanged(bool busy);

  private:
    class Private;
    std::unique_ptr<Private> d;
};
