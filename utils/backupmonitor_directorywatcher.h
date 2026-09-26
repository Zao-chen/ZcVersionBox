#pragma once

#include <QObject>
#include <QStringList>
#include <memory>

// Native directory events, including changes to their immediate files. Recursive
// enumeration and selection belong to the scanner, not the native watch thread.
class BackupDirectoryWatcher : public QObject
{
    Q_OBJECT
  public:
    explicit BackupDirectoryWatcher(QObject *parent = nullptr);
    ~BackupDirectoryWatcher() override;
    QStringList addPaths(const QStringList &paths);
    void removePaths(const QStringList &paths);
    QStringList directories() const;
    void clear();
    static QString pathKey(const QString &path);

  signals:
    // Delivered on this QObject's thread, with a bounded number of queued calls.
    // Removed/renamed directory registrations have already been discarded.
    void pathsChanged(const QStringList &paths);

  private:
    class Private;
    std::unique_ptr<Private> d;
};
