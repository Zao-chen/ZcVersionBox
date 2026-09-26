#pragma once

#include "backup_files.h"
#include "backupmonitor_types.h"
#include <QObject>
#include <QThread>
#include <functional>

class BackupSourceScanner : public QObject
{
    Q_OBJECT
  public:
    using FilesFactory = std::function<std::unique_ptr<BackupFiles>()>;
    explicit BackupSourceScanner(QObject *parent = nullptr, FilesFactory files = {});
    ~BackupSourceScanner() override;
    bool scan(const BackupScanRequest &request);
    void cancel();
    bool isBusy() const { return m_busy; }

  signals:
    void finished(const BackupScanResult &result);

  private:
    QThread m_thread;
    QObject *m_worker{new QObject};
    FilesFactory m_factory;
    std::unique_ptr<BackupFiles> m_files;
    std::shared_ptr<std::atomic_bool> m_cancel;
    bool m_busy{false};
};
