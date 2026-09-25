#include "backupmonitor.h"
#include <QDateTime>
#include <QDirIterator>

BackupMonitor::BackupMonitor(BackupService *service, QObject *parent) : QObject(parent), m_service(service)
{
    m_timer.setInterval(1500);
    connect(&m_timer, &QTimer::timeout, this, &BackupMonitor::scanNow);
    connect(service, &BackupService::trackedItemsChanged, this, &BackupMonitor::reconcile);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, service, &BackupService::trackedItemsChanged);
}
void BackupMonitor::start()
{
    QDir().mkpath(m_service->paths().backupRoot);
    if (m_watcher.directories().isEmpty())
        m_watcher.addPath(m_service->paths().backupRoot);
    reconcile();
    m_timer.start();
}
void BackupMonitor::stop() { m_timer.stop(); }
QMap<QString, QString> BackupMonitor::scan(const State &state)
{
    QMap<QString, QString> result;
    const auto add = [&result](const QFileInfo &info)
    {
        result[QDir::cleanPath(info.filePath())] = QString::number(info.size()) + "|" + QString::number(info.lastModified().toMSecsSinceEpoch());
    };
    if (state.file)
    {
        QFileInfo info(state.path);
        if (info.exists() && info.isFile())
            add(info);
        return result;
    }
    QDirIterator it(state.path, QDir::Files | QDir::Hidden | QDir::Readable | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        it.next();
        const auto path = QDir::fromNativeSeparators(QDir::cleanPath(it.filePath()));
        if (path.contains("/.git/") || path.endsWith("/.git") || path.contains("/build/") || path.endsWith("/build"))
            continue;
        add(it.fileInfo());
    }
    return result;
}
void BackupMonitor::reconcile()
{
    QSet<QString> active;
    for (const auto &item : m_service->trackedItems())
    {
        active.insert(item.id);
        if (m_states.contains(item.id))
            continue;
        auto state = std::make_shared<State>();
        state->path = QDir::cleanPath(item.sourcePath);
        state->file = QFileInfo(state->path).isFile();
        state->fingerprint = scan(*state);
        m_states.insert(item.id, state);
    }
    for (const auto &id : m_states.keys())
        if (!active.contains(id))
            m_states.remove(id);
}
void BackupMonitor::scanNow()
{
    // Snapshot references survive nested AI event loops and changes to the tracked set.
    const auto states = m_states;
    for (auto it = states.cbegin(); it != states.cend(); ++it)
    {
        const auto state = it.value();
        if (state->busy || m_states.value(it.key()) != state)
            continue;
        auto current = scan(*state);
        if (current == state->fingerprint)
            continue;
        state->busy = true;
        const auto result = m_service->backup(it.key());
        state->fingerprint = std::move(current);
        state->busy = false;
        if (!result.success || !result.warning.isEmpty())
            emit notification(result);
    }
}
