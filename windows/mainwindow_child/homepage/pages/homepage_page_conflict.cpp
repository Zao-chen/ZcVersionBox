#include "homepage_page_conflict.h"
#include "ui_homepage_page_conflict.h"
#include "windows/mainwindow_presentation.h"
#include <QFileInfo>
#include <QPointer>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTimer>

namespace
{
QString displayBytes(const QByteArray &bytes)
{
    // QTextDocument normalizes line endings. Use the same text for both the
    // display and cursor offsets; keep the original bytes in the saved choice.
    return QString::fromUtf8(bytes).replace("\r\n", "\n").replace('\r', '\n');
}
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
    UiStyle::text(ui->pageTitle, UiStyle::FontRole::Object);
    for (auto *label : {ui->progressLabel, ui->resultScope, ui->selectionLabel})
        UiStyle::text(label, UiStyle::FontRole::Caption, true);
    for (auto *editor : {ui->localContent, ui->remoteContent, ui->resultContent})
    {
        UiStyle::text(editor, UiStyle::FontRole::Code);
        editor->setLineWrapMode(QPlainTextEdit::WidgetWidth);
        editor->setWordWrapMode(QTextOption::WrapAnywhere);
    }
    ui->localContent->setAccessibleName("此电脑上的内容");
    ui->remoteContent->setAccessibleName("云端的内容");
    ui->resultContent->setAccessibleName("最终文件内容");
    ui->resultPath->setAccessibleName("预览文件路径");
    ui->files->setAccessibleName("同步差异文件");
    ui->files->setModel(&m_files);
    UiStyle::flatView(ui->files);
    ui->splitter->setStretchFactor(0, 0);
    ui->splitter->setStretchFactor(1, 1);
    ui->splitter->setSizes({180, 560});
    ui->localChoice->setCheckable(true);
    ui->remoteChoice->setCheckable(true);
    ui->expandContentButton->setCheckable(true);
    connect(ui->localChoice, &QPushButton::clicked, this, [this] { choose(ConflictChoice::Local); });
    connect(ui->remoteChoice, &QPushButton::clicked, this, [this] { choose(ConflictChoice::Remote); });
    connect(ui->files->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] { selectFile(); });
    connect(ui->previousButton, &QPushButton::clicked, this, [this] { moveQuestion(false); });
    connect(ui->nextButton, &QPushButton::clicked, this, [this] { moveQuestion(true); });
    connect(ui->expandContentButton, &QPushButton::toggled, this, [this] { showQuestion(); });
    connect(ui->previewButton, &QPushButton::clicked, this, &HomePageConflictPage::preparePreview);
    connect(ui->applyButton, &QPushButton::clicked, this, &HomePageConflictPage::apply);
    connect(ui->editChoicesButton, &QPushButton::clicked, this, [this]
    {
        m_preview = false;
        m_prepared = {};
        populateFiles();
        showQuestion();
    });
    connect(ui->laterButton, &QPushButton::clicked, this, [this] { emit navigate({PageId::Dashboard, m_id}); });
    connect(ui->reanalyzeButton, &QPushButton::clicked, this, [this]
    {
        const auto ctx = context();
        const QPointer<HomePageConflictPage> guard(this);
        if (confirmAction(this, "将重新获取云端并分析当前源内容。已有选择会清空，需要重新逐处确认。", "重新分析") && guard && current(ctx))
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
            ui->localContent->clear();
            ui->remoteContent->clear();
            ui->resultContent->clear();
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
    updateActions();
}
void HomePageConflictPage::setBackup(const QString &id)
{
    rememberPosition();
    ++m_context;
    ++m_contentRequest;
    m_id = id;
    m_repository = m_service->repositoryGeneration(id);
    m_active = true;
    m_session = {};
    m_prepared = {};
    m_busy = false;
    m_preview = false;
    m_files.clear();
    ui->localContent->clear();
    ui->remoteContent->clear();
    ui->resultContent->clear();
    load();
}
void HomePageConflictPage::load(bool restart, bool resumePreview)
{
    if (!m_active || m_busy) return;
    if (!restart && m_service->syncState(m_id) == BackupSyncState::Tracking)
    {
        const auto ctx = context();
        QTimer::singleShot(0, this, [this, ctx] { if (current(ctx)) emit navigate({PageId::History, ctx.id}); });
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
            emit navigate({PageId::History, m_id});
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
            const auto status = change.status == "D" ? "删除" : change.status == "A" ? "新增" : change.status == "=" ? "保留" : change.status == "-" ? "保持删除" : "修改";
            auto *item = new QStandardItem(QString("%1 · %2").arg(status, change.path));
            item->setToolTip(item->text() + (change.summary.isEmpty() ? QString() : '\n' + change.summary));
            m_files.appendRow(item);
        }
        if (m_files.rowCount()) ui->files->setCurrentIndex(m_files.index(0, 0));
    }
    else
    {
        for (const auto &file : m_session.files)
        {
            int done = 0;
            for (const auto &hunk : file.hunks) done += hunk.choice != ConflictChoice::Unresolved;
            auto *item = new QStandardItem(QString("%1/%2 · %3").arg(done).arg(file.hunks.size()).arg(file.path));
            item->setToolTip(item->text() + (file.managed ? QString() : "\n仅影响备份仓库"));
            m_files.appendRow(item);
        }
        ui->files->setCurrentIndex(m_files.index(m_file, 0));
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
    const QSignalBlocker blocker(ui->expandContentButton);
    ui->expandContentButton->setChecked(false);
    showQuestion();
}
void HomePageConflictPage::showQuestion()
{
    const auto request = ++m_contentRequest;
    ui->modeStack->setCurrentWidget(ui->questionPage);
    if (m_session.files.isEmpty()) { updateActions(); return; }
    const auto &file = m_session.files[m_file];
    const auto &hunk = file.hunks[m_hunk];
    ui->questionTitle->setText(QString("%1 · 第 %2 / %3 处").arg(file.path).arg(m_hunk + 1).arg(file.hunks.size()));
    ui->localChoice->setText((hunk.choice == ConflictChoice::Local ? QStringLiteral("✓ ") : QString()) + choiceText(file, true));
    ui->remoteChoice->setText((hunk.choice == ConflictChoice::Remote ? QStringLiteral("✓ ") : QString()) + choiceText(file, false));
    const QSignalBlocker localBlock(ui->localChoice), remoteBlock(ui->remoteChoice);
    ui->localChoice->setChecked(hunk.choice == ConflictChoice::Local);
    ui->remoteChoice->setChecked(hunk.choice == ConflictChoice::Remote);
    ui->selectionLabel->setText(hunk.choice == ConflictChoice::Unresolved ? "尚未选择" : hunk.choice == ConflictChoice::Local ? "已选择本地" : "已选择云端");
    const bool full = file.wholeFile || ui->expandContentButton->isChecked();
    ui->expandContentButton->setVisible(!file.wholeFile);
    ui->expandContentButton->setText(full ? "收起完整内容" : "展开完整内容");
    for (auto *editor : {ui->localContent, ui->remoteContent}) editor->setExtraSelections({});
    if (full)
    {
        loadContent(ui->localContent, file.path, ConflictSide::Local, request);
        loadContent(ui->remoteContent, file.path, ConflictSide::Remote, request);
    }
    else
    {
        const auto show = [&](QPlainTextEdit *editor, const QByteArray &part)
        {
            const auto before = displayBytes(hunk.before), text = displayBytes(part);
            editor->setPlainText(before + text + displayBytes(hunk.after));
            QTextEdit::ExtraSelection highlight;
            highlight.cursor = editor->textCursor();
            highlight.cursor.setPosition(before.size());
            highlight.cursor.setPosition(before.size() + text.size(), QTextCursor::KeepAnchor);
            highlight.format.setBackground(UiStyle::colors().selected);
            editor->setExtraSelections({highlight});
        };
        show(ui->localContent, hunk.local);
        show(ui->remoteContent, hunk.remote);
    }
    updateActions();
}
void HomePageConflictPage::loadContent(QPlainTextEdit *editor, const QString &path, ConflictSide side, quint64 request)
{
    editor->setPlainText("正在读取内容…");
    const auto ctx = context();
    const auto session = m_session.id;
    const auto revision = m_session.revision;
    m_service->syncContent(m_id, session, path, side, this, [this, ctx, session, revision, request, editor](const BackupResult<ConflictContent> &reply)
    {
        if (!current(ctx) || session != m_session.id || revision != m_session.revision || request != m_contentRequest) return;
        editor->setPlainText(reply.result.success ? reply.value.text : reply.result.title + "：" + reply.result.message);
    });
}
void HomePageConflictPage::choose(ConflictChoice choice)
{
    if (m_busy || m_session.stale || m_session.files.isEmpty()) return;
    const auto ctx = context();
    const auto &file = m_session.files[m_file];
    m_busy = true;
    updateActions();
    m_service->chooseSyncResolution(m_id, m_session.id, m_session.revision, file.path, m_hunk, choice, this,
        [this, ctx](const BackupResult<SyncResolutionSession> &reply)
    {
        if (!current(ctx)) return;
        m_busy = false;
        if (!reply.result.success) { emit notification(reply.result); load(); return; }
        acceptSession(reply.value, false);
    });
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
    const QSignalBlocker selection(ui->files->selectionModel()), expansion(ui->expandContentButton);
    ui->files->setCurrentIndex(m_files.index(m_file, 0));
    ui->files->scrollTo(ui->files->currentIndex());
    ui->expandContentButton->setChecked(false);
    showQuestion();
}
void HomePageConflictPage::preparePreview()
{
    if (m_busy || m_session.stale || m_session.id.isEmpty() || m_session.remaining()) return;
    const auto ctx = context();
    m_busy = true;
    updateActions();
    m_service->prepareSyncApply(m_id, m_session.id, m_session.revision, this, [this, ctx](const BackupResult<PreparedSyncApply> &reply)
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
    });
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
    ui->previewSummary->setText(QString("新增 %1 · 修改 %2 · 删除 %3\n包含自动合并的改动。确认后更新源位置并保存本地版本，上传仍需单独操作。").arg(added).arg(modified).arg(deleted));
    const auto row = ui->files->currentIndex().row();
    const bool valid = row >= 0 && row < m_previewFiles.size();
    const auto change = m_previewFiles.value(row);
    ui->resultPath->setText(change.path);
    ui->resultPath->setCursorPosition(0);
    ui->resultScope->setText(change.summary);
    if (!valid) ui->resultContent->setPlainText("源内容没有变化。确认后保存同步版本关系并恢复自动备份。");
    else if (change.status == "D") ui->resultContent->setPlainText("此文件将在应用时删除。");
    else if (change.status == "-") ui->resultContent->setPlainText("源位置中已不存在此文件，应用后继续保持删除。");
    else loadContent(ui->resultContent, change.path, ConflictSide::Result, request);
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
    const auto question = QString("对象：%1\n源位置：%2\n\n最终结果包含 %3 个删除文件（含仅影响备份仓库的改动）。应用后保存本地版本并恢复自动备份。云端上传需另行操作。")
                              .arg(QFileInfo(source).fileName(), QDir::toNativeSeparators(source)).arg(deletions);
    const QPointer<HomePageConflictPage> guard(this);
    if (!confirmAction(this, question, "应用并保存版本") || !guard) return;
    if (!current(ctx) || m_prepared.commit != prepared.commit || m_session.revision != prepared.revision || m_session.id != prepared.sessionId) return;
    m_busy = true;
    updateActions();
    m_service->applySync(prepared, this, [this, ctx, prepared](const OperationResult &result)
    {
        if (!current(ctx)) return;
        m_busy = false;
        emit notification(result);
        if (result.success)
        {
            m_positions.remove(m_id);
            m_session = {};
            m_prepared = {};
            emit navigate({PageId::History, ctx.id, prepared.commit});
        }
        else load();
    });
}
void HomePageConflictPage::updateActions()
{
    const bool valid = m_active && !m_session.id.isEmpty();
    const bool editable = valid && !m_busy && !m_session.stale;
    const bool question = !m_session.files.isEmpty();
    ui->progressLabel->setText(valid ? QString("已处理 %1 / %2 处").arg(m_session.total() - m_session.remaining()).arg(m_session.total()) : QString());
    ui->reanalyzeButton->setVisible(m_session.stale);
    ui->reanalyzeButton->setEnabled(m_active && !m_busy && m_service->syncState(m_id) == BackupSyncState::ResolutionPending);
    ui->reanalyzeButton->setToolTip(m_session.stale ? m_session.staleReason : QString());
    ui->files->setEnabled(valid && !m_busy);
    ui->questionPage->setSizePolicy(QSizePolicy::Preferred, m_preview ? QSizePolicy::Ignored : QSizePolicy::Preferred);
    ui->previewPage->setSizePolicy(QSizePolicy::Preferred, m_preview ? QSizePolicy::Preferred : QSizePolicy::Ignored);
    ui->questionPage->setVisible(!m_preview && question);
    ui->localChoice->setEnabled(editable && question);
    ui->remoteChoice->setEnabled(editable && question);
    ui->previousButton->setEnabled(valid && !m_busy && question && (m_file > 0 || m_hunk > 0));
    const bool laterQuestion = m_file + 1 < m_session.files.size() || m_hunk + 1 < m_session.files.value(m_file).hunks.size();
    const bool otherUnresolved = m_session.remaining() > (m_session.files.value(m_file).hunks.value(m_hunk).choice == ConflictChoice::Unresolved ? 1 : 0);
    ui->nextButton->setEnabled(valid && !m_busy && question && (laterQuestion || otherUnresolved));
    ui->expandContentButton->setEnabled(valid && !m_busy);
    ui->previewButton->setVisible(!m_preview);
    ui->previewButton->setEnabled(editable && m_session.remaining() == 0);
    ui->previewButton->setToolTip(m_session.remaining() ? QString("还有 %1 处未选择").arg(m_session.remaining()) : QString());
    ui->editChoicesButton->setVisible(m_preview && question);
    ui->editChoicesButton->setEnabled(!m_busy);
    ui->applyButton->setVisible(m_preview);
    ui->applyButton->setEnabled(editable && !m_prepared.commit.isEmpty());
}
void HomePageConflictPage::refreshTheme()
{
    for (auto *editor : {ui->localContent, ui->remoteContent})
    {
        auto selections = editor->extraSelections();
        for (auto &selection : selections) selection.format.setBackground(UiStyle::colors().selected);
        editor->setExtraSelections(selections);
    }
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
    ui->sidesLayout->setDirection(narrow ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    ui->filesPane->setMaximumHeight(narrow ? 100 : QWIDGETSIZE_MAX);
    ui->filesPane->setMinimumWidth(narrow ? 0 : 160);
    ui->filesLayout->setContentsMargins(0, 0, narrow ? 0 : 8, 0);
    ui->scrollLayout->setContentsMargins(narrow ? 0 : 8, 0, 0, 0);
    if (ui->splitter->orientation() != orientation)
    {
        ui->splitter->setOrientation(orientation);
        ui->splitter->setSizes(narrow ? QList<int>{88, 500} : QList<int>{180, 560});
    }
    const int margin = narrow ? 16 : 24;
    ui->pageLayout->setContentsMargins(margin, 12, margin, 16);
    QTimer::singleShot(0, this, [this]
    {
        // The list's viewport shrinks after the splitter/layout update.
        ui->files->scrollTo(ui->files->currentIndex());
    });
}
