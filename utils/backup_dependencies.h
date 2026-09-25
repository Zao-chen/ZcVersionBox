#pragma once
#include "backup_files.h"
#include "gitcommand.h"
#include <functional>

struct BackupDependencies
{
    GitRunner git{runGit};
    std::shared_ptr<BackupFiles> files{std::make_shared<BackupFiles>()};
    std::function<bool(const BackupRecord &)> allowSave;
    std::function<QString(const QString &)> makeId;
    int aiTimeoutMs{15000};
};
