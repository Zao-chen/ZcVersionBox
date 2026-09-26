#pragma once
#include "utils/aigateway.h"
#include "utils/backup_catalog.h"
#include "utils/backupmonitor.h"
#include "utils/backupservice.h"
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <optional>

// macOS /var and /tmp are system aliases. Fixtures select their real paths;
// explicit links inside a selected source remain covered by the safety tests.
class TestDirectory : public QTemporaryDir
{
  public:
    explicit TestDirectory(const QString &pattern = {})
        : QTemporaryDir(BackupCatalog::normalizedSource(pattern.isEmpty() ? QDir::tempPath() + "/zc-tests-XXXXXX" : pattern)) {}
};

// Only tests wait for asynchronous use cases. Production callers use task IDs
// and context-bound completions, including the shell integration entry point.
template <class T, class Start>
T awaitBackup(Start start)
{
    std::optional<T> reply;
    start([&reply](const T &value)
          { reply = value; });
    if (!QTest::qWaitFor([&reply]
                         { return reply.has_value(); }, 30000))
        qFatal("Timed out waiting for an isolated backup task");
    return *reply;
}
inline void settle(BackupService &service)
{
    QCoreApplication::processEvents();
    if (!QTest::qWaitFor([&service]
                         { return !service.isBusy(); }, 30000))
        qFatal("Backup queue did not settle");
    QCoreApplication::processEvents();
}
inline void settle(BackupMonitor &monitor, BackupService &service)
{
    QCoreApplication::processEvents();
    if (!QTest::qWaitFor([&]
                         { return monitor.isIdle() && !service.isBusy(); }, 30000))
        qFatal("Monitor did not settle");
    QCoreApplication::processEvents();
}
inline BackupScanResult scanSource(const BackupObservationTarget &target)
{
    BackupSourceScanner scanner;
    std::optional<BackupScanResult> result;
    QObject::connect(&scanner, &BackupSourceScanner::finished, &scanner, [&](const BackupScanResult &reply)
                     { result = reply; });
    scanner.scan({target, 1, 1});
    if (!QTest::qWaitFor([&]
                         { return result.has_value(); }, 30000))
        qFatal("Source scan did not settle");
    return *result;
}
inline QString testBackupId(const QString &source)
{
    auto path = BackupCatalog::normalizedSource(source);
#ifdef Q_OS_WIN
    path = path.toCaseFolded();
#endif
    return QUuid::createUuidV5(QUuid("06a8e6da-8a10-4bd0-864d-56d585a072bf"), path.toUtf8()).toString(QUuid::WithoutBraces);
}
inline BackupDependencies testDependencies(BackupDependencies dependencies = {})
{
    if (!dependencies.makeId)
        dependencies.makeId = testBackupId;
    return dependencies;
}
class TestBackupService : public BackupService
{
  public:
    explicit TestBackupService(const AppPaths &paths, QObject *parent = nullptr, AiGateway *gateway = nullptr, BackupDependencies dependencies = {})
        : BackupService(paths, parent, gateway, testDependencies(std::move(dependencies))) { settle(*this); }
    OperationResult reload()
    {
        return awaitBackup<OperationResult>([&](auto f)
                                            { BackupService::reload(this, f); });
    }
    OperationResult addLocal(const QString &path)
    {
        return awaitBackup<OperationResult>([&](auto f)
                                            { BackupService::addLocal(path, this, f); });
    }
    using BackupService::backup;
    OperationResult backup(const QString &id)
    {
        return awaitBackup<OperationResult>([&](auto f)
                                            { BackupService::backup(id, this, f); });
    }
    OperationResult statistics(const QString &id, BackupStats &out)
    {
        auto reply = awaitBackup<BackupResult<BackupStats>>([&](auto f)
                                                            { BackupService::statistics(id, this, f); });
        out = reply.value;
        return reply.result;
    }
    OperationResult history(const QString &id, QVector<Revision> &out)
    {
        auto reply = awaitBackup<BackupResult<QVector<Revision>>>([&](auto f)
                                                                  { BackupService::history(id, this, f); });
        out = reply.value;
        return reply.result;
    }
    OperationResult diff(const QString &id, const QString &commit, DiffData &out)
    {
        auto reply = awaitBackup<BackupResult<DiffData>>([&](auto f)
                                                         { BackupService::diff(id, commit, this, f); });
        out = reply.value;
        return reply.result;
    }
    OperationResult diffText(const QString &id, const DiffData &data, const QString &file, QString &out)
    {
        auto reply = awaitBackup<BackupResult<QString>>([&](auto f)
                                                        { BackupService::diffText(id, data, file, this, f); });
        out = reply.value;
        return reply.result;
    }
    OperationResult preview(const QString &id, const QString &commit)
    {
        return awaitBackup<OperationResult>([&](auto f)
                                            { BackupService::preview(id, commit, this, f); });
    }
    BackupResult<RestoreRequest> prepareRestore(const QString &id, const QString &commit)
    {
        return awaitBackup<BackupResult<RestoreRequest>>([&](auto f)
                                                         { BackupService::prepareRestore(id, commit, this, f); });
    }
    OperationResult restore(const RestoreRequest &request)
    {
        return awaitBackup<OperationResult>([&](auto f)
                                            { BackupService::restore(request, this, f); });
    }
    OperationResult restore(const QString &id, const QString &commit)
    {
        const auto reply = prepareRestore(id, commit);
        return reply.result.success ? restore(reply.value) : reply.result;
    }
    BackupResult<RestoreRequest> preparePullResolution(const QString &id)
    {
        return awaitBackup<BackupResult<RestoreRequest>>([&](auto f)
                                                         { BackupService::preparePullResolution(id, this, f); });
    }
    using BackupService::resolvePull;
    OperationResult resolvePull(const RestoreRequest &request, bool apply)
    {
        return awaitBackup<OperationResult>([&](auto f)
                                            { BackupService::resolvePull(request, apply, this, f); });
    }
    OperationResult editMessage(const QString &id, const QString &commit, const QString &message)
    {
        return awaitBackup<OperationResult>([&](auto f)
                                            { BackupService::editMessage(id, commit, message, this, f); });
    }
    OperationResult setRemote(const QString &id, const QString &url)
    {
        return awaitBackup<OperationResult>([&](auto f)
                                            { BackupService::setRemote(id, url, this, f); });
    }
    OperationResult removeRemote(const QString &id)
    {
        return awaitBackup<OperationResult>([&](auto f)
                                            { BackupService::removeRemote(id, this, f); });
    }
    using BackupService::synchronize;
    OperationResult synchronize(const QString &id, bool push)
    {
        return awaitBackup<OperationResult>([&](auto f)
                                            { BackupService::synchronize(id, push, this, f); });
    }
    OperationResult removeBackup(const QString &id)
    {
        return awaitBackup<OperationResult>([&](auto f)
                                            { BackupService::removeBackup(id, this, f); });
    }
    OperationResult rebuild(const QString &id)
    {
        return awaitBackup<OperationResult>([&](auto f)
                                            { BackupService::rebuild(id, this, f); });
    }
    OperationResult checkRemote(const QString &url)
    {
        return awaitBackup<OperationResult>([&](auto f)
                                            { BackupService::checkRemote(url, this, f); });
    }
    BackupResult<PreparedImport> prepareImport(const QString &url)
    {
        return awaitBackup<BackupResult<PreparedImport>>([&](auto f)
                                                         { BackupService::prepareImport(url, this, f); });
    }
    OperationResult finishImport(const QString &session, const QString &entry, const QString &target, bool replace = false)
    {
        return awaitBackup<OperationResult>([&](auto f)
                                            { BackupService::finishImport(session, entry, target, replace, this, f); });
    }
    OperationResult cancelImport(const QString &session)
    {
        return awaitBackup<OperationResult>([&](auto f)
                                            { BackupService::cancelImport(session, this, f); });
    }
    OperationResult recheck(const QString &id)
    {
        return awaitBackup<OperationResult>([&](auto f)
                                            { BackupService::recheck(id, this, f); });
    }
};
