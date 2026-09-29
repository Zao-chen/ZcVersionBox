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
    NeedsAttention,
    ResolutionPending
};
using SourceFingerprint = QMap<QString, QString>;
using BackupTaskId = quint64;

enum class BackupTaskPriority
{
    Foreground,
    Background
};

struct BackupRequestOptions
{
    bool changedOnly{false};
    BackupTaskPriority priority{BackupTaskPriority::Foreground};
};

struct BackupReloadOptions
{
    BackupTaskPriority priority{BackupTaskPriority::Foreground};
    bool notify{true};
};

// A value snapshot. Its version is local to the service and is never persisted.
struct BackupObservationTarget
{
    QString id, sourcePath;
    bool directory{false};
    quint64 generation{0}, version{0};
    BackupSyncState state{BackupSyncState::Tracking};
    SourceFingerprint fingerprint;
};

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
    QString resolutionSession, resolutionHead;
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

enum class ConflictChoice { Unresolved, Local, Remote };
enum class ConflictSide { Local, Remote, Result };
struct ConflictHunk
{
    QByteArray local, remote, before, after;
    ConflictChoice choice{ConflictChoice::Unresolved};
};
struct ConflictFile
{
    QString path;
    bool wholeFile{false}, managed{true};
    bool localExists{false}, remoteExists{false}, localDirectory{false}, remoteDirectory{false};
    qint64 localSize{0}, remoteSize{0};
    QVector<ConflictHunk> hunks;
};
struct SyncResolutionSession
{
    QString id, backupId;
    quint64 revision{1};
    QVector<ConflictFile> files;
    QDateTime localTime, remoteTime;
    bool stale{false};
    QString staleReason;
    QString currentPath;
    int currentHunk{0};
    int total() const { int n = 0; for (const auto &f : files) n += f.hunks.size(); return n; }
    int remaining() const { int n = 0; for (const auto &f : files) for (const auto &h : f.hunks) n += h.choice == ConflictChoice::Unresolved; return n; }
};
struct PreparedSyncApply
{
    QString id, sessionId, commit, tree;
    quint64 revision{0}, generation{0};
    SourceFingerprint sourceFingerprint;
    QVector<DiffFile> changes;
};
struct ConflictContent
{
    QString text;
    bool textual{false};
};
template <class T>
struct BackupResult
{
    OperationResult result;
    T value{};
};

QString backupStateText(BackupSyncState state);
Q_DECLARE_METATYPE(BackupSyncState)
