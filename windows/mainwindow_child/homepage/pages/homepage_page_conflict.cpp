#include "homepage_page_conflict.h"
#include "homepage_diff_delegate.h"
#include "homepage_diff_view.h"
#include "ui_homepage_page_conflict.h"
#include "windows/mainwindow_presentation.h"
#include <oclero/qlementine/widgets/LoadingSpinner.hpp>
#include <QFileInfo>
#include <QLabel>
#include <QPushButton>
#include <QPointer>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

namespace
{
QString choiceText(const ConflictFile &file, bool local)
{
    if (!file.wholeFile) return local ? QStringLiteral("保留本地这段") : QStringLiteral("使用云端这段");
    const auto side = local ? QStringLiteral("本地") : QStringLiteral("云端");
    if (!(local ? file.localExists : file.remoteExists)) return "删除文件（" + side + "）";
    if (!file.localExists || !file.remoteExists) return "保留文件（" + side + "）";
    const auto type = (local ? file.localDirectory : file.remoteDirectory) ? QStringLiteral("文件夹") : QStringLiteral("文件");
    return (local ? "保留本地" : "使用云端") + type;
}
}

HomePageConflictPage::HomePageConflictPage(BackupService *service, QWidget *parent)
    : QWidget(parent), ui(new Ui::HomePageConflictPage), m_service(service)
{
    ui->setupUi(this);
    m_loadingPage = new QWidget(ui->modeStack);
    m_loadingPage->setObjectName("conflictLoadingPage");
    auto *loadingLayout = new QVBoxLayout(m_loadingPage);
    loadingLayout->addStretch();
    m_loadingSpinner = new oclero::qlementine::LoadingSpinner(m_loadingPage);
    m_loadingSpinner->setObjectName("conflictSpinner");
    m_loadingSpinner->setAccessibleName("正在读取差异");
    m_loadingSpinner->setFixedSize(32, 32);
    auto *loadingLabel = new QLabel(QStringLiteral("正在读取差异…"), m_loadingPage);
    loadingLabel->setObjectName("conflictLoadingLabel");
    loadingLabel->setAlignment(Qt::AlignCenter);
    UiStyle::text(loadingLabel, UiStyle::FontRole::Caption, true);
    loadingLayout->addWidget(m_loadingSpinner, 0, Qt::AlignHCenter);
    loadingLayout->addWidget(loadingLabel);
    loadingLayout->addStretch();
    ui->modeStack->addWidget(m_loadingPage);
    UiStyle::text(ui->pageTitle, UiStyle::FontRole::Object);
    UiStyle::text(ui->filesLabel, UiStyle::FontRole::Section);
    UiStyle::text(ui->fileTitle, UiStyle::FontRole::Body);
    for (auto *label : {ui->progressLabel, ui->filesCount, ui->positionLabel, ui->previewSummary, ui->staleLabel})
        UiStyle::text(label, UiStyle::FontRole::Caption, true);
    // Long paths are elided in place; the header must not widen the splitter.
    ui->fileTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    ui->fileTitle->setMinimumWidth(40);
    ui->staleBanner->setVisible(false);
    ui->previewSummary->setVisible(false);

    ui->files->setAccessibleName("同步差异文件");
    ui->files->setModel(&m_files);
    UiStyle::flatView(ui->files);
    ui->files->setItemDelegate(new DiffFileDelegate(ui->files));
    ui->splitter->setStretchFactor(0, 0);
    ui->splitter->setStretchFactor(1, 1);
    ui->splitter->setSizes({180, 560});
    connect(ui->localChoice, &QPushButton::clicked, this, [this] { choose(ConflictChoice::Local); });
    connect(ui->remoteChoice, &QPushButton::clicked, this, [this] { choose(ConflictChoice::Remote); });
    connect(ui->files->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] { selectFile(); });
    connect(ui->previousButton, &QPushButton::clicked, this, [this] { moveQuestion(false); });
    connect(ui->nextButton, &QPushButton::clicked, this, [this] {
        if (ui->nextButton->text() == QStringLiteral("预览结果")) preparePreview();
        else moveQuestion(true);
    });
    connect(ui->previewButton, &QPushButton::clicked, this, &HomePageConflictPage::preparePreview);
    connect(ui->applyButton, &QPushButton::clicked, this, &HomePageConflictPage::apply);
    connect(ui->editChoicesButton, &QPushButton::clicked, this, [this]
    {
        m_preview = false;
        m_prepared = {};
        populateFiles();
        showQuestion();
    });
    connect(ui->laterButton, &QPushButton::clicked, this, [this] { emit navigate(returnRoute()); });
    connect(ui->reanalyzeButton, &QPushButton::clicked, this, [this]
    {
        const auto ctx = context();
        const QPointer<HomePageConflictPage> guard(this);
        if (confirmAction(this, m_session.mergeRef.isEmpty() ? "将重新获取云端并分析当前源内容。已有选择会清空，需要重新逐处确认。" : "将按所选方案的最新版本重新分析。已有选择会清空，需要重新逐处确认。", "重新分析") && guard && current(ctx))
            load(true);
    });
    connect(service, &BackupService::repositoryChanged, this, [this](const QString &id)
    {
        if (id == m_id && m_active && !m_busy) load();
    });
    connect(service, &BackupService::repositoryInvalidated, this, [this](const QString &id)
    {
        m_positions.remove(id);
        if (id == m_id)
        {
            deactivate();
            m_session = {};
            m_prepared = {};
            m_files.clear();
            setFileTitle({});
            ui->positionLabel->clear();
            ui->diffView->clear();
            ui->previewDiffView->clear();
            updateActions();
        }
    });
    updateActions();
}
HomePageConflictPage::~HomePageConflictPage() = default;
HomePageConflictPage::Context HomePageConflictPage::context() const { return {m_id, m_repository, m_context}; }
bool HomePageConflictPage::current(const Context &ctx) const
{
    return m_active && ctx.id == m_id && ctx.repository == m_service->repositoryGeneration(ctx.id) &&
           ctx.page == m_context && m_service->contains(ctx.id);
}
void HomePageConflictPage::rememberPosition()
{
    if (m_session.id.isEmpty()) return;
    m_positions[m_id] = {m_session.id, m_session.files.value(m_file).path, m_hunk, m_preview};
}
void HomePageConflictPage::deactivate()
{
    rememberPosition();
    m_active = false;
    ++m_context;
    ++m_contentRequest;
    m_busy = false;
    m_prepared = {};
    ui->diffView->clear();
    ui->previewDiffView->clear();
    updateActions();
}
void HomePageConflictPage::setBackup(const QString &id, const QString &returnBranchRef)
{
    rememberPosition();
    ++m_context;
    ++m_contentRequest;
    m_id = id;
    m_returnBranchRef = returnBranchRef;
    m_repository = m_service->repositoryGeneration(id);
    m_active = true;
    m_session = {};
    m_prepared = {};
    m_busy = false;
    m_preview = false;
    m_files.clear();
    setFileTitle({});
    ui->positionLabel->clear();
    ui->diffView->clear();
    ui->previewDiffView->clear();
    load();
}
Route HomePageConflictPage::returnRoute() const
{
    // A local merge was started from the 方案 list, so it returns there.
    if (!m_returnBranchRef.isEmpty()) return {PageId::Branches, m_id};
    return {m_cloudSession ? PageId::Collaboration : PageId::History, m_id};
}
void HomePageConflictPage::load(bool restart, bool resumePreview)
{
    if (!m_active || m_busy) return;
    if (!restart && m_service->syncState(m_id) == BackupSyncState::Tracking)
    {
        const auto ctx = context();
        QTimer::singleShot(0, this, [this, ctx] { if (current(ctx)) emit navigate(returnRoute()); });
        return;
    }
    rememberPosition();
    const auto ctx = context();
    m_busy = true;
    m_prepared = {};
    ++m_contentRequest;
    updateActions();
    const auto reply = [this, ctx, resumePreview](const BackupResult<SyncResolutionSession> &result)
    {
        if (!current(ctx)) return;
        m_busy = false;
        if (!result.result.success)
        {
            m_session.stale = true;
            m_session.staleReason = result.result.message;
            emit notification(result.result);
            updateActions();
            return;
        }
        if (result.value.id.isEmpty())
        {
            emit notification(result.result);
            emit navigate(returnRoute());
            return;
        }
        acceptSession(result.value, true, resumePreview);
    };
    if (restart) m_service->prepareSyncResolution(m_id, this, reply, true);
    else m_service->syncResolution(m_id, this, reply);
}
void HomePageConflictPage::acceptSession(const SyncResolutionSession &session, bool restorePosition, bool resumePreview)
{
    m_session = session;
    m_cloudSession = session.mergeRef.isEmpty();
    ui->pageTitle->setText(session.mergeRef.isEmpty() ? QStringLiteral("处理同步差异") : QString("把“%1”的修改合并到“%2”").arg(session.remoteLabel, session.localLabel));
    m_prepared = {};
    if (restorePosition)
    {
        const auto position = m_positions.value(m_id);
        const auto path = position.session == session.id ? position.path : session.currentPath;
        m_file = 0;
        for (int i = 0; i < session.files.size(); ++i)
            if (session.files[i].path == path) m_file = i;
        m_hunk = qBound(0, position.session == session.id ? position.hunk : session.currentHunk,
                        qMax(0, int(session.files.value(m_file).hunks.size()) - 1));
        m_preview = resumePreview && !session.stale && session.remaining() == 0 && (session.total() == 0 || (position.session == session.id && position.preview));
    }
    if (m_preview && !session.stale)
    {
        preparePreview();
        return;
    }
    m_preview = false;
    populateFiles();
    showQuestion();
}
void HomePageConflictPage::populateFiles()
{
    const QSignalBlocker blocker(ui->files->selectionModel());
    m_files.clear();
    ui->filesLabel->setText(m_preview ? "最终改动" : "待处理文件");
    if (m_preview)
    {
        m_previewFiles = m_prepared.changes;
        for (const auto &file : m_session.files)
        {
            bool listed = false;
            for (const auto &change : m_previewFiles)
                if (change.path == file.path || change.path.startsWith(file.path + '/')) { listed = true; break; }
            if (!listed)
            {
                const bool absent = file.wholeFile && !(file.hunks[0].choice == ConflictChoice::Local ? file.localExists : file.remoteExists);
                m_previewFiles.append({absent ? "-" : "=", file.path, file.managed ? "保留当前源内容" : "仅影响备份仓库"});
            }
        }
        for (const auto &change : m_previewFiles)
        {
            auto *item = new QStandardItem(change.path);
            item->setEditable(false);
            item->setData(change.path, DiffFileRole::PathRole);
            item->setData(change.status, DiffFileRole::StatusRole);
            item->setData(change.summary.isEmpty() ? QString() : change.summary, DiffFileRole::SummaryRole);
            item->setToolTip(QString("%1 · %2%3").arg(statusText(change.status), change.path, change.summary.isEmpty() ? QString() : "\n" + change.summary));
            m_files.appendRow(item);
        }
        if (m_files.rowCount()) ui->files->setCurrentIndex(m_files.index(0, 0));
        ui->filesCount->setText(QString::number(m_files.rowCount()));
    }
    else
    {
        int resolvedFiles = 0;
        for (const auto &file : m_session.files)
        {
            int done = 0;
            for (const auto &hunk : file.hunks) done += hunk.choice != ConflictChoice::Unresolved;
            const bool fileResolved = (done == file.hunks.size());
            resolvedFiles += fileResolved;
            auto *item = new QStandardItem(file.path);
            item->setEditable(false);
            item->setData(file.path, DiffFileRole::PathRole);
            item->setData(fileResolved ? QStringLiteral("✓") : QStringLiteral("!"), DiffFileRole::StatusRole);
            item->setData(QString("已选 %1 / %2 处%3").arg(done).arg(file.hunks.size()).arg(file.managed ? QString() : QStringLiteral(" · 仅影响备份仓库")), DiffFileRole::SummaryRole);
            item->setToolTip(QString("%1\n%2/%3 处已确认%4").arg(file.path).arg(done).arg(file.hunks.size()).arg(file.managed ? QString() : "\n仅影响备份仓库"));
            m_files.appendRow(item);
        }
        ui->files->setCurrentIndex(m_files.index(m_file, 0));
        ui->filesCount->setText(m_session.files.isEmpty() ? QString() : QString("%1 / %2").arg(resolvedFiles).arg(m_session.files.size()));
    }
    ui->files->scrollTo(ui->files->currentIndex());
    ui->files->viewport()->update();
    updateActions();
}
void HomePageConflictPage::selectFile()
{
    if (m_preview) { showResult(); return; }
    if (!ui->files->currentIndex().isValid()) return;
    m_file = ui->files->currentIndex().row();
    m_hunk = 0;
    const auto file = m_session.files.value(m_file);
    for (int i = 0; i < file.hunks.size(); ++i)
        if (file.hunks[i].choice == ConflictChoice::Unresolved) { m_hunk = i; break; }
    showQuestion();
}
void HomePageConflictPage::showQuestion()
{
    const auto request = ++m_contentRequest;
    ui->modeStack->setCurrentWidget(ui->questionPage);
    if (m_session.files.isEmpty())
    {
        setFileTitle({});
        ui->positionLabel->clear();
        ui->diffView->showNotice(QStringLiteral("没有需要逐处选择的差异"), QStringLiteral("所有改动均可自动合并，点击“预览结果”查看最终改动。"));
        updateActions();
        return;
    }
    const auto &file = m_session.files[m_file];
    const auto &hunk = file.hunks[m_hunk];
    setFileTitle(file.path);
    ui->positionLabel->setText(QString("第 %1 / %2 处").arg(m_hunk + 1).arg(file.hunks.size()));
    ui->localChoice->setText((hunk.choice == ConflictChoice::Local ? QStringLiteral("✓ ") : QString()) + choiceText(file, true));
    ui->remoteChoice->setText((hunk.choice == ConflictChoice::Remote ? QStringLiteral("✓ ") : QString()) + choiceText(file, false));
    if (!m_session.mergeRef.isEmpty())
    {
        ui->localChoice->setText(ui->localChoice->text().replace("本地", "“" + m_session.localLabel + "”"));
        ui->remoteChoice->setText(ui->remoteChoice->text().replace("云端", "“" + m_session.remoteLabel + "”"));
    }
    const QSignalBlocker localBlock(ui->localChoice), remoteBlock(ui->remoteChoice);
    ui->localChoice->setChecked(hunk.choice == ConflictChoice::Local);
    ui->remoteChoice->setChecked(hunk.choice == ConflictChoice::Remote);
    if (file.wholeFile)
    {
        ui->diffView->showNotice(QStringLiteral("正在读取内容…"), QStringLiteral("正在加载双方内容以生成对比…"));
        loadTexts(ui->diffView, file.path, ConflictSide::Local, ConflictSide::Remote, m_session.localLabel, m_session.remoteLabel, request);
    }
    else
    {
        ui->diffView->setHunk(hunk.before, hunk.local, hunk.remote, hunk.after, 1,
                              file.path, m_service->sourcePath(m_id),
                              m_session.localLabel, m_session.remoteLabel);
    }
    updateActions();
}
void HomePageConflictPage::loadTexts(HomePageDiffView *view, const QString &path, std::optional<ConflictSide> oldSide, ConflictSide newSide,
                                     const QString &oldHeader, const QString &newHeader, quint64 request)
{
    // Both sides arrive independently; render once with whichever texts were read.
    struct Pair { QString texts[2]; QString error; int pending{0}; };
    auto pair = std::make_shared<Pair>();
    pair->pending = oldSide ? 2 : 1;
    const auto ctx = context();
    const auto session = m_session.id;
    const auto revision = m_session.revision;
    const auto read = [this, view, path, oldHeader, newHeader, request, pair, ctx, session, revision](ConflictSide side, int slot)
    {
        m_service->syncContent(m_id, session, path, side, this,
            [this, view, path, oldHeader, newHeader, request, pair, ctx, session, revision, slot](const BackupResult<ConflictContent> &reply)
        {
            if (!current(ctx) || session != m_session.id || revision != m_session.revision || request != m_contentRequest) return;
            if (reply.result.success) pair->texts[slot] = reply.value.text;
            else if (pair->error.isEmpty()) pair->error = reply.result.title + "：" + reply.result.message;
            if (--pair->pending) return;
            if (!pair->error.isEmpty()) view->showNotice(QStringLiteral("无法读取内容"), pair->error);
            else view->setTexts(pair->texts[0], pair->texts[1], path, m_service->sourcePath(m_id), oldHeader, newHeader);
        });
    };
    if (oldSide) read(*oldSide, 0);
    read(newSide, 1);
}
void HomePageConflictPage::choose(ConflictChoice choice)
{
    if (m_busy || m_session.stale || m_session.files.isEmpty()) return;
    const auto ctx = context();
    const auto &file = m_session.files[m_file];
    m_busy = true;
    updateActions();
    m_service->describe(m_service->chooseSyncResolution(m_id, m_session.id, m_session.revision, file.path, m_hunk, choice, this,
        [this, ctx](const BackupResult<SyncResolutionSession> &reply)
    {
        if (!current(ctx)) return;
        m_busy = false;
        if (!reply.result.success) { emit notification(reply.result); load(); return; }
        acceptSession(reply.value, false);
    }), "正在保存选择");
}
void HomePageConflictPage::moveQuestion(bool next)
{
    if (m_session.files.isEmpty()) return;
    QVector<QPair<int, int>> questions;
    int index = 0;
    for (int f = 0; f < m_session.files.size(); ++f)
        for (int h = 0; h < m_session.files[f].hunks.size(); ++h)
        {
            if (f == m_file && h == m_hunk) index = questions.size();
            questions.append({f, h});
        }
    int target = qMax(0, index - 1);
    if (next)
    {
        target = qMin(index + 1, int(questions.size()) - 1);
        for (int step = 1; step <= questions.size(); ++step)
        {
            const int candidate = (index + step) % questions.size();
            const auto [f, h] = questions[candidate];
            if (m_session.files[f].hunks[h].choice == ConflictChoice::Unresolved) { target = candidate; break; }
        }
    }
    m_file = questions[target].first;
    m_hunk = questions[target].second;
    const QSignalBlocker selection(ui->files->selectionModel());
    ui->files->setCurrentIndex(m_files.index(m_file, 0));
    ui->files->scrollTo(ui->files->currentIndex());
    showQuestion();
}
void HomePageConflictPage::preparePreview()
{
    if (m_busy || m_session.stale || m_session.id.isEmpty() || m_session.remaining()) return;
    const auto ctx = context();
    m_busy = true;
    updateActions();
    m_service->describe(m_service->prepareSyncApply(m_id, m_session.id, m_session.revision, this, [this, ctx](const BackupResult<PreparedSyncApply> &reply)
    {
        if (!current(ctx)) return;
        m_busy = false;
        if (!reply.result.success)
        {
            emit notification(reply.result);
            m_preview = false;
            // A failed Git/write operation need not invalidate the choices.
            // Revalidate without automatically retrying a conflict-free preview.
            load(false, false);
            return;
        }
        m_prepared = reply.value;
        m_preview = true;
        populateFiles();
        showResult();
    }), "正在生成合并结果");
}
void HomePageConflictPage::showResult()
{
    const auto request = ++m_contentRequest;
    ui->modeStack->setCurrentWidget(ui->previewPage);
    int added = 0, modified = 0, deleted = 0;
    for (const auto &change : m_prepared.changes)
    {
        added += change.status == "A";
        modified += change.status == "M";
        deleted += change.status == "D";
    }
    ui->previewSummary->setText(QString("新增 %1 · 修改 %2 · 删除 %3，包含自动合并的改动。%4").arg(added).arg(modified).arg(deleted).arg(m_cloudSession ? "确认后更新本机文件，并继续全量同步。" : "确认后更新源位置，并保存到正在使用的方案。"));
    const auto row = ui->files->currentIndex().row();
    const bool valid = row >= 0 && row < m_previewFiles.size();
    const auto change = m_previewFiles.value(row);
    setFileTitle(change.path);
    ui->positionLabel->setText(valid ? statusText(change.status) + (change.summary.isEmpty() ? QString() : " · " + change.summary) : QString());
    if (!valid)
        ui->previewDiffView->showNotice(QStringLiteral("源内容没有变化"), QStringLiteral("确认后保存同步版本关系并恢复自动备份。"));
    else if (change.status == "D")
        ui->previewDiffView->showNotice(QStringLiteral("文件将被删除"), QStringLiteral("应用同步后，源位置中的此文件将被删除。"));
    else if (change.status == "-")
        ui->previewDiffView->showNotice(QStringLiteral("继续保持删除"), QStringLiteral("源位置中已不存在此文件，应用后继续保持删除。"));
    else if (change.status == "=")
        ui->previewDiffView->showNotice(QStringLiteral("文件内容保留"), QStringLiteral("此文件未做更改，保留当前源内容。"));
    else
    {
        ui->previewDiffView->showNotice(QStringLiteral("正在读取合并结果…"), QStringLiteral("正在加载合并前后的内容…"));
        loadTexts(ui->previewDiffView, change.path, change.status == "A" ? std::nullopt : std::optional(ConflictSide::Local), ConflictSide::Result,
                  QStringLiteral("修改前 (本地)"), QStringLiteral("合并后的内容"), request);
    }
    updateActions();
}
void HomePageConflictPage::apply()
{
    if (m_busy || m_session.stale || m_prepared.commit.isEmpty()) return;
    const auto ctx = context();
    const auto prepared = m_prepared;
    int deletions = 0;
    for (const auto &change : prepared.changes) deletions += change.status == "D";
    const auto source = m_service->sourcePath(m_id);
    const auto question = QString("对象：%1\n源位置：%2\n\n最终结果包含 %3 个删除文件（含仅影响备份仓库的改动）。应用后保存本地版本并恢复自动备份。%4")
                              .arg(QFileInfo(source).fileName(), QDir::toNativeSeparators(source)).arg(deletions).arg(m_cloudSession ? "随后继续全量同步。" : "");
    const QPointer<HomePageConflictPage> guard(this);
    if (!confirmAction(this, question, "应用并保存版本") || !guard) return;
    if (!current(ctx) || m_prepared.commit != prepared.commit || m_session.revision != prepared.revision || m_session.id != prepared.sessionId) return;
    m_busy = true;
    updateActions();
    m_service->describe(m_service->applySync(prepared, this, [this, ctx, prepared](const OperationResult &result)
    {
        if (!current(ctx)) return;
        m_busy = false;
        emit notification(result);
        if (result.success)
        {
            m_positions.remove(m_id);
            m_session = {};
            m_prepared = {};
            auto route = returnRoute();
            route.commit = prepared.commit;
            emit navigate(route);
        }
        else load();
    }), "正在应用合并结果");
}
void HomePageConflictPage::setLoading(bool loading)
{
    if (m_loading == loading) return;
    m_loading = loading;
    emit loadingChanged(loading);
}
void HomePageConflictPage::setFileTitle(const QString &path)
{
    m_fileTitle = path;
    ui->fileTitle->setToolTip(path);
    ui->fileTitle->setText(ui->fileTitle->fontMetrics().elidedText(path, Qt::ElideMiddle, qMax(0, ui->fileTitle->width())));
}
void HomePageConflictPage::updateActions()
{
    const bool valid = m_active && !m_session.id.isEmpty();
    const bool editable = valid && !m_busy && !m_session.stale;
    const bool question = !m_session.files.isEmpty();
    const bool showingWork = (!m_preview && question) || (m_preview && !m_prepared.commit.isEmpty());
    const bool loading = m_active && m_busy && !showingWork;
    setLoading(loading);
    if (m_loadingSpinner) m_loadingSpinner->setSpinning(loading);
    if (loading)
        ui->modeStack->setCurrentWidget(m_loadingPage);
    else if (ui->modeStack->currentWidget() == m_loadingPage)
        ui->modeStack->setCurrentWidget(m_preview ? ui->previewPage : ui->questionPage);
    ui->progressLabel->setText(valid ? QString("已处理 %1 / %2 处").arg(m_session.total() - m_session.remaining()).arg(m_session.total()) : QString());
    ui->previewSummary->setVisible(m_preview && !loading);
    ui->staleBanner->setVisible(m_session.stale);
    ui->staleLabel->setText(m_session.staleReason.isEmpty() ? QStringLiteral("内容已在选择期间变化，需要重新分析后才能应用。")
                                                             : "需要重新分析后才能应用：" + m_session.staleReason);
    ui->reanalyzeButton->setEnabled(m_active && !m_busy && m_service->syncState(m_id) == BackupSyncState::ResolutionPending);
    ui->files->setEnabled(valid && !m_busy);
    ui->choiceBar->setVisible(question);
    ui->localChoice->setEnabled(editable && question);
    ui->remoteChoice->setEnabled(editable && question);

    // The footer reads as a wizard: 上一处 / 下一处 while choosing, 返回选择 / 应用 when previewing.
    const bool laterQuestion = m_file + 1 < m_session.files.size() || m_hunk + 1 < m_session.files.value(m_file).hunks.size();
    const bool otherUnresolved = m_session.remaining() > (m_session.files.value(m_file).hunks.value(m_hunk).choice == ConflictChoice::Unresolved ? 1 : 0);
    const bool canAdvance = laterQuestion || otherUnresolved;
    const bool previewReady = editable && m_session.remaining() == 0;
    const auto previewTip = m_session.remaining() ? QString("还有 %1 处未选择").arg(m_session.remaining()) : QString();
    const bool choosing = !m_preview && question;
    ui->previousButton->setVisible(choosing);
    ui->previousButton->setEnabled(valid && !m_busy && (m_file > 0 || m_hunk > 0));
    ui->nextButton->setVisible(choosing);
    ui->nextButton->setText(canAdvance ? QStringLiteral("下一处") : QStringLiteral("预览结果"));
    ui->nextButton->setEnabled(valid && !m_busy && (canAdvance || previewReady));
    ui->nextButton->setToolTip(canAdvance ? QString() : previewTip);
    ui->previewButton->setVisible(!m_preview && !question && !loading);
    ui->previewButton->setEnabled(previewReady);
    ui->previewButton->setToolTip(previewTip);
    ui->editChoicesButton->setVisible(m_preview && question);
    ui->editChoicesButton->setEnabled(!m_busy);
    ui->applyButton->setVisible(m_preview);
    ui->applyButton->setEnabled(editable && !m_prepared.commit.isEmpty());
}
void HomePageConflictPage::refreshTheme()
{
    ui->diffView->refreshTheme();
    ui->previewDiffView->refreshTheme();
    ui->files->viewport()->update();
}
void HomePageConflictPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateLayout();
}
void HomePageConflictPage::updateLayout()
{
    const bool narrow = width() < 720;
    const auto orientation = narrow ? Qt::Vertical : Qt::Horizontal;
    ui->filesPane->setMaximumHeight(narrow ? 100 : QWIDGETSIZE_MAX);
    ui->filesPane->setMinimumWidth(narrow ? 0 : 160);
    ui->filesLayout->setContentsMargins(0, 0, narrow ? 0 : 8, narrow ? 8 : 0);
    ui->workLayout->setContentsMargins(narrow ? 0 : 8, narrow ? 4 : 0, 0, 0);
    if (ui->splitter->orientation() != orientation)
    {
        ui->splitter->setOrientation(orientation);
        ui->splitter->setSizes(narrow ? QList<int>{88, 500} : QList<int>{180, 560});
    }
    const int margin = narrow ? 16 : 24;
    ui->pageLayout->setContentsMargins(margin, 12, margin, narrow ? 8 : 16);
    ui->pageLayout->setSpacing(narrow ? 8 : 12);
    QTimer::singleShot(0, this, [this]
    {
        // The list's viewport and the header width settle after the splitter/layout update.
        ui->files->scrollTo(ui->files->currentIndex());
        setFileTitle(m_fileTitle);
    });
}
