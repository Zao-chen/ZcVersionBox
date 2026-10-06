#include "backup_test_support.h"
#include "utils/backup_engine.h"
#include "utils/backup_merge.h"
#include "utils/backupmonitor.h"
#include "utils/backupmonitor_catalog.h"
#include "utils/backupmonitor_scheduler.h"
#include "utils/diff_parser.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
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
        if (service->branchContext(id).ref != "refs/heads/" + branch &&
            !service->renameBranch(id, service->branchContext(id).ref, branch).success)
            qFatal("Cannot prepare branch");
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
    void branchReferencesHistoryAndExternalIdentity()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/work.txt";
        writeFile(source, "original\n"); CHECK_OK(service.addLocal(source));
        const auto id = service.idForSource(source);
        BranchRequest request; request.context = service.branchContext(id);
        request.name = "试验方案"; request.startCommit = request.context.head;
        auto create = awaitBackup<OperationResult>([&](auto f) { service.createBranch(request, &service, f); });
        CHECK_OK(create);
        QCOMPARE(head(service, id), request.startCommit);
        auto branches = awaitBackup<BackupResult<BranchSnapshot>>([&](auto f) { service.branches(id, &service, f); });
        CHECK_OK(branches.result); QCOMPARE(branches.value.branches.size(), 2);
        auto history = GitRepository(service.repoPath(id)).branchHistory({{}, 0, 1, true});
        CHECK_OK(history.result); QCOMPARE(history.value.revisions.size(), 1);
        QCOMPARE(history.value.revisions.first().refs.size(), 2);
        request.context = service.branchContext(id); request.ref = "refs/heads/试验方案";
        request.expectedHead = request.startCommit; request.name = "改名方案";
        CHECK_OK(awaitBackup<OperationResult>([&](auto f) { service.renameBranch(request, &service, f); }));
        QVERIFY(!service.rebuild(id).success);
        const auto before = readFile(source);
        git(service.repoPath(id), {"checkout", "改名方案"});
        QVERIFY(!service.backup(id).success);
        QCOMPARE(service.syncState(id), BackupSyncState::NeedsAttention);
        QCOMPARE(readFile(source), before);
        QVERIFY(!service.recheck(id).success);
    }
    void branchComparisonAndNameValidation()
    {
        TestDirectory dir; TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/file.txt";
        writeFile(source, "before\n"); CHECK_OK(service.addLocal(source));
        const auto id = service.idForSource(source), before = head(service, id);
        writeFile(source, "after\n"); CHECK_OK(service.backup(id));
        const auto gitRepo = GitRepository(service.repoPath(id));
        auto diff = gitRepo.diffBetween(before, head(service, id));
        CHECK_OK(diff.result); QCOMPARE(diff.value.files.size(), 1);
        QVERIFY(gitRepo.validateBranchName("中文/方案").success);
        for (const auto &name : {"bad name", "--help", "a..b", "HEAD", "a.lock"})
            QVERIFY(!gitRepo.validateBranchName(name).success);
        const auto history = gitRepo.branchHistory({{head(service, id)}, 0, 1});
        CHECK_OK(history.result); QVERIFY(history.value.hasMore);
        QCOMPARE(history.value.revisions.first().parents, QStringList{before});
    }
    void branchSwitchSavesWorkAndKeepsIgnoredFiles()
    {
        TestDirectory dir; TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/work";
        writeFile(source + "/.gitignore", "cache/\n");
        writeFile(source + "/document.txt", "original\n");
        writeFile(source + "/cache/local.txt", "first cache\n");
        CHECK_OK(service.addLocal(source)); const auto id = service.idForSource(source);
        const auto original = head(service, id);
        CHECK_OK(service.createBranch(id, "试验"));
        writeFile(source + "/document.txt", "unsaved main\n");
        writeFile(source + "/cache/local.txt", "shared cache\n");
        writeFile(source + "/.git/config", "source metadata");
        const auto request = service.prepareBranchSwitch(id, "refs/heads/试验");
        CHECK_OK(request.result); QVERIFY(request.value.savesChanges);
        QCOMPARE(request.value.savedChanges.size(), 1);
        QCOMPARE(request.value.savedChanges.first().path, QString("work/document.txt"));
        QCOMPARE(head(service, id), original);
        CHECK_OK(service.switchBranch(request.value));
        QCOMPARE(readFile(source + "/document.txt"), QByteArray("original\n"));
        QCOMPARE(readFile(source + "/cache/local.txt"), QByteArray("shared cache\n"));
        QCOMPARE(readFile(source + "/.git/config"), QByteArray("source metadata"));
        QCOMPARE(service.branchContext(id).ref, QString("refs/heads/试验"));
        const auto savedMain = git(service.repoPath(id), {"show", "main:work/document.txt"});
        QCOMPARE(savedMain.bytes, QByteArray("unsaved main\n"));
        const auto topicHead = head(service, id);
        CHECK_OK(service.switchBranch(id, "refs/heads/main"));
        QCOMPARE(readFile(source + "/document.txt"), QByteArray("unsaved main\n"));
        QCOMPARE(git(service.repoPath(id), {"rev-parse", "试验"}).output.trimmed(), topicHead);
        QVERIFY(!service.switchBranch(request.value).success);
    }
    void branchSwitchRejectsStaleConfirmationAndIgnoredCollision()
    {
        TestDirectory dir; TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/work";
        writeFile(source + "/file.txt", "one"); writeFile(source + "/local.txt", "tracked");
        CHECK_OK(service.addLocal(source)); const auto id = service.idForSource(source);
        CHECK_OK(service.createBranch(id, "old"));
        QVERIFY(QFile::remove(source + "/local.txt")); CHECK_OK(service.backup(id));
        writeFile(source + "/.gitignore", "local.txt\n"); CHECK_OK(service.backup(id));
        writeFile(source + "/local.txt", "private");
        const auto current = head(service, id);
        QVERIFY(!service.prepareBranchSwitch(id, "refs/heads/old").result.success);
        QCOMPARE(readFile(source + "/local.txt"), QByteArray("private"));
        QCOMPARE(head(service, id), current);
        QVERIFY(QFile::remove(source + "/local.txt"));
        const auto prepared = service.prepareBranchSwitch(id, "refs/heads/old"); CHECK_OK(prepared.result);
        writeFile(source + "/file.txt", "changed while confirming");
        QVERIFY(!service.switchBranch(prepared.value).success);
        QCOMPARE(service.branchContext(id).ref, QString("refs/heads/main"));
        QCOMPARE(readFile(source + "/file.txt"), QByteArray("changed while confirming"));
    }
    void branchMergeKeepsNativeParentsAndCanCancel()
    {
        TestDirectory dir; TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/document.txt";
        writeFile(source, "first\nbase\nlast\n"); CHECK_OK(service.addLocal(source));
        const auto id = service.idForSource(source);
        CHECK_OK(service.createBranch(id, "topic")); CHECK_OK(service.switchBranch(id, "refs/heads/topic"));
        writeFile(source, "first\ntopic\nlast\n"); CHECK_OK(service.backup(id)); const auto topic = head(service, id);
        CHECK_OK(service.switchBranch(id, "refs/heads/main"));
        writeFile(source, "first\nmain\nlast\n"); CHECK_OK(service.backup(id)); const auto main = head(service, id);
        auto request = service.branchRequest(id, "refs/heads/topic");
        auto session = awaitBackup<BackupResult<SyncResolutionSession>>([&](auto f) { service.prepareBranchMerge(request, &service, f); });
        CHECK_OK(session.result); QCOMPARE(session.value.mergeRef, QString("refs/heads/topic"));
        QCOMPARE(head(service, id), main); QVERIFY(session.value.remaining() > 0);
        CHECK_OK(awaitBackup<OperationResult>([&](auto f) { service.cancelResolution(id, session.value.id, &service, f); }));
        QCOMPARE(readFile(source), QByteArray("first\nmain\nlast\n"));
        session = awaitBackup<BackupResult<SyncResolutionSession>>([&](auto f) { service.prepareBranchMerge(request, &service, f); });
        CHECK_OK(session.result);
        auto chosen = service.chooseSyncResolution(session.value, session.value.files[0].path, 0, ConflictChoice::Remote);
        CHECK_OK(chosen.result);
        auto ready = service.prepareSyncApply(chosen.value); CHECK_OK(ready.result); CHECK_OK(service.applySync(ready.value));
        QCOMPARE(readFile(source), QByteArray("first\ntopic\nlast\n"));
        QCOMPARE(git(service.repoPath(id), {"show", "-s", "--format=%P", "HEAD"}).output.trimmed(), main + ' ' + topic);
        QCOMPARE(git(service.repoPath(id), {"rev-parse", "topic"}).output.trimmed(), topic);
        CHECK_OK(service.deleteBranch(id, "refs/heads/topic"));
    }
    void branchRemoteLifecycleAndUpstream()
    {
        RemoteFixture f;
        CHECK_OK(f.service->createBranch(f.id, "试验")); CHECK_OK(f.service->switchBranch(f.id, "refs/heads/试验"));
        writeFile(f.source, "remote topic\n"); CHECK_OK(f.service->backup(f.id));
        CHECK_OK(f.service->synchronize(f.id, true));
        auto info = GitRepository(f.service->repoPath(f.id)).branches(); CHECK_OK(info.result);
        bool tracked = false;
        for (const auto &b : info.value) if (b.name == "试验") tracked = b.upstream == "refs/remotes/origin/试验";
        QVERIFY(tracked);
        CHECK_OK(f.service->renameBranch(f.id, "refs/heads/试验", "新名称"));
        QCOMPARE(GitRepository(f.service->repoPath(f.id)).targetRef().value, QString("refs/heads/试验"));
        CHECK_OK(awaitBackup<OperationResult>([&](auto done) { f.service->fetchBranches(f.service->branchContext(f.id), f.service.get(), done); }));
        const auto r = f.service->branchRequest(f.id, "refs/remotes/origin/试验");
        CHECK_OK(awaitBackup<OperationResult>([&](auto done) { f.service->deleteRemoteBranch(r, f.service.get(), done); }));
        QVERIFY(GitRepository(f.service->repoPath(f.id)).resolve("refs/heads/新名称").result.success);
        QVERIFY(!runGit(f.remote, {"rev-parse", "--verify", "refs/heads/试验"}).success());
    }
    void branchFastForwardResumeAndNativeDelete()
    {
        TestDirectory dir; const auto paths = pathsIn(dir);
        auto service = std::make_unique<TestBackupService>(paths);
        const auto source = dir.path() + "/file.txt";
        writeFile(source, "base"); CHECK_OK(service->addLocal(source)); const auto id = service->idForSource(source);
        CHECK_OK(service->createBranch(id, "topic")); CHECK_OK(service->switchBranch(id, "refs/heads/topic"));
        writeFile(source, "topic"); CHECK_OK(service->backup(id)); const auto tip = head(*service, id);
        CHECK_OK(service->switchBranch(id, "refs/heads/main"));
        QVERIFY(!runGit(service->repoPath(id), {"branch", "-d", "topic"}).success());
        QVERIFY(!service->deleteBranch(id, "refs/heads/topic").success);
        const auto request = service->branchRequest(id, "refs/heads/topic");
        auto session = awaitBackup<BackupResult<SyncResolutionSession>>([&](auto f) { service->prepareBranchMerge(request, service.get(), f); });
        CHECK_OK(session.result); QCOMPARE(session.value.remaining(), 0);
        const auto sessionId = session.value.id;
        service.reset(); service = std::make_unique<TestBackupService>(paths);
        CHECK_OK(service->reload());
        session = service->syncResolution(id); CHECK_OK(session.result); QCOMPARE(session.value.id, sessionId);
        auto ready = service->prepareSyncApply(session.value); CHECK_OK(ready.result); CHECK_OK(service->applySync(ready.value));
        QCOMPARE(head(*service, id), tip); QCOMPARE(readFile(source), QByteArray("topic"));
        const auto again = service->branchRequest(id, "refs/heads/topic");
        session = awaitBackup<BackupResult<SyncResolutionSession>>([&](auto f) { service->prepareBranchMerge(again, service.get(), f); });
        CHECK_OK(session.result); QVERIFY(session.value.id.isEmpty());
        QCOMPARE(head(*service, id), tip);
        CHECK_OK(service->deleteBranch(id, "refs/heads/topic"));
        QVERIFY(!service->deleteBranch(id, "refs/heads/main", true).success);
    }
    void branchRenameMergeMatchesNativeGit_data()
    {
        QTest::addColumn<bool>("conflict");
        QTest::newRow("automatic-rename") << false;
        QTest::newRow("rename-conflict") << true;
    }
    void branchRenameMergeMatchesNativeGit()
    {
        QFETCH(bool, conflict);
        TestDirectory dir; TestBackupService service(pathsIn(dir)); const auto source = dir.path() + "/work";
        writeFile(source + "/old.txt", "first\nbase\nlast\n"); CHECK_OK(service.addLocal(source)); const auto id = service.idForSource(source);
        CHECK_OK(service.createBranch(id, "topic")); CHECK_OK(service.switchBranch(id, "refs/heads/topic"));
        QVERIFY(QFile::rename(source + "/old.txt", source + "/new.txt"));
        if (conflict) writeFile(source + "/new.txt", "first\ntopic\nlast\n");
        CHECK_OK(service.backup(id)); const auto topic = head(service, id);
        CHECK_OK(service.switchBranch(id, "refs/heads/main"));
        writeFile(source + "/old.txt", "first\nmain\nlast\n"); CHECK_OK(service.backup(id)); const auto main = head(service, id);
        writeFile(service.repoPath(id) + "/.git/info/exclude", "work/private.txt\n");
        writeFile(source + "/private.txt", "keep local only");
        const auto native = dir.path() + "/native";
        git({}, {"clone", "--no-hardlinks", service.repoPath(id), native});
        const auto merged = runGit(native, {"merge", "--no-edit", "origin/topic"});
        if (conflict)
        {
            QVERIFY(!merged.success());
            git(native, {"checkout", "--theirs", "--", "work/new.txt"});
            git(native, {"add", "--all"}); git(native, {"commit", "-m", "resolved"});
        }
        else CHECK_OK(GitRepository::outcome(merged));
        const auto request = service.branchRequest(id, "refs/heads/topic");
        auto session = awaitBackup<BackupResult<SyncResolutionSession>>([&](auto f) { service.prepareBranchMerge(request, &service, f); });
        CHECK_OK(session.result);
        while (session.value.remaining())
        {
            bool chosen = false;
            for (const auto &file : session.value.files)
            {
                for (int i = 0; i < file.hunks.size(); ++i) if (file.hunks[i].choice == ConflictChoice::Unresolved)
                { session = service.chooseSyncResolution(session.value, file.path, i, ConflictChoice::Remote); CHECK_OK(session.result); chosen = true; break; }
                if (chosen) break;
            }
        }
        const auto ready = service.prepareSyncApply(session.value); CHECK_OK(ready.result); CHECK_OK(service.applySync(ready.value));
        QCOMPARE(git(service.repoPath(id), {"rev-parse", "HEAD^{tree}"}).output, git(native, {"rev-parse", "HEAD^{tree}"}).output);
        QCOMPARE(git(service.repoPath(id), {"show", "-s", "--format=%P", "HEAD"}).output.trimmed(), main + ' ' + topic);
        QVERIFY(!QFileInfo::exists(source + "/old.txt"));
        QCOMPARE(readFile(source + "/private.txt"), QByteArray("keep local only"));
        QVERIFY(!runGit(service.repoPath(id), {"cat-file", "-e", "HEAD:work/private.txt"}).success());
        QCOMPARE(readFile(source + "/new.txt"), conflict ? QByteArray("first\ntopic\nlast\n") : QByteArray("first\nmain\nlast\n"));
    }
    void branchRemoteTrackingAndPushOnlyRebuildGuard()
    {
        RemoteFixture f;
        git(f.writer, {"checkout", "-b", "远程方案"}); git(f.writer, {"push", "-u", "origin", "远程方案"});
        const auto tip = f.commitRemote("cloud topic");
        CHECK_OK(awaitBackup<OperationResult>([&](auto done) { f.service->fetchBranches(f.service->branchContext(f.id), f.service.get(), done); }));
        auto request = f.service->branchRequest(f.id, "refs/remotes/origin/远程方案");
        request.name = "本地方案"; request.startCommit = request.expectedHead; request.upstream = request.ref;
        CHECK_OK(awaitBackup<OperationResult>([&](auto done) { f.service->createBranch(request, f.service.get(), done); }));
        QCOMPARE(readFile(f.source), QByteArray("initial\n"));
        CHECK_OK(f.service->switchBranch(f.id, "refs/heads/本地方案"));
        QCOMPARE(head(*f.service, f.id), tip); QCOMPARE(readFile(f.source), QByteArray("cloud topic"));
        QCOMPARE(GitRepository(f.service->repoPath(f.id)).targetRef().value, QString("refs/heads/远程方案"));
        writeFile(f.source, "local fork"); CHECK_OK(f.service->backup(f.id));
        const auto newer = f.commitRemote("remote fork");
        request = f.service->branchRequest(f.id, "refs/heads/本地方案");
        QVERIFY(!awaitBackup<OperationResult>([&](auto done) { f.service->uploadBranch(request, f.service.get(), done); }).success);
        QCOMPARE(git(f.remote, {"rev-parse", "远程方案"}).output.trimmed(), newer);

        RemoteFixture rebuild;
        const auto pushOnly = rebuild.dir.path() + "/push-only.git";
        git({}, {"clone", "--bare", rebuild.remote, pushOnly});
        git(pushOnly, {"branch", "extra", "main"});
        git(rebuild.service->repoPath(rebuild.id), {"remote", "set-url", "--add", "--push", "origin", pushOnly});
        const auto before = head(*rebuild.service, rebuild.id);
        QVERIFY(!rebuild.service->rebuild(rebuild.id).success);
        QCOMPARE(head(*rebuild.service, rebuild.id), before);
        QVERIFY(runGit(pushOnly, {"rev-parse", "--verify", "extra"}).success());
    }
    void branchDeletionRejectsConcurrentCommit()
    {
        TestDirectory dir; bool race = false; QString repo, replacement;
        BackupDependencies deps;
        deps.git = [&](const QString &path, const QStringList &args, const GitOptions &options) {
            if (race && path == repo && args.contains("update-ref") && args.contains("-d"))
            { race = false; git(repo, {"update-ref", "refs/heads/topic", replacement}); }
            return runGit(path, args, options);
        };
        TestBackupService service(pathsIn(dir), nullptr, nullptr, deps);
        const auto source = dir.path() + "/file.txt";
        writeFile(source, "base"); CHECK_OK(service.addLocal(source)); const auto id = service.idForSource(source); repo = service.repoPath(id);
        CHECK_OK(service.createBranch(id, "topic")); writeFile(source, "new"); CHECK_OK(service.backup(id)); replacement = head(service, id);
        race = true; QVERIFY(!service.deleteBranch(id, "refs/heads/topic", true).success);
        QCOMPARE(git(repo, {"rev-parse", "topic"}).output.trimmed(), replacement);
        CHECK_OK(service.deleteBranch(id, "refs/heads/topic", true));
    }
    void branchSwitchInjectedFailures_data()
    {
        QTest::addColumn<QString>("fault");
        for (const auto *fault : {"before-marker", "install", "final-save", "external-source", "external-branch"}) QTest::newRow(fault) << QString::fromLatin1(fault);
    }
    void branchSwitchInjectedFailures()
    {
        QFETCH(QString, fault);
        TestDirectory dir; const auto source = dir.path() + "/document.txt";
        QString repo; bool active = false, switched = false, installFailed = false;
        auto files = std::make_shared<FaultFiles>(); BackupDependencies deps; deps.files = files;
        deps.allowSave = [&](const BackupRecord &r) {
            return !(active && fault == "before-marker" && r.operation == "switch-branch") &&
                   !(active && fault == "final-save" && r.operation.isEmpty() && r.branchRef == "refs/heads/topic");
        };
        files->failRename = [&](const QString &, const QString &to) {
            if (active && switched && fault == "install" && to == source && !installFailed) { installFailed = true; return true; }
            return false;
        };
        deps.git = [&](const QString &path, const QStringList &args, const GitOptions &options) {
            const auto result = runGit(path, args, options);
            if (active && path == repo && args.contains("switch") && args.last() == "topic" && result.success())
            {
                switched = true;
                if (fault == "external-source") writeFile(source, "concurrent edit");
                if (fault == "external-branch") git(repo, {"checkout", "main"});
            }
            return result;
        };
        TestBackupService service(pathsIn(dir), nullptr, nullptr, deps);
        writeFile(source, "base"); CHECK_OK(service.addLocal(source)); const auto id = service.idForSource(source); repo = service.repoPath(id);
        CHECK_OK(service.createBranch(id, "topic")); writeFile(source, "main version"); CHECK_OK(service.backup(id));
        const auto saved = head(service, id); const auto request = service.prepareBranchSwitch(id, "refs/heads/topic"); CHECK_OK(request.result);
        active = true; QVERIFY(!service.switchBranch(request.value).success); active = false;
        if (fault == "external-source") QCOMPARE(readFile(source), QByteArray("concurrent edit"));
        else if (fault == "final-save") QCOMPARE(readFile(source), QByteArray("base"));
        else QCOMPARE(readFile(source), QByteArray("main version"));
        if (fault == "before-marker" || fault == "install")
        { QCOMPARE(head(service, id), saved); QCOMPARE(service.branchContext(id).ref, QString("refs/heads/main")); }
        else
        { QCOMPARE(service.syncState(id), BackupSyncState::NeedsAttention); QVERIFY(!record(service, id).recoveryPaths.isEmpty()); }
        QCOMPARE(git(repo, {"show", "main:document.txt"}).bytes, QByteArray("main version"));
    }
    void branchPublishingSelectedRefAndRemoteLease()
    {
        RemoteFixture f;
        CHECK_OK(f.service->createBranch(f.id, "topic"));
        auto request = f.service->branchRequest(f.id, "refs/heads/topic");
        CHECK_OK(awaitBackup<OperationResult>([&](auto done) { f.service->uploadBranch(request, f.service.get(), done); }));
        QCOMPARE(f.service->branchContext(f.id).ref, QString("refs/heads/main"));
        QCOMPARE(git(f.remote, {"rev-parse", "topic"}).output.trimmed(), request.expectedHead);
        CHECK_OK(f.service->renameBranch(f.id, "refs/heads/topic", "new-name"));
        request = f.service->branchRequest(f.id, "refs/heads/new-name"); request.name = "new-name";
        CHECK_OK(awaitBackup<OperationResult>([&](auto done) { f.service->uploadBranch(request, f.service.get(), done); }));
        QVERIFY(runGit(f.remote, {"rev-parse", "--verify", "topic"}).success());
        CHECK_OK(awaitBackup<OperationResult>([&](auto done) { f.service->fetchBranches(f.service->branchContext(f.id), f.service.get(), done); }));
        auto removed = f.service->branchRequest(f.id, "refs/remotes/origin/topic");
        git(f.writer, {"fetch"}); git(f.writer, {"checkout", "-b", "topic", "origin/topic"});
        f.commitRemote("concurrent remote");
        QVERIFY(!awaitBackup<OperationResult>([&](auto done) { f.service->deleteRemoteBranch(removed, f.service.get(), done); }).success);
        QVERIFY(runGit(f.remote, {"rev-parse", "--verify", "topic"}).success());
        auto linked = f.service->branchRequest(f.id, "refs/heads/new-name");
        const auto otherRemote = f.dir.path() + "/other.git";
        git({}, {"init", "--bare", otherRemote}); git(f.service->repoPath(f.id), {"remote", "set-url", "origin", otherRemote});
        QVERIFY(!awaitBackup<OperationResult>([&](auto done) { f.service->uploadBranch(linked, f.service.get(), done); }).success);
    }
    void isolatedResolutionPreservesBytesAndChoices_data()
    {
        QTest::addColumn<QByteArray>("base");
        QTest::addColumn<QByteArray>("local");
        QTest::addColumn<QByteArray>("remote");
        QTest::addColumn<bool>("whole");
        QTest::newRow("text") << QByteArray("first\nbase\nlast\n") << QByteArray("first\nlocal\nlast\n") << QByteArray("first\ncloud\nlast\n") << false;
        QTest::newRow("no-final-newline") << QByteArray("base") << QByteArray("local") << QByteArray("cloud") << false;
        QTest::newRow("crlf-bom") << QByteArray("\xef\xbb\xbfheader\r\nbase\r\n") << QByteArray("\xef\xbb\xbfheader\r\nlocal\r\n") << QByteArray("\xef\xbb\xbfheader\r\ncloud\r\n") << false;
        QTest::newRow("literal-marker") << QByteArray("<<<<<<< HEAD\nbase\n") << QByteArray("<<<<<<< HEAD\nlocal\n") << QByteArray("<<<<<<< HEAD\ncloud\n") << false;
        QTest::newRow("invalid-utf8") << QByteArray("base") << QByteArray("local\xff", 6) << QByteArray("cloud") << true;
        QTest::newRow("incomplete-utf8") << QByteArray("base") << QByteArray("local\xe4", 6) << QByteArray("cloud") << true;
        QTest::newRow("large-file") << QByteArray("base") << QByteArray(2 * 1024 * 1024 + 1, 'a') << QByteArray("cloud") << true;
        QTest::newRow("many-lines") << QByteArray("base") << QByteArray(20001, '\n') << QByteArray("cloud") << true;
        QTest::newRow("many-lines-without-final-newline") << QByteArray("base") << (QByteArray(20000, '\n') + 'a') << QByteArray("cloud") << true;
        QTest::newRow("binary") << QByteArray("base\0x", 6) << QByteArray("local\0x", 7) << QByteArray("cloud\0x", 7) << true;
    }
    void isolatedResolutionPreservesBytesAndChoices()
    {
        QFETCH(QByteArray, base); QFETCH(QByteArray, local); QFETCH(QByteArray, remote); QFETCH(bool, whole);
        RemoteFixture f;
        writeFile(f.source, base);
        CHECK_OK(f.service->backup(f.id));
        CHECK_OK(f.service->synchronize(f.id, true));
        git(f.writer, {"pull", "--ff-only"});
        const auto before = head(*f.service, f.id);
        const auto cloud = f.commitRemote(remote);
        writeFile(f.source, local);
        const auto root = f.dir.path() + '/' + QUuid::createUuid().toString(QUuid::WithoutBraces);
        BackupMerge merge(root, {});
        CHECK_OK(merge.create(record(*f.service, f.id), f.service->repoPath(f.id)));
        QCOMPARE(head(*f.service, f.id), before);
        QCOMPARE(readFile(f.source), local);
        QCOMPARE(merge.session().total(), 1);
        QCOMPARE(merge.session().files[0].wholeFile, whole);
        QVERIFY(!merge.prepare().result.success);
        CHECK_OK(merge.choose(1, "source.txt", 0, ConflictChoice::Local));
        BackupMerge resumed(root, {});
        CHECK_OK(resumed.load());
        QCOMPARE(resumed.session().remaining(), 0);
        const auto prepared = resumed.prepare(); CHECK_OK(prepared.result);
        CHECK_OK(resumed.exportSource(prepared.value, f.dir.path() + "/chosen-local"));
        QCOMPARE(readFile(f.dir.path() + "/chosen-local"), local);
        CHECK_OK(resumed.choose(2, "source.txt", 0, ConflictChoice::Remote));
        QVERIFY(!resumed.validRequest(prepared.value));
        const auto other = resumed.prepare(); CHECK_OK(other.result);
        CHECK_OK(resumed.exportSource(other.value, f.dir.path() + "/chosen-remote"));
        QCOMPARE(readFile(f.dir.path() + "/chosen-remote"), remote);
        QVERIFY(git(resumed.repositoryPath(), {"merge-base", "--is-ancestor", cloud, other.value.commit}).success());
        QCOMPARE(head(*f.service, f.id), before);
        QCOMPARE(readFile(f.source), local);
    }
    void isolatedResolutionCombinesSeparateChanges()
    {
        RemoteFixture f;
        writeFile(f.source, "a\nb\nc\nd\ne\nf\ng\n");
        CHECK_OK(f.service->backup(f.id)); CHECK_OK(f.service->synchronize(f.id, true));
        git(f.writer, {"pull", "--ff-only"});
        f.commitRemote("a\nb\nc\nd\ne\nf\ncloud\n");
        writeFile(f.source, "local\nb\nc\nd\ne\nf\ng\n");
        BackupMerge merge(f.dir.path() + '/' + QUuid::createUuid().toString(QUuid::WithoutBraces), {});
        CHECK_OK(merge.create(record(*f.service, f.id), f.service->repoPath(f.id)));
        QCOMPARE(merge.session().total(), 0);
        const auto result = merge.prepare(); CHECK_OK(result.result);
        CHECK_OK(merge.exportSource(result.value, f.dir.path() + "/result"));
        QCOMPARE(readFile(f.dir.path() + "/result"), QByteArray("local\nb\nc\nd\ne\nf\ncloud\n"));
    }
    void resolutionRejectsDamagedProgress()
    {
        RemoteFixture f;
        f.commitRemote("cloud\n"); writeFile(f.source, "local\n");
        const auto session = f.service->prepareSyncResolution(f.id); CHECK_OK(session.result);
        const auto root = f.paths.backupRoot + "/items/" + f.id + "/resolutions/" + session.value.id;
        const auto path = root + "/session.json";
        const auto original = readFile(path);
        const auto baseline = head(*f.service, f.id);
        const auto mutate = [&](const std::function<void(QJsonObject &)> &change)
        {
            auto data = QJsonDocument::fromJson(original).object();
            change(data); writeFile(path, QJsonDocument(data).toJson());
            return f.service->syncResolution(f.id).result;
        };
        QVERIFY(!mutate([](QJsonObject &data) { data.remove("automatic"); }).success);
        QVERIFY(!mutate([](QJsonObject &data) { data["files"] = QJsonObject{}; }).success);
        for (const auto &fault : {QString("empty-hunks"), QString("short-common"), QString("bad-bytes"), QString("duplicate-file")})
        {
            const auto result = mutate([&](QJsonObject &data)
            {
                auto files = data["files"].toArray(); auto file = files[0].toObject();
                if (fault == "empty-hunks") file["hunks"] = QJsonArray{};
                if (fault == "short-common") file["common"] = QJsonArray{};
                if (fault == "bad-bytes")
                {
                    auto hunks = file["hunks"].toArray(); auto hunk = hunks[0].toObject();
                    hunk["local"] = "%%%"; hunks[0] = hunk; file["hunks"] = hunks;
                }
                files[0] = file;
                if (fault == "duplicate-file") files.append(file);
                data["files"] = files;
            });
            QVERIFY2(!result.success, qPrintable(fault));
        }
        writeFile(path, original);
        CHECK_OK(f.service->syncResolution(f.id).result);
        QCOMPARE(head(*f.service, f.id), baseline);
        QCOMPARE(readFile(f.source), QByteArray("local\n"));
    }
    void resolutionLargeFilesAvoidTextReads()
    {
        RemoteFixture f;
        f.commitRemote(QByteArray(2 * 1024 * 1024 + 1, 'r'));
        writeFile(f.source, QByteArray(2 * 1024 * 1024 + 1, 'l'));
        int blobReads = 0;
        BackupDependencies deps;
        deps.git = [&](const QString &path, const QStringList &args, const GitOptions &options)
        {
            if (args.contains("cat-file") && args.contains("blob")) ++blobReads;
            return runGit(path, args, options);
        };
        BackupMerge merge(f.dir.path() + '/' + QUuid::createUuid().toString(QUuid::WithoutBraces), deps);
        CHECK_OK(merge.create(record(*f.service, f.id), f.service->repoPath(f.id)));
        QCOMPARE(merge.session().total(), 1);
        QVERIFY(merge.session().files[0].wholeFile);
        QCOMPARE(blobReads, 0);
    }
    void resolutionSurvivesRestartAndRejectsStaleInputs()
    {
        RemoteFixture f;
        const auto before = head(*f.service, f.id);
        const auto cloud = f.commitRemote("cloud\n");
        writeFile(f.source, "current local\n");
        auto session = f.service->prepareSyncResolution(f.id); CHECK_OK(session.result);
        QCOMPARE(f.service->syncState(f.id), BackupSyncState::ResolutionPending);
        QCOMPARE(head(*f.service, f.id), before);
        QVERIFY(!f.service->backup(f.id).success);
        QVERIFY(!f.service->synchronize(f.id, true).success);
        auto choice = f.service->chooseSyncResolution(session.value, "source.txt", 0, ConflictChoice::Remote); CHECK_OK(choice.result);
        f.service.reset(); f.service = std::make_unique<TestBackupService>(f.paths);
        session = f.service->syncResolution(f.id); CHECK_OK(session.result);
        QCOMPARE(session.value.remaining(), 0); QVERIFY(!session.value.stale);
        auto prepared = f.service->prepareSyncApply(session.value); CHECK_OK(prepared.result);
        writeFile(f.source, "edited after preview\n");
        QVERIFY(!f.service->applySync(prepared.value).success);
        QCOMPARE(head(*f.service, f.id), before);
        session = f.service->syncResolution(f.id); CHECK_OK(session.result); QVERIFY(session.value.stale);
        session = f.service->prepareSyncResolution(f.id, true); CHECK_OK(session.result); QCOMPARE(session.value.remaining(), 1);
        choice = f.service->chooseSyncResolution(session.value, "source.txt", 0, ConflictChoice::Local); CHECK_OK(choice.result);
        prepared = f.service->prepareSyncApply(choice.value); CHECK_OK(prepared.result);
        CHECK_OK(f.service->applySync(prepared.value));
        QCOMPARE(readFile(f.source), QByteArray("edited after preview\n"));
        QCOMPARE(f.service->syncState(f.id), BackupSyncState::Tracking);
        QCOMPARE(head(*f.service, f.id), prepared.value.commit);
        QVERIFY(git(f.service->repoPath(f.id), {"merge-base", "--is-ancestor", cloud, "HEAD"}).success());
        QVERIFY(!f.service->applySync(prepared.value).success);
        const auto resolved = head(*f.service, f.id);
        BackupMonitor monitor(f.service.get()); monitor.start(); monitor.scanNow(); settle(monitor, *f.service);
        QCOMPARE(head(*f.service, f.id), resolved);
    }
    void resolutionFastForwardAndLegacyPending()
    {
        RemoteFixture f;
        const auto cloud = f.commitRemote("cloud\n");
        const auto session = f.service->prepareSyncResolution(f.id); CHECK_OK(session.result); QCOMPARE(session.value.total(), 0);
        const auto prepared = f.service->prepareSyncApply(session.value); CHECK_OK(prepared.result); QCOMPARE(prepared.value.commit, cloud);
        CHECK_OK(f.service->applySync(prepared.value)); QCOMPARE(readFile(f.source), QByteArray("cloud\n"));
        const auto none = f.service->prepareSyncResolution(f.id); CHECK_OK(none.result); QVERIFY(none.value.id.isEmpty());
        f.commitRemote("second\n"); CHECK_OK(f.service->synchronize(f.id, false));
        QCOMPARE(f.service->syncState(f.id), BackupSyncState::RemotePending);
        writeFile(f.source, "local after old pull\n");
        auto legacy = f.service->syncResolution(f.id); CHECK_OK(legacy.result);
        legacy = f.service->chooseSyncResolution(legacy.value, "source.txt", 0, ConflictChoice::Remote); CHECK_OK(legacy.result);
        const auto ready = f.service->prepareSyncApply(legacy.value); CHECK_OK(ready.result);
        CHECK_OK(f.service->applySync(ready.value)); QCOMPARE(readFile(f.source), QByteArray("second\n"));
    }
    void resolutionDurablePauseAndApplyFailures()
    {
        bool rejectPrepare = true, rejectFinal = false;
        BackupDependencies deps;
        deps.allowSave = [&](const BackupRecord &r) { return !(rejectPrepare && r.operation == "prepare-resolution") && !(rejectFinal && r.state == BackupSyncState::Tracking); };
        RemoteFixture f(deps);
        const auto previous = head(*f.service, f.id);
        f.commitRemote("cloud\n");
        QVERIFY(!f.service->prepareSyncResolution(f.id).result.success);
        QCOMPARE(head(*f.service, f.id), previous); QCOMPARE(readFile(f.source), QByteArray("initial\n"));
        rejectPrepare = false;
        const auto session = f.service->prepareSyncResolution(f.id); CHECK_OK(session.result);
        const auto prepared = f.service->prepareSyncApply(session.value); CHECK_OK(prepared.result);
        rejectFinal = true;
        QVERIFY(!f.service->applySync(prepared.value).success);
        QCOMPARE(f.service->syncState(f.id), BackupSyncState::NeedsAttention);
        QCOMPARE(readFile(f.source), QByteArray("cloud\n"));
        QVERIFY(!record(*f.service, f.id).recoveryPaths.isEmpty());
        QVERIFY(!f.service->backup(f.id).success);
    }
    void resolutionMultipleHunksAndDirectoryChoices()
    {
        TestDirectory dir; TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/project";
        writeFile(source + "/notes.txt", "one\nb\nc\nd\ne\nf\ntwo\n");
        writeFile(source + "/node", "old file");
        writeFile(source + "/.gitignore", "ignored.dat\n");
        writeFile(source + "/ignored.dat", "local ignored");
        writeFile(source + "/keep.dat", "kept");
        CHECK_OK(service.addLocal(source)); const auto id = service.idForSource(source);
        const auto remote = dir.path() + "/remote.git", writer = dir.path() + "/writer";
        git({}, {"init", "--bare", "--initial-branch=main", remote}); CHECK_OK(service.setRemote(id, remote)); CHECK_OK(service.synchronize(id, true));
        git({}, {"clone", remote, writer});
        writeFile(writer + "/project/notes.txt", "cloud one\nb\nc\nd\ne\nf\ncloud two\n");
        git(writer, {"rm", "project/node"}); writeFile(writer + "/project/node/child.txt", "cloud directory");
        writeFile(writer + "/project/ignored.dat", "cloud ignored collision");
        git(writer, {"add", "-f", "project/ignored.dat"}); git(writer, {"add", "--all"}); git(writer, {"commit", "-m", "cloud"}); git(writer, {"push"});
        writeFile(source + "/notes.txt", "local one\nb\nc\nd\ne\nf\nlocal two\n");
        writeFile(source + "/node", "local file");
        auto session = service.prepareSyncResolution(id); CHECK_OK(session.result);
        QCOMPARE(session.value.total(), 4);
        const auto files = session.value.files;
        for (const auto &file : files)
            for (int i = 0; i < file.hunks.size(); ++i)
            {
                const auto choice = file.path.endsWith("notes.txt") && i == 1 ? ConflictChoice::Remote : ConflictChoice::Local;
                session = service.chooseSyncResolution(session.value, file.path, i, choice); CHECK_OK(session.result);
            }
        const auto prepared = service.prepareSyncApply(session.value); CHECK_OK(prepared.result);
        CHECK_OK(service.applySync(prepared.value));
        QCOMPARE(readFile(source + "/notes.txt"), QByteArray("local one\nb\nc\nd\ne\nf\ncloud two\n"));
        QCOMPARE(readFile(source + "/node"), QByteArray("local file"));
        QCOMPARE(readFile(source + "/ignored.dat"), QByteArray("local ignored"));
        QCOMPARE(readFile(source + "/keep.dat"), QByteArray("kept"));
    }
    void resolutionPreservesModesAndDisablesUnionMerge()
    {
        RemoteFixture f;
        writeFile(f.writer + "/.gitattributes", "source.txt merge=union\n");
        writeFile(f.writer + "/source.txt", "cloud\n");
        git(f.writer, {"add", "--all"});
        git(f.writer, {"update-index", "--chmod=+x", "source.txt"});
        git(f.writer, {"commit", "-m", "executable cloud version"}); git(f.writer, {"push"});
        writeFile(f.source, "local\n");
        auto session = f.service->prepareSyncResolution(f.id); CHECK_OK(session.result);
        QCOMPARE(session.value.total(), 1); // union must never silently combine the two paragraphs.
        session = f.service->chooseSyncResolution(session.value, "source.txt", 0, ConflictChoice::Local); CHECK_OK(session.result);
        QCOMPARE(session.value.currentPath, QString("source.txt"));
        const auto prepared = f.service->prepareSyncApply(session.value); CHECK_OK(prepared.result);
        CHECK_OK(f.service->applySync(prepared.value));
        QCOMPARE(readFile(f.source), QByteArray("local\n"));
        QVERIFY(git(f.service->repoPath(f.id), {"ls-tree", "HEAD", "source.txt"}).output.startsWith("100755"));
#ifndef Q_OS_WIN
        QVERIFY(QFileInfo(f.source).permission(QFileDevice::ExeOwner));
#endif
    }
    void resolutionQueueAndPinnedSnapshot()
    {
        RemoteFixture f;
        const auto cloud = f.commitRemote("cloud snapshot\n");
        writeFile(f.source, "local\n");
        const auto other = f.dir.path() + "/other.txt";
        writeFile(other, "one\n"); CHECK_OK(f.service->addLocal(other));
        const auto otherId = f.service->idForSource(other);
        writeFile(other, "two\n");
        BackupResult<SyncResolutionSession> session;
        OperationResult queued, independent;
        f.service->BackupService::prepareSyncResolution(f.id, f.service.get(), [&](const auto &r) { session = r; });
        f.service->BackupService::backup(f.id, f.service.get(), [&](const auto &r) { queued = r; });
        f.service->BackupService::backup(otherId, f.service.get(), [&](const auto &r) { independent = r; });
        settle(*f.service);
        CHECK_OK(session.result); QVERIFY(!queued.success); CHECK_OK(independent);
        // User choices release the process lock and queue, and pin the fetched cloud snapshot.
        QLockFile lock(f.paths.backupRoot + "/.writer.lock"); QVERIFY(lock.tryLock()); lock.unlock();
        f.commitRemote("later cloud update\n");
        session = f.service->chooseSyncResolution(session.value, "source.txt", 0, ConflictChoice::Remote); CHECK_OK(session.result);
        const auto prepared = f.service->prepareSyncApply(session.value); CHECK_OK(prepared.result);
        CHECK_OK(f.service->applySync(prepared.value));
        QCOMPARE(readFile(f.source), QByteArray("cloud snapshot\n"));
        QVERIFY(git(f.service->repoPath(f.id), {"merge-base", "--is-ancestor", cloud, "HEAD"}).success());
        QCOMPARE(readFile(f.service->repoPath(otherId) + "/other.txt"), QByteArray("two\n"));
    }
    void resolutionRefusesExternalRepositoryAndConfigurationChanges()
    {
        RemoteFixture f;
        f.commitRemote("cloud\n"); writeFile(f.source, "local\n");
        auto session = f.service->prepareSyncResolution(f.id); CHECK_OK(session.result);
        session = f.service->chooseSyncResolution(session.value, "source.txt", 0, ConflictChoice::Remote); CHECK_OK(session.result);
        const auto prepared = f.service->prepareSyncApply(session.value); CHECK_OK(prepared.result);
        const auto live = f.service->repoPath(f.id), before = head(*f.service, f.id);
        git(live, {"config", "remote.origin.url", f.remote + ".changed"});
        auto resumed = f.service->syncResolution(f.id); CHECK_OK(resumed.result); QVERIFY(resumed.value.stale);
        QVERIFY(!f.service->applySync(prepared.value).success);
        git(live, {"config", "remote.origin.url", f.remote});
        writeFile(live + "/external.txt", "external staged content"); git(live, {"add", "external.txt"});
        QVERIFY(!f.service->applySync(prepared.value).success);
        QCOMPARE(readFile(live + "/external.txt"), QByteArray("external staged content"));
        QVERIFY(git(live, {"diff", "--cached", "--name-only"}).output.contains("external.txt"));
        git(live, {"commit", "-m", "external commit"});
        const auto external = head(*f.service, f.id); QVERIFY(external != before);
        QVERIFY(!f.service->prepareSyncResolution(f.id, true).result.success);
        QVERIFY(!f.service->applySync(prepared.value).success);
        QCOMPARE(head(*f.service, f.id), external); QCOMPARE(readFile(f.source), QByteArray("local\n"));
    }
    void resolutionInjectedFailures_data()
    {
        QTest::addColumn<QString>("fault");
        for (const auto *name : {"capture", "snapshot-commit", "result-commit", "before-apply-save", "live-merge", "source-install"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }
    void resolutionInjectedFailures()
    {
        QFETCH(QString, fault);
        auto files = std::make_shared<FaultFiles>();
        bool enabled = false;
        BackupDependencies deps; deps.files = files;
        deps.allowSave = [&](const BackupRecord &r) { return !(enabled && fault == "before-apply-save" && r.operation == "apply-resolution"); };
        deps.git = [&](const QString &path, const QStringList &args, const GitOptions &options)
        {
            if (enabled && ((fault == "snapshot-commit" && args.contains("commit") && path.contains("/resolutions/")) ||
                (fault == "result-commit" && args.contains("commit-tree")) ||
                (fault == "live-merge" && args.contains("merge") && !path.contains("/resolutions/")))) return failedGit();
            return runGit(path, args, options);
        };
        RemoteFixture f(deps);
        files->failCopy = [&](const QString &, const QString &) { return enabled && fault == "capture"; };
        files->failRename = [&](const QString &, const QString &to) { return enabled && fault == "source-install" && to == f.source; };
        f.commitRemote("cloud\n"); writeFile(f.source, "local\n");
        const auto before = head(*f.service, f.id);
        enabled = true;
        auto session = f.service->prepareSyncResolution(f.id);
        if (fault == "capture" || fault == "snapshot-commit")
        {
            QVERIFY(!session.result.success);
            QCOMPARE(f.service->syncState(f.id), BackupSyncState::Tracking);
        }
        else
        {
            CHECK_OK(session.result);
            session = f.service->chooseSyncResolution(session.value, "source.txt", 0, ConflictChoice::Remote); CHECK_OK(session.result);
            const auto prepared = f.service->prepareSyncApply(session.value);
            if (fault == "result-commit") QVERIFY(!prepared.result.success);
            else
            {
                CHECK_OK(prepared.result);
                QVERIFY(!f.service->applySync(prepared.value).success);
            }
            QCOMPARE(f.service->syncState(f.id), fault == "source-install" ? BackupSyncState::NeedsAttention : BackupSyncState::ResolutionPending);
        }
        if (fault != "source-install") QCOMPARE(head(*f.service, f.id), before);
        else QVERIFY(!record(*f.service, f.id).recoveryPaths.isEmpty());
        // A failed rollback intentionally retains the original in its recovery location.
        if (QFileInfo::exists(f.source)) QCOMPARE(readFile(f.source), QByteArray("local\n"));
        else
        {
            bool retained = false;
            for (const auto &path : record(*f.service, f.id).recoveryPaths)
                if (readFile(path + "/old") == QByteArray("local\n")) retained = true;
            QVERIFY(retained);
        }
    }
    void resolutionImportedScopeAndIgnoredDeletionPreview()
    {
        TestDirectory dir; TestBackupService service(pathsIn(dir));
        const auto original = dir.path() + "/original", source = dir.path() + "/local";
        QDir().mkpath(original); git(original, {"init"});
        writeFile(original + "/managed/keep.txt", "base\n");
        writeFile(original + "/outside.txt", "base\n");
        writeFile(original + "/managed/.gitignore", "node\nuntouched.bin\n");
        git(original, {"add", "--all"}); git(original, {"commit", "-m", "initial"});
        const auto imported = service.prepareImport(original); CHECK_OK(imported.result);
        CHECK_OK(service.finishImport(imported.value.sessionId, "managed", source));
        const auto id = service.idForSource(source), live = service.repoPath(id);
        writeFile(live + "/outside.txt", "local repository\n");
        git(live, {"add", "outside.txt"}); git(live, {"commit", "-m", "previous local repository change"});
        BackupCatalog catalog(service.paths()); CHECK_OK(catalog.load());
        auto baseline = *catalog.find(id); baseline.lastCommit = head(service, id); CHECK_OK(catalog.save(baseline)); CHECK_OK(service.reload());
        writeFile(source + "/node", "ignored file\n");
        writeFile(source + "/untouched.bin", "ignored retained\n");
        writeFile(source + "/.git/config", "protected source metadata");
        writeFile(original + "/outside.txt", "cloud repository\n");
        writeFile(original + "/managed/node/child.txt", "cloud directory\n");
        writeFile(original + "/managed/-' 空格[1]$.txt", "special path\n");
        git(original, {"add", "-f", "managed/node/child.txt"});
        git(original, {"add", "--all"}); git(original, {"commit", "-m", "cloud"});
        auto session = service.prepareSyncResolution(id); CHECK_OK(session.result);
        QCOMPARE(session.value.total(), 2);
        const auto files = session.value.files;
        bool outside = false;
        for (const auto &file : files)
        {
            if (file.path == "outside.txt") { QVERIFY(!file.managed); outside = true; }
            session = service.chooseSyncResolution(session.value, file.path, 0, ConflictChoice::Remote); CHECK_OK(session.result);
        }
        QVERIFY(outside);
        const auto ready = service.prepareSyncApply(session.value); CHECK_OK(ready.result);
        bool deletion = false, repoOnly = false;
        for (const auto &change : ready.value.changes)
        {
            if (change.path == "managed/node") { QCOMPARE(change.status, QString("D")); deletion = true; }
            if (change.path == "outside.txt") { QCOMPARE(change.summary, QString("仅影响备份仓库")); repoOnly = true; }
            QVERIFY(change.path != "managed/untouched.bin");
        }
        QVERIFY(deletion); QVERIFY(repoOnly);
        const auto root = service.paths().backupRoot + "/items/" + id + "/resolutions/" + session.value.id;
        BackupMerge preview(root, {}); CHECK_OK(preview.load());
        const auto ignored = preview.content("managed/untouched.bin", ConflictSide::Result); CHECK_OK(ignored.result);
        QCOMPARE(ignored.value.text, QString("ignored retained\n"));
        CHECK_OK(preview.exportSide("managed", ConflictSide::Result, dir.path() + "/preview"));
        QCOMPARE(readFile(dir.path() + "/preview/untouched.bin"), QByteArray("ignored retained\n"));
        CHECK_OK(service.applySync(ready.value));
        QCOMPARE(readFile(source + "/node/child.txt"), QByteArray("cloud directory\n"));
        QCOMPARE(readFile(source + "/untouched.bin"), QByteArray("ignored retained\n"));
        QCOMPARE(readFile(source + "/.git/config"), QByteArray("protected source metadata"));
        QCOMPARE(readFile(source + "/-' 空格[1]$.txt"), QByteArray("special path\n"));
        QVERIFY(!QFileInfo::exists(source + "/outside.txt"));
        QCOMPARE(readFile(live + "/outside.txt"), QByteArray("cloud repository\n"));
    }
    void resolutionPartialReplacementKeepsRecovery()
    {
        auto files = std::make_shared<FaultFiles>(); BackupDependencies deps; deps.files = files;
        TestDirectory dir; TestBackupService service(pathsIn(dir), nullptr, nullptr, deps);
        const auto source = dir.path() + "/source", remote = dir.path() + "/remote.git", writer = dir.path() + "/writer";
        writeFile(source + "/a.txt", "old a\n"); writeFile(source + "/b.txt", "old b\n");
        writeFile(source + "/.git/config", "source metadata");
        CHECK_OK(service.addLocal(source)); const auto id = service.idForSource(source);
        git({}, {"init", "--bare", remote}); CHECK_OK(service.setRemote(id, remote)); CHECK_OK(service.synchronize(id, true));
        git({}, {"clone", remote, writer});
        writeFile(writer + "/source/a.txt", "cloud a\n"); writeFile(writer + "/source/b.txt", "cloud b\n");
        git(writer, {"add", "--all"}); git(writer, {"commit", "-m", "cloud"}); git(writer, {"push"});
        const auto session = service.prepareSyncResolution(id); CHECK_OK(session.result);
        const auto ready = service.prepareSyncApply(session.value); CHECK_OK(ready.result);
        bool failedOnce = false;
        files->failRename = [&](const QString &from, const QString &to)
        {
            if (!failedOnce && from.contains("/new/") && to == source + "/b.txt") { failedOnce = true; return true; }
            return false;
        };
        QVERIFY(!service.applySync(ready.value).success);
        QVERIFY(failedOnce);
        QCOMPARE(service.syncState(id), BackupSyncState::NeedsAttention);
        QCOMPARE(readFile(source + "/a.txt"), QByteArray("old a\n"));
        QCOMPARE(readFile(source + "/b.txt"), QByteArray("old b\n"));
        QCOMPARE(readFile(source + "/.git/config"), QByteArray("source metadata"));
        QVERIFY(!record(service, id).recoveryPaths.isEmpty());
    }
    void resolutionChoiceSaveFailureKeepsDurableSelection()
    {
        bool fail = false;
        BackupDependencies deps; deps.allowSave = [&](const BackupRecord &r)
        { return !(fail && r.state == BackupSyncState::ResolutionPending && r.operation.isEmpty()); };
        RemoteFixture f(deps); f.commitRemote("cloud\n"); writeFile(f.source, "local\n");
        const auto session = f.service->prepareSyncResolution(f.id); CHECK_OK(session.result);
        fail = true;
        QVERIFY(!f.service->chooseSyncResolution(session.value, "source.txt", 0, ConflictChoice::Local).result.success);
        fail = false;
        const auto recovered = f.service->syncResolution(f.id); CHECK_OK(recovered.result);
        QCOMPARE(recovered.value.files[0].hunks[0].choice, ConflictChoice::Local);
        QVERIFY(recovered.value.revision > session.value.revision);
        QVERIFY(!f.service->chooseSyncResolution(session.value, "source.txt", 0, ConflictChoice::Remote).result.success);
        QCOMPARE(readFile(f.source), QByteArray("local\n"));
    }
    void resolutionRejectsUnsupportedRemoteTrees_data()
    {
        QTest::addColumn<QString>("kind");
        for (const auto *kind : {"deleted-root", "symlink", "submodule", "no-common-history"}) QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void resolutionRejectsUnsupportedRemoteTrees()
    {
        QFETCH(QString, kind);
        RemoteFixture f;
        const auto before = head(*f.service, f.id);
        if (kind == "deleted-root") git(f.writer, {"rm", "source.txt"});
        else if (kind == "no-common-history") git(f.writer, {"checkout", "--orphan", "unrelated"});
        else
        {
            const auto oid = kind == "submodule" ? before : git(f.writer, {"rev-parse", "HEAD:source.txt"}).output.trimmed();
            git(f.writer, {"update-index", "--add", "--cacheinfo", (kind == "submodule" ? "160000," : "120000,") + oid + ",source.txt"});
        }
        git(f.writer, {"commit", "-m", "unsupported remote tree"});
        git(f.writer, {"push", "--force", "origin", "HEAD:main"});
        const auto session = f.service->prepareSyncResolution(f.id);
        QVERIFY(!session.result.success);
        QCOMPARE(head(*f.service, f.id), before);
        QCOMPARE(readFile(f.source), QByteArray("initial\n"));
        QCOMPARE(f.service->syncState(f.id), BackupSyncState::Tracking);
    }
    void linuxCaseSensitiveNamesAndRename()
    {
#ifdef Q_OS_LINUX
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/project";
        writeFile(source + "/README", "upper");
        writeFile(source + "/readme", "lower");
        writeFile(source + "/Old.txt", "renamed");
        CHECK_OK(service.addLocal(source));
        const auto id = service.idForSource(source), initial = head(service, id);
        QVERIFY(QFile::rename(source + "/Old.txt", source + "/old.txt"));
        CHECK_OK(service.backup(id));
        QVERIFY(head(service, id) != initial);
        const auto copy = service.repoPath(id) + "/project";
        QCOMPARE(readFile(copy + "/README"), QByteArray("upper"));
        QCOMPARE(readFile(copy + "/readme"), QByteArray("lower"));
        QVERIFY(!QFileInfo::exists(copy + "/Old.txt"));
        QCOMPARE(readFile(copy + "/old.txt"), QByteArray("renamed"));
        CHECK_OK(service.restore(id, initial));
        QVERIFY(QFileInfo::exists(source + "/Old.txt"));
        QVERIFY(!QFileInfo::exists(source + "/old.txt"));
        QCOMPARE(readFile(source + "/README"), QByteArray("upper"));
        QCOMPARE(readFile(source + "/readme"), QByteArray("lower"));
#else
        QSKIP("Linux case-sensitive filesystem");
#endif
    }
    void linuxExecutableChangesTriggerBackupAndRestore()
    {
#ifdef Q_OS_LINUX
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/run.sh";
        writeFile(source, "#!/bin/sh\nexit 0\n");
        const auto ordinary = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther;
        QVERIFY(QFile::setPermissions(source, ordinary));
        CHECK_OK(service.addLocal(source));
        const auto id = service.idForSource(source), initial = head(service, id);
        QVERIFY(git(service.repoPath(id), {"config", "core.filemode", "false"}).success());
        BackupMonitorOptions options;
        options.quietPeriodMs = 25;
        options.maximumCoalesceMs = 100;
        options.scanIntervalMs = options.auditIntervalMs = 60000;
        BackupMonitor monitor(&service, nullptr, options);
        monitor.start();
        settle(monitor, service);
        const auto modified = QFileInfo(source).lastModified();
        QVERIFY(QFile::setPermissions(source, ordinary | QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther));
        QCOMPARE(QFileInfo(source).lastModified(), modified);
        QTRY_VERIFY_WITH_TIMEOUT(head(service, id) != initial, 10000);
        monitor.stop();
        settle(monitor, service);
        const auto executable = head(service, id);
        QVERIFY(git(service.repoPath(id), {"ls-tree", "HEAD", "--", "run.sh"}).output.startsWith("100755"));
        CHECK_OK(service.restore(id, initial));
        QVERIFY(!QFileInfo(source).permission(QFileDevice::ExeOwner));
        CHECK_OK(service.restore(id, executable));
        QVERIFY(QFileInfo(source).permission(QFileDevice::ExeOwner));
#else
        QSKIP("Linux executable-bit tracking");
#endif
    }
    void linuxPermissionRaceCancelsCapture()
    {
#ifdef Q_OS_LINUX
        TestDirectory dir;
        const auto source = dir.path() + "/script.sh";
        writeFile(source, "content");
        QVERIFY(QFile::setPermissions(source, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        FaultFiles files;
        files.afterCopy = [&](const QString &from, const QString &) {
            if (from == source)
                QFile::setPermissions(source, QFile::permissions(source) | QFileDevice::ExeOwner);
        };
        SourceFingerprint captured;
        QVERIFY(!files.capture(source, false, dir.path() + "/snapshot", captured).success);
#else
        QSKIP("Linux executable-bit snapshot race");
#endif
    }
    void linuxUnreadableDirectoryPreservesBackup()
    {
#ifdef Q_OS_LINUX
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/source", locked = source + "/locked";
        writeFile(locked + "/important", "keep");
        CHECK_OK(service.addLocal(source));
        const auto id = service.idForSource(source), initial = head(service, id);
        const auto permissions = QFile::permissions(locked);
        const auto restorePermissions = qScopeGuard([&] { QFile::setPermissions(locked, permissions); });
        QVERIFY(QFile::setPermissions(locked, {}));
        if (QFileInfo(locked).isReadable())
            QSKIP("Run this test as an unprivileged Linux user");
        QVERIFY(!service.backup(id).success);
        QCOMPARE(head(service, id), initial);
        QCOMPARE(readFile(service.repoPath(id) + "/source/locked/important"), QByteArray("keep"));
#else
        QSKIP("Linux directory permissions");
#endif
    }
    void linuxLegacyFingerprintRefreshKeepsHistory()
    {
#ifdef Q_OS_LINUX
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/file.txt";
        writeFile(source, "legacy");
        CHECK_OK(service.addLocal(source));
        const auto id = service.idForSource(source), initial = head(service, id);
        BackupCatalog catalog(service.paths());
        CHECK_OK(catalog.load());
        auto legacy = *catalog.find(id);
        for (auto &value : legacy.fingerprint)
            value = value.section('|', 0, 2);
        CHECK_OK(catalog.save(legacy));
        CHECK_OK(service.reload());
        CHECK_OK(service.backup(id));
        QCOMPARE(head(service, id), initial);
        QVERIFY(record(service, id).fingerprint.first().endsWith("|100644"));
        legacy.state = BackupSyncState::RemotePending;
        legacy.pendingCommit = initial;
        CHECK_OK(catalog.save(legacy));
        CHECK_OK(service.reload());
        QVERIFY(!service.backup(id).success);
        QCOMPARE(record(service, id).state, BackupSyncState::RemotePending);
#else
        QSKIP("Linux legacy fingerprint refresh");
#endif
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
        QTest::qWait(1200); // Drain delayed registration events from native FSEvents on macOS.
        changed.clear();
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
        // This case isolates the periodic deadline. Delayed native startup record
        // events may legitimately request an earlier audit; stop them before
        // processing events so they cannot leave a pending catalog deadline.
        auto *catalogWatcher = monitor.findChild<BackupCatalogWatcher *>();
        QVERIFY(catalogWatcher);
        catalogWatcher->stop();
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
        CHECK_OK(f.service->renameBranch(f.id, f.service->branchContext(f.id).ref, "working"));
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
    void importantVersionsSynchronizeAcrossClients()
    {
        RemoteFixture f;
        auto &service = *f.service;
        const auto first = head(service, f.id);
        CHECK_OK(service.createTag(service.tagRequest(f.id, first, "交稿版")));
        CHECK_OK(service.synchronize(f.id, true));
        QCOMPARE(GitRepository(f.remote).tagRefs().value.value("交稿版"), first);
        TestDirectory secondDir;
        TestBackupService second(pathsIn(secondDir));
        auto imported = second.prepareImport(f.remote); CHECK_OK(imported.result);
        const auto source = secondDir.path() + "/copy.txt";
        CHECK_OK(second.finishImport(imported.value.sessionId, "source.txt", source));
        const auto secondId = testBackupId(source);
        CHECK_OK(second.removeTag(second.tagRequest(secondId, first, "交稿版", first)));
        CHECK_OK(second.synchronize(secondId, true));
        auto fetched = service.prepareSyncResolution(f.id); CHECK_OK(fetched.result);
        QVERIFY(fetched.value.id.isEmpty());
        QVERIFY(GitRepository(service.repoPath(f.id)).tagRefs().value.isEmpty());
        CHECK_OK(service.createTag(service.tagRequest(f.id, first, "修改前")));
        CHECK_OK(service.synchronize(f.id, true));
        CHECK_OK(second.prepareSyncResolution(secondId).result);
        CHECK_OK(second.renameTag(second.tagRequest(secondId, first, "修改前", first), "定稿"));
        CHECK_OK(second.synchronize(secondId, true));
        CHECK_OK(service.prepareSyncResolution(f.id).result);
        const auto tags = GitRepository(service.repoPath(f.id)).tagRefs().value;
        QCOMPARE(tags.size(), 1); QCOMPARE(tags.value("定稿"), first);
        QCOMPARE(head(service, f.id), first); QCOMPARE(readFile(f.source), QByteArray("initial\n"));
    }
    void importantVersionsConflictsKeepBothCopies()
    {
        RemoteFixture f;
        auto &service = *f.service;
        const auto first = head(service, f.id);
        CHECK_OK(service.createTag(service.tagRequest(f.id, first, "交稿版")));
        const auto remoteCommit = f.commitRemote("cloud\n");
        git(f.writer, {"tag", "交稿版"}); git(f.writer, {"push", "origin", "refs/tags/交稿版"});
        QVERIFY(!service.prepareSyncResolution(f.id).result.success);
        auto conflicts = service.tagConflicts(f.id); QCOMPARE(conflicts.size(), 1);
        QCOMPARE(conflicts[0].localOid, first); QCOMPARE(conflicts[0].remoteOid, remoteCommit);
        CHECK_OK(service.resolveTagConflict(f.id, conflicts[0], "交稿版-本机"));
        QCOMPARE(GitRepository(service.repoPath(f.id)).tagRefs().value.value("交稿版"), remoteCommit);
        QCOMPARE(GitRepository(service.repoPath(f.id)).tagRefs().value.value("交稿版-本机"), first);
        QVERIFY(service.tagConflicts(f.id).isEmpty());
        QVERIFY(!service.resolveTagConflict(f.id, conflicts[0], "旧弹窗").success);
        QCOMPARE(head(service, f.id), first); QCOMPARE(readFile(f.source), QByteArray("initial\n"));
        // A deletion racing an edited cloud tag retains the cloud version.
        CHECK_OK(service.removeTag(service.tagRequest(f.id, remoteCommit, "交稿版", remoteCommit)));
        git(f.writer, {"tag", "-f", "交稿版", first}); git(f.writer, {"push", "--force", "origin", "refs/tags/交稿版"});
        const auto updated = service.prepareSyncResolution(f.id); CHECK_OK(updated.result);
        QCOMPARE(GitRepository(service.repoPath(f.id)).tagRefs().value.value("交稿版"), first);
        QCOMPARE(readFile(f.source), QByteArray("initial\n"));
    }
    void importantVersionsRetryAtomicRenameAfterRestart()
    {
        bool rejectTags = false;
        BackupDependencies deps;
        deps.git = [&](const QString &repo, const QStringList &args, const GitOptions &options)
        {
            if (rejectTags && args.contains("push") && args.contains("--atomic")) return failedGit();
            return runGit(repo, args, options);
        };
        RemoteFixture f(deps); auto &service = *f.service;
        const auto first = head(service, f.id);
        CHECK_OK(service.createTag(service.tagRequest(f.id, first, "旧名称")));
        CHECK_OK(service.synchronize(f.id, true));
        CHECK_OK(service.renameTag(service.tagRequest(f.id, first, "旧名称", first), "新名称"));
        rejectTags = true;
        const auto failed = service.synchronize(f.id, true); QVERIFY(!failed.success);
        QVERIFY(failed.title.contains("版本已上传"));
        auto remoteTags = GitRepository(f.remote).tagRefs().value;
        QVERIFY(remoteTags.contains("旧名称")); QVERIFY(!remoteTags.contains("新名称"));
        f.service.reset(); rejectTags = false;
        f.service = std::make_unique<TestBackupService>(f.paths, nullptr, nullptr, deps);
        CHECK_OK(f.service->prepareSyncResolution(f.id).result);
        QVERIFY(!GitRepository(f.service->repoPath(f.id)).tagRefs().value.contains("旧名称"));
        CHECK_OK(f.service->synchronize(f.id, true));
        remoteTags = GitRepository(f.remote).tagRefs().value;
        QVERIFY(!remoteTags.contains("旧名称")); QCOMPARE(remoteTags.value("新名称"), first);
    }
    void importantVersionsRemoteAddressesIsolateDeletions()
    {
        RemoteFixture f; auto &service = *f.service;
        const auto first = head(service, f.id);
        CHECK_OK(service.createTag(service.tagRequest(f.id, first, "保留")));
        CHECK_OK(service.synchronize(f.id, true));
        const auto other = f.dir.path() + "/other.git";
        git({}, {"clone", "--bare", f.remote, other});
        CHECK_OK(service.removeTag(service.tagRequest(f.id, first, "保留", first)));
        CHECK_OK(service.setRemote(f.id, other));
        CHECK_OK(service.synchronize(f.id, true));
        QCOMPARE(GitRepository(other).tagRefs().value.value("保留"), first);
        QCOMPARE(GitRepository(f.remote).tagRefs().value.value("保留"), first);
    }
    void importantVersionsAtomicPushRejectsChangedRemoteTag()
    {
        bool race = false;
        QString remote, first;
        BackupDependencies deps;
        deps.git = [&](const QString &repo, const QStringList &args, const GitOptions &options)
        {
            if (race && args.contains("push") && args.contains("--atomic"))
            {
                race = false;
                git(remote, {"tag", "-a", "-f", "旧名称", "-m", "changed externally", first});
            }
            return runGit(repo, args, options);
        };
        RemoteFixture f(deps); remote = f.remote; first = head(*f.service, f.id);
        CHECK_OK(f.service->createTag(f.service->tagRequest(f.id, first, "旧名称")));
        CHECK_OK(f.service->synchronize(f.id, true));
        CHECK_OK(f.service->renameTag(f.service->tagRequest(f.id, first, "旧名称", first), "新名称"));
        race = true;
        QVERIFY(!f.service->synchronize(f.id, true).success);
        const auto tags = GitRepository(remote).tagRefs().value;
        QVERIFY(tags.contains("旧名称")); QVERIFY(tags.value("旧名称") != first); QVERIFY(!tags.contains("新名称"));
        QCOMPARE(readFile(f.source), QByteArray("initial\n"));
    }
    void importantVersionsOfflineRebuildIsBlocked()
    {
        bool offline = false;
        BackupDependencies deps;
        deps.git = [&](const QString &repo, const QStringList &args, const GitOptions &options)
        {
            if (offline && (args.contains("fetch") || args.contains("push") || args.contains("ls-remote"))) return failedGit();
            return runGit(repo, args, options);
        };
        RemoteFixture f(deps);
        const auto first = head(*f.service, f.id);
        CHECK_OK(f.service->createTag(f.service->tagRequest(f.id, first, "交稿版")));
        writeFile(f.source, "latest\n"); CHECK_OK(f.service->backup(f.id)); CHECK_OK(f.service->synchronize(f.id, true));
        const auto uploaded = head(*f.service, f.id);
        const auto confirmed = f.service->prepareRebuild(f.id); CHECK_OK(confirmed.result);
        offline = true;
        QVERIFY(!f.service->prepareRebuild(f.id).result.success);
        QVERIFY(!f.service->rebuild(confirmed.value).success);
        QCOMPARE(head(*f.service, f.id), uploaded);
        QVERIFY(GitRepository(f.service->repoPath(f.id)).tagRefs().value.contains("交稿版"));
        f.service.reset(); offline = false;
        f.service = std::make_unique<TestBackupService>(f.paths, nullptr, nullptr, deps);
        const auto ready = f.service->prepareRebuild(f.id); CHECK_OK(ready.result); CHECK_OK(f.service->rebuild(ready.value));
        QCOMPARE(git(f.remote, {"rev-parse", "HEAD"}).output.trimmed(), head(*f.service, f.id));
        QVERIFY(GitRepository(f.remote).tagRefs().value.isEmpty());
        QCOMPARE(readFile(f.source), QByteArray("latest\n"));
    }
    void importantVersionsRebuildChecksConfirmedSnapshot()
    {
        RemoteFixture f; auto &service = *f.service;
        const auto first = head(service, f.id);
        CHECK_OK(service.createTag(service.tagRequest(f.id, first, "交稿版")));
        CHECK_OK(service.synchronize(f.id, true));
        auto prepared = service.prepareRebuild(f.id); CHECK_OK(prepared.result);
        QCOMPARE(prepared.value.tags.size(), 1); QCOMPARE(prepared.value.remoteTags.size(), 1);
        CHECK_OK(service.renameTag(service.tagRequest(f.id, first, "交稿版", first), "定稿"));
        QVERIFY(!service.rebuild(prepared.value).success);
        QCOMPARE(head(service, f.id), first);
        prepared = service.prepareRebuild(f.id); CHECK_OK(prepared.result);
        CHECK_OK(service.rebuild(prepared.value));
        QVERIFY(GitRepository(service.repoPath(f.id)).tagRefs().value.isEmpty());
        QVERIFY(GitRepository(f.remote).tagRefs().value.isEmpty());
        QVector<Revision> versions; CHECK_OK(service.history(f.id, versions)); QCOMPARE(versions.size(), 1);
        QCOMPARE(readFile(f.source), QByteArray("initial\n"));
    }
    void importantVersionsRebuildPreservesPushEndpoint()
    {
        RemoteFixture fixture;
        auto &service = *fixture.service;
        const auto repo = service.repoPath(fixture.id);
        const auto pushUrl = QUrl::fromLocalFile(fixture.remote).toString();
        git(repo, {"remote", "set-url", "--push", "origin", pushUrl});
        const auto first = head(service, fixture.id);
        CHECK_OK(service.createTag(service.tagRequest(fixture.id, first, "交稿版")));
        CHECK_OK(service.synchronize(fixture.id, true));
        writeFile(fixture.source, "latest\n");
        CHECK_OK(service.backup(fixture.id));
        CHECK_OK(service.synchronize(fixture.id, true));
        const auto prepared = service.prepareRebuild(fixture.id);
        CHECK_OK(prepared.result);
        const auto rebuilt = service.rebuild(prepared.value);
        CHECK_OK(rebuilt);
        QCOMPARE(git(repo, {"remote", "get-url", "--push", "origin"}).output.trimmed(), pushUrl);
        QCOMPARE(GitRepository(repo).tagEndpoint().value, prepared.value.endpoint);
        QVERIFY2(rebuilt.warning.isEmpty(), qPrintable(rebuilt.warning));
        QCOMPARE(git(fixture.remote, {"rev-parse", "HEAD"}).output.trimmed(), head(service, fixture.id));
        QVERIFY(GitRepository(fixture.remote).tagRefs().value.isEmpty());
        QCOMPARE(readFile(fixture.source), QByteArray("latest\n"));
    }
    void importantVersionsResumePreparedSyncOffline()
    {
        bool offline = false;
        BackupDependencies dependencies;
        dependencies.git = [&](const QString &repo, const QStringList &args, const GitOptions &options)
        {
            if (offline && (args.contains("fetch") || args.contains("ls-remote"))) return failedGit();
            return runGit(repo, args, options);
        };
        RemoteFixture fixture(dependencies);
        fixture.commitRemote("cloud\n");
        writeFile(fixture.source, "local\n");
        const auto prepared = fixture.service->prepareSyncResolution(fixture.id);
        CHECK_OK(prepared.result);
        QVERIFY(!prepared.value.id.isEmpty());
        offline = true;
        const auto resumed = fixture.service->prepareSyncResolution(fixture.id);
        CHECK_OK(resumed.result);
        QCOMPARE(resumed.value.id, prepared.value.id);
        QCOMPARE(resumed.value.revision, prepared.value.revision);
        QCOMPARE(readFile(fixture.source), QByteArray("local\n"));
    }
    void importantVersionsPullRetainsConcurrentDeletionWarning()
    {
        RemoteFixture fixture;
        auto &service = *fixture.service;
        const auto first = head(service, fixture.id);
        CHECK_OK(service.createTag(service.tagRequest(fixture.id, first, "交稿版")));
        CHECK_OK(service.synchronize(fixture.id, true));
        CHECK_OK(service.removeTag(service.tagRequest(fixture.id, first, "交稿版", first)));
        const auto remoteCommit = fixture.commitRemote("cloud\n");
        git(fixture.writer, {"tag", "交稿版", remoteCommit});
        git(fixture.writer, {"push", "--force", "origin", "refs/tags/交稿版"});
        const auto pulled = service.synchronize(fixture.id, false);
        CHECK_OK(pulled);
        QVERIFY2(pulled.warning.contains("交稿版"), qPrintable(pulled.warning));
        QCOMPARE(GitRepository(service.repoPath(fixture.id)).tagRefs().value.value("交稿版"), remoteCommit);
        QCOMPARE(readFile(fixture.source), QByteArray("initial\n"));
    }
    void importantVersionsPreserveContentAndIdentity()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/draft.txt";
        writeFile(source, "first\n"); CHECK_OK(service.addLocal(source));
        const auto id = testBackupId(source), first = head(service, id), repo = service.repoPath(id);
        writeFile(source, "second\n"); CHECK_OK(service.backup(id));
        const auto latest = head(service, id);
        writeFile(repo + "/draft.txt", "external staged\n"); git(repo, {"add", "draft.txt"});
        const auto index = git(repo, {"write-tree"}).output;
        CHECK_OK(service.createTag(service.tagRequest(id, first, "  交稿版  ")));
        QCOMPARE(git(repo, {"rev-parse", "refs/tags/交稿版"}).output.trimmed(), first);
        QCOMPARE(head(service, id), latest);
        QCOMPARE(git(repo, {"write-tree"}).output, index);
        QCOMPARE(readFile(source), QByteArray("second\n"));
        QVERIFY(!service.createTag(service.tagRequest(id, latest, "交稿版")).success);
        for (const auto &name : {QString(), QString("bad name"), QString("a..b"), QString("-x"), QString("a\nb")})
            QVERIFY(!service.createTag(service.tagRequest(id, latest, name)).success);
        auto stale = service.tagRequest(id, first, "交稿版", latest);
        QVERIFY(!service.removeTag(stale).success);
        auto request = service.tagRequest(id, first, "交稿版", first);
        CHECK_OK(service.renameTag(request, "定稿"));
        QVERIFY(!service.removeTag(request).success);
        CHECK_OK(service.removeTag(service.tagRequest(id, first, "定稿", first)));
        QCOMPARE(head(service, id), latest);
        QCOMPARE(git(repo, {"write-tree"}).output, index);
        CHECK_OK(service.createTag(service.tagRequest(id, latest, "完成")));
        QVERIFY(!service.editMessage(id, latest, "new description").success);
        QCOMPARE(head(service, id), latest);
        auto oldGeneration = service.tagRequest(id, first, "过期"); ++oldGeneration.generation;
        QVERIFY(!service.createTag(oldGeneration).success);
    }
    void importantVersionsReadAnnotatedAndDetachedTags()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/draft.txt";
        writeFile(source, "first\n"); CHECK_OK(service.addLocal(source));
        const auto id = testBackupId(source), repo = service.repoPath(id), first = head(service, id);
        git(repo, {"tag", "-a", "附注", "-m", "tag message"});
        git(repo, {"tag", "别名"});
        git(repo, {"commit", "--amend", "-m", "replaced outside"});
        const auto current = head(service, id);
        QVector<Revision> history; CHECK_OK(service.history(id, history));
        QCOMPARE(history.size(), 2);
        int marked = 0;
        for (const auto &revision : history)
            if (revision.hash == first) { QCOMPARE(revision.tags.size(), 2); marked += revision.tags.size(); }
        QCOMPARE(marked, 2);
        const auto tagOid = git(repo, {"rev-parse", "refs/tags/附注"}).output.trimmed();
        CHECK_OK(service.renameTag(service.tagRequest(id, first, "附注", tagOid), "改名后"));
        QCOMPARE(git(repo, {"rev-parse", "refs/tags/改名后"}).output.trimmed(), tagOid);
        QCOMPARE(git(repo, {"rev-parse", "refs/tags/改名后^{}"}).output.trimmed(), first);
        QCOMPARE(head(service, id), current);
        BackupStats stats; CHECK_OK(service.statistics(id, stats)); QCOMPARE(stats.versionCount, 2);
        CHECK_OK(service.removeTag(service.tagRequest(id, first, "改名后", tagOid)));
        CHECK_OK(service.removeTag(service.tagRequest(id, first, "别名", first)));
        CHECK_OK(service.history(id, history)); QCOMPARE(history.size(), 2);
        QCOMPARE(history.first().hash, current);
    }
    void importantVersionsBatchHistoryPreservesClockSkewAndAliases()
    {
        TestDirectory dir;
        const auto repo = dir.path() + "/repository";
        QVERIFY(QDir().mkpath(repo));
        GitRepository repository(repo);
        CHECK_OK(repository.initialize("main"));
        QByteArray stream;
        const auto appendCommit = [&](const QString &ref, int mark, int timestamp, const QString &message, int parent)
        {
            const auto text = message.toUtf8();
            stream += "commit " + ref.toUtf8() + "\nmark :" + QByteArray::number(mark) +
                "\ncommitter Fixture <fixture@example.test> " + QByteArray::number(timestamp) +
                " +0000\ndata " + QByteArray::number(text.size()) + '\n' + text + '\n';
            if (parent) stream += "from :" + QByteArray::number(parent) + '\n';
            stream += '\n';
        };
        appendCommit("refs/heads/main", 1, 900, "root", 0);
        appendCommit("refs/heads/main", 2, 100, "skewed parent", 1);
        appendCommit("refs/heads/main", 3, 500, "current", 2);
        for (int index = 0; index < 210; ++index)
            appendCommit("refs/tags/standalone-" + QString::number(index), index + 4, index + 600, "standalone-" + QString::number(index), 1);
        appendCommit("refs/tags/oldest", 214, 50, "oldest", 1);
        stream += "reset refs/tags/alias\nfrom :213\n\n";
        CHECK_OK(GitRepository::outcome(repository.run({"fast-import", "--quiet"}, stream)));
        const auto aliased = repository.resolve("refs/tags/alias");
        CHECK_OK(aliased.result);
        git(repo, {"update-ref", "refs/zcversionbox-history/" + aliased.value, aliased.value});
        const auto history = repository.history();
        CHECK_OK(history.result);
        QCOMPARE(history.value.size(), 214);
        QCOMPARE(history.value.first().hash, repository.head().value);
        QCOMPARE(history.value.first().message, QString("current"));
        QCOMPARE(history.value[1].hash, aliased.value);
        QCOMPARE(history.value[1].tags.size(), 2);
        for (int index = 0; index < 210; ++index)
            QCOMPARE(history.value[index + 1].message, "standalone-" + QString::number(209 - index));
        QCOMPARE(history.value[211].message, QString("skewed parent"));
        QCOMPARE(history.value[212].message, QString("root"));
        QCOMPARE(history.value[213].message, QString("oldest"));
    }
    void importantVersionsRecoverDurableJournal()
    {
        TestDirectory dir;
        bool failSave = false, failFinal = false;
        BackupDependencies deps;
        deps.allowSave = [&](const BackupRecord &r) { return !failSave && !(failFinal && r.tagJournal.isEmpty()); };
        const auto source = dir.path() + "/draft.txt";
        writeFile(source, "first\n");
        QString id, commit;
        {
            TestBackupService service(pathsIn(dir), nullptr, nullptr, deps);
            CHECK_OK(service.addLocal(source)); id = testBackupId(source); commit = head(service, id);
            failSave = true;
            QVERIFY(!service.createTag(service.tagRequest(id, commit, "不能保存")).success);
            QVERIFY(GitRepository(service.repoPath(id)).tagRefs().value.isEmpty());
            failSave = false; failFinal = true;
            QVERIFY(!service.createTag(service.tagRequest(id, commit, "已保存")).success);
            QCOMPARE(record(service, id).tagJournal.size(), 1);
            QCOMPARE(GitRepository(service.repoPath(id)).tagRefs().value.value("已保存"), commit);
            failFinal = false;
        }
        TestBackupService reopened(pathsIn(dir)); CHECK_OK(reopened.reload());
        QVERIFY(record(reopened, id).tagJournal.isEmpty());
        QCOMPARE(GitRepository(reopened.repoPath(id)).tagRefs().value.value("已保存"), commit);
        // Crash before update-ref: the durable intent is completed on the next task.
        BackupCatalog catalog(pathsIn(dir)); CHECK_OK(catalog.load()); auto r = *catalog.find(id);
        r.tagJournal = {{"恢复前", {}, commit}}; CHECK_OK(catalog.save(r));
        CHECK_OK(reopened.reload());
        QCOMPARE(GitRepository(reopened.repoPath(id)).tagRefs().value.value("恢复前"), commit);
        QCOMPARE(readFile(source), QByteArray("first\n"));
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
    void diffParserParsesAndRenders()
    {
        const QString sampleDiff =
            "diff --git a/doc.txt b/doc.txt\n"
            "index 1234567..89abcdef 100644\n"
            "--- a/doc.txt\n"
            "+++ b/doc.txt\n"
            "@@ -10,3 +10,4 @@ Section One\n"
            " Context line\n"
            "-Old price ¥19\n"
            "+New price ¥29\n"
            "+Added feature line\n"
            " Tail line\n";

        const auto parsed = DiffParser::parse(sampleDiff);
        QCOMPARE(parsed.isBinary, false);
        QCOMPARE(parsed.addedCount, 2);
        QCOMPARE(parsed.deletedCount, 1);
        QVERIFY(!parsed.sideBySideRows.isEmpty());
        QVERIFY(!parsed.unifiedRows.isEmpty());

        DiffParser::RenderColors colors;
        colors.canvas = QColor("#ffffff");
        colors.surface = QColor("#f6f8fa");
        colors.text = QColor("#24292f");
        colors.secondaryText = QColor("#57606a");
        colors.border = QColor("#d0d7de");
        colors.addedBg = QColor("#dafbe1");
        colors.addedText = QColor("#1a7f37");
        colors.addedWordBg = QColor("#aceebb");
        colors.removedBg = QColor("#ffebe9");
        colors.removedText = QColor("#cf222e");
        colors.removedWordBg = QColor("#ffc1c0");
        colors.headerBg = QColor("#f6f8fa");
        colors.emptyBg = QColor("#f6f8fa");

        const auto sbsHtml = DiffParser::renderHtml(parsed, DiffParser::ViewMode::SideBySide, colors, "monospace");
        QVERIFY(sbsHtml.contains("修改前 (旧版本)"));
        QVERIFY(sbsHtml.contains("修改后 (当前版本)"));
        QVERIFY(!sbsHtml.contains("第 10 行附近的内容变更"));
        QVERIFY(sbsHtml.contains("New</span>"));
        QVERIFY(sbsHtml.contains("Old</span>"));
        QVERIFY(sbsHtml.contains("price"));

        const auto uniHtml = DiffParser::renderHtml(parsed, DiffParser::ViewMode::Unified, colors, "monospace");
        QVERIFY(!uniHtml.contains("第 10 行附近的内容变更"));
        QVERIFY(uniHtml.contains("Added feature line"));

        const auto binaryDiff = "Binary files a/img.png and b/img.png differ\n";
        const auto parsedBinary = DiffParser::parse(binaryDiff);
        QCOMPARE(parsedBinary.isBinary, true);
        const auto binHtml = DiffParser::renderHtml(parsedBinary, DiffParser::ViewMode::SideBySide, colors, "monospace");
        QVERIFY(binHtml.contains("二进制文件变更"));

        // diffHunk
        const auto hunkParsed = DiffParser::diffHunk("common header\n", "local line 1\nlocal line 2\n",
                                                     "remote line 1 modified\n", "common footer\n", 1);
        QCOMPARE(hunkParsed.deletedCount, 2);
        QCOMPARE(hunkParsed.addedCount, 1);
        const auto hunkHtml = DiffParser::renderHtml(hunkParsed, DiffParser::ViewMode::SideBySide, colors, "monospace",
                                                     QStringLiteral("此电脑上的内容 (本地)"),
                                                     QStringLiteral("云端的内容 (云端)"));
        QVERIFY(hunkHtml.contains("此电脑上的内容 (本地)"));
        QVERIFY(hunkHtml.contains("云端的内容 (云端)"));
        QVERIFY(hunkHtml.contains("common header"));
        QVERIFY(hunkHtml.contains("local"));
        QVERIFY(hunkHtml.contains("remote"));
        QVERIFY(hunkHtml.contains("common footer"));

        // diffTexts
        const auto textParsed = DiffParser::diffTexts("header\nalpha\nbeta\nfooter\n",
                                                      "header\nalpha modified\ngamma\nbeta\nfooter\n", 1);
        QVERIFY(textParsed.addedCount > 0);
        const auto textHtml = DiffParser::renderHtml(textParsed, DiffParser::ViewMode::SideBySide, colors, "monospace");
        QVERIFY(textHtml.contains("alpha"));
        QVERIFY(textHtml.contains("gamma"));
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
