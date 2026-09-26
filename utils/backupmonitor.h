#pragma once

#include "backupmonitor_catalog.h"
#include "backupmonitor_scanner.h"
#include "backupmonitor_scheduler.h"
#include "backupmonitor_watcher.h"
#include "backupservice.h"
#include <QElapsedTimer>
#include <QPointer>

struct BackupMonitorDependencies
{
    BackupMonitorScheduler::Clock clock;
    BackupSourceScanner::FilesFactory scanFiles;
    BackupSourceWatcher::AddPaths addWatchPaths;
};

class BackupMonitor : public QObject
{
    Q_OBJECT
  public:
    enum class State
    {
        Stopped,
        Running,
        Stopping
    };
    Q_ENUM(State)
    explicit BackupMonitor(BackupService *service, QObject *parent = nullptr,
                           BackupMonitorOptions options = {}, BackupMonitorDependencies dependencies = {});
    ~BackupMonitor() override;
    void start();
    void stop();
    State state() const { return m_state; }
    bool isIdle() const;
    int trackedCount() const { return m_scheduler.trackedCount(); }
    void reconcile();
    void scanNow();

  signals:
    void notification(const OperationResult &result);
    void stateChanged(BackupMonitor::State state);
    void scanStarted(const QString &id);
    void backupRequested(const QString &id);

  private:
    QPointer<BackupService> m_service;
    QElapsedTimer m_elapsed;
    BackupMonitorOptions m_options;
    BackupMonitorScheduler::Clock m_clock;
    BackupMonitorScheduler m_scheduler;
    BackupSourceWatcher m_sources;
    BackupSourceScanner m_scanner;
    BackupCatalogWatcher m_catalog;
    QTimer m_wakeup;
    State m_state{State::Stopped};
    quint64 m_epoch{0};
    bool m_restartRequested{false}, m_catalogDirty{false};
    qint64 m_firstCatalogChange{0}, m_lastCatalogChange{0}, m_nextAudit{0}, m_auditRetryAt{0};
    int m_auditFailures{0};
    BackupTaskId m_backupTask{0}, m_reloadTask{0};
    std::optional<BackupScanRequest> m_scanRequest, m_backupRequest;
    QMap<QString, QStringList> m_errors;
    void pump();
    void armTimer();
    void scanned(const BackupScanResult &result);
    void catalogChanged();
    void audited(const OperationResult &result, bool notify);
    void report(const QString &id, const OperationResult &result, bool notify = true);
    qint64 auditDeadline() const;
    void finishStopping();
    void setState(State state);
};
