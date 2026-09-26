#include "backupmonitor_directorywatcher.h"
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QSet>
#include <efsw/efsw.hpp>
#include <utility>

namespace
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
constexpr bool nativeRecursive = true;
#else
constexpr bool nativeRecursive = false;
#endif
bool within(const QString &path, const QString &root)
{
    return path == root || path.startsWith(root.endsWith('/') ? root : root + '/');
}
} // namespace

class BackupDirectoryWatcher::Private : public efsw::FileWatchListener
{
  public:
    explicit Private(BackupDirectoryWatcher *owner) : owner(owner) {}
    BackupDirectoryWatcher *owner;
    std::unique_ptr<efsw::FileWatcher> native;
    QHash<QString, efsw::WatchID> watches;
    QSet<QString> logicalPaths;
    QMutex mutex;
    struct WatchPath
    {
        QString path, canonical;
    };
    QHash<efsw::WatchID, WatchPath> callbackPaths;
    QSet<QString> acceptedDirectories;
    struct Pending
    {
        QSet<QString> paths, removed;
        bool overflow{false};
    };
    QHash<efsw::WatchID, Pending> pending;
    bool queued{false};
    quint64 epoch{0};

    void handleFileAction(efsw::WatchID id, const std::string &directory, const std::string &filename,
                          efsw::Action action, const std::string &oldFilename) override
    {
        const auto dir = QString::fromUtf8(directory);
        enqueue(id, QDir(dir).filePath(QString::fromUtf8(filename)), action == efsw::Actions::Delete,
                action == efsw::Actions::Moved ? QDir(dir).filePath(QString::fromUtf8(oldFilename)) : QString(), false);
    }
    void handleMissedFileActions(efsw::WatchID id, const std::string &) override
    {
        enqueue(id, {}, false, {}, true);
    }
    void enqueue(efsw::WatchID id, const QString &filename, bool removed, const QString &oldFilename, bool missed)
    {
        QMutexLocker lock(&mutex);
        const auto found = callbackPaths.constFind(id);
        if (found == callbackPaths.cend())
            return;
        auto &batch = pending[id];
        if (!batch.overflow)
        {
            const auto translate = [&](const QString &nativePath)
            {
                return QDir::fromNativeSeparators(QDir::cleanPath(QDir(found->path).filePath(QDir(found->canonical).relativeFilePath(nativePath))));
            };
            const auto path = missed ? found->path : translate(filename);
            const auto oldPath = oldFilename.isEmpty() ? QString() : translate(oldFilename);
            const auto accepted = [&](const QString &candidate)
            {
                return acceptedDirectories.contains(BackupDirectoryWatcher::pathKey(candidate)) ||
                       acceptedDirectories.contains(BackupDirectoryWatcher::pathKey(QFileInfo(candidate).absolutePath()));
            };
            if (!missed && !accepted(path) && (oldPath.isEmpty() || !accepted(oldPath)))
                return;
            batch.paths.insert(path);
            if (removed)
                batch.removed.insert(BackupDirectoryWatcher::pathKey(path));
            if (!oldFilename.isEmpty())
            {
                batch.paths.insert(oldPath);
                batch.removed.insert(BackupDirectoryWatcher::pathKey(oldPath));
            }
            if (missed || batch.paths.size() > 256)
            {
                // The directory is the conservative replacement for a large or
                // incomplete batch. Reinstall coverage as well as scanning it.
                batch.paths = {found->path};
                batch.removed = {found->path};
                batch.overflow = true;
            }
        }
        if (!queued)
        {
            queued = true;
            const auto currentEpoch = epoch;
            QMetaObject::invokeMethod(owner, [this, currentEpoch]
                                      { drain(currentEpoch); }, Qt::QueuedConnection);
        }
    }
    void drain(quint64 currentEpoch)
    {
        QHash<efsw::WatchID, Pending> batches;
        {
            QMutexLocker lock(&mutex);
            if (currentEpoch != epoch)
                return;
            batches = std::exchange(pending, {});
            queued = false;
        }
        QSet<QString> changes, invalidated;
        for (const auto &batch : batches)
        {
            changes |= batch.paths;
            invalidated |= batch.removed;
        }
        QStringList lost;
        for (const auto &directory : std::as_const(logicalPaths))
            for (const auto &path : invalidated)
                if (within(directory, path))
                {
                    lost.append(directory);
                    break;
                }
        owner->removePaths(lost);
        if (!changes.isEmpty())
            emit owner->pathsChanged(changes.values());
    }
    bool covered(const QString &path) const
    {
        for (auto it = watches.cbegin(); it != watches.cend(); ++it)
            if (it.key() == path || (nativeRecursive && within(path, it.key())))
                return true;
        return false;
    }
    void removeNative(const QString &path)
    {
        const auto id = watches.take(path);
        {
            QMutexLocker lock(&mutex);
            callbackPaths.remove(id);
            pending.remove(id);
        }
        native->removeWatch(id);
    }
};

BackupDirectoryWatcher::BackupDirectoryWatcher(QObject *parent) : QObject(parent), d(std::make_unique<Private>(this)) {}
BackupDirectoryWatcher::~BackupDirectoryWatcher() { clear(); }

QString BackupDirectoryWatcher::pathKey(const QString &path)
{
    auto result = QDir::fromNativeSeparators(QDir::cleanPath(path));
#ifdef Q_OS_WIN
    result = result.toCaseFolded();
#elif defined(Q_OS_MACOS)
    result = result.normalized(QString::NormalizationForm_C);
#endif
    return result;
}

QStringList BackupDirectoryWatcher::addPaths(const QStringList &paths)
{
    if (!d->native)
    {
        // If the native backend is unavailable, do not introduce a hidden 1 s
        // polling loop. The monitor's independent full checks remain active.
        d->native = std::make_unique<efsw::FileWatcher>(false, 30000);
        d->native->followSymlinks(false);
        d->native->allowOutOfScopeLinks(false);
    }
    QStringList rejected;
    for (const auto &path : paths)
    {
        const auto key = pathKey(path);
        if (d->logicalPaths.contains(key))
            continue;
        if (!d->covered(key))
        {
            const auto id = d->native->addWatch(key.toUtf8().toStdString(), d.get(), nativeRecursive);
            if (id < 0)
            {
                rejected.append(path);
                continue;
            }
            d->watches.insert(key, id);
            {
                QMutexLocker lock(&d->mutex);
                d->callbackPaths.insert(id, {key, pathKey(QFileInfo(key).canonicalFilePath())});
            }
            // Windows forbids renaming an ancestor of an open child directory,
            // even with FILE_SHARE_DELETE. A recursive parent watch replaces
            // those child handles, without a gap in event coverage.
            if (nativeRecursive)
                for (const auto &oldRoot : d->watches.keys())
                    if (oldRoot != key && within(oldRoot, key))
                        d->removeNative(oldRoot);
        }
        d->logicalPaths.insert(key);
        QMutexLocker lock(&d->mutex);
        d->acceptedDirectories.insert(key);
    }
    d->native->watch();
    return rejected;
}

void BackupDirectoryWatcher::removePaths(const QStringList &paths)
{
    for (const auto &path : paths)
    {
        const auto key = pathKey(path);
        d->logicalPaths.remove(key);
        QMutexLocker lock(&d->mutex);
        d->acceptedDirectories.remove(key);
    }
    for (const auto &root : d->watches.keys())
    {
        bool used = false;
        for (const auto &path : std::as_const(d->logicalPaths))
            if (path == root || (nativeRecursive && within(path, root)))
            {
                used = true;
                break;
            }
        if (!used)
            d->removeNative(root);
    }
}

QStringList BackupDirectoryWatcher::directories() const { return d->logicalPaths.values(); }

void BackupDirectoryWatcher::clear()
{
    {
        QMutexLocker lock(&d->mutex);
        ++d->epoch;
        d->callbackPaths.clear();
        d->acceptedDirectories.clear();
        d->pending.clear();
        d->queued = false;
    }
    // The native destructor joins callbacks before destroying their listener.
    d->native.reset();
    d->watches.clear();
    d->logicalPaths.clear();
}
