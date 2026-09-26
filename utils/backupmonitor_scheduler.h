#pragma once

#include "backupmonitor_types.h"
#include <functional>
#include <optional>

// No QObject, timers, I/O or task execution: all time comes from the caller.
class BackupMonitorScheduler
{
  public:
    enum class Phase
    {
        Idle,
        Debouncing,
        Scanning,
        AwaitingBackup,
        BackingUp,
        BackingOff,
        Paused
    };
    using Clock = std::function<qint64()>;
    explicit BackupMonitorScheduler(Clock clock, BackupMonitorOptions options = {});
    void reconcile(const QVector<BackupObservationTarget> &targets);
    void clear();
    void markDirty(const QString &id);
    void requestScan(const QString &id = {});
    std::optional<BackupScanRequest> takeScan();
    std::optional<BackupScanRequest> takeBackup();
    bool completeScan(const BackupScanResult &result);
    bool completeBackup(const BackupScanRequest &request, const OperationResult &result);
    bool isCurrent(const BackupScanRequest &request) const;
    bool sameRepository(const BackupScanRequest &request) const;
    qint64 nextDeadline() const;
    Phase phase(const QString &id) const;
    int trackedCount() const { return m_entries.size(); }
    bool hasWork() const;

  private:
    struct Entry
    {
        BackupObservationTarget target;
        quint64 changes{0};
        bool dirty{true}, immediate{true};
        qint64 firstChange{0}, lastChange{0}, nextCheck{0}, retryAt{0};
        int failures{0};
        std::optional<BackupScanRequest> backup;
    };
    Clock m_clock;
    BackupMonitorOptions m_options;
    QMap<QString, Entry> m_entries;
    QStringList m_order;
    QString m_lastScan, m_lastBackup;
    quint64 m_sequence{0};
    std::optional<BackupScanRequest> m_scan, m_backup;
    bool occupied(const QString &id) const;
    qint64 due(const Entry &entry) const;
    QStringList orderedAfter(const QString &id) const;
    void failed(Entry &entry);
};
