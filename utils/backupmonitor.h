#pragma once
#include "backupservice.h"
#include <QFileSystemWatcher>
#include <QMap>
#include <QTimer>
#include <memory>

class BackupMonitor : public QObject
{
    Q_OBJECT
  public:
    explicit BackupMonitor(BackupService *service, QObject *parent = nullptr);
    void start();
    void stop();
    int trackedCount() const { return m_states.size(); }
    void reconcile();
    void scanNow();
  signals:
    void notification(const OperationResult &result);

  private:
    struct State
    {
        QString path;
        bool file{false};
        bool busy{false};
        QMap<QString, QString> fingerprint;
    };
    static QMap<QString, QString> scan(const State &state);
    BackupService *m_service;
    QTimer m_timer;
    QFileSystemWatcher m_watcher;
    QMap<QString, std::shared_ptr<State>> m_states;
};
