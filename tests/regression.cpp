#include "services/backupmonitor.h"
#include "services/gitcommand.h"
#include "ui/mainwindow.h"
#include "ui/pages/backupsPage.h"
#include "ui/pages/dashboardPage.h"
#include "ui/pages/diffPage.h"
#include "ui/pages/historyPage.h"
#include "ui/pages/settingsPage.h"
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QTextEdit>
#include <QToolButton>
#include <QUrl>
#include <oclero/qlementine/widgets/Expander.hpp>
#include <oclero/qlementine/widgets/Switch.hpp>

namespace
{
void writeFile(const QString &path, const QByteArray &text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(text) != text.size())
        qFatal("Cannot write test fixture");
}
QByteArray readFile(const QString &path)
{
    QFile file(path);
    file.open(QIODevice::ReadOnly);
    return file.readAll();
}
AppPaths pathsIn(const QTemporaryDir &dir) { return {dir.path() + "/Backup", dir.path() + "/config.ini"}; }
QString encoded(const QString &path) { return QString::fromUtf8(QUrl::toPercentEncoding(path)); }
QString head(const BackupService &service, const QString &id) { return runGit(service.repoPath(id), {"rev-parse", "HEAD"}).output.trimmed(); }

class FakeAi : public AiGateway
{
  public:
    QVector<ModelsCallback> models;
    QVector<SummaryCallback> summaries;
    void fetchModels(const AiConfigHelper::RuntimeConfig &, QObject *, ModelsCallback callback) override { models.append(std::move(callback)); }
    void summarize(const AiConfigHelper::RuntimeConfig &, const QString &, QObject *, SummaryCallback callback) override { summaries.append(std::move(callback)); }
};
class CountingBackup : public BackupService
{
  public:
    using BackupService::BackupService;
    int calls{0};
    OperationResult backup(const QString &) override
    {
        ++calls;
        return OperationResult::ok({});
    }
};
} // namespace
class Regression : public QObject
{
    Q_OBJECT
  private slots:
    void initTestCase()
    {
        // Never read user git configuration, install hooks, or contact external remotes.
        QVERIFY(m_environment.isValid());
        writeFile(m_environment.path() + "/gitconfig", "[init]\n defaultBranch = master\n[core]\n autocrlf = false\n");
        qputenv("GIT_CONFIG_NOSYSTEM", "1");
        qputenv("GIT_CONFIG_GLOBAL", (m_environment.path() + "/gitconfig").toUtf8());
        QVERIFY(runGit({}, {"--version"}).success());
        QCoreApplication::setApplicationName("ZcVersionBox tests");
        QCoreApplication::setApplicationVersion("0.1.0");
        m_theme = new ThemeController(this);
    }
    void localBackupRestoreAndDiff()
    {
        QTemporaryDir dir;
        BackupService service(pathsIn(dir));
        const auto source = dir.path() + "/源 file.txt";
        const auto id = encoded(source);
        writeFile(source, "first\n");
        QVERIFY(service.addLocal(source).success);
        const auto first = head(service, id);
        writeFile(source, "second\nextra\n");
        QVERIFY(service.backup(id).success);
        QVector<Revision> revisions;
        QVERIFY(service.history(id, revisions).success);
        QCOMPARE(revisions.size(), 2);
        BackupStats stats;
        QVERIFY(service.statistics(id, stats).success);
        QCOMPARE(stats.fileCount, 1);
        QCOMPARE(stats.versionCount, 2);
        DiffData data;
        QVERIFY(service.diff(id, first, data).success);
        QCOMPARE(data.oldCommit, QString("4b825dc642cb6eb9a060e54bf8d69288fbee4904"));
        QVERIFY(!data.files.isEmpty());
        QCOMPARE(data.files.first().path, QString("源 file.txt"));
        QString firstText;
        QVERIFY(service.diffText(id, data, data.files.first().path, firstText).success);
        QVERIFY(firstText.contains("first"));
        QVERIFY(service.diff(id, head(service, id), data).success);
        QString text;
        QVERIFY(service.diffText(id, data, {}, text).success);
        QVERIFY(text.contains("second"));
        const auto before = head(service, id);
        QVERIFY(service.restore(id, first).success);
        QCOMPARE(readFile(source), QByteArray("first\n"));
        QCOMPARE(head(service, id), before);
        const auto preview = service.preview(id, first);
        QVERIFY(preview.success);
        QVERIFY(QFileInfo::exists(preview.path + "/源 file.txt"));
        QCOMPARE(head(service, id), before);
        QFile::setPermissions(preview.path + "/源 file.txt", QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        QVERIFY(QDir(preview.path).removeRecursively());
        // Baseline keeps the existing short/full-hash edit restriction.
        QVERIFY(!service.editMessage(id, revisions.first().hash, "edited").success);
        QVERIFY(!service.editMessage(id, first, "").success);
        QVERIFY(service.removeBackup(id).success);
        QVERIFY(QFileInfo::exists(source));
        QVERIFY(!service.contains(id));
        QVERIFY(!service.restore(id, first).success);
    }
    void directoryBinaryAndRemoteImport()
    {
        QTemporaryDir dir;
        BackupService service(pathsIn(dir));
        const auto source = dir.path() + "/source";
        const auto id = encoded(source);
        writeFile(source + "/.hidden", "hidden");
        writeFile(source + "/nested/item.txt", "old");
        writeFile(source + "/image.bin", QByteArray("\0first", 6));
        QVERIFY(service.addLocal(source).success);
        const auto first = head(service, id);
        writeFile(source + "/image.bin", QByteArray("\0second", 7));
        QVERIFY(service.backup(id).success);
        DiffData data;
        QVERIFY(service.diff(id, head(service, id), data).success);
        bool binary = false;
        for (const auto &file : data.files)
            binary |= file.summary == "二进制";
        QVERIFY(binary);
        QVERIFY(service.restore(id, first).success);
        QCOMPARE(readFile(source + "/image.bin"), QByteArray("\0first", 6));
        QVERIFY(QFileInfo::exists(source + "/.hidden"));
        const auto remote = dir.path() + "/remote.git";
        QVERIFY(runGit({}, {"init", "--bare", remote}).success());
        QVERIFY(service.setRemote(id, remote).success);
        QVERIFY(service.synchronize(id, true).success);
        QVERIFY(service.synchronize(id, false).success);
        QVERIFY(service.checkRemote(remote).success);
        QVERIFY(service.rebuild(id).success);
        BackupStats stats;
        QVERIFY(service.statistics(id, stats).success);
        QCOMPARE(stats.versionCount, 1);
        QCOMPARE(stats.remoteUrl, remote);
        const auto prepared = service.prepareImport(remote);
        QVERIFY(prepared.result.success);
        QVERIFY(prepared.directory);
        QCOMPARE(service.trackedItems().size(), 1);
        const auto imported = dir.path() + "/imported";
        QVERIFY(service.finishImport(prepared.temporaryRepo, imported).success);
        QCOMPARE(readFile(imported + "/nested/item.txt"), QByteArray("old"));
        QCOMPARE(service.trackedItems().size(), 2);
        const auto cancelled = service.prepareImport(remote);
        QVERIFY(cancelled.result.success);
        service.cancelImport(cancelled.temporaryRepo);
        QVERIFY(!QFileInfo::exists(cancelled.temporaryRepo));
        QVERIFY(service.removeRemote(id).success);
        QVERIFY(service.statistics(id, stats).success);
        QVERIFY(stats.remoteUrl.isEmpty());
    }
    void monitorSurvivesPageRefresh()
    {
        QTemporaryDir dir;
        CountingBackup service(pathsIn(dir));
        const auto source = dir.path() + "/file.txt";
        writeFile(source, "initial");
        QVERIFY(service.addLocal(source).success);
        BackupMonitor monitor(&service);
        monitor.reconcile();
        QCOMPARE(monitor.trackedCount(), 1);
        {
            BackupsPage page(&service);
            for (int i = 0; i < 10; ++i)
                page.refresh();
        }
        writeFile(source, "modified and larger");
        monitor.reconcile();
        monitor.scanNow();
        QCOMPARE(service.calls, 1);
        monitor.scanNow();
        QCOMPARE(service.calls, 1);
        QVERIFY(service.removeBackup(encoded(source)).success);
        QCOMPARE(monitor.trackedCount(), 0);
    }
    void automaticAiMessagesAndDeletedContext()
    {
        QTemporaryDir dir;
        const auto paths = pathsIn(dir);
        const auto source = dir.path() + "/file.txt";
        const auto id = encoded(source);
        {
            QSettings ini(paths.settingsFile, QSettings::IniFormat);
            ini.setValue("AI/Enabled", true);
        }
        QString reply = "AI fixture message";
        bool replaceDuringRequest = false;
        int requests = 0;
        BackupService *active = nullptr;
        BackupService service(paths, nullptr, [&](const QString &diff, const QString &settingsFile)
                              {
                                  ++requests;
                                  if (diff.isEmpty() || settingsFile != paths.settingsFile)
                                      qFatal("Wrong AI request context");
                                  if (replaceDuringRequest)
                                  {
                                      if (!active->removeBackup(id).success || !active->addLocal(source).success)
                                          qFatal("Cannot replace test repository");
                                  }
                                  return reply;
                              });
        active = &service;
        writeFile(source, "one");
        QVERIFY(service.addLocal(source).success);
        writeFile(source, "two");
        QVERIFY(service.backup(id).success);
        QCOMPARE(requests, 1);
        QVector<Revision> revisions;
        QVERIFY(service.history(id, revisions).success);
        QCOMPARE(revisions.first().message, reply);
        reply.clear();
        writeFile(source, "three");
        QVERIFY(service.backup(id).success);
        QVERIFY(service.history(id, revisions).success);
        QVERIFY(revisions.first().message.startsWith("Auto backup - "));
        replaceDuringRequest = true;
        writeFile(source, "four");
        QVERIFY(!service.backup(id).success);
        QVERIFY(service.history(id, revisions).success);
        QCOMPARE(revisions.size(), 1);
        QCOMPARE(revisions.first().message, QString("Initial backup"));
    }
    void navigationUsesIdentifiers()
    {
        Navigation navigation;
        navigation.go({PageId::Dashboard, "first-id"});
        navigation.go({PageId::History, "second-id"});
        QVERIFY(navigation.canBack());
        navigation.back();
        QCOMPARE(navigation.current().backupId, QString("first-id"));
        navigation.forward();
        QCOMPARE(navigation.current().backupId, QString("second-id"));
        navigation.removeBackup("second-id");
        QCOMPARE(navigation.current().page, PageId::Backups);
        navigation.go({PageId::About});
        QVERIFY(!navigation.canForward());
        navigation.retainBackups({});
        navigation.back();
        navigation.back();
        QVERIFY(navigation.current().backupId.isEmpty());
    }
    void duplicateNamesHaveIndependentRoutes()
    {
        QTemporaryDir dir;
        BackupService service(pathsIn(dir));
        const auto first = dir.path() + "/a/same.txt";
        const auto second = dir.path() + "/b/same.txt";
        writeFile(first, "first");
        writeFile(second, "second");
        QVERIFY(service.addLocal(first).success);
        QVERIFY(service.addLocal(second).success);
        BackupsPage page(&service);
        QSignalSpy routes(&page, &BackupsPage::navigate);
        const auto buttons = page.findChildren<QPushButton *>("historyButton");
        QCOMPARE(buttons.size(), 2);
        const auto items = service.trackedItems();
        for (int i = 0; i < buttons.size(); ++i)
        {
            buttons[i]->setText("Changed caption");
            buttons[i]->click();
            QCOMPARE(qvariant_cast<Route>(routes.last().first()).backupId, items[i].id);
        }
        QVERIFY(items[0].id != items[1].id);
    }
    void settingsRejectStaleResponses()
    {
        QTemporaryDir dir;
        const auto paths = pathsIn(dir);
        {
            QSettings ini(paths.settingsFile, QSettings::IniFormat);
            ini.setValue("AI/BaseUrl", "https://api.example.test/v1");
            ini.setValue("AI/ApiKey", "test-only");
            ini.setValue("AI/Model", "old");
        }
        FakeAi gateway;
        SettingsService settings(paths, &gateway);
        QCOMPARE(settings.provider(), QString("Custom"));
        QCOMPARE(settings.config("Custom").modelName, QString("old"));
        settings.fetchModels();
        QCOMPARE(gateway.models.size(), 1);
        settings.selectProvider("OpenAI");
        settings.saveField("ApiKey", "fixture");
        settings.fetchModels();
        QCOMPARE(gateway.models.size(), 2);
        gateway.models[0]({"stale-custom-model"}, {});
        QVERIFY(settings.config("OpenAI").modelList.isEmpty());
        gateway.models[1]({"model-a", "model-b"}, {});
        QCOMPARE(settings.config("OpenAI").modelName, QString("model-a"));
        settings.fetchModels();
        settings.saveField("ApiKey", "changed-key");
        gateway.models[2]({"stale-key-model"}, {});
        QVERIFY(settings.config("OpenAI").modelList.isEmpty());
        settings.setAiEnabled(true);
        QCOMPARE(settings.value("AI/AutoCommitMessage").toBool(), true);
        settings.saveField("Model", "manual");
        QCOMPARE(settings.value("AI/Model").toString(), QString("manual"));
        QSignalSpy changed(&settings, &SettingsService::changed);
        GeneralSettingsPage general(&settings);
        AiSettingsPage ai(&settings);
        ai.refresh();
        ai.refresh();
        QCOMPARE(changed.count(), 0);
        ai.setAttribute(Qt::WA_DontShowOnScreen);
        ai.show();
        auto *key = ai.findChild<QLineEdit *>("apiKey");
        key->setFocus();
        settings.selectProvider("Custom");
        QCOMPARE(key->text(), QString("test-only"));
        settings.selectProvider("OpenAI");
        QCOMPARE(key->text(), QString("changed-key"));
    }
    void stableModelsAndStaleDiff()
    {
        QTemporaryDir dir;
        BackupService service(pathsIn(dir));
        const auto source = dir.path() + "/file.txt";
        writeFile(source, "one");
        QVERIFY(service.addLocal(source).success);
        const auto id = encoded(source);
        const auto commit = head(service, id);
        FakeAi gateway;
        SettingsService settings(pathsIn(dir), &gateway);
        settings.saveField("ApiKey", "fixture");
        settings.saveField("Model", "fixture-model");
        HistoryPage history(&service);
        history.setBackup(id);
        auto *table = history.findChild<QTableView *>("table");
        auto *model = table->model();
        for (int i = 0; i < 20; ++i)
            history.setBackup(id);
        QCOMPARE(table->model(), model);
        DiffPage diff(&service, &settings, &gateway);
        diff.setRevision(id, commit);
        auto *analyze = diff.findChild<QPushButton *>("analyzeButton");
        analyze->click();
        QCOMPARE(gateway.summaries.size(), 1);
        diff.deactivate();
        gateway.summaries[0]("stale-result", {});
        QVERIFY(!diff.findChild<QPlainTextEdit *>("analysis")->toPlainText().contains("stale-result"));
        diff.setRevision(id, commit);
        const auto originalDiff = diff.findChild<QTextEdit *>("content")->toPlainText();
        analyze->click();
        gateway.summaries[1]("current-result", {});
        QCOMPARE(diff.findChild<QPlainTextEdit *>("analysis")->toPlainText(), QString("current-result"));
        QCOMPARE(diff.findChild<QTextEdit *>("content")->toPlainText(), originalDiff);
        analyze->click();
        QCOMPARE(gateway.summaries.size(), 3);
        QVERIFY(service.removeBackup(id).success);
        QVERIFY(service.addLocal(source).success);
        gateway.summaries[2]("deleted-context-result", {});
        QVERIFY(!diff.findChild<QPlainTextEdit *>("analysis")->toPlainText().contains("deleted-context-result"));
        DashboardPage dashboard(&service);
        dashboard.setBackup(id);
        dashboard.findChild<QPushButton *>("expandButton")->click();
        dashboard.findChild<QPushButton *>("expandButton")->click();
        QVERIFY(!runGit(service.repoPath(id), {"remote", "get-url", "origin"}).success());
        const auto other = dir.path() + "/other.txt";
        writeFile(other, "other");
        QVERIFY(service.addLocal(other).success);
        const auto otherId = encoded(other);
        QVERIFY(service.setRemote(id, "https://example.test/first.git").success);
        QVERIFY(service.setRemote(otherId, "https://example.test/second.git").success);
        auto *url = dashboard.findChild<QLineEdit *>("remoteUrl");
        url->setFocus();
        dashboard.setBackup(otherId);
        QCOMPARE(url->text(), QString("https://example.test/second.git"));
        dashboard.findChild<QPushButton *>("expandButton")->click();
        dashboard.findChild<QPushButton *>("expandButton")->click();
        QCOMPARE(runGit(service.repoPath(otherId), {"remote", "get-url", "origin"}).output.trimmed(), QString("https://example.test/second.git"));
        auto *toggle = dashboard.findChild<oclero::qlementine::Switch *>("remoteSwitch");
        QTest::keyClick(toggle, Qt::Key_Space);
        QVERIFY(!toggle->isChecked());
        QVERIFY(!runGit(service.repoPath(otherId), {"remote", "get-url", "origin"}).success());
    }
    void manyRevisionsAndEditFeedback()
    {
        QTemporaryDir dir;
        BackupService service(pathsIn(dir));
        const auto source = dir.path() + "/file.txt";
        const auto id = encoded(source);
        writeFile(source, "initial");
        QVERIFY(service.addLocal(source).success);
        const auto initial = head(service, id).toUtf8();
        QByteArray stream;
        for (int i = 1; i <= 1000; ++i)
        {
            const auto message = QByteArray("Revision ") + QByteArray::number(i);
            stream += "commit refs/heads/master\nmark :" + QByteArray::number(i) + "\ncommitter Fixture <fixture@example.test> " + QByteArray::number(i) + " +0000\ndata " + QByteArray::number(message.size()) + "\n" + message + "\nfrom " + (i == 1 ? initial : ":" + QByteArray::number(i - 1)) + "\n\n";
        }
        QProcess git;
        git.setWorkingDirectory(service.repoPath(id));
        git.start("git", {"fast-import", "--quiet"});
        QVERIFY(git.waitForStarted());
        git.write(stream);
        git.closeWriteChannel();
        QVERIFY(git.waitForFinished());
        QCOMPARE(git.exitCode(), 0);
        HistoryPage page(&service);
        page.setBackup(id);
        auto *table = page.findChild<QTableView *>("table");
        auto *model = qobject_cast<QStandardItemModel *>(table->model());
        QCOMPARE(model->rowCount(), 1001);
        QSignalSpy notifications(&page, &HistoryPage::notification);
        model->item(0, 1)->setText("attempted edit");
        QCoreApplication::processEvents();
        QCOMPARE(notifications.count(), 1);
        QCOMPARE(model->item(0, 1)->text(), QString("Revision 1000"));
        for (int i = 0; i < 5; ++i)
            page.refresh();
        QCOMPARE(table->model(), model);
        QCOMPARE(model->rowCount(), 1001);
    }
    void confirmationsRespectObjectContext()
    {
        QTemporaryDir dir;
        BackupService service(pathsIn(dir));
        const auto source = dir.path() + "/file.txt";
        writeFile(source, "initial");
        QVERIFY(service.addLocal(source).success);
        const auto id = encoded(source);
        DashboardPage page(&service);
        page.setBackup(id);
        bool cancelWasDefault = false;
        QTimer::singleShot(0, [&]
                           {
                               auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                               if (!dialog)
                                   qFatal("Confirmation dialog missing");
                               auto *buttons = dialog->findChild<QDialogButtonBox *>();
                               cancelWasDefault = buttons->button(QDialogButtonBox::Cancel)->isDefault();
                               dialog->reject();
                           });
        page.findChild<QPushButton *>("removeButton")->click();
        QVERIFY(cancelWasDefault);
        QVERIFY(service.contains(id));
        QTimer::singleShot(0, [&]
                           {
                               if (!service.removeBackup(id).success || !service.addLocal(source).success)
                                   qFatal("Cannot replace confirmation fixture");
                               qobject_cast<QDialog *>(QApplication::activeModalWidget())->accept();
                           });
        page.findChild<QPushButton *>("removeButton")->click();
        QVERIFY(service.contains(id));
    }
    void renderAllPages()
    {
        QTemporaryDir dir;
        BackupService service(pathsIn(dir));
        const auto source = dir.path() + "/演示项目/My project.txt";
        writeFile(source, "hello\nworld\n");
        QVERIFY(service.addLocal(source).success);
        const auto id = encoded(source);
        writeFile(source, "hello\nQlementine\n");
        QVERIFY(service.backup(id).success);
        FakeAi gateway;
        SettingsService settings(pathsIn(dir), &gateway);
        MainWindow window(&service, &settings, &gateway, m_theme, false);
        settings.saveField("ApiKey", "fixture-only");
        settings.saveField("Model", "test-model");
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.show();
        const auto output = qEnvironmentVariable("ZC_TEST_SCREENSHOTS");
        if (!output.isEmpty())
            QDir().mkpath(output);
        const QVector<Route> routes{{}, {PageId::Dashboard, id}, {PageId::History, id}, {PageId::Diff, id, head(service, id)}, {PageId::GeneralSettings}, {PageId::AiSettings}, {PageId::About}};
        for (int theme = 0; theme < 2; ++theme)
        {
            for (int i = 0; i < routes.size(); ++i)
            {
                window.navigate(routes[i]);
                QTest::qWait(230);
                QVERIFY(window.isVisible());
                QVERIFY(window.width() <= 1100);
                if (!output.isEmpty())
                    QVERIFY(window.grab().save(output + QString("/page-%1-%2.png").arg(theme).arg(i)));
                if (routes[i].page == PageId::Diff)
                {
                    window.findChild<DiffPage *>()->findChild<QPushButton *>("analyzeButton")->click();
                    QVERIFY(!gateway.summaries.isEmpty());
                    gateway.summaries.last()("本次版本更新了示例文档。\n\n• 保留原有问候文本\n• 将第二行内容更新为 Qlementine\n\n原始差异仍显示在上方。", {});
                    QTest::qWait(230);
                    if (!output.isEmpty())
                        QVERIFY(window.grab().save(output + QString("/analysis-%1.png").arg(theme)));
                }
            }
            m_theme->toggle();
        }
        window.resize(760, 520);
        window.navigate({PageId::Dashboard, id});
        QTest::qWait(230);
        QCOMPARE(window.size(), QSize(760, 520));
        if (!output.isEmpty())
            QVERIFY(window.grab().save(output + "/compact.png"));
        window.findChild<DashboardPage *>()->findChild<QPushButton *>("expandButton")->click();
        QTest::qWait(230);
        if (!output.isEmpty())
            QVERIFY(window.grab().save(output + "/compact-expanded.png"));
        auto *dashboard = window.findChild<DashboardPage *>();
        auto *sourceField = dashboard->findChild<QLineEdit *>("sourcePathEdit");
        const auto longPath = "C:/Workspace/中文目录/" + QString(180, 'x') + "/项目文件.txt";
        sourceField->setText(longPath);
        sourceField->setCursorPosition(0);
        QVERIFY(sourceField->isReadOnly());
        sourceField->selectAll();
        QCOMPARE(sourceField->selectedText(), longPath);
        QTest::qWait(20);
        QCOMPARE(window.size(), QSize(760, 520));
        QCOMPARE(dashboard->findChild<QScrollArea *>("scroll")->horizontalScrollBar()->maximum(), 0);
        if (!output.isEmpty())
            QVERIFY(window.grab().save(output + "/long-path.png"));
        window.findChild<QToolButton *>("collapseButton")->click();
        QVERIFY(!window.findChild<QWidget *>("sidebar")->isVisible());
        window.findChild<QToolButton *>("collapseButton")->click();
        QVERIFY(window.findChild<QWidget *>("sidebar")->isVisible());
        window.findChild<QToolButton *>("pinButton")->click();
        QVERIFY(window.windowFlags().testFlag(Qt::WindowStaysOnTopHint));
        window.findChild<QToolButton *>("pinButton")->click();
        QVERIFY(!window.windowFlags().testFlag(Qt::WindowStaysOnTopHint));
        window.navigate({PageId::About});
        window.findChild<QToolButton *>("backButton")->click();
        QCOMPARE(window.findChild<QStackedWidget *>("pages")->currentWidget(), window.findChild<DashboardPage *>());
        window.findChild<QToolButton *>("forwardButton")->click();
        QCOMPARE(window.findChild<QStackedWidget *>("pages")->currentWidget()->objectName(), QString("AboutPage"));
        QVERIFY(service.removeBackup(id).success);
        window.navigate({});
        QTest::qWait(230);
        QVERIFY(window.findChild<BackupsPage *>()->findChild<QLabel *>("emptyLabel")->isVisible());
        if (!output.isEmpty())
            QVERIFY(window.grab().save(output + "/empty.png"));
    }

  private:
    QTemporaryDir m_environment;
    ThemeController *m_theme{nullptr};
};
QTEST_MAIN(Regression)
#include "regression.moc"
