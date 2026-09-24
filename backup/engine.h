#pragma once
#include "snapshotstore.h"
#include <optional>

namespace Backup
{
class Engine
{
  public:
    Engine(QString root, Tracking tracking);
    TaskResult execute(const Request &request, Cancellation cancel = {});
    void recover();
    void setFaultHook(FaultHook hook) { m_fault = std::move(hook); }
    QString objectPath() const;
    QString repositoryPath() const;
    Tracking tracking() const { return m_tracking; }

  private:
    QString m_root;
    Tracking m_tracking;
    std::optional<Snapshot> m_index;
    FaultHook m_fault;
    Cancellation m_cancel;
    SourceIndex scanner() const;
    QString journalPath() const;
    void checkpoint(const QString &name) const;
    void saveJournal(const QJsonObject &journal) const;
    void recoverReplacement(QJsonObject journal, const QString &current);
    void finishPublication(TaskResult &result, const QString &revision, const QString &checkpointName);
    QString snapshot(bool full, const QStringList &dirty, const QString &operation, TaskResult &result);
    void replaceSource(const QString &next, const QString &base, const std::optional<Snapshot> &before, TaskResult &result);
    void rebuild(TaskResult &result);
};
} // namespace Backup
