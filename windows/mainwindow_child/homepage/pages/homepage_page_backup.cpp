#include "homepage_page_backup.h"
#include "ui_homepage_page_backup.h"
#include "windows/mainwindow_presentation.h"
#include <QAction>
#include <QContextMenuEvent>
#include <QFocusEvent>
#include <QHeaderView>
#include <QHelpEvent>
#include <QHideEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPersistentModelIndex>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QShortcut>
#include <QShowEvent>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolTip>
#include <QtMath>
#include <array>
#include <utility>

namespace
{
constexpr int CommitRole = Qt::UserRole + 1;
constexpr int ActionsColumn = 3;
constexpr int ActionSize = 28;
constexpr int ActionSpacing = 4;
constexpr int ActionsWidth = 3 * ActionSize + 2 * ActionSpacing + 16;
enum class RevisionAction
{
    Preview,
    Compare,
    More
};
class HistoryDelegate : public QStyledItemDelegate
{
  public:
    using ActionHandler = std::function<void(const QModelIndex &, RevisionAction, const QPoint &)>;
    HistoryDelegate(QTableView *view, std::function<void()> editing, ActionHandler action)
        : QStyledItemDelegate(view), m_view(view), m_editing(std::move(editing)), m_action(std::move(action))
    {
        updateIcons();
        view->viewport()->installEventFilter(this);
        view->installEventFilter(this);
        connect(view->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex &current, const QModelIndex &previous)
                {
            updateRow(previous);
            updateRow(current); });
        connect(view->verticalScrollBar(), &QScrollBar::valueChanged, this, [this]
                { if (m_pointerInside) updateHover(m_pointerPosition); });
        connect(view->model(), &QAbstractItemModel::modelAboutToBeReset, this, [this]
                {
                    m_hovered = QModelIndex{};
                    m_pressed = QModelIndex{};
                    m_editorOpen = false;
                    // Keep consuming a pending release even when the pressed row disappears.
                });
        connect(view->model(), &QAbstractItemModel::modelReset, this, [this]
                {
            // A reset precedes repopulation; resolve the row under a stationary pointer afterwards.
            QTimer::singleShot(0, this, [this] { if (m_pointerInside) updateHover(m_pointerPosition); }); });
        connect(this, &QAbstractItemDelegate::closeEditor, this, [this]
                {
            m_editorOpen = false;
            m_view->viewport()->update(); });
    }
    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        auto *editor = QStyledItemDelegate::createEditor(parent, option, index);
        if (editor)
        {
            m_editorOpen = true;
            m_editing();
            m_view->viewport()->update();
        }
        return editor;
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        auto opt = option;
        initStyleOption(&opt, index);
        const auto colors = UiStyle::colors();
        const auto font = UiStyle::font(index.column() == 0   ? UiStyle::FontRole::Body
                                        : index.column() == 2 ? UiStyle::FontRole::Code
                                                              : UiStyle::FontRole::Caption);
        painter->save();
        painter->setClipRect(opt.rect);
        painter->setRenderHint(QPainter::Antialiasing);
        // Clear the native per-cell background before drawing the continuous row state.
        painter->fillRect(opt.rect, opt.palette.base());
        const auto row = QRect(0, opt.rect.y() + 2, m_view->viewport()->width(), opt.rect.height() - 4);
        const bool hover = m_hovered.isValid() && m_hovered.row() == index.row();
        if (opt.state.testFlag(QStyle::State_Selected) || hover)
        {
            painter->setPen(Qt::NoPen);
            painter->setBrush(opt.state.testFlag(QStyle::State_Selected) ? colors.selected : colors.hover);
            painter->drawRoundedRect(row, 6, 6);
        }
        if (UiStyle::isKeyboardNavigationActive() && m_view->hasFocus() && m_view->currentIndex().row() == index.row())
        {
            painter->setBrush(Qt::NoBrush);
            painter->setPen(colors.secondary);
            painter->drawRoundedRect(QRectF(row).adjusted(.5, .5, -.5, -.5), 6, 6);
        }
        if (index.column() == ActionsColumn)
        {
            if (actionsVisible(index))
            {
                for (int action = 0; action < static_cast<int>(m_icons.size()); ++action)
                {
                    const auto rect = actionRect(opt.rect, action);
                    if (hover && rect.contains(m_pointerPosition))
                    {
                        painter->setPen(Qt::NoPen);
                        const bool pressed = m_actionPress && m_pressed.row() == index.row() && m_pressedAction == action;
                        painter->setBrush(pressed ? colors.separator : opt.state.testFlag(QStyle::State_Selected) ? colors.hover
                                                                                                                  : colors.selected);
                        painter->drawRoundedRect(rect, 6, 6);
                    }
                    m_icons[action].paint(painter, QRect(rect.center() - QPoint(8, 8), QSize(16, 16)));
                }
            }
        }
        else
        {
            const auto textRect = opt.rect.adjusted(12, 0, -12, 0);
            painter->setFont(font);
            painter->setPen(index.column() == 0 ? colors.text : colors.secondary);
            painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                              QFontMetrics(font, painter->device()).elidedText(opt.text, Qt::ElideRight, qMax(0, textRect.width())));
        }
        painter->restore();
    }

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_view->viewport())
        {
            if (event->type() == QEvent::MouseMove)
                updateHover(static_cast<QMouseEvent *>(event)->position().toPoint());
            else if (event->type() == QEvent::Leave)
            {
                const auto previous = m_hovered;
                m_hovered = QModelIndex{};
                m_pressed = QModelIndex{};
                m_pointerInside = false;
                m_view->viewport()->unsetCursor();
                updateRow(previous);
            }
            else if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick)
            {
                const auto *mouse = static_cast<QMouseEvent *>(event);
                if (mouse->button() == Qt::LeftButton)
                {
                    m_pressed = QModelIndex{};
                    m_actionPress = false;
                    const auto position = mouse->position().toPoint();
                    updateHover(position);
                    const auto index = m_view->indexAt(position);
                    const int action = hitAction(index, position);
                    if (action >= 0)
                    {
                        m_actionPress = true;
                        m_pressed = event->type() == QEvent::MouseButtonPress ? index.siblingAtColumn(0) : QModelIndex{};
                        m_pressedAction = action;
                        m_view->setCurrentIndex(index.siblingAtColumn(0));
                        m_view->selectionModel()->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                        m_view->setFocus(Qt::MouseFocusReason);
                        updateRow(index);
                        return true; // A painted action must not also activate the table row.
                    }
                }
            }
            else if (event->type() == QEvent::MouseButtonRelease && m_actionPress)
            {
                const auto *mouse = static_cast<QMouseEvent *>(event);
                if (mouse->button() == Qt::LeftButton)
                {
                    const auto pressed = m_pressed;
                    const int action = m_pressedAction;
                    m_actionPress = false;
                    m_pressed = QModelIndex{};
                    const auto position = mouse->position().toPoint();
                    updateHover(position);
                    const auto index = m_view->indexAt(position);
                    if (pressed.isValid() && index.siblingAtColumn(0) == pressed && hitAction(index, position) == action)
                        m_action(index, static_cast<RevisionAction>(action), m_view->viewport()->mapToGlobal(actionRect(m_view->visualRect(index.siblingAtColumn(ActionsColumn)), action).bottomLeft()));
                    return true;
                }
            }
            else if (event->type() == QEvent::ToolTip)
            {
                const auto *help = static_cast<QHelpEvent *>(event);
                const auto index = m_view->indexAt(help->pos());
                const int action = hitAction(index, help->pos());
                if (action >= 0)
                {
                    const std::array<QString, 3> labels{"预览 (Alt+P)", "对比 (Enter)", "更多操作 (Shift+F10)"};
                    QToolTip::showText(help->globalPos(), labels[action], m_view->viewport(), actionRect(m_view->visualRect(index.siblingAtColumn(ActionsColumn)), action));
                    return true;
                }
            }
        }
        if ((watched == m_view || watched == m_view->viewport()) && event->type() == QEvent::ContextMenu && !m_editorOpen)
        {
            const auto *context = static_cast<QContextMenuEvent *>(event);
            const bool keyboard = context->reason() == QContextMenuEvent::Keyboard;
            const auto position = watched == m_view ? m_view->viewport()->mapFrom(m_view, context->pos()) : context->pos();
            const auto index = keyboard ? m_view->currentIndex() : m_view->indexAt(position);
            if (index.isValid())
                m_action(index, RevisionAction::More, keyboard ? m_view->viewport()->mapToGlobal(m_view->visualRect(index.siblingAtColumn(ActionsColumn)).bottomLeft()) : context->globalPos());
            return true;
        }
        if (watched == m_view && (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange))
            updateIcons();
        if (watched == m_view && event->type() == QEvent::Hide)
        {
            m_hovered = QModelIndex{};
            m_pressed = QModelIndex{};
            m_actionPress = false;
            m_pointerInside = false;
        }
        // The base filter manages editor focus and commit events, not the view itself.
        if (watched == m_view || watched == m_view->viewport())
            return false;
        return QStyledItemDelegate::eventFilter(watched, event);
    }

  private:
    QTableView *m_view;
    std::function<void()> m_editing;
    ActionHandler m_action;
    std::array<QIcon, 3> m_icons;
    QPersistentModelIndex m_hovered;
    QPersistentModelIndex m_pressed;
    QPoint m_pointerPosition;
    int m_pressedAction{-1};
    bool m_actionPress{false};
    bool m_pointerInside{false};
    mutable bool m_editorOpen{false};
    void updateIcons()
    {
        m_icons = {UiStyle::icon("preview"), UiStyle::icon("compare"), UiStyle::icon("more")};
        m_view->viewport()->update();
    }
    QRect actionRect(const QRect &cell, int action) const
    {
        return {cell.x() + 8 + action * (ActionSize + ActionSpacing), cell.center().y() - ActionSize / 2, ActionSize, ActionSize};
    }
    bool actionsVisible(const QModelIndex &index) const
    {
        return index.isValid() && !m_editorOpen && ((m_hovered.isValid() && m_hovered.row() == index.row()) || (m_view->hasFocus() && m_view->currentIndex().row() == index.row()));
    }
    int hitAction(const QModelIndex &index, const QPoint &position) const
    {
        if (actionsVisible(index))
        {
            const auto cell = m_view->visualRect(index.siblingAtColumn(ActionsColumn));
            for (int action = 0; action < 3; ++action)
                if (actionRect(cell, action).contains(position))
                    return action;
        }
        return -1;
    }
    void updateRow(const QModelIndex &index) const
    {
        if (index.isValid())
        {
            const auto cell = m_view->visualRect(index);
            m_view->viewport()->update(QRect(0, cell.top(), m_view->viewport()->width(), cell.height()));
        }
    }
    void updateHover(const QPoint &position)
    {
        const auto previous = m_hovered;
        m_pointerPosition = position;
        m_pointerInside = m_view->viewport()->rect().contains(position);
        const auto index = m_pointerInside ? m_view->indexAt(position) : QModelIndex{};
        m_hovered = index.siblingAtColumn(0);
        if (previous != m_hovered)
            updateRow(previous);
        updateRow(m_hovered);
        if (hitAction(index, position) >= 0)
            m_view->viewport()->setCursor(Qt::PointingHandCursor);
        else
            m_view->viewport()->unsetCursor();
    }
};
} // namespace

HomePageBackupPage::HomePageBackupPage(BackupService *service, QWidget *parent) : QWidget(parent), ui(new Ui::HomePageBackupPage), m_service(service)
{
    ui->setupUi(this);
    ui->table->setModel(&m_model);
    ui->table->setAccessibleName("历史版本");
    ui->table->setAccessibleDescription("按 Enter 对比版本，Alt+P 预览，Shift+F10 打开版本菜单，F2 编辑说明。");
    UiStyle::flatView(ui->table);
    ui->table->setEditTriggers(QAbstractItemView::EditKeyPressed);
    ui->table->setSelectionBehavior(QAbstractItemView::SelectRows);
    auto *delegate = new HistoryDelegate(ui->table, [this]
                                         { m_editing = true; }, [this](const QModelIndex &index, RevisionAction action, const QPoint &position)
                                         {
        const auto context = revisionContext(index);
        switch (action)
        {
        case RevisionAction::Preview: previewRevision(context); break;
        case RevisionAction::Compare: compareRevision(context); break;
        case RevisionAction::More: showRevisionMenu(context, position); break;
        } });
    ui->table->setItemDelegate(delegate);
    connect(delegate, &QAbstractItemDelegate::closeEditor, this, [this]
            {
        m_editing = false;
        if (std::exchange(m_refreshPending, false))
        {
            const auto id = m_id;
            QTimer::singleShot(0, this, [this, id] { if (m_id == id) refresh(); });
        } });
    ui->table->verticalHeader()->hide();
    ui->table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    ui->table->verticalHeader()->setDefaultSectionSize(UiStyle::rowHeight(40, UiStyle::font(UiStyle::FontRole::Body)));
    ui->table->horizontalHeader()->setHighlightSections(false);
    ui->table->horizontalHeader()->setSectionsClickable(false);
    ui->table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    ui->table->horizontalHeader()->setMinimumSectionSize(64);
    UiStyle::text(ui->table->horizontalHeader(), UiStyle::FontRole::Caption, true);
    UiStyle::text(ui->countLabel, UiStyle::FontRole::Caption, true);
    UiStyle::text(ui->hintLabel, UiStyle::FontRole::Caption, true);
    UiStyle::text(ui->emptyLabel, UiStyle::FontRole::Body, true);
    m_compare = UiStyle::action(this, "compareAction", "对比", "compare");
    m_preview = UiStyle::action(this, "previewAction", "预览", "preview");
    m_restore = UiStyle::action(this, "restoreAction", "恢复到此版本…", "restore");
    m_edit = UiStyle::action(this, "editMessageAction", "编辑说明", "edit");
    m_more = UiStyle::action(this, "revisionMenuAction", "更多版本操作", "more");
    m_refresh = UiStyle::action(this, "refreshHistoryAction", "刷新历史", "refresh");
    m_refresh->setProperty("iconOnly", true);
    m_edit->setShortcut(QKeySequence(Qt::Key_F2));
    m_edit->setShortcutContext(Qt::WidgetShortcut);
    ui->table->addAction(m_edit);
    m_preview->setShortcut(QKeySequence("Alt+P"));
    m_preview->setShortcutContext(Qt::WidgetShortcut);
    ui->table->addAction(m_preview);
    m_more->setShortcuts({QKeySequence("Shift+F10"), QKeySequence(Qt::Key_Menu)});
    m_more->setShortcutContext(Qt::WidgetShortcut);
    ui->table->addAction(m_more);
    m_refresh->setShortcut(QKeySequence::Refresh);
    m_refresh->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    addAction(m_refresh);
    connect(m_compare, &QAction::triggered, this, [this]
            { compareRevision(revisionContext(ui->table->currentIndex())); });
    connect(ui->table, &QTableView::activated, this, [this](const QModelIndex &index)
            { compareRevision(revisionContext(index)); });
    connect(m_preview, &QAction::triggered, this, [this]
            { previewRevision(revisionContext(ui->table->currentIndex())); });
    connect(m_restore, &QAction::triggered, this, [this]
            { restoreRevision(revisionContext(ui->table->currentIndex())); });
    connect(m_refresh, &QAction::triggered, this, &HomePageBackupPage::refresh);
    connect(m_edit, &QAction::triggered, this, [this]
            { editRevision(revisionContext(ui->table->currentIndex())); });
    connect(m_more, &QAction::triggered, this, [this]
            {
        const auto index = ui->table->currentIndex();
        showRevisionMenu(revisionContext(index), ui->table->viewport()->mapToGlobal(ui->table->visualRect(index.siblingAtColumn(ActionsColumn)).bottomLeft())); });
    connect(ui->table->selectionModel(), &QItemSelectionModel::selectionChanged, this, &HomePageBackupPage::updateActions);
    connect(ui->table->selectionModel(), &QItemSelectionModel::currentChanged, this, &HomePageBackupPage::updateActions);
    connect(&m_model, &QStandardItemModel::itemChanged, this, [this](QStandardItem *item)
            {
        if (m_loading || item->column() != 0)
            return;
        const auto id = m_id;
        const auto generation = m_service->repositoryGeneration(id);
        m_service->editMessage(id, item->data(CommitRole).toString(), item->text(), this, [this, id, generation](const OperationResult &result)
        {
            if (id == m_id && generation == m_service->repositoryGeneration(id))
            {
                emit notification(result);
                refresh();
            }
        }); });
    connect(service, &BackupService::repositoryChanged, this, [this](const QString &id)
            {
        if (id == m_id && isVisible())
        {
            if (m_editing)
                m_refreshPending = true;
            else if (!m_loading)
                refresh();
        } });
    connect(service, &BackupService::repositoryInvalidated, this, [this](const QString &id)
            {
        m_states.remove(id);
        if (m_loadedId == id)
        {
            ++m_contextGeneration;
            closeRevisionMenu();
            m_editing = false;
            m_refreshPending = false;
            m_loadedId.clear();
            QScopedValueRollback<bool> loading(m_loading, true);
            m_model.clear();
            updateActions();
        } });
    updateActions();
}
HomePageBackupPage::~HomePageBackupPage() = default;
QList<QAction *> HomePageBackupPage::toolbarActions() const { return {m_refresh}; }
void HomePageBackupPage::rememberState()
{
    if (!m_id.isEmpty() && m_loadedId == m_id && m_loadedGeneration == m_service->repositoryGeneration(m_id))
        m_states[m_id] = {m_loadedGeneration, selectedCommit(), ui->table->verticalScrollBar()->value()};
}
void HomePageBackupPage::deactivate()
{
    rememberState();
    m_refreshNeeded = true;
    ++m_contextGeneration;
    ++m_refreshGeneration;
    closeRevisionMenu();
}
void HomePageBackupPage::setBackup(const QString &id)
{
    rememberState();
    ++m_contextGeneration;
    closeRevisionMenu();
    m_id = id;
    refresh();
}
HomePageBackupPage::RevisionContext HomePageBackupPage::revisionContext(const QModelIndex &index) const
{
    if (!index.isValid() || index.model() != &m_model || m_loadedId != m_id)
        return {};
    return {m_id, index.data(CommitRole).toString(), m_loadedGeneration, m_contextGeneration};
}
bool HomePageBackupPage::isCurrentContext(const RevisionContext &context) const
{
    return !context.commit.isEmpty() && context.backupId == m_id && context.backupId == m_loadedId &&
           context.pageGeneration == m_contextGeneration && context.repositoryGeneration == m_service->repositoryGeneration(context.backupId) &&
           m_service->contains(context.backupId);
}
QModelIndex HomePageBackupPage::indexForRevision(const RevisionContext &context) const
{
    const auto found = m_model.match(m_model.index(0, 0), CommitRole, context.commit, 1, Qt::MatchExactly);
    return found.isEmpty() ? QModelIndex{} : found.first();
}
void HomePageBackupPage::compareRevision(const RevisionContext &context)
{
    if (isCurrentContext(context))
        emit navigate({PageId::Diff, context.backupId, context.commit});
}
void HomePageBackupPage::previewRevision(const RevisionContext &context)
{
    if (!isCurrentContext(context))
        return;
    m_service->preview(context.backupId, context.commit, this, [this, context](const OperationResult &result)
                       {
        if (!isCurrentContext(context))
            return;
        emit notification(result);
        if (result.success && !result.path.isEmpty())
            openLocalPath(this, result.path); });
}
void HomePageBackupPage::restoreRevision(const RevisionContext &context)
{
    if (!isCurrentContext(context))
        return;
    m_service->prepareRestore(context.backupId, context.commit, this, [this, context](const BackupResult<RestoreRequest> &prepared)
                              {
        if (!isCurrentContext(context))
            return;
        if (!prepared.result.success)
        {
            emit notification(prepared.result);
            return;
        }
        const auto name = QFileInfo(m_service->sourcePath(context.backupId)).fileName();
        const auto question = QString("将“%1”恢复到版本 %2？\n\n源文件或文件夹中的当前内容将被该版本替换。历史版本记录会保留。").arg(name, context.commit.left(8));
        const QPointer<HomePageBackupPage> guard(this);
        if (!confirmAction(this, question, "恢复版本") || !guard)
            return;
        if (!isCurrentContext(context))
        {
            emit notification(OperationResult::warn("操作已取消", "版本上下文已变化，请重新打开版本菜单后再试。"));
            return;
        }
        m_service->restore(prepared.value, this, [this, context](const OperationResult &result)
        {
            if (isCurrentContext(context))
                emit notification(result);
        }); });
}
void HomePageBackupPage::editRevision(const RevisionContext &context)
{
    if (!isCurrentContext(context))
        return;
    const auto index = indexForRevision(context);
    if (index.isValid())
    {
        ui->table->setCurrentIndex(index);
        ui->table->edit(index);
    }
}
void HomePageBackupPage::showRevisionMenu(const RevisionContext &context, const QPoint &position)
{
    if (!isCurrentContext(context) || m_editing)
        return;
    closeRevisionMenu();
    const auto index = indexForRevision(context);
    if (!index.isValid())
        return;
    ui->table->setCurrentIndex(index);
    ui->table->selectionModel()->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    auto *menu = new QMenu(this);
    menu->setObjectName("revisionMenu");
    m_revisionMenu = menu;
    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    menu->addSection("版本 " + context.commit.left(8));
    const auto add = [this, menu, context](QAction *source, void (HomePageBackupPage::*callback)(const RevisionContext &))
    {
        auto *action = UiStyle::action(menu, source->objectName() + "Menu", source->text(), source->property("iconName").toString());
        action->setShortcuts(source->shortcuts());
        action->setShortcutContext(Qt::WidgetShortcut);
        action->setEnabled(source->isEnabled());
        menu->addAction(action);
        // Capture the version, never the row number or a later selection.
        connect(action, &QAction::triggered, this, [this, context, callback]
                { (this->*callback)(context); });
    };
    add(m_preview, &HomePageBackupPage::previewRevision);
    add(m_compare, &HomePageBackupPage::compareRevision);
    menu->addSeparator();
    add(m_edit, &HomePageBackupPage::editRevision);
    menu->addSeparator();
    add(m_restore, &HomePageBackupPage::restoreRevision);
    menu->popup(position);
}
void HomePageBackupPage::closeRevisionMenu()
{
    if (m_revisionMenu)
        m_revisionMenu->close();
    m_revisionMenu = nullptr;
}
QString HomePageBackupPage::selectedCommit() const
{
    const auto rows = ui->table->selectionModel()->selectedRows();
    return rows.isEmpty() ? QString() : rows.first().data(CommitRole).toString();
}
void HomePageBackupPage::updateActions()
{
    const bool enabled = ui->table->currentIndex().isValid() && m_loadedId == m_id && m_loadedGeneration == m_service->repositoryGeneration(m_id);
    for (auto *action : {m_compare, m_preview, m_restore, m_edit, m_more})
        action->setEnabled(enabled);
    m_restore->setEnabled(enabled && m_service->syncState(m_id) == BackupSyncState::Tracking);
    m_edit->setEnabled(enabled && m_service->syncState(m_id) == BackupSyncState::Tracking);
}
void HomePageBackupPage::refresh()
{
    if (m_id.isEmpty())
        return;
    if (m_editing)
    {
        m_refreshPending = true;
        return;
    }
    m_refreshPending = false;
    m_refreshNeeded = false;
    rememberState();
    const auto id = m_id;
    const auto generation = m_service->repositoryGeneration(id);
    const auto request = ++m_refreshGeneration;
    ui->countLabel->setText("正在读取历史…");
    updateActions();
    m_service->history(id, this, [this, id, generation, request](const BackupResult<QVector<Revision>> &reply)
                       {
    if (id != m_id || generation != m_service->repositoryGeneration(id) || request != m_refreshGeneration)
        return;
    if (m_editing)
    {
        m_refreshPending = true;
        return;
    }
    rememberState();
    auto state = m_states.value(id);
    if (state.generation != generation)
        state = {};
    const auto &revisions = reply.value;
    QScopedValueRollback<bool> loading(m_loading, true);
    m_model.clear();
    m_model.setHorizontalHeaderLabels({"提交说明", "提交时间", "短哈希", {}});
    m_model.horizontalHeaderItem(ActionsColumn)->setData("版本操作", Qt::AccessibleTextRole);
    if (!reply.result.success)
        emit notification(reply.result);
    int selectedRow = 0;
    for (const auto &revision : revisions)
    {
        const auto time = revision.committedAt.toLocalTime().toString("yyyy-MM-dd HH:mm");
        auto *message = new QStandardItem(revision.message);
        auto *date = new QStandardItem(time);
        auto *hash = new QStandardItem(revision.shortHash);
        auto *actions = new QStandardItem;
        date->setEditable(false);
        hash->setEditable(false);
        actions->setEditable(false);
        actions->setData("预览、对比、更多版本操作", Qt::AccessibleTextRole);
        for (auto *item : {message, date, hash, actions})
        {
            item->setData(revision.hash, CommitRole);
            item->setToolTip(revision.message + "\n" + time + " · " + revision.hash);
            item->setData("Enter 对比，Alt+P 预览，Shift+F10 更多操作，F2 编辑说明。", Qt::AccessibleDescriptionRole);
        }
        m_model.appendRow({message, date, hash, actions});
        if (revision.hash == state.commit)
            selectedRow = m_model.rowCount() - 1;
    }
    m_loadedId = m_id;
    m_loadedGeneration = m_service->repositoryGeneration(m_id);
    ui->table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    ui->table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    ui->table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    ui->table->horizontalHeader()->setSectionResizeMode(ActionsColumn, QHeaderView::Fixed);
    // Round up fractional glyph advances so high-DPI hinting cannot elide the final digits.
    ui->table->setColumnWidth(1, qCeil(QFontMetricsF(UiStyle::font(UiStyle::FontRole::Caption), ui->table->viewport()).horizontalAdvance("2000-00-00 00:00")) + 24);
    ui->table->setColumnWidth(2, qCeil(QFontMetricsF(UiStyle::font(UiStyle::FontRole::Code), ui->table->viewport()).horizontalAdvance("00000000")) + 24);
    ui->table->setColumnWidth(ActionsColumn, ActionsWidth);
    ui->table->setColumnHidden(2, width() < 640);
    ui->countLabel->setText(QString("%1 个版本").arg(revisions.size()));
    ui->emptyLabel->setVisible(revisions.isEmpty());
    ui->table->setVisible(!revisions.isEmpty());
    if (!revisions.isEmpty())
        ui->table->selectRow(selectedRow);
    ui->table->verticalScrollBar()->setValue(state.scroll);
    QTimer::singleShot(0, this, [this, state, request]
                       {
        if (request == m_refreshGeneration)
            ui->table->verticalScrollBar()->setValue(state.scroll); });
    updateActions(); });
}
void HomePageBackupPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    const int margin = width() < 640 ? 16 : 24;
    ui->pageLayout->setContentsMargins(margin, 12, margin, 16);
    ui->table->setColumnHidden(2, width() < 640);
    ui->hintLabel->setVisible(width() >= 640);
}
void HomePageBackupPage::hideEvent(QHideEvent *event)
{
    deactivate();
    QWidget::hideEvent(event);
}
void HomePageBackupPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (m_refreshNeeded)
        refresh();
}
