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
    connect(m_refresh, &QAction::triggered, this, &HomePagePreviewPage::refresh);
    connect(m_open, &QAction::triggered, this, [this] {
        const auto path = sourcePreviewPath(m_directory.location);
        if (m_valid && path.result.success) openLocalPath(this, path.value);
    });
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
QList<QAction *> HomePagePreviewPage::toolbarActions() const { return {m_open, m_refresh}; }
void HomePagePreviewPage::refreshTheme()
{
    UiStyle::text(ui->editor, UiStyle::FontRole::Code);
    UiStyle::text(ui->files, UiStyle::FontRole::Body);
    UiStyle::text(ui->statusLabel, UiStyle::FontRole::Caption, true);
    UiStyle::text(ui->fileInfo, UiStyle::FontRole::Caption, true);
    UiStyle::text(ui->commitButton, UiStyle::FontRole::Caption, true);
    for (auto *action : {m_open, m_refresh}) action->setIcon(UiStyle::icon(action->property("iconName").toString()));
    for (int row = 0; row < m_model.rowCount(); ++row)
        m_model.item(row)->setIcon(UiStyle::icon(m_directory.children.value(row).directory ? "folder" : "file"));
}
void HomePagePreviewPage::rememberScroll()
{
    if (!m_valid) return;
    const auto key = m_id + QChar(0x1f) + m_path;
    if (m_directory.entry.directory) m_scrolls[key] = {ui->files->horizontalScrollBar()->value(), ui->files->verticalScrollBar()->value()};
    else if (m_content.textual) m_scrolls[key] = {ui->editor->horizontalScrollBar()->value(), ui->editor->verticalScrollBar()->value()};
}
void HomePagePreviewPage::restoreScroll()
{
    const auto point = m_scrolls.value(m_id + QChar(0x1f) + m_path);
    auto *view = m_directory.entry.directory ? static_cast<QAbstractScrollArea *>(ui->files) : static_cast<QAbstractScrollArea *>(ui->editor);
    view->horizontalScrollBar()->setValue(point.x()); view->verticalScrollBar()->setValue(point.y());
}
void HomePagePreviewPage::setBackup(const QString &id, const QString &path)
{
    rememberScroll();
    m_id = id; m_path = path; m_paths[id] = path; m_active = true;
    m_valid = false; m_fit = true; m_zoom = 1;
    ui->findBar->hide(); ui->markdownButton->setChecked(false);
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
    m_open->setEnabled(m_valid); m_refresh->setEnabled(m_active && !loading);
}
void HomePagePreviewPage::refresh()
{
    if (!m_active) return;
    rememberScroll();
    for (const auto task : m_tasks) m_service->cancel(task);
    m_tasks.clear();
    const auto request = ++m_request;
    ++m_changesRequest;
    setLoading(true); m_valid = false; m_content = {}; m_changes.clear();
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
                m_model.appendRow({name, new QStandardItem("正在读取…"), new QStandardItem});
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
                setLoading(false); showContent(); restoreScroll();
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
        for (auto it = reply.value.revisions.cbegin(); it != reply.value.revisions.cend(); ++it) m_changes.insert(it.key(), it.value());
        updateChanges(); loadChanges(offset + 64);
    }));
}
void HomePagePreviewPage::updateChanges()
{
    const auto revision = m_changes.value(m_path);
    ui->commitButton->setText(revision.hash.isEmpty() ? "暂无备份记录" : revision.message + "  ·  " + relativeTime(revision.committedAt));
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
    ui->markdownButton->setVisible(markdown);
    ui->statusLabel->setText(m_content.notice); ui->statusLabel->setVisible(!m_content.notice.isEmpty());
    ui->imageTools->setVisible(!m_content.image.isNull());
    if (m_content.textual) ui->contentStack->setCurrentWidget(markdown && !ui->markdownButton->isChecked() ? static_cast<QWidget *>(ui->markdown) : ui->editor);
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
