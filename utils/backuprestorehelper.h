#ifndef BACKUPRESTOREHELPER_H
#define BACKUPRESTOREHELPER_H

#include <QString>

struct BackupRestoreResult
{
    bool success{false};
    QString errorMessage;
    QString warningMessage;
};

class BackupRestoreHelper
{
  public:
    static BackupRestoreResult restoreFromGitRevision(const QString &repoPath,
                                                      const QString &revision,
                                                      const QString &targetPath);
};

#endif // BACKUPRESTOREHELPER_H
