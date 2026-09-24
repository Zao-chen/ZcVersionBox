#include "backup/backupservice.h"
#include "backup/engine.h"
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>
#include <cstdlib>

using namespace Backup;
namespace
{
void put(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size())
        qFatal("Cannot write test fixture");
}
QByteArray get(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}
struct Fixture
{
    QTemporaryDir temp;
    QString root = temp.path() + "/data", source = temp.path() + "/source";
    Tracking tracking{newId(), source, {}};
    Engine engine{root, tracking};
    Fixture()
    {
        QDir().mkpath(source);
        writeJson(engine.objectPath() + "/config.json", tracking.json());
    }
    TaskResult run(QString operation = "snapshot", QString revision = {}, bool full = true, QStringList dirty = {})
    {
        Request r;
        r.operation = operation;
        r.revision = revision;
        r.fullScan = full;
        r.dirtyPaths = dirty;
        return engine.execute(r);
    }
};
class TestMonitor : public SourceMonitor
{
  public:
    using SourceMonitor::SourceMonitor;
    bool reject = false;
    void start() override
    {
        QTimer::singleShot(0, this, [this]
                           {
                               if (reject)
                                   emit failed("injected unavailable monitor");
                               else
                                   emit ready();
                           });
    }
    void stop() override {}
    void change(QStringList paths, bool lost = false) { emit changed(paths, lost); }
};
} // namespace
class BackupTests : public QObject
{
    Q_OBJECT
  private slots:
    void roundTripAndIncremental()
    {
        Fixture f;
        put(f.source + "/文档 space.txt", "one\r\ntwo\r\n");
        put(f.source + "/.hidden", QByteArray("\0\1\2", 3));
        put(f.source + "/.gitignore", "ignored\n");
        put(f.source + "/ignored", "keep");
        put(f.source + "/build/a", "build");
        put(f.source + "/.git/config", "excluded");
        QDir().mkpath(f.source + "/empty/nested");
        auto first = f.run();
        QVERIFY2(first.success, qPrintable(first.message));
        QVERIFY(first.changed);
        SnapshotStore store(f.engine.repositoryPath());
        QCOMPARE(git(store.path(), {"rev-parse", "--is-bare-repository"}).trimmed(), QByteArray("true"));
        auto s = store.readSnapshot(first.revision);
        QCOMPARE(s.files.size(), 5);
        QVERIFY(!s.files.contains(".git/config"));
        QVERIFY(s.directories.contains("empty/nested"));
        const auto exportPath = f.temp.path() + "/export";
        store.exportSnapshot(first.revision, exportPath);
        QCOMPARE(get(exportPath + "/文档 space.txt"), QByteArray("one\r\ntwo\r\n"));
        QCOMPARE(get(exportPath + "/.hidden"), QByteArray("\0\1\2", 3));
        QVERIFY(QFileInfo(exportPath + "/empty/nested").isDir());
        auto unchanged = f.run();
        QVERIFY2(unchanged.success, qPrintable(unchanged.message));
        QVERIFY(!unchanged.changed);
        QCOMPARE(unchanged.revision, first.revision);
        put(f.source + "/build/a", "changed");
        auto second = f.run("snapshot", {}, false, {f.source + "/build/a"});
        QVERIFY2(second.success, qPrintable(second.message));
        QCOMPARE(second.filesRead, 1);
        QCOMPARE(second.bytesWritten, 7);
        QVERIFY(second.revision != first.revision);
        QFile::remove(f.source + "/ignored");
        auto deletion = f.run("snapshot", {}, false, {f.source + "/ignored"});
        QVERIFY2(deletion.success, qPrintable(deletion.message));
        QVERIFY(!store.readSnapshot(deletion.revision).files.contains("ignored"));
        QCOMPARE(store.head(), deletion.revision);
    }
    void noFiltersAndSpecialNames()
    {
        Fixture f;
        put(f.source + "/.gitattributes", "* text eol=lf filter=bad\n");
        put(f.source + "/data.bin", QByteArray("\0hello\r\n", 8));
#ifndef Q_OS_WIN
        put(f.source + "/line\nquote\".txt", "special");
        put(f.source + "/back\\slash.txt", "backslash");
#endif
        auto r = f.run();
        QVERIFY2(r.success, qPrintable(r.message));
        SnapshotStore store(f.engine.repositoryPath());
        const auto dest = f.temp.path() + "/out";
        store.exportSnapshot(r.revision, dest);
        QCOMPARE(get(dest + "/data.bin"), QByteArray("\0hello\r\n", 8));
#ifndef Q_OS_WIN
        QCOMPARE(get(dest + "/line\nquote\".txt"), QByteArray("special"));
#endif
    }
    void sourceTypesAndEmptyDirectories()
    {
        Fixture f;
        auto empty = f.run();
        QVERIFY2(empty.success, qPrintable(empty.message));
        QDir().mkpath(f.source + "/empty");
        auto r = f.run();
        QVERIFY2(r.success, qPrintable(r.message));
        QVERIFY(r.changed);
#ifndef Q_OS_WIN
        QVERIFY(QFile::link(f.source + "/empty", f.source + "/link"));
        auto bad = f.run();
        QVERIFY(!bad.success);
        QCOMPARE(bad.code, ErrorCode::Unsupported);
        QCOMPARE(SnapshotStore(f.engine.repositoryPath()).head(), r.revision);
#endif
    }
    void restoreProtectsCurrentContent()
    {
        Fixture f;
        put(f.source + "/a", "old");
        auto old = f.run();
        QVERIFY2(old.success, qPrintable(old.message));
        put(f.source + "/a", "unsaved");
        auto restore = f.run("restore", old.revision);
        QVERIFY2(restore.success, qPrintable(restore.message));
        QCOMPARE(get(f.source + "/a"), QByteArray("old"));
        SnapshotStore store(f.engine.repositoryPath());
        const auto parent = QString::fromLatin1(git(store.path(), {"rev-parse", restore.revision + "^"})).trimmed();
        store.exportSnapshot(parent, f.temp.path() + "/protected");
        QCOMPARE(get(f.temp.path() + "/protected/a"), QByteArray("unsaved"));
        QVERIFY(restore.revision != old.revision);
    }
    void interruptedPublication_data()
    {
        QTest::addColumn<QString>("phase");
        QTest::newRow("objects") << "afterObjects";
        QTest::newRow("published") << "afterPublish";
    }
    void interruptedPublication()
    {
        QFETCH(QString, phase);
        Fixture f;
        put(f.source + "/a", "old");
        auto old = f.run();
        QVERIFY(old.success);
        put(f.source + "/a", "new");
        f.engine.setFaultHook([phase](const QString &at)
                              {
                                  if (at == phase)
                                      throw std::runtime_error("simulated process death");
                              });
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, f.run());
        Engine recovered(f.root, f.tracking);
        recovered.recover();
        SnapshotStore store(recovered.repositoryPath());
        if (phase == "afterObjects")
            QCOMPARE(store.head(), old.revision);
        else
            QVERIFY(store.head() != old.revision);
        Request req;
        req.operation = "snapshot";
        req.fullScan = true;
        auto next = recovered.execute(req);
        QVERIFY2(next.success, qPrintable(next.message));
        store.exportSnapshot(store.head(), f.temp.path() + "/recovered");
        QCOMPARE(get(f.temp.path() + "/recovered/a"), QByteArray("new"));
    }
    void interruptedRestore_data()
    {
        QTest::addColumn<QString>("phase");
        for (auto p : {"afterExport", "afterOriginalMoved", "afterSourceSwap", "afterPublish"})
            QTest::newRow(p) << QString(p);
    }
    void interruptedRestore()
    {
        QFETCH(QString, phase);
        Fixture f;
        put(f.source + "/a", "old");
        auto old = f.run();
        QVERIFY(old.success);
        put(f.source + "/a", "current");
        auto current = f.run();
        QVERIFY(current.success);
        f.engine.setFaultHook([phase](const QString &at)
                              {
                                  if (at == phase)
                                      throw std::runtime_error("simulated process death");
                              });
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, f.run("restore", old.revision));
        Engine recovered(f.root, f.tracking);
        recovered.recover();
        QCOMPARE(get(f.source + "/a"), phase == "afterPublish" ? QByteArray("old") : QByteArray("current"));
        QVERIFY(!QFileInfo::exists(recovered.objectPath() + "/transaction.json"));
    }
    void remoteFastForwardAndConflict()
    {
        Fixture a;
        put(a.source + "/a", "initial");
        auto initial = a.run();
        QVERIFY(initial.success);
        const auto remote = a.temp.path() + "/remote.git";
        SnapshotStore(remote).initialize();
        Request configure;
        configure.operation = "setRemote";
        configure.remote = remote;
        QVERIFY(a.engine.execute(configure).success);
        QVERIFY(a.run("push").success);
        Fixture b;
        QDir(b.source).removeRecursively();
        configure.remote = remote;
        QVERIFY(b.engine.execute(configure).success);
        auto imported = b.run("import");
        QVERIFY2(imported.success, qPrintable(imported.message));
        QCOMPARE(get(b.source + "/a"), QByteArray("initial"));
        put(a.source + "/a", "remote");
        QVERIFY(a.run("push").success);
        auto pulled = b.run("pull");
        QVERIFY2(pulled.success, qPrintable(pulled.message));
        QCOMPARE(get(b.source + "/a"), QByteArray("remote"));
        put(a.source + "/a", "remote next");
        QVERIFY(a.run("push").success);
        put(b.source + "/a", "local");
        auto conflict = b.run("pull");
        QVERIFY(!conflict.success);
        QCOMPARE(conflict.code, ErrorCode::Conflict);
        QCOMPARE(conflict.conflict.category, QString("HistoryDiverged"));
        QCOMPARE(get(b.source + "/a"), QByteArray("local"));
    }
    void notesDoNotRewriteSnapshots()
    {
        Fixture f;
        put(f.source + "/a", "a");
        auto r = f.run();
        QVERIFY(r.success);
        Request note;
        note.operation = "annotateAi";
        note.revision = r.revision;
        note.text = "AI note";
        auto n = f.engine.execute(note);
        QVERIFY2(n.success, qPrintable(n.message));
        QCOMPARE(SnapshotStore(f.engine.repositoryPath()).head(), r.revision);
        auto history = f.run("history");
        QVERIFY2(history.success, qPrintable(history.message));
        QCOMPARE(history.data["rows"].toArray().first().toObject()["annotation"].toObject()["ai"].toString(), QString("AI note"));
    }
    void nativeMonitor()
    {
        QTemporaryDir tmp;
        auto monitor = std::unique_ptr<SourceMonitor>(SourceMonitor::create(tmp.path(), nullptr));
        QSignalSpy ready(monitor.get(), &SourceMonitor::ready), changes(monitor.get(), &SourceMonitor::changed), errors(monitor.get(), &SourceMonitor::failed);
        monitor->start();
        QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty() || !errors.isEmpty(), 5000);
        QVERIFY2(errors.isEmpty(), "Native monitor failed");
        put(tmp.path() + "/file", "a");
        QTRY_VERIFY_WITH_TIMEOUT(!changes.isEmpty(), 5000);
        monitor->stop();
    }
    void fileRootAndTypeChanges()
    {
        Fixture f;
        QDir(f.source).removeRecursively();
        put(f.source, "file");
        auto first = f.run();
        QVERIFY2(first.success, qPrintable(first.message));
        SnapshotStore store(f.engine.repositoryPath());
        store.exportSnapshot(first.revision, f.temp.path() + "/file-export");
        QCOMPARE(get(f.temp.path() + "/file-export"), QByteArray("file"));
        QFile::remove(f.source);
        put(f.source + "/child", "directory");
        auto second = f.run();
        QVERIFY2(second.success, qPrintable(second.message));
        QCOMPARE(store.readSnapshot(second.revision).kind, QString("directory"));
        QDir(f.source).removeRecursively();
        auto missing = f.run();
        QVERIFY(!missing.success);
        QCOMPARE(missing.code, ErrorCode::SourceUnavailable);
        QCOMPARE(store.head(), second.revision);
        auto restored = f.run("restore", second.revision);
        QVERIFY2(restored.success, qPrintable(restored.message));
        QCOMPARE(get(f.source + "/child"), QByteArray("directory"));
        put(f.source + "/child", "back");
        auto recreated = f.run();
        QVERIFY2(recreated.success, qPrintable(recreated.message));
    }
    void changedDuringStreamDoesNotPublish()
    {
        Fixture f;
        put(f.source + "/a", "old");
        auto old = f.run();
        QVERIFY(old.success);
        put(f.source + "/a", "next");
        f.engine.setFaultHook([&](const QString &phase)
                              {
                                  if (phase == "beforeObjects")
                                      put(f.source + "/a", "racing change");
                              });
        auto failure = f.run();
        QVERIFY(!failure.success);
        QCOMPARE(failure.code, ErrorCode::SourceChanged);
        QCOMPARE(SnapshotStore(f.engine.repositoryPath()).head(), old.revision);
        f.engine.setFaultHook({});
        QVERIFY(f.run().success);
    }
    void ioFailureKeepsBaseline()
    {
        Fixture f;
        put(f.source + "/a", "old");
        auto old = f.run();
        QVERIFY(old.success);
        put(f.source + "/a", "next");
        f.engine.setFaultHook([](const QString &phase)
                              {
                                  if (phase == "afterObjects")
                                      throw Error(ErrorCode::Io, "injected disk failure", true);
                              });
        auto failure = f.run();
        QVERIFY(!failure.success);
        QVERIFY(failure.retryable);
        QCOMPARE(SnapshotStore(f.engine.repositoryPath()).head(), old.revision);
        f.engine.setFaultHook({});
        auto retried = f.run();
        QVERIFY2(retried.success, qPrintable(retried.message));
        QVERIFY(retried.changed);
    }
    void changedRollbackMaterialBlocksRecovery()
    {
        Fixture f;
        put(f.source + "/a", "old");
        auto old = f.run();
        put(f.source + "/a", "current");
        QVERIFY(f.run().success);
        f.engine.setFaultHook([](const QString &phase)
                              {
                                  if (phase == "afterSourceSwap")
                                      throw std::runtime_error("crash");
                              });
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, f.run("restore", old.revision));
        put(f.source + "/a", "edited after crash");
        Engine recovered(f.root, f.tracking);
        QVERIFY_THROWS_EXCEPTION(Error, recovered.recover());
        QCOMPARE(get(f.source + "/a"), QByteArray("edited after crash"));
        QVERIFY(QFileInfo::exists(recovered.objectPath() + "/transaction.json"));
    }
    void cleanupFailureDoesNotRepeatRestore()
    {
        Fixture f;
        put(f.source + "/a", "old");
        const auto old = f.run();
        put(f.source + "/a", "current");
        QVERIFY(f.run().success);
        f.engine.setFaultHook([](const QString &phase)
                              {
                                  if (phase == "afterPublish")
                                      throw Error(ErrorCode::Io, "injected cleanup failure", true);
                              });
        const auto restored = f.run("restore", old.revision);
        QVERIFY(restored.success);
        QVERIFY(restored.data["recoveryRequired"].toBool());
        QVERIFY(!restored.retryable);
        QCOMPARE(get(f.source + "/a"), QByteArray("old"));
        QCOMPARE(SnapshotStore(f.engine.repositoryPath()).head(), restored.revision);
        QVERIFY(QFileInfo::exists(f.engine.objectPath() + "/transaction.json"));
        f.engine.setFaultHook({});
        const auto cleaned = f.run("recover");
        QVERIFY2(cleaned.success, qPrintable(cleaned.message));
        QCOMPARE(cleaned.revision, restored.revision);
        QVERIFY(!QFileInfo::exists(f.engine.objectPath() + "/transaction.json"));
        QCOMPARE(git(f.engine.repositoryPath(), {"rev-list", "--count", "main"}).trimmed(), QByteArray("3"));
    }
    void restorePreservesGitMetadata_data()
    {
        QTest::addColumn<QString>("phase");
        for (auto phase : {"afterSourceSwap", "afterMetadataMoved", "afterPublish"})
            QTest::newRow(phase) << QString(phase);
    }
    void readOnlyPreviewCanBeCleaned()
    {
        Fixture f;
        put(f.source + "/folder/a", "content");
        const auto revision = f.run().revision;
        const auto preview = f.temp.path() + "/preview";
        SnapshotStore(f.engine.repositoryPath()).exportSnapshot(revision, preview, true);
        QVERIFY(!(QFileInfo(preview + "/folder/a").permissions() & QFile::WriteOwner));
#ifndef Q_OS_WIN
        QVERIFY(!(QFileInfo(preview + "/folder").permissions() & QFile::WriteOwner));
#endif
        removeOwnedPath(preview);
        QVERIFY(!QFileInfo::exists(preview));
    }
    void concurrentSourceEditRetainsBothContents()
    {
        Fixture f;
        put(f.source + "/a", "old");
        const auto old = f.run();
        put(f.source + "/a", "current");
        const auto current = f.run();
        f.engine.setFaultHook([&](const QString &phase)
                              {
                                  if (phase == "afterMetadataMoved")
                                      put(f.source + "/a", "concurrent edit");
                              });
        const auto result = f.run("restore", old.revision);
        QVERIFY(!result.success);
        QCOMPARE(result.conflict.category, QString("SourceChanged"));
        QCOMPARE(SnapshotStore(f.engine.repositoryPath()).head(), current.revision);
        QCOMPARE(get(f.source + "/a"), QByteArray("concurrent edit"));
        const auto journal = readJson(f.engine.objectPath() + "/transaction.json");
        const auto rollback = QFileInfo(f.source).absolutePath() + "/.source.zcversionbox-" + journal["token"].toString() + "-rollback";
        QCOMPARE(get(rollback + "/a"), QByteArray("current"));
    }
    void realProcessInterruption_data()
    {
        QTest::addColumn<QString>("phase");
        for (auto phase : {"afterSourceSwap", "afterPublish"})
            QTest::newRow(phase) << QString(phase);
    }
    void realProcessInterruption()
    {
        QFETCH(QString, phase);
        Fixture f;
        put(f.source + "/a", "old");
        const auto old = f.run();
        put(f.source + "/a", "current");
        QVERIFY(f.run().success);
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--crash-worker", f.root, f.tracking.id, old.revision, phase});
        QVERIFY(child.waitForStarted());
        QVERIFY(child.waitForFinished(15000));
        QCOMPARE(child.exitCode(), 86);
        Engine recovered(f.root, f.tracking);
        recovered.recover();
        QCOMPARE(get(f.source + "/a"), phase == "afterPublish" ? QByteArray("old") : QByteArray("current"));
        QVERIFY(!QFileInfo::exists(recovered.objectPath() + "/transaction.json"));
    }
    void restorePreservesGitMetadata()
    {
        QFETCH(QString, phase);
        Fixture f;
        put(f.source + "/a", "old");
        put(f.source + "/.git/config", "opaque original metadata");
        const auto old = f.run();
        put(f.source + "/a", "current");
        QVERIFY(f.run().success);
        f.engine.setFaultHook([phase](const QString &at)
                              {
                                  if (at == phase)
                                      throw std::runtime_error("crash");
                              });
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, f.run("restore", old.revision));
        Engine recovered(f.root, f.tracking);
        recovered.recover();
        QCOMPARE(get(f.source + "/.git/config"), QByteArray("opaque original metadata"));
        QCOMPARE(get(f.source + "/a"), phase == "afterPublish" ? QByteArray("old") : QByteArray("current"));
    }
    void malformedNotesAreRejected()
    {
        Fixture f;
        put(f.source + "/a", "one");
        const auto first = f.run();
        const auto remote = f.temp.path() + "/remote.git";
        SnapshotStore(remote).initialize();
        Request config;
        config.operation = "setRemote";
        config.remote = remote;
        QVERIFY(f.engine.execute(config).success);
        QVERIFY(f.run("push").success);
        git(remote, {"notes", "--ref=zcversionbox", "add", "-F", "-", first.revision}, "not a supported note");
        const auto pulled = f.run("pull");
        QVERIFY(!pulled.success);
        QCOMPARE(pulled.code, ErrorCode::InvalidFormat);
        QVERIFY(SnapshotStore(f.engine.repositoryPath()).head("refs/notes/zcversionbox").isEmpty());
        git(f.engine.repositoryPath(), {"notes", "--ref=zcversionbox", "add", "-F", "-", first.revision}, "{\"version\":99}");
        Request note;
        note.operation = "annotate";
        note.revision = first.revision;
        note.text = "must not overwrite";
        QCOMPARE(f.engine.execute(note).code, ErrorCode::InvalidFormat);
        QCOMPARE(f.run("history").code, ErrorCode::InvalidFormat);
        QVERIFY(git(f.engine.repositoryPath(), {"notes", "--ref=zcversionbox", "show", first.revision}).contains("99"));
    }
    void interruptedRebuild_data()
    {
        QTest::addColumn<QString>("phase");
        for (auto p : {"rebuildPrepared", "rebuildOriginalMoved", "rebuildPublished"})
            QTest::newRow(p) << QString(p);
    }
    void interruptedRebuild()
    {
        QFETCH(QString, phase);
        Fixture f;
        put(f.source + "/a", "old");
        auto old = f.run();
        QVERIFY(old.success);
        put(f.source + "/a", "new");
        QVERIFY(f.run().success);
        auto base = SnapshotStore(f.engine.repositoryPath()).head();
        f.engine.setFaultHook([phase](const QString &at)
                              {
                                  if (at == phase)
                                      throw std::runtime_error("crash");
                              });
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, f.run("rebuild"));
        Engine recovered(f.root, f.tracking);
        recovered.recover();
        SnapshotStore store(recovered.repositoryPath());
        if (phase == "rebuildPublished")
            QCOMPARE(git(store.path(), {"rev-list", "--count", "main"}).trimmed(), QByteArray("1"));
        else
            QCOMPARE(store.head(), base);
        QCOMPARE(get(f.source + "/a"), QByteArray("new"));
    }
    void oldFormatAndUnsafeManifestAreRejected()
    {
        Fixture f;
        const auto remote = f.temp.path() + "/old.git";
        SnapshotStore old(remote);
        old.initialize();
        const auto tree = git(remote, {"mktree"}).trimmed();
        const auto commit = git(remote, {"commit-tree", QString::fromLatin1(tree)}, "old\n").trimmed();
        old.publish(QString::fromLatin1(commit), {});
        QVERIFY_THROWS_EXCEPTION(Error, old.readSnapshot(QString::fromLatin1(commit)));
        QDir(f.source).removeRecursively();
        Request cfg;
        cfg.operation = "setRemote";
        cfg.remote = remote;
        QVERIFY(f.engine.execute(cfg).success);
        auto imported = f.run("import");
        QVERIFY(!imported.success);
        QCOMPARE(imported.code, ErrorCode::InvalidFormat);
        QVERIFY(!QFileInfo::exists(f.source));
        Snapshot s;
        s.kind = "directory";
        s.name = "../../escape";
        QVERIFY_THROWS_EXCEPTION(Error, Snapshot::parse(s.json()));
        s.name = "safe";
        s.files["../escape"] = {QString(40, 'a'), 1, false};
        QVERIFY_THROWS_EXCEPTION(Error, Snapshot::parse(s.json()));
    }
    void notesDivergenceAndLeaseProtection()
    {
        Fixture a;
        put(a.source + "/a", "one");
        auto first = a.run();
        QVERIFY(first.success);
        const auto remote = a.temp.path() + "/remote.git";
        SnapshotStore(remote).initialize();
        Request cfg;
        cfg.operation = "setRemote";
        cfg.remote = remote;
        QVERIFY(a.engine.execute(cfg).success);
        Request note;
        note.operation = "annotate";
        note.revision = first.revision;
        note.text = "base note";
        QVERIFY(a.engine.execute(note).success);
        QVERIFY(a.run("push").success);
        Fixture b;
        QDir(b.source).removeRecursively();
        QVERIFY(b.engine.execute(cfg).success);
        QVERIFY(b.run("import").success);
        note.text = "remote note";
        QVERIFY(a.engine.execute(note).success);
        QVERIFY(a.run("push").success);
        note.text = "local note";
        QVERIFY(b.engine.execute(note).success);
        auto conflict = b.run("pull");
        QVERIFY(!conflict.success);
        QCOMPARE(conflict.conflict.category, QString("NotesDiverged"));
        put(a.source + "/a", "two");
        QVERIFY(a.run("push").success);
        auto before = SnapshotStore(remote).head();
        Request reset;
        reset.operation = "resetRemote";
        reset.revision = first.revision;
        auto rejected = b.engine.execute(reset);
        QVERIFY(!rejected.success);
        QCOMPARE(rejected.code, ErrorCode::Conflict);
        QCOMPARE(SnapshotStore(remote).head(), before);
        Request resolve;
        resolve.operation = "resolveConflict";
        QCOMPARE(b.engine.execute(resolve).code, ErrorCode::Unsupported);
    }
    void serviceEventsAndIdle()
    {
        QTemporaryDir temp;
        const auto source = temp.path() + "/source";
        put(source + "/a", "one");
        TestMonitor *monitor = nullptr;
        BackupService service(temp.path() + "/data", nullptr, [&](const QString &, QObject *parent)
                              {
                                  monitor = new TestMonitor(parent);
                                  return monitor;
                              });
        QSignalSpy results(&service, &BackupService::taskFinished);
        service.start();
        service.track(source);
        QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 5000);
        QVERIFY(qvariant_cast<TaskResult>(results.last()[0]).success);
        QTest::qWait(2300);
        QCOMPARE(results.size(), 1);
        put(source + "/a", "two");
        monitor->change({source + "/a"});
        monitor->change({source + "/a"});
        QTRY_COMPARE_WITH_TIMEOUT(results.size(), 2, 6000);
        auto changed = qvariant_cast<TaskResult>(results.last()[0]);
        QVERIFY2(changed.success, qPrintable(changed.message));
        QCOMPARE(changed.filesRead, 1);
        put(source + "/b", "new");
        monitor->change({}, true);
        QTRY_COMPARE_WITH_TIMEOUT(results.size(), 3, 6000);
        QVERIFY(qvariant_cast<TaskResult>(results.last()[0]).success);
        service.shutdown();
    }
    void incrementalScanReconcilesReplacedAncestor()
    {
        Fixture f;
        put(f.source + "/sub/a", "before");
        QVERIFY(f.run().success);
        removeOwnedPath(f.source + "/sub");
        put(f.source + "/sub", "replacement file");
        const auto result = f.run("snapshot", {}, false, {f.source + "/sub/a"});
        QVERIFY2(result.success, qPrintable(result.message));
        const auto snapshot = SnapshotStore(f.engine.repositoryPath()).readSnapshot(result.revision);
        QVERIFY(snapshot.files.contains("sub"));
        QVERIFY(!snapshot.files.contains("sub/a"));
        QVERIFY(!snapshot.directories.contains("sub"));
    }
#ifndef Q_OS_WIN
    void incrementalScanRejectsSymlinkAncestor()
    {
        Fixture f;
        put(f.source + "/sub/a", "before");
        const auto before = f.run();
        removeOwnedPath(f.source + "/sub");
        const auto outside = f.temp.path() + "/outside";
        put(outside + "/a", "outside content");
        QVERIFY(QFile::link(outside, f.source + "/sub"));
        const auto result = f.run("snapshot", {}, false, {f.source + "/sub/a"});
        QVERIFY(!result.success);
        QCOMPARE(result.code, ErrorCode::Unsupported);
        QCOMPARE(SnapshotStore(f.engine.repositoryPath()).head(), before.revision);
    }
#endif
    void importDoesNotDeleteConcurrentDestination()
    {
        Fixture origin;
        put(origin.source + "/a", "remote");
        QVERIFY(origin.run().success);
        Fixture target;
        removeOwnedPath(target.source);
        Request configure;
        configure.operation = "setRemote";
        configure.remote = origin.engine.repositoryPath();
        QVERIFY(target.engine.execute(configure).success);
        target.engine.setFaultHook([&](const QString &phase)
                                   {
                                       if (phase == "afterExport")
                                           put(target.source + "/a", "remote");
                                   });
        const auto result = target.run("import");
        QVERIFY(!result.success);
        QCOMPARE(result.conflict.category, QString("SourceChanged"));
        QCOMPARE(get(target.source + "/a"), QByteArray("remote"));
        QVERIFY(SnapshotStore(target.engine.repositoryPath()).head().isEmpty());
    }
    void retriesRemainBoundToOriginalOperation()
    {
        QTemporaryDir temp;
        const auto source = temp.path() + "/source";
        put(source, "content");
        std::atomic_int calls{0};
        BackupService service(temp.path() + "/data", nullptr, [](const QString &, QObject *p)
                              {
                                  return new TestMonitor(p);
                              },
                              [&](const QString &root, const Tracking &tracking)
                              {
                                  auto engine = std::make_shared<Engine>(root, tracking);
                                  engine->setFaultHook([&](const QString &phase)
                                                       {
                                                           if (phase == "beforeObjects" && calls++ == 0)
                                                               throw Error(ErrorCode::Io, "retry snapshot", true);
                                                       });
                                  return engine;
                              });
        QSignalSpy results(&service, &BackupService::taskFinished);
        service.start();
        const auto id = service.track(source);
        QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 5000);
        QVERIFY(qvariant_cast<TaskResult>(results.first().first()).retryable);
        Request configure;
        configure.trackingId = id;
        configure.operation = "setRemote";
        configure.remote = "-invalid";
        service.submit(configure);
        QTRY_COMPARE(results.size(), 2);
        QTRY_COMPARE_WITH_TIMEOUT(results.size(), 3, 7000);
        const auto retried = qvariant_cast<TaskResult>(results.last().first());
        QCOMPARE(retried.operation, QString("snapshot"));
        QVERIFY2(retried.success, qPrintable(retried.message));
        service.shutdown();
    }
    void partialCleanupCanResume_data()
    {
        QTest::addColumn<QString>("mode");
        for (const auto mode : {"published", "rollback", "rebuild"})
            QTest::newRow(mode) << QString(mode);
    }
    void partialCleanupCanResume()
    {
        QFETCH(QString, mode);
        Fixture f;
        put(f.source + "/a", "old");
        put(f.source + "/b", "old");
        const auto old = f.run();
        put(f.source + "/a", "current");
        put(f.source + "/b", "current");
        QVERIFY(f.run().success);
        f.engine.setFaultHook([&](const QString &phase)
                              {
                                  if (mode == "rollback" && phase == "afterSourceSwap")
                                      throw std::runtime_error("interrupt replacement");
                                  if (mode == "published" && phase == "replacementCleanup")
                                  {
                                      const auto journal = readJson(f.engine.objectPath() + "/transaction.json");
                                      const auto rollback = QFileInfo(f.source).absolutePath() + "/.source.zcversionbox-" + journal["token"].toString() + "-rollback";
                                      QFile::remove(rollback + "/a");
                                      throw std::runtime_error("interrupt partial cleanup");
                                  }
                                  if (mode == "rebuild" && phase == "rebuildCleanup")
                                  {
                                      QFile::remove(f.engine.objectPath() + "/repository.previous.git/HEAD");
                                      throw std::runtime_error("interrupt repository cleanup");
                                  }
                              });
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, f.run(mode == "rebuild" ? "rebuild" : "restore", old.revision));
        if (mode == "rollback")
        {
            Engine interrupted(f.root, f.tracking);
            interrupted.setFaultHook([&](const QString &phase)
                                     {
                                         if (phase == "rollbackCleanup")
                                         {
                                             QFile::remove(f.source + "/a");
                                             throw std::runtime_error("interrupt rollback deletion");
                                         }
                                     });
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, interrupted.recover());
        }
        Engine recovered(f.root, f.tracking);
        recovered.recover();
        const QByteArray expected = mode == "published" ? "old" : "current";
        QCOMPARE(get(f.source + "/a"), expected);
        QCOMPARE(get(f.source + "/b"), expected);
        QVERIFY(!QFileInfo::exists(recovered.objectPath() + "/transaction.json"));
    }
    void startupRecoversNestedSourceBeforeScanning_data()
    {
        QTest::addColumn<bool>("conflict");
        QTest::newRow("recovered") << false;
        QTest::newRow("blocked") << true;
    }
    void startupRecoversNestedSourceBeforeScanning()
    {
        QFETCH(bool, conflict);
        QTemporaryDir temp;
        const auto root = temp.path() + "/data", source = temp.path() + "/source";
        Tracking parent{"00000000-0000-4000-8000-000000000001", source, {}};
        Tracking child{"00000000-0000-4000-8000-000000000002", source + "/child", {}};
        Engine parentEngine(root, parent), childEngine(root, child);
        writeJson(root + "/engine.json", {{"format", "zcversionbox-engine"}, {"version", 1}});
        writeJson(parentEngine.objectPath() + "/config.json", parent.json());
        writeJson(childEngine.objectPath() + "/config.json", child.json());
        put(child.source + "/a", "old");
        Request snapshot;
        snapshot.operation = "snapshot";
        snapshot.fullScan = true;
        const auto old = childEngine.execute(snapshot);
        put(child.source + "/a", "current");
        QVERIFY(childEngine.execute(snapshot).success);
        QVERIFY(parentEngine.execute(snapshot).success);
        childEngine.setFaultHook([](const QString &phase)
                                 {
                                     if (phase == "afterSourceSwap")
                                         throw std::runtime_error("crash");
                                 });
        Request restore;
        restore.operation = "restore";
        restore.revision = old.revision;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, childEngine.execute(restore));
        if (conflict)
            put(child.source + "/a", "concurrent edit");
        BackupService service(root, nullptr, [](const QString &, QObject *p)
                              {
                                  return new TestMonitor(p);
                              });
        QSignalSpy results(&service, &BackupService::taskFinished);
        service.start();
        QTRY_COMPARE_WITH_TIMEOUT(results.size(), conflict ? 1 : 2, 7000);
        if (conflict)
        {
            QCOMPARE(qvariant_cast<TaskResult>(results.first().first()).code, ErrorCode::RecoveryRequired);
            QTest::qWait(2300);
            QCOMPARE(results.size(), 1);
            QCOMPARE(get(child.source + "/a"), QByteArray("concurrent edit"));
        }
        else
            QCOMPARE(get(child.source + "/a"), QByteArray("current"));
        const auto saved = SnapshotStore(parentEngine.repositoryPath()).readSnapshot(SnapshotStore(parentEngine.repositoryPath()).head());
        QCOMPARE(saved.files.size(), 1);
        QVERIFY(saved.files.contains("child/a"));
        QCOMPARE(git(parentEngine.repositoryPath(), {"cat-file", "blob", saved.files["child/a"].oid}), QByteArray("current"));
        service.shutdown();
    }
    void failedImportDoesNotBecomeLocalBackup()
    {
        QTemporaryDir temp;
        const auto root = temp.path() + "/data", source = temp.path() + "/source";
        const auto remote = temp.path() + "/invalid.git";
        SnapshotStore(remote).initialize();
        const auto tree = git(remote, {"mktree"}).trimmed();
        const auto commit = QString::fromLatin1(git(remote, {"commit-tree", QString::fromLatin1(tree)}, "old format")).trimmed();
        SnapshotStore(remote).publish(commit, {});
        TestMonitor *monitor = nullptr;
        BackupService service(root, nullptr, [&](const QString &, QObject *p)
                              {
                                  monitor = new TestMonitor(p);
                                  return monitor;
                              });
        QSignalSpy results(&service, &BackupService::taskFinished);
        service.start();
        const auto id = service.track(source, remote, true);
        QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 6000);
        QCOMPARE(qvariant_cast<TaskResult>(results.first().first()).code, ErrorCode::InvalidFormat);
        put(source + "/a", "user created");
        monitor->change({source + "/a"});
        QTest::qWait(2300);
        QCOMPARE(results.size(), 1);
        Request request;
        request.operation = "snapshot";
        request.trackingId = id;
        service.submit(request);
        QTRY_COMPARE(results.size(), 2);
        QVERIFY(!qvariant_cast<TaskResult>(results.last().first()).success);
        QVERIFY(SnapshotStore(root + "/objects/" + id + "/repository.git").head().isEmpty());
        QVERIFY(service.trackings().first().pendingImport);
        QCOMPARE(get(source + "/a"), QByteArray("user created"));
        service.shutdown();
    }
    void restorePreservesConcurrentlyCreatedGitMetadata_data()
    {
        QTest::addColumn<QString>("phase");
        QTest::newRow("before-move") << QString("afterExport");
        QTest::newRow("after-move") << QString("afterMetadataMoved");
    }
    void restorePreservesConcurrentlyCreatedGitMetadata()
    {
        QFETCH(QString, phase);
        Fixture f;
        put(f.source + "/a", "old");
        const auto old = f.run();
        put(f.source + "/a", "current");
        const auto current = f.run();
        f.engine.setFaultHook([&](const QString &at)
                              {
                                  if (at != phase)
                                      return;
                                  auto directory = f.source;
                                  if (phase == "afterMetadataMoved")
                                  {
                                      const auto journal = readJson(f.engine.objectPath() + "/transaction.json");
                                      directory = QFileInfo(f.source).absolutePath() + "/.source.zcversionbox-" + journal["token"].toString() + "-rollback";
                                  }
                                  put(directory + "/.git/config", "concurrent git init");
                              });
        const auto result = f.run("restore", old.revision);
        QVERIFY(!result.success);
        QCOMPARE(get(f.source + "/.git/config"), QByteArray("concurrent git init"));
        QCOMPARE(get(f.source + "/a"), QByteArray("current"));
        QCOMPARE(SnapshotStore(f.engine.repositoryPath()).head(), current.revision);
    }
    void monitorFailureDoesNotPoll()
    {
        QTemporaryDir temp;
        const auto source = temp.path() + "/source";
        put(source, "one");
        BackupService service(temp.path() + "/data", nullptr, [](const QString &, QObject *parent)
                              {
                                  auto monitor = new TestMonitor(parent);
                                  monitor->reject = true;
                                  return monitor;
                              });
        QSignalSpy results(&service, &BackupService::taskFinished);
        service.start();
        service.track(source);
        QTRY_COMPARE(results.size(), 1);
        QCOMPARE(qvariant_cast<TaskResult>(results[0][0]).code, ErrorCode::Monitor);
        QTest::qWait(2200);
        QCOMPARE(results.size(), 1);
        service.shutdown();
    }
    void serviceRetryAndResponsiveness()
    {
        QTemporaryDir temp;
        const auto source = temp.path() + "/source";
        put(source, "one");
        std::atomic_int calls{0};
        int pulses = 0;
        BackupService service(temp.path() + "/data", nullptr, [](const QString &, QObject *p)
                              {
                                  return new TestMonitor(p);
                              },
                              [&](const QString &root, const Tracking &tracking)
                              {
                                  auto engine = std::make_shared<Engine>(root, tracking);
                                  engine->setFaultHook([&](const QString &phase)
                                                       {
                                                           if (phase == "beforeObjects")
                                                           {
                                                               QThread::msleep(150);
                                                               if (calls++ == 0)
                                                                   throw Error(ErrorCode::Io, "transient failure", true);
                                                           }
                                                       });
                                  return engine;
                              });
        QTimer pulse;
        pulse.setInterval(10);
        connect(&pulse, &QTimer::timeout, this, [&]
                {
                    ++pulses;
                });
        pulse.start();
        QSignalSpy results(&service, &BackupService::taskFinished);
        service.start();
        service.track(source);
        QTRY_VERIFY_WITH_TIMEOUT(results.size() >= 2, 10000);
        QVERIFY(!qvariant_cast<TaskResult>(results.first()[0]).success);
        QVERIFY(qvariant_cast<TaskResult>(results.last()[0]).success);
        QVERIFY(pulses > 100);
        service.shutdown();
    }
    void tenThousandFilesIncremental()
    {
        Fixture f;
        for (int i = 0; i < 10000; ++i)
            put(f.source + "/" + QString::number(i) + ".txt", QByteArray::number(i));
        QElapsedTimer timer;
        timer.start();
        auto first = f.run();
        QVERIFY2(first.success, qPrintable(first.message));
        QCOMPARE(first.filesRead, 10000);
        const auto initialMs = timer.elapsed();
        put(f.source + "/500.txt", "updated");
        timer.restart();
        auto second = f.run("snapshot", {}, false, {f.source + "/500.txt"});
        QVERIFY2(second.success, qPrintable(second.message));
        QCOMPARE(second.filesRead, 1);
        QCOMPARE(second.bytesWritten, 7);
        qInfo() << "10000-file initial ms:" << initialMs << "single-file update ms:" << timer.elapsed() << "copied bytes:" << second.bytesWritten;
    }
    void interruptedImportResumesFromPublishedState()
    {
        Fixture origin;
        put(origin.source + "/a", "remote");
        QVERIFY(origin.run().success);
        Fixture destination;
        QDir(destination.source).removeRecursively();
        destination.tracking.remote = origin.engine.repositoryPath();
        destination.tracking.pendingImport = true;
        Engine importing(destination.root, destination.tracking);
        writeJson(importing.objectPath() + "/config.json", destination.tracking.json());
        importing.setFaultHook([](const QString &phase)
                               {
                                   if (phase == "afterPublish")
                                       throw std::runtime_error("crash");
                               });
        Request request;
        request.operation = "import";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, importing.execute(request));
        Engine recovered(destination.root, destination.tracking);
        auto completed = recovered.execute(request);
        QVERIFY2(completed.success, qPrintable(completed.message));
        QCOMPARE(get(destination.source + "/a"), QByteArray("remote"));
        QVERIFY(!Tracking::parse(readJson(recovered.objectPath() + "/config.json")).pendingImport);
    }
    void cancellationRollsBackReplacement()
    {
        Fixture f;
        put(f.source + "/a", "old");
        auto old = f.run();
        put(f.source + "/a", "current");
        auto current = f.run();
        auto cancel = std::make_shared<std::atomic_bool>(false);
        f.engine.setFaultHook([&](const QString &phase)
                              {
                                  if (phase == "afterSourceSwap")
                                      cancel->store(true);
                              });
        Request r;
        r.operation = "restore";
        r.revision = old.revision;
        auto result = f.engine.execute(r, cancel);
        QCOMPARE(result.code, ErrorCode::Cancelled);
        QCOMPARE(get(f.source + "/a"), QByteArray("current"));
        QCOMPARE(SnapshotStore(f.engine.repositoryPath()).head(), current.revision);
    }
    void sameTimestampAndIgnoredGitEvents()
    {
        Fixture f;
        put(f.source + "/a", "111");
        auto initial = f.run();
        const auto time = QFileInfo(f.source + "/a").lastModified();
        put(f.source + "/a", "222");
        QFile file(f.source + "/a");
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(time, QFileDevice::FileModificationTime));
        file.close();
        auto updated = f.run("snapshot", {}, false, {f.source + "/a"});
        QVERIFY(updated.success);
        QVERIFY(updated.revision != initial.revision);
        put(f.source + "/.git/objects/object", "ignored");
        auto ignored = f.run("snapshot", {}, false, {f.source + "/.git/objects/object"});
        QVERIFY2(ignored.success, qPrintable(ignored.message));
        QVERIFY(!ignored.changed);
        QCOMPARE(ignored.filesRead, 0);
    }
    void overlappingSourcesAreSerialized()
    {
        QTemporaryDir temp;
        const auto parent = temp.path() + "/source";
        put(parent + "/child/a", "one");
        std::atomic_int active{0}, maxActive{0};
        BackupService service(temp.path() + "/data", nullptr, [](const QString &, QObject *p)
                              {
                                  return new TestMonitor(p);
                              },
                              [&](const QString &root, const Tracking &tracking)
                              {
                                  auto engine = std::make_shared<Engine>(root, tracking);
                                  engine->setFaultHook([&](const QString &phase)
                                                       {
                                                           if (phase == "beforeObjects")
                                                           {
                                                               const auto current = ++active;
                                                               int prior = maxActive.load();
                                                               while (current > prior && !maxActive.compare_exchange_weak(prior, current))
                                                               {
                                                               }
                                                               QThread::msleep(150);
                                                           }
                                                           if (phase == "afterPublish")
                                                               --active;
                                                       });
                                  return engine;
                              });
        QSignalSpy results(&service, &BackupService::taskFinished);
        service.start();
        service.track(parent);
        service.track(parent + "/child");
        QTRY_COMPARE_WITH_TIMEOUT(results.size(), 2, 7000);
        QCOMPARE(maxActive.load(), 1);
        for (const auto &r : results)
            QVERIFY(qvariant_cast<TaskResult>(r.first()).success);
        service.shutdown();
    }
};
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() == 6 && args[1] == "--crash-worker")
    {
        const auto tracking = Tracking::parse(readJson(args[2] + "/objects/" + args[3] + "/config.json"));
        Engine engine(args[2], tracking);
        engine.setFaultHook([&](const QString &phase)
                            {
                                if (phase == args[5])
                                    std::_Exit(86);
                            });
        Request request;
        request.operation = "restore";
        request.revision = args[4];
        engine.execute(request);
        return 2;
    }
    BackupTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "backup_tests.moc"
