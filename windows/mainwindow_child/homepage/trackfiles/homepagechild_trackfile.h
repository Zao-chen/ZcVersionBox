#pragma once
#include "utils/backupservice.h"
#include <QAbstractListModel>
#include <QSortFilterProxyModel>
#include <QStyledItemDelegate>

class QListView;

// A read-only snapshot shared by independent sidebar/page proxies. No service
// calls, filesystem reads or Git queries occur during data access or painting.
class BackupListModel : public QAbstractListModel
{
    Q_OBJECT
  public:
    enum Role
    {
        IdRole = Qt::UserRole + 1,
        PathRole,
        ParentPathRole,
        SearchRole
    };
    explicit BackupListModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    void setItems(const QVector<TrackedItem> &items);
    QModelIndex indexForId(const QString &id) const;

  private:
    QVector<TrackedItem> m_items;
};

class BackupFilterModel : public QSortFilterProxyModel
{
    Q_OBJECT
  public:
    explicit BackupFilterModel(QObject *parent = nullptr);

  protected:
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;
};

// Two-line rows and a painted menu affordance. There are no per-row widgets.
class BackupItemDelegate : public QStyledItemDelegate
{
    Q_OBJECT
  public:
    BackupItemDelegate(QListView *view, bool sidebar);
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
  signals:
    void menuRequested(const QString &id, const QPoint &globalPosition);

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    QListView *m_view;
    bool m_sidebar;
    QString m_pressedId;
    QRect menuRect(const QRect &row) const;
};
