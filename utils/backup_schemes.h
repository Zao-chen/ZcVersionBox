#pragma once
#include "backup_git.h"

// Shared identities outlive branch names. The directory and branch updates are
// published in one lease-protected atomic push; local intentions survive offline.
struct SchemePlan
{
    QString remote, endpoint, revision;
    QMap<QString, QString> remoteHeads, effectiveHeads;
    QMap<QString, SharedScheme> directory;
    QStringList warnings;
};

class BackupSchemes
{
  public:
    static constexpr auto DirectoryRef = "refs/zcversionbox/schemes";
    static OperationResult track(BackupRecord &record, const GitRepository &git);
    static BackupResult<SchemePlan> read(const BackupRecord &record, const GitRepository &git);
    // Caller persists an operation marker before applying any local changes.
    static OperationResult reconcile(BackupRecord &record, const GitRepository &git, SchemePlan &plan);
    static OperationResult publish(BackupRecord &record, const GitRepository &git, SchemePlan &plan);
    static BackupResult<QString> merge(const GitRepository &git, const QString &repository,
                                      const QString &ours, const QString &theirs,
                                      const GitRunner &runner, std::shared_ptr<std::atomic_bool> cancel);
};
