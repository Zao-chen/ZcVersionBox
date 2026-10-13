#include "homepage_page_preview.h"
#include "ui_homepage_page_preview.h"
#include "windows/mainwindow_presentation.h"
#include <QAction>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShortcut>
#include <QMenu>
#include <QMessageBox>
#include <QInputDialog>
#include <QPointer>
#include <QClipboard>
#include <QApplication>
#include <oclero/qlementine/widgets/LoadingSpinner.hpp>

namespace
{
constexpr int PathRole = Qt::UserRole + 1;
QString relativeTime(const QDateTime &time)
{
    const auto seconds = qMax<qint64>(0, time.secsTo(QDateTime::currentDateTimeUtc()));
    if (seconds < 60) return "刚刚";
    if (seconds < 3600) return QString("%1 分钟前").arg(seconds / 60);
    if (seconds < 86400) return QString("%1 小时前").arg(seconds / 3600);
    if (seconds < 86400 * 30) return QString("%1 天前").arg(seconds / 86400);
    return time.toLocalTime().toString("yyyy-MM-dd");
}
}
HomePagePreviewPage::HomePagePreviewPage(BackupService *service, QWidget *parent)
    : QWidget(parent), ui(new Ui::HomePagePreviewPage), m_service(service), m_watcher(this)
{
    ui->setupUi(this);
    ui->pageLayout->setStretch(ui->pageLayout->indexOf(ui->contentStack), 1);
    ui->pageLayout->setAlignment(ui->commitButton, Qt::AlignLeft);
    ui->statusLabel->hide();
    m_model.setHorizontalHeaderLabels({"名称", "最近提交说明", "提交时间"});
    ui->files->setModel(&m_model);
    UiStyle::flatView(ui->files);
    ui->files->setAccessibleName("本地文件列表");
    ui->files->header()->setStretchLastSection(false);
    ui->files->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    ui->files->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    ui->files->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    ui->files->setColumnWidth(0, 240);
    ui->editor->setReadOnly(true);
    ui->editor->setAccessibleName("文件内容");
    ui->markdown->setAccessibleName("Markdown 预览");
    m_refresh = UiStyle::action(this, "refreshPreviewAction", "刷新预览", "refresh");
    m_refresh->setProperty("iconOnly", true);
    m_open = UiStyle::action(this, "openPreviewSourceAction", "用系统应用打开", "external");
    connect(m_refresh, &QAction::triggered, this, [this] { requestLeave([this] { m_editing = false; refresh(); }); });
    connect(m_open, &QAction::triggered, this, [this] {
        const auto path = sourcePreviewPath(m_directory.location);
        if (m_valid && path.result.success) openLocalPath(this, path.value);
    });
    m_new = UiStyle::action(this, "newSourceEntryAction", "新建", "add");
    auto *newMenu = new QMenu(this);
    newMenu->addAction("新建文件…", this, [this] { createEntry(false); });
    newMenu->addAction("新建文件夹…", this, [this] { createEntry(true); });
    m_new->setMenu(newMenu);
    m_edit = UiStyle::action(this, "editSourceAction", "编辑", "edit");
    m_save = UiStyle::action(this, "saveSourceAction", "保存");
    m_cancel = UiStyle::action(this, "cancelSourceEditAction", "取消");
    m_manage = UiStyle::action(this, "manageSourceEntryAction", "更多", "more");
    for (auto *action : {m_edit, m_save, m_cancel, m_manage})
    {
        auto *button = UiStyle::toolButton(ui->fileHeader, action);
        button->setObjectName(action->objectName() + "Button");
        ui->fileHeaderLayout->addWidget(button);
        connect(action, &QAction::changed, button, [button, action] { button->setVisible(action->isVisible()); });
    }
    connect(m_edit, &QAction::triggered, this, &HomePagePreviewPage::beginEdit);
    connect(m_save, &QAction::triggered, this, [this] { save(); });
    connect(m_cancel, &QAction::triggered, this, [this] { requestLeave([this] { discard(); refresh(); }); });
    connect(m_manage, &QAction::triggered, this, [this] { entryMenu(m_content.location, QCursor::pos()); });
    connect(ui->files, &QTreeView::customContextMenuRequested, this, [this](const QPoint &point) {
        const auto index = ui->files->indexAt(point);
        if (!index.isValid()) return;
        auto location = m_directory.location;
        location.path = index.siblingAtColumn(0).data(PathRole).toString();
        entryMenu(location, ui->files->viewport()->mapToGlobal(point));
    });
    auto *saveShortcut = new QShortcut(QKeySequence::Save, this);
    saveShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(saveShortcut, &QShortcut::activated, this, [this] { if (m_editing) save(); });
    connect(ui->editor->document(), &QTextDocument::modificationChanged, this, &HomePagePreviewPage::updateActions);
    connect(service, &BackupService::busyChanged, this, &HomePagePreviewPage::updateActions);
    connect(service, &BackupService::sourceChanged, this, [this](const QString &id) { if (id == m_id && m_active) m_refreshTimer.start(); });
    connect(ui->files, &QTreeView::clicked, this, [this](const QModelIndex &index) {
        const auto path = index.siblingAtColumn(0).data(PathRole).toString();
        if (index.column() == 1)
        {
            const auto revision = m_changes.value(path);
            if (!revision.hash.isEmpty()) emit navigate({PageId::History, m_id, revision.hash});
        }
        else openPath(path);
    });
    connect(ui->files, &QTreeView::activated, this, [this](const QModelIndex &index) {
        openPath(index.siblingAtColumn(0).data(PathRole).toString());
    });
    connect(ui->commitButton, &QPushButton::clicked, this, [this] {
        const auto revision = m_changes.value(m_path);
        if (!revision.hash.isEmpty()) emit navigate({PageId::History, m_id, revision.hash});
    });
    connect(ui->markdownButton, &QToolButton::toggled, this, &HomePagePreviewPage::showContent);
    connect(ui->markdown, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) {
        if (url.toString().startsWith('#')) { ui->markdown->scrollToAnchor(url.fragment()); return; }
        const auto target = ui->markdown->localTarget(url);
        if (target.result.success) openPath(target.value); else emit notification(target.result);
    });
    connect(ui->fitButton, &QPushButton::clicked, this, [this] { m_fit = true; showImage(); });
    connect(ui->actualButton, &QPushButton::clicked, this, [this] { m_fit = false; m_zoom = 1; showImage(); });
    connect(ui->zoomIn, &QPushButton::clicked, this, [this] { m_fit = false; m_zoom = qMin(4.0, m_zoom * 1.25); showImage(); });
    connect(ui->zoomOut, &QPushButton::clicked, this, [this] { m_fit = false; m_zoom = qMax(0.1, m_zoom / 1.25); showImage(); });
    connect(ui->findNext, &QToolButton::clicked, this, [this] { find(false); });
    connect(ui->findPrevious, &QToolButton::clicked, this, [this] { find(true); });
    connect(ui->findEdit, &QLineEdit::returnPressed, this, [this] { find(false); });
    connect(ui->findClose, &QToolButton::clicked, ui->findBar, &QWidget::hide);
    auto *search = new QShortcut(QKeySequence::Find, this);
    search->setContext(Qt::WidgetWithChildrenShortcut);
    connect(search, &QShortcut::activated, this, [this] {
        if (m_content.textual) { ui->findBar->show(); ui->findEdit->setFocus(); ui->findEdit->selectAll(); }
    });
    m_spinner = new oclero::qlementine::LoadingSpinner(this);
    m_spinner->setFixedSize(16, 16);
    ui->fileHeaderLayout->insertWidget(0, m_spinner);
    m_refreshTimer.setSingleShot(true); m_refreshTimer.setInterval(250);
    connect(&m_refreshTimer, &QTimer::timeout, this, &HomePagePreviewPage::refresh);
    connect(&m_watcher, &BackupDirectoryWatcher::pathsChanged, this, [this] { if (m_active) m_refreshTimer.start(); });
    connect(service, &BackupService::repositoryChanged, this, [this](const QString &id) {
        if (m_active && id == m_id) m_refreshTimer.start();
    });
    ui->findBar->hide();
    refreshTheme();
    setLoading(false);
}
HomePagePreviewPage::~HomePagePreviewPage() { deactivate(); }
QList<QAction *> HomePagePreviewPage::toolbarActions() const { return {m_new, m_open, m_refresh}; }
void HomePagePreviewPage::refreshTheme()
{
    UiStyle::text(ui->editor, UiStyle::FontRole::Code);
    UiStyle::text(ui->files, UiStyle::FontRole::Body);
    UiStyle::text(ui->statusLabel, UiStyle::FontRole::Caption, true);
    UiStyle::text(ui->fileInfo, UiStyle::FontRole::Caption, true);
    UiStyle::text(ui->commitButton, UiStyle::FontRole::Caption, true);
    for (auto *action : {m_open, m_refresh, m_new, m_edit, m_manage}) action->setIcon(UiStyle::icon(action->property("iconName").toString()));
    for (int row = 0; row < m_model.rowCount(); ++row)
        m_model.item(row)->setIcon(UiStyle::icon(m_directory.children.value(row).directory ? "folder" : "file"));
}
void HomePagePreviewPage::rememberScroll()
{
    if (!m_valid) return;
    const auto key = m_id + QChar(0x1f) + m_path;
    m_rawModes[key] = ui->markdownButton->isChecked();
    auto *view = m_directory.entry.directory ? static_cast<QAbstractScrollArea *>(ui->files) : qobject_cast<QAbstractScrollArea *>(ui->contentStack->currentWidget());
    if (view) m_scrolls[key + QChar(0x1f) + view->objectName()] = {view->horizontalScrollBar()->value(), view->verticalScrollBar()->value()};
}
void HomePagePreviewPage::restoreScroll()
{
    auto *view = m_directory.entry.directory ? static_cast<QAbstractScrollArea *>(ui->files) : qobject_cast<QAbstractScrollArea *>(ui->contentStack->currentWidget());
    if (!view) return;
    const auto point = m_scrolls.value(m_id + QChar(0x1f) + m_path + QChar(0x1f) + view->objectName());
    view->horizontalScrollBar()->setValue(point.x()); view->verticalScrollBar()->setValue(point.y());
}

void HomePagePreviewPage::setBackup(const QString &id, const QString &path)
{
    if (hasUnsavedChanges() && m_id == id && m_path == path)
    {
        m_active = true;
        const auto source = sourcePreviewPath(m_directory.location);
        if (source.result.success) m_watcher.addPaths({QFileInfo(source.value).absolutePath()});
        status("备份对象已更新，当前编辑内容已保留。"); updateActions(); return;
    }
    rememberScroll();
    m_editing = m_newFile = false; m_statusText.clear(); ui->editor->setReadOnly(true);
    m_id = id; m_path = path; m_paths[id] = path; m_active = true;
    m_valid = false; m_fit = true; m_zoom = 1;
    ui->findBar->hide(); ui->markdownButton->setChecked(m_rawModes.value(m_id + QChar(0x1f) + m_path));
    refresh();
}
void HomePagePreviewPage::deactivate()
{
    rememberScroll(); m_active = false; ++m_request; ++m_changesRequest;
    m_refreshTimer.stop(); m_watcher.clear();
    for (const auto task : m_tasks) m_service->cancel(task);
    m_tasks.clear(); m_spinner->setSpinning(false);
}
void HomePagePreviewPage::openPath(const QString &path)
{
    Route route{PageId::Preview, m_id}; route.path = path;
    emit navigate(route);
}
void HomePagePreviewPage::setLoading(bool loading)
{
    m_loading = loading; m_spinner->setVisible(loading); m_spinner->setSpinning(loading && m_active);
    updateActions();
}
void HomePagePreviewPage::refresh()
{
    if (!m_active) return;
    if (m_saving || m_mutating) return;
    if (m_editing)
    {
        status("源内容或备份状态已更新；保存前将核对文件，当前编辑内容已保留。");
        ++m_changesRequest; m_changesHead.clear(); loadChanges(); updateActions(); return;
    }
    m_refreshTimer.stop();
    rememberScroll();
    for (const auto task : m_tasks) m_service->cancel(task);
    m_tasks.clear();
    const auto request = ++m_request;
    ++m_changesRequest;
    setLoading(true); m_valid = false; m_content = {}; m_changes.clear(); m_changesHead.clear();
    ui->fileHeader->show(); ui->fileInfo->setText("本地最新内容"); ui->statusLabel->clear(); ui->statusLabel->hide();
    ui->imageTools->hide(); ui->markdownButton->hide(); ui->commitButton->hide();
    ui->notice->setText("正在读取源内容…"); ui->contentStack->setCurrentWidget(ui->notice);
    breadcrumbs();
    m_tasks.append(m_service->sourceDirectory(m_id, m_path, this, [this, request](const BackupResult<SourceDirectory> &reply) {
        if (!m_active || request != m_request) return;
        if (!reply.result.success) { ui->notice->setText(reply.result.message); setLoading(false); return; }
        m_directory = reply.value; m_valid = true;
        m_watcher.clear();
        const auto absolute = m_path.isEmpty() ? m_directory.location.root : QDir(m_directory.location.root).filePath(m_path);
        auto watched = QStringList{QFileInfo(absolute).absolutePath()};
        if (m_directory.entry.directory) watched.append(absolute);
        watched.removeDuplicates(); m_watcher.addPaths(watched);
        ui->commitButton->show();
        ui->commitButton->setText("正在读取提交信息…"); ui->commitButton->setEnabled(false);
        loadChanges();
        if (m_directory.entry.directory)
        {
            m_model.removeRows(0, m_model.rowCount());
            for (const auto &entry : m_directory.children)
            {
                auto *name = new QStandardItem(UiStyle::icon(entry.directory ? "folder" : "file"), entry.name);
                name->setData(entry.path, PathRole); name->setToolTip(entry.link ? "链接不支持在预览中打开" : entry.name);
                name->setSizeHint(QSize(0, UiStyle::rowHeight(40, ui->files->font())));
                m_model.appendRow({name, new QStandardItem(entry.link ? "链接不支持预览" : "正在读取…"), new QStandardItem(entry.link ? "—" : "")});
            }
            ui->fileInfo->setText(QString("本地最新内容 · %1 个项目").arg(m_directory.children.size()));
            if (m_directory.children.isEmpty()) { ui->notice->setText("此文件夹为空"); ui->contentStack->setCurrentWidget(ui->notice); }
            else ui->contentStack->setCurrentWidget(ui->directoryPage);
            setLoading(false); restoreScroll();
        }
        else
        {
            ui->fileInfo->setText(QString("本地最新内容 · %1").arg(formatBytes(m_directory.entry.size)));
            m_tasks.append(m_service->sourceContent(m_id, m_path, this, [this, request](const BackupResult<SourceContent> &content) {
                if (!m_active || request != m_request) return;
                if (!content.result.success) { ui->notice->setText(content.result.message); setLoading(false); return; }
                m_content = content.value;
                ui->editor->setPlainText(m_content.text); ui->editor->document()->setModified(false);
                if (m_content.textual) ui->markdown->setContent(m_content);
                setLoading(false); showContent(); restoreScroll(); updateActions();
            }));
        }
    }));
}
void HomePagePreviewPage::loadChanges(int offset)
{
    if (!m_active || !m_valid) return;
    const auto request = m_request, changesRequest = m_changesRequest;
    QStringList paths{m_path};
    for (const auto &entry : m_directory.children) if (!entry.link) paths.append(entry.path);
    const auto batch = paths.mid(offset, 64);
    if (batch.isEmpty()) return;
    m_tasks.append(m_service->sourceChanges(m_id, batch, this, [this, request, changesRequest, offset](const BackupResult<SourceChanges> &reply) {
        if (!m_active || request != m_request || changesRequest != m_changesRequest) return;
        if (!reply.result.success)
        {
            ui->commitButton->setText("提交信息暂不可用"); ui->commitButton->setEnabled(false);
            for (int row = 0; row < m_model.rowCount(); ++row)
                if (!m_changes.contains(m_model.item(row)->data(PathRole).toString())) m_model.item(row, 1)->setText("暂不可用");
            return;
        }
        if (!m_changesHead.isEmpty() && m_changesHead != reply.value.head) { m_refreshTimer.start(); return; }
        m_changesHead = reply.value.head;
        for (auto it = reply.value.revisions.cbegin(); it != reply.value.revisions.cend(); ++it) m_changes.insert(it.key(), it.value());
        updateChanges(); loadChanges(offset + 64);
    }));
}
void HomePagePreviewPage::updateChanges()
{
    const auto revision = m_changes.value(m_path);
    const auto caption = revision.hash.isEmpty() ? QString("暂无备份记录") : revision.message + "  ·  " + relativeTime(revision.committedAt);
    ui->commitButton->setText(ui->commitButton->fontMetrics().elidedText(caption, Qt::ElideRight, qMax(120, width() - 80)));
    ui->commitButton->setToolTip(revision.committedAt.toLocalTime().toString("yyyy-MM-dd HH:mm:ss"));
    ui->commitButton->setEnabled(!revision.hash.isEmpty());
    for (int row = 0; row < m_model.rowCount(); ++row)
    {
        const auto path = m_model.item(row)->data(PathRole).toString();
        if (!m_changes.contains(path)) continue;
        const auto item = m_changes.value(path);
        m_model.item(row, 1)->setText(item.hash.isEmpty() ? "暂无备份记录" : item.message);
        m_model.item(row, 1)->setToolTip(item.message);
        m_model.item(row, 2)->setText(item.hash.isEmpty() ? "—" : relativeTime(item.committedAt));
        m_model.item(row, 2)->setToolTip(item.committedAt.toLocalTime().toString("yyyy-MM-dd HH:mm:ss"));
    }
}
void HomePagePreviewPage::showContent()
{
    if (!m_valid || m_directory.entry.directory) return;
    const auto name = m_directory.entry.name.toLower();
    const bool markdown = m_content.textual && (name.endsWith(".md") || name.endsWith(".markdown"));
    ui->markdownButton->setVisible(markdown && !m_editing);
    status(m_statusText.isEmpty() ? m_content.notice : m_statusText);
    ui->imageTools->setVisible(!m_content.image.isNull());
    if (m_content.textual) ui->contentStack->setCurrentWidget(markdown && !m_editing && !ui->markdownButton->isChecked() ? static_cast<QWidget *>(ui->markdown) : ui->editor);
    else if (!m_content.image.isNull()) { ui->contentStack->setCurrentWidget(ui->imageScroll); showImage(); }
    else { ui->notice->setText(m_content.notice); ui->contentStack->setCurrentWidget(ui->notice); }
}
void HomePagePreviewPage::showImage()
{
    if (m_content.image.isNull()) return;
    auto size = m_content.image.size();
    if (m_fit) size.scale(ui->imageScroll->viewport()->size() - QSize(16, 16), Qt::KeepAspectRatio);
    else size *= m_zoom;
    // Keep zoom previews bounded even for large source images.
    if (qint64(size.width()) * size.height() > SourcePreview::PixelLimit) size.scale(4096, 4096, Qt::KeepAspectRatio);
    ui->imageLabel->setPixmap(QPixmap::fromImage(m_content.image).scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    ui->imageLabel->resize(size);
}
void HomePagePreviewPage::find(bool previous)
{
    const auto flags = previous ? QTextDocument::FindBackward : QTextDocument::FindFlags{};
    auto *view = ui->contentStack->currentWidget() == ui->markdown ? static_cast<QWidget *>(ui->markdown) : static_cast<QWidget *>(ui->editor);
    bool found = view == ui->markdown ? ui->markdown->find(ui->findEdit->text(), flags) : ui->editor->find(ui->findEdit->text(), flags);
    if (!found)
    {
        if (view == ui->markdown) { ui->markdown->moveCursor(previous ? QTextCursor::End : QTextCursor::Start); ui->markdown->find(ui->findEdit->text(), flags); }
        else { ui->editor->moveCursor(previous ? QTextCursor::End : QTextCursor::Start); ui->editor->find(ui->findEdit->text(), flags); }
    }
}
void HomePagePreviewPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    const auto margin = width() < 640 ? 16 : 24;
    ui->pageLayout->setContentsMargins(margin, 8, margin, 16);
    ui->files->setColumnWidth(0, qBound(120, width() / 3, 300));
    showImage();
    if (m_valid) updateChanges();
}
void HomePagePreviewPage::breadcrumbs()
{
    while (auto *item = ui->breadcrumbLayout->takeAt(0)) { delete item->widget(); delete item; }
    const auto add = [this](const QString &name, const QString &path) {
        auto *button = new QToolButton(ui->breadcrumbHost); button->setText(name); button->setToolTip(name); button->setAutoRaise(true);
        UiStyle::text(button, UiStyle::FontRole::Body); ui->breadcrumbLayout->addWidget(button);
        connect(button, &QToolButton::clicked, this, [this, path] { openPath(path); });
    };
    add(QFileInfo(m_service->sourcePath(m_id)).fileName(), {});
    QString path;
    for (const auto &part : m_path.split('/', Qt::SkipEmptyParts))
    {
        auto *separator = new QLabel("/", ui->breadcrumbHost); UiStyle::text(separator, UiStyle::FontRole::Caption, true); ui->breadcrumbLayout->addWidget(separator);
        path += (path.isEmpty() ? "" : "/") + part; add(part, path);
    }
    ui->breadcrumbLayout->addStretch();
}

void HomePagePreviewPage::status(const QString &text)
{
    ui->statusLabel->setText(text); ui->statusLabel->setVisible(!text.isEmpty());
}
bool HomePagePreviewPage::hasUnsavedChanges() const
{
    return m_editing && (m_newFile || ui->editor->document()->isModified());
}
void HomePagePreviewPage::updateActions()
{
    if (!m_edit) return;
    const bool tracking = m_service->contains(m_id) && m_service->syncState(m_id) == BackupSyncState::Tracking;
    const auto context = m_service->branchContext(m_id);
    const auto original = m_content.location.context;
    const bool contextMatches = !m_editing || (context.ref == original.ref && context.version == original.version && context.generation == original.generation);
    const bool ready = m_active && m_valid && !m_loading && !m_saving && !m_mutating;
    m_open->setEnabled(ready && !m_newFile);
    m_refresh->setEnabled(m_active && !m_loading && !m_saving && !m_mutating);
    m_new->setEnabled(ready && tracking && m_directory.entry.directory && !m_editing);
    m_edit->setVisible(!m_directory.entry.directory && !m_editing);
    m_edit->setEnabled(ready && tracking && m_content.editable);
    m_save->setVisible(m_editing); m_cancel->setVisible(m_editing);
    m_save->setEnabled(ready && tracking && contextMatches && hasUnsavedChanges());
    m_cancel->setEnabled(!m_saving && !m_mutating);
    m_manage->setVisible(!m_directory.entry.directory && !m_editing && !m_path.isEmpty());
    m_manage->setEnabled(ready && tracking && m_directory.location.rootDirectory);
    ui->editor->setReadOnly(!m_editing || m_saving || m_mutating);
}
void HomePagePreviewPage::beginEdit()
{
    if (!m_edit->isEnabled()) return;
    m_editing = true; m_statusText.clear();
    ui->editor->document()->setModified(false);
    showContent(); updateActions(); ui->editor->setFocus();
}
void HomePagePreviewPage::discard()
{
    m_editing = m_newFile = false; m_statusText.clear();
    ui->editor->document()->setModified(false); ui->editor->setReadOnly(true); updateActions();
}
void HomePagePreviewPage::requestLeave(std::function<void()> continuation)
{
    if (m_prompt || m_saving || m_mutating) return;
    if (!hasUnsavedChanges()) { m_editing = false; updateActions(); continuation(); return; }
    auto *dialog = new QMessageBox(QMessageBox::Question, "保存修改", "文件有尚未保存的修改。", QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this);
    dialog->setObjectName("previewUnsavedDialog"); dialog->setTextFormat(Qt::PlainText);
    dialog->setInformativeText(m_newFile ? m_content.location.path : m_directory.entry.name);
    dialog->button(QMessageBox::Save)->setText("保存"); dialog->button(QMessageBox::Discard)->setText("放弃"); dialog->button(QMessageBox::Cancel)->setText("取消");
    if (!m_service->contains(m_id))
    {
        dialog->button(QMessageBox::Save)->setEnabled(false);
        auto *copy = dialog->addButton("复制编辑内容", QMessageBox::ActionRole);
        connect(copy, &QAbstractButton::clicked, this, [this] { QApplication::clipboard()->setText(ui->editor->toPlainText()); });
    }
    dialog->setDefaultButton(QMessageBox::Save); dialog->setEscapeButton(QMessageBox::Cancel);
    dialog->setAttribute(Qt::WA_DeleteOnClose); m_prompt = true;
    connect(dialog, &QDialog::finished, this, [this, continuation = std::move(continuation)](int choice) {
        m_prompt = false;
        if (choice == QMessageBox::Save) save(continuation);
        else if (choice == QMessageBox::Discard) { discard(); continuation(); }
    });
    dialog->open();
}
void HomePagePreviewPage::save(std::function<void()> continuation)
{
    if (m_saving || m_mutating || !m_editing) return;
    SourceSaveRequest request{m_content.location, m_content.stamp, ui->editor->toPlainText(), m_newFile};
    m_saving = true; updateActions();
    status("正在保存…");
    m_service->saveSourceFile(request, this, [this, request, continuation = std::move(continuation)](const OperationResult &result) {
        m_saving = false;
        if (!result.success) { status(result.title + "：" + result.message); emit notification(result); updateActions(); return; }
        discard(); m_statusText = "已保存到本地";
        if (continuation) continuation();
        else if (request.create) openPath(request.location.path);
        else refresh();
    });
}
void HomePagePreviewPage::createEntry(bool directory)
{
    if (!m_new->isEnabled()) return;
    const auto location = m_directory.location;
    bool ok = false;
    const auto name = QInputDialog::getText(this, directory ? "新建文件夹" : "新建文件", "名称", QLineEdit::Normal, directory ? QString() : "untitled.txt", &ok);
    if (!ok) return;
    if (!validSourceEntryName(name)) { emit notification(OperationResult::fail("名称无效", "请输入单个名称，不要包含路径或保留名称。")); return; }
    if (directory) { mutateEntry(location, SourceMutationKind::CreateDirectory, name); return; }
    auto target = location; target.path = target.path.isEmpty() ? name : target.path + '/' + name;
    const auto checked = sourcePreviewPath(target, false);
    if (!checked.result.success || QFileInfo::exists(checked.value)) { emit notification(OperationResult::fail("无法新建文件", "路径无效或同名项目已存在。")); return; }
    m_content = {}; m_content.location = target; m_content.textual = m_content.editable = true;
    m_editing = m_newFile = true;
    m_directory.entry.directory = false;
    ui->editor->clear(); ui->editor->document()->setModified(false);
    ui->commitButton->hide(); ui->markdownButton->hide(); ui->imageTools->hide();
    ui->fileInfo->setText("新文件 · " + name);
    ui->contentStack->setCurrentWidget(ui->editor);
    status("首次保存后创建文件"); updateActions(); ui->editor->setFocus();
}
void HomePagePreviewPage::entryMenu(const SourceLocation &location, const QPoint &position)
{
    if (!location.rootDirectory || location.path.isEmpty() || m_saving || m_mutating || m_loading) return;
    auto *menu = new QMenu(this);
    menu->setObjectName("sourceEntryMenu");
    menu->setAttribute(Qt::WA_DeleteOnClose);
    auto *rename = menu->addAction("重命名…"); auto *remove = menu->addAction("永久删除…");
    const bool enabled = m_service->syncState(location.context.id) == BackupSyncState::Tracking;
    rename->setEnabled(enabled); remove->setEnabled(enabled);
    connect(rename, &QAction::triggered, this, [this, location] {
        requestLeave([this, location] {
            bool ok = false;
            const auto name = QInputDialog::getText(this, "重命名", "新名称", QLineEdit::Normal, QFileInfo(location.path).fileName(), &ok);
            if (ok) mutateEntry(location, SourceMutationKind::Rename, name);
        });
    });
    connect(remove, &QAction::triggered, this, [this, location] {
        requestLeave([this, location] { mutateEntry(location, SourceMutationKind::Delete); });
    });
    menu->popup(position);
}
void HomePagePreviewPage::mutateEntry(const SourceLocation &location, SourceMutationKind kind, const QString &name)
{
    if (m_mutating || m_saving) return;
    m_mutating = true; updateActions();
    m_service->prepareSourceMutation(location, kind, name, this, [this](const BackupResult<SourceMutationRequest> &reply) {
        if (!reply.result.success) { m_mutating = false; updateActions(); emit notification(reply.result); return; }
        const auto request = reply.value;
        const auto apply = [this, request] {
            m_service->applySourceMutation(request, this, [this, request](const OperationResult &result) {
                m_mutating = false; emit notification(result);
                if (result.success && m_path == request.location.path && request.kind != SourceMutationKind::CreateDirectory)
                {
                    auto parent = request.location.path.section('/', 0, -2);
                    if (!request.location.path.contains('/')) parent.clear();
                    const auto path = request.kind == SourceMutationKind::Rename ? (parent.isEmpty() ? request.name : parent + '/' + request.name) : parent;
                    openPath(path);
                }
                else refresh();
                updateActions();
            });
        };
        if (request.kind != SourceMutationKind::Delete) { apply(); return; }
        auto *dialog = new QMessageBox(QMessageBox::Warning, "永久删除", "确定永久删除此项目及其内部内容？", QMessageBox::Yes | QMessageBox::Cancel, this);
        dialog->setObjectName("previewDeleteDialog"); dialog->setTextFormat(Qt::PlainText);
        dialog->setInformativeText(QDir::toNativeSeparators(QDir(request.location.root).filePath(request.location.path)) + "\n\n不会移入废纸篓。未备份的修改无法从历史版本找回。");
        dialog->button(QMessageBox::Yes)->setText("永久删除"); dialog->button(QMessageBox::Cancel)->setText("取消");
        dialog->setDefaultButton(QMessageBox::Cancel); dialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(dialog, &QDialog::finished, this, [this, apply](int choice) {
            if (choice == QMessageBox::Yes) apply(); else { m_mutating = false; updateActions(); }
        });
        dialog->open();
    });
}
