#pragma once

#include "backup_types.h"
#include "backupmonitor_directorywatcher.h"
#include <QSet>
#include <QTimer>

class BackupCatalogWatcher : public QObject
{
    Q_OBJECT
  public:
    explicit BackupCatalogWatcher(QString backupRoot, QObject *parent = nullptr);
    void start();
    void stop();
    void setTargets(const QVector<BackupObservationTarget> &targets);

  signals:
    void changed();
    void registrationFailed(const QString &path);

  private:
    QString m_root;
    BackupDirectoryWatcher m_watcher;
    QTimer m_refresh;
    QStringList m_ids;
    QMap<QString, QString> m_recordStamps;
    QSet<QString> m_reported;
    bool m_running{false}, m_primed{false}, m_recordEvent{false};
    void refresh();
    void pathsChanged(const QStringList &paths);
    static QString stamp(const QString &path);
};
