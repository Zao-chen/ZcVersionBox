#pragma once
#include "backupservice.h"
#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QTimer>
#include <memory>

class BackupMonitor : public QObject
{
    Q_OBJECT
  public:
    explicit BackupMonitor(BackupService *service, QObject *parent = nullptr, std::function<qint64()> clock = {});
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
        quint64 generation{0};
        bool busy{false};
        BackupTaskId task{0};
        int failures{0};
        qint64 retryAt{0};
        QString lastError;
    };
    BackupService *m_service;
    QTimer m_timer;
    QFileSystemWatcher m_watcher;
    QElapsedTimer m_elapsed;
    std::function<qint64()> m_clock;
    QMap<QString, std::shared_ptr<State>> m_states;
    quint64 m_epoch{0};
    bool m_enabled{true}, m_reloadPending{false};
    void watchCatalog();
    void completed(const std::shared_ptr<State> &state, const OperationResult &result);
};
