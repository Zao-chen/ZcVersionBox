#pragma once

#include "backup_types.h"
#include "backupmonitor_directorywatcher.h"
#include <QHash>
#include <QSet>
#include <QTimer>
#include <functional>

class BackupSourceWatcher : public QObject
{
    Q_OBJECT
  public:
    // Returns rejected directory paths; injectable to verify fallback checks.
    using AddPaths = std::function<QStringList(BackupDirectoryWatcher &, const QStringList &)>;
    explicit BackupSourceWatcher(QObject *parent = nullptr, int batchSize = 256, AddPaths addPaths = {});
    void setTargets(const QVector<BackupObservationTarget> &targets);
    void updatePaths(const QString &id, const QStringList &paths, bool complete);
    void clear();
    bool isRegistering() const { return m_batch.isActive() || m_rearm.isActive(); }
    int watchedPathCount() const { return m_registered.size(); }

  signals:
    void sourceChanged(const QString &id);
    void coverageEstablished(const QString &id);
    void registrationFailed(const QString &id, const QString &path);

  private:
    BackupDirectoryWatcher m_watcher;
    QTimer m_batch, m_rearm;
    int m_batchSize;
    quint64 m_epoch{0};
    AddPaths m_addPaths;
    QMap<QString, BackupObservationTarget> m_targets;
    QHash<QString, QSet<QString>> m_paths, m_owners;
    QSet<QString> m_registered, m_failed, m_reported, m_coverage, m_changed;
    static QString key(const QString &path);
    QSet<QString> anchors(const BackupObservationTarget &target) const;
    void replace(const QString &id, QSet<QString> paths);
    void changed(const QStringList &paths);
    void rearm();
    void registerBatch();
};
