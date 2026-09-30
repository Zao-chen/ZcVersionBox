#pragma once
#include "backup_dependencies.h"
#include "backup_git.h"
#include <QJsonObject>

// A durable, isolated merge workspace. Only called on BackupService's worker.
// Neither analysis nor choices modify the live repository or source.
class BackupMerge
{
  public:
    BackupMerge(QString root, BackupDependencies dependencies, std::shared_ptr<std::atomic_bool> cancel = {});
    OperationResult create(const BackupRecord &record, const QString &liveRepository, const QString &pinnedRemote = {});
    OperationResult load();
    OperationResult save();
    SyncResolutionSession session() const;
    OperationResult verify(const BackupRecord &record, const QString &liveRepository) const;
    OperationResult choose(quint64 revision, const QString &path, int hunk, ConflictChoice choice);
    BackupResult<PreparedSyncApply> prepare();
    OperationResult exportSource(const PreparedSyncApply &request, const QString &target) const;
    BackupResult<ConflictContent> content(const QString &path, ConflictSide side) const;
    OperationResult exportSide(const QString &path, ConflictSide side, const QString &target) const;
    QString repositoryPath() const { return m_root + "/repository"; }
    QString remoteCommit() const;
    QString expectedHead() const;
    bool validRequest(const PreparedSyncApply &request) const;

  private:
    struct Entry { QString oid, mode; bool operator==(const Entry &o) const { return oid == o.oid && mode == o.mode; } };
    using Tree = QMap<QString, Entry>;
    QString m_root;
    BackupDependencies m_dependencies;
    std::shared_ptr<std::atomic_bool> m_cancel;
    QJsonObject m_data;
    GitRepository git() const;
    BackupResult<Tree> tree(const QString &revision) const;
    BackupResult<QByteArray> blob(const Entry &entry) const;
    OperationResult exportTree(const Tree &tree, const QString &prefix, const QString &target) const;
    BackupResult<Tree> resultTree() const;
    Tree withRetainedExtras(Tree entries) const;
    Tree sideTree(ConflictSide side) const;
    OperationResult analyze();
    OperationResult mergeText(QJsonObject &file, const Entry &base, const Entry &local, const Entry &remote);
    static QJsonObject encodeTree(const Tree &tree);
    static Tree decodeTree(const QJsonObject &object);
};
