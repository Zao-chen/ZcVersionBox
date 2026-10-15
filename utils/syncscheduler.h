#pragma once
#include "backupservice.h"
#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>

struct SyncSchedulerOptions
{
    qint64 quietPeriodMs{3000}, checkIntervalMs{30000}, initialRetryMs{15000}, maximumRetryMs{300000};
};

// Application-owned scheduling only. All repository work remains in BackupService.
class SyncScheduler : public QObject
{
    Q_OBJECT
  public:
    enum class State { Stopped, Running, Stopping };
    using Clock = std::function<qint64()>;
    explicit SyncScheduler(BackupService *service, QObject *parent = nullptr, SyncSchedulerOptions options = {}, Clock clock = {});
    ~SyncScheduler() override;
    void start();
    void stop();
    State state() const { return m_state; }
    bool isIdle() const { return !m_task; }
    void tick();
  signals:
    void notification(const OperationResult &result);
    void syncRequested(const QString &id);
  private:
    struct Entry
    {
        quint64 generation{0}, token{0}, changes{0};
        QString remote, lastError;
        qint64 due{0};
        int failures{0};
        SyncState lastState{SyncState::Disconnected};
    };
    QPointer<BackupService> m_service;
    SyncSchedulerOptions m_options;
    QElapsedTimer m_elapsed;
    Clock m_clock;
    QTimer m_timer;
    QMap<QString, Entry> m_entries;
    State m_state{State::Stopped};
    BackupTaskId m_task{0};
    QString m_activeId;
    quint64 m_sequence{0};
    bool m_dispatching{false}, m_restart{false};
    bool eligible(const QString &id) const;
    void reconcile();
    void changed(const QString &id);
    void arm();
    void finishStop();
};
