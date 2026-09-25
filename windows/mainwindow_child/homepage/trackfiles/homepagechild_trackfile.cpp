#include "homepagechild_trackfile.h"
#include "windows/mainwindow_presentation.h"
#include <QContextMenuEvent>
#include <QDir>
#include <QFileInfo>
#include <QFocusEvent>
#include <QListView>
#include <QMouseEvent>
#include <QPainter>
#include <QSet>
#include <utility>

int BackupListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_items.size();
}
QVariant BackupListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
        return {};
    const auto &item = m_items.at(index.row());
    switch (role)
    {
    case Qt::DisplayRole:
        return item.name;
    case Qt::ToolTipRole:
        return item.sourcePath;
    case Qt::AccessibleTextRole:
        return item.name + ", " + item.sourcePath;
    case IdRole:
        return item.id;
    case PathRole:
        return item.sourcePath;
    case ParentPathRole:
    {
        const auto path = QDir::fromNativeSeparators(item.sourcePath);
        return path.left(qMax(0, path.lastIndexOf('/')));
    }
    case SearchRole:
        return item.name + "\n" + item.sourcePath + "\n" + QDir::fromNativeSeparators(item.sourcePath);
    default:
        return {};
    }
}
QModelIndex BackupListModel::indexForId(const QString &id) const
{
    for (int row = 0; row < m_items.size(); ++row)
        if (m_items[row].id == id)
            return index(row);
    return {};
}
void BackupListModel::setItems(const QVector<TrackedItem> &items)
{
    QSet<QString> ids;
    for (const auto &item : items)
        ids.insert(item.id);
    for (int row = m_items.size() - 1; row >= 0; --row)
        if (!ids.contains(m_items[row].id))
        {
            beginRemoveRows({}, row, row);
            m_items.removeAt(row);
            endRemoveRows();
        }
    for (int row = 0; row < items.size(); ++row)
    {
        const auto &item = items[row];
        const auto old = indexForId(item.id);
        if (!old.isValid())
        {
            beginInsertRows({}, row, row);
            m_items.insert(row, item);
            endInsertRows();
        }
        else
        {
            if (old.row() != row)
            {
                beginMoveRows({}, old.row(), old.row(), {}, row);
                m_items.move(old.row(), row);
                endMoveRows();
            }
            if (m_items[row].name != item.name || m_items[row].sourcePath != item.sourcePath)
            {
                m_items[row] = item;
                emit dataChanged(index(row), index(row));
            }
        }
    }
}
BackupFilterModel::BackupFilterModel(QObject *parent) : QSortFilterProxyModel(parent)
{
    setFilterRole(BackupListModel::SearchRole);
    setFilterCaseSensitivity(Qt::CaseInsensitive);
    setSortCaseSensitivity(Qt::CaseInsensitive);
    setDynamicSortFilter(true);
}
bool BackupFilterModel::lessThan(const QModelIndex &left, const QModelIndex &right) const
{
    const auto comparison = QString::localeAwareCompare(left.data().toString().toCaseFolded(), right.data().toString().toCaseFolded());
    if (comparison != 0)
        return comparison < 0;
    return left.data(BackupListModel::IdRole).toString() < right.data(BackupListModel::IdRole).toString();
}
BackupItemDelegate::BackupItemDelegate(QListView *view, bool sidebar) : QStyledItemDelegate(view), m_view(view), m_sidebar(sidebar)
{
    view->viewport()->installEventFilter(this);
    view->installEventFilter(this);
}
QRect BackupItemDelegate::menuRect(const QRect &row) const
{
    return QRect(row.right() - 32, row.center().y() - 14, 28, 28);
}
QSize BackupItemDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const
{
    return {0, UiStyle::rowHeight(m_sidebar ? 52 : 56, UiStyle::font(m_sidebar ? UiStyle::FontRole::Sidebar : UiStyle::FontRole::Body), true)};
}
void BackupItemDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const auto colors = UiStyle::colors();
    const auto row = option.rect.adjusted(0, 2, 0, -2);
    const bool selected = option.state.testFlag(QStyle::State_Selected);
    const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
    if (selected || hovered)
    {
        painter->setPen(Qt::NoPen);
        painter->setBrush(selected ? colors.selected : colors.hover);
        painter->drawRoundedRect(row, 6, 6);
    }
    if (m_keyboardFocus && option.state.testFlag(QStyle::State_HasFocus))
    {
        painter->setBrush(Qt::NoBrush);
        painter->setPen(colors.secondary);
        painter->drawRoundedRect(QRectF(row).adjusted(.5, .5, -.5, -.5), 6, 6);
    }
    const auto content = row.adjusted(12, 6, -36, -6);
    const auto mainFont = UiStyle::font(m_sidebar ? UiStyle::FontRole::Sidebar : UiStyle::FontRole::Body);
    const auto captionFont = UiStyle::font(UiStyle::FontRole::Caption);
    const int mainHeight = QFontMetrics(mainFont).height();
    painter->setFont(mainFont);
    painter->setPen(colors.text);
    painter->drawText(QRect(content.x(), content.y(), content.width(), mainHeight), Qt::AlignLeft | Qt::AlignVCenter,
                      QFontMetrics(mainFont).elidedText(index.data().toString(), Qt::ElideRight, content.width()));
    painter->setFont(captionFont);
    painter->setPen(colors.secondary);
    const auto path = index.data(m_sidebar ? BackupListModel::ParentPathRole : BackupListModel::PathRole).toString();
    painter->drawText(QRect(content.x(), content.y() + mainHeight + 2, content.width(), QFontMetrics(captionFont).height()), Qt::AlignLeft | Qt::AlignVCenter,
                      QFontMetrics(captionFont).elidedText(QDir::toNativeSeparators(path), Qt::ElideMiddle, content.width()));
    if (hovered || selected)
        UiStyle::icon("more").paint(painter, QRect(menuRect(row).center() - QPoint(8, 8), QSize(16, 16)));
    painter->restore();
}
bool BackupItemDelegate::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_view->viewport())
    {
        if (event->type() == QEvent::MouseButtonPress)
        {
            if (m_keyboardFocus)
            {
                m_keyboardFocus = false;
                m_view->viewport()->update();
            }
            const auto *mouse = static_cast<QMouseEvent *>(event);
            const auto index = m_view->indexAt(mouse->position().toPoint());
            if (mouse->button() == Qt::LeftButton && index.isValid() && menuRect(m_view->visualRect(index)).contains(mouse->position().toPoint()))
            {
                m_pressedId = index.data(BackupListModel::IdRole).toString();
                return true; // Opening the menu must not also navigate.
            }
        }
        else if (event->type() == QEvent::MouseButtonRelease && !m_pressedId.isEmpty())
        {
            const auto id = std::exchange(m_pressedId, {});
            const auto *mouse = static_cast<QMouseEvent *>(event);
            const auto index = m_view->indexAt(mouse->position().toPoint());
            if (index.data(BackupListModel::IdRole).toString() == id)
                emit menuRequested(id, mouse->globalPosition().toPoint());
            return true;
        }
    }
    if (watched == m_view || watched == m_view->viewport())
    {
        if (event->type() == QEvent::KeyPress)
        {
            if (!m_keyboardFocus)
            {
                m_keyboardFocus = true;
                m_view->viewport()->update();
            }
        }
        else if (event->type() == QEvent::FocusIn)
        {
            const auto *focus = static_cast<QFocusEvent *>(event);
            const bool keyboard = focus->reason() == Qt::TabFocusReason || focus->reason() == Qt::BacktabFocusReason;
            if (m_keyboardFocus != keyboard)
            {
                m_keyboardFocus = keyboard;
                m_view->viewport()->update();
            }
        }
        else if (event->type() == QEvent::FocusOut)
        {
            if (m_keyboardFocus)
            {
                m_keyboardFocus = false;
                m_view->viewport()->update();
            }
        }
    }
    if ((watched == m_view->viewport() || watched == m_view) && event->type() == QEvent::ContextMenu)
    {
        const auto *context = static_cast<QContextMenuEvent *>(event);
        const auto index = context->reason() == QContextMenuEvent::Keyboard ? m_view->currentIndex() : m_view->indexAt(context->pos());
        if (index.isValid())
            emit menuRequested(index.data(BackupListModel::IdRole).toString(),
                               context->reason() == QContextMenuEvent::Keyboard ? m_view->viewport()->mapToGlobal(m_view->visualRect(index).bottomRight()) : context->globalPos());
        return true;
    }
    return QStyledItemDelegate::eventFilter(watched, event);
}
