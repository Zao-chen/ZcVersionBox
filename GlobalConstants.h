#pragma once
#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>
#include <QVariant>

inline QString settingsPath()
{
    const auto root = QCoreApplication::instance()->property("backupDataRoot").toString();
    const auto directory = root.isEmpty()
                               ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath("backup-engine")
                               : root;
    return QDir(directory).filePath("settings.ini");
}
