#pragma once

#include "GlobalConstants.h"

struct AppPaths
{
    QString backupRoot;
    QString settingsFile;

    static AppPaths defaults()
    {
        return {BackupPath, Settingpath};
    }
};
