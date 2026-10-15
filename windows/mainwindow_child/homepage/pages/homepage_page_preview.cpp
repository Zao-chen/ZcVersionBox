#include "homepage_page_preview.h"
#include "ui_homepage_page_preview.h"
#include "windows/mainwindow_presentation.h"
#include "windows/mainwindow_dialog.h"
#include <QAction>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShortcut>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QPointer>
#include <QClipboard>
#include <QApplication>
#include <QLabel>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QFocusEvent>
#include <QTimer>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QPersistentModelIndex>
#include <QItemSelectionModel>
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

class BreadcrumbLabel : public QLabel
{
  public:
    explicit BreadcrumbLabel(const QString &text, const QString &path, bool isCurrent, std::function<void(const QString &)> onClick, QWidget *parent = nullptr)
        : QLabel(text, parent), m_path(path), m_isCurrent(isCurrent), m_onClick(std::move(onClick))
    {
        if (!m_isCurrent)
        {
            setCursor(Qt::PointingHandCursor);
            setFocusPolicy(Qt::TabFocus);
        }
        UiStyle::text(this, UiStyle::FontRole::Body, !isCurrent);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }
  protected:
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        QLabel::mouseReleaseEvent(event);
        if (!m_isCurrent && event->button() == Qt::LeftButton && rect().contains(event->pos()) && m_onClick)
        {
            const auto onClick = m_onClick;
            const auto path = m_path;
            QTimer::singleShot(0, [onClick, path]() {
                onClick(path);
            });
        }
    }
    void keyPressEvent(QKeyEvent *event) override
    {
        if (!m_isCurrent && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter || event->key() == Qt::Key_Space) && m_onClick)
        {
            const auto onClick = m_onClick;
            const auto path = m_path;
            QTimer::singleShot(0, [onClick, path]() {
                onClick(path);
            });
            event->accept();
            return;
        }
        QLabel::keyPressEvent(event);
    }
    void enterEvent(QEnterEvent *event) override
    {
        QLabel::enterEvent(event);
        if (!m_isCurrent)
        {
            auto f = font();
            f.setUnderline(true);
            setFont(f);
        }
    }
    void leaveEvent(QEvent *event) override
    {
        QLabel::leaveEvent(event);
        if (!m_isCurrent)
        {
            auto f = font();
            f.setUnderline(false);
            setFont(f);
        }
    }
    void focusInEvent(QFocusEvent *event) override
    {
        QLabel::focusInEvent(event);
        if (!m_isCurrent)
        {
            auto f = font();
            f.setUnderline(true);
            setFont(f);
        }
    }
    void focusOutEvent(QFocusEvent *event) override
    {
        QLabel::focusOutEvent(event);
        if (!m_isCurrent)
        {
            auto f = font();
            f.setUnderline(false);
            setFont(f);
        }
    }
  private:
    QString m_path;
    bool m_isCurrent{false};
    std::function<void(const QString &)> m_onClick;
};

class PreviewFileDelegate : public QStyledItemDelegate
{
  public:
    explicit PreviewFileDelegate(QTreeView *view, QObject *parent = nullptr)
        : QStyledItemDelegate(parent ? parent : view), m_view(view)
    {
        m_view->viewport()->installEventFilter(this);
        m_view->installEventFilter(this);
        if (m_view->selectionModel())
        {
            connect(m_view->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex &current, const QModelIndex &previous) {
                updateRow(previous);
                updateRow(current);
            });
            connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this](const QItemSelection &selected, const QItemSelection &deselected) {
                for (const auto &index : selected.indexes()) updateRow(index);
                for (const auto &index : deselected.indexes()) updateRow(index);
            });
        }
        connect(m_view->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
            if (m_pointerInside) updateHover(m_pointerPosition);
        });
        if (m_view->model())
        {
            connect(m_view->model(), &QAbstractItemModel::modelAboutToBeReset, this, [this] {
                m_hovered = QPersistentModelIndex{};
            });
            connect(m_view->model(), &QAbstractItemModel::modelReset, this, [this] {
                QTimer::singleShot(0, this, [this] { if (m_pointerInside) updateHover(m_pointerPosition); });
            });
            connect(m_view->model(), &QAbstractItemModel::rowsAboutToBeRemoved, this, [this] {
                m_hovered = QPersistentModelIndex{};
            });
        }
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        size.rwidth() += 16;
        size.setHeight(UiStyle::rowHeight(40, UiStyle::font(UiStyle::FontRole::Body)));
        return size;
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        auto opt = option;
        initStyleOption(&opt, index);

        const auto colors = UiStyle::colors();
        painter->save();
        painter->setClipRect(opt.rect);
        painter->setRenderHint(QPainter::Antialiasing);

        painter->fillRect(opt.rect, opt.palette.base());

        const auto row = QRect(0, opt.rect.y() + 2, m_view->viewport()->width(), opt.rect.height() - 4);
        const bool selected = m_view->selectionModel() && m_view->selectionModel()->isRowSelected(index.row(), index.parent());
        const bool hover = m_hovered.isValid() && m_hovered.row() == index.row() && m_hovered.parent() == index.parent();

        if (selected)
        {
            painter->setPen(Qt::NoPen);
            painter->setBrush(colors.selected);
            painter->drawRoundedRect(row, 6, 6);
        }
        else if (hover)
        {
            painter->setPen(Qt::NoPen);
            painter->setBrush(colors.hover);
            painter->drawRoundedRect(row, 6, 6);
        }

        if (UiStyle::isKeyboardNavigationActive() && m_view->hasFocus() && m_view->currentIndex().row() == index.row() && m_view->currentIndex().parent() == index.parent())
        {
            painter->setBrush(Qt::NoBrush);
            painter->setPen(colors.secondary);
            painter->drawRoundedRect(QRectF(row).adjusted(.5, .5, -.5, -.5), 6, 6);
        }

        opt.state.setFlag(QStyle::State_Selected, false);
        opt.state.setFlag(QStyle::State_MouseOver, false);
        opt.state.setFlag(QStyle::State_HasFocus, false);

        if (index.column() == 0)
        {
            opt.font = UiStyle::font(UiStyle::FontRole::Body);
            opt.palette.setColor(QPalette::Text, colors.text);
            opt.rect = opt.rect.adjusted(8, 0, -8, 0);
        }
        else
        {
            opt.font = UiStyle::font(UiStyle::FontRole::Caption);
            opt.palette.setColor(QPalette::Text, colors.secondary);
            opt.rect = opt.rect.adjusted(8, 0, -8, 0);
        }

        QStyledItemDelegate::paint(painter, opt, index);

        painter->restore();
    }

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_view->viewport())
        {
            if (event->type() == QEvent::MouseMove)
            {
                updateHover(static_cast<QMouseEvent *>(event)->position().toPoint());
            }
            else if (event->type() == QEvent::Leave)
            {
                const auto previous = m_hovered;
                m_hovered = QPersistentModelIndex{};
                m_pointerInside = false;
                updateRow(previous);
            }
        }
        else if (watched == m_view)
        {
            if (event->type() == QEvent::Hide)
            {
                m_hovered = QPersistentModelIndex{};
                m_pointerInside = false;
            }
        }
        return QStyledItemDelegate::eventFilter(watched, event);
    }

  private:
    QTreeView *m_view{nullptr};
    QPersistentModelIndex m_hovered;
    QPoint m_pointerPosition;
    bool m_pointerInside{false};

    void updateRow(const QModelIndex &index) const
    {
        if (index.isValid() && m_view && m_view->viewport())
        {
            const auto cell = m_view->visualRect(index);
            m_view->viewport()->update(QRect(0, cell.top(), m_view->viewport()->width(), cell.height()));
        }
    }

    void updateHover(const QPoint &position)
    {
        if (!m_view || !m_view->viewport()) return;
        const auto previous = m_hovered;
        m_pointerPosition = position;
        m_pointerInside = m_view->viewport()->rect().contains(position);
        const auto index = m_pointerInside ? m_view->indexAt(position) : QModelIndex{};
        m_hovered = index.isValid() ? QPersistentModelIndex(index.siblingAtColumn(0)) : QPersistentModelIndex{};
        if (previous != m_hovered)
        {
            updateRow(previous);
            updateRow(m_hovered);
        }
    }
};
}
HomePagePreviewPage::HomePagePreviewPage(BackupService *service, QWidget *parent)
    : QWidget(parent), ui(new Ui::HomePagePreviewPage), m_service(service), m_watcher(this)
{
    ui->setupUi(this);
    ui->breadcrumbLayout->setAlignment(Qt::AlignVCenter);
    ui->breadcrumbLayout->setContentsMargins(12, 0, 0, 0);
    ui->pageLayout->setSpacing(0);
    ui->pageLayout->setStretch(ui->pageLayout->indexOf(ui->contentStack), 1);
    ui->statusLabel->setContentsMargins(0, 4, 0, 4);
    ui->statusLabel->hide();
    ui->markdownButton->setMinimumSize(28, 28);
    m_model.setHorizontalHeaderLabels({"名称", "最近提交说明", "提交时间"});
    ui->files->setModel(&m_model);
    UiStyle::flatView(ui->files);
    ui->files->setHeaderHidden(true);
    ui->files->setAccessibleName("本地文件列表");
    ui->files->setSelectionBehavior(QAbstractItemView::SelectRows);
    auto *fileDelegate = new PreviewFileDelegate(ui->files, this);
    ui->files->setItemDelegate(fileDelegate);
    ui->files->header()->setStretchLastSection(false);
    ui->files->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    ui->files->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    ui->files->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    ui->files->setColumnWidth(0, 240);
    ui->editor->setReadOnly(true);
    ui->editor->setFrameStyle(0);
    ui->editor->setFrameShape(QFrame::NoFrame);
    ui->editor->setFrameShadow(QFrame::Plain);
    ui->editor->setLineWidth(0);
    ui->editor->setMidLineWidth(0);
    ui->editor->setAccessibleName("文件内容");
    ui->editor->setHeaderText("文件内容");
    ui->markdown->setFrameStyle(0);
    ui->markdown->setFrameShape(QFrame::NoFrame);
    ui->markdown->setFrameShadow(QFrame::Plain);
    ui->markdown->setLineWidth(0);
    ui->markdown->setMidLineWidth(0);
    ui->markdown->setAccessibleName("Markdown 预览");
    ui->imageScroll->setFrameStyle(0);
    ui->imageScroll->setFrameShape(QFrame::NoFrame);
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
    for (auto *action : {m_new, m_open, m_edit, m_save, m_cancel, m_manage})
    {
        auto *button = UiStyle::toolButton(ui->actionHost, action);
        button->setObjectName(action->objectName() + "Button");
        button->setMinimumSize(28, 28);
        if (action == m_new) button->setPopupMode(QToolButton::InstantPopup);
        ui->actionLayout->addWidget(button);
        connect(action, &QAction::changed, button, [button, action] {
            button->setVisible(action->isVisible());
            button->setEnabled(action->isEnabled());
        });
    }
    connect(m_edit, &QAction::triggered, this, &HomePagePreviewPage::beginEdit);
    connect(m_save, &QAction::triggered, this, [this] { save(); });
    connect(m_manage, &QAction::triggered, this, [this] {
        auto *btn = ui->actionHost->findChild<QToolButton *>("manageSourceEntryActionButton");
        const QPoint pos = (btn && btn->isVisible()) ? btn->mapToGlobal(QPoint(0, btn->height())) : QCursor::pos();
        const auto location = m_directory.entry.directory ? m_directory.location : (m_content.location.path.isEmpty() ? m_directory.location : m_content.location);
        entryMenu(location, pos);
    });
    connect(ui->files, &QTreeView::customContextMenuRequested, this, [this](const QPoint &point) {
        const auto index = ui->files->indexAt(point);
        if (!index.isValid()) return;
        ui->files->setCurrentIndex(index);
        auto location = m_directory.location;
        location.path = index.siblingAtColumn(0).data(PathRole).toString();
        entryMenu(location, ui->files->viewport()->mapToGlobal(point));
    });
    auto *saveShortcut = new QShortcut(QKeySequence::Save, this);
    saveShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(saveShortcut, &QShortcut::activated, this, [this] { if (m_editing) save(); });
    connect(ui->editor->document(), &QTextDocument::modificationChanged, this, &HomePagePreviewPage::updateActions);
    connect(service, &BackupService::busyChanged, this, &HomePagePreviewPage::updateActions);
    connect(service, &BackupService::syncStatusChanged, this, [this](const QString &id) { if (id == m_id) updateActions(); });
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
    ui->actionLayout->insertWidget(0, m_spinner);
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
HomePagePreviewPage::~HomePagePreviewPage() { m_service->setSyncEditing(m_id, false); deactivate(); }
QList<QAction *> HomePagePreviewPage::toolbarActions() const { return {m_refresh}; }
void HomePagePreviewPage::refreshTheme()
{
    ui->editor->refreshTheme();
    UiStyle::text(ui->files, UiStyle::FontRole::Body);
    UiStyle::text(ui->statusLabel, UiStyle::FontRole::Caption, true);
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
    m_service->setSyncEditing(m_id, false);
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
    ui->statusLabel->clear(); ui->statusLabel->hide();
    ui->imageTools->hide(); ui->markdownButton->hide();
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
            if (m_directory.children.isEmpty()) { ui->notice->setText("此文件夹为空"); ui->contentStack->setCurrentWidget(ui->notice); }
            else ui->contentStack->setCurrentWidget(ui->directoryPage);
            setLoading(false); restoreScroll();
        }
        else
        {
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
    ui->imageTools->setVisible(!m_content.image.isNull());
    if (m_content.textual)
    {
        status(m_statusText.isEmpty() ? m_content.notice : m_statusText);
        ui->contentStack->setCurrentWidget(markdown && !m_editing && !ui->markdownButton->isChecked() ? static_cast<QWidget *>(ui->markdown) : ui->editor);
    }
    else if (!m_content.image.isNull())
    {
        status(m_statusText);
        ui->contentStack->setCurrentWidget(ui->imageScroll);
        showImage();
    }
    else
    {
        status(m_statusText);
        ui->notice->setText(m_content.notice);
        ui->contentStack->setCurrentWidget(ui->notice);
    }
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
    while (auto *item = ui->breadcrumbLayout->takeAt(0))
    {
        if (auto *widget = item->widget())
            widget->deleteLater();
        delete item;
    }
    const auto parts = m_path.split('/', Qt::SkipEmptyParts);
    const auto rootName = QFileInfo(m_service->sourcePath(m_id)).fileName();
    const auto add = [this](const QString &name, const QString &path, bool isCurrent) {
        auto *label = new BreadcrumbLabel(name, path, isCurrent, [this](const QString &target) { openPath(target); }, ui->breadcrumbHost);
        label->setToolTip(name);
        label->setAccessibleName(name);
        ui->breadcrumbLayout->addWidget(label);
    };
    add(rootName, {}, parts.isEmpty());
    QString path;
    for (int i = 0; i < parts.size(); ++i)
    {
        const auto &part = parts[i];
        auto *separator = new QLabel("/", ui->breadcrumbHost);
        UiStyle::text(separator, UiStyle::FontRole::Caption, true);
        ui->breadcrumbLayout->addWidget(separator);
        path += (path.isEmpty() ? "" : "/") + part;
        add(part, path, i == parts.size() - 1);
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
    m_service->setSyncEditing(m_id, hasUnsavedChanges() || m_saving || m_mutating);
    const bool tracking = m_service->contains(m_id) && m_service->syncState(m_id) == BackupSyncState::Tracking;
    const auto context = m_service->branchContext(m_id);
    const auto original = m_content.location.context;
    const bool contextMatches = !m_editing || (context.ref == original.ref && context.version == original.version && context.generation == original.generation);
    const bool ready = m_active && m_valid && !m_loading && !m_saving && !m_mutating && !m_service->isSynchronizing(m_id);
    const bool isDir = m_directory.entry.directory;
    m_new->setVisible(isDir && !m_editing);
    m_new->setEnabled(ready && tracking && isDir && !m_editing);
    m_open->setVisible(!m_editing);
    m_open->setEnabled(ready && !m_newFile);
    m_refresh->setEnabled(m_active && !m_loading && !m_saving && !m_mutating);
    m_edit->setVisible(!isDir && !m_editing);
    m_edit->setEnabled(ready && tracking && m_content.editable);
    m_save->setVisible(m_editing); m_cancel->setVisible(m_editing);
    m_save->setEnabled(ready && tracking && contextMatches && hasUnsavedChanges());
    m_cancel->setEnabled(!m_saving && !m_mutating);
    m_manage->setVisible(!m_editing && !m_path.isEmpty());
    m_manage->setEnabled(ready && tracking && m_directory.location.rootDirectory);
    ui->editor->setReadOnly(!m_editing || m_saving || m_mutating || m_service->isSynchronizing(m_id));
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
    auto *dialog = new UiDialog::Dialog(this, QStringLiteral("previewUnsavedDialog"), QStringLiteral("保存修改"));
    dialog->setMinimumWidth(460);
    auto *body = new QLabel("文件有尚未保存的修改。\n\n" + (m_newFile ? m_content.location.path : m_directory.entry.name), dialog);
    body->setTextFormat(Qt::PlainText);
    body->setWordWrap(true);
    UiStyle::text(body);
    dialog->contentLayout()->addWidget(body);

    auto *buttons = dialog->buttonBox(QDialogButtonBox::Save | QDialogButtonBox::Discard | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Save)->setText("保存");
    buttons->button(QDialogButtonBox::Discard)->setText("放弃");
    buttons->button(QDialogButtonBox::Cancel)->setText("取消");
    buttons->button(QDialogButtonBox::Save)->setDefault(true);

    auto *footer = dialog->footerLayout();
    if (!m_service->contains(m_id))
    {
        buttons->button(QDialogButtonBox::Save)->setEnabled(false);
        auto *copy = new QPushButton("复制编辑内容", dialog);
        copy->setAutoDefault(false);
        connect(copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(ui->editor->toPlainText()); });
        footer->addWidget(copy);
    }
    footer->addStretch();
    footer->addWidget(buttons);
    dialog->prepareAsync();
    m_prompt = true;
    connect(buttons, &QDialogButtonBox::clicked, dialog, [dialog, buttons](QAbstractButton *button) {
        if (button == buttons->button(QDialogButtonBox::Save)) dialog->done(QMessageBox::Save);
        else if (button == buttons->button(QDialogButtonBox::Discard)) dialog->done(QMessageBox::Discard);
        else dialog->done(QMessageBox::Cancel);
    });
    connect(dialog, &QDialog::finished, this, [this, continuation = std::move(continuation)](int choice) {
        m_prompt = false;
        if (choice == QMessageBox::Save || choice == static_cast<int>(QDialogButtonBox::Save)) save(continuation);
        else if (choice == QMessageBox::Discard || choice == static_cast<int>(QDialogButtonBox::Discard)) { discard(); continuation(); }
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
    const auto name = UiDialog::getText(this, directory ? "新建文件夹" : "新建文件", "名称", directory ? QString() : "untitled.txt", &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    if (!validSourceEntryName(name)) { emit notification(OperationResult::fail("名称无效", "请输入单个名称，不要包含路径或保留名称。")); return; }
    if (directory) { mutateEntry(location, SourceMutationKind::MakeDirectory, name); return; }
    auto target = location; target.path = target.path.isEmpty() ? name : target.path + '/' + name;
    const auto checked = sourcePreviewPath(target, false);
    if (!checked.result.success || QFileInfo::exists(checked.value)) { emit notification(OperationResult::fail("无法新建文件", "路径无效或同名项目已存在。")); return; }
    m_content = {}; m_content.location = target; m_content.textual = m_content.editable = true;
    m_editing = m_newFile = true;
    m_directory.entry.directory = false;
    ui->editor->clear(); ui->editor->document()->setModified(false);
    ui->markdownButton->hide(); ui->imageTools->hide();
    ui->contentStack->setCurrentWidget(ui->editor);
    status("新文件 · " + name + "；首次保存后创建文件"); updateActions(); ui->editor->setFocus();
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
            const auto name = UiDialog::getText(this, "重命名", "新名称", QFileInfo(location.path).fileName(), &ok).trimmed();
            if (ok && !name.isEmpty()) mutateEntry(location, SourceMutationKind::Rename, name);
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
                if (result.success && m_path == request.location.path && request.kind != SourceMutationKind::MakeDirectory)
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
        auto *dialog = new UiDialog::Dialog(this, QStringLiteral("previewDeleteDialog"), QStringLiteral("永久删除"));
        dialog->setMinimumWidth(480);
        auto *body = new QLabel(QString("确定永久删除此项目及其内部内容？\n\n%1\n\n不会移入废纸篓。未备份的修改无法从历史版本找回。")
            .arg(QDir::toNativeSeparators(QDir(request.location.root).filePath(request.location.path))), dialog);
        body->setTextFormat(Qt::PlainText);
        body->setWordWrap(true);
        UiStyle::text(body);
        dialog->contentLayout()->addWidget(body);

        auto *buttons = dialog->buttonBox(QDialogButtonBox::Cancel);
        auto *confirmButton = buttons->addButton("永久删除", QDialogButtonBox::AcceptRole);
        confirmButton->setAutoDefault(false);
        buttons->button(QDialogButtonBox::Cancel)->setText("取消");
        buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
        buttons->button(QDialogButtonBox::Cancel)->setFocus();

        auto *footer = dialog->footerLayout();
        footer->addStretch();
        footer->addWidget(buttons);
        dialog->bindButtonBox(buttons);
        dialog->prepareAsync();
        connect(dialog, &QDialog::finished, this, [this, apply](int choice) {
            if (choice == QDialog::Accepted || choice == QMessageBox::Yes) apply();
            else { m_mutating = false; updateActions(); }
        });
        dialog->open();
    });
}
