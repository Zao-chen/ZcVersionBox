#include "backupmonitor_watcher.h"
#include "backup_files.h"
#include <QDir>
#include <QFileInfo>
#include <algorithm>
#include <utility>

BackupSourceWatcher::BackupSourceWatcher(QObject *parent, int batchSize, AddPaths addPaths)
    : QObject(parent), m_batchSize(std::max(1, batchSize)), m_addPaths(std::move(addPaths))
{
    m_batch.setSingleShot(true);
    m_rearm.setSingleShot(true);
    connect(&m_batch, &QTimer::timeout, this, &BackupSourceWatcher::registerBatch);
    connect(&m_rearm, &QTimer::timeout, this, &BackupSourceWatcher::rearm);
    connect(&m_watcher, &BackupDirectoryWatcher::pathsChanged, this, &BackupSourceWatcher::changed);
}

QString BackupSourceWatcher::key(const QString &path)
{
    return BackupDirectoryWatcher::pathKey(path);
}

QSet<QString> BackupSourceWatcher::anchors(const BackupObservationTarget &target) const
{
    QSet<QString> paths;
    if (target.directory && QFileInfo(target.sourcePath).isDir() && !BackupFiles::hasLinkedAncestor(target.sourcePath))
        paths.insert(key(target.sourcePath));
    auto parent = QFileInfo(target.sourcePath).absolutePath();
    while (!parent.isEmpty())
    {
        if (QFileInfo(parent).isDir() && !BackupFiles::hasLinkedAncestor(parent))
        {
            paths.insert(key(parent));
            break;
        }
        const auto next = QFileInfo(parent).absolutePath();
        if (next == parent)
            break;
        parent = next;
    }
    return paths;
}

void BackupSourceWatcher::setTargets(const QVector<BackupObservationTarget> &targets)
{
    QSet<QString> active;
    for (const auto &target : targets)
    {
        active.insert(target.id);
        const auto old = m_targets.constFind(target.id);
        const bool retained = old != m_targets.cend() && old->sourcePath == target.sourcePath &&
                              old->directory == target.directory && old->generation == target.generation;
        m_targets[target.id] = target;
        replace(target.id, retained ? m_paths.value(target.id) | anchors(target) : anchors(target));
    }
    for (const auto &id : m_targets.keys())
        if (!active.contains(id))
        {
            replace(id, {});
            m_targets.remove(id);
            m_paths.remove(id);
            m_reported.remove(id);
            m_coverage.remove(id);
            m_changed.remove(id);
        }
}

void BackupSourceWatcher::updatePaths(const QString &id, const QStringList &paths, bool complete)
{
    if (!m_targets.contains(id))
        return;
    auto wanted = anchors(m_targets[id]);
    for (const auto &path : paths)
        wanted.insert(key(path));
    if (!complete)
        wanted |= m_paths.value(id); // An incomplete walk must not discard valid coverage.
    for (const auto &path : wanted)
        m_failed.remove(path); // Retry registration on the next completed scan, not a tight loop.
    replace(id, std::move(wanted));
}

void BackupSourceWatcher::replace(const QString &id, QSet<QString> paths)
{
    const auto previous = m_paths.value(id);
    QStringList removed;
    for (const auto &path : previous - paths)
    {
        auto owners = m_owners.find(path);
        if (owners == m_owners.end())
            continue;
        owners->remove(id);
        if (owners->isEmpty())
        {
            m_owners.erase(owners);
            m_failed.remove(path);
            if (m_registered.remove(path))
                removed.append(path);
        }
    }
    if (!removed.isEmpty())
        m_watcher.removePaths(removed);
    for (const auto &path : paths)
        m_owners[path].insert(id);
    if (!(paths - previous).isEmpty())
        m_coverage.insert(id);
    m_paths[id] = std::move(paths);
    if (!m_batch.isActive())
        m_batch.start(0);
}

void BackupSourceWatcher::changed(const QStringList &paths)
{
    QSet<QString> ids;
    for (const auto &path : paths)
    {
        const auto eventPath = key(path);
        auto candidates = m_owners.value(eventPath) | m_owners.value(key(QFileInfo(eventPath).absolutePath()));
        // A native recursive root can still serve nested owners after its
        // original owner is removed. Overflow invalidates all those owners.
        for (const auto &target : std::as_const(m_targets))
            if (key(target.sourcePath).startsWith(eventPath.endsWith('/') ? eventPath : eventPath + '/'))
                candidates.insert(target.id);
        for (const auto &id : candidates)
        {
            const auto target = m_targets.constFind(id);
            if (target == m_targets.cend())
                continue;
            const auto root = key(target->sourcePath);
            const auto childPrefix = eventPath.endsWith('/') ? eventPath : eventPath + '/';
            const auto rootPrefix = root.endsWith('/') ? root : root + '/';
            // Ancestor events locate a deleted/recreated source. For an existing
            // source, unrelated siblings and excluded build/.git traffic do not
            // invalidate the content check.
            if (root == eventPath || root.startsWith(childPrefix) ||
                (target->directory && eventPath.startsWith(rootPrefix) &&
                 BackupSelectionPolicy::observes(eventPath) && !BackupFiles::isGitMetadataPath(eventPath)))
                ids.insert(id);
        }
    }
    m_changed |= ids;
    if (!ids.isEmpty() && !m_rearm.isActive())
        m_rearm.start(0);
    for (const auto &id : ids)
        emit sourceChanged(id);
}

void BackupSourceWatcher::rearm()
{
    const auto directories = m_watcher.directories();
    QSet<QString> actual;
    QStringList removed;
    for (const auto &path : directories)
        if (QFileInfo(path).isDir() && !BackupFiles::hasLinkedAncestor(path))
            actual.insert(key(path));
        else
            removed.append(path);
    m_watcher.removePaths(removed);
    const auto lost = m_registered - actual;
    m_registered = std::move(actual);
    for (const auto &path : lost)
    {
        m_coverage |= m_owners.value(path);
        m_failed.remove(path);
    }
    const auto changedIds = std::exchange(m_changed, {});
    for (const auto &id : changedIds)
        if (m_targets.contains(id))
            replace(id, m_paths.value(id) | anchors(m_targets[id]));
    if (!m_batch.isActive())
        m_batch.start(0);
}

void BackupSourceWatcher::registerBatch()
{
    QStringList pending;
    for (auto it = m_owners.cbegin(); it != m_owners.cend() && pending.size() < m_batchSize; ++it)
        if (!m_registered.contains(it.key()) && !m_failed.contains(it.key()))
        {
            if (!QFileInfo(it.key()).isDir() || BackupFiles::hasLinkedAncestor(it.key()))
                m_failed.insert(it.key()); // A normal rename is not a registration failure.
            else
                pending.append(it.key());
        }
    if (!pending.isEmpty())
    {
        const auto rejected = m_addPaths ? m_addPaths(m_watcher, pending) : m_watcher.addPaths(pending);
        const QSet<QString> failed(rejected.cbegin(), rejected.cend());
        for (const auto &path : pending)
        {
            if (failed.contains(path))
            {
                m_failed.insert(path);
                for (const auto &id : m_owners.value(path))
                    if (!m_reported.contains(id))
                    {
                        m_reported.insert(id);
                        emit registrationFailed(id, path);
                    }
            }
            else
            {
                m_registered.insert(path);
                m_coverage |= m_owners.value(path);
            }
        }
        m_batch.start(0);
        return;
    }
    const auto completed = std::exchange(m_coverage, {});
    for (const auto &id : completed)
        if (m_targets.contains(id) && (m_paths.value(id) & m_failed).isEmpty())
        {
            m_reported.remove(id);
            // Covers the interval between enumeration and installing the watches.
            emit coverageEstablished(id);
        }
}

void BackupSourceWatcher::clear()
{
    m_batch.stop();
    m_rearm.stop();
    m_watcher.clear();
    m_targets.clear();
    m_paths.clear();
    m_owners.clear();
    m_registered.clear();
    m_failed.clear();
    m_reported.clear();
    m_coverage.clear();
    m_changed.clear();
}
