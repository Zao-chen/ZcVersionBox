#pragma once

#include <QJsonObject>
#include <QMap>
#include <QMetaType>
#include <QStringList>
#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>

namespace Backup
{
enum class ErrorCode
{
    None,
    Io,
    Permission,
    SourceUnavailable,
    SourceChanged,
    Unsupported,
    InvalidFormat,
    Git,
    Timeout,
    Cancelled,
    Conflict,
    RecoveryRequired,
    Monitor
};
class Error : public std::runtime_error
{
  public:
    ErrorCode code;
    bool retryable;
    QString message;
    Error(ErrorCode c, QString text, bool retry = false)
        : std::runtime_error(text.toStdString()), code(c), retryable(retry), message(std::move(text)) {}
};
using Cancellation = std::shared_ptr<std::atomic_bool>;
using FaultHook = std::function<void(const QString &)>;
inline void checkCancelled(const Cancellation &cancel)
{
    if (cancel && cancel->load())
        throw Error(ErrorCode::Cancelled, QStringLiteral("任务已取消"));
}

struct Tracking
{
    QString id, source, remote;
    bool pendingImport = false;
    QJsonObject json() const;
    static Tracking parse(const QJsonObject &value);
};
struct FileEntry
{
    QString oid;
    qint64 size = 0;
    bool executable = false;
};
struct Snapshot
{
    QString kind, name;
    QMap<QString, FileEntry> files;
    QStringList directories;
    bool sameContent(const Snapshot &other) const;
    QJsonObject json() const;
    static Snapshot parse(const QJsonObject &value);
};
struct ConflictInfo
{
    QString operation, category, base, local, remote;
    QStringList paths;
    QJsonObject json() const;
};
struct TaskResult
{
    QString requestId, trackingId, operation, revision, message;
    ErrorCode code = ErrorCode::None;
    bool success = true, changed = false, retryable = false;
    qint64 filesRead = 0, bytesRead = 0, bytesWritten = 0;
    QJsonObject data;
    ConflictInfo conflict;
};
struct Request
{
    QString requestId, trackingId, operation, revision, text, remote;
    QStringList dirtyPaths;
    int offset = 0, limit = 50;
    bool fullScan = false;
};
QString newId();
QString storageRoot();
QString normalizedPath(const QString &path);
bool containsPath(const QString &parent, const QString &child);
void validateRelative(const QString &path);
void writeJson(const QString &path, const QJsonObject &value);
QJsonObject readJson(const QString &path);
void removeOwnedPath(const QString &path);
void renamePath(const QString &from, const QString &to);
} // namespace Backup
Q_DECLARE_METATYPE(Backup::TaskResult)
