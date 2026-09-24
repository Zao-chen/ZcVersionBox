#pragma once
#include "gitprocess.h"
#include "sourceindex.h"

namespace Backup
{
class SnapshotStore
{
  public:
    explicit SnapshotStore(QString path, Cancellation cancel = {});
    const QString &path() const { return m_path; }
    void initialize();
    QString head(const QString &ref = "refs/heads/main") const;
    Snapshot readSnapshot(const QString &revision) const;
    QString candidate(const Snapshot &snapshot, const QString &source, const QString &base, const QString &pendingRef, const QString &operation, TaskResult *stats = nullptr) const;
    QString copyCommit(const QString &revision, const QString &base, const QString &operation) const;
    void publish(const QString &revision, const QString &base) const;
    void exportSnapshot(const QString &revision, const QString &destination, bool readOnly = false) const;
    QJsonObject history(int offset, int limit) const;
    QJsonObject diff(const QString &revision) const;
    bool ancestor(const QString &older, const QString &newer) const;
    QString fetch(const QString &remote) const;
    void push(const QString &remote, const QString &lease = {}) const;
    void annotate(const QString &revision, const QString &text, bool ai) const;
    void syncNotes() const;

  private:
    QString m_path;
    Cancellation m_cancel;
    void validateNotes(const QString &ref) const;
};
} // namespace Backup
