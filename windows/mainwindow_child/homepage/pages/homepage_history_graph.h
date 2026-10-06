#pragma once
#include "utils/backup_types.h"
#include <QStandardItemModel>
#include <QPainter>

namespace HistoryGraph
{
enum Role { Node = Qt::UserRole + 20, Edges, Incoming, Color, Width, Merge, Current, Refs, NodeRefs, LaneRefs };
void populate(QStandardItemModel &model, const QVector<Revision> &revisions, const QString &currentCommit);
void paint(QPainter *painter, const QRect &rect, const QModelIndex &index);
QStringList hit(const QRect &rect, const QModelIndex &index, const QPoint &position);
QColor color(int index);
}
