#include "backupmonitor_catalog.h"
#include "backup_catalog.h"
#include "backup_files.h"
#include <QDir>
#include <QFileInfo>

BackupCatalogWatcher::BackupCatalogWatcher(QString backupRoot, QObject *parent)
    : QObject(parent), m_root(BackupDirectoryWatcher::pathKey(backupRoot))
{
    m_refresh.setSingleShot(true);
    connect(&m_refresh, &QTimer::timeout, this, &BackupCatalogWatcher::refresh);
    connect(&m_watcher, &BackupDirectoryWatcher::pathsChanged, this, &BackupCatalogWatcher::pathsChanged);
}

void BackupCatalogWatcher::start()
{
    if (m_running)
        return;
    m_running = true;
    refresh();
}

void BackupCatalogWatcher::stop()
{
    m_running = false;
    m_refresh.stop();
    m_watcher.clear();
    m_recordStamps.clear();
    m_reported.clear();
    m_primed = m_recordEvent = false;
}

void BackupCatalogWatcher::setTargets(const QVector<BackupObservationTarget> &targets)
{
    m_ids.clear();
    for (const auto &target : targets)
        m_ids.append(target.id);
    if (m_running)
        refresh();
}

QString BackupCatalogWatcher::stamp(const QString &path)
{
    const QFileInfo file(path);
    return file.exists() ? QString::number(file.size()) + '|' + QString::number(file.lastModified().toMSecsSinceEpoch()) : QString();
}

void BackupCatalogWatcher::refresh()
{
    if (!m_running)
        return;
    QSet<QString> wanted;
    const auto add = [&](const QString &path)
    {
        if (QFileInfo::exists(path) && !BackupFiles::hasLinkedAncestor(path))
            wanted.insert(BackupDirectoryWatcher::pathKey(path));
    };
    add(m_root);
    auto parent = QFileInfo(m_root).absolutePath();
    while (!parent.isEmpty())
    {
        if (QFileInfo(parent).isDir() && !BackupFiles::hasLinkedAncestor(parent))
        {
            add(parent);
            break;
        }
        const auto next = QFileInfo(parent).absolutePath();
        if (next == parent)
            break;
        parent = next;
    }
    const auto items = m_root + "/items";
    add(items);
    QMap<QString, QString> stamps;
    const auto directories = QDir(items).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);
    auto ids = m_ids;
    for (const auto &id : directories)
        if (BackupCatalog::validId(id))
        {
            stamps[items + '/' + id + "/record.json"] = stamp(items + '/' + id + "/record.json");
            if (!ids.contains(id))
                ids.append(id);
        }
    for (const auto &id : ids)
    {
        add(items + '/' + id);
    }
    const bool modified = m_primed && (m_recordEvent || stamps != m_recordStamps);
    m_recordStamps = std::move(stamps);
    m_recordEvent = false;
    m_primed = true;
    const auto paths = m_watcher.directories();
    const QSet<QString> current(paths.cbegin(), paths.cend());
    const auto removed = (current - wanted).values(), added = (wanted - current).values();
    if (!removed.isEmpty())
        m_watcher.removePaths(removed);
    if (!added.isEmpty())
    {
        const auto rejected = m_watcher.addPaths(added);
        const QSet<QString> failed(rejected.cbegin(), rejected.cend());
        for (const auto &path : added)
            if (failed.contains(path))
            {
                if (!m_reported.contains(path))
                {
                    m_reported.insert(path);
                    emit registrationFailed(path);
                }
            }
            else
                m_reported.remove(path);
    }
    if (modified)
        emit changed();
}

void BackupCatalogWatcher::pathsChanged(const QStringList &paths)
{
    if (!m_running)
        return;
    for (const auto &path : paths)
        m_recordEvent = m_recordEvent || m_recordStamps.contains(path);
    // Compare catalog entries, not ancestor directory timestamps: lock/staging
    // traffic and creating an empty store must not schedule another reload.
    if (!m_refresh.isActive())
        m_refresh.start(0);
}
