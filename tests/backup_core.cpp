#include "backup_test_support.h"
#include "utils/backup_engine.h"
#include "utils/backupmonitor.h"
#include "utils/backupmonitor_scheduler.h"
#include <QCoreApplication>
#include <QFile>
#include <QLockFile>
#include <QProcess>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <cstdio>
#include <future>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#define CHECK_OK(expression)                                                                                                           \
    do                                                                                                                                 \
    {                                                                                                                                  \
        const auto checkedResult = (expression);                                                                                       \
        QVERIFY2(checkedResult.success, qPrintable(checkedResult.title + ": " + checkedResult.message + " " + checkedResult.warning)); \
    } while (false)

namespace
{
void writeFile(const QString &path, const QByteArray &contents)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(contents) != contents.size())
        qFatal("Cannot write fixture");
}
bool replaceFileAtomically(const QString &path, const QByteArray &contents)
{
#ifdef Q_OS_WIN
    const auto candidate = path + '.' + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".tmp";
    writeFile(candidate, contents);
    const auto targetName = QDir::toNativeSeparators(path), candidateName = QDir::toNativeSeparators(candidate);
    // ReplaceFile permits readers sharing DELETE. QSaveFile's Windows rename
    // primitive rejects an open destination even when its reader shares DELETE.
    const bool replaced = ReplaceFileW(reinterpret_cast<const wchar_t *>(targetName.utf16()),
                                       reinterpret_cast<const wchar_t *>(candidateName.utf16()), nullptr,
                                       REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr);
    if (!replaced)
        QFile::remove(candidate);
    return replaced;
#else
    QSaveFile replacement(path);
    return replacement.open(QIODevice::WriteOnly) && replacement.write(contents) == contents.size() && replacement.commit();
#endif
}
QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}
AppPaths pathsIn(const QTemporaryDir &dir) { return {dir.path() + "/Backup", dir.path() + "/config.ini"}; }
GitResult git(const QString &path, const QStringList &args)
{
    auto result = runGit(path, args);
    if (!result.success())
        qFatal("Git fixture failed: %s", qPrintable(result.error));
    return result;
}
QString head(const BackupService &service, const QString &id) { return git(service.repoPath(id), {"rev-parse", "HEAD"}).output.trimmed(); }
BackupRecord record(const BackupService &service, const QString &id)
{
    BackupCatalog catalog(service.paths());
    if (!catalog.load().success || !catalog.find(id))
        qFatal("Missing fixture record");
    return *catalog.find(id);
}
GitResult failedGit()
{
    GitResult result;
    result.started = true;
    result.finished = true;
    result.exitCode = 1;
    result.error = "injected Git failure";
    return result;
}
class FaultFiles : public BackupFiles
{
  public:
    std::function<bool(const QString &)> failChildren;
    BackupResult<QStringList> children(const QString &path) const override
    {
        return failChildren && failChildren(path) ? BackupResult<QStringList>{OperationResult::fail("enumeration failure", path)} : BackupFiles::children(path);
    }
    std::function<bool(const QString &, const QString &)> failCopy, failRename;
    std::function<void(const QString &, const QString &)> afterCopy, afterRename;
    OperationResult copy(const QString &from, const QString &to) const override
    {
        if (failCopy && failCopy(from, to))
            return OperationResult::fail("copy failure", from);
        auto result = BackupFiles::copy(from, to);
        if (result.success && afterCopy)
            afterCopy(from, to);
        return result;
    }
    OperationResult rename(const QString &from, const QString &to) const override
    {
        if (failRename && failRename(from, to))
            return OperationResult::fail("rename failure", from);
        const auto result = BackupFiles::rename(from, to);
        if (result.success && afterRename)
            afterRename(from, to);
        return result;
    }
};
struct ScanGate
{
    std::atomic_bool hold{true};
    std::atomic_int enumerations{0};
    QSemaphore entered, resume;
};
class HeldScanFiles : public BackupFiles
{
  public:
    explicit HeldScanFiles(std::shared_ptr<ScanGate> gate) : m_gate(std::move(gate)) {}
    BackupResult<QStringList> children(const QString &path) const override
    {
        ++m_gate->enumerations;
        if (m_gate->hold.exchange(false))
        {
            m_gate->entered.release();
            while (!m_gate->resume.tryAcquire(1, 10))
                if (cancellation && cancellation->load())
                    return {OperationResult::warn("cancelled", {})};
        }
        return BackupFiles::children(path);
    }

  private:
    std::shared_ptr<ScanGate> m_gate;
};
class HeldAi : public AiGateway
{
  public:
    QVector<SummaryCallback> callbacks;
    void generateCommitMessage(const AiConfigHelper::RuntimeConfig &, const QString &, QObject *, SummaryCallback callback) override
    {
        callbacks.append(std::move(callback));
    }
};
void configureAi(const AppPaths &paths)
{
    QSettings settings(paths.settingsFile, QSettings::IniFormat);
    settings.setValue("AI/Enabled", true);
    settings.setValue("AI/Provider", "OpenAI");
    settings.setValue("AI/Providers/OpenAI/ApiKey", "isolated-fixture");
    settings.setValue("AI/Providers/OpenAI/Model", "fake-model");
}
struct RemoteFixture
{
    TestDirectory dir;
    AppPaths paths{pathsIn(dir)};
    std::unique_ptr<TestBackupService> service;
    QString source{dir.path() + "/source.txt"}, id{testBackupId(source)};
    QString remote{dir.path() + "/remote.git"}, writer{dir.path() + "/writer"};
    explicit RemoteFixture(BackupDependencies dependencies = {}, const QString &branch = "main")
    {
        service = std::make_unique<TestBackupService>(paths, nullptr, nullptr, std::move(dependencies));
        writeFile(source, "initial\n");
        if (!service->addLocal(source).success)
            qFatal("Cannot prepare backup");
        git(service->repoPath(id), {"branch", "-M", branch});
        git({}, {"init", "--bare", "--initial-branch=" + branch, remote});
        if (!service->setRemote(id, remote).success || !service->synchronize(id, true).success)
            qFatal("Cannot prepare remote");
        git({}, {"clone", "--no-hardlinks", remote, writer});
    }
    QString commitRemote(const QByteArray &contents, const QString &file = "source.txt")
    {
        writeFile(writer + '/' + file, contents);
        git(writer, {"add", "--all"});
        git(writer, {"commit", "-m", "remote edit"});
        git(writer, {"push"});
        return git(writer, {"rev-parse", "HEAD"}).output.trimmed();
    }
};
} // namespace

class BackupCoreRegression : public QObject
{
    Q_OBJECT
  private slots:
    void initTestCase()
    {
        QVERIFY(m_environment.isValid());
        writeFile(m_environment.path() + "/gitconfig", "[init]\n defaultBranch = main\n[core]\n autocrlf = false\n[user]\n name = Fixture\n email = fixture@example.test\n");
        qputenv("GIT_CONFIG_NOSYSTEM", "1");
        qputenv("GIT_CONFIG_GLOBAL", (m_environment.path() + "/gitconfig").toUtf8());
        QVERIFY(runGit({}, {"--version"}).success());
    }
    void fingerprintReaderAllowsAtomicSourceReplacement()
    {
        TestDirectory dir;
        const auto source = dir.path() + "/source.txt";
        writeFile(source, "original");
        QFile reader(source);
        QVERIFY(BackupFiles::openForFingerprint(reader));
        QVERIFY(replaceFileAtomically(source, "replaced"));
        QCOMPARE(reader.readAll(), QByteArray("original"));
        QCOMPARE(readFile(source), QByteArray("replaced"));
    }
    void sourceScannerDoesNotLockStorageOrBlockTheService()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/source";
        writeFile(source + "/file.txt", "initial");
        CHECK_OK(service.addLocal(source));
        const auto target = service.observationTargets().first();
        writeFile(source + "/file.txt", "changed");
        const auto gate = std::make_shared<ScanGate>();
        BackupSourceScanner scanner(nullptr, [gate]
                                    { return std::make_unique<HeldScanFiles>(gate); });
        QSignalSpy finished(&scanner, &BackupSourceScanner::finished), busy(&service, &BackupService::busyChanged);
        QLockFile lock(service.paths().backupRoot + "/.writer.lock");
        QVERIFY(lock.tryLock());
        QVERIFY(scanner.scan({target, 1, 1}));
        QVERIFY(!scanner.scan({target, 2, 2}));
        QTRY_VERIFY(gate->entered.available() > 0);
        QVERIFY(!service.isBusy());
        bool tick = false;
        QTimer::singleShot(0, this, [&]
                           { tick = true; });
        QTRY_VERIFY(tick);
        gate->resume.release();
        QTRY_COMPARE(finished.size(), 1);
        const auto reply = qvariant_cast<BackupScanResult>(finished[0][0]);
        QCOMPARE(reply.status, BackupScanStatus::Changed);
        QVERIFY(reply.watchPaths.contains(source));
        QCOMPARE(busy.size(), 0);
        QCOMPARE(record(service, target.id).fingerprint, target.fingerprint);
        QCOMPARE(readFile(service.repoPath(target.id) + "/source/file.txt"), QByteArray("initial"));
    }
    void sourceEventsHandleAtomicSavesNewDirectoriesAndRecreation()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/source";
        writeFile(source + "/deep/file.txt", "initial");
        CHECK_OK(service.addLocal(source));
        const auto id = service.idForSource(source), repository = service.repoPath(id) + "/source";
        BackupMonitorOptions options;
        options.quietPeriodMs = 25;
        options.maximumCoalesceMs = 100;
        options.scanIntervalMs = options.auditIntervalMs = 60000;
        BackupMonitor monitor(&service, nullptr, options);
        QSignalSpy requested(&monitor, &BackupMonitor::backupRequested);
        monitor.start();
        settle(monitor, service);
        QVERIFY(replaceFileAtomically(source + "/deep/file.txt", "atomic save"));
        QTRY_COMPARE_WITH_TIMEOUT(readFile(repository + "/deep/file.txt"), QByteArray("atomic save"), 10000);
        settle(monitor, service);
        writeFile(source + "/new/nested/added.txt", "new directory");
        QTRY_COMPARE_WITH_TIMEOUT(readFile(repository + "/new/nested/added.txt"), QByteArray("new directory"), 10000);
        settle(monitor, service);
        writeFile(source + "/new/nested/added.txt", "registered child");
        QTRY_COMPARE_WITH_TIMEOUT(readFile(repository + "/new/nested/added.txt"), QByteArray("registered child"), 10000);
        settle(monitor, service);
        QVERIFY(QDir().rename(source, source + "-old"));
        QTest::qWait(100);
        writeFile(source + "/returned.txt", "recreated source");
        QTRY_COMPARE_WITH_TIMEOUT(readFile(repository + "/returned.txt"), QByteArray("recreated source"), 10000);
        settle(monitor, service);
        QVERIFY(requested.size() >= 4);
        QCOMPARE(readFile(source + "-old/deep/file.txt"), QByteArray("atomic save"));
    }
    void rejectedSourceWatchesFallBackToPeriodicContentChecks()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/source.txt";
        writeFile(source, "one\n");
        CHECK_OK(service.addLocal(source));
        const auto id = service.idForSource(source);
        BackupMonitorOptions options;
        options.scanIntervalMs = 150;
        options.auditIntervalMs = 60000;
        BackupMonitorDependencies dependencies;
        dependencies.addWatchPaths = [](BackupDirectoryWatcher &, const QStringList &paths)
        { return paths; };
        BackupMonitor monitor(&service, nullptr, options, dependencies);
        QSignalSpy notifications(&monitor, &BackupMonitor::notification), requested(&monitor, &BackupMonitor::backupRequested);
        monitor.start();
        settle(monitor, service);
        QCOMPARE(notifications.size(), 1);
        const auto modified = QFileInfo(source).lastModified();
        writeFile(source, "two\n");
        QFile file(source);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(modified, QFileDevice::FileModificationTime));
        file.close();
        QTRY_COMPARE_WITH_TIMEOUT(readFile(service.repoPath(id) + "/source.txt"), QByteArray("two\n"), 10000);
        settle(monitor, service);
        QCOMPARE(requested.size(), 1);
        QCOMPARE(notifications.size(), 1);
        monitor.stop();
        QTRY_COMPARE(monitor.state(), BackupMonitor::State::Stopped);
    }
    void sourceWatchesSharePathsAndFilterUnrelatedChanges()
    {
        TestDirectory dir;
        const auto root = dir.path() + "/source";
        writeFile(root + "/one.txt", "one");
        writeFile(root + "/two.txt", "two");
        writeFile(root + "/build/output", "ignored");
        BackupObservationTarget first{"one", root + "/one.txt", false, 1, 1};
        BackupObservationTarget second{"two", root + "/two.txt", false, 1, 2};
        BackupObservationTarget directory{"directory", root, true, 1, 3};
        BackupSourceWatcher watcher;
        QSignalSpy changed(&watcher, &BackupSourceWatcher::sourceChanged);
        watcher.setTargets({first, second, directory});
        watcher.updatePaths(directory.id, {root}, true);
        QTRY_VERIFY(!watcher.isRegistering());
        QCOMPARE(watcher.watchedPathCount(), 2); // The shared directory and its parent.
        writeFile(dir.path() + "/unrelated.txt", "unrelated");
        writeFile(root + "/build/output", "still ignored");
        QTest::qWait(1200); // Also allows a native FSEvents batch to arrive on macOS.
        QCOMPARE(changed.size(), 0);
        writeFile(first.sourcePath, "changed one");
        const auto hasChanged = [&](const QString &id)
        {
            return std::any_of(changed.cbegin(), changed.cend(), [&](const QList<QVariant> &event)
                               { return event.first().toString() == id; });
        };
        QTRY_VERIFY(hasChanged(first.id) && hasChanged(directory.id));
        QVERIFY(!hasChanged(second.id));
        watcher.setTargets({second});
        QTRY_VERIFY(!watcher.isRegistering());
        QCOMPARE(watcher.watchedPathCount(), 1);
        changed.clear();
        QVERIFY(replaceFileAtomically(second.sourcePath, "changed two"));
        QTRY_VERIFY(hasChanged(second.id));
        QVERIFY(!hasChanged(first.id));
        watcher.clear();
        changed.clear();
        writeFile(second.sourcePath, "after stop");
        QTest::qWait(100);
        QCOMPARE(changed.size(), 0);
    }
    void missingEventsAreRecoveredAtThePeriodicDeadline()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/source.txt";
        writeFile(source, "one\n");
        CHECK_OK(service.addLocal(source));
        const auto id = service.idForSource(source);
        qint64 now = 0;
        BackupMonitorDependencies dependencies;
        dependencies.clock = [&]
        { return now; };
        // Successfully registered coverage that supplies no events models a
        // silently missed notification, separately from registration failure.
        dependencies.addWatchPaths = [](BackupDirectoryWatcher &, const QStringList &)
        { return QStringList{}; };
        BackupMonitor monitor(&service, nullptr, {}, dependencies);
        QSignalSpy scans(&monitor, &BackupMonitor::scanStarted), backups(&monitor, &BackupMonitor::backupRequested);
        monitor.start();
        settle(monitor, service);
        now = 500;
        monitor.reconcile();
        settle(monitor, service); // Finish the registration coverage check.
        const auto initialScans = scans.size();
        const auto modified = QFileInfo(source).lastModified();
        writeFile(source, "two\n");
        QFile file(source);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(modified, QFileDevice::FileModificationTime));
        file.close();
        now = 30499;
        monitor.reconcile();
        settle(monitor, service);
        QCOMPARE(scans.size(), initialScans);
        QCOMPARE(backups.size(), 0);
        now = 30500;
        monitor.reconcile();
        settle(monitor, service);
        QCOMPARE(backups.size(), 1);
        QCOMPARE(readFile(service.repoPath(id) + "/source.txt"), QByteArray("two\n"));
    }
    void monitorCancelsQueuedBackupWhenPullPausesTarget()
    {
        RemoteFixture fixture;
        const auto baseline = record(*fixture.service, fixture.id).fingerprint;
        const auto pulled = fixture.commitRemote("remote version\n");
        writeFile(fixture.source, "local edit\n");
        BackupMonitor monitor(fixture.service.get());
        QSignalSpy requests(&monitor, &BackupMonitor::backupRequested), notifications(&monitor, &BackupMonitor::notification);
        QSignalSpy finished(fixture.service.get(), &BackupService::taskFinished);
        bool pulling = false;
        connect(&monitor, &BackupMonitor::backupRequested, this, [&]
                {
            if (!pulling)
            {
                pulling = true;
                fixture.service->BackupService::synchronize(fixture.id, false, this);
            } });
        monitor.start();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.service->syncState(fixture.id), BackupSyncState::RemotePending, 15000);
        settle(monitor, *fixture.service);
        QCOMPARE(requests.size(), 1);
        QCOMPARE(notifications.size(), 0);
        QVERIFY(std::any_of(finished.cbegin(), finished.cend(), [](const QList<QVariant> &event)
                            { return qvariant_cast<OperationResult>(event[2]).cancelled; }));
        QCOMPARE(head(*fixture.service, fixture.id), pulled);
        QCOMPARE(record(*fixture.service, fixture.id).fingerprint, baseline);
        QCOMPARE(readFile(fixture.source), QByteArray("local edit\n"));
        monitor.scanNow();
        settle(monitor, *fixture.service);
        QCOMPARE(requests.size(), 1);
    }
    void largeProjectMonitorBoundsEventStorms()
    {
        TestDirectory dir;
        const auto source = dir.path() + "/large-source";
        for (int directory = 0; directory < 100; ++directory)
            for (int file = 0; file < 100; ++file)
                writeFile(source + QString("/dir-%1/file-%2.txt").arg(directory).arg(file), "initial\n");
        TestBackupService service(pathsIn(dir));
        std::optional<OperationResult> added;
        service.BackupService::addLocal(source, this, [&](const OperationResult &result)
                                        { added = result; });
        QTRY_VERIFY_WITH_TIMEOUT(added.has_value(), 90000);
        CHECK_OK(*added);
        const auto id = service.idForSource(source);
        QCOMPARE(record(service, id).fingerprint.size(), 10000);
        const auto gate = std::make_shared<ScanGate>();
        gate->hold.store(false);
        BackupMonitorDependencies dependencies;
        dependencies.scanFiles = [gate]
        { return std::make_unique<HeldScanFiles>(gate); };
        BackupMonitor monitor(&service, nullptr, {}, dependencies);
        QSignalSpy scans(&monitor, &BackupMonitor::scanStarted), requests(&monitor, &BackupMonitor::backupRequested);
        QSignalSpy notifications(&monitor, &BackupMonitor::notification);
        int pending = 0, maximumPending = 0;
        connect(&monitor, &BackupMonitor::backupRequested, this, [&](const QString &)
                { maximumPending = std::max(maximumPending, ++pending); });
        connect(&service, &BackupService::taskFinished, this, [&](BackupTaskId, const QString &item, const OperationResult &)
                { if (item == id && pending) --pending; });
        monitor.start();
        settle(monitor, service);
        QTest::qWait(1600);
        settle(monitor, service);
        const auto idleScans = scans.size();
        QTest::qWait(2100);
        QCOMPARE(scans.size(), idleScans);
        QCOMPARE(requests.size(), 0);
        gate->hold.store(true);
        writeFile(source + "/dir-0/file-0.txt", "first event\n");
        QTRY_VERIFY_WITH_TIMEOUT(gate->entered.available() > 0, 10000);
        const auto stormScans = scans.size();
        auto writes = std::async(std::launch::async, [source]
                                 {
            for (int i = 0; i < 2000; ++i)
                writeFile(source + QString("/dir-%1/file-%2.txt").arg(i % 100).arg(i / 100), "storm-" + QByteArray::number(i)); });
        QTRY_VERIFY_WITH_TIMEOUT(writes.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 30000);
        writes.get();
        QTest::qWait(1200);
        QCOMPARE(scans.size(), stormScans);
        QCOMPARE(requests.size(), 0);
        QElapsedTimer drainTime;
        drainTime.start();
        gate->resume.release();
        QTRY_VERIFY_WITH_TIMEOUT(requests.size() > 0 && monitor.isIdle() && !service.isBusy(), 180000);
        QTest::qWait(1200);
        settle(monitor, service);
        QCOMPARE(readFile(service.repoPath(id) + "/large-source/dir-99/file-19.txt"), QByteArray("storm-1999"));
        QCOMPARE(maximumPending, 1);
        QVERIFY(requests.size() <= 2);
        QCOMPARE(notifications.size(), 0);
        qInfo("10000 files: idle scans in 2.1 s = 0; 2000 writes: scan starts = %lld, backup requests = %lld, maximum pending = %d, drain = %lld ms",
              qlonglong(scans.size() - idleScans), qlonglong(requests.size()), maximumPending, qlonglong(drainTime.elapsed()));
    }
    void monitorStopRestartDrainsTheOldScan()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/source";
        writeFile(source + "/file.txt", "initial");
        CHECK_OK(service.addLocal(source));
        const auto gate = std::make_shared<ScanGate>();
        BackupMonitorDependencies dependencies;
        dependencies.scanFiles = [gate]
        { return std::make_unique<HeldScanFiles>(gate); };
        BackupMonitor monitor(&service, nullptr, {}, dependencies);
        QSignalSpy states(&monitor, &BackupMonitor::stateChanged), notifications(&monitor, &BackupMonitor::notification);
        QCOMPARE(monitor.state(), BackupMonitor::State::Stopped);
        monitor.scanNow();
        QCOMPARE(gate->enumerations.load(), 0);
        monitor.start();
        monitor.start();
        QTRY_VERIFY(gate->entered.available() > 0);
        monitor.stop();
        QCOMPARE(monitor.state(), BackupMonitor::State::Stopping);
        monitor.start();
        QTRY_COMPARE(monitor.state(), BackupMonitor::State::Running);
        settle(monitor, service);
        QCOMPARE(states.size(), 4);
        QCOMPARE(qvariant_cast<BackupMonitor::State>(states[0][0]), BackupMonitor::State::Running);
        QCOMPARE(qvariant_cast<BackupMonitor::State>(states[1][0]), BackupMonitor::State::Stopping);
        QCOMPARE(qvariant_cast<BackupMonitor::State>(states[2][0]), BackupMonitor::State::Stopped);
        QCOMPARE(qvariant_cast<BackupMonitor::State>(states[3][0]), BackupMonitor::State::Running);
        QCOMPARE(notifications.size(), 0);
        QVERIFY(gate->enumerations.load() >= 2);
    }
    void monitorStopCancelsItsBackupButKeepsManualWork()
    {
        TestDirectory dir;
        HeldAi ai;
        const auto paths = pathsIn(dir);
        configureAi(paths);
        TestBackupService service(paths, nullptr, &ai);
        const auto source = dir.path() + "/source.txt";
        writeFile(source, "one\n");
        CHECK_OK(service.addLocal(source));
        const auto id = service.idForSource(source), before = head(service, id);
        writeFile(source, "two\n");
        BackupMonitor monitor(&service);
        QSignalSpy notifications(&monitor, &BackupMonitor::notification);
        monitor.start();
        QTRY_COMPARE(ai.callbacks.size(), 1);
        bool manualFinished = false;
        service.BackupService::statistics(id, this, [&](const BackupResult<BackupStats> &reply)
                                          { manualFinished = reply.result.success; });
        monitor.stop();
        QTRY_COMPARE(monitor.state(), BackupMonitor::State::Stopped);
        QTRY_VERIFY(manualFinished);
        const auto late = ai.callbacks.first();
        late("late summary", {});
        settle(monitor, service);
        QCOMPARE(head(service, id), before);
        QCOMPARE(readFile(service.repoPath(id) + "/source.txt"), QByteArray("one\n"));
        QCOMPARE(readFile(source), QByteArray("two\n"));
        QCOMPARE(notifications.size(), 0);
    }
    void monitorSharesStartupLoadAndTracksExternalCatalogChanges()
    {
        TestDirectory dir;
        const auto paths = pathsIn(dir);
        BackupService service(paths);
        BackupMonitorOptions options;
        options.quietPeriodMs = 25;
        options.maximumCoalesceMs = 100;
        options.scanIntervalMs = options.auditIntervalMs = 60000;
        BackupMonitor monitor(&service, nullptr, options);
        QSignalSpy tasks(&service, &BackupService::taskStarted);
        monitor.start();
        QTRY_VERIFY(service.isReady());
        settle(monitor, service);
        QCOMPARE(tasks.size(), 1);
        const auto source = dir.path() + "/source.txt";
        writeFile(source, "external add");
        TestBackupService external(paths);
        CHECK_OK(external.addLocal(source));
        const auto id = external.idForSource(source);
        QTRY_VERIFY_WITH_TIMEOUT(service.contains(id), 10000);
        settle(monitor, service);
        QCOMPARE(monitor.trackedCount(), 1);
        CHECK_OK(external.removeBackup(id));
        QTRY_VERIFY_WITH_TIMEOUT(!service.contains(id), 10000);
        QCOMPARE(monitor.trackedCount(), 0);
        QCOMPARE(readFile(source), QByteArray("external add"));
    }
    void catalogChangesDuringReloadAreCoalesced()
    {
        TestDirectory dir;
        const auto paths = pathsIn(dir);
        const auto gate = std::make_shared<ScanGate>();
        gate->hold.store(false);
        BackupDependencies dependencies;
        dependencies.git = [gate](const QString &repo, const QStringList &args, const GitOptions &options)
        {
            if (gate->hold.exchange(false))
            {
                gate->entered.release();
                gate->resume.acquire();
            }
            return runGit(repo, args, options);
        };
        TestBackupService service(paths, nullptr, nullptr, dependencies);
        const auto source = dir.path() + "/source.txt";
        writeFile(source, "source");
        CHECK_OK(service.addLocal(source));
        const auto target = service.observationTargets().first();
        auto external = record(service, target.id);
        BackupMonitorOptions options;
        options.quietPeriodMs = 25;
        options.maximumCoalesceMs = 100;
        options.scanIntervalMs = options.auditIntervalMs = 60000;
        BackupMonitor monitor(&service, nullptr, options);
        monitor.start();
        settle(monitor, service);
        QTest::qWait(200);
        settle(monitor, service);
        QSignalSpy reloads(&service, &BackupService::reloadFinished);
        const auto releaseOnFailure = qScopeGuard([&]
                                                  { gate->resume.release(); });
        gate->hold.store(true);
        service.BackupService::reload(this);
        QTRY_VERIFY(gate->entered.available() > 0);
        BackupCatalog catalog(paths);
        for (int i = 0; i < 10; ++i)
        {
            external.stateDetail = QString::number(i);
            CHECK_OK(catalog.save(external));
            QCoreApplication::processEvents();
        }
        QTest::qWait(1200);
        QCOMPARE(reloads.size(), 0);
        gate->resume.release();
        QTRY_COMPARE_WITH_TIMEOUT(reloads.size(), 2, 10000);
        settle(monitor, service);
        QTest::qWait(1200);
        settle(monitor, service);
        QCOMPARE(reloads.size(), 2);
        QVERIFY(service.observationTargets().first().version > target.version);
        QCOMPARE(record(service, target.id).fingerprint, target.fingerprint);
    }
    void periodicMonitorAuditFindsExternalGitChanges()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/source.txt";
        writeFile(source, "local\n");
        CHECK_OK(service.addLocal(source));
        const auto target = service.observationTargets().first();
        qint64 now = 0;
        BackupMonitorDependencies dependencies;
        dependencies.clock = [&]
        { return now; };
        BackupMonitor monitor(&service, nullptr, {}, dependencies);
        monitor.start();
        settle(monitor, service);
        const auto repo = service.repoPath(target.id);
        writeFile(repo + "/source.txt", "external\n");
        git(repo, {"add", "--all"});
        git(repo, {"commit", "-m", "external edit"});
        QTest::qWait(100);
        now = 29999;
        monitor.reconcile();
        settle(monitor, service);
        QCOMPARE(service.syncState(target.id), BackupSyncState::Tracking);
        now = 30000;
        monitor.reconcile();
        settle(monitor, service);
        QCOMPARE(service.syncState(target.id), BackupSyncState::NeedsAttention);
        QCOMPARE(record(service, target.id).fingerprint, target.fingerprint);
        QCOMPARE(readFile(source), QByteArray("local\n"));
    }
    void removingATargetCancelsItsScanAndAllowsReaddingIt()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/source";
        writeFile(source + "/file.txt", "initial");
        CHECK_OK(service.addLocal(source));
        const auto id = service.idForSource(source);
        const auto generation = service.repositoryGeneration(id);
        const auto gate = std::make_shared<ScanGate>();
        BackupMonitorDependencies dependencies;
        dependencies.scanFiles = [gate]
        { return std::make_unique<HeldScanFiles>(gate); };
        BackupMonitor monitor(&service, nullptr, {}, dependencies);
        QSignalSpy requested(&monitor, &BackupMonitor::backupRequested), notifications(&monitor, &BackupMonitor::notification);
        monitor.start();
        QTRY_VERIFY(gate->entered.available() > 0);
        CHECK_OK(service.removeBackup(id));
        settle(monitor, service);
        QCOMPARE(monitor.trackedCount(), 0);
        CHECK_OK(service.addLocal(source));
        QCOMPARE(service.idForSource(source), id);
        QVERIFY(service.repositoryGeneration(id) > generation);
        settle(monitor, service);
        QCOMPARE(requested.size(), 0);
        QCOMPARE(notifications.size(), 0);
        writeFile(source + "/file.txt", "new generation");
        monitor.scanNow();
        settle(monitor, service);
        QCOMPARE(readFile(service.repoPath(id) + "/source/file.txt"), QByteArray("new generation"));
    }
    void monitorAndServiceCanBeDestroyedIndependently()
    {
        TestDirectory dir;
        auto service = std::make_unique<TestBackupService>(pathsIn(dir));
        const auto source = dir.path() + "/source";
        writeFile(source + "/file.txt", "initial");
        CHECK_OK(service->addLocal(source));
        auto gate = std::make_shared<ScanGate>();
        BackupMonitorDependencies dependencies;
        dependencies.scanFiles = [gate]
        { return std::make_unique<HeldScanFiles>(gate); };
        auto monitor = std::make_unique<BackupMonitor>(service.get(), nullptr, BackupMonitorOptions{}, dependencies);
        monitor->start();
        QTRY_VERIFY(gate->entered.available() > 0);
        monitor.reset(); // Joins a cancelled read without a nested UI event loop.
        QVERIFY(service->isReady());
        gate = std::make_shared<ScanGate>();
        dependencies.scanFiles = [gate]
        { return std::make_unique<HeldScanFiles>(gate); };
        monitor = std::make_unique<BackupMonitor>(service.get(), nullptr, BackupMonitorOptions{}, dependencies);
        monitor->start();
        QTRY_VERIFY(gate->entered.available() > 0);
        service.reset();
        QTRY_COMPARE(monitor->state(), BackupMonitor::State::Stopped);
        monitor->start();
        QCOMPARE(monitor->state(), BackupMonitor::State::Stopped);
    }
    void catalogWatchesAllowAtomicRecordReplacement()
    {
        TestDirectory dir;
        const auto paths = pathsIn(dir);
        TestBackupService service(paths);
        const auto source = dir.path() + "/source.txt";
        writeFile(source, "one");
        CHECK_OK(service.addLocal(source));
        const auto target = service.observationTargets().first();
        const auto original = record(service, target.id);
        BackupCatalogWatcher watcher(paths.backupRoot);
        watcher.setTargets({target});
        watcher.start();
        auto saves = std::async(std::launch::async, [paths, original]
                                {
            BackupCatalog catalog(paths);
            auto changed = original;
            for (int i = 0; i < 200; ++i)
            {
                changed.stateDetail = QString::number(i);
                auto result = catalog.save(changed);
                if (!result.success)
                {
                    result.message.prepend(QString("save %1: ").arg(i));
                    return result;
                }
            }
            return OperationResult::ok({}); });
        QTRY_VERIFY_WITH_TIMEOUT(saves.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 15000);
        CHECK_OK(saves.get());
    }
    void monitorSchedulerCoalescesAndAudits()
    {
        qint64 now = 0;
        BackupMonitorScheduler scheduler([&]
                                         { return now; });
        BackupObservationTarget target{"one", "/source.txt", false, 1, 1};
        scheduler.reconcile({target});
        auto request = scheduler.takeScan();
        QVERIFY(request);
        QVERIFY(!scheduler.takeScan());
        QVERIFY(scheduler.completeScan({*request, BackupScanStatus::Unchanged, OperationResult::ok({})}));
        QCOMPARE(scheduler.nextDeadline(), qint64(30000));
        now = 100;
        scheduler.markDirty(target.id);
        now = 599;
        QVERIFY(!scheduler.takeScan());
        now = 600;
        request = scheduler.takeScan();
        QVERIFY(request);
        scheduler.completeScan({*request, BackupScanStatus::Unchanged, OperationResult::ok({})});

        now = 1000;
        scheduler.markDirty(target.id);
        for (now = 1100; now < 6000; now += 100)
        {
            scheduler.markDirty(target.id);
            QVERIFY(!scheduler.takeScan());
        }
        request = scheduler.takeScan();
        QVERIFY(request); // Continuous events cannot postpone the check past five seconds.
        scheduler.completeScan({*request, BackupScanStatus::Unchanged, OperationResult::ok({})});
        now = 35999;
        QVERIFY(!scheduler.takeScan());
        now = 36000;
        QVERIFY(scheduler.takeScan());
    }
    void monitorSchedulerRetainsChangesAndInvalidatesSnapshots()
    {
        qint64 now = 0;
        BackupMonitorScheduler scheduler([&]
                                         { return now; });
        BackupObservationTarget target{"one", "/source.txt", false, 1, 1};
        scheduler.reconcile({target});
        const auto initial = scheduler.takeScan();
        QVERIFY(initial);
        now = 100;
        scheduler.markDirty(target.id);
        scheduler.completeScan({*initial, BackupScanStatus::Changed, OperationResult::ok({})});
        const auto backup = scheduler.takeBackup();
        QVERIFY(backup);
        QVERIFY(!scheduler.takeBackup());
        QVERIFY(!scheduler.takeScan());
        now = 200;
        scheduler.markDirty(target.id);
        scheduler.completeBackup(*backup, OperationResult::ok({}));
        now = 699;
        QVERIFY(!scheduler.takeScan());
        now = 700;
        const auto followup = scheduler.takeScan();
        QVERIFY(followup);
        QVERIFY(followup->changeVersion > initial->changeVersion);
        ++target.version;
        scheduler.reconcile({target});
        QVERIFY(!scheduler.completeScan({*followup, BackupScanStatus::Changed, OperationResult::ok({})}));
        QVERIFY(!scheduler.takeBackup());
        const auto current = scheduler.takeScan();
        QVERIFY(current);
        QCOMPARE(current->target.version, target.version);
        scheduler.reconcile({});
        QVERIFY(!scheduler.completeScan({*current, BackupScanStatus::Changed, OperationResult::ok({})}));
        QVERIFY(!scheduler.hasWork());
    }
    void monitorSchedulerBoundsWorkAndRetries()
    {
        qint64 now = 0;
        BackupMonitorScheduler scheduler([&]
                                         { return now; });
        BackupObservationTarget first{"one", "/one", false, 1, 1}, second{"two", "/two", false, 1, 2};
        scheduler.reconcile({first, second});
        auto request = scheduler.takeScan();
        QVERIFY(request);
        QCOMPARE(request->target.id, first.id);
        scheduler.completeScan({*request, BackupScanStatus::Changed, OperationResult::ok({})});
        const auto backup = scheduler.takeBackup();
        QVERIFY(backup);
        request = scheduler.takeScan();
        QVERIFY(request);
        QCOMPARE(request->target.id, second.id);
        scheduler.completeScan({*request, BackupScanStatus::Changed, OperationResult::ok({})});
        QVERIFY(!scheduler.takeBackup());
        scheduler.completeBackup(*backup, OperationResult::fail("test", "retry"));
        const auto secondBackup = scheduler.takeBackup();
        QVERIFY(secondBackup);
        QCOMPARE(secondBackup->target.id, second.id);
        scheduler.completeBackup(*secondBackup, OperationResult::ok({}));
        QCOMPARE(scheduler.phase(first.id), BackupMonitorScheduler::Phase::BackingOff);
        now = 1499;
        scheduler.requestScan(first.id);
        QVERIFY(!scheduler.takeScan());
        qint64 delay = 1500;
        for (int failure = 0; failure < 7; ++failure)
        {
            now = delay;
            request = scheduler.takeScan();
            QVERIFY(request);
            QCOMPARE(request->target.id, first.id);
            scheduler.completeScan({*request, BackupScanStatus::Changed, OperationResult::ok({})});
            const auto retry = scheduler.takeBackup();
            QVERIFY(retry);
            scheduler.completeBackup(*retry, OperationResult::fail("test", "retry"));
            const auto expected = qMin<qint64>(60000, qint64(3000) << failure);
            // Keep the unrelated item's periodic check from obscuring this deadline.
            scheduler.reconcile({first});
            QCOMPARE(scheduler.nextDeadline(), now + expected);
            delay = now + expected;
        }
    }
    void monitorSchedulerPausesWritesAndResumesImmediately()
    {
        qint64 now = 0;
        BackupMonitorScheduler scheduler([&]
                                         { return now; });
        BackupObservationTarget target{"one", "/source", false, 1, 1, BackupSyncState::RemotePending};
        scheduler.reconcile({target});
        const auto request = scheduler.takeScan();
        QVERIFY(request);
        scheduler.completeScan({*request, BackupScanStatus::Changed, OperationResult::ok({})});
        QVERIFY(!scheduler.takeBackup());
        QCOMPARE(scheduler.phase(target.id), BackupMonitorScheduler::Phase::Paused);
        target.state = BackupSyncState::Tracking;
        ++target.version;
        scheduler.reconcile({target});
        QVERIFY(scheduler.takeScan());
        scheduler.clear();
        QVERIFY(!scheduler.hasWork());
    }
    void monitorSchedulerCancellationDoesNotBackOff()
    {
        qint64 now = 0;
        BackupMonitorScheduler scheduler([&]
                                         { return now; });
        BackupObservationTarget target{"one", "/source", false, 1, 1};
        scheduler.reconcile({target});
        const auto scan = scheduler.takeScan();
        QVERIFY(scan);
        scheduler.completeScan({*scan, BackupScanStatus::Changed, OperationResult::ok({})});
        const auto backup = scheduler.takeBackup();
        QVERIFY(backup);
        QVERIFY(!scheduler.completeBackup(*backup, OperationResult::cancel("cancelled")));
        QCOMPARE(scheduler.nextDeadline(), now);
        const auto again = scheduler.takeScan();
        QVERIFY(again);
        scheduler.clear();
        scheduler.reconcile({target});
        const auto restarted = scheduler.takeScan();
        QVERIFY(restarted);
        QVERIFY(restarted->token != again->token);
        QVERIFY(!scheduler.completeScan({*again, BackupScanStatus::Changed, OperationResult::ok({})}));
        QVERIFY(!scheduler.takeBackup());
        QVERIFY(scheduler.completeScan({*restarted, BackupScanStatus::Unchanged, OperationResult::ok({})}));
    }
    void backgroundServiceTasksAreFairAndBusyDoesNotFlicker()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        CHECK_OK(awaitBackup<OperationResult>([&](auto f)
                                              { service.BackupService::reload(this, f, {BackupTaskPriority::Background, false}); }));
        QSignalSpy started(&service, &BackupService::taskStarted), busy(&service, &BackupService::busyChanged);
        const auto background = service.BackupService::reload(this, {}, {BackupTaskPriority::Background, false});
        const auto lastBackground = service.BackupService::reload(this, {}, {BackupTaskPriority::Background, false});
        QVector<BackupTaskId> foreground;
        for (int i = 0; i < 10; ++i)
            foreground.append(service.BackupService::reload(this, {}, {BackupTaskPriority::Foreground, false}));
        QVERIFY(service.isReloading());
        settle(service);
        QVERIFY(!service.isReloading());
        QCOMPARE(started.size(), 12);
        for (int i = 0; i < 8; ++i)
            QCOMPARE(started[i][0].toULongLong(), foreground[i]);
        QCOMPARE(started[8][0].toULongLong(), background);
        QCOMPARE(started[9][0].toULongLong(), foreground[8]);
        QCOMPARE(started[10][0].toULongLong(), foreground[9]);
        QCOMPARE(started[11][0].toULongLong(), lastBackground);
        QCOMPARE(busy.size(), 2);
        QCOMPARE(busy[0][0].toBool(), true);
        QCOMPARE(busy[1][0].toBool(), false);
    }
    void foregroundWorkBeforeBackgroundArrivesDoesNotConsumeItsQuota()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        for (int i = 0; i < 8; ++i)
            service.BackupService::reload(this, {}, {BackupTaskPriority::Foreground, false});
        settle(service);
        QSignalSpy started(&service, &BackupService::taskStarted);
        const auto background = service.BackupService::reload(this, {}, {BackupTaskPriority::Background, false});
        const auto foreground = service.BackupService::reload(this, {}, {BackupTaskPriority::Foreground, false});
        settle(service);
        QCOMPARE(started.size(), 2);
        QCOMPARE(started[0][0].toULongLong(), foreground);
        QCOMPARE(started[1][0].toULongLong(), background);
    }
    void observationSnapshotsChangeOnlyWhenRecordsChange()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/source.txt";
        writeFile(source, "one\n");
        CHECK_OK(service.addLocal(source));
        const auto before = service.observationTargets().first();
        QVERIFY(before.version > 0);
        QVERIFY(!before.directory);
        CHECK_OK(service.reload());
        QCOMPARE(service.observationTargets().first().version, before.version);
        writeFile(source, "two\n");
        CHECK_OK(service.backup(before.id));
        const auto after = service.observationTargets().first();
        QVERIFY(after.version > before.version);
        QVERIFY(after.fingerprint != before.fingerprint);
        QCOMPARE(after.generation, before.generation);
        CHECK_OK(service.reload());
        QCOMPARE(service.observationTargets().first().version, after.version);
    }
    void pullPausesQueuedBackupsAndSurvivesRestart_data()
    {
        QTest::addColumn<bool>("applyToSource");
        QTest::newRow("apply-pulled-version") << true;
        QTest::newRow("keep-source-create-version") << false;
    }
    void pullPausesQueuedBackupsAndSurvivesRestart()
    {
        QFETCH(bool, applyToSource);
        RemoteFixture f;
        const auto baseline = record(*f.service, f.id);
        const auto remoteCommit = f.commitRemote("pulled\n");
        writeFile(f.source, "local edits while pulling\n");
        OperationResult pull, backup;
        f.service->synchronize(f.id, false, this, [&](const OperationResult &r)
                               { pull = r; });
        f.service->backup(f.id, this, [&](const OperationResult &r)
                          { backup = r; }, {true, BackupTaskPriority::Foreground});
        settle(*f.service);
        CHECK_OK(pull);
        QVERIFY(!backup.success);
        QCOMPARE(head(*f.service, f.id), remoteCommit);
        QCOMPARE(readFile(f.source), QByteArray("local edits while pulling\n"));
        QCOMPARE(f.service->syncState(f.id), BackupSyncState::RemotePending);
        QCOMPARE(record(*f.service, f.id).fingerprint, baseline.fingerprint);
        QCOMPARE(record(*f.service, f.id).lastCommit, baseline.lastCommit);
        f.service.reset();
        f.service = std::make_unique<TestBackupService>(f.paths);
        QCOMPARE(f.service->syncState(f.id), BackupSyncState::RemotePending);
        CHECK_OK(f.service->setRemote(f.id, f.remote));
        QVERIFY(!f.service->synchronize(f.id, false).success);
        QVERIFY(!f.service->rebuild(f.id).success);
        QVERIFY(!f.service->restore(f.id, baseline.lastCommit).success);
        CHECK_OK(f.service->synchronize(f.id, true));
        CHECK_OK(f.service->preview(f.id, remoteCommit));
        QVector<Revision> history;
        CHECK_OK(f.service->history(f.id, history));
        QCOMPARE(history.first().hash, remoteCommit);

        // Preparing and cancelling a confirmation changes neither source nor state.
        const auto discarded = f.service->preparePullResolution(f.id);
        CHECK_OK(discarded.result);
        writeFile(f.source, "continued source edits\n");
        BackupMonitor monitor(f.service.get());
        monitor.start();
        monitor.scanNow();
        settle(monitor, *f.service);
        QCOMPARE(head(*f.service, f.id), remoteCommit);
        QCOMPARE(record(*f.service, f.id).fingerprint, baseline.fingerprint);
        QVERIFY(!f.service->resolvePull(discarded.value, applyToSource).success);
        QCOMPARE(f.service->syncState(f.id), BackupSyncState::RemotePending);
        const auto confirmed = f.service->preparePullResolution(f.id);
        CHECK_OK(confirmed.result);
        CHECK_OK(f.service->resolvePull(confirmed.value, applyToSource));
        QCOMPARE(f.service->syncState(f.id), BackupSyncState::Tracking);
        if (applyToSource)
        {
            QCOMPARE(readFile(f.source), QByteArray("pulled\n"));
            QCOMPARE(head(*f.service, f.id), remoteCommit);
        }
        else
        {
            QCOMPARE(readFile(f.source), QByteArray("continued source edits\n"));
            QCOMPARE(git(f.service->repoPath(f.id), {"rev-parse", "HEAD^"}).output.trimmed(), remoteCommit);
        }
        QVERIFY(runGit(f.service->repoPath(f.id), {"merge-base", "--is-ancestor", remoteCommit, "HEAD"}).success());
        const auto resolvedHead = head(*f.service, f.id);
        monitor.scanNow();
        settle(monitor, *f.service);
        QCOMPARE(head(*f.service, f.id), resolvedHead);
    }
    void pullWithEqualContentStillRequiresResolution()
    {
        RemoteFixture f;
        const auto remoteCommit = f.commitRemote("same\n");
        writeFile(f.source, "same\n");
        CHECK_OK(f.service->synchronize(f.id, false));
        QCOMPARE(f.service->syncState(f.id), BackupSyncState::RemotePending);
        const auto request = f.service->preparePullResolution(f.id);
        CHECK_OK(request.result);
        CHECK_OK(f.service->resolvePull(request.value, false));
        QCOMPARE(f.service->syncState(f.id), BackupSyncState::Tracking);
        QCOMPARE(head(*f.service, f.id), remoteCommit);
    }
    void unrelatedPullDoesNotAdvanceSourceFingerprint()
    {
        RemoteFixture f;
        const auto baseline = record(*f.service, f.id);
        const auto remoteCommit = f.commitRemote("unmanaged\n", "other.txt");
        writeFile(f.source, "local only\n");
        CHECK_OK(f.service->synchronize(f.id, false));
        QCOMPARE(f.service->syncState(f.id), BackupSyncState::Tracking);
        QCOMPARE(record(*f.service, f.id).fingerprint, baseline.fingerprint);
        QCOMPARE(record(*f.service, f.id).lastCommit, remoteCommit);
        BackupMonitor monitor(f.service.get());
        monitor.start();
        monitor.scanNow();
        settle(monitor, *f.service);
        QCOMPARE(git(f.service->repoPath(f.id), {"show", "HEAD:source.txt"}).bytes, QByteArray("local only\n"));
        QCOMPARE(git(f.service->repoPath(f.id), {"show", "HEAD:other.txt"}).bytes, QByteArray("unmanaged\n"));
        QCOMPARE(git(f.service->repoPath(f.id), {"rev-parse", "HEAD^"}).output.trimmed(), remoteCommit);
    }
    void pullHonorsActualUpstreamAndRejectsDivergence()
    {
        RemoteFixture f({}, "release/example");
        const auto repo = f.service->repoPath(f.id);
        git(repo, {"remote", "rename", "origin", "team"});
        git(repo, {"branch", "-m", "working"});
        git(repo, {"remote", "add", "origin", f.dir.path() + "/does-not-exist.git"});
        auto remoteCommit = f.commitRemote("upstream\n");
        CHECK_OK(f.service->synchronize(f.id, false));
        QCOMPARE(head(*f.service, f.id), remoteCommit);
        const auto request = f.service->preparePullResolution(f.id);
        CHECK_OK(request.result);
        CHECK_OK(f.service->resolvePull(request.value, true));
        BackupStats stats;
        CHECK_OK(f.service->statistics(f.id, stats));
        QCOMPARE(stats.remoteUrl, f.remote);
        writeFile(f.source, "local branch\n");
        CHECK_OK(f.service->backup(f.id));
        const auto localHead = head(*f.service, f.id);
        f.commitRemote("diverged branch\n");
        QVERIFY(!f.service->synchronize(f.id, false).success);
        QCOMPARE(head(*f.service, f.id), localHead);
        QCOMPARE(f.service->syncState(f.id), BackupSyncState::Tracking);
        QVERIFY(git(repo, {"status", "--porcelain"}).output.isEmpty());
    }
    void removedRemoteMappingNeverDeletesSource()
    {
        RemoteFixture f;
        git(f.writer, {"rm", "source.txt"});
        git(f.writer, {"commit", "-m", "remove managed path"});
        git(f.writer, {"push"});
        CHECK_OK(f.service->synchronize(f.id, false));
        QCOMPARE(f.service->syncState(f.id), BackupSyncState::RemotePending);
        const auto result = f.service->preparePullResolution(f.id);
        QVERIFY(!result.result.success);
        QVERIFY(result.result.message.contains("source.txt"));
        QCOMPARE(readFile(f.source), QByteArray("initial\n"));
        QVERIFY(!f.service->backup(f.id).success);
    }
    void pullCannotRunWithoutDurablePause()
    {
        bool fail = false;
        BackupDependencies dependencies;
        dependencies.allowSave = [&](const BackupRecord &r)
        { return !(fail && r.operation == "pull"); };
        RemoteFixture f(dependencies);
        const auto before = head(*f.service, f.id);
        f.commitRemote("remote\n");
        fail = true;
        QVERIFY(!f.service->synchronize(f.id, false).success);
        QCOMPARE(head(*f.service, f.id), before);
        QCOMPARE(f.service->syncState(f.id), BackupSyncState::Tracking);
        QVERIFY(!QFileInfo::exists(f.service->repoPath(f.id) + "/.git/FETCH_HEAD"));
    }
    void uncertainPullRequiresAttention()
    {
        bool fail = false;
        BackupDependencies dependencies;
        dependencies.git = [&](const QString &path, const QStringList &args, const GitOptions &options)
        {
            const auto result = runGit(path, args, options);
            return fail && args.contains("merge") && result.success() ? failedGit() : result;
        };
        RemoteFixture f(dependencies);
        const auto remoteCommit = f.commitRemote("remote\n");
        fail = true;
        QVERIFY(!f.service->synchronize(f.id, false).success);
        QCOMPARE(head(*f.service, f.id), remoteCommit);
        QCOMPARE(f.service->syncState(f.id), BackupSyncState::NeedsAttention);
        QCOMPARE(readFile(f.source), QByteArray("initial\n"));
        QVERIFY(!f.service->backup(f.id).success);
        CHECK_OK(f.service->recheck(f.id));
        QCOMPARE(f.service->syncState(f.id), BackupSyncState::RemotePending);
    }
    void backupFailureRetainsSuccessfulBaseline_data()
    {
        QTest::addColumn<QString>("failure");
        for (const auto *name : {"copy", "replacement", "add", "commit", "initial-record"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }
    void backupFailureRetainsSuccessfulBaseline()
    {
        QFETCH(QString, failure);
        TestDirectory dir;
        const auto source = dir.path() + "/source.txt";
        bool enabled = false;
        auto files = std::make_shared<FaultFiles>();
        files->failCopy = [&](const QString &, const QString &)
        { return enabled && failure == "copy"; };
        files->failRename = [&](const QString &from, const QString &)
        { return enabled && failure == "replacement" && from.endsWith("/new"); };
        BackupDependencies dependencies;
        dependencies.files = files;
        dependencies.allowSave = [&](const BackupRecord &r)
        { return !(enabled && failure == "initial-record" && r.operation == "backup"); };
        dependencies.git = [&](const QString &path, const QStringList &args, const GitOptions &options)
        {
            if (enabled && ((failure == "commit" && args.contains("commit")) || (failure == "add" && args.contains("add"))))
                return failedGit();
            return runGit(path, args, options);
        };
        TestBackupService service(pathsIn(dir), nullptr, nullptr, dependencies);
        writeFile(source, "one\n");
        CHECK_OK(service.addLocal(source));
        const auto id = service.idForSource(source);
        const auto before = record(service, id);
        writeFile(source, "two\n");
        enabled = true;
        QVERIFY(!service.backup(id).success);
        QCOMPARE(head(service, id), before.lastCommit);
        QCOMPARE(readFile(service.repoPath(id) + "/source.txt"), QByteArray("one\n"));
        QCOMPARE(readFile(source), QByteArray("two\n"));
        QCOMPARE(record(service, id).fingerprint, before.fingerprint);
        QCOMPARE(service.syncState(id), BackupSyncState::Tracking);
        QVERIFY(git(service.repoPath(id), {"status", "--porcelain"}).output.isEmpty());
        enabled = false;
        CHECK_OK(service.backup(id));
        QCOMPARE(git(service.repoPath(id), {"show", "HEAD:source.txt"}).bytes, QByteArray("two\n"));
    }
    void finalRecordFailurePreservesCopiesAndPauses()
    {
        TestDirectory dir;
        bool fail = false;
        BackupDependencies dependencies;
        dependencies.allowSave = [&](const BackupRecord &r)
        { return !(fail && r.operation.isEmpty()); };
        TestBackupService service(pathsIn(dir), nullptr, nullptr, dependencies);
        const auto source = dir.path() + "/source.txt";
        writeFile(source, "one\n");
        CHECK_OK(service.addLocal(source));
        const auto id = service.idForSource(source), initial = head(service, id);
        writeFile(source, "two\n");
        fail = true;
        const auto result = service.backup(id);
        QVERIFY(!result.success);
        QVERIFY(!result.warning.isEmpty());
        QCOMPARE(service.syncState(id), BackupSyncState::NeedsAttention);
        QVERIFY(head(service, id) != initial);
        const auto interrupted = record(service, id);
        QCOMPARE(interrupted.lastCommit, initial);
        QVERIFY(!interrupted.operation.isEmpty());
        QVERIFY(!interrupted.recoveryPaths.isEmpty());
        QCOMPARE(readFile(interrupted.recoveryPaths.first() + "/old"), QByteArray("one\n"));
        QVERIFY(!service.backup(id).success);
        fail = false;
        CHECK_OK(service.reload());
        QCOMPARE(service.syncState(id), BackupSyncState::NeedsAttention);
        QVERIFY(!service.recheck(id).success);
    }
    void failedPullResolutionDoesNotResumeTracking_data()
    {
        QTest::addColumn<bool>("apply");
        QTest::newRow("source-applied-state-unsaved") << true;
        QTest::newRow("source-committed-state-unsaved") << false;
    }
    void failedPullResolutionDoesNotResumeTracking()
    {
        QFETCH(bool, apply);
        bool fail = false;
        BackupDependencies dependencies;
        dependencies.allowSave = [&](const BackupRecord &r)
        { return !(fail && r.state == BackupSyncState::Tracking && r.operation.isEmpty()); };
        RemoteFixture f(dependencies);
        const auto remote = f.commitRemote("pulled\n");
        CHECK_OK(f.service->synchronize(f.id, false));
        const auto request = f.service->preparePullResolution(f.id);
        CHECK_OK(request.result);
        fail = true;
        const auto result = f.service->resolvePull(request.value, apply);
        QVERIFY(!result.success);
        QCOMPARE(f.service->syncState(f.id), BackupSyncState::NeedsAttention);
        QVERIFY(!f.service->backup(f.id).success);
        QVERIFY(runGit(f.service->repoPath(f.id), {"merge-base", "--is-ancestor", remote, "HEAD"}).success());
        const auto paused = record(*f.service, f.id);
        QVERIFY(!paused.recoveryPaths.isEmpty());
        QVERIFY(QFileInfo::exists(paused.recoveryPaths.first()));
        fail = false;
        CHECK_OK(f.service->reload());
        QCOMPARE(f.service->syncState(f.id), BackupSyncState::NeedsAttention);
    }
    void rollbackFailurePreservesCopiesAndPauses()
    {
        TestDirectory dir;
        bool fail = false;
        BackupDependencies dependencies;
        auto files = std::make_shared<FaultFiles>();
        files->failRename = [&](const QString &from, const QString &)
        { return fail && from.endsWith("/old"); };
        dependencies.files = files;
        dependencies.git = [&](const QString &path, const QStringList &args, const GitOptions &options)
        { return fail && args.contains("commit") ? failedGit() : runGit(path, args, options); };
        TestBackupService service(pathsIn(dir), nullptr, nullptr, dependencies);
        const auto source = dir.path() + "/source.txt";
        writeFile(source, "one\n");
        CHECK_OK(service.addLocal(source));
        const auto id = service.idForSource(source), initial = head(service, id);
        writeFile(source, "two\n");
        fail = true;
        const auto result = service.backup(id);
        QVERIFY(!result.success);
        QCOMPARE(service.syncState(id), BackupSyncState::NeedsAttention);
        QCOMPARE(head(service, id), initial);
        const auto paused = record(service, id);
        QCOMPARE(readFile(paused.recoveryPaths.first() + "/old"), QByteArray("one\n"));
        QCOMPARE(readFile(paused.recoveryPaths.first() + "/new"), QByteArray("two\n"));
        QVERIFY(!service.backup(id).success);
    }
    void copyDetectsSameSizeAndTimeChanges()
    {
        TestDirectory dir;
        const auto source = dir.path() + "/source.txt";
        bool mutate = false;
        auto files = std::make_shared<FaultFiles>();
        files->afterCopy = [&](const QString &from, const QString &)
        {
            if (!mutate || from != source)
                return;
            const auto modified = QFileInfo(source).lastModified();
            writeFile(source, "new\n");
            QFile file(source);
            file.open(QIODevice::ReadWrite);
            file.setFileTime(modified, QFileDevice::FileModificationTime);
        };
        BackupDependencies dependencies;
        dependencies.files = files;
        TestBackupService service(pathsIn(dir), nullptr, nullptr, dependencies);
        writeFile(source, "one\n");
        CHECK_OK(service.addLocal(source));
        const auto id = service.idForSource(source), before = head(service, id);
        mutate = true;
        QVERIFY(!service.backup(id).success);
        QCOMPARE(head(service, id), before);
        QCOMPARE(readFile(service.repoPath(id) + "/source.txt"), QByteArray("one\n"));
        mutate = false;
        const auto changed = scanSource(service.observationTargets().first());
        CHECK_OK(changed.result);
        QCOMPARE(changed.status, BackupScanStatus::Changed);
        CHECK_OK(service.backup(id));
        QCOMPARE(readFile(service.repoPath(id) + "/source.txt"), QByteArray("new\n"));
    }
    void installedRepositoryEditNeverBecomesSuccessfulBaseline()
    {
        TestDirectory dir;
        auto files = std::make_shared<FaultFiles>();
        BackupDependencies dependencies;
        dependencies.files = files;
        TestBackupService service(pathsIn(dir), nullptr, nullptr, dependencies);
        const auto source = dir.path() + "/source.txt";
        writeFile(source, "initial\n");
        CHECK_OK(service.addLocal(source));
        const auto id = service.idForSource(source), target = service.repoPath(id) + "/source.txt";
        const auto before = record(service, id);
        writeFile(source, "intended backup\n");
        files->afterRename = [&](const QString &from, const QString &to)
        {
            if (from.endsWith("/new") && to == target)
                writeFile(target, "external edit\n");
        };
        QVERIFY(!service.backup(id).success);
        QCOMPARE(service.syncState(id), BackupSyncState::NeedsAttention);
        QCOMPARE(head(service, id), before.lastCommit);
        QCOMPARE(record(service, id).fingerprint, before.fingerprint);
        QCOMPARE(readFile(source), QByteArray("intended backup\n"));
        QCOMPARE(readFile(target), QByteArray("external edit\n"));
        const auto retained = record(service, id).recoveryPaths;
        QVERIFY(!retained.isEmpty());
        QCOMPARE(readFile(retained.first() + "/old"), QByteArray("initial\n"));
        CHECK_OK(service.reload());
        QVERIFY(!service.backup(id).success);
    }
    void installedSourceEditDoesNotResolvePull()
    {
        auto files = std::make_shared<FaultFiles>();
        BackupDependencies dependencies;
        dependencies.files = files;
        RemoteFixture f(dependencies);
        const auto remote = f.commitRemote("pulled\n");
        CHECK_OK(f.service->synchronize(f.id, false));
        const auto before = record(*f.service, f.id);
        const auto request = f.service->preparePullResolution(f.id);
        CHECK_OK(request.result);
        files->afterRename = [&](const QString &from, const QString &to)
        {
            if (from.endsWith("/new") && to == f.source)
                writeFile(f.source, "external source edit\n");
        };
        QVERIFY(!f.service->resolvePull(request.value, true).success);
        QCOMPARE(f.service->syncState(f.id), BackupSyncState::NeedsAttention);
        QCOMPARE(head(*f.service, f.id), remote);
        const auto paused = record(*f.service, f.id);
        QCOMPARE(paused.lastCommit, before.lastCommit);
        QCOMPARE(paused.pendingCommit, remote);
        QCOMPARE(paused.fingerprint, before.fingerprint);
        QCOMPARE(readFile(f.source), QByteArray("external source edit\n"));
        QVERIFY(!paused.recoveryPaths.isEmpty());
        QCOMPARE(readFile(paused.recoveryPaths.first() + "/old"), QByteArray("initial\n"));
    }
    void rollbackDoesNotRestoreAnExternallyEditedRetainedCopy()
    {
        TestDirectory dir;
        QString recovery;
        bool fail = false;
        BackupDependencies dependencies;
        dependencies.allowSave = [&](const BackupRecord &r)
        {
            if (r.operation == "backup")
                recovery = r.recoveryPaths.value(0);
            return true;
        };
        dependencies.git = [&](const QString &path, const QStringList &args, const GitOptions &options)
        {
            if (fail && args.contains("commit"))
            {
                writeFile(recovery + "/old", "external retained edit\n");
                return failedGit();
            }
            return runGit(path, args, options);
        };
        TestBackupService service(pathsIn(dir), nullptr, nullptr, dependencies);
        const auto source = dir.path() + "/source.txt";
        writeFile(source, "initial\n");
        CHECK_OK(service.addLocal(source));
        const auto id = service.idForSource(source);
        const auto before = record(service, id);
        writeFile(source, "next\n");
        fail = true;
        QVERIFY(!service.backup(id).success);
        QCOMPARE(service.syncState(id), BackupSyncState::NeedsAttention);
        QCOMPARE(readFile(recovery + "/old"), QByteArray("external retained edit\n"));
        QCOMPARE(readFile(service.repoPath(id) + "/source.txt"), QByteArray("next\n"));
        QCOMPARE(head(service, id), before.lastCommit);
        QCOMPARE(record(service, id).fingerprint, before.fingerprint);
    }
    void externalStagingDuringBackupIsPreserved_data()
    {
        QTest::addColumn<QString>("during");
        QTest::newRow("staging") << QString("add");
        QTest::newRow("commit") << QString("commit");
    }
    void externalStagingDuringBackupIsPreserved()
    {
        QFETCH(QString, during);
        TestDirectory dir;
        QString repo;
        bool inject = false;
        BackupDependencies dependencies;
        dependencies.git = [&](const QString &path, const QStringList &args, const GitOptions &options)
        {
            if (inject && path == repo && args.contains(during))
            {
                inject = false;
                writeFile(repo + "/external.txt", "external staged data\n");
                git(repo, {"add", "external.txt"});
            }
            return runGit(path, args, options);
        };
        TestBackupService service(pathsIn(dir), nullptr, nullptr, dependencies);
        const auto source = dir.path() + "/source.txt";
        writeFile(source, "initial\n");
        CHECK_OK(service.addLocal(source));
        const auto id = service.idForSource(source);
        repo = service.repoPath(id);
        const auto before = record(service, id);
        writeFile(source, "intended backup\n");
        inject = true;
        QVERIFY(!service.backup(id).success);
        QCOMPARE(service.syncState(id), BackupSyncState::NeedsAttention);
        QVERIFY(!runGit(repo, {"cat-file", "-e", "HEAD:external.txt"}).success());
        QCOMPARE(git(repo, {"show", ":external.txt"}).bytes, QByteArray("external staged data\n"));
        QCOMPARE(readFile(repo + "/external.txt"), QByteArray("external staged data\n"));
        QCOMPARE(record(service, id).fingerprint, before.fingerprint);
        QCOMPARE(record(service, id).lastCommit, before.lastCommit);
        QVERIFY(!record(service, id).recoveryPaths.isEmpty());
        if (during == "add")
            QCOMPARE(head(service, id), before.lastCommit);
        else
            QCOMPARE(git(repo, {"show", "HEAD:source.txt"}).bytes, QByteArray("intended backup\n"));
    }
    void unreadableDirectoryIsNeverTreatedAsEmpty()
    {
        TestDirectory dir;
        const auto source = dir.path() + "/source";
        bool fail = false;
        auto files = std::make_shared<FaultFiles>();
        files->failChildren = [&](const QString &path)
        { return fail && path == source + "/nested"; };
        BackupDependencies dependencies;
        dependencies.files = files;
        TestBackupService service(pathsIn(dir), nullptr, nullptr, dependencies);
        writeFile(source + "/nested/keep.txt", "keep\n");
        CHECK_OK(service.addLocal(source));
        const auto id = service.idForSource(source), before = head(service, id);
        const auto fingerprint = record(service, id).fingerprint;
        fail = true;
        QVERIFY(!service.backup(id).success);
        QCOMPARE(head(service, id), before);
        QCOMPARE(record(service, id).fingerprint, fingerprint);
        QCOMPARE(readFile(service.repoPath(id) + "/source/nested/keep.txt"), QByteArray("keep\n"));
        QCOMPARE(service.syncState(id), BackupSyncState::Tracking);
    }
    void missingSourceRetriesWithoutAdvancingBaseline()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/source.txt";
        writeFile(source, "one\n");
        CHECK_OK(service.addLocal(source));
        const auto id = service.idForSource(source);
        const auto before = record(service, id);
        qint64 now = 0;
        BackupMonitorDependencies monitorDependencies;
        monitorDependencies.clock = [&]
        { return now; };
        BackupMonitor monitor(&service, nullptr, {}, monitorDependencies);
        monitor.start();
        settle(monitor, service);
        QSignalSpy notifications(&monitor, &BackupMonitor::notification);
        QVERIFY(QFile::remove(source));
        monitor.scanNow();
        settle(monitor, service);
        QCOMPARE(notifications.size(), 1);
        now = 2000;
        monitor.scanNow();
        settle(monitor, service);
        QCOMPARE(notifications.size(), 1);
        QCOMPARE(record(service, id).fingerprint, before.fingerprint);
        QCOMPARE(head(service, id), before.lastCommit);
        writeFile(source, "returned\n");
        now = 6000;
        monitor.scanNow();
        monitor.scanNow();
        monitor.scanNow();
        settle(monitor, service);
        QCOMPARE(git(service.repoPath(id), {"rev-list", "--count", "HEAD"}).output.trimmed(), QString("2"));
        QCOMPARE(readFile(service.repoPath(id) + "/source.txt"), QByteArray("returned\n"));
        monitor.stop();
        writeFile(source, "after stop\n");
        monitor.scanNow();
        settle(monitor, service);
        QCOMPARE(readFile(service.repoPath(id) + "/source.txt"), QByteArray("returned\n"));
    }
    void dirtyRepositoryAndExternalCommitRequireInspection()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/source.txt";
        writeFile(source, "one\n");
        CHECK_OK(service.addLocal(source));
        const auto id = service.idForSource(source), repo = service.repoPath(id);
        writeFile(repo + "/source.txt", "external worktree\n");
        writeFile(source, "local\n");
        QVERIFY(!service.backup(id).success);
        QCOMPARE(service.syncState(id), BackupSyncState::NeedsAttention);
        QCOMPARE(readFile(repo + "/source.txt"), QByteArray("external worktree\n"));
        git(repo, {"add", "--all"});
        git(repo, {"commit", "-m", "external version"});
        CHECK_OK(service.recheck(id));
        QCOMPARE(service.syncState(id), BackupSyncState::RemotePending);
        QVERIFY(!service.backup(id).success);
        const auto request = service.preparePullResolution(id);
        CHECK_OK(request.result);
        CHECK_OK(service.resolvePull(request.value, false));
        QCOMPARE(service.syncState(id), BackupSyncState::Tracking);
        QCOMPARE(git(repo, {"show", "HEAD^:source.txt"}).bytes, QByteArray("external worktree\n"));
    }
    void aiWaitDoesNotBlockUiAndExternalChangesArePreserved()
    {
        TestDirectory dir;
        HeldAi ai;
        const auto paths = pathsIn(dir);
        configureAi(paths);
        TestBackupService service(paths, nullptr, &ai);
        const auto source = dir.path() + "/source.txt";
        writeFile(source, "one\n");
        CHECK_OK(service.addLocal(source));
        const auto id = service.idForSource(source), repo = service.repoPath(id), initial = head(service, id);
        writeFile(source, "two\n");
        bool finished = false, uiAlive = false;
        OperationResult result;
        service.backup(id, this, [&](const OperationResult &r)
                       { result = r; finished = true; });
        QTRY_COMPARE(ai.callbacks.size(), 1);
        QTimer::singleShot(0, this, [&]
                           { uiAlive = true; });
        QTRY_VERIFY(uiAlive);
        QVERIFY(!finished);
        writeFile(repo + "/source.txt", "external during AI\n");
        ai.callbacks[0]("message", {});
        QTRY_VERIFY(finished);
        QVERIFY(!result.success);
        QCOMPARE(service.syncState(id), BackupSyncState::NeedsAttention);
        QCOMPARE(readFile(repo + "/source.txt"), QByteArray("external during AI\n"));
        QCOMPARE(head(service, id), initial);
        const auto paused = record(service, id);
        QCOMPARE(readFile(paused.recoveryPaths.first() + "/old"), QByteArray("one\n"));
    }
    void editsDoNotIncludeStagedFiles()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/source.txt";
        writeFile(source, "one\n");
        CHECK_OK(service.addLocal(source));
        const auto id = service.idForSource(source), repo = service.repoPath(id), before = head(service, id);
        writeFile(repo + "/source.txt", "staged\n");
        git(repo, {"add", "--all"});
        CHECK_OK(service.editMessage(id, before, "new description"));
        QCOMPARE(git(repo, {"show", "HEAD:source.txt"}).bytes, QByteArray("one\n"));
        QCOMPARE(git(repo, {"show", ":source.txt"}).bytes, QByteArray("staged\n"));
        QCOMPARE(git(repo, {"log", "-1", "--format=%s"}).output.trimmed(), QString("new description"));
        QVERIFY(!service.backup(id).success);
    }
    void ordinaryGitImportKeepsMappingAndHistory_data()
    {
        QTest::addColumn<QString>("entry");
        QTest::newRow("entire-root") << QString(".");
        QTest::newRow("subdirectory") << QString("docs");
        QTest::newRow("renamed-file") << QString("docs/file.txt");
    }
    void failedImportLeavesOriginalTarget_data()
    {
        QTest::addColumn<QString>("failure");
        for (const auto *name : {"copy", "replacement", "pause-record", "final-record"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }
    void failedImportLeavesOriginalTarget()
    {
        QFETCH(QString, failure);
        TestDirectory dir;
        const auto original = dir.path() + "/ordinary";
        writeFile(original + "/file.txt", "imported\n");
        git(original, {"init"});
        git(original, {"add", "--all"});
        git(original, {"commit", "-m", "original"});
        bool enabled = false;
        auto files = std::make_shared<FaultFiles>();
        files->failCopy = [&](const QString &, const QString &)
        { return enabled && failure == "copy"; };
        files->failRename = [&](const QString &from, const QString &)
        { return enabled && failure == "replacement" && from.endsWith("/new"); };
        BackupDependencies dependencies;
        dependencies.files = files;
        dependencies.allowSave = [&](const BackupRecord &r)
        {
            return !(enabled && ((failure == "pause-record" && r.operation == "import") || (failure == "final-record" && r.operation.isEmpty())));
        };
        TestBackupService service(pathsIn(dir), nullptr, nullptr, dependencies);
        const auto prepared = service.prepareImport(original);
        CHECK_OK(prepared.result);
        const auto target = dir.path() + "/target";
        writeFile(target + "/keep.txt", "original target\n");
        enabled = true;
        QVERIFY(!service.finishImport(prepared.value.sessionId, ".", target, true).success);
        QCOMPARE(readFile(target + "/keep.txt"), QByteArray("original target\n"));
        QVERIFY(!QFileInfo::exists(target + "/file.txt"));
        QVERIFY(service.trackedItems().isEmpty());
        enabled = false;
        CHECK_OK(service.finishImport(prepared.value.sessionId, ".", target, true));
        QCOMPARE(readFile(target + "/file.txt"), QByteArray("imported\n"));
        QVERIFY(!QFileInfo::exists(target + "/keep.txt"));
    }
    void installedImportEditIsPreserved()
    {
        TestDirectory dir;
        const auto original = dir.path() + "/ordinary", target = dir.path() + "/target.txt";
        writeFile(original + "/file.txt", "imported\n");
        git(original, {"init"});
        git(original, {"add", "--all"});
        git(original, {"commit", "-m", "original"});
        writeFile(target, "original target\n");
        auto files = std::make_shared<FaultFiles>();
        files->afterRename = [&](const QString &from, const QString &to)
        {
            if (from.endsWith("/new") && to == target)
                writeFile(target, "external target edit\n");
        };
        BackupDependencies dependencies;
        dependencies.files = files;
        TestBackupService service(pathsIn(dir), nullptr, nullptr, dependencies);
        const auto prepared = service.prepareImport(original);
        CHECK_OK(prepared.result);
        QVERIFY(!service.finishImport(prepared.value.sessionId, "file.txt", target, true).success);
        const auto id = service.idForSource(target);
        QVERIFY(!id.isEmpty());
        QCOMPARE(service.syncState(id), BackupSyncState::NeedsAttention);
        QCOMPARE(readFile(target), QByteArray("external target edit\n"));
        const auto paused = record(service, id);
        QVERIFY(paused.fingerprint.isEmpty());
        QVERIFY(!paused.recoveryPaths.isEmpty());
        QCOMPARE(readFile(paused.recoveryPaths.first() + "/old"), QByteArray("original target\n"));
    }
    void gitMetadataSourcesAreRejectedBeforeRunningGit_data()
    {
        QTest::addColumn<QString>("kind");
        for (const auto *kind : {"pointer", "directory", "nested-file"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void gitMetadataSourcesAreRejectedBeforeRunningGit()
    {
        QFETCH(QString, kind);
        TestDirectory dir;
        const auto donor = dir.path() + "/donor";
        writeFile(donor + "/keep.txt", "original\n");
        git(donor, {"init"});
        git(donor, {"add", "--all"});
        git(donor, {"commit", "-m", "original"});
        const auto before = git(donor, {"rev-parse", "HEAD"}).output;
        const auto config = readFile(donor + "/.git/config");
        const auto pointer = dir.path() + "/worktree/.git";
        writeFile(pointer, ("gitdir: " + donor + "/.git\n").toUtf8());
        int commands = 0;
        BackupDependencies dependencies;
        dependencies.git = [&](const QString &path, const QStringList &args, const GitOptions &options)
        {
            ++commands;
            return runGit(path, args, options);
        };
        TestBackupService service(pathsIn(dir), nullptr, nullptr, dependencies);
        const auto source = kind == "pointer" ? pointer : kind == "directory" ? donor + "/.git"
                                                                              : donor + "/.git/config";
        QVERIFY(!service.addLocal(source).success);
        QCOMPARE(commands, 0);
        QCOMPARE(git(donor, {"rev-parse", "HEAD"}).output, before);
        QCOMPARE(readFile(donor + "/.git/config"), config);
        QVERIFY(git(donor, {"status", "--porcelain"}).output.isEmpty());
        QVERIFY(service.trackedItems().isEmpty());
    }
    void importsCannotTargetSourceGitMetadata_data()
    {
        QTest::addColumn<bool>("directory");
        QTest::newRow("git-directory") << true;
        QTest::newRow("git-config-file") << false;
    }
    void importsCannotTargetSourceGitMetadata()
    {
        QFETCH(bool, directory);
        TestDirectory dir;
        const auto donor = dir.path() + "/donor", original = dir.path() + "/ordinary";
        for (const auto &repo : {donor, original})
        {
            writeFile(repo + "/keep.txt", "original\n");
            git(repo, {"init"});
            git(repo, {"add", "--all"});
            git(repo, {"commit", "-m", "original"});
        }
        const auto before = git(donor, {"rev-parse", "HEAD"}).output;
        const auto config = readFile(donor + "/.git/config");
        TestBackupService service(pathsIn(dir));
        const auto prepared = service.prepareImport(original);
        CHECK_OK(prepared.result);
        QVERIFY(!service.finishImport(prepared.value.sessionId, directory ? "." : "keep.txt",
                                      donor + (directory ? "/.git" : "/.git/config"), true)
                     .success);
        QCOMPARE(git(donor, {"rev-parse", "HEAD"}).output, before);
        QCOMPARE(readFile(donor + "/.git/config"), config);
        QVERIFY(service.trackedItems().isEmpty());
        CHECK_OK(service.finishImport(prepared.value.sessionId, ".", dir.path() + "/safe-target"));
    }
    void initializeRejectsExistingGitStorage()
    {
        TestDirectory dir;
        const auto path = dir.path() + "/candidate";
        writeFile(path + "/.git", ("gitdir: " + dir.path() + "/other/.git\n").toUtf8());
        int commands = 0;
        GitRepository repository(path, [&](const QString &repo, const QStringList &args, const GitOptions &options)
                                 {
            ++commands;
            return runGit(repo, args, options); });
        QVERIFY(!repository.initialize().success);
        QCOMPARE(commands, 0);
    }
    void monitorStartDoesNotReloadItsOwnLock()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/source.txt";
        writeFile(source, "one\n");
        CHECK_OK(service.addLocal(source));
        QSignalSpy finished(&service, &BackupService::taskFinished);
        BackupMonitor monitor(&service);
        monitor.start();
        settle(monitor, service);
        QTest::qWait(120);
        QVERIFY(finished.size() <= 2);
        QVERIFY(!service.isBusy());
        monitor.stop();
    }
    void reloadAuditsExternalChangesAndRetainsLegacyData()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/source.txt";
        writeFile(source, "one\n");
        CHECK_OK(service.addLocal(source));
        const auto id = service.idForSource(source), repo = service.repoPath(id);
        writeFile(repo + "/source.txt", "external\n");
        writeFile(service.paths().backupRoot + "/old-percent-encoded-repository/file.txt", "legacy\n");
        CHECK_OK(service.reload());
        QCOMPARE(service.syncState(id), BackupSyncState::NeedsAttention);
        QCOMPARE(readFile(repo + "/source.txt"), QByteArray("external\n"));
        QCOMPARE(readFile(service.paths().backupRoot + "/old-percent-encoded-repository/file.txt"), QByteArray("legacy\n"));
        QVERIFY(!service.backup(id).success);
    }
    void rebuildPreservesCommittedIgnoredPaths()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/source";
        writeFile(source + "/keep.log", "tracked\n");
        CHECK_OK(service.addLocal(source));
        const auto id = service.idForSource(source), repo = service.repoPath(id);
        writeFile(source + "/.gitignore", "*.log\n");
        writeFile(source + "/skip.log", "ignored\n");
        CHECK_OK(service.backup(id));
        const auto tree = git(repo, {"rev-parse", "HEAD^{tree}"}).output.trimmed();
        const auto generation = service.repositoryGeneration(id);
        CHECK_OK(service.rebuild(id));
        QCOMPARE(git(repo, {"rev-parse", "HEAD^{tree}"}).output.trimmed(), tree);
        QCOMPARE(service.repositoryGeneration(id), generation + 1);
        QCOMPARE(git(repo, {"rev-list", "--count", "HEAD"}).output.trimmed(), QString("1"));
        QCOMPARE(readFile(repo + "/source/skip.log"), QByteArray("ignored\n"));
        QVERIFY(!runGit(repo, {"cat-file", "-e", "HEAD:source/skip.log"}).success());
        CHECK_OK(service.backup(id));
        QCOMPARE(git(repo, {"rev-list", "--count", "HEAD"}).output.trimmed(), QString("1"));
    }
    void rebuildKeepsRepositoryObjectFormat_data()
    {
        QTest::addColumn<QString>("format");
        QTest::newRow("sha1") << QString("sha1");
        QTest::newRow("sha256") << QString("sha256");
    }
    void rebuildKeepsRepositoryObjectFormat()
    {
        QFETCH(QString, format);
        TestDirectory dir;
        const auto original = dir.path() + "/ordinary";
        writeFile(original + "/file.txt", "version\n");
        git(original, {"init", "--object-format=" + format});
        git(original, {"add", "--all"});
        git(original, {"commit", "-m", "original"});
        TestBackupService service(pathsIn(dir));
        const auto prepared = service.prepareImport(original);
        CHECK_OK(prepared.result);
        const auto target = dir.path() + "/renamed.txt";
        CHECK_OK(service.finishImport(prepared.value.sessionId, "file.txt", target));
        const auto id = service.idForSource(target), repo = service.repoPath(id);
        const auto tree = git(repo, {"rev-parse", "HEAD^{tree}"}).output.trimmed();
        CHECK_OK(service.rebuild(id));
        QCOMPARE(git(repo, {"rev-parse", "--show-object-format"}).output.trimmed(), format);
        QCOMPARE(git(repo, {"rev-parse", "HEAD^{tree}"}).output.trimmed(), tree);
        QCOMPARE(head(service, id).size(), format == "sha1" ? 40 : 64);
    }
    void rebuildDoesNotDropAnExternalCommit()
    {
        TestDirectory dir;
        bool inject = false;
        QString repository, externalCommit;
        BackupDependencies dependencies;
        dependencies.git = [&](const QString &path, const QStringList &args, const GitOptions &options)
        {
            const auto result = runGit(path, args, options);
            if (inject && path == repository && args.contains("worktree") && args.contains("remove"))
            {
                inject = false;
                writeFile(path + "/source.txt", "external\n");
                git(path, {"add", "--all"});
                git(path, {"commit", "-m", "external commit"});
                externalCommit = git(path, {"rev-parse", "HEAD"}).output.trimmed();
            }
            return result;
        };
        TestBackupService service(pathsIn(dir), nullptr, nullptr, dependencies);
        const auto source = dir.path() + "/source.txt";
        writeFile(source, "one\n");
        CHECK_OK(service.addLocal(source));
        const auto id = service.idForSource(source);
        repository = service.repoPath(id);
        inject = true;
        QVERIFY(!service.rebuild(id).success);
        QVERIFY(!externalCommit.isEmpty());
        QCOMPARE(head(service, id), externalCommit);
        QCOMPARE(readFile(repository + "/source.txt"), QByteArray("external\n"));
        QCOMPARE(readFile(source), QByteArray("one\n"));
        QCOMPARE(service.syncState(id), BackupSyncState::NeedsAttention);
    }
    void ordinaryGitImportKeepsMappingAndHistory()
    {
        QFETCH(QString, entry);
        TestDirectory dir;
        const auto original = dir.path() + "/ordinary";
        writeFile(original + "/docs/file.txt", "one\n");
        writeFile(original + "/root.txt", "root\n");
        git(original, {"init", "--initial-branch=topic/import"});
        git(original, {"add", "--all"});
        git(original, {"commit", "-m", "original paths"});
        const auto initial = git(original, {"rev-parse", "HEAD"}).output.trimmed();
        writeFile(original + "/docs/file.txt", "two\n");
        git(original, {"add", "--all"});
        git(original, {"commit", "-m", "second version"});
        TestBackupService service(pathsIn(dir));
        const auto prepared = service.prepareImport(original);
        CHECK_OK(prepared.result);
        QVERIFY(prepared.value.entries.size() >= 4);
        QCOMPARE(prepared.value.suggestedPath, QString("."));
        const auto target = dir.path() + "/renamed-target";
        CHECK_OK(service.finishImport(prepared.value.sessionId, entry, target));
        const auto id = service.idForSource(target), repo = service.repoPath(id);
        const auto file = entry == "." ? target + "/docs/file.txt" : entry == "docs" ? target + "/file.txt"
                                                                                     : target;
        QCOMPARE(readFile(file), QByteArray("two\n"));
        QCOMPARE(record(service, id).repositoryPath, entry);
        QCOMPARE(git(repo, {"branch", "--show-current"}).output.trimmed(), QString("topic/import"));
        QCOMPARE(git(repo, {"rev-list", "--count", "HEAD"}).output.trimmed(), QString("2"));
        CHECK_OK(service.restore(id, initial));
        QCOMPARE(readFile(file), QByteArray("one\n"));
        writeFile(file, "local mapped\n");
        CHECK_OK(service.backup(id));
        QCOMPARE(git(repo, {"show", "HEAD:docs/file.txt"}).bytes, QByteArray("local mapped\n"));
        QCOMPARE(git(repo, {"show", "HEAD:root.txt"}).bytes, QByteArray("root\n"));
        QVERIFY(!runGit(repo, {"cat-file", "-e", "HEAD:renamed-target"}).success());
        QCOMPARE(readFile(original + "/docs/file.txt"), QByteArray("two\n"));
    }
    void sourceGitMetadataAndIgnoredFilesArePreserved()
    {
        TestDirectory dir;
        const auto source = dir.path() + "/source";
        writeFile(source + "/file.txt", "one\n");
        writeFile(source + "/nested/value.txt", "nested\n");
        git(source, {"init"});
        git(source + "/nested", {"init"});
        const auto rootConfig = readFile(source + "/.git/config"), nestedConfig = readFile(source + "/nested/.git/config");
        TestBackupService service(pathsIn(dir));
        CHECK_OK(service.addLocal(source));
        const auto id = service.idForSource(source), initial = head(service, id), repo = service.repoPath(id);
        QVERIFY(!QFileInfo::exists(repo + "/source/.git"));
        QVERIFY(!QFileInfo::exists(repo + "/source/nested/.git"));
        writeFile(source + "/new.txt", "new\n");
        CHECK_OK(service.restore(id, initial));
        QCOMPARE(readFile(source + "/.git/config"), rootConfig);
        QCOMPARE(readFile(source + "/nested/.git/config"), nestedConfig);
        QVERIFY(!QFileInfo::exists(source + "/new.txt"));
        CHECK_OK(service.removeBackup(id));
        QCOMPARE(readFile(source + "/.git/config"), rootConfig);
        QCOMPARE(readFile(source + "/nested/.git/config"), nestedConfig);
    }
    void restorePreservesGitOnlyDirectoriesAndRollsBackPartialMoves()
    {
        TestDirectory dir;
        const auto source = dir.path() + "/source";
        writeFile(source + "/a.txt", "initial a\n");
        writeFile(source + "/b.txt", "initial b\n");
        auto files = std::make_shared<FaultFiles>();
        BackupDependencies dependencies;
        dependencies.files = files;
        TestBackupService service(pathsIn(dir), nullptr, nullptr, dependencies);
        CHECK_OK(service.addLocal(source));
        const auto id = service.idForSource(source), initial = head(service, id);
        git(source, {"init"});
        QDir().mkpath(source + "/git-only/nested");
        git(source + "/git-only/nested", {"init"});
        const auto rootConfig = readFile(source + "/.git/config");
        const auto nestedConfig = readFile(source + "/git-only/nested/.git/config");
        writeFile(source + "/a.txt", "current a\n");
        writeFile(source + "/b.txt", "current b\n");
        files->failRename = [](const QString &from, const QString &)
        { return from.endsWith("/new/b.txt"); };
        QVERIFY(!service.restore(id, initial).success);
        QCOMPARE(service.syncState(id), BackupSyncState::Tracking);
        QCOMPARE(readFile(source + "/a.txt"), QByteArray("current a\n"));
        QCOMPARE(readFile(source + "/b.txt"), QByteArray("current b\n"));
        QCOMPARE(readFile(source + "/.git/config"), rootConfig);
        QCOMPARE(readFile(source + "/git-only/nested/.git/config"), nestedConfig);
        files->failRename = {};
        CHECK_OK(service.restore(id, initial));
        QCOMPARE(readFile(source + "/a.txt"), QByteArray("initial a\n"));
        QCOMPARE(readFile(source + "/b.txt"), QByteArray("initial b\n"));
        QCOMPARE(readFile(source + "/.git/config"), rootConfig);
        QCOMPARE(readFile(source + "/git-only/nested/.git/config"), nestedConfig);
    }
    void recursiveStorageAndLinksAreRejected()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        QVERIFY(!service.addLocal(dir.path()).success);
        const auto source = dir.path() + "/Backup/inside.txt";
        writeFile(source, "owned\n");
        QVERIFY(!service.addLocal(source).success);
        const auto outside = dir.path() + "/external", linked = dir.path() + "/selected";
        writeFile(outside + "/untouched.txt", "do not change\n");
        QDir().mkpath(linked);
#ifdef Q_OS_WIN
        QProcess junction;
        junction.start("powershell", {"-NoProfile", "-NonInteractive", "-Command", "New-Item -ItemType Junction -Path '" + linked + "/link' -Target '" + outside + "' | Out-Null"});
        QVERIFY(junction.waitForFinished());
        QCOMPARE(junction.exitCode(), 0);
#else
        QVERIFY(QFile::link(outside, linked + "/link"));
#endif
        QVERIFY(!service.addLocal(linked).success);
        QVERIFY(!service.addLocal(linked + "/link/untouched.txt").success);
        QCOMPARE(readFile(outside + "/untouched.txt"), QByteArray("do not change\n"));
        // Remove only the link itself before QTemporaryDir cleans this fixture.
#ifdef Q_OS_WIN
        QVERIFY(QDir().rmdir(linked + "/link"));
#else
        QVERIFY(QFile::remove(linked + "/link"));
#endif
    }
    void concurrentProcessCannotWriteLockedStore()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/source.txt";
        writeFile(source, "one\n");
        CHECK_OK(service.addLocal(source));
        QLockFile lock(service.paths().backupRoot + "/.writer.lock");
        QVERIFY(lock.tryLock());
        const auto other = dir.path() + "/other.txt";
        writeFile(other, "other\n");
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--add-fixture", service.paths().backupRoot, other});
        QVERIFY(child.waitForFinished(15000));
        QCOMPARE(child.exitCode(), 2);
        QVERIFY(!service.addLocal(other).success);
        lock.unlock();
        CHECK_OK(service.addLocal(other));
        QCOMPARE(service.trackedItems().size(), 2);
    }
    void interruptedProcessLeavesDurablePause()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/source.txt";
        writeFile(source, "one\n");
        CHECK_OK(service.addLocal(source));
        const auto id = service.idForSource(source), initial = head(service, id);
        writeFile(source, "two\n");
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--prepare-and-hold", service.paths().backupRoot, id});
        QVERIFY(child.waitForStarted());
        QVERIFY(child.waitForReadyRead(15000));
        QCOMPARE(child.readAllStandardOutput().trimmed(), QByteArray("prepared"));
        child.kill();
        QVERIFY(child.waitForFinished());
        CHECK_OK(service.reload());
        QCOMPARE(service.syncState(id), BackupSyncState::NeedsAttention);
        QVERIFY(!service.backup(id).success);
        QCOMPARE(head(service, id), initial);
        const auto interrupted = record(service, id);
        QCOMPARE(readFile(interrupted.recoveryPaths.first() + "/old"), QByteArray("one\n"));
        QCOMPARE(readFile(service.repoPath(id) + "/source.txt"), QByteArray("two\n"));
    }

  private:
    TestDirectory m_environment;
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() == 4 && args[1] == "--add-fixture")
    {
        BackupService service({args[2], args[2] + "/fixture-config.ini"});
        service.addLocal(args[3], &app, [&](const OperationResult &result)
                         { app.exit(result.success ? 0 : 2); });
        return app.exec();
    }
    if (args.size() == 4 && args[1] == "--prepare-and-hold")
    {
        BackupEngine engine({args[2], args[2] + "/fixture-config.ini"}, {});
        if (!engine.begin(std::make_shared<std::atomic_bool>(false)).success)
            return 3;
        const auto work = engine.prepareBackup(args[3]);
        if (!work.result.success || !work.value)
            return 4;
        std::puts("prepared");
        std::fflush(stdout);
        return app.exec();
    }
    BackupCoreRegression tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "backup_core.moc"
