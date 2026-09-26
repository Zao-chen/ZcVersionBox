#include "backup_test_support.h"
#include "utils/backupmonitor.h"
#include "utils/gitcommand.h"
#include "windows/mainwindow.h"
#include "windows/mainwindow_child/homepage/homepage.h"
#include "windows/mainwindow_child/homepage/pages/homepage_page_backup.h"
#include "windows/mainwindow_child/homepage/pages/homepage_page_dashboard.h"
#include "windows/mainwindow_child/homepage/pages/homepage_page_diff.h"
#include "windows/mainwindow_child/homepage/pages/homepage_page_trackfiles.h"
#include "windows/mainwindow_child/homepage/trackfiles/homepagechild_trackfile.h"
#include "windows/mainwindow_child/settingpage/settingpage.h"
#include <QAbstractItemModelTester>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCompleter>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFontInfo>
#include <QGlyphRun>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPersistentModelIndex>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRawFont>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QTextEdit>
#include <QTextLayout>
#include <QTimeZone>
#include <QToolButton>
#include <QUrl>
#include <algorithm>
#include <oclero/qlementine/widgets/Expander.hpp>
#include <oclero/qlementine/widgets/LoadingSpinner.hpp>
#include <oclero/qlementine/widgets/Popover.hpp>
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
QString encoded(const QString &path) { return testBackupId(path); }
QString head(const BackupService &service, const QString &id) { return runGit(service.repoPath(id), {"rev-parse", "HEAD"}).output.trimmed(); }
QPoint historyActionPoint(QTableView *table, int row, int action)
{
    const auto cell = table->visualRect(table->model()->index(row, table->model()->columnCount() - 1));
    return {cell.x() + (2 * action + 1) * cell.width() / 6, cell.center().y()};
}
void hoverHistoryAction(QTableView *table, int row, int action)
{
    const auto point = historyActionPoint(table, row, action);
    // WA_DontShowOnScreen windows cannot receive native cursor motion in render fixtures.
    QMouseEvent move(QEvent::MouseMove, point, table->viewport()->mapToGlobal(point), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(table->viewport(), &move);
}

class PreviewUrls : public QObject
{
    Q_OBJECT
  public:
    PreviewUrls() { QDesktopServices::setUrlHandler("file", this, "opened"); }
    ~PreviewUrls() override { QDesktopServices::unsetUrlHandler("file"); }
    QList<QUrl> urls;
  public slots:
    void opened(const QUrl &url) { urls.append(url); }
};

class FakeAi : public AiGateway
{
  public:
    QVector<ModelsCallback> models;
    QVector<SummaryCallback> summaries;
    QVector<SummaryCallback> messages;
    void fetchModels(const AiConfigHelper::RuntimeConfig &, QObject *, ModelsCallback callback) override { models.append(std::move(callback)); }
    void summarize(const AiConfigHelper::RuntimeConfig &, const QString &, QObject *, SummaryCallback callback) override { summaries.append(std::move(callback)); }
    void generateCommitMessage(const AiConfigHelper::RuntimeConfig &, const QString &, QObject *, SummaryCallback callback) override { messages.append(std::move(callback)); }
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
        m_theme = new ThemeController(nullptr, this);
    }
    void typographyUsesThemeRoles()
    {
        const auto body = UiStyle::font(UiStyle::FontRole::Body);
        const auto caption = UiStyle::font(UiStyle::FontRole::Caption);
        const auto title = UiStyle::font(UiStyle::FontRole::Object);
        QVERIFY(body.pointSizeF() > caption.pointSizeF());
        QVERIFY(title.pointSizeF() > body.pointSizeF());
        QCOMPARE(body.weight(), QFont::Normal);
        QCOMPARE(title.weight(), QFont::DemiBold);
        QTextLayout layout("文件备份 Aa", body);
        layout.beginLayout();
        auto line = layout.createLine();
        line.setLineWidth(400);
        layout.endLayout();
        QStringList faces;
        for (const auto &run : layout.glyphRuns())
            faces.append(run.rawFont().familyName());
        qInfo().noquote() << "Body fonts:" << body.families().join(", ") << "; resolved:" << faces.join(", ")
                          << "; pixels:" << QFontInfo(body).pixelSize();
        m_theme->toggle();
        QCOMPARE(UiStyle::font(UiStyle::FontRole::Body), body);
        m_theme->toggle();
    }
    void focusVisiblePolicy()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        FakeAi gateway;
        SettingsService settings(pathsIn(dir), &gateway);
        MainWindow window(&service, &settings, &gateway, m_theme, false);
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.show();

        UiStyle::setKeyboardNavigationActive(false);
        QVERIFY(!UiStyle::isKeyboardNavigationActive());

        // Pressing Tab enables keyboard navigation active state
        QKeyEvent tabPress(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
        qApp->sendEvent(&window, &tabPress);
        QVERIFY(UiStyle::isKeyboardNavigationActive());

        // Mouse click disables keyboard navigation active state
        auto *button = window.findChild<QToolButton *>("backupsButton");
        QVERIFY(button);
        QMouseEvent mousePress(QEvent::MouseButtonPress, QPointF(5, 5), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        qApp->sendEvent(button, &mousePress);
        QVERIFY(!UiStyle::isKeyboardNavigationActive());

        // Switching to settings and returning to application retains clean mouse focus state
        auto *settingsButton = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(settingsButton);
        settingsButton->click();
        auto *returnButton = window.findChild<QToolButton *>("returnApplicationButton");
        QVERIFY(returnButton);
        returnButton->click();
        QVERIFY(!UiStyle::isKeyboardNavigationActive());
    }
    void localBackupRestoreAndDiff()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/源 file.txt";
        const auto id = encoded(source);
        writeFile(source, "first\n");
        QVERIFY(service.addLocal(source).success);
        const auto first = head(service, id);
        writeFile(source, "second\nextra\n");
        QVERIFY(service.backup(id).success);
        settle(service);
        QVector<Revision> revisions;
        QVERIFY(service.history(id, revisions).success);
        QCOMPARE(revisions.size(), 2);
        BackupStats stats;
        QVERIFY(service.statistics(id, stats).success);
        QCOMPARE(stats.fileCount, 1);
        QCOMPARE(stats.versionCount, 2);
        DiffData data;
        QVERIFY(service.diff(id, first, data).success);
        QVERIFY(data.oldCommit.isEmpty());
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
        // Complete commit IDs are used for editing; older revisions remain immutable.
        QVERIFY(service.editMessage(id, revisions.first().hash, "edited").success);
        QVERIFY(!service.editMessage(id, first, "").success);
        QVERIFY(service.removeBackup(id).success);
        QVERIFY(QFileInfo::exists(source));
        QVERIFY(!service.contains(id));
        QVERIFY(!service.restore(id, first).success);
    }
    void directoryBinaryAndRemoteImport()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/source";
        const auto id = encoded(source);
        writeFile(source + "/.hidden", "hidden");
        writeFile(source + "/nested/item.txt", "old");
        writeFile(source + "/image.bin", QByteArray("\0first", 6));
        QVERIFY(service.addLocal(source).success);
        const auto first = head(service, id);
        writeFile(source + "/image.bin", QByteArray("\0second", 7));
        QVERIFY(service.backup(id).success);
        settle(service);
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
        QCOMPARE(prepared.value.suggestedPath, QString("source"));
        QCOMPARE(service.trackedItems().size(), 1);
        const auto imported = dir.path() + "/imported";
        QVERIFY(service.finishImport(prepared.value.sessionId, prepared.value.suggestedPath, imported).success);
        QCOMPARE(readFile(imported + "/nested/item.txt"), QByteArray("old"));
        QCOMPARE(service.trackedItems().size(), 2);
        const auto cancelled = service.prepareImport(remote);
        QVERIFY(cancelled.result.success);
        QVERIFY(service.cancelImport(cancelled.value.sessionId).success);
        QVERIFY(!service.finishImport(cancelled.value.sessionId, ".", dir.path() + "/cancelled").success);
        QVERIFY(service.removeRemote(id).success);
        QVERIFY(service.statistics(id, stats).success);
        QVERIFY(stats.remoteUrl.isEmpty());
    }
    void monitorSurvivesPageRefresh()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/file.txt";
        writeFile(source, "initial");
        QVERIFY(service.addLocal(source).success);
        BackupMonitor monitor(&service);
        monitor.reconcile();
        QCOMPARE(monitor.trackedCount(), 1);
        {
            HomePage page(&service);
            for (int i = 0; i < 10; ++i)
                page.refresh();
            settle(service);
        }
        writeFile(source, "modified and larger");
        monitor.reconcile();
        monitor.scanNow();
        settle(service);
        settle(service);
        QCOMPARE(runGit(service.repoPath(encoded(source)), {"rev-list", "--count", "HEAD"}).output.trimmed(), QString("2"));
        monitor.scanNow();
        settle(service);
        settle(service);
        QCOMPARE(runGit(service.repoPath(encoded(source)), {"rev-list", "--count", "HEAD"}).output.trimmed(), QString("2"));
        QVERIFY(service.removeBackup(encoded(source)).success);
        QCOMPARE(monitor.trackedCount(), 0);
    }
    void defaultFileSelectionKeepsBuildAndGitIgnoreSemantics()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/project";
        const auto id = encoded(source);
        writeFile(source + "/visible.txt", "one");
        writeFile(source + "/.hidden", "hidden");
        writeFile(source + "/build/output.bin", "initial build");
        writeFile(source + "/ignored.log", "ignored");
        writeFile(source + "/.gitignore", "*.log\n");
        QVERIFY(service.addLocal(source).success);
        const auto initial = head(service, id);
        const auto repo = service.repoPath(id);
        QVERIFY(runGit(repo, {"cat-file", "-e", "HEAD:project/build/output.bin"}).success());
        QVERIFY(runGit(repo, {"cat-file", "-e", "HEAD:project/.hidden"}).success());
        QVERIFY(!runGit(repo, {"cat-file", "-e", "HEAD:project/ignored.log"}).success());
        QCOMPARE(readFile(repo + "/project/ignored.log"), QByteArray("ignored"));
        BackupMonitor monitor(&service);
        monitor.reconcile();
        writeFile(source + "/build/output.bin", "changed build only");
        monitor.scanNow();
        settle(service);
        QCOMPARE(head(service, id), initial);
        QCOMPARE(readFile(repo + "/project/build/output.bin"), QByteArray("initial build"));
        writeFile(source + "/visible.txt", "two, normal change");
        monitor.scanNow();
        settle(service);
        QVERIFY(head(service, id) != initial);
        QCOMPARE(readFile(repo + "/project/build/output.bin"), QByteArray("changed build only"));
        QCOMPARE(runGit(repo, {"show", "HEAD:project/build/output.bin"}).output, QString("changed build only"));
        QVERIFY(!runGit(repo, {"cat-file", "-e", "HEAD:project/ignored.log"}).success());
    }
    void automaticAiMessagesAndDeletedContext()
    {
        TestDirectory dir;
        const auto paths = pathsIn(dir);
        const auto source = dir.path() + "/file.txt";
        const auto id = encoded(source);
        FakeAi gateway;
        SettingsService settings(paths, &gateway);
        settings.saveField("ApiKey", "fixture-only");
        settings.saveField("Model", "test-model");
        {
            QSettings ini(paths.settingsFile, QSettings::IniFormat);
            ini.setValue("AI/Enabled", true);
        }
        BackupDependencies dependencies;
        dependencies.aiTimeoutMs = 150;
        auto service = std::make_unique<TestBackupService>(paths, nullptr, &gateway, dependencies);
        writeFile(source, "one");
        QVERIFY(service->addLocal(source).success);
        writeFile(source, "two");
        bool finished = false;
        OperationResult result;
        service->backup(id, this, [&](const OperationResult &r)
                        { result = r; finished = true; });
        QTRY_COMPARE(gateway.messages.size(), 1);
        QVERIFY(!finished);
        gateway.messages[0]("AI fixture message", {});
        QTRY_VERIFY(finished);
        QVERIFY2(result.success, qPrintable(result.message));
        QVector<Revision> revisions;
        QVERIFY(service->history(id, revisions).success);
        QCOMPARE(revisions.first().message, QString("AI fixture message"));

        writeFile(source, "three");
        QVERIFY(service->backup(id).success); // No reply: use the timeout fallback.
        QVERIFY(service->history(id, revisions).success);
        QVERIFY(revisions.first().message.startsWith("Auto backup - "));
        const auto fallbackHead = head(*service, id);
        gateway.messages[1]("late message", {});
        settle(*service);
        QCOMPARE(head(*service, id), fallbackHead);

        writeFile(source, "four");
        finished = false;
        const auto task = service->backup(id, this, [&](const OperationResult &r)
                                          { result = r; finished = true; });
        QTRY_COMPARE(gateway.messages.size(), 3);
        service->cancel(task);
        QTRY_VERIFY(finished);
        QVERIFY(!result.success);
        QCOMPARE(head(*service, id), fallbackHead);
        QCOMPARE(readFile(service->repoPath(id) + "/file.txt"), QByteArray("three"));
        gateway.messages[2]("cancelled request", {});

        service->backup(id, this);
        QTRY_COMPARE(gateway.messages.size(), 4);
        service.reset(); // The worker rolls back while the asynchronous AI request is outstanding.
        gateway.messages[3]("reply after service destruction", {});
        TestBackupService reopened(paths);
        QCOMPARE(head(reopened, id), fallbackHead);
        QCOMPARE(reopened.syncState(id), BackupSyncState::Tracking);
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
        navigation.go({PageId::Diff, "rebuilt-id", "old-commit"});
        navigation.invalidateRevisions("rebuilt-id");
        QCOMPARE(navigation.current(), (Route{PageId::History, "rebuilt-id"}));
    }
    void duplicateNamesHaveIndependentRoutes()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto first = dir.path() + "/a/same.txt";
        const auto second = dir.path() + "/b/same.txt";
        writeFile(first, "first");
        writeFile(second, "second");
        QVERIFY(service.addLocal(first).success);
        QVERIFY(service.addLocal(second).success);
        HomePage page(&service);
        page.setAttribute(Qt::WA_DontShowOnScreen);
        page.resize(640, 480);
        page.show();
        QSignalSpy routes(&page, &HomePage::navigate);
        auto *view = page.findChild<QListView *>("backupList");
        QCOMPARE(view->model()->rowCount(), 2);
        QTest::qWait(20);
        for (int row = 0; row < view->model()->rowCount(); ++row)
        {
            const auto index = view->model()->index(row, 0);
            QTest::mouseClick(view->viewport(), Qt::LeftButton, {}, view->visualRect(index).center());
            QCOMPARE(qvariant_cast<Route>(routes.last().first()).backupId, index.data(BackupListModel::IdRole).toString());
            QCOMPARE(qvariant_cast<Route>(routes.last().first()).page, PageId::History);
        }
        auto *source = page.findChild<BackupListModel *>();
        auto items = service.trackedItems();
        items[0].name = "Changed caption";
        source->setItems(items);
        const auto renamed = view->model()->index(0, 0);
        QTest::mouseClick(view->viewport(), Qt::LeftButton, {}, view->visualRect(renamed).center());
        QCOMPARE(qvariant_cast<Route>(routes.last().first()).backupId, renamed.data(BackupListModel::IdRole).toString());

        FakeAi gateway;
        SettingsService settings(pathsIn(dir), &gateway);
        MainWindow window(&service, &settings, &gateway, m_theme, false);
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.show();
        auto *sidebar = window.findChild<QListView *>("sidebarList");
        auto *filter = window.findChild<QLineEdit *>("sidebarFilter");
        const auto index = sidebar->model()->index(1, 0);
        const auto id = index.data(BackupListModel::IdRole).toString();
        window.navigate({PageId::History, id});
        settle(service);
        QCOMPARE(sidebar->currentIndex().data(BackupListModel::IdRole).toString(), id);
        auto *pages = window.findChild<QStackedWidget *>("pages");
        QCOMPARE(pages->currentWidget(), window.findChild<HomePageBackupPage *>());
        filter->setText("no matching object");
        QCOMPARE(sidebar->model()->rowCount(), 0);
        QCOMPARE(pages->currentWidget(), window.findChild<HomePageBackupPage *>());
        filter->clear();
        QCOMPARE(sidebar->currentIndex().data(BackupListModel::IdRole).toString(), id);
        // The page and sidebar have independent filters over the same model.
        window.findChild<HomePage *>()->findChild<QLineEdit *>("filter")->setText("/a/");
        QCOMPARE(sidebar->model()->rowCount(), 2);
    }
    void settingsRejectStaleResponses()
    {
        TestDirectory dir;
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
        SettingPage general(&settings, m_theme);
        SettingPageAiPage ai(&settings);
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
    void settingsNavigationPreservesApplication()
    {
        TestDirectory dir;
        const auto paths = pathsIn(dir);
        TestBackupService service(paths);
        const auto source = dir.path() + "/设置导航.txt";
        const auto id = encoded(source);
        writeFile(source, "first\n");
        QVERIFY(service.addLocal(source).success);
        writeFile(source, "second\n");
        QVERIFY(service.backup(id).success);
        settle(service);
        FakeAi gateway;
        SettingsService settings(paths, &gateway);
        settings.saveField("ApiKey", "fixture-only");
        MainWindow window(&service, &settings, &gateway, m_theme, false);
        window.show();
        window.activateWindow();
        QTRY_VERIFY(window.isActiveWindow());

        auto *pages = window.findChild<QStackedWidget *>("pages");
        auto *sidebar = window.findChild<QWidget *>("sidebar");
        auto *sidebarStack = window.findChild<QStackedWidget *>("sidebarStack");
        auto *sidebarList = window.findChild<QListView *>("sidebarList");
        auto *backupFilter = window.findChild<QLineEdit *>("sidebarFilter");
        auto *search = window.findChild<QLineEdit *>("settingsSearch");
        auto *empty = window.findChild<QWidget *>("settingsEmptyState");
        auto *returnButton = window.findChild<QToolButton *>("returnApplicationButton");
        auto *generalButton = window.findChild<QToolButton *>("generalTab");
        auto *aiButton = window.findChild<QToolButton *>("aiTab");
        auto *aboutButton = window.findChild<QToolButton *>("aboutTab");
        auto *history = window.findChild<HomePageBackupPage *>();
        auto *ai = window.findChild<SettingPageAiPage *>();
        auto *table = history->findChild<QTableView *>("table");
        auto *spinner = ai->findChild<oclero::qlementine::LoadingSpinner *>("modelsSpinner");
        const auto before = readFile(paths.settingsFile);
        QSignalSpy changes(&settings, &SettingsService::changed);

        window.navigate({PageId::History, id});
        settle(service);
        table->setCurrentIndex(table->model()->index(1, 0));
        const auto selected = table->currentIndex().data(Qt::UserRole + 1).toString();
        QVERIFY(!selected.isEmpty());
        backupFilter->setText("设置导航");
        window.findChild<QToolButton *>("collapseButton")->click();
        QTRY_VERIFY(!sidebar->isVisible());
        QTest::keySequence(&window, QKeySequence("Ctrl+,"));
        QTRY_COMPARE(pages->currentWidget(), window.findChild<SettingPage *>());
        QCOMPARE(sidebarStack->currentWidget()->objectName(), QString("settingsSidebar"));
        QVERIFY(sidebar->isVisible());
        QVERIFY(sidebar->width() >= 200);
        QVERIFY(!window.findChild<QWidget *>("header")->isVisible());
        QVERIFY(!window.findChild<QWidget *>("tabs")->isVisible());
        QVERIFY(!sidebarList->isVisible());
        QVERIFY(window.findChild<QToolButton *>("appMenuButton")->isVisible());
        QTest::keySequence(&window, QKeySequence("Ctrl+B"));
        QVERIFY(sidebar->isVisible());
        QTest::keySequence(&window, QKeySequence::Find);
        QTRY_VERIFY(search->hasFocus());

        search->setText("右键菜单快捷入口");
        QVERIFY(generalButton->isVisible());
        search->setText("开机自动启动");
        QVERIFY(generalButton->isVisible());
        search->setText("自动生成提交说明");
        QCOMPARE(pages->currentWidget(), ai);
        search->setText("Api KEY");
        QCOMPARE(pages->currentWidget(), ai);
        QVERIFY(aiButton->isVisible());
        QVERIFY(!generalButton->isVisible());
        QVERIFY(!aboutButton->isVisible());
        settings.fetchModels();
        QVERIFY(spinner->spinning());
        search->setText("没有这样的设置");
        QVERIFY(empty->isVisible());
        QVERIFY(!pages->isVisible());
        QVERIFY(!spinner->spinning());
        QVERIFY(settings.isFetching());
        window.findChild<QPushButton *>("clearSettingsSearchButton")->click();
        QVERIFY(!empty->isVisible());
        QVERIFY(pages->isVisible());
        QCOMPARE(pages->currentWidget(), ai);
        QVERIFY(spinner->spinning());
        QVERIFY(generalButton->isVisible() && aiButton->isVisible() && aboutButton->isVisible());
        gateway.models.last()({}, "测试服务暂时不可用");
        QVERIFY(!spinner->spinning());
        window.findChild<NotificationBar *>()->dismiss();

        search->setText("关于 版本");
        QCOMPARE(pages->currentWidget()->objectName(), QString("AboutPage"));
        QTest::keyClick(&window, Qt::Key_Escape);
        QVERIFY(search->text().isEmpty());
        QCOMPARE(pages->currentWidget()->objectName(), QString("AboutPage"));
        QTest::keyClick(&window, Qt::Key_Escape);
        QTRY_COMPARE(pages->currentWidget(), history);
        QVERIFY(!sidebar->isVisible());
        QVERIFY(window.findChild<QWidget *>("header")->isVisible());
        QCOMPARE(sidebarStack->currentWidget()->objectName(), QString("applicationSidebar"));
        QCOMPARE(table->currentIndex().data(Qt::UserRole + 1).toString(), selected);
        QCOMPARE(backupFilter->text(), QString("设置导航"));
        QCOMPARE(sidebarList->currentIndex().data(BackupListModel::IdRole).toString(), id);

        QTest::keySequence(&window, QKeySequence("Ctrl+,"));
        QCOMPARE(pages->currentWidget()->objectName(), QString("AboutPage"));
        generalButton->click();
        search->setText("MODEL");
        QCOMPARE(pages->currentWidget(), ai);
        QTest::keySequence(&window, QKeySequence::Back);
        QCOMPARE(pages->currentWidget(), window.findChild<SettingPage *>());
        QVERIFY(search->text().isEmpty());
        QTest::keySequence(&window, QKeySequence::Forward);
        QCOMPARE(pages->currentWidget(), ai);
        search->setText("没有匹配项");
        window.navigate({PageId::About});
        settle(service);
        QVERIFY(search->text().isEmpty());
        QVERIFY(pages->isVisible());
        returnButton->click();
        QCOMPARE(pages->currentWidget(), history);
        QCOMPARE(changes.count(), 0);
        QCOMPARE(readFile(paths.settingsFile), before);

        // Returning to the application must respect repository and object invalidation.
        window.findChild<QToolButton *>("collapseButton")->click();
        window.navigate({PageId::Diff, id, head(service, id)});
        window.findChild<QToolButton *>("settingsButton")->click();
        QVERIFY(!returnButton->hasFocus());
        QVERIFY(service.rebuild(id).success);
        returnButton->click();
        QVERIFY(!window.findChild<QToolButton *>("backupsButton")->hasFocus());
        QCOMPARE(pages->currentWidget(), history);
        QVERIFY(sidebar->isVisible());
        window.navigate({PageId::Dashboard, id});
        settle(service);
        window.findChild<QToolButton *>("settingsButton")->click();
        QVERIFY(!returnButton->hasFocus());
        QVERIFY(service.removeBackup(id).success);
        returnButton->click();
        QVERIFY(!window.findChild<QToolButton *>("backupsButton")->hasFocus());
        QCOMPARE(pages->currentWidget(), window.findChild<HomePage *>());
        QVERIFY(!sidebarList->currentIndex().isValid());
    }
    void stableModelsAndStaleDiff()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/file.txt";
        writeFile(source, "one");
        QVERIFY(service.addLocal(source).success);
        const auto id = encoded(source);
        const auto commit = head(service, id);
        FakeAi gateway;
        SettingsService settings(pathsIn(dir), &gateway);
        settings.saveField("ApiKey", "fixture");
        settings.saveField("Model", "fixture-model");
        HomePageBackupPage history(&service);
        history.setBackup(id);
        settle(service);
        auto *table = history.findChild<QTableView *>("table");
        auto *model = table->model();
        for (int i = 0; i < 20; ++i)
            history.setBackup(id);
        settle(service);
        QCOMPARE(table->model(), model);
        HomePageDiffPage diff(&service, &settings, &gateway);
        diff.setRevision(id, commit);
        settle(service);
        auto *analyze = diff.findChild<QAction *>("analyzeAction");
        analyze->trigger();
        settle(service);
        QCOMPARE(gateway.summaries.size(), 1);
        diff.deactivate();
        gateway.summaries[0]("stale-result", {});
        QVERIFY(!diff.findChild<QPlainTextEdit *>("analysis")->toPlainText().contains("stale-result"));
        diff.setRevision(id, commit);
        settle(service);
        const auto originalDiff = diff.findChild<QPlainTextEdit *>("content")->toPlainText();
        analyze->trigger();
        settle(service);
        gateway.summaries[1]("current-result", {});
        QCOMPARE(diff.findChild<QPlainTextEdit *>("analysis")->toPlainText(), QString("current-result"));
        QCOMPARE(diff.findChild<QPlainTextEdit *>("content")->toPlainText(), originalDiff);
        analyze->trigger();
        settle(service);
        QCOMPARE(gateway.summaries.size(), 3);
        QVERIFY(service.removeBackup(id).success);
        QVERIFY(service.addLocal(source).success);
        gateway.summaries[2]("deleted-context-result", {});
        QVERIFY(!diff.findChild<QPlainTextEdit *>("analysis")->toPlainText().contains("deleted-context-result"));
        HomePageDashboardPage dashboard(&service);
        dashboard.setBackup(id);
        settle(service);
        dashboard.findChild<QToolButton *>("expandButton")->click();
        dashboard.findChild<QToolButton *>("expandButton")->click();
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
        settle(service);
        QCOMPARE(url->text(), QString("https://example.test/second.git"));
        dashboard.findChild<QToolButton *>("expandButton")->click();
        dashboard.findChild<QToolButton *>("expandButton")->click();
        QCOMPARE(runGit(service.repoPath(otherId), {"remote", "get-url", "origin"}).output.trimmed(), QString("https://example.test/second.git"));
        auto *toggle = dashboard.findChild<oclero::qlementine::Switch *>("remoteSwitch");
        QTest::keyClick(toggle, Qt::Key_Space);
        QVERIFY(!toggle->isChecked());
        settle(service);
        QVERIFY(!runGit(service.repoPath(otherId), {"remote", "get-url", "origin"}).success());
    }
    void historyRowActionsUseClickedVersion()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/行内操作.txt";
        const auto id = encoded(source);
        writeFile(source, "first\n");
        QVERIFY(service.addLocal(source).success);
        writeFile(source, "second\n");
        QVERIFY(service.backup(id).success);
        settle(service);
        HomePageBackupPage page(&service);
        page.resize(820, 580);
        page.setBackup(id);
        settle(service);
        page.show();
        page.activateWindow();
        QTRY_VERIFY(page.isActiveWindow());
        auto *table = page.findChild<QTableView *>("table");
        const auto first = table->model()->index(1, 0).data(Qt::UserRole + 1).toString();
        const auto latest = table->model()->index(0, 0).data(Qt::UserRole + 1).toString();
        const auto originalHead = head(service, id);
        const auto toolbar = page.toolbarActions();
        QVERIFY(toolbar.contains(page.findChild<QAction *>("refreshHistoryAction")));
        for (const auto *name : {"compareAction", "previewAction", "restoreAction", "editMessageAction"})
            QVERIFY(!toolbar.contains(page.findChild<QAction *>(name)));
        QSignalSpy routes(&page, &HomePageBackupPage::navigate);
        PreviewUrls preview;

        // A row action operates on the hovered row without first selecting it elsewhere.
        table->selectRow(0);
        const auto previewPoint = historyActionPoint(table, 1, 0);
        QTest::mouseMove(table->viewport(), previewPoint);
        QTest::mouseClick(table->viewport(), Qt::LeftButton, {}, previewPoint);
        QTRY_COMPARE(preview.urls.size(), 1);
        QCOMPARE(routes.count(), 0);
        const auto previewPath = preview.urls.first().toLocalFile();
        QVERIFY(previewPath.startsWith(QDir(QDir::tempPath()).canonicalPath() + "/ZcVersionBoxPreview-"));
        QVERIFY(previewPath.endsWith("/contents"));
        QCOMPARE(readFile(previewPath + "/行内操作.txt"), QByteArray("first\n"));
        QCOMPARE(readFile(source), QByteArray("second\n"));
        QCOMPARE(head(service, id), originalHead);
        const auto comparePoint = historyActionPoint(table, 0, 1);
        QTest::mouseClick(table->viewport(), Qt::LeftButton, {}, comparePoint);
        QCOMPARE(routes.count(), 1);
        QCOMPARE(qvariant_cast<Route>(routes.last().first()).commit, latest);

        // Releasing over another icon or after a model reset must cancel the press.
        QTest::mousePress(table->viewport(), Qt::LeftButton, {}, historyActionPoint(table, 1, 1));
        QTest::mouseRelease(table->viewport(), Qt::LeftButton, {}, previewPoint);
        QCOMPARE(routes.count(), 1);
        QTRY_COMPARE(preview.urls.size(), 1);
        QTest::mousePress(table->viewport(), Qt::LeftButton, {}, historyActionPoint(table, 1, 1));
        page.refresh();
        settle(service);
        QTest::mouseRelease(table->viewport(), Qt::LeftButton, {}, historyActionPoint(table, 1, 1));
        QCOMPARE(routes.count(), 1);
        QTest::mousePress(table->viewport(), Qt::LeftButton, {}, historyActionPoint(table, 1, 1));
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(table->viewport(), &leave);
        QTest::mouseRelease(table->viewport(), Qt::LeftButton, {}, historyActionPoint(table, 1, 1));
        QCOMPARE(routes.count(), 1);
        QTest::mouseDClick(table->viewport(), Qt::LeftButton, {}, historyActionPoint(table, 1, 0));
        QTest::mouseRelease(table->viewport(), Qt::LeftButton, {}, historyActionPoint(table, 1, 0));
        QCOMPARE(routes.count(), 1);

        const auto rowPoint = table->visualRect(table->model()->index(1, 0)).center();
        QTest::mouseClick(table->viewport(), Qt::LeftButton, {}, rowPoint);
        QTest::mouseDClick(table->viewport(), Qt::LeftButton, {}, rowPoint);
        QCOMPARE(routes.count(), 2);
        QCOMPARE(qvariant_cast<Route>(routes.last().first()).commit, first);

        // A right click on a row opens the revision menu and does not activate/navigate.
        QTest::mouseClick(table->viewport(), Qt::RightButton, {}, rowPoint);
        QCOMPARE(routes.count(), 2);
        QContextMenuEvent rightClick(QContextMenuEvent::Mouse, rowPoint, table->viewport()->mapToGlobal(rowPoint));
        QApplication::sendEvent(table->viewport(), &rightClick);
        QTRY_VERIFY(QApplication::activePopupWidget());
        auto *contextMenu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        QVERIFY(contextMenu);
        QCOMPARE(contextMenu->objectName(), QString("revisionMenu"));
        contextMenu->close();
        QTRY_VERIFY(!QApplication::activePopupWidget());
        QCOMPARE(routes.count(), 2);
        table->setFocus();
        QTest::keyClick(table, Qt::Key_Return);
        QCOMPARE(routes.count(), 3);
        QCOMPARE(qvariant_cast<Route>(routes.last().first()).commit, first);
        QTest::keySequence(table, QKeySequence("Alt+P"));
        QTRY_COMPARE(preview.urls.size(), 2);
        QCOMPARE(readFile(preview.urls.last().toLocalFile() + "/行内操作.txt"), QByteArray("first\n"));

        QTest::keySequence(table, QKeySequence("Shift+F10"));
        QTRY_VERIFY(QApplication::activePopupWidget());
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        QVERIFY(menu);
        QCOMPARE(menu->objectName(), QString("revisionMenu"));
        QVERIFY(menu->findChild<QAction *>("restoreActionMenu"));
        QVERIFY(menu->findChild<QAction *>("editMessageActionMenu"));
        QTest::keyPress(menu, Qt::Key_Escape);
        QTRY_VERIFY(!QApplication::activePopupWidget());
        QTest::keyClick(table, Qt::Key_Menu);
        QTRY_VERIFY(QApplication::activePopupWidget());
        menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        QVERIFY(menu);
        menu->setActiveAction(menu->findChild<QAction *>("editMessageActionMenu"));
        QTest::keyPress(menu, Qt::Key_Return);
        QPointer<QLineEdit> editor = table->findChild<QLineEdit *>();
        QVERIFY(editor);
        QCOMPARE(table->currentIndex().data(Qt::UserRole + 1).toString(), first);
        QTest::keyClick(editor, Qt::Key_Escape);
        QTRY_VERIFY(!editor);
        QTest::keyClick(table, Qt::Key_F2);
        editor = table->findChild<QLineEdit *>();
        QVERIFY(editor);
        QTest::keyClick(editor, Qt::Key_Escape);
        QTRY_VERIFY(!editor);
        const QPoint blank(12, table->viewport()->height() - 8);
        QVERIFY(!table->indexAt(blank).isValid());
        QContextMenuEvent context(QContextMenuEvent::Mouse, blank, table->viewport()->mapToGlobal(blank));
        QApplication::sendEvent(table->viewport(), &context);
        QVERIFY(!QApplication::activePopupWidget());
        page.resize(520, 400);
        QTest::qWait(20);
        QVERIFY(table->isColumnHidden(2));
        QCOMPARE(table->horizontalScrollBar()->maximum(), 0);
        QTest::mouseClick(table->viewport(), Qt::LeftButton, {}, historyActionPoint(table, 0, 1));
        QCOMPARE(routes.count(), 4);
        QCOMPARE(qvariant_cast<Route>(routes.last().first()).commit, latest);
        QFile::setPermissions(previewPath + "/行内操作.txt", QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        QVERIFY(QDir(previewPath).removeRecursively());
    }
    void pullStateActionsRespectConfirmation()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/source.txt";
        writeFile(source, "initial\n");
        QVERIFY(service.addLocal(source).success);
        const auto id = service.idForSource(source);
        const auto remote = dir.path() + "/remote.git", writer = dir.path() + "/writer";
        QVERIFY(runGit({}, {"init", "--bare", remote}).success());
        QVERIFY(service.setRemote(id, remote).success);
        QVERIFY(service.synchronize(id, true).success);
        QVERIFY(runGit({}, {"clone", remote, writer}).success());
        QVERIFY(runGit(writer, {"config", "user.name", "Fixture"}).success());
        QVERIFY(runGit(writer, {"config", "user.email", "fixture@example.test"}).success());
        writeFile(writer + "/source.txt", "pulled\n");
        QVERIFY(runGit(writer, {"add", "--all"}).success());
        QVERIFY(runGit(writer, {"commit", "-m", "remote"}).success());
        QVERIFY(runGit(writer, {"push"}).success());
        QVERIFY(service.synchronize(id, false).success);
        const auto pulled = head(service, id);
        HomePageDashboardPage page(&service);
        page.setAttribute(Qt::WA_DontShowOnScreen);
        page.resize(700, 600);
        page.setBackup(id);
        page.show();
        settle(service);
        auto *apply = page.findChild<QPushButton *>("applyPullButton");
        auto *keep = page.findChild<QPushButton *>("keepSourceButton");
        QVERIFY(apply->isVisible());
        QVERIFY(keep->isVisible());
        QVERIFY(!page.findChild<QPushButton *>("pullButton")->isEnabled());
        QVERIFY(page.findChild<QPushButton *>("pushButton")->isEnabled());
        QVERIFY(page.findChild<QLabel *>("syncDetailLabel")->text().contains("自动备份已暂停"));
        const auto respond = [&](QPushButton *button, const auto &answer)
        {
            bool shown = false;
            QTimer timer;
            timer.setInterval(10);
            connect(&timer, &QTimer::timeout, &timer, [&]
                    {
                if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
                {
                    timer.stop(); shown = true; answer(dialog);
                } });
            timer.start();
            button->click();
            if (!QTest::qWaitFor([&]
                                 { return shown; }, 5000))
                return false;
            settle(service);
            return true;
        };
        QVERIFY(respond(apply, [](QDialog *dialog)
                        { dialog->reject(); }));
        QCOMPARE(service.syncState(id), BackupSyncState::RemotePending);
        QCOMPARE(readFile(source), QByteArray("initial\n"));
        QVERIFY(respond(apply, [&](QDialog *dialog)
                        {
            writeFile(source, "edited during confirmation\n");
            dialog->accept(); }));
        QCOMPARE(service.syncState(id), BackupSyncState::RemotePending);
        QCOMPARE(readFile(source), QByteArray("edited during confirmation\n"));
        QVERIFY(respond(keep, [](QDialog *dialog)
                        { dialog->accept(); }));
        QCOMPARE(service.syncState(id), BackupSyncState::Tracking);
        QVERIFY(!apply->isVisible());
        QVERIFY(!keep->isVisible());
        QVERIFY(page.findChild<QPushButton *>("pullButton")->isEnabled());
        QCOMPARE(runGit(service.repoPath(id), {"rev-parse", "HEAD^"}).output.trimmed(), pulled);

        QVERIFY(service.synchronize(id, true).success);
        QVERIFY(runGit(writer, {"pull", "--ff-only"}).success());
        writeFile(writer + "/source.txt", "new remote version\n");
        QVERIFY(runGit(writer, {"add", "--all"}).success());
        QVERIFY(runGit(writer, {"commit", "-m", "second remote"}).success());
        QVERIFY(runGit(writer, {"push"}).success());
        QVERIFY(service.synchronize(id, false).success);
        settle(service);
        const auto latest = head(service, id);
        QVERIFY(respond(apply, [](QDialog *dialog)
                        { dialog->accept(); }));
        QCOMPARE(service.syncState(id), BackupSyncState::Tracking);
        QCOMPARE(readFile(source), QByteArray("new remote version\n"));
        QCOMPARE(head(service, id), latest);
    }
    void historyMenusAndRestoreKeepVersionContext()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/restore-context.txt";
        const auto other = dir.path() + "/other.txt";
        const auto id = encoded(source);
        const auto otherId = encoded(other);
        writeFile(source, "first\n");
        QVERIFY(service.addLocal(source).success);
        writeFile(source, "second\n");
        QVERIFY(service.backup(id).success);
        settle(service);
        writeFile(other, "other\n");
        QVERIFY(service.addLocal(other).success);
        HomePageBackupPage page(&service);
        page.resize(820, 580);
        page.setBackup(id);
        settle(service);
        page.show();
        auto *table = page.findChild<QTableView *>("table");
        const auto first = table->model()->index(1, 0).data(Qt::UserRole + 1).toString();
        const auto openMenu = [&](int row)
        {
            QTest::mouseClick(table->viewport(), Qt::LeftButton, {}, historyActionPoint(table, row, 2));
            if (!QTest::qWaitFor([]
                                 { return QApplication::activePopupWidget() != nullptr; }, 1000))
                return static_cast<QMenu *>(nullptr);
            return qobject_cast<QMenu *>(QApplication::activePopupWidget());
        };
        const auto confirmRestore = [&service](QMenu *menu, const auto &answer)
        {
            bool shown = false;
            QTimer responder;
            responder.setInterval(10);
            QObject::connect(&responder, &QTimer::timeout, &responder, [&]
                             {
                if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
                {
                    responder.stop();
                    shown = true;
                    answer(dialog);
                } });
            responder.start();
            menu->setActiveAction(menu->findChild<QAction *>("restoreActionMenu"));
            // The menu can be deleted while the confirmation runs its nested event loop.
            QTest::keyPress(menu, Qt::Key_Return);
            if (!QTest::qWaitFor([&]
                                 { return shown; }, 5000))
                return false;
            settle(service);
            return shown;
        };
        QSignalSpy routes(&page, &HomePageBackupPage::navigate);
        QPointer<QMenu> menu = openMenu(1);
        QVERIFY(menu);
        table->selectRow(0);
        writeFile(source, "third\n");
        QVERIFY(service.backup(id).success);
        settle(service);
        QCOMPARE(table->model()->rowCount(), 3);
        menu->setActiveAction(menu->findChild<QAction *>("compareActionMenu"));
        QTest::keyPress(menu, Qt::Key_Return);
        QCOMPARE(routes.count(), 1);
        QCOMPARE(qvariant_cast<Route>(routes.first().first()).commit, first);

        menu = openMenu(2);
        QVERIFY(menu);
        QPointer<QAction> staleAction = menu->findChild<QAction *>("compareActionMenu");
        page.setBackup(otherId);
        settle(service);
        // Settling the asynchronous refresh can process the menu's deferred deletion.
        QVERIFY(!menu || !menu->isVisible());
        if (staleAction)
            staleAction->trigger();
        QCOMPARE(routes.count(), 1);
        page.setBackup(id);
        settle(service);

        bool cancelDefault = false;
        menu = openMenu(2);
        QVERIFY(menu);
        QVERIFY(confirmRestore(menu, [&](QDialog *dialog)
                               {
            cancelDefault = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel)->isDefault();
            dialog->reject(); }));
        QVERIFY(cancelDefault);
        QCOMPARE(readFile(source), QByteArray("third\n"));

        const auto originalHead = head(service, id);
        menu = openMenu(2);
        QVERIFY(menu);
        QVERIFY(confirmRestore(menu, [&](QDialog *dialog)
                               {
            table->selectRow(0);
            dialog->accept(); }));
        QCOMPARE(readFile(source), QByteArray("first\n"));
        QCOMPARE(head(service, id), originalHead);

        writeFile(source, "uncommitted\n");
        menu = openMenu(2);
        QVERIFY(menu);
        QVERIFY(confirmRestore(menu, [&](QDialog *dialog)
                               {
            page.setBackup(otherId);
            settle(service);
            dialog->accept(); }));
        QCOMPARE(readFile(source), QByteArray("uncommitted\n"));
        QCOMPARE(readFile(other), QByteArray("other\n"));

        page.setBackup(id);
        settle(service);
        menu = openMenu(2);
        QVERIFY(menu);
        QVERIFY(confirmRestore(menu, [&](QDialog *dialog)
                               {
            if (!service.rebuild(id).success) qFatal("Cannot rebuild restore fixture");
            dialog->accept(); }));
        QCOMPARE(readFile(source), QByteArray("uncommitted\n"));
        QCOMPARE(table->model()->rowCount(), 1);

        menu = openMenu(0);
        QVERIFY(menu);
        QVERIFY(confirmRestore(menu, [&](QDialog *dialog)
                               {
            if (!service.removeBackup(id).success || !service.addLocal(source).success) qFatal("Cannot replace restore fixture");
            dialog->accept(); }));
        QCOMPARE(readFile(source), QByteArray("uncommitted\n"));
        QVERIFY(service.contains(id));
    }
    void manyRevisionsAndEditFeedback()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/file.txt";
        const auto id = encoded(source);
        writeFile(source, "initial");
        QVERIFY(service.addLocal(source).success);
        const auto initial = head(service, id).toUtf8();
        HomePageBackupPage page(&service);
        page.setAttribute(Qt::WA_DontShowOnScreen);
        page.resize(820, 580);
        page.setBackup(id);
        settle(service);
        page.show();
        QTest::qWait(20);
        auto *table = page.findChild<QTableView *>("table");
        const auto widgetCount = table->findChildren<QWidget *>().size();
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
        page.refresh();
        settle(service);
        auto *model = qobject_cast<QStandardItemModel *>(table->model());
        QCOMPARE(model->rowCount(), 1001);
        QCOMPARE(table->findChildren<QWidget *>().size(), widgetCount);
        QCoreApplication::processEvents();
        const auto last = model->index(1000, 0);
        table->scrollTo(last, QAbstractItemView::PositionAtBottom);
        QTest::qWait(20);
        QSignalSpy routes(&page, &HomePageBackupPage::navigate);
        QTest::mouseClick(table->viewport(), Qt::LeftButton, {}, historyActionPoint(table, 1000, 1));
        QCOMPARE(routes.count(), 1);
        QCOMPARE(qvariant_cast<Route>(routes.first().first()).commit, last.data(Qt::UserRole + 1).toString());
        QSignalSpy notifications(&page, &HomePageBackupPage::notification);
        model->item(1, 0)->setText("attempted edit");
        settle(service);
        QCOMPARE(notifications.count(), 1);
        QCOMPARE(model->item(0, 0)->text(), QString("Revision 1000"));
        for (int i = 0; i < 5; ++i)
            page.refresh();
        settle(service);
        QCOMPARE(table->model(), model);
        QCOMPARE(model->rowCount(), 1001);
    }
    void confirmationsRespectObjectContext()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/file.txt";
        writeFile(source, "initial");
        QVERIFY(service.addLocal(source).success);
        const auto id = encoded(source);
        QWidget owner;
        owner.setAttribute(Qt::WA_DontShowOnScreen);
        BackupUiActions actions(&service, &owner);
        bool cancelWasDefault = false;
        QTimer::singleShot(0, [&]
                           {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                qFatal("Confirmation dialog missing");
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            cancelWasDefault = buttons->button(QDialogButtonBox::Cancel)->isDefault();
            dialog->reject(); });
        actions.remove(id);
        QVERIFY(cancelWasDefault);
        QVERIFY(service.contains(id));
        QTimer::singleShot(0, [&]
                           {
            if (!service.removeBackup(id).success || !service.addLocal(source).success)
                qFatal("Cannot replace confirmation fixture");
            qobject_cast<QDialog *>(QApplication::activeModalWidget())->accept(); });
        actions.remove(id);
        QVERIFY(service.contains(id));
        QTimer::singleShot(0, [&]
                           {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                qFatal("Rebuild dialog missing");
            cancelWasDefault = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel)->isDefault();
            dialog->reject(); });
        actions.rebuild(id);
        QVERIFY(cancelWasDefault);
    }
    void sharedBackupModelScales()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        BackupListModel source;
        QAbstractItemModelTester tester(&source, QAbstractItemModelTester::FailureReportingMode::QtTest);
        HomePage page(&service, nullptr, &source);
        page.setAttribute(Qt::WA_DontShowOnScreen);
        page.resize(820, 640);
        page.show();
        QTest::qWait(20);
        auto *view = page.findChild<QListView *>("backupList");
        const auto initialWidgets = page.findChildren<QWidget *>().size();
        QVector<TrackedItem> items;
        for (int count : {0, 1, 50, 500})
        {
            items.clear();
            for (int row = 0; row < count; ++row)
            {
                const auto path = QString("C:/Workspace/长中文目录-%1/项目文件.txt").arg(row);
                items.append({QString::number(row), path, QString("备份对象 %1").arg(row, 3, 10, QChar('0'))});
            }
            source.setItems(items);
            QCOMPARE(source.rowCount(), count);
            QCOMPARE(view->model()->rowCount(), count);
            QCoreApplication::processEvents();
            QVERIFY(page.findChildren<QWidget *>().size() <= initialWidgets + 8);
        }
        const QPersistentModelIndex retained(source.indexForId("125"));
        std::reverse(items.begin(), items.end());
        items[items.size() - 126].name = "重命名后仍保留 ID";
        source.setItems(items);
        QVERIFY(retained.isValid());
        QCOMPARE(retained.data(BackupListModel::IdRole).toString(), QString("125"));
        page.findChild<QLineEdit *>("filter")->setText("目录-125/");
        QCOMPARE(view->model()->rowCount(), 1);
        QCOMPARE(view->model()->index(0, 0).data(BackupListModel::IdRole).toString(), QString("125"));
        page.findChild<QLineEdit *>("filter")->clear();
        QCOMPARE(view->model()->rowCount(), 500);
        QVERIFY(view->viewport()->height() / view->sizeHintForRow(0) >= 8);
        QVERIFY(page.findChildren<QGroupBox *>().isEmpty());
        QSignalSpy routes(&page, &HomePage::navigate);
        auto *delegate = qobject_cast<BackupItemDelegate *>(view->itemDelegate());
        QSignalSpy menus(delegate, &BackupItemDelegate::menuRequested);
        view->setCurrentIndex(view->model()->index(0, 0));
        const auto rect = view->visualRect(view->currentIndex());
        QTest::mouseClick(view->viewport(), Qt::LeftButton, {}, QPoint(rect.right() - 16, rect.center().y()));
        QCOMPARE(routes.count(), 0);
        QCOMPARE(menus.count(), 1);
        QContextMenuEvent context(QContextMenuEvent::Keyboard, rect.center(), view->viewport()->mapToGlobal(rect.center()));
        QApplication::sendEvent(view->viewport(), &context);
        QCOMPARE(menus.count(), 2);
    }
    void revisionTimeAndSelection()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/history.txt";
        const auto id = encoded(source);
        writeFile(source, "one\n");
        QVERIFY(service.addLocal(source).success);
        QVERIFY(runGit(service.repoPath(id), {"commit", "--amend", "--allow-empty-message", "-m", "", "--date=2001-02-03T04:05:06+0800"}).success());
        QVector<Revision> revisions;
        QVERIFY(service.history(id, revisions).success);
        QCOMPARE(revisions.size(), 1);
        QVERIFY(revisions.first().message.isEmpty());
        QVERIFY(revisions.first().committedAt.isValid());
        // The UI uses committer time (%ct), not a timestamp parsed from a subject.
        const auto epoch = runGit(service.repoPath(id), {"log", "-1", "--format=%ct"}).output.trimmed().toLongLong();
        QCOMPARE(revisions.first().committedAt.toSecsSinceEpoch(), epoch);
        writeFile(source, "two\n");
        QVERIFY(service.backup(id).success);
        settle(service);
        HomePageBackupPage page(&service);
        page.setAttribute(Qt::WA_DontShowOnScreen);
        page.resize(820, 640);
        page.setBackup(id);
        settle(service);
        page.show();
        auto *table = page.findChild<QTableView *>("table");
        table->selectRow(1);
        const auto previous = table->model()->index(1, 2).data(Qt::UserRole + 1).toString();
        writeFile(source, "three\n");
        QVERIFY(service.backup(id).success);
        settle(service);
        QCOMPARE(table->model()->index(table->currentIndex().row(), 2).data(Qt::UserRole + 1).toString(), previous);
        QSignalSpy routes(&page, &HomePageBackupPage::navigate);
        table->setFocus();
        QTest::keyClick(table, Qt::Key_Return);
        QCOMPARE(routes.count(), 1);
        QCOMPARE(qvariant_cast<Route>(routes.first().first()).commit, previous);
        // A monitor update cannot destroy an in-progress message editor.
        page.findChild<QAction *>("editMessageAction")->trigger();
        auto *editor = table->findChild<QLineEdit *>();
        QVERIFY(editor);
        editor->selectAll();
        QTest::keyClicks(editor, "pending edit");
        writeFile(source, "four\n");
        QVERIFY(service.backup(id).success);
        settle(service);
        QCOMPARE(table->model()->rowCount(), 3);
        QCOMPARE(editor->text(), QString("pending edit"));
        QTest::keyClick(editor, Qt::Key_Escape);
        QTRY_COMPARE(table->model()->rowCount(), 4);
        page.resize(520, 400);
        QCoreApplication::processEvents();
        QVERIFY(table->isColumnHidden(2));
        QCOMPARE(table->horizontalScrollBar()->maximum(), 0);
        QVERIFY(service.rebuild(id).success);
        page.setBackup(id);
        settle(service);
        QCOMPARE(table->model()->rowCount(), 1);
        QCOMPARE(table->currentIndex().row(), 0);
    }
    void asyncControlsAndThemePreserveContext()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/long.txt";
        QByteArray document;
        for (int row = 0; row < 240; ++row)
            document += QByteArray("line ") + QByteArray::number(row) + " " + QByteArray(150, 'x') + "\n";
        writeFile(source, document);
        QVERIFY(service.addLocal(source).success);
        const auto id = encoded(source);
        FakeAi gateway;
        SettingsService settings(pathsIn(dir), &gateway);
        settings.saveField("ApiKey", "fixture-only");
        settings.saveField("Model", "manual-model");
        HomePageDiffPage page(&service, &settings, &gateway);
        page.setAttribute(Qt::WA_DontShowOnScreen);
        page.resize(820, 620);
        page.setRevision(id, head(service, id));
        settle(service);
        page.show();
        QTest::qWait(20);
        auto *content = page.findChild<QPlainTextEdit *>("content");
        auto cursor = content->textCursor();
        cursor.setPosition(20);
        cursor.setPosition(80, QTextCursor::KeepAnchor);
        content->setTextCursor(cursor);
        content->verticalScrollBar()->setValue(60);
        content->horizontalScrollBar()->setValue(60);
        const auto selection = content->textCursor().selectedText();
        const auto scroll = QPoint(content->horizontalScrollBar()->value(), content->verticalScrollBar()->value());
        const auto text = content->toPlainText();
        m_theme->toggle();
        page.refreshTheme();
        QCoreApplication::processEvents();
        QCOMPARE(content->toPlainText(), text);
        QCOMPARE(content->textCursor().selectedText(), selection);
        QCOMPARE(QPoint(content->horizontalScrollBar()->value(), content->verticalScrollBar()->value()), scroll);
        m_theme->toggle();
        page.refreshTheme();
        auto *analyze = page.findChild<QAction *>("analyzeAction");
        auto *spinner = page.findChild<oclero::qlementine::LoadingSpinner *>("analysisSpinner");
        analyze->trigger();
        settle(service);
        QVERIFY(spinner->spinning());
        page.deactivate();
        page.hide();
        QVERIFY(!spinner->spinning());
        gateway.summaries.last()("stale after leaving", {});
        QVERIFY(page.findChild<QPlainTextEdit *>("analysis")->toPlainText().isEmpty());
        page.show();
        analyze->trigger();
        settle(service);
        QVERIFY(spinner->spinning());
        gateway.summaries.last()("当前分析结果", {});
        QVERIFY(!spinner->spinning());
        auto *expander = page.findChild<oclero::qlementine::Expander *>("analysisExpander");
        QVERIFY(expander->expanded());
        QTest::qWait(140);
        QVERIFY(page.findChild<QWidget *>("analysisContent")->height() <= page.findChild<QWidget *>("editorPane")->height() / 3);
        QVERIFY(expander->height() <= page.findChild<QWidget *>("editorPane")->height() / 3);
        page.resize(520, 500);
        QCoreApplication::processEvents();
        QCOMPARE(page.findChild<QSplitter *>("splitter")->orientation(), Qt::Vertical);
        QTest::qWait(140);
        QVERIFY(page.findChild<QWidget *>("analysisContent")->height() <= page.findChild<QWidget *>("editorPane")->height() / 3);
        QVERIFY(expander->height() <= page.findChild<QWidget *>("editorPane")->height() / 3);
        analyze->trigger();
        settle(service);
        gateway.summaries.last()({}, "模拟分析失败");
        QVERIFY(!spinner->spinning());
        QCOMPARE(page.findChild<QPlainTextEdit *>("analysis")->toPlainText(), QString("模拟分析失败"));
        analyze->trigger();
        settle(service);
        QVERIFY(service.rebuild(id).success);
        QVERIFY(!spinner->spinning());
        gateway.summaries.last()("stale after rebuilding", {});
        QVERIFY(page.findChild<QPlainTextEdit *>("analysis")->toPlainText().isEmpty());

        SettingPageAiPage ai(&settings);
        ai.setAttribute(Qt::WA_DontShowOnScreen);
        ai.show();
        auto *modelsSpinner = ai.findChild<oclero::qlementine::LoadingSpinner *>("modelsSpinner");
        ai.findChild<QPushButton *>("fetchButton")->click();
        QVERIFY(modelsSpinner->spinning());
        ai.hide();
        QVERIFY(!modelsSpinner->spinning());
        ai.show();
        QVERIFY(modelsSpinner->spinning());
        settings.selectProvider("Custom");
        QVERIFY(!modelsSpinner->spinning());
        gateway.models.last()({"stale"}, {});
        QVERIFY(settings.config("Custom").modelList.isEmpty());
        settings.selectProvider("OpenAI");
        ai.findChild<QPushButton *>("fetchButton")->click();
        gateway.models.last()({"model-a", "model-b"}, {});
        auto *combo = ai.findChild<QComboBox *>("model");
        QCOMPARE(combo->currentText(), QString("manual-model"));
        QCOMPARE(combo->count(), 2);
        QCOMPARE(combo->completer()->filterMode(), Qt::MatchContains);
        QVERIFY(!modelsSpinner->spinning());
        ai.findChild<QPushButton *>("fetchButton")->click();
        gateway.models.last()({}, "模拟模型获取失败");
        QVERIFY(!modelsSpinner->spinning());
        QVERIFY(ai.findChild<QLabel *>("statusLabel")->text().contains("模拟模型获取失败"));
    }
    void notificationsPauseWhileDetailsAreOpen()
    {
        QWidget host;
        host.setAttribute(Qt::WA_DontShowOnScreen);
        host.resize(640, 480);
        host.show();
        QApplication::setActiveWindow(&host);
        NotificationBar notification(&host);
        const QString message = "第一行错误。\n" + QString(180, QChar(u'文')) + "\n可完整选择与复制。";
        notification.showResult(OperationResult::fail("测试提示", message, 220));
        auto *detailsButton = notification.findChild<QPushButton *>("notificationDetailsButton");
        QVERIFY(detailsButton->isVisible());
        detailsButton->setFocus();
        QVERIFY(detailsButton->hasFocus());
        detailsButton->click();
        QPointer<oclero::qlementine::Popover> popover = notification.findChild<oclero::qlementine::Popover *>("notificationPopover");
        QVERIFY(popover);
        QTRY_VERIFY(popover->isOpened());
        QTest::qWait(260);
        QVERIFY(notification.isVisible());
        auto *details = popover->findChild<QPlainTextEdit *>("notificationDetails");
        QCOMPARE(details->toPlainText(), "测试提示\n\n" + message);
        details->selectAll();
        auto savedClipboard = std::make_unique<QMimeData>();
        const auto *original = QApplication::clipboard()->mimeData();
        if (original)
            for (const auto &format : original->formats())
                savedClipboard->setData(format, original->data(format));
        details->copy();
        const auto copied = QApplication::clipboard()->text();
        QApplication::clipboard()->setMimeData(savedClipboard.release());
        QCOMPARE(copied, details->toPlainText());
        QTest::keyClick(details, Qt::Key_Escape);
        QTRY_VERIFY(!popover);
        QTRY_VERIFY(detailsButton->hasFocus());
        QTRY_VERIFY(!notification.isVisible());
        notification.showResult(OperationResult::fail("旧消息", message, 220));
        detailsButton->click();
        popover = notification.findChild<oclero::qlementine::Popover *>("notificationPopover");
        QVERIFY(popover);
        notification.showResult(OperationResult::ok("新消息", "新通知会关闭旧详情。", 180));
        QTRY_VERIFY(!popover);
        QVERIFY(notification.isVisible());
        QTRY_VERIFY(!notification.isVisible());
        notification.showResult(OperationResult::fail("外部关闭", message, 300));
        detailsButton->click();
        popover = notification.findChild<oclero::qlementine::Popover *>("notificationPopover");
        QVERIFY(popover);
        QTest::qWait(140);
        // Deliver an outside click to the popup, as the native popup grab does.
        QTest::mouseClick(popover, Qt::LeftButton, {}, QPoint(-12, -12));
        QTRY_VERIFY(!popover);
        QTRY_VERIFY(!notification.isVisible());
    }
    void renderAllPages()
    {
        TestDirectory dir(QDir::tempPath() + "/zcu-XXXXXX");
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/Projects/示例项目";
        const auto readme = source + "/README.md";
        writeFile(readme, "# 项目说明\n\n自动保存每一次重要修改。\n");
        writeFile(source + "/src/settings.json", "{\n  \"theme\": \"system\",\n  \"interval\": 1500\n}\n");
        writeFile(source + "/docs/使用说明.txt", "选择文件后自动保存版本。\n");
        const auto added = service.addLocal(source);
        QVERIFY2(added.success, qPrintable(added.message));
        const auto id = encoded(source);
        const QStringList messages{"补充项目说明", "更新使用文档", "调整默认配置", "整理文件结构", "记录设计方案", "补充中文路径示例",
                                   "更新设置说明", "整理版本说明", "完善备份流程", "检查远程配置", "统一界面文字", "改进历史版本的阅读体验"};
        for (int i = 0; i < messages.size(); ++i)
        {
            writeFile(readme, QString("# 项目说明\n\n%1\n\n自动备份文件与文件夹。\n版本 %2\n").arg(messages[i]).arg(i + 1).toUtf8());
            QVERIFY(service.backup(id).success);
            settle(service);
            QVERIFY(runGit(service.repoPath(id), {"commit", "--amend", "-m", messages[i]}).success());
        }
        writeFile(readme, "# 项目说明\n\n集中查看历史版本与文件变更。\n\n支持文件、文件夹和云端导入。\n");
        writeFile(source + "/src/settings.json", "{\n  \"theme\": \"neutral\",\n  \"interval\": 1500,\n  \"compact\": true\n}\n");
        writeFile(source + "/docs/使用说明.txt", "从侧栏选择备份对象，直接进入历史版本。\n双击版本可查看变更内容。\n");
        QVERIFY(service.backup(id).success);
        settle(service);
        QVERIFY(runGit(service.repoPath(id), {"commit", "--amend", "-m", "统一导航，简化版本对比与配置"}).success());
        // Small real fixtures populate the isolated catalog used by the list.
        const QStringList names{"产品需求.md", "会议记录", "个人知识库", "设计资源", "Release notes.md", "配置文件", "实验数据", "开发日志.md", "项目归档", "工作清单.md", "长中文名称的项目资料与使用文档"};
        for (int i = 0; i < names.size(); ++i)
        {
            const auto path = dir.path() + "/工作目录/" + names[i];
            writeFile(path, "fixture");
            QVERIFY(service.addLocal(path).success);
        }
        FakeAi gateway;
        SettingsService settings(pathsIn(dir), &gateway);
        MainWindow window(&service, &settings, &gateway, m_theme, false);
        settings.saveField("ApiKey", "fixture-only");
        settings.saveField("Model", "test-model");
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.show();
        QVERIFY(!window.windowFlags().testFlag(Qt::FramelessWindowHint));
        auto *windowSplitter = window.findChild<QSplitter *>("windowSplitter");
        for (int sidebarWidth : {200, 280, 224})
        {
            windowSplitter->setSizes({sidebarWidth, window.width() - sidebarWidth});
            QCOMPARE(window.findChild<QWidget *>("sidebar")->width(), sidebarWidth);
        }
        const auto output = qEnvironmentVariable("ZC_TEST_SCREENSHOTS");
        if (!output.isEmpty())
            QDir().mkpath(output);
        const auto capture = [&](const QString &name)
        {
            if (!output.isEmpty() && !window.grab().save(output + "/" + name + ".png"))
                qFatal("Cannot save screenshot");
        };
        const QVector<Route> routes{{}, {PageId::Dashboard, id}, {PageId::History, id}, {PageId::Diff, id, head(service, id)}, {PageId::GeneralSettings}, {PageId::AiSettings}, {PageId::About}};
        for (int theme = 0; theme < 2; ++theme)
        {
            for (int i = 0; i < routes.size(); ++i)
            {
                window.navigate(routes[i]);
                settle(service);
                QTest::qWait(160);
                QVERIFY(window.isVisible());
                QCOMPARE(window.size(), QSize(1080, 740));
                capture(QString("page-%1-%2").arg(theme).arg(i));
                if (routes[i].page == PageId::History)
                {
                    auto *table = window.findChild<HomePageBackupPage *>()->findChild<QTableView *>("table");
                    QVERIFY(table->viewport()->height() / table->rowHeight(0) >= 10);
                    hoverHistoryAction(table, 2, 0);
                    capture(QString("history-hover-%1").arg(theme));
                    QTest::mouseClick(table->viewport(), Qt::LeftButton, {}, historyActionPoint(table, 2, 2));
                    QTRY_VERIFY(QApplication::activePopupWidget());
                    auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                    QVERIFY(menu);
                    QTest::qWait(160);
                    if (!output.isEmpty())
                        QVERIFY(menu->grab().save(output + QString("/history-menu-%1.png").arg(theme)));
                    menu->close();
                    QTRY_VERIFY(!QApplication::activePopupWidget());
                    QApplication::setActiveWindow(&window);
                    table->setFocus(Qt::TabFocusReason);
                    QVERIFY(table->hasFocus());
                    QEvent leave(QEvent::Leave);
                    QApplication::sendEvent(table->viewport(), &leave);
                    QTest::keyClick(table, Qt::Key_Down);
                    capture(QString("history-focus-%1").arg(theme));
                }
                if (routes[i].page == PageId::Backups)
                {
                    auto *view = window.findChild<HomePage *>()->findChild<QListView *>("backupList");
                    QVERIFY(view->viewport()->height() / view->sizeHintForRow(0) >= 8);
                    window.findChild<QAction *>("addBackupAction")->trigger();
                    auto *menu = window.findChild<QMenu *>("addBackupMenu");
                    menu->setActiveAction(menu->actions().first());
                    QTest::qWait(140);
                    if (!output.isEmpty())
                        QVERIFY(menu->grab().save(output + QString("/add-menu-%1.png").arg(theme)));
                    menu->hide();
                }
                if (routes[i].page == PageId::Diff)
                {
                    window.findChild<HomePageDiffPage *>()->findChild<QAction *>("analyzeAction")->trigger();
                    settle(service);
                    QVERIFY(!gateway.summaries.isEmpty());
                    capture(QString("loading-%1").arg(theme));
                    gateway.summaries.last()("本次版本统一了导航与版本对比。\n\n• 更新项目说明，集中展示历史版本\n• 保留原有自动备份周期\n• 补充从侧栏打开历史的使用说明\n\n变更未调整备份格式或恢复策略。", {});
                    QTest::qWait(160);
                    capture(QString("analysis-%1").arg(theme));
                }
            }
            window.resize(1440, 920);
            for (const auto pageId : {PageId::GeneralSettings, PageId::AiSettings, PageId::About})
            {
                window.navigate({pageId});
                settle(service);
                QTest::qWait(160);
                auto *page = window.findChild<QStackedWidget *>("pages")->currentWidget();
                auto *scroll = page->findChild<QScrollArea *>("scroll");
                auto *body = page->findChild<QWidget *>("body");
                QVERIFY(body->width() <= 760);
                const auto center = body->mapTo(scroll->viewport(), body->rect().center());
                QVERIFY(qAbs(center.x() - scroll->viewport()->rect().center().x()) <= 2);
                QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
                capture(QString("settings-wide-%1-%2").arg(theme).arg(static_cast<int>(pageId)));
            }
            window.resize(1080, 740);
            window.navigate({PageId::AiSettings});
            settle(service);
            auto *settingsSearch = window.findChild<QLineEdit *>("settingsSearch");
            settingsSearch->setText("API Key");
            QTest::qWait(160);
            capture(QString("settings-search-%1").arg(theme));
            settingsSearch->setText("没有匹配项");
            QTest::qWait(160);
            capture(QString("settings-empty-%1").arg(theme));
            settingsSearch->clear();
            settings.selectProvider("Custom");
            settings.saveField("BaseUrl", "https://api.example.test/v1");
            settings.saveField("ApiKey", "fixture-only");
            settings.saveField("Model", "manual-model");
            QTest::qWait(160);
            auto *aiPage = window.findChild<SettingPageAiPage *>();
            QVERIFY(aiPage->findChild<QLineEdit *>("baseUrl")->isVisible());
            capture(QString("settings-custom-%1").arg(theme));
            settings.selectProvider("OpenAI");
            settings.fetchModels();
            QTest::qWait(40);
            capture(QString("models-loading-%1").arg(theme));
            gateway.models.last()({"test-model", "manual-model", "example-model"}, {});
            window.notify(OperationResult::fail("同步未完成", "无法连接到示例仓库。\n请检查远程地址是否正确，以及当前账户是否拥有仓库访问权限。"
                                                              "\n本地备份和历史版本仍然保留，可以稍后重新尝试同步。"
                                                              "\n详细信息：连接在等待响应时超时，远程服务未返回结果。请检查网络连接和代理设置；如果问题持续，请联系仓库管理员。",
                                                0));
            QTest::qWait(160);
            capture(QString("notification-%1").arg(theme));
            window.findChild<QPushButton *>("notificationDetailsButton")->click();
            QTest::qWait(160);
            auto *popover = window.findChild<oclero::qlementine::Popover *>("notificationPopover");
            QVERIFY(popover);
            if (!output.isEmpty())
                QVERIFY(popover->grab().save(output + QString("/popover-%1.png").arg(theme)));
            window.findChild<NotificationBar *>()->dismiss();
            m_theme->toggle();
        }
        QVERIFY(window.findChildren<QGroupBox *>().isEmpty());
        window.resize(760, 520);
        for (int i = 0; i < routes.size(); ++i)
        {
            window.navigate(routes[i]);
            settle(service);
            QTest::qWait(160);
            QCOMPARE(window.size(), QSize(760, 520));
            auto *page = window.findChild<QStackedWidget *>("pages")->currentWidget();
            for (auto *scroll : page->findChildren<QScrollArea *>())
                QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
            if (routes[i].page == PageId::History)
            {
                auto *table = page->findChild<QTableView *>("table");
                hoverHistoryAction(table, 2, 1);
                QTest::qWait(160);
                QCOMPARE(table->horizontalScrollBar()->maximum(), 0);
            }
            if (routes[i].page == PageId::AiSettings)
            {
                auto *label = page->findChild<QLabel *>("providerLabel");
                auto *field = page->findChild<QComboBox *>("provider");
                auto *body = page->findChild<QWidget *>("body");
                QVERIFY(field->mapTo(body, QPoint()).y() > label->mapTo(body, label->rect().bottomLeft()).y());
            }
            capture(QString("compact-%1").arg(i));
            if (routes[i].page == PageId::AiSettings)
            {
                settings.selectProvider("Custom");
                QTest::qWait(160);
                auto *scroll = page->findChild<QScrollArea *>("scroll");
                QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
                capture("compact-ai-custom");
                scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
                QTest::qWait(160);
                QVERIFY(page->findChild<QPushButton *>("fetchButton")->visibleRegion().boundingRect().height() > 0);
                capture("compact-ai-custom-bottom");
                settings.selectProvider("OpenAI");
            }
        }
        window.navigate({PageId::Dashboard, id});
        settle(service);
        auto *dashboard = window.findChild<HomePageDashboardPage *>();
        dashboard->findChild<QToolButton *>("expandButton")->click();
        QTest::qWait(160);
        capture("compact-expanded");
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
        capture("long-path");
        window.findChild<QToolButton *>("collapseButton")->click();
        QTRY_VERIFY(!window.findChild<QWidget *>("sidebar")->isVisible());
        capture("sidebar-collapsed");
        window.findChild<QToolButton *>("collapseButton")->click();
        QTRY_VERIFY(window.findChild<QWidget *>("sidebar")->isVisible());
        window.findChild<QAction *>("pinAction")->trigger();
        QVERIFY(window.windowFlags().testFlag(Qt::WindowStaysOnTopHint));
        window.findChild<QAction *>("pinAction")->trigger();
        QVERIFY(!window.windowFlags().testFlag(Qt::WindowStaysOnTopHint));
        window.navigate({PageId::About});
        settle(service);
        window.findChild<QToolButton *>("backButton")->click();
        QCOMPARE(window.findChild<QStackedWidget *>("pages")->currentWidget(), window.findChild<HomePageDashboardPage *>());
        window.findChild<QToolButton *>("forwardButton")->click();
        QCOMPARE(window.findChild<QStackedWidget *>("pages")->currentWidget()->objectName(), QString("AboutPage"));
        QVERIFY(service.removeBackup(id).success);
        // Remove only registered fixtures in this QTemporaryDir.
        for (const auto &item : service.trackedItems())
            QVERIFY(service.removeBackup(item.id).success);
        emit service.trackedItemsChanged();
        window.navigate({});
        settle(service);
        QTest::qWait(160);
        QVERIFY(window.findChild<HomePage *>()->findChild<QLabel *>("emptyLabel")->isVisible());
        capture("empty");
    }

  private:
    TestDirectory m_environment;
    ThemeController *m_theme{nullptr};
};
QTEST_MAIN(Regression)
#include "regression.moc"
