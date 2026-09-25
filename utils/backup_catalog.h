#pragma once

#include "apppaths.h"
#include "backup_types.h"
#include <functional>

// Application records live outside the ordinary Git working repositories.
class BackupCatalog
{
  public:
    explicit BackupCatalog(AppPaths paths);
    OperationResult load();
    OperationResult save(const BackupRecord &record);
    OperationResult erase(const QString &id);
    void forget(const QString &id) { m_records.remove(id); }
    const QMap<QString, BackupRecord> &records() const { return m_records; }
    const BackupRecord *find(const QString &id) const;
    QString idForSource(const QString &source) const;
    QString itemPath(const QString &id) const;
    QString repoPath(const QString &id) const;
    QString stagingRoot() const;
    QString lockPath() const;
    const AppPaths &paths() const { return m_paths; }
    static QString normalizedSource(const QString &source);
    static bool validId(const QString &id);
    static bool validRepositoryPath(const QString &path);
    // Injectable persistence boundary, used to exercise failed durable state changes.
    std::function<bool(const BackupRecord &)> allowSave;

  private:
    AppPaths m_paths;
    QMap<QString, BackupRecord> m_records;
};
