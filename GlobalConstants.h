#pragma once
#include <QString>
#include <QStandardPaths>
#include <QDir>

inline const QString BackupPath = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                                       .filePath("ZcVersionBox/Backup");

inline const QString Settingpath = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                                .filePath("ZcVersionBox/config.ini");
