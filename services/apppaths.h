#pragma once

#include <QDir>
#include <QStandardPaths>
#include <QString>

struct AppPaths
{
    QString backupRoot;
    QString settingsFile;

    static AppPaths defaults()
    {
        const auto root = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                              .filePath(QStringLiteral("ZcVersionBox"));
        return {root + "/Backup", root + "/config.ini"};
    }
};
