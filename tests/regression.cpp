#include <QInputDialog>
#include <QMessageBox>
#include "windows/mainwindow_child/homepage/pages/homepage_page_preview.h"
#include "windows/mainwindow_child/homepage/pages/homepage_preview_widgets.h"
#include "backup_test_support.h"
#include "utils/backupmonitor.h"
#include "utils/gitcommand.h"
#include "utils/update_service.h"
#include "windows/mainwindow.h"
#include "windows/mainwindow_child/homepage/homepage.h"
#include "windows/mainwindow_child/homepage/pages/homepage_page_backup.h"
#include "windows/mainwindow_child/homepage/pages/homepage_page_branches.h"
#include "windows/mainwindow_child/homepage/pages/homepage_history_graph.h"
#include <QTabWidget>
#include <QTextBrowser>
#include <QTreeView>
#include <QTimer>
#include "windows/mainwindow_child/homepage/pages/homepage_page_dashboard.h"
#include "windows/mainwindow_child/homepage/pages/homepage_page_collaboration.h"
#include "windows/mainwindow_child/homepage/pages/homepage_page_diff.h"
#include "windows/mainwindow_child/homepage/pages/homepage_page_conflict.h"
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
#include <QCheckBox>
#include <QFile>
#include <QFontInfo>
#include <QGlyphRun>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QLockFile>
#include <QMenu>
#include <QMimeData>
#ifdef _WIN32
#include <windows.h>
#endif
#include <QMouseEvent>
#include <QPainter>
#include <QPersistentModelIndex>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRawFont>
#include <QScrollArea>
#include <QScrollBar>
#include <QSemaphore>
#include <QSettings>
#include <QStyleHints>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QTextEdit>
#include <QTextLayout>
#include <QTimeZone>
#include <QSystemTrayIcon>
#include <QSslSocket>
#include <QImageReader>
#include <QToolButton>
#include <QToolTip>
#include <QUrl>
#include <algorithm>
#include <atomic>
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
    return {cell.x() + 8 + action * (28 + 4) + 14, cell.center().y()};
}
void openHistoryMenu(QTableView *table, int row)
{
    const auto point = table->visualRect(table->model()->index(row, 0)).center();
    QTest::mouseMove(table->viewport(), point);
    QContextMenuEvent context(QContextMenuEvent::Mouse, point, table->viewport()->mapToGlobal(point));
    QApplication::sendEvent(table->viewport(), &context);
}
QWidget *branchRow(QWidget *page, const QString &ref)
{
    for (auto *row : page->findChildren<QWidget *>("branchRow"))
        if (row->property("ref").toString() == ref) return row;
    return nullptr;
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

struct ResolutionFixture
{
    TestDirectory dir;
    TestBackupService service;
    QString source{dir.path() + "/project"}, id, remote{dir.path() + "/remote.git"}, writer{dir.path() + "/writer"};
    explicit ResolutionFixture(bool conflicts = true, BackupDependencies dependencies = {}, bool crlf = false)
        : service(pathsIn(dir), nullptr, nullptr, std::move(dependencies))
    {
        const auto git = [](const QString &path, const QStringList &args)
        {
            const auto r = runGit(path, args);
            if (!r.success()) qFatal("Conflict fixture: %s", qPrintable(r.error));
        };
        const auto text = [crlf](QByteArray bytes) { return crlf ? bytes.replace("\n", "\r\n") : bytes; };
        writeFile(source + "/使用说明.txt", text("第一段\nb\nc\nd\ne\nf\n第二段\n"));
        writeFile(source + "/image.bin", QByteArray("base\0image", 10));
        writeFile(source + "/remove.txt", "base\n");
        if (!service.addLocal(source).success) qFatal("Cannot create conflict fixture");
        id = service.idForSource(source);
        git({}, {"init", "--bare", remote});
        if (!service.setRemote(id, remote).success || !service.synchronize(id, true).success) qFatal("Cannot configure fixture remote");
        git({}, {"clone", remote, writer});
        git(writer, {"config", "user.name", "Fixture"});
        git(writer, {"config", "user.email", "fixture@example.test"});
        writeFile(writer + "/project/使用说明.txt", text("云端第一段\nb\nc\nd\ne\nf\n云端第二段\n"));
        writeFile(writer + "/project/image.bin", QByteArray("remote\0image", 12));
        git(writer, {"rm", "project/remove.txt"});
        writeFile(writer + "/project/automatic.txt", "自动合并的新增文件\n");
        git(writer, {"add", "--all"}); git(writer, {"commit", "-m", "cloud changes"}); git(writer, {"push"});
        if (conflicts)
        {
            writeFile(source + "/使用说明.txt", text("本地第一段\nb\nc\nd\ne\nf\n本地第二段\n"));
            writeFile(source + "/image.bin", QByteArray("local\0image", 11));
            writeFile(source + "/remove.txt", "local changes\n");
        }
    }
};
bool answerConfirmation(QAbstractButton *button, BackupService &service, const std::function<void(QDialog *)> &answer)
{
    bool shown = false;
    QTimer timer;
    timer.setInterval(10);
    QObject::connect(&timer, &QTimer::timeout, &timer, [&]
    {
        if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
        {
            timer.stop(); shown = true; answer(dialog);
        }
    });
    timer.start(); button->click();
    if (!QTest::qWaitFor([&] { return shown; }, 5000)) return false;
    settle(service);
    return true;
}
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
    void startupHomeWaitsBeforeShowingEmptyState()
    {
        TestDirectory dir;
        BackupService service(pathsIn(dir));
        FakeAi gateway;
        SettingsService settings(pathsIn(dir), &gateway);
        MainWindow window(&service, &settings, &gateway, m_theme, false);
        auto *page = window.findChild<HomePage *>();
        auto *spinner = window.findChild<oclero::qlementine::LoadingSpinner *>("pageSpinner");
        QVERIFY(page && spinner);
        QVERIFY(page->loading());
        QVERIFY(!spinner->isHidden());
        QCOMPARE(page->findChild<QLabel *>("emptyLabel")->text(), QString("正在加载备份"));
        QVERIFY(!page->findChild<QLabel *>("countLabel")->text().contains("0"));
        QVERIFY(page->findChild<QPushButton *>("emptyAddButton")->isHidden());
        settle(service);
        QVERIFY(!page->loading());
        QVERIFY(spinner->isHidden());
        QCOMPARE(page->findChild<QLabel *>("emptyLabel")->text(), QString("还没有备份"));
        QCOMPARE(page->findChild<QLabel *>("countLabel")->text(), QString("0 个备份"));
        QVERIFY(!page->findChild<QPushButton *>("emptyAddButton")->isHidden());
        QVERIFY(page->findChild<QPushButton *>("retryButton")->isHidden());
    }
    void startupHomeDisplaysItemsBeforeSlowAuditFinishes()
    {
        TestDirectory dir;
        const auto paths = pathsIn(dir);
        const auto source = dir.path() + "/source.txt";
        writeFile(source, "keep\n");
        {
            TestBackupService seed(paths);
            QVERIFY(seed.addLocal(source).success);
        }
        struct Gate
        {
            std::atomic_bool hold{true};
            QSemaphore entered, resume;
        };
        const auto gate = std::make_shared<Gate>();
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
        BackupService service(paths, nullptr, nullptr, dependencies);
        const auto releaseOnFailure = qScopeGuard([&] { gate->resume.release(); });
        FakeAi gateway;
        SettingsService settings(paths, &gateway);
        MainWindow window(&service, &settings, &gateway, m_theme, false);
        auto *page = window.findChild<HomePage *>();
        auto *spinner = window.findChild<oclero::qlementine::LoadingSpinner *>("pageSpinner");
        auto *list = page->findChild<QListView *>("backupList");
        QTRY_VERIFY(gate->entered.available() > 0);
        QTRY_COMPARE(list->model()->rowCount(), 1);
        QCOMPARE(page->findChild<QLabel *>("countLabel")->text(), QString("1 个备份"));
        QVERIFY(!page->findChild<QWidget *>("listPage")->isHidden());
        QVERIFY(!spinner->isHidden());
        QVERIFY(!service.isReady());
        window.navigate({PageId::GeneralSettings});
        QVERIFY(spinner->isHidden());
        window.navigate({});
        QVERIFY(!spinner->isHidden());
        gate->resume.release();
        settle(service);
        QVERIFY(spinner->isHidden());
        QVERIFY(service.isReady());
        QCOMPARE(list->model()->rowCount(), 1);
    }
    void startupHomeOffersRetryAfterLoadFailure_data()
    {
        QTest::addColumn<QString>("failure");
        QTest::newRow("writer-lock") << QString("lock");
        QTest::newRow("storage-unavailable") << QString("storage");
        QTest::newRow("unreadable-record") << QString("record");
    }
    void startupHomeOffersRetryAfterLoadFailure()
    {
        QFETCH(QString, failure);
        TestDirectory dir;
        const auto paths = pathsIn(dir);
        QLockFile lock(paths.backupRoot + "/.writer.lock");
        const auto recordPath = paths.backupRoot + "/items/" + QUuid::createUuid().toString(QUuid::WithoutBraces) + "/record.json";
        if (failure == "storage")
            writeFile(paths.backupRoot, "not a directory");
        else if (failure == "record")
            writeFile(recordPath, "{invalid record");
        else
        {
            QVERIFY(QDir().mkpath(paths.backupRoot));
            QVERIFY(lock.tryLock(0));
        }
        BackupService service(paths);
        settle(service);
        FakeAi gateway;
        SettingsService settings(paths, &gateway);
        MainWindow window(&service, &settings, &gateway, m_theme, false);
        auto *page = window.findChild<HomePage *>();
        auto *retry = page->findChild<QPushButton *>("retryButton");
        auto *spinner = window.findChild<oclero::qlementine::LoadingSpinner *>("pageSpinner");
        QVERIFY(!retry->isHidden());
        QVERIFY(!page->findChild<QLabel *>("emptyHint")->text().isEmpty());
        QVERIFY(page->findChild<QPushButton *>("emptyAddButton")->isHidden());
        QVERIFY(!page->findChild<QLabel *>("countLabel")->text().contains("0"));
        QVERIFY(spinner->isHidden());
        if (failure == "record")
        {
            QCOMPARE(page->findChild<QLabel *>("emptyLabel")->text(), QString("备份记录无法完整读取"));
            QVERIFY(QFile::remove(recordPath));
            QVERIFY(QDir().rmdir(QFileInfo(recordPath).absolutePath()));
        }
        else
        {
            QCOMPARE(page->findChild<QLabel *>("emptyLabel")->text(), QString("备份加载失败"));
            if (failure == "storage")
                QVERIFY(QFile::remove(paths.backupRoot));
            else
                lock.unlock();
        }
        retry->click();
        QVERIFY(page->loading());
        QVERIFY(!spinner->isHidden());
        QVERIFY(retry->isHidden());
        settle(service);
        QVERIFY(service.isReady());
        QVERIFY(spinner->isHidden());
        QVERIFY(retry->isHidden());
        QCOMPARE(page->findChild<QLabel *>("emptyLabel")->text(), QString("还没有备份"));
        QVERIFY(!page->findChild<QPushButton *>("emptyAddButton")->isHidden());
    }
    void sourcePreviewUnsavedNavigationAndExternalEdits()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/file.txt";
        writeFile(source, "original\n"); QVERIFY(service.addLocal(source).success);
        const auto id = service.idForSource(source);
        FakeAi gateway; SettingsService settings(pathsIn(dir), &gateway);
        MainWindow window(&service, &settings, &gateway, m_theme, false);
        window.setAttribute(Qt::WA_DontShowOnScreen); window.show(); window.navigate({PageId::Preview, id}); settle(service);
        auto *page = window.findChild<HomePagePreviewPage *>();
        auto *editor = page->findChild<PreviewTextEdit *>("editor");
        auto *edit = page->findChild<QAction *>("editSourceAction");
        auto *save = page->findChild<QAction *>("saveSourceAction");
        QVERIFY(edit->isEnabled()); edit->trigger(); QVERIFY(!editor->isReadOnly());
        editor->moveCursor(QTextCursor::End); editor->insertPlainText("edited\n");
        QVERIFY(page->hasUnsavedChanges()); QVERIFY(service.isSyncEditing(id));
        const auto blocked = service.syncAll(id); QCOMPARE(blocked.value.status.state, SyncState::Editing);
        QCOMPARE(readFile(source), QByteArray("original\n"));
        auto *history = window.findChild<QToolButton *>("historyTab"); history->click();
        auto *dialog = page->findChild<QDialog *>("previewUnsavedDialog"); QVERIFY(dialog && dialog->isVisible());
        dialog->done(QMessageBox::Cancel); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(window.findChild<QToolButton *>("previewTab")->isChecked());
        QCOMPARE(window.findChild<QStackedWidget *>("pages")->currentWidget(), page);
        QCOMPARE(readFile(source), QByteArray("original\n"));
        history->click();
        dialog = page->findChild<QDialog *>("previewUnsavedDialog"); QVERIFY(dialog); dialog->done(QMessageBox::Save);
        settle(service); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(readFile(source), QByteArray("original\nedited\n"));
        QVERIFY(!page->hasUnsavedChanges()); QVERIFY(!service.isSyncEditing(id));
        QCOMPARE(window.findChild<QStackedWidget *>("pages")->currentWidget(), window.findChild<HomePageBackupPage *>());
        window.findChild<QToolButton *>("previewTab")->click(); settle(service);
        edit->trigger(); editor->moveCursor(QTextCursor::End); editor->insertPlainText("keep this buffer");
        const auto buffer = editor->toPlainText();
        writeFile(source, "outside editor\n"); QTest::qWait(350); settle(service);
        QCOMPARE(editor->toPlainText(), buffer);
        save->trigger(); settle(service);
        QCOMPARE(readFile(source), QByteArray("outside editor\n")); QVERIFY(page->hasUnsavedChanges());
        QCOMPARE(editor->toPlainText(), buffer);
        // Closing a non-tray window must go through the same unsaved guard.
        window.close(); dialog = page->findChild<QDialog *>("previewUnsavedDialog"); QVERIFY(dialog);
        dialog->done(QMessageBox::Cancel); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(window.isVisible());
        QEvent quitEvent(QEvent::Quit); QApplication::sendEvent(qApp, &quitEvent);
        dialog = page->findChild<QDialog *>("previewUnsavedDialog"); QVERIFY(dialog);
        dialog->done(QMessageBox::Cancel); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        history->click(); dialog = page->findChild<QDialog *>("previewUnsavedDialog"); QVERIFY(dialog); dialog->done(QMessageBox::Discard);
        settle(service); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!page->hasUnsavedChanges()); QCOMPARE(readFile(source), QByteArray("outside editor\n"));
    }
    void sourcePreviewCreatesFilesAndConfirmsPermanentDeletion()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto root = dir.path() + "/source";
        writeFile(root + "/existing.txt", "keep"); QVERIFY(service.addLocal(root).success);
        const auto id = service.idForSource(root);
        FakeAi gateway; SettingsService settings(pathsIn(dir), &gateway);
        MainWindow window(&service, &settings, &gateway, m_theme, false);
        window.setAttribute(Qt::WA_DontShowOnScreen); window.show(); window.navigate({PageId::Preview, id}); settle(service);
        auto *page = window.findChild<HomePagePreviewPage *>();
        auto *newAction = page->findChild<QAction *>("newSourceEntryAction"); QVERIFY(newAction->isEnabled());
        QTimer::singleShot(0, [] {
            if (auto *input = qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) {
                input->setTextValue("new.txt"); input->accept();
            } else if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
                if (auto *edit = dialog->findChild<QLineEdit *>()) edit->setText("new.txt");
                dialog->accept();
            }
        });
        newAction->menu()->actions().first()->trigger();
        QVERIFY(page->hasUnsavedChanges()); QVERIFY(!QFileInfo::exists(root + "/new.txt"));
        auto *editor = page->findChild<PreviewTextEdit *>("editor"); editor->insertPlainText("new content");
        page->findChild<QAction *>("saveSourceAction")->trigger(); settle(service);
        QCOMPARE(readFile(root + "/new.txt"), QByteArray("new content"));
        QCOMPARE(page->rememberedPath(id), QString("new.txt"));
        const auto openDelete = [&] {
            page->findChild<QAction *>("manageSourceEntryAction")->trigger();
            auto *menu = page->findChild<QMenu *>("sourceEntryMenu");
            QVERIFY(menu);
            menu->actions().last()->trigger();
            menu->close();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            settle(service);
        };
        openDelete();
        auto *dialog = page->findChild<QDialog *>("previewDeleteDialog"); QVERIFY(dialog && dialog->isVisible());
        auto *label = dialog->findChild<QLabel *>(); QVERIFY(label && label->text().contains("不会移入废纸篓"));
        writeFile(root + "/new.txt", "changed after confirmation");
        dialog->done(QMessageBox::Yes); settle(service); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(readFile(root + "/new.txt"), QByteArray("changed after confirmation"));
        openDelete(); dialog = page->findChild<QDialog *>("previewDeleteDialog"); QVERIFY(dialog);
        dialog->done(QMessageBox::Yes); settle(service); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!QFileInfo::exists(root + "/new.txt")); QCOMPARE(page->rememberedPath(id), QString());
        QCOMPARE(readFile(root + "/existing.txt"), QByteArray("keep"));
    }
    void sourcePreviewMarkdownImagesAndLimits()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto root = dir.path() + "/docs";
        writeFile(root + "/README.md", "# Local preview\n\n![local](image.png)\n\n![remote](https://invalid.example/image.png)\n\n<img src=\"file:///outside\">\n");
        QImage fixture(320, 180, QImage::Format_RGB32); fixture.fill(QColor("#3b82f6")); QVERIFY(fixture.save(root + "/image.png"));
        QVERIFY(service.addLocal(root).success);
        const auto id = service.idForSource(root);
        HomePagePreviewPage page(&service); page.setAttribute(Qt::WA_DontShowOnScreen); page.resize(800, 560); page.show();
        page.setBackup(id, "README.md"); settle(service);
        auto *markdown = page.findChild<PreviewMarkdown *>("markdown"); QVERIFY(markdown->isVisible());
        QCOMPARE(qvariant_cast<QImage>(markdown->document()->resource(QTextDocument::ImageResource, QUrl::fromLocalFile(root + "/image.png"))).size(), fixture.size());
        QVERIFY(!markdown->document()->resource(QTextDocument::ImageResource, QUrl("https://invalid.example/image.png")).isValid());
        QVERIFY(!markdown->localTarget(QUrl::fromLocalFile(dir.path() + "/outside")).result.success);
        auto *mode = page.findChild<QToolButton *>("markdownButton"); mode->click();
        QVERIFY(page.findChild<PreviewTextEdit *>("editor")->isVisible());
        page.deactivate(); page.setBackup(id, "README.md"); settle(service); QVERIFY(mode->isChecked());
        page.setBackup(id, "image.png"); settle(service);
        QVERIFY(page.findChild<QScrollArea *>("imageScroll")->isVisible());
        page.findChild<QPushButton *>("actualButton")->click();
        QCOMPARE(page.findChild<QLabel *>("imageLabel")->pixmap().size(), fixture.size());
        const auto output = qEnvironmentVariable("ZC_PREVIEW_SCREENSHOTS");
        if (!output.isEmpty()) { QDir().mkpath(output); QVERIFY(page.grab().save(output + "/preview-image.png")); }
        QFile large(root + "/large.bin"); QVERIFY(large.open(QIODevice::WriteOnly)); QVERIFY(large.resize(SourcePreview::ImageLimit + 1)); large.close();
        page.setBackup(id, "large.bin"); settle(service);
        QVERIFY(page.findChild<QLabel *>("notice")->text().contains("上限"));
        QVERIFY(!page.findChild<QAction *>("editSourceAction")->isEnabled());
        page.deactivate();
    }
    void sourcePreviewNavigationAndRendering()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/示例工程";
        writeFile(source + "/docs/README.md", "# Preview\n\nLocal **Markdown**.\n");
        writeFile(source + "/main.cpp", "int main() {\n    return 0;\n}\n");
        writeFile(source + "/.hidden", "hidden");
        QVERIFY(service.addLocal(source).success);
        FakeAi gateway;
        SettingsService settings(pathsIn(dir), &gateway);
        MainWindow window(&service, &settings, &gateway, m_theme, false);
        window.setAttribute(Qt::WA_DontShowOnScreen); window.show();
        auto *sidebar = window.findChild<QListView *>("sidebarList");
        emit sidebar->clicked(sidebar->model()->index(0, 0)); settle(service);
        auto *page = window.findChild<HomePagePreviewPage *>();
        QCOMPARE(window.findChild<QStackedWidget *>("pages")->currentWidget(), page);
        QVERIFY(window.findChild<QToolButton *>("previewTab")->isChecked());
        auto *files = page->findChild<QTreeView *>("files");
        QCOMPARE(files->model()->rowCount(), 3);
        const auto output = qEnvironmentVariable("ZC_PREVIEW_SCREENSHOTS");
        if (!output.isEmpty()) QDir().mkpath(output);
        for (auto theme : {ThemeMode::Light, ThemeMode::Dark})
        {
            m_theme->setMode(theme);
            for (const auto size : {QSize(1080, 740), QSize(760, 520)})
            {
                window.resize(size); QTest::qWait(200);
                if (!output.isEmpty()) QVERIFY(window.grab().save(output + QString("/preview-%1-%2.png").arg(theme == ThemeMode::Light ? "light" : "dark").arg(size.width())));
            }
        }
        Route file{PageId::Preview, service.idForSource(source)}; file.path = "docs/README.md";
        window.navigate(file); settle(service);
        QCOMPARE(page->findChild<PreviewTextEdit *>("editor")->toPlainText(), QString("# Preview\n\nLocal **Markdown**.\n"));
        QVERIFY(page->findChild<PreviewMarkdown *>("markdown")->isVisible());
        window.navigate({PageId::History, file.backupId}); settle(service);
        window.findChild<QToolButton *>("previewTab")->click(); settle(service);
        QCOMPARE(page->rememberedPath(file.backupId), file.path);
        QVERIFY(page->findChild<PreviewMarkdown *>("markdown")->isVisible());
        m_theme->setMode(ThemeMode::Light);
    }
    void desktopIntegrationFilesAreOwnedAndAtomic()
    {
        TestDirectory dir;
        LinuxIntegrationPaths paths{dir.path() + "/config", dir.path() + "/data",
                                    dir.path() + QStringLiteral("/app's 空格 $`%/ZcVersionBox")};
        LinuxIntegration integration(paths);
        const auto startup = paths.configHome + "/autostart/com.zc.versionbox.desktop";
        const auto script = paths.dataHome + QStringLiteral("/nautilus/scripts/添加到 ZcVersionBox");
        QVERIFY(integration.setAutoStart(true).success);
        const auto original = readFile(startup);
        QVERIFY(original.contains("[Desktop Entry]"));
        QVERIFY(original.contains("%%"));
        QVERIFY(original.contains("Icon=zcversionbox"));
        QVERIFY(integration.setAutoStart(true).success);
        QCOMPARE(readFile(startup), original);
        QVERIFY(integration.setNautilusScript(true).success);
        QVERIFY(readFile(script).contains("--nautilus-add"));
        QVERIFY(readFile(script).contains(QByteArray("'\\''")));
#ifdef Q_OS_LINUX
        QVERIFY(QFileInfo(script).permission(QFileDevice::ExeOwner));
        QProcess validator;
        validator.start("desktop-file-validate", {startup});
        QVERIFY(validator.waitForFinished());
        QVERIFY2(validator.exitCode() == 0, validator.readAllStandardError().constData());
#endif
        QVERIFY(integration.setAutoStart(false).success);
        QVERIFY(integration.setAutoStart(false).success);
        QVERIFY(integration.setNautilusScript(false).success);
        QVERIFY(!QFileInfo::exists(startup));
        QVERIFY(!QFileInfo::exists(script));
        writeFile(startup, "user-owned content");
        QVERIFY(!integration.setAutoStart(true).success);
        QVERIFY(!integration.setAutoStart(false).success);
        QCOMPARE(readFile(startup), QByteArray("user-owned content"));
        const auto blocked = dir.path() + "/blocked";
        writeFile(blocked, "file, not a directory");
        LinuxIntegration invalid({blocked, blocked, paths.executable});
        QVERIFY(!invalid.setAutoStart(true).success);
        QVERIFY(!invalid.setNautilusScript(true).success);
        QCOMPARE(readFile(blocked), QByteArray("file, not a directory"));
        LinuxIntegration newline({paths.configHome, paths.dataHome, paths.executable + '\n'});
        QVERIFY(!newline.setAutoStart(true).success);
    }
    void nautilusSelectionPreservesPaths()
    {
        TestDirectory dir;
        const auto first = dir.path() + QStringLiteral("/中文 空格'100%.txt");
        const auto second = dir.path() + "/directory";
        writeFile(first, "first");
        writeFile(second + "/nested.txt", "second");
        const auto uris = QUrl::fromLocalFile(first).toEncoded() + '\n' + QUrl::fromLocalFile(second).toEncoded() + '\n';
        QStringList paths;
        QVERIFY(LinuxIntegration::nautilusSelection(uris, paths).success);
        QCOMPARE(paths, QStringList({first, second}));
        auto localAuthority = QUrl::fromLocalFile(first);
        localAuthority.setHost("localhost");
        QVERIFY(LinuxIntegration::nautilusSelection(uris + localAuthority.toEncoded() + '\n', paths).success);
        QCOMPARE(paths, QStringList({first, second}));
        TestBackupService service(pathsIn(dir));
        for (const auto &path : paths)
            QVERIFY(service.addLocal(path).success);
        QCOMPARE(service.trackedItems().size(), 2);
        QCOMPARE(readFile(first), QByteArray("first"));
        QCOMPARE(readFile(second + "/nested.txt"), QByteArray("second"));
        QVERIFY(!LinuxIntegration::nautilusSelection(uris + "sftp://host/private", paths).success);
        QVERIFY(paths.isEmpty());
        QVERIFY(!LinuxIntegration::nautilusSelection("file://server/share", paths).success);
        QVERIFY(!LinuxIntegration::nautilusSelection("file://user@localhost/private", paths).success);
        QVERIFY(!LinuxIntegration::nautilusSelection("file://localhost:22/private", paths).success);
        QVERIFY(!LinuxIntegration::nautilusSelection(QUrl::fromLocalFile(first).toEncoded() + "?query=1", paths).success);
        QVERIFY(!LinuxIntegration::nautilusSelection("file:///tmp/%00", paths).success);
        QVERIFY(!LinuxIntegration::nautilusSelection({}, paths).success);
#ifdef Q_OS_LINUX
        const auto newline = dir.path() + "/name\nwith-newline";
        QVERIFY(LinuxIntegration::nautilusSelection(QUrl::fromLocalFile(newline).toEncoded(), paths).success);
        QCOMPARE(paths, QStringList({newline}));
#endif
    }
    void linuxSystemSettingsKeepFailedState()
    {
#ifdef Q_OS_LINUX
        TestDirectory dir;
        FakeAi ai;
        LinuxIntegrationPaths system{dir.path() + "/config", dir.path() + "/data", dir.path() + "/app"};
        SettingsService settings(pathsIn(dir), &ai, nullptr, system);
        QVERIFY(settings.setSystemOption("AutoStart", true).success);
        QVERIFY(settings.value("AutoStart", false).toBool());
        writeFile(system.configHome + "/autostart/com.zc.versionbox.desktop", "user replacement");
        QVERIFY(!settings.setSystemOption("AutoStart", false).success);
        QVERIFY(settings.value("AutoStart", false).toBool());
        writeFile(system.dataHome, "blocked");
        QVERIFY(!settings.setSystemOption("RightClickMenu", true).success);
        QVERIFY(!settings.value("RightClickMenu", false).toBool());
#else
        QSKIP("Linux settings adapter");
#endif
    }
    void closingWithoutTrayExitsWindow()
    {
        if (QSystemTrayIcon::isSystemTrayAvailable())
            QSKIP("This scenario requires a desktop without a tray");
        class QuitFilter : public QObject
        {
            bool eventFilter(QObject *, QEvent *event) override { return event->type() == QEvent::Quit; }
        } filter;
        qApp->installEventFilter(&filter);
        TestDirectory dir;
        FakeAi ai;
        TestBackupService service(pathsIn(dir));
        SettingsService settings(pathsIn(dir), &ai);
        MainWindow window(&service, &settings, &ai, m_theme, true);
        window.show();
        QTRY_VERIFY(window.isVisible());
        QVERIFY(window.close());
        QVERIFY(!window.isVisible());
        QCoreApplication::processEvents();
    }
    void platformRuntimeSmoke()
    {
        const auto expected = qEnvironmentVariable("ZCVERSIONBOX_EXPECTED_QPA");
        if (!expected.isEmpty())
            QCOMPARE(QGuiApplication::platformName(), expected);
        QVERIFY(QSslSocket::supportsSsl());
        QVERIFY(QImageReader::supportedImageFormats().contains("svg"));
        const auto expectedIcon = qEnvironmentVariable("ZCVERSIONBOX_EXPECTED_ICON");
        if (!expectedIcon.isEmpty())
        {
            const auto originalTheme = QIcon::themeName();
            const auto originalPaths = QIcon::themeSearchPaths();
            auto paths = originalPaths;
            const QStringList standardDirs = {
                QStringLiteral("/usr/share/icons"),
                QStringLiteral("/usr/local/share/icons"),
                QDir::homePath() + QStringLiteral("/.local/share/icons")
            };
            for (const auto &dir : standardDirs)
            {
                if (!paths.contains(dir) && QDir(dir).exists())
                    paths.prepend(dir);
            }
            const auto xdgDataDirs = qEnvironmentVariable("XDG_DATA_DIRS");
            if (!xdgDataDirs.isEmpty())
            {
                for (const auto &rawDir : xdgDataDirs.split(u':', Qt::SkipEmptyParts))
                {
                    const auto iconDir = rawDir + QStringLiteral("/icons");
                    if (!paths.contains(iconDir) && QDir(iconDir).exists())
                        paths.prepend(iconDir);
                }
            }
            for (const auto &dataDir : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation))
            {
                const auto iconDir = dataDir + QStringLiteral("/icons");
                if (!paths.contains(iconDir) && QDir(iconDir).exists())
                    paths.prepend(iconDir);
            }
            QIcon::setThemeSearchPaths(paths);
            QIcon::setThemeName("hicolor");
            const auto desktopIcon = QIcon::fromTheme(expectedIcon).pixmap(64);
            QIcon::setThemeName(originalTheme);
            QIcon::setThemeSearchPaths(originalPaths);
            QVERIFY2(!desktopIcon.isNull(), "Installed desktop icon must resolve through the system theme");
        }
        TestDirectory dir;
        FakeAi ai;
        TestBackupService service(pathsIn(dir));
        SettingsService settings(pathsIn(dir), &ai);
        MainWindow window(&service, &settings, &ai, m_theme, false);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const auto screenshot = window.grab();
        QVERIFY(!screenshot.isNull());
        const auto output = qEnvironmentVariable("ZCVERSIONBOX_SMOKE_OUTPUT");
        if (!output.isEmpty())
            QVERIFY(screenshot.save(output));
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
        const bool nativeFonts = QGuiApplication::platformName() != "minimal";
        if (nativeFonts)
        {
            for (const auto &run : layout.glyphRuns())
            {
                const auto rawFont = run.rawFont();
                if (rawFont.isValid())
                    faces.append(rawFont.familyName());
            }
        }
        if (nativeFonts)
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
        monitor.start();
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
        settle(monitor, service);
        QCOMPARE(runGit(service.repoPath(encoded(source)), {"rev-list", "--count", "HEAD"}).output.trimmed(), QString("2"));
        monitor.scanNow();
        settle(monitor, service);
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
        monitor.start();
        writeFile(source + "/build/output.bin", "changed build only");
        monitor.scanNow();
        settle(monitor, service);
        QCOMPARE(head(service, id), initial);
        QCOMPARE(readFile(repo + "/project/build/output.bin"), QByteArray("initial build"));
        writeFile(source + "/visible.txt", "two, normal change");
        monitor.scanNow();
        settle(monitor, service);
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
            QCOMPARE(qvariant_cast<Route>(routes.last().first()).page, PageId::Preview);
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
    void aiProviderChannels()
    {
        QCOMPARE(AiConfigHelper::serviceTypeForProvider("OpenAI"), AiProvider::OpenAI);
        QCOMPARE(AiConfigHelper::serviceTypeForProvider("DeepSeek"), AiProvider::DeepSeek);
        QCOMPARE(AiConfigHelper::serviceTypeForProvider("qwen"), AiProvider::Qwen);
        QCOMPARE(AiConfigHelper::serviceTypeForProvider("Kimi"), AiProvider::Moonshot);
        QCOMPARE(AiConfigHelper::serviceTypeForProvider("glm"), AiProvider::Zhipu);
        QCOMPARE(AiConfigHelper::serviceTypeForProvider("Doubao"), AiProvider::Doubao);
        QCOMPARE(AiConfigHelper::serviceTypeForProvider("SiliconFlow"), AiProvider::SiliconFlow);
        QCOMPARE(AiConfigHelper::serviceTypeForProvider("claude"), AiProvider::Anthropic);
        QCOMPARE(AiConfigHelper::serviceTypeForProvider("Gemini"), AiProvider::Gemini);
        QCOMPARE(AiConfigHelper::serviceTypeForProvider("Grok"), AiProvider::Grok);
        QCOMPARE(AiConfigHelper::serviceTypeForProvider("Custom"), AiProvider::Custom);

        TestDirectory dir;
        const auto paths = pathsIn(dir);
        FakeAi gateway;
        SettingsService settings(paths, &gateway);
        settings.selectProvider("GLM");
        settings.saveField("ApiKey", "fixture-glm");
        QVERIFY(settings.config("GLM").modelList.isEmpty());
        settings.fetchModels();
        QCOMPARE(gateway.models.size(), 1);
        gateway.models.last()({"glm-4.7", "glm-5.3"}, {});
        QCOMPARE(settings.config("GLM").modelName, QString("glm-4.7"));
        settings.selectProvider("Claude");
        settings.saveField("ApiKey", "fixture-claude");
        settings.fetchModels();
        QCOMPARE(gateway.models.size(), 2);
        gateway.models.last()({"claude-sonnet-4-5", "claude-opus-4-6"}, {});
        QCOMPARE(settings.config("Claude").modelName, QString("claude-sonnet-4-5"));
        settings.selectProvider("GLM");
        QCOMPARE(settings.config("GLM").modelName, QString("glm-4.7"));
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
        settle(service);
        auto *tabs = window.findChild<QWidget *>("tabs");
        QVERIFY(tabs->isVisible());
        auto *returnHistory = window.findChild<QToolButton *>("returnHistoryButton");
        QVERIFY(returnHistory);
        QVERIFY(returnHistory->isVisible());
        QVERIFY(!window.findChild<QToolButton *>("historyTab")->isVisible());
        returnHistory->click();
        settle(service);
        QCOMPARE(pages->currentWidget(), history);
        QVERIFY(tabs->isVisible());
        QVERIFY(!returnHistory->isVisible());
        QVERIFY(window.findChild<QToolButton *>("historyTab")->isVisible());
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
        HomePageCollaborationPage collaboration(&service);
        collaboration.setBackup(id);
        auto *toggle = collaboration.findChild<oclero::qlementine::Switch *>("automaticSyncSwitch");
        QVERIFY(toggle && !toggle->isChecked());
        const auto other = dir.path() + "/other.txt";
        writeFile(other, "other"); QVERIFY(service.addLocal(other).success);
        const auto otherId = encoded(other);
        QVERIFY(service.setRemote(id, "https://example.test/first.git").success);
        QVERIFY(service.setRemote(otherId, "https://example.test/second.git").success);
        auto *url = collaboration.findChild<QLineEdit *>("remoteUrl");
        url->setFocus(); collaboration.setBackup(otherId); settle(service);
        QCOMPARE(url->text(), QString("https://example.test/second.git"));
        QVERIFY(toggle->isChecked());
        toggle->click(); settle(service);
        QVERIFY(!toggle->isChecked()); QVERIFY(!service.syncStatus(otherId).automatic);
        QCOMPARE(runGit(service.repoPath(otherId), {"remote", "get-url", "origin"}).output.trimmed(), QString("https://example.test/second.git"));
        toggle->click(); settle(service); QVERIFY(toggle->isChecked());
    }
    void importantVersionUiFlow()
    {
        TestDirectory dir; TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/方案.md", other = dir.path() + "/其他.md";
        writeFile(source, "first\n"); QVERIFY(service.addLocal(source).success);
        const auto id = encoded(source), first = head(service, id);
        writeFile(source, "second\n"); QVERIFY(service.backup(id).success);
        const auto latest = head(service, id);
        writeFile(other, "other\n"); QVERIFY(service.addLocal(other).success);
        HomePageBackupPage page(&service); page.resize(820, 580); page.setBackup(id); settle(service); page.show();
        auto *table = page.findChild<QTableView *>("table");
        auto *filter = page.findChild<QAbstractButton *>("milestoneOnlySwitch"); QVERIFY(filter);
        const auto openTagManager = [&](int row) -> QDialog *
        {
            QTest::mouseClick(table->viewport(), Qt::LeftButton, {}, historyActionPoint(table, row, 2));
            QCoreApplication::processEvents();
            return page.findChild<QDialog *>("versionTagDialog");
        };
        QPointer<QDialog> dialog = openTagManager(1); QVERIFY(dialog);
        auto *name = dialog->findChild<QLineEdit *>("versionTagName");
        auto *buttons = dialog->findChild<QDialogButtonBox *>();
        name->setText("bad name"); QTest::mouseClick(buttons->button(QDialogButtonBox::Save), Qt::LeftButton); settle(service);
        QVERIFY(dialog->findChild<QLabel *>("versionTagError")->isVisible());
        QCOMPARE(table->model()->rowCount(), 2);
        name->setText("交稿版"); QTest::keyClick(name, Qt::Key_Return); settle(service);
        QTRY_VERIFY(!dialog || !dialog->isVisible());
        QTRY_COMPARE(table->model()->index(1, 0).data(Qt::UserRole + 2).value<QVector<VersionTag>>().size(), 1);
        QCOMPARE(table->model()->index(1, 0).data(Qt::UserRole + 2).value<QVector<VersionTag>>().first().name, QString("交稿版"));
        QCOMPARE(table->model()->index(1, 0).data(Qt::UserRole + 1).toString(), first);
        QVERIFY(!page.findChild<QAction *>("editMessageAction")->isEnabled());
        QVERIFY(!(table->model()->flags(table->model()->index(1, 0)) & Qt::ItemIsEditable));
        filter->setChecked(true);
        QVERIFY(table->isRowHidden(0)); QVERIFY(!table->isRowHidden(1));
        page.setBackup(encoded(other)); settle(service); QVERIFY(!filter->isChecked());
        page.setBackup(id); settle(service); QVERIFY(filter->isChecked()); QVERIFY(table->isRowHidden(0));
        dialog = openTagManager(1); QVERIFY(dialog);
        name = dialog->findChild<QLineEdit *>("versionTagName"); name->setText("定稿");
        QTest::keyClick(name, Qt::Key_Return); settle(service); QTRY_VERIFY(!dialog || !dialog->isVisible());
        QTRY_COMPARE(table->model()->index(1, 0).data(Qt::UserRole + 2).value<QVector<VersionTag>>().size(), 1);
        QTRY_COMPARE(table->model()->index(1, 0).data(Qt::UserRole + 2).value<QVector<VersionTag>>().first().name, QString("定稿"));
        dialog = openTagManager(1); QVERIFY(dialog);
        QTest::mouseClick(dialog->findChild<QPushButton *>("removeVersionTagButton"), Qt::LeftButton); settle(service);
        QTRY_VERIFY(!dialog || !dialog->isVisible());
        QTRY_VERIFY(!table->isVisible()); QVERIFY(page.findChild<QLabel *>("emptyLabel")->text().contains("还没有里程碑版本"));
        filter->setChecked(false); QVERIFY(table->isVisible()); QCOMPARE(table->model()->rowCount(), 2);
        dialog = openTagManager(0); QVERIFY(dialog);
        dialog->findChild<QLineEdit *>("versionTagName")->setText("旧弹窗");
        page.setBackup(encoded(other)); settle(service); QTRY_VERIFY(!dialog || !dialog->isVisible());
        page.setBackup(id); settle(service);
        QVERIFY(table->model()->index(0, 0).data(Qt::UserRole + 2).value<QVector<VersionTag>>().isEmpty());
        QCOMPARE(head(service, id), latest); QCOMPARE(readFile(source), QByteArray("second\n"));
        page.hide();
    }
    void historyRequestedVersionOverridesMilestoneFilter()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/方案.md";
        writeFile(source, "first\n");
        QVERIFY(service.addLocal(source).success);
        const auto id = encoded(source), first = head(service, id);
        QVERIFY(service.createTag(service.tagRequest(id, first, "交稿版")).success);
        writeFile(source, "second\n");
        QVERIFY(service.backup(id).success);
        const auto latest = head(service, id);
        HomePageBackupPage page(&service);
        page.setBackup(id);
        settle(service);
        auto *table = page.findChild<QTableView *>("table");
        auto *filter = page.findChild<QAbstractButton *>("milestoneOnlySwitch");
        QVERIFY(table);
        QVERIFY(filter);
        filter->setChecked(true);
        page.setBackup(id, first);
        settle(service);
        QVERIFY(filter->isChecked());
        QCOMPARE(table->currentIndex().data(Qt::UserRole + 1).toString(), first);
        page.setBackup(id, latest);
        settle(service);
        QVERIFY(!filter->isChecked());
        QCOMPARE(table->currentIndex().data(Qt::UserRole + 1).toString(), latest);
        QVERIFY(!table->isRowHidden(table->currentIndex().row()));
    }
    void renderImportantVersions()
    {
        TestDirectory dir; TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/项目方案.md";
        writeFile(source, "first\n"); QVERIFY(service.addLocal(source).success);
        const auto id = encoded(source), first = head(service, id);
        QVERIFY(service.createTag(service.tagRequest(id, first, "初稿")).success);
        for (int i = 0; i < 5; ++i)
        {
            writeFile(source, QString("方案内容 %1\n").arg(i).toUtf8()); QVERIFY(service.backup(id).success);
            QVERIFY(service.editMessage(id, head(service, id), QString("补充项目说明与计划安排 %1").arg(i + 1)).success);
            if (i == 2) QVERIFY(service.createTag(service.tagRequest(id, head(service, id), "交稿版")).success);
        }
        QVERIFY(service.createTag(service.tagRequest(id, head(service, id), "修改前的里程碑名称很长也不会挤占其他操作")).success);
        FakeAi gateway; SettingsService settings(pathsIn(dir), &gateway);
        MainWindow window(&service, &settings, &gateway, m_theme, false);
        window.setAttribute(Qt::WA_DontShowOnScreen); window.show(); window.navigate({PageId::History, id}); settle(service);
        auto *page = window.findChild<HomePageBackupPage *>();
        auto *filter = page->findChild<QAbstractButton *>("milestoneOnlySwitch");
        auto *table = page->findChild<QTableView *>("table");
        const auto output = qEnvironmentVariable("ZC_TEST_SCREENSHOTS"); if (!output.isEmpty()) QDir().mkpath(output);
        for (int theme = 0; theme < 2; ++theme)
        {
            for (const auto size : {QSize(1080, 740), QSize(760, 520)})
            {
                window.resize(size); filter->setChecked(false); QTest::qWait(120);
                QCOMPARE(window.size(), size);
                QVERIFY(table->columnWidth(0) > 80);
                QVERIFY(!table->horizontalScrollBar()->isVisible());
                const auto stem = QString("/important-%1-%2").arg(theme).arg(size.width());
                if (!output.isEmpty()) QVERIFY(window.grab().save(output + stem + ".png"));
                filter->setChecked(true); QTest::qWait(50);
                int visible = 0; for (int row = 0; row < table->model()->rowCount(); ++row) visible += !table->isRowHidden(row);
                QCOMPARE(visible, 3);
                if (!output.isEmpty()) QVERIFY(window.grab().save(output + stem + "-filtered.png"));
                QTest::mouseClick(table->viewport(), Qt::LeftButton, {}, historyActionPoint(table, 0, 2));
                QCoreApplication::processEvents();
                auto *dialog = page->findChild<QDialog *>("versionTagDialog"); QVERIFY(dialog);
                QTest::qWait(100);
                if (!output.isEmpty()) QVERIFY(dialog->grab().save(output + stem + "-dialog.png"));
                dialog->reject(); settle(service);
            }
            m_theme->toggle();
        }
        window.hide();
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
#ifdef Q_OS_MACOS
        if (QGuiApplication::platformName() == "offscreen")
            QSKIP("The macOS offscreen plugin does not deliver keyboard focus after closing a popup");
#endif
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
        table->setCurrentIndex(table->model()->index(0, 0));
        QTest::keyClick(table, Qt::Key_Menu);
        QTRY_VERIFY(QApplication::activePopupWidget());
        menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        QVERIFY(menu);
        menu->setActiveAction(menu->findChild<QAction *>("editMessageActionMenu"));
        QTest::keyPress(menu, Qt::Key_Return);
        QPointer<QLineEdit> editor = table->findChild<QLineEdit *>();
        QVERIFY(editor);
        QCOMPARE(table->currentIndex().data(Qt::UserRole + 1).toString(), latest);
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
    void directHistoryEditAndRestore_data()
    {
        QTest::addColumn<bool>("acceptRestore");
        QTest::addColumn<bool>("changeContext");
        QTest::newRow("cancel") << false << false;
        QTest::newRow("restore-clicked-version") << true << false;
        QTest::newRow("reject-stale-confirmation") << true << true;
    }
    void directHistoryEditAndRestore()
    {
        QFETCH(bool, acceptRestore);
        QFETCH(bool, changeContext);
        TestDirectory dir; TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/inline.txt", other = dir.path() + "/other.txt";
        writeFile(source, "base\n"); QVERIFY(service.addLocal(source).success);
        const auto id = service.idForSource(source), base = head(service, id);
        writeFile(source, "latest\n"); QVERIFY(service.backup(id).success);
        writeFile(other, "other\n"); QVERIFY(service.addLocal(other).success);
        HomePageBackupPage page(&service); page.resize(820, 580); page.show();
        page.setBackup(id); settle(service);
        page.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        auto *table = page.findChild<QTableView *>("table"); QVERIFY(table);
        QSignalSpy routes(&page, &HomePageBackupPage::navigate);
        // An older row cannot open an editor, including through the native edit key.
        QTest::mouseClick(table->viewport(), Qt::LeftButton, {}, historyActionPoint(table, 1, 4));
        QVERIFY(!table->findChild<QLineEdit *>());
        QVERIFY(!page.findChild<QAction *>("editMessageAction")->isEnabled());
        QVERIFY(!(table->model()->flags(table->model()->index(1, 0)) & Qt::ItemIsEditable));
        QTest::keyClick(table, Qt::Key_F2);
        QVERIFY(!table->findChild<QLineEdit *>());
        QTest::mouseClick(table->viewport(), Qt::LeftButton, {}, historyActionPoint(table, 0, 4));
        QPointer<QLineEdit> editor = table->findChild<QLineEdit *>(); QVERIFY(editor);
        editor->selectAll(); QTest::keyClicks(editor, "Edited latest description");
        QTest::keyClick(editor, Qt::Key_Return);
        settle(service); QTRY_VERIFY(!editor);
        QCOMPARE(table->model()->index(0, 0).data().toString(), QString("Edited latest description"));
        QCOMPARE(readFile(source), QByteArray("latest\n"));
        const auto latest = head(service, id);
        QVERIFY(service.createTag(service.tagRequest(id, latest, "定稿")).success);
        settle(service);
        QTest::mouseClick(table->viewport(), Qt::LeftButton, {}, historyActionPoint(table, 0, 4));
        QVERIFY(!table->findChild<QLineEdit *>());
        QVERIFY(!page.findChild<QAction *>("editMessageAction")->isEnabled());
        const auto output = qEnvironmentVariable("ZC_TEST_SCREENSHOTS");
        if (!output.isEmpty())
        {
            QDir().mkpath(output);
            for (int theme = 0; theme < 2; ++theme)
            {
                for (int width : {820, 520})
                {
                    page.resize(width, 400); QTest::qWait(40);
                    hoverHistoryAction(table, 0, 2);
                    QVERIFY(table->viewport()->rect().contains(historyActionPoint(table, 0, 5)));
                    QCOMPARE(table->horizontalScrollBar()->maximum(), 0);
                    QVERIFY(page.grab().save(output + QString("/inline-marked-%1-%2.png").arg(theme).arg(width)));
                }
                m_theme->toggle();
            }
        }
        page.resize(520, 400); QTest::qWait(20);
        QVERIFY(table->viewport()->rect().contains(historyActionPoint(table, 1, 5)));
        bool shown = false, defaultCancel = false, rightVersion = false;
        QTimer responder; responder.setInterval(10);
        connect(&responder, &QTimer::timeout, &page, [&] {
            auto *dialog = page.findChild<QDialog *>("confirmDialog");
            if (!dialog || !dialog->isVisible()) return;
            responder.stop(); shown = true;
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            defaultCancel = buttons && buttons->button(QDialogButtonBox::Cancel)->isDefault();
            for (auto *label : dialog->findChildren<QLabel *>())
                rightVersion |= label->text().contains(base.left(8));
            // A later selection must never change the requested restore revision.
            table->setCurrentIndex(table->model()->index(0, 0));
            if (changeContext) { page.setBackup(service.idForSource(other)); settle(service); }
            if (acceptRestore) dialog->accept(); else dialog->reject();
        });
        QTimer watchdog; watchdog.setSingleShot(true);
        connect(&watchdog, &QTimer::timeout, &page, [&] {
            if (auto *dialog = page.findChild<QDialog *>("confirmDialog")) dialog->reject();
        });
        responder.start(); watchdog.start(5000);
        QTest::mouseClick(table->viewport(), Qt::LeftButton, {}, historyActionPoint(table, 1, 5));
        QTRY_VERIFY_WITH_TIMEOUT(shown, 5000);
        watchdog.stop(); responder.stop(); settle(service);
        QVERIFY(defaultCancel); QVERIFY(rightVersion);
        QCOMPARE(routes.count(), 0);
        QCOMPARE(readFile(source), acceptRestore && !changeContext ? QByteArray("base\n") : QByteArray("latest\n"));
        QCOMPARE(readFile(other), QByteArray("other\n"));
        QCOMPARE(head(service, id), latest);
    }
    void conflictChoicesPreviewAndApply()
    {
        ResolutionFixture f;
        auto session = f.service.prepareSyncResolution(f.id);
        QVERIFY2(session.result.success, qPrintable(session.result.message));
        QCOMPARE(session.value.total(), 4);
        HomePageConflictPage page(&f.service);
        page.setAttribute(Qt::WA_DontShowOnScreen);
        page.resize(900, 600);
        page.setBackup(f.id);
        page.show();
        settle(f.service);
        auto *files = page.findChild<QListView *>("files");
        const auto select = [&](const QString &path)
        {
            for (int i = 0; i < files->model()->rowCount(); ++i)
                if (files->model()->index(i, 0).data().toString().contains(path))
                    files->setCurrentIndex(files->model()->index(i, 0));
            settle(f.service);
        };
        auto *local = page.findChild<QPushButton *>("localChoice");
        auto *remote = page.findChild<QPushButton *>("remoteChoice");
        auto *preview = page.findChild<QPushButton *>("previewButton");
        QVERIFY(!preview->isEnabled());
        QVERIFY(!local->isChecked());
        QVERIFY(!remote->isChecked());
        select("project/使用说明.txt");
        QVERIFY(page.findChild<QWidget *>("diffView")->findChild<QTextBrowser *>()->toPlainText().contains("本地第一段"));
        QTest::keyClick(local, Qt::Key_Space);
        settle(f.service);
        QVERIFY(local->isChecked());
        QVERIFY(page.findChild<QLabel *>("positionLabel")->text().contains("第 1 / 2"));
        remote->click(); settle(f.service);
        QVERIFY(remote->isChecked()); QVERIFY(!local->isChecked());
        local->click(); settle(f.service);
        page.findChild<QPushButton *>("nextButton")->click();
        QVERIFY(page.findChild<QLabel *>("positionLabel")->text().contains("第 2 / 2"));
        remote->click(); settle(f.service);
        page.findChild<QPushButton *>("previousButton")->click();
        QVERIFY(local->isChecked());
        select("project/image.bin");
        QVERIFY(local->text().contains("本地文件"));
        local->click(); settle(f.service);
        select("project/remove.txt");
        QVERIFY(remote->text().contains("删除文件"));
        remote->click(); settle(f.service);
        QVERIFY(preview->isEnabled());
        preview->click(); settle(f.service);
        QVERIFY(page.findChild<QPushButton *>("applyButton")->isVisible());
        QVERIFY(page.findChild<QLabel *>("previewSummary")->text().contains("删除 1"));
        QVERIFY(!page.findChild<QPushButton *>("resultOpen"));
        bool automatic = false;
        for (int i = 0; i < files->model()->rowCount(); ++i)
            if (files->model()->index(i, 0).data().toString().contains("automatic.txt")) automatic = true;
        QVERIFY(automatic);
        // Every final text shown to the user is reconstructed content, never a patch.
        for (int i = 0; i < files->model()->rowCount(); ++i)
        {
            files->setCurrentIndex(files->model()->index(i, 0)); settle(f.service);
            QVERIFY(!page.findChild<QWidget *>("previewDiffView")->findChild<QTextBrowser *>()->toPlainText().contains("<<<<<<<"));
        }
        auto *apply = page.findChild<QPushButton *>("applyButton");
        bool defaultCancel = false, correctScope = false;
        QVERIFY(answerConfirmation(apply, f.service, [&](QDialog *dialog)
        {
            defaultCancel = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel)->isDefault();
            correctScope = dialog->findChild<QLabel *>()->text().contains(QDir::toNativeSeparators(f.source)) && dialog->findChild<QLabel *>()->text().contains("1 个删除");
            dialog->reject();
        }));
        QVERIFY(defaultCancel); QVERIFY(correctScope);
        QCOMPARE(f.service.syncState(f.id), BackupSyncState::ResolutionPending);
        QVERIFY(QFileInfo::exists(f.source + "/remove.txt"));
        QSignalSpy navigation(&page, &HomePageConflictPage::navigate);
        QVERIFY(answerConfirmation(apply, f.service, [](QDialog *dialog) { dialog->accept(); }));
        QCOMPARE(f.service.syncState(f.id), BackupSyncState::Tracking);
        QCOMPARE(readFile(f.source + "/使用说明.txt"), QByteArray("本地第一段\nb\nc\nd\ne\nf\n云端第二段\n"));
        QCOMPARE(readFile(f.source + "/image.bin"), QByteArray("local\0image", 11));
        QCOMPARE(readFile(f.source + "/automatic.txt"), QByteArray("自动合并的新增文件\n"));
        QVERIFY(!QFileInfo::exists(f.source + "/remove.txt"));
        QCOMPARE(navigation.size(), 1);
        const auto route = qvariant_cast<Route>(navigation.first().first());
        QCOMPARE(route.page, PageId::Collaboration);
        QCOMPARE(route.commit, head(f.service, f.id));
    }
    void conflictNavigationStalenessAndLateReplies()
    {
        ResolutionFixture f;
        FakeAi gateway;
        SettingsService settings(pathsIn(f.dir), &gateway);
        MainWindow window(&f.service, &settings, &gateway, m_theme, false);
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.show();
        window.navigate({PageId::Collaboration, f.id}); settle(f.service);
        auto *collaboration = window.findChild<HomePageCollaborationPage *>();
        collaboration->findChild<QPushButton *>("syncButton")->click(); settle(f.service);
        auto *page = window.findChild<HomePageConflictPage *>();
        QVERIFY(page->isVisible());
        auto *returnHistory = window.findChild<QToolButton *>("returnHistoryButton");
        QVERIFY(returnHistory && returnHistory->isVisible());
        QCOMPARE(returnHistory->text(), QString("返回多人协作"));
        QVERIFY(!window.findChild<QToolButton *>("historyTab")->isVisible());
        QVERIFY(!window.findChild<QToolButton *>("overviewTab")->isVisible());
        auto *local = page->findChild<QPushButton *>("localChoice");
        local->click(); settle(f.service);
        const auto firstTitle = page->findChild<QLabel *>("positionLabel")->text();
        window.navigate({PageId::GeneralSettings}); settle(f.service);
        window.findChild<QToolButton *>("returnApplicationButton")->click(); settle(f.service);
        QVERIFY(page->isVisible()); QVERIFY(local->isChecked());
        QCOMPARE(page->findChild<QLabel *>("positionLabel")->text(), firstTitle);
        window.findChild<QToolButton *>("backButton")->click(); settle(f.service);
        QVERIFY(window.findChild<SettingPage *>()->isVisible());
        window.findChild<QToolButton *>("forwardButton")->click(); settle(f.service);
        QVERIFY(page->isVisible());
        const auto other = f.dir.path() + "/other.txt";
        writeFile(other, "other"); QVERIFY(f.service.addLocal(other).success);
        const auto otherId = f.service.idForSource(other);
        // Leaving while saving retains the choice but cannot navigate back or fill another object.
        page->findChild<QPushButton *>("remoteChoice")->click();
        window.navigate({PageId::History, otherId}); settle(f.service);
        QVERIFY(!page->isVisible());
        window.navigate({PageId::Conflict, f.id}); settle(f.service);
        QVERIFY(page->findChild<QPushButton *>("remoteChoice")->isChecked());
        writeFile(f.source + "/使用说明.txt", "源内容在选择期间变化\n");
        window.navigate({PageId::Collaboration, f.id}); settle(f.service);
        auto *resume = collaboration->findChild<QPushButton *>("syncButton");
        QVERIFY(resume->isVisible()); resume->click(); settle(f.service);
        QVERIFY(page->findChild<QPushButton *>("reanalyzeButton")->isVisible());
        QVERIFY(!local->isEnabled());
        QVERIFY(!page->findChild<QPushButton *>("previewButton")->isEnabled());
        QVERIFY(page->findChild<QPushButton *>("remoteChoice")->isChecked());
        QVERIFY(answerConfirmation(page->findChild<QPushButton *>("reanalyzeButton"), f.service, [](QDialog *dialog) { dialog->accept(); }));
        QVERIFY(local->isEnabled()); QVERIFY(!local->isChecked());
        QVERIFY(!page->findChild<QPushButton *>("remoteChoice")->isChecked());
        // Removed objects invalidate a queued content reply and all navigation entries.
        auto *files = page->findChild<QListView *>("files");
        for (int i = 0; i < files->model()->rowCount(); ++i)
            if (files->model()->index(i, 0).data().toString().contains("image.bin"))
                files->setCurrentIndex(files->model()->index(i, 0));
        QVERIFY(f.service.removeBackup(f.id).success); settle(f.service);
        QVERIFY(window.findChild<HomePage *>()->isVisible());
        window.findChild<QToolButton *>("backButton")->click(); settle(f.service);
        QVERIFY(!page->isVisible());
        QVERIFY(f.service.contains(otherId));
    }
    void conflictNotificationBadgeAndNormalNavigation()
    {
        ResolutionFixture f;
        f.service.prepareSyncResolution(f.id);
        settle(f.service);
        QCOMPARE(f.service.syncState(f.id), BackupSyncState::ResolutionPending);

        FakeAi gateway;
        SettingsService settings(pathsIn(f.dir), &gateway);
        MainWindow window(&f.service, &settings, &gateway, m_theme, false);
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.show();
        window.navigate({});
        settle(f.service);

        // Opening an object lands on its local preview, including paused objects.
        auto *sidebar = window.findChild<QListView *>("sidebarList");
        QVERIFY(sidebar);
        auto *model = sidebar->model();
        QVERIFY(model && model->rowCount() > 0);
        const auto index = model->index(0, 0);
        QCOMPARE(index.data(BackupListModel::IdRole).toString(), f.id);

        emit sidebar->clicked(index);
        settle(f.service);

        auto *pages = window.findChild<QStackedWidget *>("pages");
        auto *historyPage = window.findChild<HomePagePreviewPage *>();
        auto *conflictPage = window.findChild<HomePageConflictPage *>();
        QCOMPARE(pages->currentWidget(), historyPage);
        QVERIFY(!conflictPage->isVisible());

        auto *overviewTab = window.findChild<QToolButton *>("collaborationTab");
        QVERIFY(overviewTab && overviewTab->isVisible());
        auto *badge = window.findChild<QWidget *>("collaborationNotificationBadge");
        QVERIFY(badge && badge->isVisible());
        QVERIFY(overviewTab->toolTip().contains("存在待处理事项"));

        // Navigating to standalone conflict page hides collaboration tab and badge
        window.navigate({PageId::Conflict, f.id});
        settle(f.service);
        QCOMPARE(pages->currentWidget(), conflictPage);
        QVERIFY(!badge->isVisible());
        QVERIFY(!overviewTab->isVisible());

        // Returning from a conflict opened outside History lands on Collaboration.
        window.findChild<QToolButton *>("returnHistoryButton")->click();
        settle(f.service);
        QCOMPARE(pages->currentWidget(), window.findChild<HomePageCollaborationPage *>());
        QVERIFY(badge->isVisible());
        QVERIFY(overviewTab->isVisible());

        // Delegate paints in hovered and selected states without losing font or pen
        auto *delegate = sidebar->itemDelegate();
        QStyleOptionViewItem option;
        option.rect = QRect(0, 0, 200, 52);
        option.state = QStyle::State_Selected | QStyle::State_MouseOver;
        QPixmap pixmap(200, 52);
        pixmap.fill(Qt::transparent);
        {
            QPainter painter(&pixmap);
            delegate->paint(&painter, option, index);
        }
        const auto image = pixmap.toImage();
        QVERIFY(!image.isNull());
        QVERIFY(image.pixelColor(100, 26).alpha() > 0);
    }
    void conflictFastForwardAndChangedConfirmation()
    {
        ResolutionFixture f(false);
        auto session = f.service.prepareSyncResolution(f.id);
        QVERIFY(session.result.success); QCOMPARE(session.value.total(), 0);
        HomePageConflictPage page(&f.service);
        page.setAttribute(Qt::WA_DontShowOnScreen); page.resize(800, 600); page.show(); page.setBackup(f.id); settle(f.service);
        auto *apply = page.findChild<QPushButton *>("applyButton");
        QVERIFY(apply->isVisible()); QVERIFY(apply->isEnabled());
        const auto previous = head(f.service, f.id);
        QVERIFY(answerConfirmation(apply, f.service, [&](QDialog *dialog)
        {
            writeFile(f.source + "/使用说明.txt", "临时新修改\n");
            dialog->accept();
        }));
        QCOMPARE(head(f.service, f.id), previous);
        QCOMPARE(readFile(f.source + "/使用说明.txt"), QByteArray("临时新修改\n"));
        QCOMPARE(f.service.syncState(f.id), BackupSyncState::ResolutionPending);
        QVERIFY(page.findChild<QPushButton *>("reanalyzeButton")->isVisible());
        QVERIFY(!apply->isEnabled());
    }
    void conflictPreviewFailurePreservesChoices_data()
    {
        QTest::addColumn<bool>("conflicts");
        QTest::newRow("manual-choices") << true;
        QTest::newRow("automatic-preview") << false;
    }
    void conflictPreviewFailurePreservesChoices()
    {
        QFETCH(bool, conflicts);
        bool failPreview = false;
        int failedAttempts = 0;
        BackupDependencies deps;
        deps.git = [&](const QString &path, const QStringList &args, const GitOptions &options)
        {
            if (failPreview && args.contains("write-tree"))
            {
                ++failedAttempts;
                GitResult result; result.started = result.finished = true; result.exitCode = 1;
                result.error = "模拟临时预览失败"; return result;
            }
            return runGit(path, args, options);
        };
        ResolutionFixture f(conflicts, deps);
        auto session = f.service.prepareSyncResolution(f.id); QVERIFY(session.result.success);
        const auto files = session.value.files;
        for (const auto &file : files)
            for (int h = 0; h < file.hunks.size(); ++h)
            {
                session = f.service.chooseSyncResolution(session.value, file.path, h, ConflictChoice::Local);
                QVERIFY(session.result.success);
            }
        HomePageConflictPage page(&f.service);
        page.setAttribute(Qt::WA_DontShowOnScreen); page.show();
        failPreview = true;
        page.setBackup(f.id); settle(f.service);
        auto *preview = page.findChild<QPushButton *>("previewButton");
        if (conflicts) { preview->click(); settle(f.service); }
        QCOMPARE(failedAttempts, 1);
        QVERIFY(!page.findChild<QPushButton *>("reanalyzeButton")->isVisible());
        QVERIFY(preview->isEnabled());
        QVERIFY(conflicts ? !preview->isVisible() : preview->isVisible());
        const auto resumed = f.service.syncResolution(f.id);
        QVERIFY(resumed.result.success); QCOMPARE(resumed.value.revision, session.value.revision);
        QCOMPARE(resumed.value.remaining(), 0);
        failPreview = false;
        preview->click(); settle(f.service);
        QVERIFY(page.findChild<QPushButton *>("applyButton")->isEnabled());
        QVERIFY(page.findChild<QPushButton *>("applyButton")->isVisible());
    }
    void conflictCrlfHighlightMatchesSelectedText()
    {
        ResolutionFixture f(true, {}, true);
        QVERIFY(f.service.prepareSyncResolution(f.id).result.success);
        HomePageConflictPage page(&f.service);
        page.setAttribute(Qt::WA_DontShowOnScreen); page.show(); page.setBackup(f.id); settle(f.service);
        auto *files = page.findChild<QListView *>("files");
        for (int i = 0; i < files->model()->rowCount(); ++i)
            if (files->model()->index(i, 0).data().toString().contains("使用说明.txt"))
                files->setCurrentIndex(files->model()->index(i, 0));
        page.findChild<QPushButton *>("localChoice")->click(); settle(f.service);
        page.findChild<QPushButton *>("nextButton")->click();
        QVERIFY(page.findChild<QLabel *>("positionLabel")->text().contains("第 2 / 2"));
        const auto shown = page.findChild<QWidget *>("diffView")->findChild<QTextBrowser *>()->toPlainText();
        QVERIFY(shown.contains("本地第二段")); QVERIFY(shown.contains("云端第二段"));
        QVERIFY(!shown.contains('\r'));
    }
    void conflictKeyboardNavigation()
    {
        // Exercise full Tab traversal without changing the user's macOS setting.
        auto *hints = QGuiApplication::styleHints();
        const auto previousTabBehavior = hints->tabFocusBehavior();
        hints->setTabFocusBehavior(Qt::TabFocusAllControls);
        const auto restoreTabBehavior = qScopeGuard([=] { hints->setTabFocusBehavior(previousTabBehavior); });
        ResolutionFixture f;
        QVERIFY(f.service.prepareSyncResolution(f.id).result.success);
        HomePageConflictPage page(&f.service);
        page.resize(520, 540); page.show(); page.activateWindow();
        page.setBackup(f.id); settle(f.service);
        auto *local = page.findChild<QPushButton *>("localChoice");
        local->setFocus(Qt::TabFocusReason);
        QTRY_VERIFY(local->hasFocus());
        QTest::keyClick(local, Qt::Key_Space); settle(f.service);
        QVERIFY(local->isChecked());
        local->setFocus(Qt::TabFocusReason);
        QTest::keyClick(local, Qt::Key_Tab);
        auto *remote = page.findChild<QPushButton *>("remoteChoice");
        QVERIFY(remote->hasFocus());
        QTest::keyClick(remote, Qt::Key_Space); settle(f.service);
        QVERIFY(remote->isChecked()); QVERIFY(!local->isChecked());
        page.close();
    }
    void renderConflictPages()
    {
        ResolutionFixture f;
        QVERIFY(f.service.prepareSyncResolution(f.id).result.success);
        FakeAi gateway;
        SettingsService settings(pathsIn(f.dir), &gateway);
        MainWindow window(&f.service, &settings, &gateway, m_theme, false);
        window.setAttribute(Qt::WA_DontShowOnScreen); window.show();
        window.navigate({PageId::Conflict, f.id}); settle(f.service);
        auto *page = window.findChild<HomePageConflictPage *>();
        auto *fileList = page->findChild<QListView *>("files");
        fileList->setCurrentIndex(fileList->model()->index(2, 0));
        page->findChild<QPushButton *>("localChoice")->click(); settle(f.service);
        const auto output = qEnvironmentVariable("ZC_TEST_SCREENSHOTS");
        if (!output.isEmpty()) QDir().mkpath(output);
        for (int theme = 0; theme < 2; ++theme)
        {
            for (const auto size : {QSize(1080, 740), QSize(760, 520)})
            {
                window.resize(size); QTest::qWait(120);
                QCOMPARE(window.size(), size);
                const auto selectedRect = fileList->visualRect(fileList->currentIndex());
                QVERIFY2(selectedRect.top() >= 0 && selectedRect.bottom() < fileList->viewport()->height(),
                         qPrintable(QString("selected row y=%1..%2, viewport height=%3").arg(selectedRect.top()).arg(selectedRect.bottom()).arg(fileList->viewport()->height())));
                QCOMPARE(page->findChild<QSplitter *>("splitter")->orientation(), size.width() == 760 ? Qt::Vertical : Qt::Horizontal);
                // Without an outer scroll area every control must fit the page.
                for (const auto *name : {"remoteChoice", "nextButton", "laterButton"})
                    QVERIFY2(!page->findChild<QPushButton *>(name)->visibleRegion().isEmpty(), name);
                const auto diffHeight = page->findChild<QWidget *>("diffView")->height();
                QVERIFY2(diffHeight >= 120, qPrintable(QString("diff view height=%1 at width %2").arg(diffHeight).arg(size.width())));
                if (!output.isEmpty()) QVERIFY(window.grab().save(output + QString("/conflict-%1-%2.png").arg(theme).arg(size.width())));
            }
            m_theme->toggle();
        }
        auto session = f.service.syncResolution(f.id);
        const auto files = session.value.files;
        for (const auto &file : files)
            for (int h = 0; h < file.hunks.size(); ++h)
            {
                session = f.service.chooseSyncResolution(session.value, file.path, h, ConflictChoice::Remote);
                QVERIFY(session.result.success);
            }
        settle(f.service);
        page->findChild<QPushButton *>("previewButton")->click(); settle(f.service);
        for (int theme = 0; theme < 2; ++theme)
        {
            for (const auto size : {QSize(1080, 740), QSize(760, 520)})
            {
                window.resize(size); QTest::qWait(120);
                QCOMPARE(window.size(), size);
                QVERIFY(!page->findChild<QPushButton *>("applyButton")->visibleRegion().isEmpty());
                if (!output.isEmpty()) QVERIFY(window.grab().save(output + QString("/conflict-preview-%1-%2.png").arg(theme).arg(size.width())));
            }
            m_theme->toggle();
        }
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
            openHistoryMenu(table, row);
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
        cancelWasDefault = false;
        bool rebuildShown = false;
        QTimer responder;
        responder.setInterval(10);
        connect(&responder, &QTimer::timeout, &responder, [&]
        {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            responder.stop(); rebuildShown = true;
            cancelWasDefault = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel)->isDefault();
            dialog->reject();
        });
        responder.start();
        actions.rebuild(id);
        QTRY_VERIFY(rebuildShown);
        settle(service);
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
        items[125].state = BackupSyncState::ResolutionPending;
        items[125].stateDetail = "还有 2 / 3 处内容需要选择。自动备份已暂停。";
        source.setItems(items);
        QVERIFY(retained.isValid());
        QCOMPARE(retained.data(BackupListModel::StateRole).toInt(), int(BackupSyncState::ResolutionPending));
        QCOMPARE(retained.data(BackupListModel::StateDetailRole).toString(), items[125].stateDetail);
        QVERIFY(retained.data(Qt::ToolTipRole).toString().contains("2 / 3"));
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
        // WidgetShortcut activation needs an active window. Even the Linux
        // offscreen plugin cannot activate a WA_DontShowOnScreen window.
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
        page.setBackup(id, head(service, id));
        settle(service);
        QCOMPARE(table->currentIndex().row(), 0);
        page.setBackup(id, previous);
        settle(service);
        QCOMPARE(table->model()->index(table->currentIndex().row(), 2).data(Qt::UserRole + 1).toString(), previous);
        QSignalSpy routes(&page, &HomePageBackupPage::navigate);
#ifdef Q_OS_MACOS
        if (QGuiApplication::platformName() == "offscreen")
            QSKIP("The macOS offscreen plugin does not deliver keyboard focus to a hidden view");
#endif
        page.activateWindow();
        table->setFocus();
        QTRY_VERIFY(table->hasFocus());
        QTest::keyClick(table, Qt::Key_Return);
        QCOMPARE(routes.count(), 1);
        QCOMPARE(qvariant_cast<Route>(routes.first().first()).commit, previous);
        // A monitor update cannot destroy an in-progress message editor.
        table->setCurrentIndex(table->model()->index(0, 0));
        page.findChild<QAction *>("editMessageAction")->trigger();
        QPointer<QLineEdit> editor = table->findChild<QLineEdit *>();
        QVERIFY(editor);
        editor->setFocus();
        QTRY_VERIFY(editor && editor->hasFocus());
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
        const auto toolTipBaseBefore = QToolTip::palette().color(QPalette::ToolTipBase);
        m_theme->toggle();
        page.refreshTheme();
        QCoreApplication::processEvents();
        const auto toolTipBaseToggled = QToolTip::palette().color(QPalette::ToolTipBase);
        QVERIFY(toolTipBaseBefore != toolTipBaseToggled);
        QCOMPARE(content->toPlainText(), text);
        QCOMPARE(content->textCursor().selectedText(), selection);
        QCOMPARE(QPoint(content->horizontalScrollBar()->value(), content->verticalScrollBar()->value()), scroll);
        m_theme->toggle();
        page.refreshTheme();
        QCOMPARE(QToolTip::palette().color(QPalette::ToolTipBase), toolTipBaseBefore);
        auto *analyze = page.findChild<QAction *>("analyzeAction");
        analyze->trigger();
        settle(service);
        QVERIFY(page.isLoading());
        page.deactivate();
        page.hide();
        QVERIFY(!page.isLoading());
        gateway.summaries.last()("stale after leaving", {});
        QVERIFY(page.findChild<QPlainTextEdit *>("analysis")->toPlainText().isEmpty());
        page.show();
        analyze->trigger();
        settle(service);
        QVERIFY(page.isLoading());
        gateway.summaries.last()("当前分析结果", {});
        QVERIFY(!page.isLoading());
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
        QVERIFY(!page.isLoading());
        QCOMPARE(page.findChild<QPlainTextEdit *>("analysis")->toPlainText(), QString("模拟分析失败"));
        analyze->trigger();
        settle(service);
        QVERIFY(service.rebuild(id).success);
        QVERIFY(!page.isLoading());
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
        QSignalSpy fetchFailure(&settings, &SettingsService::notification);
        ai.findChild<QPushButton *>("fetchButton")->click();
        gateway.models.last()({}, "模拟模型获取失败");
        QVERIFY(!modelsSpinner->spinning());
        QCOMPARE(fetchFailure.count(), 1);
        QCOMPARE(fetchFailure.first().first().value<OperationResult>().message, QString("模拟模型获取失败"));
        auto *aiSwitch = ai.findChild<oclero::qlementine::Switch *>("aiEnabledSwitch");
        QVERIFY(aiSwitch != nullptr);
        QVERIFY(aiSwitch->isEnabled());
        settings.saveField("Model", "");
        ai.refresh();
        QVERIFY(!aiSwitch->isEnabled());
        HomePageDiffPage diffPage(&service, &settings, &gateway);
        diffPage.setRevision(id, head(service, id));
        settle(service);
        auto *analyzeAction = diffPage.findChild<QAction *>("analyzeAction");
        QVERIFY(analyzeAction != nullptr);
        QVERIFY(!analyzeAction->isEnabled());
        settings.saveField("Model", "manual-model");
        ai.refresh();
        QVERIFY(aiSwitch->isEnabled());
        QVERIFY(analyzeAction->isEnabled());
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
#ifdef Q_OS_MACOS
        if (QGuiApplication::platformName() == "offscreen")
            QSKIP("The macOS offscreen plugin does not deliver keyboard focus to a hidden host");
#endif
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
    void historyMenuCreatesBranchAfterMouseDispatch_data()
    {
        QTest::addColumn<bool>("acceptCreate");
        QTest::addColumn<bool>("switchAfter");
        QTest::addColumn<bool>("keyboard");
        QTest::addColumn<bool>("filtered");
        QTest::addColumn<bool>("direct");
        QTest::newRow("mouse-cancel-history") << false << false << false << false << false;
        QTest::newRow("mouse-create-history") << true << false << false << false << false;
        QTest::newRow("mouse-create-and-switch-filtered") << true << true << false << true << false;
        QTest::newRow("keyboard-create-filtered") << true << false << true << true << false;
        QTest::newRow("direct-cancel-history") << false << false << false << false << true;
        QTest::newRow("direct-create-history") << true << false << false << false << true;
        QTest::newRow("direct-create-and-switch-filtered") << true << true << false << true << true;
    }
    void historyMenuCreatesBranchAfterMouseDispatch()
    {
        QFETCH(bool, acceptCreate);
        QFETCH(bool, switchAfter);
        QFETCH(bool, keyboard);
        QFETCH(bool, filtered);
        QFETCH(bool, direct);
        TestDirectory dir; TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/document.txt";
        writeFile(source, "base\n"); QVERIFY(service.addLocal(source).success);
        const auto id = service.idForSource(source);
        const auto base = head(service, id);
        writeFile(source, "latest\n"); QVERIFY(service.backup(id).success);
        const auto original = service.branchContext(id);
        FakeAi gateway; SettingsService settings(pathsIn(dir), &gateway);
        MainWindow window(&service, &settings, &gateway, m_theme, false);
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        Route route{PageId::History, id};
        if (filtered) route.branchRef = original.ref;
        window.navigate(route); settle(service);
        auto *history = window.findChild<HomePageBackupPage *>();
        QVERIFY(history && history->isVisible());
        auto *table = history->findChild<QTableView *>("table"); QVERIFY(table);
        QCOMPARE(table->model()->rowCount(), 2);
        QPointer<QMenu> menu;
        QAction *create = nullptr;
        if (!direct)
        {
            openHistoryMenu(table, 1);
            menu = history->findChild<QMenu *>("revisionMenu"); QVERIFY(menu);
            QTRY_VERIFY(menu->isVisible() && menu->width() > 0);
            for (auto *action : menu->actions()) if (action->text() == "从此版本新建方案…") create = action;
            QVERIFY(create && create->isEnabled());
        }
        QSignalSpy requested(history, &HomePageBackupPage::createBranchRequested);
        // The direct opener and the menu must both preserve the clicked revision.
        table->selectRow(0);
        auto *branches = window.findChild<HomePageBranchesPage *>(); QVERIFY(branches);
        QSignalSpy notifications(branches, &HomePageBranchesPage::notification);
        bool shown = false, menuReleased = false, switchShown = false, timedOut = false;
        // Creation, history refreshes and switching share this budget. Git process
        // startup on Windows runners can take the workflow beyond ten seconds.
        // Match awaitBackup/settle while retaining a bounded modal-dialog watchdog.
        constexpr int workflowTimeoutMs = 30000;
        QTimer responder;
        responder.setInterval(10);
        connect(&responder, &QTimer::timeout, &window, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            if (dialog->objectName() == "createBranchDialog")
            {
                shown = true;
                menuReleased = menu.isNull();
                auto *name = dialog->findChild<QLineEdit *>("branchName");
                auto *use = dialog->findChild<QCheckBox *>();
                if (!name || !use) { dialog->reject(); return; }
                name->setText("菜单创建方案");
                use->setChecked(switchAfter);
                if (acceptCreate) dialog->accept(); else dialog->reject();
            }
            else if (dialog->objectName() == "switchBranchDialog")
            {
                switchShown = true;
                dialog->accept();
            }
        });
        QTimer watchdog;
        watchdog.setSingleShot(true);
        connect(&watchdog, &QTimer::timeout, &window, [&] {
            timedOut = true;
            responder.stop();
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject();
            if (menu) menu->close();
        });
        responder.start(); watchdog.start(workflowTimeoutMs);
        if (direct)
        {
            QTest::mouseClick(table->viewport(), Qt::LeftButton, {}, historyActionPoint(table, 1, 3));
            table->selectRow(0);
        }
        else if (keyboard)
        {
            menu->setActiveAction(create);
            QTest::keyPress(menu, Qt::Key_Return);
        }
        else
        {
            // Exercise Qlementine's delayed synthetic mouse release, not QAction::trigger().
            const auto point = menu->actionGeometry(create).center();
            QTest::mouseMove(menu, point);
            // Successive rows may reuse the cursor position; still deliver a move.
            QMouseEvent move(QEvent::MouseMove, point, menu->mapToGlobal(point), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(menu, &move);
            QTest::mouseClick(menu, Qt::LeftButton, {}, point);
            QVERIFY(create->property("qlementine_flashing").toBool());
        }
        QTRY_COMPARE_WITH_TIMEOUT(requested.count(), 1, workflowTimeoutMs + 1000);
        QCOMPARE(requested.first().at(1).toString(), base);
        QTRY_VERIFY_WITH_TIMEOUT(shown || timedOut, workflowTimeoutMs + 1000);
        if (acceptCreate) QTRY_VERIFY_WITH_TIMEOUT(notifications.count() >= (switchAfter ? 2 : 1) || timedOut, workflowTimeoutMs + 1000);
        watchdog.stop(); responder.stop(); settle(service);
        QVERIFY2(!timedOut, qPrintable(QString("Branch workflow timed out: create dialog=%1, switch dialog=%2, notifications=%3")
            .arg(shown).arg(switchShown).arg(notifications.count())));
        QVERIFY(shown);
        QVERIFY2(menuReleased, "The menu must finish dispatching and be released before the modal dialog opens");
        QVERIFY(!menu);
        QCOMPARE(switchShown, switchAfter);
        for (const auto &args : notifications) QVERIFY(qvariant_cast<OperationResult>(args.first()).success);
        const auto created = runGit(service.repoPath(id), {"rev-parse", "--verify", "refs/heads/菜单创建方案"});
        QCOMPARE(created.success(), acceptCreate);
        if (acceptCreate) QCOMPARE(created.output.trimmed(), base);
        QCOMPARE(service.branchContext(id).ref, switchAfter ? QString("refs/heads/菜单创建方案") : original.ref);
        QCOMPARE(head(service, id), switchAfter ? base : original.head);
        QCOMPARE(readFile(source), switchAfter ? QByteArray("base\n") : QByteArray("latest\n"));
    }
    void historyMergesCloudCopyOfSameBranch()
    {
        Revision same, behind;
        same.refs = {"main", "origin/main"}; same.branchRefs = {"refs/heads/main", "refs/remotes/origin/main"};
        behind.refs = {"origin/topic"}; behind.branchRefs = {"refs/remotes/origin/topic"};
        const auto shown = HistoryGraph::userFacing({same, behind});
        QCOMPARE(shown[0].refs, QStringList({"main"}));
        QCOMPARE(shown[0].branchRefs, QStringList({"refs/heads/main"}));
        QCOMPARE(shown[1].refs, QStringList({"topic（云端）"}));
        QCOMPARE(shown[1].branchRefs, QStringList({"refs/remotes/origin/topic"}));
    }
    void historyBranchBadgesKeepExactTargets()
    {
        TestDirectory dir; TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/document.txt";
        writeFile(source, "base\n"); QVERIFY(service.addLocal(source).success);
        const auto id = service.idForSource(source);
        QVERIFY(service.renameBranch(id, service.branchContext(id).ref, "main").success);
        QVERIFY(service.createBranch(id, "earlier").success);
        writeFile(source, "latest\n"); QVERIFY(service.backup(id).success);
        QVERIFY(service.createBranch(id, "alias").success);
        HomePageBackupPage page(&service);
        page.resize(1080, 700); page.setAttribute(Qt::WA_DontShowOnScreen); page.show();
        page.setBackup(id); settle(service);
        auto *table = page.findChild<QTableView *>("table"); QVERIFY(table);
        QCOMPARE(table->model()->rowCount(), 2);
        QSignalSpy requested(&page, &HomePageBackupPage::branchRequested);
        const auto badgePoint = [table](int row, int offset = 4) {
            const auto index = table->model()->index(row, 0);
            const auto cell = table->visualRect(index);
            return QPoint(cell.left() + index.data(HistoryGraph::Width).toInt() + offset, cell.center().y());
        };
        QTest::mouseClick(table->viewport(), Qt::LeftButton, {}, badgePoint(0));
        auto *menu = page.findChild<QMenu *>("historyBranchMenu"); QVERIFY(menu);
        QAction *alias = nullptr;
        for (auto *action : menu->actions()) if (action->data().toString() == "refs/heads/alias") alias = action;
        QVERIFY(alias); alias->trigger(); menu->close();
        QCOMPARE(requested.count(), 1);
        QCOMPARE(requested.takeFirst().first().toString(), QString("refs/heads/alias"));
        QTest::mouseClick(table->viewport(), Qt::LeftButton, {}, badgePoint(1));
        QCOMPARE(requested.count(), 1);
        QCOMPARE(requested.takeFirst().first().toString(), QString("refs/heads/earlier"));
        QTest::mouseClick(table->viewport(), Qt::LeftButton, {}, badgePoint(1, 145));
        QCOMPARE(requested.count(), 0);
        QVERIFY(table->model()->index(1, 0).data(HistoryGraph::NodeRefs).toStringList().contains("refs/heads/earlier"));

        QVERIFY(runGit(service.repoPath(id), {"tag", "milestone", "HEAD"}).success());
        page.refresh(); settle(service);
        auto *filter = page.findChild<oclero::qlementine::Switch *>(); QVERIFY(filter);
        filter->setChecked(true); settle(service);
        const auto cell = table->visualRect(table->model()->index(0, 0));
        QTest::mouseClick(table->viewport(), Qt::LeftButton, {}, QPoint(cell.left() + 14, cell.center().y()));
        QCOMPARE(requested.count(), 0);
        QCOMPARE(readFile(source), QByteArray("latest\n"));
    }
    void branchDeletionKeepsConfirmedVersion_data()
    {
        QTest::addColumn<bool>("changedDuringConfirmation");
        QTest::newRow("unchanged-version") << false;
        QTest::newRow("refreshed-version") << true;
    }
    void branchDeletionKeepsConfirmedVersion()
    {
        QFETCH(bool, changedDuringConfirmation);
        TestDirectory dir; TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/document.txt";
        writeFile(source, "base\n"); QVERIFY(service.addLocal(source).success);
        const auto id = service.idForSource(source);
        QVERIFY(service.createBranch(id, "topic").success);
        writeFile(source, "latest\n"); QVERIFY(service.backup(id).success);
        const auto latest = head(service, id);
        HomePageBranchesPage page(&service);
        page.resize(1080, 700); page.setAttribute(Qt::WA_DontShowOnScreen); page.show();
        page.setBackup(id); settle(service);
        auto *row = branchRow(&page, "refs/heads/topic"); QVERIFY(row);
        auto *operations = row->findChild<QToolButton *>("branchMore"); QVERIFY(operations && operations->isEnabled());
        QSignalSpy notifications(&page, &HomePageBranchesPage::notification);
        bool confirmed = false, updated = false;
        QTimer responder;
        responder.setInterval(10);
        connect(&responder, &QTimer::timeout, &page, [&] {
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
            {
                responder.stop();
                confirmed = true;
                updated = !changedDuringConfirmation || runGit(service.repoPath(id), {"update-ref", "refs/heads/topic", latest}).success();
                page.refresh(); settle(service);
                dialog->accept();
            }
        });
        QTimer choose;
        choose.setInterval(10);
        connect(&choose, &QTimer::timeout, &page, [&] {
            if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget()))
            {
                choose.stop();
                for (auto *action : menu->actions()) if (action->text() == "删除…")
                {
                    menu->setActiveAction(action);
                    QTest::keyPress(menu, Qt::Key_Return);
                    return;
                }
                menu->close();
            }
        });
        QTimer watchdog;
        watchdog.setSingleShot(true);
        connect(&watchdog, &QTimer::timeout, &page, [] {
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject();
            if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) menu->close();
        });
        responder.start(); choose.start(); watchdog.start(10000);
        operations->click();
        watchdog.stop(); responder.stop(); choose.stop();
        settle(service);
        QVERIFY(confirmed && updated);
        const auto branch = runGit(service.repoPath(id), {"rev-parse", "--verify", "refs/heads/topic"});
        QCOMPARE(branch.success(), changedDuringConfirmation);
        if (changedDuringConfirmation) QCOMPARE(branch.output.trimmed(), latest);
        QCOMPARE(notifications.count(), 1);
        QCOMPARE(qvariant_cast<OperationResult>(notifications.first().first()).success, !changedDuringConfirmation);
    }
    void branchDeletionAsksAgainForUniqueVersions_data()
    {
        QTest::addColumn<bool>("confirmSecond");
        QTest::newRow("keep") << false;
        QTest::newRow("delete") << true;
    }
    void branchDeletionAsksAgainForUniqueVersions()
    {
        QFETCH(bool, confirmSecond);
        TestDirectory dir; TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/document.txt";
        writeFile(source, "base\n"); QVERIFY(service.addLocal(source).success);
        const auto id = service.idForSource(source);
        const auto original = service.branchContext(id).ref;
        QVERIFY(service.createBranch(id, "topic").success);
        QVERIFY(service.switchBranch(id, "refs/heads/topic").success);
        writeFile(source, "topic only\n"); QVERIFY(service.backup(id).success);
        QVERIFY(service.switchBranch(id, original).success);
        HomePageBranchesPage page(&service);
        page.resize(1080, 700); page.setAttribute(Qt::WA_DontShowOnScreen); page.show();
        page.setBackup(id); settle(service);
        auto *row = branchRow(&page, "refs/heads/topic"); QVERIFY(row);
        auto *operations = row->findChild<QToolButton *>("branchMore"); QVERIFY(operations && operations->isEnabled());
        int dialogs = 0;
        QTimer responder; responder.setInterval(10);
        connect(&responder, &QTimer::timeout, &page, [&] {
            if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget()))
            {
                for (auto *action : menu->actions()) if (action->text() == "删除…") { menu->setActiveAction(action); QTest::keyPress(menu, Qt::Key_Return); return; }
                menu->close();
            }
            else if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
            {
                ++dialogs;
                if (dialogs == 1 || confirmSecond) dialog->accept(); else dialog->reject();
                if (dialogs == 2) responder.stop();
            }
        });
        QTimer watchdog; watchdog.setSingleShot(true);
        connect(&watchdog, &QTimer::timeout, &page, [&] {
            responder.stop();
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject();
            if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) menu->close();
        });
        responder.start(); watchdog.start(15000);
        operations->click();
        // The second confirmation opens from the service callback after the menu returns.
        QTRY_VERIFY_WITH_TIMEOUT(dialogs == 2, 10000);
        settle(service);
        watchdog.stop(); responder.stop();
        QCOMPARE(runGit(service.repoPath(id), {"rev-parse", "--verify", "refs/heads/topic"}).success(), !confirmSecond);
    }
    void collaborationProvidesFullSyncControls()
    {
        TestDirectory dir; TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/document.txt";
        writeFile(source, "base\n"); QVERIFY(service.addLocal(source).success);
        const auto id = service.idForSource(source);
        QVERIFY(service.renameBranch(id, service.branchContext(id).ref, "main").success);
        QVERIFY(service.createBranch(id, "topic").success);
        const auto remote = dir.path() + "/remote.git";
        QVERIFY(runGit({}, {"init", "--bare", "--initial-branch=main", remote}).success());
        QVERIFY(service.setRemote(id, remote).success);
        FakeAi gateway; SettingsService settings(pathsIn(dir), &gateway);
        MainWindow window(&service, &settings, &gateway, m_theme, false);
        window.setAttribute(Qt::WA_DontShowOnScreen); window.resize(1080, 740); window.show();
        window.navigate({PageId::Collaboration, id}); settle(service);
        auto *overview = window.findChild<HomePageCollaborationPage *>(); QVERIFY(overview && overview->isVisible());
        auto *upload = overview->findChild<QPushButton *>("syncButton"); QVERIFY(upload && upload->isEnabled());
        QCOMPARE(upload->accessibleName(), QString("立即同步全部方案"));
        QSignalSpy notifications(overview, &HomePageCollaborationPage::notification);
        upload->click(); settle(service);
        QVERIFY(!notifications.isEmpty());
        QVERIFY(qvariant_cast<OperationResult>(notifications.last().first()).success);
        const auto topic = runGit(service.repoPath(id), {"rev-parse", "topic"}); QVERIFY(topic.success());
        QCOMPARE(runGit(remote, {"rev-parse", "topic"}).output.trimmed(), topic.output.trimmed());
        QVERIFY(runGit(remote, {"branch", "cloud-only", "main"}).success());
        notifications.clear();
        auto *download = overview->findChild<QPushButton *>("syncButton"); QVERIFY(download && download->isEnabled());
        QCOMPARE(download->accessibleName(), QString("立即同步全部方案"));
        download->click(); settle(service);
        QVERIFY(!notifications.isEmpty());
        QVERIFY(qvariant_cast<OperationResult>(notifications.last().first()).success);
        QVERIFY(runGit(service.repoPath(id), {"rev-parse", "refs/remotes/origin/cloud-only"}).success());
        QCOMPARE(readFile(source), QByteArray("base\n"));
        auto *automatic = overview->findChild<oclero::qlementine::Switch *>("automaticSyncSwitch"); QVERIFY(automatic && automatic->isChecked());
        automatic->click(); settle(service); QVERIFY(!service.syncStatus(id).automatic);
        QCOMPARE(service.syncStatus(id).remoteUrl, remote);
        writeFile(source, "manual update\n"); upload->click(); settle(service);
        QCOMPARE(runGit(remote, {"show", "main:document.txt"}).bytes, QByteArray("manual update\n"));
        automatic->click(); settle(service);
        const auto output = qEnvironmentVariable("ZC_TEST_SCREENSHOTS");
        if (!output.isEmpty())
        {
            QDir().mkpath(output);
            if (auto *notification = window.findChild<NotificationBar *>()) notification->dismiss();
            for (const auto theme : {ThemeMode::Light, ThemeMode::Dark})
            {
                m_theme->setMode(theme);
                for (const auto size : {QSize(1080, 740), QSize(760, 520)})
                {
                    window.resize(size); QTest::qWait(80);
                    auto *scroll = overview->findChild<QScrollArea *>("scroll"); QVERIFY(scroll);
                    scroll->ensureWidgetVisible(upload);
                    QTest::qWait(30);
                    QVERIFY(window.grab().save(output + QString("/collaboration-%1-%2.png").arg(theme == ThemeMode::Light ? "light" : "dark").arg(size.width())));
                }
            }
        }
        window.navigate({PageId::Dashboard, id}); settle(service);
        QVERIFY(!window.findChild<HomePageDashboardPage *>()->findChild<QPushButton *>("pushButton"));
        QVERIFY(!window.findChild<HomePageDashboardPage *>()->findChild<QLineEdit *>("remoteUrl"));
        window.navigate({PageId::Branches, id}); settle(service);
        auto *branches = window.findChild<HomePageBranchesPage *>(); QVERIFY(branches && branches->isVisible());
        QVERIFY(!branches->findChild<QAction *>("fetchBranchesAction"));
        auto *topicRow = branchRow(branches, "refs/heads/topic"); QVERIFY(topicRow);
        auto *operations = topicRow->findChild<QToolButton *>("branchMore"); QVERIFY(operations && operations->isEnabled());
        QStringList menuItems;
        QTimer closer;
        closer.setInterval(10);
        connect(&closer, &QTimer::timeout, &window, [&] {
            if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget()))
            {
                for (auto *action : menu->actions()) if (!action->isSeparator()) menuItems.append(action->text());
                menu->close();
                closer.stop();
            }
        });
        // Bound a failed popup detection so this test cannot leave a modal menu open.
        QTimer::singleShot(5000, &window, [&] { if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) menu->close(); });
        closer.start(); operations->click(); closer.stop();
        QCOMPARE(menuItems, QStringList({"查看历史", "切换到此方案…", "从此方案新建…", "与其他方案比较…", "合并到正在使用的方案…", "重命名…", "删除…"}));
    }
    void collaborationConnectionUsesExplicitSaveAndRejectsStaleReply()
    {
        TestDirectory dir; TestBackupService service(pathsIn(dir));
        const auto first = dir.path() + "/first.txt", second = dir.path() + "/second.txt", remote = dir.path() + "/shared.git";
        writeFile(first, "first\n"); writeFile(second, "second\n");
        QVERIFY(service.addLocal(first).success); QVERIFY(service.addLocal(second).success);
        const auto firstId = service.idForSource(first), secondId = service.idForSource(second);
        QVERIFY(runGit({}, {"init", "--bare", remote}).success());
        FakeAi gateway; SettingsService settings(pathsIn(dir), &gateway);
        MainWindow window(&service, &settings, &gateway, m_theme, false);
        window.setAttribute(Qt::WA_DontShowOnScreen); window.resize(1080, 740); window.show();
        window.navigate({PageId::Collaboration, firstId}); settle(service);
        auto *page = window.findChild<HomePageCollaborationPage *>(); QVERIFY(page);
        auto *url = page->findChild<QLineEdit *>("remoteUrl"); auto *connectButton = page->findChild<QAbstractButton *>("connectButton");
        const auto enterAddress = [&] { url->setText(remote); url->setModified(true); emit url->textEdited(remote); };
        enterAddress(); settle(service); QVERIFY(service.syncStatus(firstId).remoteUrl.isEmpty());
        bool navigated = false;
        const auto connection = connect(&service, &BackupService::taskStarted, &window, [&](BackupTaskId, const QString &id) {
            if (!navigated && id.isEmpty()) { navigated = true; window.navigate({PageId::Collaboration, secondId}); }
        });
        connectButton->click(); settle(service); disconnect(connection);
        QVERIFY(navigated); QVERIFY(service.syncStatus(firstId).remoteUrl.isEmpty()); QVERIFY(service.syncStatus(secondId).remoteUrl.isEmpty());
        window.navigate({PageId::Collaboration, firstId}); settle(service);
        enterAddress(); connectButton->click(); settle(service);
        QCOMPARE(service.syncStatus(firstId).remoteUrl, remote); QVERIFY(service.syncStatus(firstId).automatic);
        page->findChild<QAbstractButton *>("copyButton")->click(); QCOMPARE(QApplication::clipboard()->text(), remote);
        page->findChild<QPushButton *>("syncButton")->click(); settle(service);
        QCOMPARE(service.syncStatus(firstId).state, SyncState::Synced);
        const auto output = qEnvironmentVariable("ZC_TEST_SCREENSHOTS");
        if (!output.isEmpty())
        {
            if (auto *notice = window.findChild<NotificationBar *>()) notice->dismiss();
            for (const auto mode : {ThemeMode::Light, ThemeMode::Dark})
            {
                m_theme->setMode(mode);
                for (const auto size : {QSize(1080, 740), QSize(760, 520)})
                {
                    window.resize(size); QTest::qWait(60);
                    auto *scroll = page->findChild<QScrollArea *>("scroll");
                    QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
                    QVERIFY(window.grab().save(output + QString("/collaboration-final-%1-%2.png").arg(mode == ThemeMode::Light ? "light" : "dark").arg(size.width())));
                }
            }
        }
        QVERIFY(answerConfirmation(page->findChild<QAbstractButton *>("disconnectButton"), service, [](QDialog *dialog) { dialog->accept(); }));
        QVERIFY(service.syncStatus(firstId).remoteUrl.isEmpty()); QVERIFY(!page->findChild<QPushButton *>("syncButton")->isEnabled());
        QCOMPARE(readFile(first), QByteArray("first\n"));
    }

    void branchUiBrowsingGraphAndConfirmation()
    {
        TestDirectory dir; TestBackupService service(pathsIn(dir));
        const auto source = dir.path() + "/项目/document.txt";
        writeFile(source, "base\n"); QVERIFY(service.addLocal(source).success);
        const auto id = service.idForSource(source);
        QVERIFY(service.renameBranch(id, service.branchContext(id).ref, "main").success);
        for (int version = 1; version < 7; ++version)
        {
            writeFile(source, QByteArray::number(version) + '\n');
            QVERIFY(service.backup(id).success);
        }
        QVERIFY(service.createBranch(id, "试验方案").success);
        QVERIFY(service.switchBranch(id, "refs/heads/试验方案").success);
        writeFile(source, "topic\n"); QVERIFY(service.backup(id).success); const auto topic = head(service, id);
        QVERIFY(service.switchBranch(id, "refs/heads/main").success);
        writeFile(source, "main\n"); QVERIFY(service.backup(id).success); const auto main = head(service, id);
        FakeAi gateway; SettingsService settings(pathsIn(dir), &gateway);
        MainWindow window(&service, &settings, &gateway, m_theme, false);
        window.setAttribute(Qt::WA_DontShowOnScreen); window.show();
        window.navigate({PageId::History, id}); settle(service);
        auto *history = window.findChild<HomePageBackupPage *>(); QVERIFY(history);
        auto *historyTable = history->findChild<QTableView *>("table"); QVERIFY(historyTable);
        auto *scope = history->findChild<QComboBox *>("historyScope"); QVERIFY(scope);
        QCOMPARE(historyTable->model()->rowCount(), 9);
        // Clicking a 方案's line narrows the history to that 方案 without switching.
        const auto lineCell = historyTable->visualRect(historyTable->model()->index(0, 0));
        QTest::mouseClick(historyTable->viewport(), Qt::LeftButton, {}, QPoint(lineCell.left() + 14, lineCell.center().y()));
        settle(service);
        QVERIFY(history->isVisible());
        QCOMPARE(scope->currentData().toString(), QString("refs/heads/main"));
        QCOMPARE(historyTable->model()->rowCount(), 8);
        QCOMPARE(head(service, id), main);
        QCOMPARE(readFile(source), QByteArray("main\n"));
        // Clicking a 方案 row opens its history without switching.
        QVERIFY(service.createBranch(id, "全部共享", topic).success);
        window.navigate({PageId::Branches, id}); settle(service);
        auto *page = window.findChild<HomePageBranchesPage *>(); QVERIFY(page && page->isVisible());
        auto *trial = branchRow(page, "refs/heads/试验方案"); QVERIFY(trial && trial->isVisible());
        QTest::mouseClick(trial, Qt::LeftButton, Qt::NoModifier, QPoint(40, trial->height() / 2)); settle(service);
        QVERIFY(history->isVisible());
        QCOMPARE(scope->currentData().toString(), QString("refs/heads/试验方案"));
        QCOMPARE(head(service, id), main);
        QCOMPARE(readFile(source), QByteArray("main\n"));
        // The 方案 tab lists every branch as a normal tab page; the current one offers no switch.
        window.navigate({PageId::Branches, id}); settle(service);
        QVERIFY(window.findChild<QToolButton *>("branchesTab")->isChecked());
        QVERIFY(!window.findChild<QToolButton *>("returnHistoryButton")->isVisible());
        QVERIFY(branchRow(page, "refs/heads/main") && branchRow(page, "refs/heads/试验方案") && branchRow(page, "refs/heads/全部共享"));
        QVERIFY(!branchRow(page, "refs/heads/main")->findChild<QPushButton *>("branchUse"));
        QVERIFY(branchRow(page, "refs/heads/试验方案")->findChild<QPushButton *>("branchUse"));
        // The header menu button lists local branches and leads to the full list.
        window.navigate({PageId::History, id}); settle(service);
        auto *selector = window.findChild<QToolButton *>("branchSelector"); QVERIFY(selector && selector->isVisible());
        QTRY_COMPARE(selector->text(), QString("方案：main"));
        QStringList entries;
        for (auto *action : selector->menu()->actions()) if (!action->isSeparator()) entries.append(action->text());
        QCOMPARE(entries, QStringList({"main", "全部共享", "试验方案", "管理全部方案"}));
        selector->menu()->actions().last()->trigger(); settle(service);
        QVERIFY(page->isVisible());
        QCOMPARE(head(service, id), main);
        QCOMPARE(readFile(source), QByteArray("main\n"));
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
#ifdef Q_OS_LINUX
        // QWindowKit uses client-side title bars for X11 and Wayland.
        QVERIFY(window.windowFlags().testFlag(Qt::FramelessWindowHint));
#else
        QVERIFY(!window.windowFlags().testFlag(Qt::FramelessWindowHint));
#endif
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
        const QVector<Route> routes{{}, {PageId::Dashboard, id}, {PageId::Collaboration, id}, {PageId::History, id}, {PageId::Diff, id, head(service, id)}, {PageId::GeneralSettings}, {PageId::AiSettings}, {PageId::About}};
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
                const bool isSettings = routes[i].page == PageId::GeneralSettings || routes[i].page == PageId::AiSettings || routes[i].page == PageId::About;
                QCOMPARE(window.findChild<QToolButton *>("collapseButton")->isVisible(), !isSettings);
                if (routes[i].page == PageId::History)
                {
                    auto *table = window.findChild<HomePageBackupPage *>()->findChild<QTableView *>("table");
                    QVERIFY(table->viewport()->height() / table->rowHeight(0) >= 10);
                    hoverHistoryAction(table, 2, 0);
                    capture(QString("history-hover-%1").arg(theme));
                    openHistoryMenu(table, 2);
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
    void tooltipVisualSmoke()
    {
        QWidget parent;
        parent.resize(200, 200);
        parent.show();
        QTest::qWaitForWindowExposed(&parent);
        QToolTip::showText(parent.mapToGlobal(QPoint(50, 50)), "概览", &parent);
        QTRY_VERIFY(QToolTip::isVisible());
        QWidget *tipLabel = nullptr;
        for (auto *w : QApplication::topLevelWidgets()) {
            if (w->inherits("QTipLabel") && w->isVisible()) {
                tipLabel = w;
                break;
            }
        }
        QVERIFY(tipLabel != nullptr);
        QVERIFY(tipLabel->windowFlags().testFlag(Qt::NoDropShadowWindowHint));
#ifdef _WIN32
        auto hwnd = reinterpret_cast<HWND>(tipLabel->winId());
        auto classStyle = (unsigned long)GetClassLongPtrW(hwnd, GCL_STYLE);
        QVERIFY(!(classStyle & 0x00020000));
        QVERIFY(!tipLabel->mask().isEmpty());
#endif
        QToolTip::hideText();
    }

    void updateNotificationBadgeAndNavigation()
    {
        TestDirectory dir;
        TestBackupService service(pathsIn(dir));
        FakeAi gateway;
        SettingsService settings(pathsIn(dir), &gateway);
        MainWindow window(&service, &settings, &gateway, m_theme, false);
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.show();

        auto *updates = window.findChild<UpdateService *>();
        auto *aboutTab = window.findChild<QToolButton *>("aboutTab");
        auto *badge = window.findChild<QWidget *>("aboutNotificationBadge");
        QVERIFY(updates && aboutTab && badge);
        window.navigate({PageId::GeneralSettings});
        QVERIFY(!badge->isVisible());

        UpdateRelease release;
        release.currentVersion = "0.1.0";
        release.version = "0.2.0";
        release.releaseUrl = QUrl("https://github.com/Zao-chen/ZcVersionBox/releases/tag/v0.2.0");
        updates->automaticUpdateAvailable(release);
        QTRY_VERIFY(badge->isVisible());
        QVERIFY(aboutTab->toolTip().contains("有新版本"));

        window.navigate({PageId::About});
        QTRY_VERIFY(!badge->isVisible());
        QCOMPARE(aboutTab->toolTip(), QString("关于"));
    }

  private:
    TestDirectory m_environment;
    ThemeController *m_theme{nullptr};
};
QTEST_MAIN(Regression)
#include "regression.moc"
