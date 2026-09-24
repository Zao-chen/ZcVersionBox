#pragma once
#include "types.h"

namespace Backup
{
QString fileIdentity(const QString &path);
QString fileIdentityWithin(const QString &source, const QString &path);
FileEntry inspectFile(const QString &path, Cancellation cancel, TaskResult *stats = nullptr);
class SourceIndex
{
  public:
    explicit SourceIndex(QStringList excluded = {});
    Snapshot scan(const QString &source, const Snapshot *previous = nullptr, const QStringList &dirty = {}, Cancellation cancel = {}, TaskResult *stats = nullptr) const;
    bool excluded(const QString &path) const;

  private:
    QStringList m_excluded;
    void visit(const QString &source, const QString &relative, Snapshot &snapshot, Cancellation cancel, TaskResult *stats) const;
};
} // namespace Backup
