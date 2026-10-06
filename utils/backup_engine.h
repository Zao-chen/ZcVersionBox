#pragma once
#include "backup_catalog.h"
#include "backup_dependencies.h"
#include "backup_git.h"
#include <QLockFile>
#include <QTemporaryDir>

struct PendingBackup
{
    BackupRecord before;
    SourceFingerprint fingerprint;
    std::shared_ptr<BackupReplacement> replacement;
    QString head, indexState, diff;
    bool changed{false};
};

// Synchronous use cases, confined to the service's worker. They contain no UI,
// timers or network event loops. AI waits occur between prepare and finish.
class BackupEngine
{
  public:
    BackupEngine(AppPaths paths, BackupDependencies dependencies);
    ~BackupEngine();
    OperationResult begin(std::shared_ptr<std::atomic_bool> cancellation);
    void end();
    QVector<BackupRecord> records() const;
    OperationResult reload();
    OperationResult addLocal(const QString &source);
    BackupResult<std::shared_ptr<PendingBackup>> prepareBackup(const QString &id, bool changedOnly = false, const RestoreRequest *resolution = nullptr);
    OperationResult finishBackup(std::shared_ptr<PendingBackup> pending, const QString &message, bool cancelled = false);
    BackupResult<BackupStats> statistics(const QString &id);
    BackupResult<QVector<Revision>> history(const QString &id);
    BackupResult<DiffData> diff(const QString &id, const QString &commit);
    BackupResult<QString> diffText(const QString &id, const DiffData &data, const QString &file);
    OperationResult preview(const QString &id, const QString &commit);
    BackupResult<RestoreRequest> prepareRestore(const QString &id, const QString &commit, bool pulledVersion);
    OperationResult restore(const RestoreRequest &request);
    OperationResult resolveTagConflict(const QString &id, const TagConflict &conflict, const QString &newName);
    BackupResult<PreparedRebuild> prepareRebuild(const QString &id);
    OperationResult rebuild(const PreparedRebuild &request);
    OperationResult createTag(const TagRequest &request);
    OperationResult renameTag(const TagRequest &request, const QString &name);
    OperationResult removeTag(const TagRequest &request);
    OperationResult editMessage(const QString &id, const QString &commit, const QString &message);
    OperationResult setRemote(const QString &id, const QString &url);
    OperationResult removeRemote(const QString &id);
    OperationResult synchronize(const QString &id, bool push);
    BackupResult<SyncResolutionSession> prepareSyncResolution(const QString &id, bool restart = false);
    BackupResult<SyncResolutionSession> syncResolution(const QString &id);
    BackupResult<SyncResolutionSession> chooseSyncResolution(const QString &id, const QString &sessionId, quint64 revision, const QString &path, int hunk, ConflictChoice choice);
    BackupResult<PreparedSyncApply> prepareSyncApply(const QString &id, const QString &sessionId, quint64 revision);
    OperationResult applySync(const PreparedSyncApply &request);
    BackupResult<ConflictContent> syncContent(const QString &id, const QString &sessionId, const QString &path, ConflictSide side);
    OperationResult previewSync(const QString &id, const QString &sessionId, const QString &path, ConflictSide side);
    OperationResult removeBackup(const QString &id);
    OperationResult rebuild(const QString &id);
    OperationResult checkRemote(const QString &url);
    BackupResult<PreparedImport> prepareImport(const QString &url);
    OperationResult finishImport(const QString &session, const QString &entry, const QString &target, bool replaceExisting);
    OperationResult cancelImport(const QString &session);
    OperationResult recheck(const QString &id);
    BackupResult<BranchSnapshot> branches(const QString &id);
    OperationResult createBranch(const BranchRequest &request);
    OperationResult renameBranch(const BranchRequest &request);
    OperationResult deleteBranch(const BranchRequest &request);
    OperationResult fetchBranches(const BranchContext &context);
    OperationResult setBranchUpstream(const BranchRequest &request);
    OperationResult deleteRemoteBranch(const BranchRequest &request);
    OperationResult uploadBranch(const BranchRequest &request);
    BackupResult<HistoryPage> branchHistory(const QString &id, const HistoryQuery &query);
    BackupResult<DiffData> diffBetween(const QString &id, const QString &oldCommit, const QString &newCommit);
    BackupResult<PreparedBranchSwitch> prepareBranchSwitch(const BranchRequest &request);
    OperationResult switchBranch(const PreparedBranchSwitch &request);
    BackupResult<SyncResolutionSession> prepareBranchMerge(const BranchRequest &request);
    OperationResult cancelResolution(const QString &id, const QString &session);

  private:
    struct ImportSession
    {
        std::shared_ptr<QTemporaryDir> directory;
        PreparedImport data;
    };
    BackupCatalog m_catalog;
    BackupDependencies m_dependencies;
    std::unique_ptr<QLockFile> m_lock;
    std::shared_ptr<std::atomic_bool> m_cancel;
    QMap<QString, ImportSession> m_imports;
    QMap<QString, quint64> m_generations;
    QStringList m_previews;
    std::shared_ptr<PendingBackup> m_pending;
    GitRepository repository(const QString &id) const;
    BackupResult<BackupRecord> require(const QString &id, bool writing = false, bool allowPending = false);
    OperationResult attention(BackupRecord record, const QString &reason);
    OperationResult abortBackup(const std::shared_ptr<PendingBackup> &work, OperationResult failure);
    OperationResult completeReplacement(BackupRecord record, BackupReplacement &replacement, OperationResult result);
    OperationResult verifyRequest(const RestoreRequest &request, BackupRecord &record);
    OperationResult validateSource(const QString &path, bool mustExist) const;
    QString newId(const QString &source) const;
    OperationResult changeTag(const TagRequest &request, const QString &name, bool create);
    OperationResult commitTagChanges(BackupRecord record, const QVector<TagRefChange> &changes);
    OperationResult syncTags(const QString &id, bool push);
    OperationResult recoverTags(BackupRecord record);
    BackupResult<SyncResolutionSession> prepareResolution(const QString &id, bool restart, QString mergeRef = {}, QString mergeHead = {});
    QString resolutionPath(const BackupRecord &record) const;
    BackupResult<BackupRecord> requireBranch(const BranchContext &context);
    OperationResult checkBranchTarget(const BranchRequest &request, bool remote = false);
    OperationResult rebuildBranchesSafe(const QString &id);
    OperationResult prepareSwitchFiles(const BackupRecord &record, const QString &target, const QString &destination,
                                       SourceFingerprint &source, QVector<DiffFile> &changes, bool &savesChanges,
                                       QVector<DiffFile> *savedChanges = nullptr);
    BackupResult<BackupRecord> resolutionRecord(const QString &id, const QString &sessionId = {});
};
