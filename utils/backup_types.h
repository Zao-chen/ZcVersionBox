#pragma once

#include "operationresult.h"
#include <QDateTime>
#include <QMap>
#include <QStringList>
#include <QVector>

enum class BackupSyncState
{
    Tracking,
    RemotePending,
    NeedsAttention
};
using SourceFingerprint = QMap<QString, QString>;
using BackupTaskId = quint64;

struct TrackedItem
{
    QString id;
    QString sourcePath;
    QString name;
    BackupSyncState state{BackupSyncState::Tracking};
    QString stateDetail;
};

struct BackupRecord
{
    QString id, sourcePath, repositoryPath;
    bool directory{false};
    quint64 generation{1};
    BackupSyncState state{BackupSyncState::Tracking};
    QString stateDetail, lastCommit, pendingCommit, operation;
    SourceFingerprint fingerprint;
    QStringList recoveryPaths;
    TrackedItem item() const;
};

struct BackupStats
{
    int versionCount{0}, fileCount{0};
    qint64 fileSize{0}, cacheSize{0};
    QString sourceState, remoteUrl;
    BackupSyncState syncState{BackupSyncState::Tracking};
    QString syncDetail, pendingCommit;
};
struct Revision
{
    QString hash, message;
    QDateTime committedAt;
    QString shortHash;
};
struct DiffFile
{
    QString status, path, summary;
};
struct DiffData
{
    QString oldCommit, newCommit;
    QVector<DiffFile> files;
};
struct ImportEntry
{
    QString path;
    bool directory{false};
};
struct PreparedImport
{
    QString sessionId;
    QVector<ImportEntry> entries;
    QString suggestedPath;
};
struct RestoreRequest
{
    QString id, commit;
    quint64 generation{0};
    SourceFingerprint sourceFingerprint;
    bool sourceExists{false};
    bool pulledVersion{false};
};
template <class T>
struct BackupResult
{
    OperationResult result;
    T value{};
};

QString backupStateText(BackupSyncState state);
Q_DECLARE_METATYPE(BackupSyncState)
