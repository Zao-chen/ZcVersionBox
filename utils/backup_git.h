#pragma once
#include "backup_files.h"
#include "gitcommand.h"
#include <optional>

class GitRepository
{
  public:
    explicit GitRepository(QString path, GitRunner runner = runGit, std::shared_ptr<std::atomic_bool> cancel = {});
    GitResult run(const QStringList &args, const QByteArray &input = {}, bool cancellable = true, int timeoutMs = 120000) const;
    OperationResult validate() const;
    OperationResult initialize(const QString &branch = {}, const QString &objectFormat = {}) const;
    BackupResult<QString> head() const;
    BackupResult<QString> resolve(const QString &revision) const;
    BackupResult<QString> branch() const;
    OperationResult clean() const;
    BackupResult<QString> stagedState(const QString &managedPath = ".") const;
    BackupResult<QVector<Revision>> history() const;
    BackupResult<DiffData> diff(const QString &revision) const;
    BackupResult<QString> diffText(const DiffData &data, const QString &file) const;
    OperationResult exportRevision(const QString &revision, const QString &relativePath, const QString &target, const BackupFiles &files) const;
    BackupResult<QVector<ImportEntry>> importEntries() const;
    OperationResult push(bool forceWithLease = false, const QString &expectedRemote = {}) const;
    OperationResult pull() const;
    BackupResult<QString> remoteHead() const;
    BackupResult<QString> targetRef() const;
    BackupResult<QString> remoteName() const;
    OperationResult validateMapping(const QString &commit, const QString &path, std::optional<bool> directory = {}) const;
    static OperationResult outcome(const GitResult &result, const QString &title = "Git 操作失败");

  private:
    QString m_path;
    GitRunner m_runner;
    std::shared_ptr<std::atomic_bool> m_cancel;
};
