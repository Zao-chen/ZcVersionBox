#pragma once
#include "engine.h"
#include "sourcemonitor.h"
#include <QQueue>
#include <QSet>
#include <QThreadPool>
#include <QTimer>

namespace Backup
{
class BackupService : public QObject
{
    Q_OBJECT
  public:
    using MonitorFactory = std::function<SourceMonitor *(const QString &, QObject *)>;
    using EngineFactory = std::function<std::shared_ptr<Engine>(const QString &, const Tracking &)>;
    explicit BackupService(QString root = storageRoot(), QObject *parent = nullptr, MonitorFactory factory = SourceMonitor::create, EngineFactory engines = {});
    ~BackupService() override;
    void start();
    QString track(const QString &source, const QString &remote = {}, bool importing = false);
    QList<Tracking> trackings() const;
    ConflictInfo queryConflict(const QString &id) const { return m_conflicts.value(id); }
    QString submit(Request request);
    void cancel(const QString &id);
    void retry(const QString &id);
    void requestAiAnnotation(const QString &id, const QString &revision) { emit aiAnnotationRequested(id, revision); }
    void shutdown();
    bool shuttingDown() const { return m_stopping; }
    QString root() const { return m_root; }
  signals:
    void trackingChanged();
    void taskFinished(const Backup::TaskResult &result);
    void stateChanged(const QString &id, const QString &channel, const QString &state);
    void snapshotCommitted(const QString &id, const QString &revision);
    void stopped();
    void stopping();
    void startupFailed(const QString &message);
    void aiAnnotationRequested(const QString &id, const QString &revision);
    void trackingResetRequested(const QString &id);

  private:
    struct Failure
    {
        Request request;
        int attempts = 0;
        qint64 due = -1;
    };
    struct Entry
    {
        Tracking tracking;
        std::shared_ptr<Engine> engine;
        SourceMonitor *monitor = nullptr;
        QTimer *debounce = nullptr, *deadline = nullptr, *retry = nullptr;
        QSet<QString> dirty;
        bool full = true, busy = false, ready = false, initializing = true, recoveryBlocked = false, maintenance = false;
        QString monitorError, activeOperation;
        Cancellation cancellation;
        QMap<QString, Failure> failures;
    };
    QString m_root;
    MonitorFactory m_factory;
    EngineFactory m_engineFactory;
    QMap<QString, std::shared_ptr<Entry>> m_entries;
    QMap<QString, ConflictInfo> m_conflicts;
    QQueue<Request> m_queue;
    QThreadPool m_pool;
    bool m_stopping = false;
    bool m_started = false;
    void attach(const Tracking &tracking);
    void enqueueSnapshot(const QString &id);
    void enqueueInitial(const QString &id);
    void armRetries(const QString &id);
    void retryDue(const QString &id);
    void cancelTasks(const QString &id, bool cancelActive);
    void dispatch();
    void finish(const Request &request, TaskResult result);
};
} // namespace Backup
