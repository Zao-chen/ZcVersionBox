#include "backupmonitor_scanner.h"
#include <QDir>
#include <QFileInfo>

BackupSourceScanner::BackupSourceScanner(QObject *parent, FilesFactory files)
    : QObject(parent), m_factory(std::move(files))
{
    qRegisterMetaType<BackupScanResult>();
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread.setObjectName("BackupSourceScanner");
    m_thread.start();
}

BackupSourceScanner::~BackupSourceScanner()
{
    cancel();
    QMetaObject::invokeMethod(m_worker, [this]
                              { m_files.reset(); }, Qt::BlockingQueuedConnection);
    m_thread.quit();
    m_thread.wait();
}

void BackupSourceScanner::cancel()
{
    if (m_cancel)
        m_cancel->store(true);
}

bool BackupSourceScanner::scan(const BackupScanRequest &request)
{
    if (m_busy)
        return false;
    m_busy = true;
    m_cancel = std::make_shared<std::atomic_bool>(false);
    const auto cancelled = m_cancel;
    QMetaObject::invokeMethod(m_worker, [this, request, cancelled]
                              {
        if (!m_files)
            m_files = m_factory ? m_factory() : std::make_unique<BackupFiles>();
        BackupScanResult reply;
        reply.request = request;
        SourceFingerprint fingerprint;
        if (m_files)
        {
            m_files->cancellation = cancelled;
            reply.result = m_files->fingerprint(request.target.sourcePath, request.target.directory, fingerprint, true, &reply.watchPaths);
            m_files->cancellation.reset();
        }
        else
            reply.result = OperationResult::fail("源检查失败", "无法创建文件访问对象");
        reply.status = cancelled->load() ? BackupScanStatus::Cancelled :
                       !reply.result.success ? BackupScanStatus::Unavailable :
                       fingerprint != request.target.fingerprint ? BackupScanStatus::Changed : BackupScanStatus::Unchanged;
        QMetaObject::invokeMethod(this, [this, reply]
        {
            m_busy = false;
            m_cancel.reset();
            emit finished(reply);
        }, Qt::QueuedConnection); }, Qt::QueuedConnection);
    return true;
}
