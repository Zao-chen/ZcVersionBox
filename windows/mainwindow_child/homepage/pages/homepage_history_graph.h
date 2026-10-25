#pragma once
#include "utils/backup_types.h"
#include <QStandardItemModel>
#include <QPainter>

namespace HistoryGraph
{
enum Role { Node = Qt::UserRole + 20, Edges, Incoming, Color, Width, Merge, Current, Refs, NodeRefs, LaneRefs, TipRefs, AncestorCount, AncestorsExpanded };

struct HoverInfo
{
    QStringList refs;
    int colorIndex{-1};
    double progress{0.0};
};

struct HitResult
{
    QStringList refs;
    int colorIndex{-1};
};

// Merges a cloud copy into the local branch of the same name at the same version,
// and names the remaining cloud branches without their remote prefix.
QVector<Revision> userFacing(QVector<Revision> revisions);
void populate(QStandardItemModel &model, const QVector<Revision> &revisions, const QString &currentCommit);
void insertCommonAncestors(QStandardItemModel &model, int row, int count, bool expanded);
void paint(QPainter *painter, const QRect &rect, const QModelIndex &index, const HoverInfo &hover = {});
HitResult hitTest(const QRect &rect, const QModelIndex &index, const QPoint &position);
QStringList hit(const QRect &rect, const QModelIndex &index, const QPoint &position);
QColor color(int index);
}
